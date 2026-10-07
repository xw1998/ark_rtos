/*
 * ark_bus_crypto.h - frame authentication (the M5-beyond boundary).
 *
 * The core trusts a node id: any node on the medium may claim any src_id.  The
 * tool layer says so out loud (ark_bus_tool.h) and the architecture notes call
 * it the boundary "that needs cryptography".  This is that boundary, kept as a
 * seam rather than a policy:
 *
 *   - A bus has no key by default, and then nothing here runs even when the
 *     feature is compiled in: frames carry no tag and are accepted as before.
 *   - Install a pre-shared key with ark_bus_crypto_set_key() and that bus
 *     starts sealing every frame it sends with a truncated HMAC-SHA256 over
 *     its header fields and payload, and starts refusing every frame whose tag
 *     does not verify -- including frames that arrive with no tag at all.
 *     That is a fail-closed choice: an authenticated node will not accept an
 *     unauthenticated one, because accepting it would let an attacker strip a
 *     tag and forge src_id.
 *
 * By itself it authenticates; it does not encrypt.  With ARK_BUS_ENCRYPT_-
 * ENABLED the same seam also encrypts the payload with ChaCha20 under the tag
 * (encrypt-then-MAC), so a passive listener sees ciphertext; see
 * ark_bus_chacha20.h.  Authentication makes a node id unforgeable and a
 * payload un-tamperable -- which is what the trust model was missing -- and
 * encryption keeps the payload private as well.
 *
 * MCU budget (measured, host gcc -Os, see tools/footprint.sh): SHA-256 + HMAC
 * plus the seal/open seam is ~2.7 KB of .text + .rodata (the rounding
 * constants) and no .bss -- the hashing state lives on the stack of the frame
 * being sealed or opened.  The core's call sites add ~180 B, and the key costs
 * ARK_BUS_CRYPTO_KEY_MAX + 2 bytes inside the bus struct (32 + 2 by default).
 * A target build with thumb-2 is usually smaller than that host figure.
 *
 * The tag is ARK_BUS_CRYPTO_TAG_LEN bytes of the 32-byte HMAC-SHA256 digest
 * (8 by default = a 64-bit tag).  Both ends must agree on the key and the tag
 * length; a mismatch simply fails to verify.
 */
#ifndef ARK_BUS_CRYPTO_H
#define ARK_BUS_CRYPTO_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

#if ARK_BUS_CRYPTO_ENABLED

struct ark_bus;

/*
 * Install the pre-shared key this bus authenticates with.  `len` is 1..
 * ARK_BUS_CRYPTO_KEY_MAX.  Returns ARK_BUS_OK, or ARK_BUS_E_INVAL on a NULL
 * bus, a NULL key or a length outside that range.  The bytes are copied.
 */
int ark_bus_crypto_set_key(struct ark_bus *bus, const uint8_t *key,
                           uint16_t len);

/* Forget the key: the bus stops sealing and stops refusing plain frames. */
void ark_bus_crypto_clear_key(struct ark_bus *bus);

/* 1 when a key is installed, 0 otherwise. */
int ark_bus_crypto_key_set(const struct ark_bus *bus);

/*
 * The primitives, exposed so a caller can verify them against published
 * vectors (the test suite does).  SHA-256 is FIPS 180-4; HMAC is RFC 2104 with
 * SHA-256 and `key` of `klen` bytes (any length; longer than 64 is hashed
 * first).  `out` receives 32 bytes.
 */
void ark_bus_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
void ark_bus_hmac_sha256(const uint8_t *key, uint16_t klen,
                         const uint8_t *data, uint32_t len, uint8_t out[32]);

#endif /* ARK_BUS_CRYPTO_ENABLED */

#endif /* ARK_BUS_CRYPTO_H */
