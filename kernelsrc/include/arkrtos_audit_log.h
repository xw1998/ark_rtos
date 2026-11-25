/**
* @file arkrtos_audit_log.h
* @brief Persistent audit trail (SEC-G05): what happened, in order, on the volume
* @details The crash journal answers "how many times did this slot fault"; the
*          fault ring answers "what happened just before the reset" and is
*          cleared by design on every boot. Neither answers the question an
*          auditor asks after the fact: who installed what, which image ran,
*          which console attempt was rejected, and in which order. That answer
*          has to be on a volume, not in RAM, or a reset erases exactly the
*          evidence the trail exists for.
*
*          Design, stated so a reading is not taken for more than it is:
*            - one fixed-size file, `ARKRTOS_AUDIT_LOG_PATH`, laid out as a
*              32-byte header followed by `ARKRTOS_AUDIT_LOG_RECORDS` slots of
*              32 bytes. The ring wraps; the oldest record is overwritten.
*            - every record carries its own crc32 and a 1-based `seq` that only
*              ever increases. A record cut in half by a reset fails its crc and
*              is reported as `bad_crc` instead of being read as content.
*            - the header is written *after* the record it points at. A reset
*              between the two leaves a slot whose record is valid but whose
*              header is one behind; the next open scans the slots and takes the
*              newest by `seq`, so the record is recovered rather than skipped.
*            - only task context writes. A fault handler that tried to open a
*              file would make the panic path depend on the file system, so the
*              fault trail stays where it already is (arkrtos_crash.h).
*            - one writer at a time: a second caller while a write is in
*              progress is refused with ARKRTOS_AUDIT_LOG_ERR_BUSY and counted
*              in `busy` rather than being silently merged. Losing a record is
*              recorded as a loss, not hidden.
*
*          What this does NOT do, so the trail is not read as tamper-proof: a
*          writer with access to the volume can delete the file or forge
*          records that pass their crc. Detecting that needs a key (SEC-G10) or
*          an append-only medium, neither of which exists here.
*
* @note Kernel-only header. Apps and drivers must never include it.
*/

#ifndef __ARKRTOS_AUDIT_LOG_H__
#define __ARKRTOS_AUDIT_LOG_H__

#include "arkrtos_types.h"

/* ------------------------------------------------------------------
 * Event kinds
 *
 * The numbering is part of the on-volume format: a record written by an older
 * build must still be readable by a newer one, so a kind is never renumbered
 * and never reused for something else. New kinds append.
 * ------------------------------------------------------------------ */
#define ARKRTOS_AUDIT_EV_LOG_OPEN     (1u)   /* trail opened; code = records recovered */
#define ARKRTOS_AUDIT_EV_ROOT_MOUNT   (2u)   /* volume mounted at boot; code = 0 */
#define ARKRTOS_AUDIT_EV_IMAGE_OK     (3u)   /* image installed; code = 0 */
#define ARKRTOS_AUDIT_EV_IMAGE_REFUSE (4u)   /* image refused; code = loader error */
#define ARKRTOS_AUDIT_EV_EXEC_START   (5u)   /* image started; code = task id */
#define ARKRTOS_AUDIT_EV_EXEC_EXIT    (6u)   /* image ended; code = exit code */
#define ARKRTOS_AUDIT_EV_AUTH_FAIL    (7u)   /* wrong console password; code = try no */
#define ARKRTOS_AUDIT_EV_AUTH_LOCK    (8u)   /* console hard-locked; code = limit */
#define ARKRTOS_AUDIT_EV_QUOTA_REFUSE (9u)   /* app task quota hit; code = owner */
#define ARKRTOS_AUDIT_EV_ROLLBACK_ARM (10u)  /* rollback target armed; code = budget */
#define ARKRTOS_AUDIT_EV_ROLLBACK_OK  (11u)  /* downgrade admitted; code = that entry's used */
#define ARKRTOS_AUDIT_EV_LAST         ARKRTOS_AUDIT_EV_ROLLBACK_OK

/* ------------------------------------------------------------------
 * One record, 32 bytes on the volume
 * ------------------------------------------------------------------ */
#define ARKRTOS_AUDIT_REC_MAGIC   (0x41554452u)   /* "AUDR" */
#define ARKRTOS_AUDIT_REC_WORDS   (8u)
#define ARKRTOS_AUDIT_REC_SIZE    (32u)

typedef struct {
    uint32 magic;      /* ARKRTOS_AUDIT_REC_MAGIC, 0 means "empty slot" */
    uint32 seq;        /* 1-based, strictly increasing, never reused */
    uint32 tick;       /* kernel tick when the event was recorded */
    uint32 kind;       /* ARKRTOS_AUDIT_EV_x */
    uint32 owner;      /* task / owner id, 0 = kernel or console itself */
    uint32 code;       /* kind-specific: errno, exit code, slot, count */
    uint32 spare;      /* reserved, written as 0 */
    uint32 crc;        /* crc32 over the seven words above */
} arkrtos_audit_rec_t;

/* The volume layout is an ABI: the size is checked at compile time so a field
 * added here cannot silently change the stride (and with it, every offset). */
typedef char arkrtos_audit_rec_size_check[
    (sizeof(arkrtos_audit_rec_t) == ARKRTOS_AUDIT_REC_SIZE) ? 1 : -1];

/* ------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------ */
#define ARKRTOS_AUDIT_LOG_OK         (0)
#define ARKRTOS_AUDIT_LOG_ERR_PARAM  (-1)   /* null pointer / unknown kind */
#define ARKRTOS_AUDIT_LOG_ERR_NOMOUNT (-2)  /* no volume, nothing to write to */
#define ARKRTOS_AUDIT_LOG_ERR_WRITE  (-3)   /* the volume refused the record */
#define ARKRTOS_AUDIT_LOG_ERR_EMPTY  (-4)   /* index past the newest record */
#define ARKRTOS_AUDIT_LOG_ERR_CRC    (-5)   /* slot on disk fails its own crc */
#define ARKRTOS_AUDIT_LOG_ERR_BUSY   (-6)   /* another write is in progress */

/** @brief What the trail knows about itself, for the console and the tests. */
typedef struct {
    uint32 cap;        /* slots the file was created with */
    uint32 used;       /* records currently readable (0..cap) */
    uint32 first_seq;  /* seq of the oldest record, 0 when empty */
    uint32 last_seq;   /* seq of the newest record, 0 when empty */
    uint32 wrapped;    /* 1 = the ring has overwritten at least one record */
    uint32 dropped;    /* records this boot wanted to write and could not */
    uint32 busy;       /* of `dropped`, the ones refused because a write was in flight */
    uint32 bad_crc;    /* slots carrying the record magic with a wrong crc */
    uint32 rebuilds;   /* times the header had to be rebuilt from the slots */
    uint32 opens;      /* times the trail was opened this boot */
    uint8  ready;      /* 1 = a volume is mounted and the file is usable */
    uint8  reserved[3];
} arkrtos_audit_log_stats_t;

/**
* @brief Open (or create) the trail on the mounted volume and recover its tail.
* @details Idempotent. Called on demand by every entry point, so a system whose
*          volume is mounted later than boot still gets a trail; it also means
*          the console can retry after `umount`/`mount` without a reboot.
*          On success one ARKRTOS_AUDIT_EV_LOG_OPEN record is appended with the
*          number of records recovered, so an empty trail and a trail that lost
*          its tail do not look the same from the console.
* @return ARKRTOS_AUDIT_LOG_OK, ERR_NOMOUNT when no volume is mounted, or
*         ERR_WRITE when the file exists but cannot be read/created.
*/
int32 arkrtos_audit_log_init(void);

/**
* @brief Append one record.
* @param kind   ARKRTOS_AUDIT_EV_x; an unknown value is ERR_PARAM (the trail
*               does not accept "some other event" - it would be unreadable).
* @param owner  task / owner id, 0 for kernel or console.
* @param code   kind-specific number.
* @return ARKRTOS_AUDIT_LOG_OK, ERR_NOMOUNT, ERR_BUSY or ERR_WRITE. A failure
*         increments `dropped` (and `busy` for the busy case) so the console
*         reports the loss instead of showing a gap in `seq` without a reason.
*/
int32 arkrtos_audit_log_record(uint32 kind, uint32 owner, uint32 code);

/**
* @brief Read one record by age: 0 = oldest.
* @return OK, ERR_EMPTY past the newest, ERR_CRC when the slot on disk fails its
*         own crc (the caller must not read `*p_out` as content then).
*/
int32 arkrtos_audit_log_read(uint32 index, arkrtos_audit_rec_t *p_out);

/** @brief Fill @p p_out with the current counters (no I/O beyond one open). */
void arkrtos_audit_log_stats(arkrtos_audit_log_stats_t *p_out);

/** @brief Configured ring depth, so a caller can size a dump before reading. */
uint32 arkrtos_audit_log_cap(void);

/** @brief File the trail lives in (compile-time; never built from user input). */
const char *arkrtos_audit_log_path(void);

/** @brief Stable name of a kind, or "unknown" for a value this build has no name for. */
const char *arkrtos_audit_kind_name(uint32 kind);

#endif /* __ARKRTOS_AUDIT_LOG_H__ */
