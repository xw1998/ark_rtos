/**
* @file arkrtos_audit_log.c
* @brief Persistent audit trail -- the ring on the volume, see the header
* @details Three rules decide every branch in this file:
*
*            1. A slot is only content when its own crc says so. Everything
*               else (a cut record, a stale header, a slot never written) is
*               reported as what it is; none of it is repaired by copying a
*               neighbour's bytes around.
*            2. The record goes to the volume before the header that points at
*               it. A reset in between costs one header rebuild, never a
*               record: the rebuild takes the newest `seq` it can find.
*            3. A write that did not fully land is a dropped record, and the
*               in-RAM ring position is rolled back with it. Advancing after a
*               failed write would leave a slot behind the head that the next
*               open reads as "newest" and the next append overwrites.
*/

#include "arkrtos_audit_log.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_AUDIT_LOG == 1)

#include "arkrtos_app_image.h"   /* arkrtos_crc32 */
#include "arkrtos_fs.h"
#include "arkrtos_task.h"        /* arkrtos_kernel_get_tick */

/* ------------------------------------------------------------------ */
/* on-volume layout                                                    */
/* ------------------------------------------------------------------ */

#define AL_HDR_MAGIC    (0x41554448u)   /* "AUDH" */
#define AL_HDR_VERSION  (1u)
#define AL_HDR_SIZE     (32u)

/* Header, 32 bytes, same stride as a record: magic / version / cap / head /
 * wrapped / seq_next / reserved / crc. */
#define AL_OFF_MAGIC     (0u)
#define AL_OFF_VERSION   (4u)
#define AL_OFF_CAP       (8u)
#define AL_OFF_HEAD      (12u)
#define AL_OFF_WRAPPED   (16u)
#define AL_OFF_SEQNEXT   (20u)
#define AL_OFF_HRES      (24u)
#define AL_OFF_HCRC      (28u)

#define AL_CAP          ((uint32)ARKRTOS_AUDIT_LOG_RECORDS)
#define AL_REC_BASE     (AL_HDR_SIZE)
#define AL_REC_OFF(i)   (AL_REC_BASE + ((i) * ARKRTOS_AUDIT_REC_SIZE))

/* "A write is in progress" is a byte, not a lock: the critical section here
 * spans file I/O and a spinlock held for milliseconds is a worse lie than a
 * refused record. See the header for why a refusal is counted. */
static volatile uint8 g_al_in_use;

static struct {
    uint8  ready;
    uint8  wrapped;
    uint8  reserved[2];
    uint32 head;        /* slot the next record goes into */
    uint32 seq_next;    /* seq of the next record */
    uint32 dropped;
    uint32 busy;
    uint32 bad_crc;
    uint32 rebuilds;
    uint32 opens;
} g_al;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void al_put32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

static uint32 al_get32(const uint8 *p)
{
    return ((uint32)p[0]) | (((uint32)p[1]) << 8) |
           (((uint32)p[2]) << 16) | (((uint32)p[3]) << 24);
}

static void al_enc_header(uint8 *p, uint32 head, uint32 wrapped, uint32 seq_next)
{
    uint32 i;

    for(i = 0u; i < AL_HDR_SIZE; i++)
    {
        p[i] = 0u;
    }

    al_put32(&p[AL_OFF_MAGIC], AL_HDR_MAGIC);
    al_put32(&p[AL_OFF_VERSION], AL_HDR_VERSION);
    al_put32(&p[AL_OFF_CAP], AL_CAP);
    al_put32(&p[AL_OFF_HEAD], head);
    al_put32(&p[AL_OFF_WRAPPED], wrapped);
    al_put32(&p[AL_OFF_SEQNEXT], seq_next);
    al_put32(&p[AL_OFF_HRES], 0u);
    al_put32(&p[AL_OFF_HCRC], arkrtos_crc32(p, AL_OFF_HCRC, 0u));
}

static int32 al_read_at(uint8 *buf, uint32 len, uint32 off, uint32 *out_len)
{
    *out_len = 0u;
    if(arkrtos_fs_read_at(arkrtos_audit_log_path(), buf, len, off, out_len) != 0)
    {
        return ARKRTOS_AUDIT_LOG_ERR_WRITE;
    }
    return ARKRTOS_AUDIT_LOG_OK;
}

static int32 al_write_at(const uint8 *buf, uint32 len, uint32 off)
{
    if(arkrtos_fs_write_at(arkrtos_audit_log_path(), buf, len, off) != 0)
    {
        return ARKRTOS_AUDIT_LOG_ERR_WRITE;
    }
    return ARKRTOS_AUDIT_LOG_OK;
}

static void al_enc_rec(uint8 *p, uint32 seq, uint32 tick, uint32 kind,
                       uint32 owner, uint32 code)
{
    uint32 i;

    for(i = 0u; i < ARKRTOS_AUDIT_REC_SIZE; i++)
    {
        p[i] = 0u;
    }

    al_put32(&p[0], ARKRTOS_AUDIT_REC_MAGIC);
    al_put32(&p[4], seq);
    al_put32(&p[8], tick);
    al_put32(&p[12], kind);
    al_put32(&p[16], owner);
    al_put32(&p[20], code);
    al_put32(&p[24], 0u);
    al_put32(&p[28], arkrtos_crc32(p, 28u, 0u));
}

/* A slot is content only when both the magic and the crc agree. `0` here means
 * "no record", not "a record of zeroes": an erased slot is all 0xFF or all
 * 0x00 and fails the crc either way. */
static uint32 al_slot_valid(const uint8 *p, arkrtos_audit_rec_t *p_out)
{
    if(al_get32(&p[0]) != ARKRTOS_AUDIT_REC_MAGIC)
    {
        return 0u;
    }
    if(al_get32(&p[28]) != arkrtos_crc32(p, 28u, 0u))
    {
        return 0u;
    }
    p_out->magic = ARKRTOS_AUDIT_REC_MAGIC;
    p_out->seq   = al_get32(&p[4]);
    p_out->tick  = al_get32(&p[8]);
    p_out->kind  = al_get32(&p[12]);
    p_out->owner = al_get32(&p[16]);
    p_out->code  = al_get32(&p[20]);
    p_out->spare = al_get32(&p[24]);
    p_out->crc   = al_get32(&p[28]);
    return (p_out->seq != 0u) ? 1u : 0u;   /* seq 0 is never a real record */
}

/* Rebuild the ring position from the slots themselves.
 *
 * Pass 1 finds the newest seq (and counts the slots that carry the record magic
 * with a bad crc). Pass 2 walks backwards one slot at a time while the
 * predecessor's seq is where the ring order says it must be. No map of the
 * slots is kept: a table of AL_CAP entries on the shell task's stack would be
 * the largest object in a 3 KB stack, and the walk needs no more reads than the
 * records that actually exist. */
static void al_scan(uint32 *p_used, uint32 *p_first_seq, uint32 *p_last_seq,
                    uint32 *p_head, uint32 *p_wrapped, uint32 *p_seq_next)
{
    uint8 buf[ARKRTOS_AUDIT_REC_SIZE];
    arkrtos_audit_rec_t rec;
    uint32 got = 0u;
    uint32 i;
    uint32 any = 0u;
    uint32 max_seq = 0u;
    uint32 max_slot = 0u;
    uint32 used = 0u;

    g_al.bad_crc = 0u;

    for(i = 0u; i < AL_CAP; i++)
    {
        if(al_read_at(buf, ARKRTOS_AUDIT_REC_SIZE, AL_REC_OFF(i), &got) != 0)
        {
            break;
        }
        if(got != ARKRTOS_AUDIT_REC_SIZE)
        {
            break;      /* short file: the rest of the ring does not exist yet */
        }
        if(al_slot_valid(buf, &rec) != 0u)
        {
            any = 1u;
            if(rec.seq > max_seq)
            {
                max_seq = rec.seq;
                max_slot = i;
            }
        }
        else if(al_get32(&buf[0]) == ARKRTOS_AUDIT_REC_MAGIC)
        {
            g_al.bad_crc++;     /* a cut record: counted, not read as content */
        }
    }

    *p_first_seq = 0u;
    *p_last_seq = 0u;
    *p_head = 0u;
    *p_wrapped = 0u;
    *p_seq_next = 1u;
    *p_used = 0u;

    if(any == 0u)
    {
        return;
    }

    /* Walk back while the previous seq sits in the previous slot. */
    used = 1u;
    while(used < AL_CAP)
    {
        uint32 want = max_seq + 1u - (used + 1u);
        uint32 slot = (max_slot + AL_CAP - used) % AL_CAP;

        if(want == 0u)
        {
            break;
        }
        if(al_read_at(buf, ARKRTOS_AUDIT_REC_SIZE, AL_REC_OFF(slot), &got) != 0)
        {
            break;
        }
        if(got != ARKRTOS_AUDIT_REC_SIZE)
        {
            break;
        }
        if((al_slot_valid(buf, &rec) == 0u) || (rec.seq != want))
        {
            break;
        }
        used++;
    }

    *p_used = used;
    *p_first_seq = max_seq + 1u - used;
    *p_last_seq = max_seq;
    *p_head = (max_slot + 1u) % AL_CAP;
    *p_wrapped = (used == AL_CAP) ? 1u : 0u;
    *p_seq_next = max_seq + 1u;
}

/* ------------------------------------------------------------------ */
/* open / create                                                       */
/* ------------------------------------------------------------------ */

int32 arkrtos_audit_log_init(void)
{
    uint8 hdr[AL_HDR_SIZE];
    uint32 got = 0u;
    uint32 used = 0u;
    uint32 first_seq = 0u;
    uint32 last_seq = 0u;
    uint32 head = 0u;
    uint32 wrapped = 0u;
    uint32 seq_next = 1u;
    uint8 ok;

    if(arkrtos_fs_mounted() == 0u)
    {
        g_al.ready = 0u;
        return ARKRTOS_AUDIT_LOG_ERR_NOMOUNT;
    }

    if(g_al_in_use != 0u)
    {
        return ARKRTOS_AUDIT_LOG_ERR_BUSY;
    }
    g_al_in_use = 1u;

    if(al_read_at(hdr, AL_HDR_SIZE, 0u, &got) != ARKRTOS_AUDIT_LOG_OK)
    {
        got = 0u;
    }

    ok = ((got == AL_HDR_SIZE) &&
          (al_get32(&hdr[AL_OFF_MAGIC]) == AL_HDR_MAGIC) &&
          (al_get32(&hdr[AL_OFF_VERSION]) == AL_HDR_VERSION) &&
          (al_get32(&hdr[AL_OFF_CAP]) == AL_CAP) &&
          (al_get32(&hdr[AL_OFF_HCRC]) == arkrtos_crc32(hdr, AL_OFF_HCRC, 0u)))
         ? 1u : 0u;

    if(ok != 0u)
    {
        head     = al_get32(&hdr[AL_OFF_HEAD]);
        wrapped  = al_get32(&hdr[AL_OFF_WRAPPED]);
        seq_next = al_get32(&hdr[AL_OFF_SEQNEXT]);

        /* A header whose crc is fine but whose fields are impossible is not
         * trusted either: that is what a hand-edited volume looks like. */
        if((head >= AL_CAP) || (wrapped > 1u) || (seq_next == 0u))
        {
            ok = 0u;
        }
    }

    g_al.bad_crc = 0u;

    if(ok != 0u)
    {
        used = (wrapped != 0u) ? AL_CAP : head;
    }
    else
    {
        if(got == AL_HDR_SIZE)
        {
            /* Something header-shaped was on the volume and did not hold up. */
            g_al.rebuilds++;
        }

        al_scan(&used, &first_seq, &last_seq, &head, &wrapped, &seq_next);

        {
            uint8 nh[AL_HDR_SIZE];

            al_enc_header(nh, head, wrapped, seq_next);
            if(al_write_at(nh, AL_HDR_SIZE, 0u) != ARKRTOS_AUDIT_LOG_OK)
            {
                g_al_in_use = 0u;
                g_al.ready = 0u;
                return ARKRTOS_AUDIT_LOG_ERR_WRITE;
            }
        }
    }

    g_al.head = head;
    g_al.wrapped = (uint8)(wrapped & 0xFFu);
    g_al.seq_next = seq_next;
    g_al.ready = 1u;
    g_al.opens++;
    g_al_in_use = 0u;

    /* The open itself is an event: an empty trail and a trail whose tail was
     * cut both answer "why is seq not what I remember" this way. */
    (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_LOG_OPEN, 0u, used);

    return ARKRTOS_AUDIT_LOG_OK;
}

/* ------------------------------------------------------------------ */
/* append                                                              */
/* ------------------------------------------------------------------ */

int32 arkrtos_audit_log_record(uint32 kind, uint32 owner, uint32 code)
{
    uint8 rec[ARKRTOS_AUDIT_REC_SIZE];
    uint8 hdr[AL_HDR_SIZE];
    uint32 head;
    uint32 seq;
    uint32 new_head;
    uint32 new_wrapped;

    if((kind == 0u) || (kind > ARKRTOS_AUDIT_EV_LAST))
    {
        return ARKRTOS_AUDIT_LOG_ERR_PARAM;
    }

    if(g_al.ready == 0u)
    {
        int32 rc = arkrtos_audit_log_init();

        if(rc != ARKRTOS_AUDIT_LOG_OK)
        {
            g_al.dropped++;
            return rc;
        }
    }

    if(g_al_in_use != 0u)
    {
        g_al.busy++;
        g_al.dropped++;
        return ARKRTOS_AUDIT_LOG_ERR_BUSY;
    }
    g_al_in_use = 1u;

    head = g_al.head;
    seq  = g_al.seq_next;

    al_enc_rec(rec, seq, arkrtos_kernel_get_tick(), kind, owner, code);

    if(al_write_at(rec, ARKRTOS_AUDIT_REC_SIZE, AL_REC_OFF(head)) !=
       ARKRTOS_AUDIT_LOG_OK)
    {
        /* The ring position is rolled back with the record: a head advanced
         * past a write that did not land would make the next open read the
         * half-written slot as the newest record. */
        g_al_in_use = 0u;
        g_al.dropped++;
        return ARKRTOS_AUDIT_LOG_ERR_WRITE;
    }

    new_head = (head + 1u) % AL_CAP;
    new_wrapped = ((g_al.wrapped != 0u) || (new_head == 0u)) ? 1u : 0u;

    al_enc_header(hdr, new_head, new_wrapped, seq + 1u);

    if(al_write_at(hdr, AL_HDR_SIZE, 0u) != ARKRTOS_AUDIT_LOG_OK)
    {
        /* The record is on the volume but nothing points at it yet; the next
         * open scans the slots and takes it by seq. Keep the RAM ring where it
         * was so this boot does not hand out a seq twice. */
        g_al_in_use = 0u;
        g_al.dropped++;
        return ARKRTOS_AUDIT_LOG_ERR_WRITE;
    }

    g_al.head = new_head;
    g_al.wrapped = (uint8)(new_wrapped & 0xFFu);
    g_al.seq_next = seq + 1u;
    g_al_in_use = 0u;

    return ARKRTOS_AUDIT_LOG_OK;
}

/* ------------------------------------------------------------------ */
/* read back                                                           */
/* ------------------------------------------------------------------ */

int32 arkrtos_audit_log_read(uint32 index, arkrtos_audit_rec_t *p_out)
{
    uint8 buf[ARKRTOS_AUDIT_REC_SIZE];
    uint32 got = 0u;
    uint32 used;
    uint32 slot;

    if(p_out == 0)
    {
        return ARKRTOS_AUDIT_LOG_ERR_PARAM;
    }

    if(g_al.ready == 0u)
    {
        int32 rc = arkrtos_audit_log_init();

        if(rc != ARKRTOS_AUDIT_LOG_OK)
        {
            return rc;
        }
    }

    used = (g_al.wrapped != 0u) ? AL_CAP : g_al.head;

    if(index >= used)
    {
        return ARKRTOS_AUDIT_LOG_ERR_EMPTY;
    }

    slot = (g_al.wrapped != 0u) ? ((g_al.head + index) % AL_CAP) : index;

    if(al_read_at(buf, ARKRTOS_AUDIT_REC_SIZE, AL_REC_OFF(slot), &got) !=
       ARKRTOS_AUDIT_LOG_OK)
    {
        return ARKRTOS_AUDIT_LOG_ERR_WRITE;
    }
    if(got != ARKRTOS_AUDIT_REC_SIZE)
    {
        return ARKRTOS_AUDIT_LOG_ERR_EMPTY;
    }
    if(al_slot_valid(buf, p_out) == 0u)
    {
        return ARKRTOS_AUDIT_LOG_ERR_CRC;
    }

    return ARKRTOS_AUDIT_LOG_OK;
}

void arkrtos_audit_log_stats(arkrtos_audit_log_stats_t *p_out)
{
    arkrtos_audit_rec_t rec;
    uint32 i;

    if(p_out == 0)
    {
        return;
    }

    for(i = 0u; i < (uint32)sizeof(*p_out); i++)
    {
        ((uint8 *)p_out)[i] = 0u;
    }

    p_out->cap = AL_CAP;
    p_out->wrapped = g_al.wrapped;
    p_out->dropped = g_al.dropped;
    p_out->busy = g_al.busy;
    p_out->bad_crc = g_al.bad_crc;
    p_out->rebuilds = g_al.rebuilds;
    p_out->opens = g_al.opens;
    p_out->ready = g_al.ready;

    if((g_al.ready == 0u) || (arkrtos_fs_mounted() == 0u))
    {
        return;
    }

    p_out->used = (g_al.wrapped != 0u) ? AL_CAP : g_al.head;

    if(p_out->used != 0u)
    {
        if(arkrtos_audit_log_read(0u, &rec) == ARKRTOS_AUDIT_LOG_OK)
        {
            p_out->first_seq = rec.seq;
        }
        if(arkrtos_audit_log_read(p_out->used - 1u, &rec) == ARKRTOS_AUDIT_LOG_OK)
        {
            p_out->last_seq = rec.seq;
        }
    }
}

uint32 arkrtos_audit_log_cap(void)
{
    return AL_CAP;
}

const char *arkrtos_audit_log_path(void)
{
    return ARKRTOS_AUDIT_LOG_PATH;
}

const char *arkrtos_audit_kind_name(uint32 kind)
{
    switch(kind)
    {
    case ARKRTOS_AUDIT_EV_LOG_OPEN:     return "log_open";
    case ARKRTOS_AUDIT_EV_ROOT_MOUNT:   return "root_mount";
    case ARKRTOS_AUDIT_EV_IMAGE_OK:     return "image_ok";
    case ARKRTOS_AUDIT_EV_IMAGE_REFUSE: return "image_refuse";
    case ARKRTOS_AUDIT_EV_EXEC_START:   return "exec_start";
    case ARKRTOS_AUDIT_EV_EXEC_EXIT:    return "exec_exit";
    case ARKRTOS_AUDIT_EV_AUTH_FAIL:    return "auth_fail";
    case ARKRTOS_AUDIT_EV_AUTH_LOCK:    return "auth_lock";
    case ARKRTOS_AUDIT_EV_QUOTA_REFUSE: return "quota_refuse";
    case ARKRTOS_AUDIT_EV_ROLLBACK_ARM: return "rollback_arm";
    case ARKRTOS_AUDIT_EV_ROLLBACK_OK:  return "rollback_ok";
    default:                            break;
    }
    return "unknown";
}

#else  /* ARKRTOS_USE_AUDIT_LOG */

/* The off build is an empty translation unit with no symbols at all: the gate
 * checks that by name, because "the file compiled" is not the same claim. */
typedef int arkrtos_audit_log_off_unit_t;

#endif /* ARKRTOS_USE_AUDIT_LOG */
