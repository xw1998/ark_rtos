/**
* @file arkrtos_trust.h
* @brief Trust root for the image verification key (SEC-G12)
* @details Verification answers one question: "did the holder of this key sign
*          these bytes". It cannot answer "is this key the one the vendor
*          meant", because the key has to come from somewhere. When the public
*          key is a compile-time initialiser, that somewhere is the firmware
*          image itself: whoever can build a new firmware can put their own key
*          in it, and every signature check downstream then agrees with them.
*
*          This module closes that one gap and nothing else:
*            - the key can come from the board (OTP / fuse / a protected
*              region) through arkrtos_port_trust_pubkey(). A key read from
*              one-time-programmable storage survives a reflash, which is what
*              makes it a root rather than a copy;
*            - the 32-byte SHA-256 digest of whichever key was used is PINNED
*              through arkrtos_port_trust_pin(). On a blank slot the digest is
*              written on first boot (trust on first use); on later boots it
*              must match. A mismatch latches arkrtos_trust_locked(), and
*              arkrtos_loader_accept() refuses to claim any image while that
*              latch is set. Fail closed: two different keys cannot both be
*              "the vendor's key" on the same device.
*
*          What this does NOT do (read the limits before quoting it):
*            1. It does not verify the kernel itself. Covering that needs a
*               BootROM that checks the kernel before it runs, which is a
*               chip/board property; on a part without one, "the kernel is
*               trusted because it started" is an assumption, not a chain.
*            2. Trust-on-first-use has a first-boot window: an attacker who can
*               replace the key before the digest is ever pinned wins. Pin at
*               production time (or use a part whose OTP is programmed in the
*               factory) and this window does not exist; leave it to first boot
*               and it does.
*            3. A pinned digest says "the key did not change", not "the key is
*               mine". If the first key was wrong, the pin keeps it wrong
*               consistently - which is exactly what makes it a root, and
*               exactly why it must be provisioned deliberately.
*            4. There is no attestation and no key rotation/revocation here.
*               One key per device, for its lifetime, is the scope.
*
* @note Kernel-only header. Apps and drivers must never include it.
*/

#ifndef __ARKRTOS_TRUST_H__
#define __ARKRTOS_TRUST_H__

#include "arkrtos_types.h"

/* ------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------ */
#define ARKRTOS_TRUST_OK              (0)
#define ARKRTOS_TRUST_ERR_PARAM       (-1)   /* null output pointer */
#define ARKRTOS_TRUST_ERR_MISMATCH    (-2)   /* pinned digest != current key: locked */
#define ARKRTOS_TRUST_ERR_PIN         (-3)   /* the pin store refused the write */

/* There is deliberately no "no key" code: arkrtos_features.h refuses to build
 * this module without the asymmetric profile, and that profile already
 * requires ARKRTOS_IMAGE_SIGN_PUBKEY, so a key always exists (from the board,
 * otherwise from the image). A code for an unreachable state would only invite
 * dead branches. */

/* Where the key came from. The two are NOT the same claim. */
#define ARKRTOS_TRUST_SRC_NONE        (0u)   /* nothing usable */
#define ARKRTOS_TRUST_SRC_OTP         (1u)   /* board read it out of OTP/fuse/protected area */
#define ARKRTOS_TRUST_SRC_BUILTIN     (2u)   /* compiled in: a key, not a root */

/* State of the pin slot after this boot. */
#define ARKRTOS_TRUST_PIN_NONE        (0u)   /* not attempted (no key) */
#define ARKRTOS_TRUST_PIN_NEW         (1u)   /* slot was blank, digest written now */
#define ARKRTOS_TRUST_PIN_MATCH       (2u)   /* slot already held this digest */
#define ARKRTOS_TRUST_PIN_MISMATCH    (3u)   /* slot held a different digest */
#define ARKRTOS_TRUST_PIN_UNAVAILABLE (4u)   /* board has no pin store (read failed) */
#define ARKRTOS_TRUST_PIN_WRITEFAIL   (5u)   /* slot was blank and the write failed */

typedef struct
{
    uint32 source;      /* ARKRTOS_TRUST_SRC_* */
    uint32 pin;         /* ARKRTOS_TRUST_PIN_* */
    uint32 key_len;     /* bytes accepted from the source (0 when none) */
    uint8  digest[32];  /* sha256 of the key that is in force now */
} arkrtos_trust_status_t;

/**
* @brief Resolve the verification key, pin its digest, latch a mismatch.
* @return ARKRTOS_TRUST_OK when the pin matched or was written now;
*         ARKRTOS_TRUST_ERR_MISMATCH when the pinned digest is different - the
*         latch is set and stays set until reset; ARKRTOS_TRUST_ERR_PIN when
*         the slot was blank and could not be written (no root established,
*         but no latch: a store that cannot be written is not evidence of
*         tampering).
* @details Safe to call once per boot, before any image is claimed. Calling it
*          again repeats the comparison; a latch is never cleared by it.
*          A board whose pin store is missing is reported as UNAVAILABLE and
*          does NOT latch: "this part has no OTP" is not evidence of tampering.
*/
int32 arkrtos_trust_boot(void);

/** @brief 1 when the pinned digest contradicts the key in force. */
uint32 arkrtos_trust_locked(void);

/**
* @brief Copy the last boot's status out.
* @param p_out destination; zeroed first
* @return ARKRTOS_TRUST_OK, or ERR_PARAM on a null pointer
*/
int32 arkrtos_trust_status_get(arkrtos_trust_status_t *p_out);

/** @brief Human-readable names, for the sec report and the shell. */
const char *arkrtos_trust_src_name(uint32 source);
const char *arkrtos_trust_pin_name(uint32 pin);

/* ------------------------------------------------------------------
 * Board hooks (weak defaults live in arkrtos_trust.c)
 * ------------------------------------------------------------------ */

/**
* @brief Read the image verification public key from non-volatile board storage.
* @param pp_key  set to the key bytes (kept valid for the caller's use)
* @param p_len   set to the length; the Ed25519 profile expects exactly 32
* @return 0 when a key was returned; anything else means "this board has none"
* @note Called once per boot. An implementation that reads OTP must not write
*       it, and must not return a key it synthesised: a key computed at run
*       time is not a root.
*/
int32 arkrtos_port_trust_pubkey(const uint8 **pp_key, uint32 *p_len);

/**
* @brief Read or write the 32-byte key digest pin.
* @param p_digest 32 bytes in/out
* @param write    0 = read the stored digest, 1 = write p_digest
* @return 0 on success; non-zero when the part has no pin store (read) or the
*         write was refused (one-time storage already programmed, protection
*         on, ...)
* @note A blank slot is all-0x00 or all-0xFF; the caller writes it once.
*/
int32 arkrtos_port_trust_pin(uint8 *p_digest, uint32 write);

#endif /* __ARKRTOS_TRUST_H__ */
