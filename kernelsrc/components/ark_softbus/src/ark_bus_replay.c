/*
 * ark_bus_replay.c - the per-peer replay window.
 *
 * Authentication (ark_bus_crypto.c) proves a frame came from the node its
 * src_id claims and was not altered.  It does not stop an attacker from
 * recording a valid frame and sending it again later: a verbatim replay
 * verifies just as well the second time.  This file remembers the sequence
 * numbers a peer has already used and drops a repeat before it reaches any
 * handler.
 *
 * The window is a 16-bit modular sliding bitmap.  `last` is the highest
 * sequence number seen from that peer and bit i of `seen` records that
 * (last - i) was accepted.  A frame whose number is ahead of `last` slides the
 * window; one inside it and already set is a replay; one older than the window
 * is refused as well (it is either a replay or too late to matter).
 *
 * The sequence number is only worth trusting because the MAC covers it when
 * this feature is on (see ark_bus_config.h and ark_bus_crypto.c): an attacker
 * cannot renumber a captured frame to slip past the window.
 *
 * A frame from a peer the table has no room for is dropped (fail closed):
 * evicting an entry to make space would let that peer's replays through, so
 * refusing is the safer failure.  Size ARK_BUS_REPLAY_PEERS for the number of
 * peers that send to this node at once.
 *
 * The whole file is inside ARK_BUS_REPLAY_ENABLED, so a build that does not ask
 * for the window links none of it.  No allocation, no libc, no clock.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_REPLAY_ENABLED

/* The entry tracking `id`, or the first free slot, or NULL when the table is
 * full and `id` is not already in it. */
static ark_bus_replay_entry_t *slot_for(ark_bus_t *bus, uint32_t id)
{
    ark_bus_replay_entry_t *free_slot = (ark_bus_replay_entry_t *)0;
    int i;

    for (i = 0; i < (int)ARK_BUS_REPLAY_PEERS; i++) {
        if (bus->replay[i].used) {
            if (bus->replay[i].id == id) {
                return &bus->replay[i];
            }
        } else if (!free_slot) {
            free_slot = &bus->replay[i];
        }
    }
    return free_slot;
}

int ark_bus_replay_check(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_replay_entry_t *e;
    int16_t                 diff;

    if (!bus || !f) {
        return ARK_BUS_E_INVAL;
    }
    /* No identity to track: leave the frame alone (the core never sends from
     * id 0; a frame that claims it is already refused elsewhere). */
    if (f->src_id == 0u) {
        return ARK_BUS_OK;
    }

    e = slot_for(bus, f->src_id);
    if (!e) {
        return ARK_BUS_E_STATE;   /* table full: fail closed */
    }

    if (!e->used) {
        e->used = 1u;
        e->id   = f->src_id;
        e->last = f->seq;
        e->seen = 1u;
        return ARK_BUS_OK;
    }

    /* Signed 16-bit distance: > 0 means ahead of `last`, < 0 means behind. */
    diff = (int16_t)(f->seq - e->last);

    if (diff > 0) {
        if (diff >= (int16_t)ARK_BUS_REPLAY_WINDOW) {
            /* A jump past the whole window: accept it and forget the history,
             * which must have been long-lost frames or a peer restart. */
            e->seen = 1u;
        } else {
            e->seen = (e->seen << diff) | 1u;
        }
        e->last = f->seq;
        return ARK_BUS_OK;
    }

    if (diff == 0) {
        return ARK_BUS_E_STATE;   /* bit 0 is always set: a replay */
    }

    {
        int back = -(int)diff;
        uint64_t bit;

        if (back >= (int)ARK_BUS_REPLAY_WINDOW) {
            return ARK_BUS_E_STATE;   /* older than the window */
        }
        bit = (uint64_t)1u << back;
        if ((e->seen & bit) != 0u) {
            return ARK_BUS_E_STATE;   /* already accepted: a replay */
        }
        e->seen |= bit;
        return ARK_BUS_OK;
    }
}

#endif /* ARK_BUS_REPLAY_ENABLED */
