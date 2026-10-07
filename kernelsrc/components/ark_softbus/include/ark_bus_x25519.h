/*
 * ark_bus_x25519.h - X25519 Diffie-Hellman (RFC 7748).
 *
 * The handshake needs a shared secret that a passive listener cannot compute
 * and that is not the pre-shared key, so two nodes can mint a fresh per-session
 * key over the wire and discard it afterwards (forward secrecy).  X25519 is the
 * smallest standard choice for an MCU: one 255-bit field, no tables, no
 * bignum library, and a published test vector to check against (RFC 7748
 * section 5.2, in tests/test_ark_bus.c).
 *
 * This is the raw scalar multiplication only -- it does not authenticate, and
 * it does not by itself stop a man in the middle.  The handshake's
 * pre-shared-key confirmation MAC is what authenticates the exchange; this
 * file just does the arithmetic.
 *
 * Gated by ARK_BUS_HANDSHAKE_ENABLED.
 */
#ifndef ARK_BUS_X25519_H
#define ARK_BUS_X25519_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_HANDSHAKE_ENABLED

/* q = n * p  (all 32-byte little-endian; the scalar is clamped inside per the
 * RFC, so callers may pass any 32 random bytes as the private key).  Returns 0
 * on success, or -1 when the result is all zero -- RFC 7748's low-order-point
 * check, which a peer must not be allowed to force (the shared secret would be
 * zero and every session key derived from it public). */
int ark_bus_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]);

/* q = n * basepoint, i.e. the public key for the clamped private key n. */
void ark_bus_x25519_base(uint8_t q[32], const uint8_t n[32]);

/* Clamp a scalar in place (clear the low three bits, clear the top bit, set
 * the second-highest bit), the RFC 7748 "decodeScalar25519" step. */
void ark_bus_x25519_clamp(uint8_t n[32]);

#endif /* ARK_BUS_HANDSHAKE_ENABLED */

#endif /* ARK_BUS_X25519_H */
