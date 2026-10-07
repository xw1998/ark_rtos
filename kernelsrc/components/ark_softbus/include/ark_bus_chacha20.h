/*
 * ark_bus_chacha20.h - the ChaCha20 stream cipher (RFC 8439).
 *
 * The confidentiality follow-on the authentication layer named.  Frame
 * authentication (ark_bus_crypto.h) makes a node id unforgeable and a payload
 * un-tamperable; it leaves the payload readable.  This file is the cipher that
 * closes that gap, kept as a primitive so it can be checked against the
 * published RFC vectors before anything is built on it.
 *
 * Why ChaCha20 and not AES: it is a byte-oriented ARX design with no S-box
 * tables and no lookup-dependent timing, so it is both small and safe without
 * a hardware AES unit, and it is the same primitive an Internet-facing peer
 * already speaks.  It is a stream cipher: encryption and decryption are the
 * same XOR, so there is one function.
 *
 * The whole file is inside ARK_BUS_ENCRYPT_ENABLED, so a build that does not
 * ask for confidentiality links none of it (proven by tools/footprint.sh).
 * Nothing here allocates and nothing here keeps state between calls.
 *
 * Reference: RFC 8439, "ChaCha20 and Poly1305 for IETF Protocols", sections
 * 2.3 (the block function) and 2.4 (the stream cipher).  The test suite checks
 * this file against both vectors.
 */
#ifndef ARK_BUS_CHACHA20_H
#define ARK_BUS_CHACHA20_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_ENCRYPT_ENABLED

/*
 * One 64-byte ChaCha20 keystream block, RFC 8439 section 2.3.  `key` is 32
 * bytes, `nonce` is 12 bytes (the IETF 96-bit variant) and `counter` is the
 * 32-bit block counter.  `out` receives the 64 keystream bytes.
 */
void ark_bus_chacha20_block(const uint8_t key[32], uint32_t counter,
                            const uint8_t nonce[12], uint8_t out[64]);

/*
 * ChaCha20 encryption/decryption, RFC 8439 section 2.4.  XORs `len` bytes of
 * `in` with the keystream starting at block `counter` and writes `out`.  `in`
 * and `out` may not overlap (callers here always use distinct buffers).  The
 * block counter increments per 64-byte block; the 32-bit counter is assumed
 * not to wrap within one call (a frame is far below 2^32 blocks).
 */
void ark_bus_chacha20_xor(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], const uint8_t *in,
                          uint8_t *out, uint32_t len);

#endif /* ARK_BUS_ENCRYPT_ENABLED */

#endif /* ARK_BUS_CHACHA20_H */
