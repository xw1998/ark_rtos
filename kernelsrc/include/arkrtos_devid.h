/**
* @file arkrtos_devid.h
* @brief Per-device identity and challenge-response proof (SEC-G13)
* @details Every other security item answers "is this thing right": the image
*          signature (SEC-G10) says who signed a firmware, the trust root
*          (SEC-G12) says whether the verification key is still the one that was
*          pinned, the console password (SEC-G01) is a gate shared by every unit
*          of the same build. None of them answers "which device am I talking
*          to", so a firmware image plus its flash contents can be copied onto
*          another board and every check still agrees.
*
*          This module gives one device one identity and one way to prove
*          possession of it:
*            - the ID is a digest derived from the chip's unique ID
*              (arkrtos_port_devid_uid()). It is public and stable: it names the
*              device, it proves nothing.
*            - the PROOF is an HMAC-SHA256 over a caller-supplied challenge,
*              keyed by a per-device secret read from non-volatile board storage
*              (arkrtos_port_devid_secret(), typically OTP / fuse / a protected
*              region). Only a party that can read that secret on that part can
*              answer a fresh challenge, so replaying a recording does not work.
*
*          Fail closed: with no per-device secret the module reports NOT READY
*          and arkrtos_devid_prove() refuses. It never falls back to "prove with
*          an all-zero key" - an identity that everybody can forge is worse than
*          no identity, because it looks like one.
*
*          What this does NOT do (read the limits before quoting it):
*            1. It does not provision anything. Writing the secret into OTP and
*               issuing the matching certificate are production-line steps; a
*               secret that arrives through the build (ARKRTOS_DEVICE_SECRET) is
*               reported as BUILTIN, is identical on every unit built that way,
*               and therefore is not an identity at all. It exists so the proof
*               path can be exercised on a simulator or a dev board.
*            2. It does not implement the transport. Whether the peer asks for a
*               challenge, how long it is, and what it does with the answer is
*               the protocol's business; this module only makes the answer
*               computable and checkable.
*            3. It cannot defend against an attacker who can read the secret out
*               of the part (probe, side channel, debug unlock). That needs the
*               chip's own protection, which is a board/silicon property.
*            4. There is no rotation, no revocation and no attestation chain.
*               One secret per device for its lifetime is the scope here.
*
* @note Kernel-only header. Apps and drivers must never include it: an App that
*       could read the secret could clone the device.
*/

#ifndef __ARKRTOS_DEVID_H__
#define __ARKRTOS_DEVID_H__

#include "arkrtos_types.h"

#ifndef ARKRTOS_DEVICE_ID_LEN
#define ARKRTOS_DEVICE_ID_LEN   (16u)
#endif

/* Challenge bounds: a proof of zero bytes proves nothing, and a caller that
 * asks to MAC more than this is asked to hash it first. Bounded on purpose -
 * the buffer is on the caller's stack. */
#define ARKRTOS_DEVID_CHALLENGE_MAX  (256u)
#define ARKRTOS_DEVID_MAC_LEN        (32u)

/* ------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------ */
#define ARKRTOS_DEVID_OK           (0)
#define ARKRTOS_DEVID_ERR_PARAM    (-1)   /* null pointer / empty or oversized challenge */
#define ARKRTOS_DEVID_ERR_NOKEY    (-2)   /* no per-device secret: nothing to prove with */
#define ARKRTOS_DEVID_ERR_STATE    (-3)   /* boot() not run, or no usable chip ID */
#define ARKRTOS_DEVID_ERR_RANGE    (-4)   /* board hook returned more than the caller's buffer */
#define ARKRTOS_DEVID_ERR_MISMATCH (-5)   /* arkrtos_devid_check(): MAC does not match */

/* Where the identity material came from. These are NOT the same claim:
 * only OTP names one specific device. */
#define ARKRTOS_DEVID_SRC_NONE     (0u)   /* nothing usable */
#define ARKRTOS_DEVID_SRC_UID_ONLY (1u)   /* chip ID only: a name, no proof */
#define ARKRTOS_DEVID_SRC_OTP      (2u)   /* board read a per-device secret out of OTP/fuse */
#define ARKRTOS_DEVID_SRC_BUILTIN  (3u)   /* secret came from this image: identical on every unit */

typedef struct
{
    uint32 source;                    /* ARKRTOS_DEVID_SRC_* */
    uint32 ready;                     /* 1 = a proof can be produced */
    uint32 uid_len;                   /* bytes accepted from the chip ID hook */
    uint32 secret_len;                /* bytes accepted from the secret hook */
    uint32 id_len;                    /* bytes written to id[] (0 when no chip ID) */
    uint8  id[ARKRTOS_DEVICE_ID_LEN]; /* public device ID: names it, proves nothing */
} arkrtos_devid_status_t;

/**
* @brief Collect the chip ID and the per-device secret, derive the public ID.
* @return ARKRTOS_DEVID_OK when the ID was derived (a secret is optional);
*         ARKRTOS_DEVID_ERR_RANGE when a board hook asked for more room than the
*         internal buffers have; ARKRTOS_DEVID_ERR_STATE when no chip ID was
*         available, so there is nothing to name.
* @details Safe to call once per boot, before anything authenticates. Calling it
*          again repeats the collection; there is no latch, because reading a
*          chip ID twice is not an event.
*/
int32 arkrtos_devid_boot(void);

/** @brief 1 when arkrtos_devid_prove() can answer (a secret is in force). */
uint32 arkrtos_devid_ready(void);

/**
* @brief Copy the public device ID out.
* @param p_out destination (at least id_len bytes)
* @param cap   capacity of p_out
* @param p_len set to the number of bytes copied
* @return ARKRTOS_DEVID_OK, ERR_PARAM on null pointers, ERR_STATE before boot,
*         ERR_RANGE when cap is too small
*/
int32 arkrtos_devid_id(uint8 *p_out, uint32 cap, uint32 *p_len);

/**
* @brief Answer a challenge: HMAC-SHA256(secret, "ark-devid-proof-v1" || challenge).
* @param p_challenge caller-supplied bytes (1..ARKRTOS_DEVID_CHALLENGE_MAX)
* @param len         challenge length
* @param mac         32-byte output
* @return ARKRTOS_DEVID_OK; ERR_PARAM on a null/empty/oversized challenge;
*         ERR_NOKEY when no per-device secret is in force (fail closed)
* @note The answer is different for every challenge, so recording one exchange
*       does not let anyone answer the next one.
*/
int32 arkrtos_devid_prove(const uint8 *p_challenge, uint32 len, uint8 mac[ARKRTOS_DEVID_MAC_LEN]);

/**
* @brief Recomputed locally and compare in constant time.
* @return ARKRTOS_DEVID_OK when the MAC matches, ERR_MISMATCH when it does not,
*         plus the codes arkrtos_devid_prove() can return
* @details This is the verifier half, used by self-test and by any local
*          loopback. The comparison never returns early on the first differing
*          byte: an early return leaks how much of a forged MAC was right.
*/
int32 arkrtos_devid_check(const uint8 *p_challenge, uint32 len,
                          const uint8 mac[ARKRTOS_DEVID_MAC_LEN]);

/**
* @brief Copy the last boot's status out.
* @param p_out destination; zeroed first
* @return ARKRTOS_DEVID_OK, or ERR_PARAM on a null pointer
*/
int32 arkrtos_devid_status_get(arkrtos_devid_status_t *p_out);

/** @brief Human-readable source name, for the sec report and the shell. */
const char *arkrtos_devid_src_name(uint32 source);

/* ------------------------------------------------------------------
 * Board hooks (weak defaults live in arkrtos_devid.c)
 * ------------------------------------------------------------------ */

/**
* @brief Read the chip's unique ID.
* @param p_out caller's buffer
* @param cap   capacity of p_out
* @param p_len set to the number of bytes written (typically 12 for a 96-bit
*              UID; anything from 1 to 32 is accepted)
* @return 0 when an ID was returned; anything else means "this part has none"
* @note An implementation must not synthesise an ID from a counter or from
*       uninitialised memory: a name that changes between boots is not an
*       identity, and one that every unit shares is not either.
*/
int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len);

/**
* @brief Read the per-device secret from non-volatile storage.
* @param p_out caller's buffer
* @param cap   capacity of p_out (16 to 64 bytes are accepted)
* @return the number of bytes written, or a negative value when this board has
*         no such slot
* @note Called once per boot. An implementation that reads OTP must not write
*       it, and must not return a value it derived from public data (a secret
*       everyone can compute is not a secret).
*/
int32 arkrtos_port_devid_secret(uint8 *p_out, uint32 cap);

#endif /* __ARKRTOS_DEVID_H__ */
