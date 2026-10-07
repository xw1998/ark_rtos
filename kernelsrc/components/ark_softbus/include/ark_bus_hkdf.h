/*
 * ark_bus_hkdf.h - HKDF-SHA256 (RFC 5869).
 *
 * A small, standard key-derivation primitive on top of the HMAC-SHA256 that
 * ark_bus_crypto.c already carries.  It exists so the handshake never feeds a
 * raw shared secret into the MAC or the cipher directly: extract() turns the
 * ECDH output into a pseudorandom key, and expand() turns that key into as
 * many labelled subkeys as the session needs (mac / enc), each under its own
 * `info` label.  That is textbook key separation and it is exactly what RFC
 * 5869 was specified for.
 *
 * Tested against the published vectors (RFC 5869 appendix A.1 and A.3) in
 * tests/test_ark_bus.c.
 *
 * Gated by ARK_BUS_HANDSHAKE_ENABLED: without the handshake nothing here is
 * linked, so a build that does not use it pays no bytes and no symbols.
 */
#ifndef ARK_BUS_HKDF_H
#define ARK_BUS_HKDF_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_HANDSHAKE_ENABLED

/*
 * RFC 5869 extract: PRK = HMAC-Hash(salt, IKM).  A NULL/zero-length salt uses
 * HashLen zero bytes, as the RFC specifies.  `prk` receives 32 bytes.
 */
void ark_bus_hkdf_extract(const uint8_t *salt, uint16_t salt_len,
                          const uint8_t *ikm, uint32_t ikm_len,
                          uint8_t prk[32]);

/*
 * RFC 5869 expand: fills `out` with `out_len` bytes from `prk` (32 bytes) and
 * the context label `info`.  `out_len` must be 1..255*32; a larger request is
 * clamped, which is safe because every caller here asks for 32.
 */
void ark_bus_hkdf_expand(const uint8_t prk[32],
                         const uint8_t *info, uint32_t info_len,
                         uint8_t *out, uint32_t out_len);

#endif /* ARK_BUS_HANDSHAKE_ENABLED */

#endif /* ARK_BUS_HKDF_H */
