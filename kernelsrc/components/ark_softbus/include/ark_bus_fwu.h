/*
 * ark_bus_fwu.h - fleet management: ship a *signed* firmware image to a target
 * and have it verify the signature before it is applied.
 *
 * The bus can already move an object to a peer (the data plane) and agree on
 * values (consensus).  What it cannot yet do is hand a device new firmware and
 * be sure the device got *our* firmware -- and for a fleet that is the whole
 * point: one bad push to a thousand nodes is a thousand bricks.  This layer is
 * that last mile, kept as a seam rather than a policy:
 *
 *   - the controller signs an image and streams {manifest || image} to the
 *     target over the existing data plane (so it reuses the stop-and-wait
 *     chunking, the cumulative ACK and the per-frame CRC for free);
 *   - the target checks the signature against its own key before it hands the
 *     image to a user-supplied apply hook.  A bad signature, an unknown key or
 *     a truncated object is refused and counted, never applied.
 *
 * The signature is symmetric (HMAC-SHA256), matching the rest of this bus's
 * trust model: it authenticates that the sender knows the shared key, it is
 * not a public-key detached signature.  There is still no PKI and no
 * out-of-band key distribution (see docs/authentication.md).
 *
 * Compile-gated (ARK_BUS_FWU_ENABLED) and off by default; it needs the data
 * plane and crypto, and a build that does not ask for it links none of this.
 */
#ifndef ARK_BUS_FWU_H
#define ARK_BUS_FWU_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_FWU_ENABLED

struct ark_bus;

/* On-the-wire manifest, prepended to the image inside one streamed object.
 *
 *   偏移  长度  字段
 *     0     2   magic 0x46 0x57 ('F','W')
 *     2     1   version (1)
 *     3     1   name_len
 *     4     4   image_len (大端)
 *     8    32   sha256(image)
 *    40    32   hmac_sha256(key, sha256(image)) -- the "signature"
 *    72  name_len  name (no NUL)
 *   ...          image
 *
 * The digest is what is signed (SHA-256 then HMAC), not the whole image: a
 * detached signature over the hash is standard, and it avoids hashing a large
 * image twice on an MCU.
 */
#define ARK_BUS_FWU_MAGIC0   0x46u
#define ARK_BUS_FWU_MAGIC1   0x57u
#define ARK_BUS_FWU_SIG_LEN  32u
#define ARK_BUS_FWU_HDR_LEN  72u

typedef struct {
    uint8_t  key[ARK_BUS_FWU_KEY_MAX];
    uint16_t key_len;

    /* Stream receive callback we displaced at init, so a non-FWU object still
     * reaches whoever wanted it (chained, not lost). */
    void    *prev_recv_cb;
    void    *prev_recv_user;

    /* Apply hook: called on the target for each image whose signature checked
     * out.  Return ARK_BUS_OK to accept it as applied. */
    int    (*sink)(struct ark_bus *bus, uint32_t from, const char *name,
                   const uint8_t *image, uint32_t len, void *user);
    void    *sink_user;

    /* Large transmit scratch: the contiguous {manifest || image} handed to the
     * data plane.  Only touched while a push is being built. */
    uint8_t  object[ARK_BUS_FWU_OBJ_MAX];

    uint8_t  inflight;  /* a push has borrowed `object` and is not done yet */

    uint32_t pushed;    /* signatures computed and handed to the data plane */
    uint32_t verified;  /* images whose signature checked out on this node   */
    uint32_t rejected;  /* FWU objects refused (bad magic/sig/length/no key) */
} ark_bus_fwu_state_t;

/* Install the signing/verifying key (1..ARK_BUS_FWU_KEY_MAX bytes).  Without a
 * key a push is refused and an incoming image is rejected. */
int  ark_bus_fwu_set_key(struct ark_bus *bus, const uint8_t *key, uint16_t len);

/* Forget the key: pushes are refused and incoming images rejected again. */
void ark_bus_fwu_clear_key(struct ark_bus *bus);

/* Target side: register a hook for data-plane objects that are *not* FWU
 * images.  FWU owns the data plane's inbound-object callback (it must, to see
 * an image the instant it is reassembled), so an application that also moves
 * raw objects must hand its handler here instead of calling
 * ark_bus_stream_on_recv() -- that call would displace FWU and it would stop
 * seeing images. */
void ark_bus_fwu_set_fallback(struct ark_bus *bus,
                              void (*cb)(struct ark_bus *bus, uint32_t from,
                                         const uint8_t *data, uint16_t len,
                                         void *user),
                              void *user);

/* Target side: register the hook called for each image that verified.  `name`
 * is NUL-terminated and lives only for the call (it is the received name);
 * `image` likewise points into the reassembly buffer and is only valid for the
 * call -- copy it if you keep it. */
void ark_bus_fwu_on_image(struct ark_bus *bus,
                          int (*cb)(struct ark_bus *bus, uint32_t from,
                                    const char *name, const uint8_t *image,
                                    uint32_t len, void *user),
                          void *user);

/*
 * Controller side: sign `image` and stream it to `dst`.  Returns ARK_BUS_OK
 * once the transfer has started (finish is reported through the data plane's
 * completion; poll until ark_bus_fwu_pushed() has moved), or
 * ARK_BUS_E_STATE with no key, ARK_BUS_E_INVAL on bad arguments (NULL image
 * with len > 0, an over-long name, len past the object ceiling) or
 * ARK_BUS_E_NOMEM when {manifest || image} does not fit the object ceiling.
 */
int  ark_bus_fwu_push(struct ark_bus *bus, uint32_t dst, const char *name,
                      const uint8_t *image, uint32_t len);

/* Counters: pushes that started here, images that verified here, FWU objects
 * refused here.  Any NULL pointer is ignored. */
void ark_bus_fwu_get_stats(const struct ark_bus *bus,
                           uint32_t *pushed, uint32_t *verified,
                           uint32_t *rejected);

#endif /* ARK_BUS_FWU_ENABLED */

#endif /* ARK_BUS_FWU_H */
