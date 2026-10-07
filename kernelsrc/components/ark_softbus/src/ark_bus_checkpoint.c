/*
 * ark_bus_checkpoint.c - task-state checkpoint and restore (M4).
 *
 * Format (all multi-byte fields big-endian, like every other ark_softbus wire):
 *
 *   header (6 B):  magic u16 (0xB5C1) | version u8 (1) | ntask u8 | next_id u16
 *   task   (16 B + params):  state u8 | retries u8 | task_id u16 | method u16 |
 *                            worker_id u32 | require_caps u32 | plen u16 |
 *                            params[plen]
 *
 * Only non-terminal tasks (QUEUED, RUNNING) are written; the count in the
 * header says how many follow, so a reader can walk the blob without trusting
 * a length it has not checked yet.
 */
#include "ark_bus_priv.h"
#include "ark_bus_task.h"

#if ARK_BUS_CHECKPOINT_ENABLED

#define CHK_MAGIC    0xB5C1u
#define CHK_VERSION  1u
#define CHK_HDR      6u
#define CHK_TASK     16u

static ark_bus_task_entry_t *chk_free_entry(ark_bus_t *bus)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (!bus->tasks.entries[i].used) {
            return &bus->tasks.entries[i];
        }
    }
    return NULL;
}

static uint8_t chk_savable(uint8_t state)
{
    return (uint8_t)(state == (uint8_t)ARK_BUS_TASK_QUEUED ||
                     state == (uint8_t)ARK_BUS_TASK_RUNNING);
}

static uint8_t chk_count_live(const ark_bus_t *bus)
{
    uint8_t i;
    uint8_t n = 0;

    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        const ark_bus_task_entry_t *e = &bus->tasks.entries[i];
        if (e->used && chk_savable(e->state)) {
            n++;
        }
    }
    return n;
}

uint16_t ark_bus_checkpoint_size(const ark_bus_t *bus)
{
    uint16_t size = CHK_HDR;
    uint8_t  i;

    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        const ark_bus_task_entry_t *e = &bus->tasks.entries[i];
        if (e->used && chk_savable(e->state)) {
            size = (uint16_t)(size + CHK_TASK + e->params_len);
        }
    }
    return size;
}

int ark_bus_checkpoint_save(const ark_bus_t *bus, uint8_t *buf, uint16_t cap,
                            uint16_t *len)
{
    uint16_t need;
    uint16_t off;
    uint8_t  i;

    if (!bus || !buf || !len) {
        return ARK_BUS_E_INVAL;
    }

    need = ark_bus_checkpoint_size(bus);
    if (cap < need) {
        return ARK_BUS_E_NOMEM;   /* nothing written */
    }

    ark_wr16(buf, CHK_MAGIC);
    buf[2] = (uint8_t)CHK_VERSION;
    buf[3] = chk_count_live(bus);
    ark_wr16(buf + 4, bus->tasks.next_task_id);
    off = CHK_HDR;

    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        const ark_bus_task_entry_t *e = &bus->tasks.entries[i];

        if (!e->used || !chk_savable(e->state)) {
            continue;
        }
        buf[off++] = e->state;
        buf[off++] = e->retries;
        ark_wr16(buf + off, e->task_id);        off = (uint16_t)(off + 2);
        ark_wr16(buf + off, e->method);         off = (uint16_t)(off + 2);
        ark_wr32(buf + off, e->worker_id);      off = (uint16_t)(off + 4);
        ark_wr32(buf + off, e->require_caps);   off = (uint16_t)(off + 4);
        ark_wr16(buf + off, e->params_len);     off = (uint16_t)(off + 2);
        ark_memcpy(buf + off, e->params, e->params_len);
        off = (uint16_t)(off + e->params_len);
    }

    *len = off;
    return ARK_BUS_OK;
}

int ark_bus_checkpoint_count(const uint8_t *buf, uint16_t len)
{
    uint16_t off = CHK_HDR;
    uint8_t  n;
    uint8_t  i;

    if (!buf || len < CHK_HDR) {
        return ARK_BUS_E_NOMEM;
    }
    if (ark_rd16(buf) != CHK_MAGIC || buf[2] != (uint8_t)CHK_VERSION) {
        return ARK_BUS_E_FORMAT;
    }

    n = buf[3];
    for (i = 0; i < n; i++) {
        uint16_t plen;
        if ((uint16_t)(off + CHK_TASK) > len) {
            return ARK_BUS_E_NOMEM;         /* truncated record */
        }
        plen = ark_rd16(buf + off + 14);
        off = (uint16_t)(off + CHK_TASK);
        if ((uint32_t)off + plen > len) {
            return ARK_BUS_E_NOMEM;         /* params run past the end */
        }
        off = (uint16_t)(off + plen);
    }
    return (int)n;
}

int ark_bus_task_restore(struct ark_bus *bus, const uint8_t *buf, uint16_t len)
{
    ark_bus_t *b = (ark_bus_t *)bus;
    uint16_t   off = CHK_HDR;
    uint8_t    n;
    uint8_t    i;
    uint8_t    adopted = 0;
    int        rc;

    if (!bus || !buf) {
        return ARK_BUS_E_INVAL;
    }

    rc = ark_bus_checkpoint_count(buf, len);
    if (rc < 0) {
        return rc;
    }
    n = (uint8_t)rc;

    for (i = 0; i < n; i++) {
        ark_bus_task_entry_t *e;
        uint8_t               state;
        uint8_t               retries;
        uint16_t              task_id;
        uint16_t              method;
        uint32_t              worker_id;
        uint32_t              require_caps;
        uint16_t              plen;

        state        = buf[off++];
        retries      = buf[off++];
        task_id      = ark_rd16(buf + off);  off = (uint16_t)(off + 2);
        method       = ark_rd16(buf + off);  off = (uint16_t)(off + 2);
        worker_id    = ark_rd32(buf + off);  off = (uint16_t)(off + 4);
        require_caps = ark_rd32(buf + off);  off = (uint16_t)(off + 4);
        plen         = ark_rd16(buf + off);  off = (uint16_t)(off + 2);

        if (plen > ARK_BUS_TASK_PARAM_MAX) {
            return ARK_BUS_E_FORMAT;        /* a record we cannot honour */
        }

        e = chk_free_entry(b);
        if (!e) {
            /* No room: the ones adopted so far stay, and the caller is told
             * the table is full rather than silently losing the rest. */
            return ARK_BUS_E_FULL;
        }

        ark_memset(e, 0, sizeof(*e));
        e->used         = 1;
        e->state        = (uint8_t)ARK_BUS_TASK_QUEUED;  /* re-drive it */
        e->retries      = retries;
        e->task_id      = task_id;
        e->method       = method;
        e->worker_id    = 0u;               /* any eligible worker */
        e->tried_mask   = 0u;
        e->submit_ms    = ark_bus_tick_ms();
        e->require_caps = require_caps;
        e->owner        = b;
        e->cb           = NULL;             /* a closure cannot be restored */
        e->user         = NULL;
        e->req_id       = 0u;
        e->result_slot  = 0xFFu;
        e->params_len   = plen;
        ark_memcpy(e->params, buf + off, plen);
        off = (uint16_t)(off + plen);

        (void)state;
        (void)worker_id;
        adopted++;
    }

    /* Keep task ids unique against anything the restored table remembers. */
    if (ark_rd16(buf + 4) > b->tasks.next_task_id) {
        b->tasks.next_task_id = ark_rd16(buf + 4);
    }
    return (int)adopted;
}

#endif /* ARK_BUS_CHECKPOINT_ENABLED */
