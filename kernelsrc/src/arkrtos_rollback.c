/**
* @file arkrtos_rollback.c
* @brief Explicit, bounded rollback to a known-good image (SEC-G11)
* @details The policy is small enough to state in one screen: a table of
*          (image_id, version, image_size, crc32, budget, used) entries that
*          lives in RAM, is filled only by arkrtos_rollback_arm(), and is
*          consulted only by arkrtos_loader_check_install() when the incoming
*          version is LOWER than what is already installed.
*
*          Why the anchor is read out of the pool and not passed in by the
*          operator: "known good" is a property of bytes, not of a number. If
*          arm() took a crc32 from the console, a typo would whitelist whatever
*          happens to share that crc32; reading the installed copy's header
*          means the whitelisted content is exactly the copy that was running
*          when the operator decided it was good.
*
*          Why RAM-only: a persistent entry is a permanent downgrade ticket.
*          Anyone who reaches the console once (before SEC-G01 auth, or with
*          it) could arm a target and come back after any number of reboots.
*          Cleared on reset, an arm is an operational decision made now, and it
*          has to be made again next boot. The audit trail keeps the record of
*          when it was made.
*
* @note Kernel-only. Called from the installer path and the console.
*/

#include "arkrtos_rollback.h"
#include "arkrtos_ptable.h"
#include "arkrtos_share.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_ROLLBACK == 1)

#include "arkrtos_audit_log.h"   /* kinds + record(); only called when on */
#include <stdint.h>              /* uintptr_t: slot_base is a 32-bit phys addr */

/* The whole state. Zero means "no entry", which is why image_id == 0 is
 * refused by arm(): it would be an entry nobody can tell apart from a hole. */
static arkrtos_rollback_entry_t g_rb[ARKRTOS_ROLLBACK_ENTRIES];
static uint32 g_rb_permits;

/* An admitted rollback is the event an auditor looks for after the fact:
 * "this device went back to an older build, here is when". Written from the
 * task context that installs images (installer task or console), so the
 * filesystem-backed trail is safe to touch here. */
static void rb_audit(uint32 kind, uint32 code)
{
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    (void)arkrtos_audit_log_record(kind, 0u, code);
#else
    (void)kind;
    (void)code;
#endif
}

int32 arkrtos_rollback_arm_ex(uint32 image_id, uint32 version,
                              uint32 image_size, uint32 crc32, uint32 uses)
{
    uint32 i;
    uint32 free_idx = (uint32)ARKRTOS_ROLLBACK_ENTRIES;

    if((image_id == 0u) || (version == 0u) ||
       (image_size == 0u) || (crc32 == 0u))
    {
        return ARKRTOS_ROLLBACK_ERR_PARAM;
    }
    if(uses == 0u)
    {
        uses = (uint32)ARKRTOS_ROLLBACK_USES;
    }

    for(i = 0u; i < (uint32)ARKRTOS_ROLLBACK_ENTRIES; i++)
    {
        if(g_rb[i].image_id == 0u)
        {
            if(free_idx == (uint32)ARKRTOS_ROLLBACK_ENTRIES)
            {
                free_idx = i;
            }
            continue;
        }
        if((g_rb[i].image_id == image_id) && (g_rb[i].version == version) &&
           (g_rb[i].image_size == image_size) && (g_rb[i].crc32 == crc32))
        {
            /* Re-arming the same target tops up its budget. Saturating rather
             * than wrapping: a budget of 0xFFFFFFFF would be indistinguishable
             * from "unlimited" to a reader of the counters. */
            uint32 sum = g_rb[i].budget + uses;

            g_rb[i].budget = (sum < g_rb[i].budget) ? 0xFFFFFFFFu : sum;
            rb_audit(ARKRTOS_AUDIT_EV_ROLLBACK_ARM, g_rb[i].budget);
            return ARKRTOS_ROLLBACK_OK;
        }
    }

    if(free_idx == (uint32)ARKRTOS_ROLLBACK_ENTRIES)
    {
        return ARKRTOS_ROLLBACK_ERR_FULL;
    }

    g_rb[free_idx].image_id   = image_id;
    g_rb[free_idx].version    = version;
    g_rb[free_idx].image_size = image_size;
    g_rb[free_idx].crc32      = crc32;
    g_rb[free_idx].budget     = uses;
    g_rb[free_idx].used       = 0u;
    rb_audit(ARKRTOS_AUDIT_EV_ROLLBACK_ARM, uses);

    return ARKRTOS_ROLLBACK_OK;
}

int32 arkrtos_rollback_arm(uint32 image_id, uint32 version, uint32 uses)
{
    arkrtos_partition_table_t *pt;
    uint32 i;

    if((image_id == 0u) || (version == 0u))
    {
        return ARKRTOS_ROLLBACK_ERR_PARAM;
    }

    pt = arkrtos_ptable_get();

    /* Any slot type: a driver can be rolled back for the same reason an app
     * can. Only a committed copy is an anchor - an INVALID one was disabled by
     * the crash policy and is precisely what a rollback is trying to escape. */
    for(i = 0u; (i < pt->slot_max) && (i < (uint32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        const arkrtos_app_header_t *p_old;
        uint32 base = pt->slot_base[i];

        if(base == 0u)
        {
            continue;
        }
        if((pt->slot_state[i] != ARKRTOS_APP_SLOT_LOADED) &&
           (pt->slot_state[i] != ARKRTOS_APP_SLOT_RUNNING))
        {
            continue;
        }
        /* slot_base is a 32-bit physical address; widening through uintptr_t
         * keeps the LA64 build quiet instead of adding to the existing
         * int-to-pointer noise. Bit pattern is unchanged. */
        if(*(const volatile uint32 *)(uintptr_t)base != (uint32)ARKRTOS_APP_MAGIC)
        {
            continue;
        }

        p_old = (const arkrtos_app_header_t *)(uintptr_t)base;

        if((p_old->image_id != image_id) || (p_old->version != version))
        {
            continue;
        }

        return arkrtos_rollback_arm_ex(image_id, version,
                                       p_old->image_size, p_old->crc32, uses);
    }

    return ARKRTOS_ROLLBACK_ERR_NOTFOUND;
}

int32 arkrtos_rollback_clear(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)ARKRTOS_ROLLBACK_ENTRIES; i++)
    {
        g_rb[i].image_id   = 0u;
        g_rb[i].version    = 0u;
        g_rb[i].image_size = 0u;
        g_rb[i].crc32      = 0u;
        g_rb[i].budget     = 0u;
        g_rb[i].used       = 0u;
    }
    g_rb_permits = 0u;

    return ARKRTOS_ROLLBACK_OK;
}

int32 arkrtos_rollback_allow(const arkrtos_app_header_t *p_hdr)
{
    uint32 i;

    if(p_hdr == 0)
    {
        return ARKRTOS_ROLLBACK_ERR_PARAM;
    }

    for(i = 0u; i < (uint32)ARKRTOS_ROLLBACK_ENTRIES; i++)
    {
        if((g_rb[i].image_id   != p_hdr->image_id) ||
           (g_rb[i].version    != p_hdr->version) ||
           (g_rb[i].image_size != p_hdr->image_size) ||
           (g_rb[i].crc32      != p_hdr->crc32))
        {
            continue;
        }

        if(g_rb[i].used >= g_rb[i].budget)
        {
            return ARKRTOS_ROLLBACK_ERR_NOENTRY;    /* armed, but spent */
        }

        g_rb[i].used++;
        g_rb_permits++;

        /* Consumed at admission, not at success: a failed install burns the
         * authorisation. The other order would leave the ticket open for a
         * retry that nobody re-authorised. */
        rb_audit(ARKRTOS_AUDIT_EV_ROLLBACK_OK, g_rb[i].used);

        return ARKRTOS_ROLLBACK_OK;
    }

    return ARKRTOS_ROLLBACK_ERR_NOENTRY;
}

uint32 arkrtos_rollback_list(arkrtos_rollback_entry_t *p_out, uint32 max)
{
    uint32 i;
    uint32 n = 0u;

    if(p_out == 0)
    {
        return 0u;
    }

    for(i = 0u; (i < (uint32)ARKRTOS_ROLLBACK_ENTRIES) && (n < max); i++)
    {
        if(g_rb[i].image_id == 0u)
        {
            continue;
        }
        p_out[n] = g_rb[i];
        n++;
    }

    return n;
}

uint32 arkrtos_rollback_permits(void)
{
    return g_rb_permits;
}

#else   /* ARKRTOS_USE_ROLLBACK == 0 */

/* Off (the default) means "no exception to version monotonicity exists", and
 * the object must say nothing about it: no symbols, no state, no size. The
 * caller-side #if keeps the loader from asking. */

#endif /* ARKRTOS_USE_ROLLBACK */
