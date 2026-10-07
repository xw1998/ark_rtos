/*
 * ark_bus_session.h - per-peer session keys via a pre-shared-key-authenticated
 * X25519 handshake (M5 security).
 *
 * The frame-authentication layer (ark_bus_crypto.h) makes a node id
 * unforgeable, but with a single pre-shared key it authenticates the *group*:
 * everyone holding the key can impersonate everyone else, and one leaked key
 * exposes every frame on the link.  This module closes that gap on top of the
 * same seam.
 *
 *   - Two nodes exchange ephemeral X25519 public keys (ark_bus_x25519.h) and
 *     each computes the same shared secret.  The exchange is authenticated by
 *     a confirmation MAC keyed from the pre-shared key, so a man in the middle
 *     without the pre-shared key cannot complete it.
 *   - HKDF-SHA256 (ark_bus_hkdf.h) turns the shared secret into two labelled
 *     subkeys per peer -- one for the MAC, one for the cipher -- which the
 *     crypto layer then uses for every frame to and from that peer instead of
 *     the group key.
 *   - The ephemeral secrets are wiped once the session is ready, so the
 *     result has forward secrecy: recovering the pre-shared key later does not
 *     recover session keys already used.
 *   - Because every pair has its own key, ark_bus_session_revoke() can retire
 *     exactly one peer's session, and re-running the handshake rotates a key
 *     (the previous pair is kept for one handover window so frames already in
 *     flight still open).
 *
 * The trust anchor is still the pre-shared key handed to
 * ark_bus_crypto_set_key(); there is deliberately no PKI and no certificate
 * here.  What this adds is that the key is used to *authenticate an exchange*,
 * not to protect every frame directly, which is what buys the per-peer keys,
 * the rotation, and the forward secrecy.
 *
 * A random source is required (the HAL's `rng`, see ark_bus_hal.h).  With none
 * the handshake refuses with ARK_BUS_E_NOSYS rather than mint a guessable key,
 * unless ARK_BUS_CRYPTO_ALLOW_WEAK_RNG is set on purpose (ark_bus_config.h).
 *
 * Gated by ARK_BUS_HANDSHAKE_ENABLED; with it off none of this is linked and a
 * bus behaves exactly as before.
 */
#ifndef ARK_BUS_SESSION_H
#define ARK_BUS_SESSION_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_HANDSHAKE_ENABLED

#include "ark_bus_types.h"

/*
 * Called exactly once when a handshake started by ark_bus_handshake_start()
 * finishes: status ARK_BUS_OK when a session key is now installed, otherwise a
 * negative error (ARK_BUS_E_TIMEOUT after the retries, ARK_BUS_E_AUTH if the
 * peer's confirmation did not verify, ARK_BUS_E_NOSYS with no random source).
 * A responder's own completion happens synchronously inside the reply and has
 * no callback.
 */
typedef void (*ark_bus_handshake_done_fn)(struct ark_bus *bus, uint32_t peer,
                                          int status, void *ctx);

/*
 * Begin (or restart) a handshake with `peer`.  `done` may be NULL (poll
 * ark_bus_session_ready() instead).  Returns ARK_BUS_OK once the request is on
 * the wire, or a negative error immediately: ARK_BUS_E_STATE when no key is
 * installed, ARK_BUS_E_NOSYS with no usable random source, ARK_BUS_E_INVAL on
 * a NULL bus or a peer id of 0 or self, ARK_BUS_E_FULL when the session table
 * is full of live sessions.  Restarting on a revoked peer lifts the revocation
 * and runs a fresh exchange (a new epoch).
 */
int ark_bus_handshake_start(struct ark_bus *bus, uint32_t peer,
                            ark_bus_handshake_done_fn done, void *ctx);

/*
 * Drop `peer`'s session and refuse further non-handshake frames from it
 * (counted in stats.rx_denied) until it handshakes again.  Returns ARK_BUS_OK,
 * ARK_BUS_E_NOTFOUND when the peer has no session at all, or ARK_BUS_E_FULL
 * when the table is full so no entry can be reserved for the denial.
 */
int ark_bus_session_revoke(struct ark_bus *bus, uint32_t peer);

/*
 * Drop `peer`'s session without denying it: frames to and from it fall back to
 * the pre-shared group key.  Returns ARK_BUS_OK or ARK_BUS_E_NOTFOUND.
 */
int ark_bus_session_forget(struct ark_bus *bus, uint32_t peer);

/* 1 when a ready session key exists for `peer`, 0 otherwise. */
int ark_bus_session_ready(const struct ark_bus *bus, uint32_t peer);

/* The peer's current session epoch, or 0 when it has no session. */
uint16_t ark_bus_session_epoch(const struct ark_bus *bus, uint32_t peer);

/* Number of ready sessions (handy for a status line). */
int ark_bus_session_count(const struct ark_bus *bus);

#endif /* ARK_BUS_HANDSHAKE_ENABLED */

#endif /* ARK_BUS_SESSION_H */
