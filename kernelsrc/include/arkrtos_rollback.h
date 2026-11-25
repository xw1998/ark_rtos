/**
* @file arkrtos_rollback.h
* @brief Explicit, bounded rollback to a known-good image (SEC-G11)
* @details The install path refuses any image whose version is not higher than
*          the copy already in the pool (arkrtos_loader_check_install). That
*          rule is what stops a downgrade attack, and it is also what stops the
*          one legitimate operation an operator needs after a bad release:
*          put the previous, known-good build back.
*
*          This module is the explicit exception, and it is deliberately narrow:
*            - the whitelist lives in RAM only. A reset clears it, so an arm
*              cannot be replayed forever; after a reboot the operator has to
*              authorise the rollback again, on purpose. A persistent entry
*              would turn one console command into a permanent downgrade
*              ticket for whoever can reach the device afterwards.
*            - an entry is bound to the CONTENT, not just to the numbers: arm()
*              reads the crc32 and image size out of the copy already installed
*              in the pool, and check_install only matches an incoming image
*              whose crc32 and size are identical. The install path verifies
*              that crc32 against the payload it received, so a permitted
*              rollback reinstalls the exact bytes that were there before, not
*              "a build that calls itself v1.2.3".
*            - every entry carries a use budget. It is consumed when the image
*              is ADMITTED, not when it is written successfully: a failed
*              install therefore costs an authorisation instead of leaving the
*              ticket open. Fail closed.
*            - each arm and each admitted rollback is written to the persistent
*              audit trail (SEC-G05) when that switch is on, so "why is this
*              older image running again" has an answer after the fact.
*
*          What this does NOT do: it does not decide that a version is good.
*          The operator asserts that by arming it; this module only guarantees
*          that the assertion is bounded, content-bound, explicit and recorded.
*          It also does not remove the bad copy from the pool - after a
*          rollback both versions are installed, and which one starts is the
*          autostart/`app start` decision, not this module's.
*
* @note Kernel-only header. Apps and drivers must never include it.
*/

#ifndef __ARKRTOS_ROLLBACK_H__
#define __ARKRTOS_ROLLBACK_H__

#include "arkrtos_types.h"
#include "arkrtos_app_image.h"   /* arkrtos_app_header_t (allow() takes one) */

/* ------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------ */
#define ARKRTOS_ROLLBACK_OK            (0)
#define ARKRTOS_ROLLBACK_ERR_PARAM     (-1)   /* zero id / version / size / crc */
#define ARKRTOS_ROLLBACK_ERR_NOTFOUND  (-2)   /* no installed copy of (id, version) */
#define ARKRTOS_ROLLBACK_ERR_FULL      (-3)   /* every whitelist entry is taken */
#define ARKRTOS_ROLLBACK_ERR_NOENTRY   (-4)   /* no armed entry authorises this image */

/* ------------------------------------------------------------------
 * One whitelisted rollback target
 * ------------------------------------------------------------------ */
typedef struct {
    uint32 image_id;     /* payload identity (crc32 of the payload, as packed) */
    uint32 version;      /* the version being authorised to come back */
    uint32 image_size;   /* bound to the installed copy: payload length */
    uint32 crc32;        /* bound to the installed copy: pack-time payload crc */
    uint32 budget;       /* uses granted (accumulates across repeated arm()) */
    uint32 used;         /* times check_install admitted this target */
} arkrtos_rollback_entry_t;

/**
* @brief Arm one rollback target from the copy already installed in the pool.
* @param image_id  payload identity of the image to roll back to
* @param version   version of that image
* @param uses      admissions to grant; 0 means ARKRTOS_ROLLBACK_USES
* @return ARKRTOS_ROLLBACK_OK, ERR_PARAM, ERR_NOTFOUND when no LOADED/RUNNING
*         copy of (image_id, version) with a valid header is in the pool, or
*         ERR_FULL when the whitelist has no free entry.
* @details Reading the anchor out of the pool is the point: the operator names
*          a version, the device decides which bytes that means. Re-arming the
*          same target adds to its budget instead of creating a second entry.
*          A copy whose state is INVALID (disabled by the crash policy) is not
*          a usable anchor - reinstall it first if that is what you want back.
*/
int32 arkrtos_rollback_arm(uint32 image_id, uint32 version, uint32 uses);

/**
* @brief Arm an explicit target with the caller-supplied anchor values.
* @details Same as above without the pool lookup. Provided for a host test and
*          for a bootloader-style caller that already holds the header; the
*          console path must use arkrtos_rollback_arm() so the crc comes from
*          the pool rather than from a number somebody typed.
*/
int32 arkrtos_rollback_arm_ex(uint32 image_id, uint32 version,
                              uint32 image_size, uint32 crc32, uint32 uses);

/** @brief Drop every entry and the permit counter. Returns OK always. */
int32 arkrtos_rollback_clear(void);

/**
* @brief Ask whether this incoming image is an armed rollback, and consume one
*        admission if it is.
* @param p_hdr  the incoming image header (the same one check_install holds)
* @return ARKRTOS_ROLLBACK_OK when admitted (one use consumed), ERR_PARAM on a
*         null header, ERR_NOENTRY when nothing matches or the matching entry's
*         budget is exhausted.
* @details Called from arkrtos_loader_check_install() only, and only for a
*          version strictly below the installed best - an equal or higher
*          version never needs this path.
*/
int32 arkrtos_rollback_allow(const arkrtos_app_header_t *p_hdr);

/** @brief Copy out the current entries; returns how many were written. */
uint32 arkrtos_rollback_list(arkrtos_rollback_entry_t *p_out, uint32 max);

/** @brief Admissions granted since the last reset (each one was a rollback). */
uint32 arkrtos_rollback_permits(void);

#endif /* __ARKRTOS_ROLLBACK_H__ */
