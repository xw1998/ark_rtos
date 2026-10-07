/*
 * ark_bus_fwu.c - fleet management: signed firmware images (see ark_bus_fwu.h).
 *
 * The whole file compiles to nothing when ARK_BUS_FWU_ENABLED is 0.  It adds no
 * frames of its own: an image travels as one data-plane object whose payload is
 * {manifest || image}, so the chunking, the cumulative ACK and the per-frame CRC
 * are the data plane's, and the signature is what proves the image is *ours*
 * once it has been reassembled.
 *
 * On the receive side it displaces the data plane's inbound-object callback and
 * chains to whatever was there, so an object that is not ours still reaches its
 * original consumer.  The magic in the manifest is the discriminator.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_FWU_ENABLED

#include "ark_bus_fwu.h"
#include "ark_bus_crypto.h"

/* ------------------------------------------------------------------ */
/* Receive side                                                        */
/* ------------------------------------------------------------------ */

static void fwu_chain(struct ark_bus *bus, uint32_t from, const uint8_t *data,
                      uint16_t len)
{
    ark_bus_stream_recv_cb_t cb =
        (ark_bus_stream_recv_cb_t)bus->fwu.prev_recv_cb;
    if (cb) {
        cb(bus, from, data, len, bus->fwu.prev_recv_user);
    }
}

/* The data plane delivers each reassembled object here. */
static void fwu_recv(struct ark_bus *bus, uint32_t from, const uint8_t *data,
                     uint16_t len, void *user)
{
    ark_bus_fwu_state_t *s = &bus->fwu;
    uint8_t   name_len;
    uint32_t  image_len;
    const uint8_t *image;
    uint8_t   digest[32];
    uint8_t   mac[32];
    char      namebuf[ARK_BUS_FWU_NAME_MAX + 1u];

    (void)user;
    if (!bus || !data) {
        return;
    }

    /* Not an FWU object: hand it back to whoever had the callback. */
    if (len < (uint16_t)ARK_BUS_FWU_HDR_LEN ||
        data[0] != ARK_BUS_FWU_MAGIC0 || data[1] != ARK_BUS_FWU_MAGIC1) {
        fwu_chain(bus, from, data, len);
        return;
    }

    if (data[2] != 1u) {           /* unknown manifest version */
        s->rejected++;
        return;
    }
    name_len  = data[3];
    image_len = ark_rd32(data + 4);
    if (name_len > (uint8_t)ARK_BUS_FWU_NAME_MAX ||
        (uint32_t)ARK_BUS_FWU_HDR_LEN + (uint32_t)name_len + image_len !=
            (uint32_t)len) {
        s->rejected++;             /* a truncated or inconsistent object */
        return;
    }
    if (s->key_len == 0u) {
        s->rejected++;             /* no key to verify with: refuse, never apply */
        return;
    }

    image = data + ARK_BUS_FWU_HDR_LEN + name_len;

    /* The digest must match the bytes we actually reassembled, and the
     * signature must be an HMAC over *that* digest under our key.  Checking the
     * computed digest (not the claimed one) is what binds the signature to the
     * image. */
    ark_bus_sha256(image, image_len, digest);
    if (ark_memcmp(digest, data + 8, 32u) != 0) {
        s->rejected++;             /* the image does not hash to what was signed */
        return;
    }
    ark_bus_hmac_sha256(s->key, s->key_len, digest, 32u, mac);
    if (ark_memcmp(mac, data + 40, (size_t)ARK_BUS_FWU_SIG_LEN) != 0) {
        s->rejected++;             /* wrong key, or the signature is forged */
        return;
    }

    s->verified++;
    if (s->sink) {
        ark_memcpy(namebuf, data + ARK_BUS_FWU_HDR_LEN, name_len);
        namebuf[name_len] = '\0';
        (void)s->sink(bus, from, namebuf, image, image_len, s->sink_user);
    }
}

/* ------------------------------------------------------------------ */
/* Send side                                                           */
/* ------------------------------------------------------------------ */

static void fwu_done(int status, uint16_t xid, void *user)
{
    struct ark_bus *bus = (struct ark_bus *)user;
    (void)status;
    (void)xid;
    if (bus) {
        bus->fwu.inflight = 0u;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ark_bus_fwu_init(struct ark_bus *bus)
{
    if (!bus) {
        return;
    }
    /* bus was zeroed by ark_bus_init(), so the state starts empty; we only have
     * to take over the inbound-object callback and remember the old one. */
    bus->fwu.prev_recv_cb   = (void *)bus->stream.recv_cb;
    bus->fwu.prev_recv_user = bus->stream.recv_user;
    ark_bus_stream_on_recv(bus, fwu_recv, bus);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int ark_bus_fwu_set_key(struct ark_bus *bus, const uint8_t *key, uint16_t len)
{
    if (!bus || !key || len == 0u || len > (uint16_t)ARK_BUS_FWU_KEY_MAX) {
        return ARK_BUS_E_INVAL;
    }
    ark_memcpy(bus->fwu.key, key, len);
    bus->fwu.key_len = len;
    return ARK_BUS_OK;
}

void ark_bus_fwu_clear_key(struct ark_bus *bus)
{
    if (!bus) {
        return;
    }
    ark_memset(bus->fwu.key, 0, sizeof(bus->fwu.key));
    bus->fwu.key_len = 0u;
}

void ark_bus_fwu_set_fallback(struct ark_bus *bus,
                              void (*cb)(struct ark_bus *bus, uint32_t from,
                                         const uint8_t *data, uint16_t len,
                                         void *user),
                              void *user)
{
    if (!bus) {
        return;
    }
    bus->fwu.prev_recv_cb   = (void *)cb;
    bus->fwu.prev_recv_user = user;
}

void ark_bus_fwu_on_image(struct ark_bus *bus,
                          int (*cb)(struct ark_bus *bus, uint32_t from,
                                    const char *name, const uint8_t *image,
                                    uint32_t len, void *user),
                          void *user)
{
    if (!bus) {
        return;
    }
    bus->fwu.sink      = cb;
    bus->fwu.sink_user = user;
}

int ark_bus_fwu_push(struct ark_bus *bus, uint32_t dst, const char *name,
                     const uint8_t *image, uint32_t len)
{
    ark_bus_fwu_state_t *s;
    uint8_t  *obj;
    uint16_t  name_len;
    uint32_t  total;
    uint8_t   digest[32];
    uint16_t  xid = 0u;
    int       rc;

    if (!bus || (!image && len > 0u)) {
        return ARK_BUS_E_INVAL;
    }
    s = &bus->fwu;
    if (s->key_len == 0u) {
        return ARK_BUS_E_STATE;    /* cannot sign without a key */
    }
    if (s->inflight) {
        return ARK_BUS_E_FULL;     /* one push at a time: the object buffer is shared */
    }
    name_len = name ? (uint16_t)ark_strlen(name) : 0u;
    if (name_len > (uint16_t)ARK_BUS_FWU_NAME_MAX) {
        return ARK_BUS_E_INVAL;
    }
    total = (uint32_t)ARK_BUS_FWU_HDR_LEN + (uint32_t)name_len + len;
    if (total > (uint32_t)ARK_BUS_FWU_OBJ_MAX) {
        return ARK_BUS_E_NOMEM;    /* {manifest || image} does not fit one object */
    }

    obj = s->object;
    obj[0] = ARK_BUS_FWU_MAGIC0;
    obj[1] = ARK_BUS_FWU_MAGIC1;
    obj[2] = 1u;
    obj[3] = (uint8_t)name_len;
    ark_wr32(obj + 4, len);

    ark_bus_sha256(image, len, digest);
    ark_memcpy(obj + 8, digest, 32u);
    ark_bus_hmac_sha256(s->key, s->key_len, digest, 32u, obj + 40);
    if (name_len > 0u) {
        ark_memcpy(obj + ARK_BUS_FWU_HDR_LEN, name, name_len);
    }
    if (len > 0u) {
        ark_memcpy(obj + ARK_BUS_FWU_HDR_LEN + name_len, image, len);
    }

    s->inflight = 1u;
    rc = ark_bus_stream_send(bus, dst, obj, total, fwu_done, bus, &xid);
    if (rc != ARK_BUS_OK) {
        s->inflight = 0u;
        return rc;
    }
    s->pushed++;
    return ARK_BUS_OK;
}

void ark_bus_fwu_get_stats(const struct ark_bus *bus, uint32_t *pushed,
                           uint32_t *verified, uint32_t *rejected)
{
    if (!bus) {
        return;
    }
    if (pushed)   { *pushed   = bus->fwu.pushed;   }
    if (verified) { *verified = bus->fwu.verified; }
    if (rejected) { *rejected = bus->fwu.rejected; }
}

#endif /* ARK_BUS_FWU_ENABLED */
