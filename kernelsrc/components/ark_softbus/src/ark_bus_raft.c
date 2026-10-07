/*
 * ark_bus_raft.c - a small Raft (see ark_bus_raft.h).
 *
 * Nothing here allocates: the log and the per-member bookkeeping live in
 * ark_bus_t, and every message is built in a stack buffer.  The whole file
 * compiles to nothing when ARK_BUS_RAFT_ENABLED is 0.
 *
 * It keeps one entry in flight per follower rather than batching, which costs
 * round trips but keeps the state machine and the buffers tiny -- the right
 * trade on an MCU.  The rules follow the Raft paper (Ongaro & Ousterhout, "In
 * Search of an Understandable Consensus Algorithm", USENIX ATC 2014):
 *
 *   - a candidate needs a majority of votes to lead;
 *   - a vote is granted only to a candidate whose log is at least as
 *     up-to-date as ours, so a leader can never lack a committed entry;
 *   - a follower refuses an AppendEntries whose prev index/term does not match,
 *     and the leader walks its next_index back and retries until it does;
 *   - the leader commits an entry from its own term once a majority has stored
 *     it, and tells followers to commit via leader_commit.
 *
 * Timing uses the HAL clock through ark_bus_tick_ms(); a follower campaigns
 * after ARK_BUS_RAFT_ELECTION_MIN_MS (+ a per-id spread) with no heartbeat.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_RAFT_ENABLED

#include "ark_bus_raft.h"
#include "ark_bus_hal.h"

/* Wire payload sizes. */
#define RAFT_VOTE_REQ_LEN   16u   /* term|cand|last_idx|last_term          */
#define RAFT_VOTE_REP_LEN    9u   /* term|voter|granted                    */
#define RAFT_APPEND_HDR     21u   /* term|leader|prev_idx|prev_term|commit|count */
#define RAFT_ENTRY_HDR      10u   /* index|term|len                        */
#define RAFT_APPEND_REP_LEN 13u   /* term|follower|success|match_index     */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t raft_majority(const ark_bus_raft_state_t *r)
{
    return (uint32_t)(r->member_count / 2u) + 1u;
}

static uint32_t raft_last_index(const ark_bus_raft_state_t *r)
{
    return r->log_count;
}

static uint32_t raft_last_term(const ark_bus_raft_state_t *r)
{
    return r->log_count > 0u ? r->log[r->log_count - 1u].term : 0u;
}

static ark_bus_raft_entry_t *raft_entry_at(ark_bus_raft_state_t *r, uint32_t idx)
{
    if (idx < 1u || idx > r->log_count) {
        return NULL;
    }
    return &r->log[idx - 1u];
}

static int raft_member_slot(const ark_bus_raft_state_t *r, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < r->member_count; i++) {
        if (r->members[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

static void raft_reset_timer(ark_bus_raft_state_t *r)
{
    r->election_deadline_ms = ark_bus_tick_ms() + r->election_timeout_ms;
}

static void raft_broadcast(ark_bus_t *bus, uint8_t msg_type,
                           const uint8_t *buf, uint16_t len)
{
    const ark_bus_raft_state_t *r = &bus->raft;
    uint8_t i;
    for (i = 0; i < r->member_count; i++) {
        if (r->members[i] == bus->self_id) {
            continue;
        }
        (void)ark_bus_emit(bus, r->members[i], msg_type, ARK_BUS_TOPIC_NONE,
                           buf, len, 0);
    }
}

/* Apply every committed-but-not-yet-applied entry, in index order. */
static void raft_apply(ark_bus_t *bus)
{
    ark_bus_raft_state_t *r = &bus->raft;
    while (r->last_applied < r->commit_index) {
        ark_bus_raft_entry_t *e;
        r->last_applied++;
        e = raft_entry_at(r, r->last_applied);
        if (e && r->apply_cb) {
            r->apply_cb(e->index, e->data, e->len, r->apply_user);
        }
    }
}

static void raft_become_follower(ark_bus_t *bus, uint32_t term)
{
    ark_bus_raft_state_t *r = &bus->raft;
    r->role      = ARK_BUS_RAFT_FOLLOWER;
    r->current_term = term;
    r->voted_for = 0;
    raft_reset_timer(r);
}

static void raft_send_append(ark_bus_t *bus, uint8_t slot);

static void raft_become_leader(ark_bus_t *bus)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint8_t i;
    r->role      = ARK_BUS_RAFT_LEADER;
    r->leader_id = bus->self_id;
    r->last_heartbeat_ms = ark_bus_tick_ms();
    for (i = 0; i < r->member_count; i++) {
        r->next_index[i]  = r->log_count + 1u;
        r->match_index[i] = 0u;
    }
    /* Assert leadership at once: the first heartbeat also stops a peer that was
     * about to campaign from winning. */
    for (i = 0; i < r->member_count; i++) {
        if (r->members[i] != bus->self_id) {
            raft_send_append(bus, i);
        }
    }
}

static void raft_start_election(ark_bus_t *bus)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint8_t  buf[RAFT_VOTE_REQ_LEN];

    r->role         = ARK_BUS_RAFT_CANDIDATE;
    r->current_term++;
    r->voted_for    = bus->self_id;
    r->leader_id    = 0;
    r->votes_granted = 1u;   /* our own vote */
    raft_reset_timer(r);

    ark_wr32(buf + 0, r->current_term);
    ark_wr32(buf + 4, bus->self_id);
    ark_wr32(buf + 8, raft_last_index(r));
    ark_wr32(buf + 12, raft_last_term(r));
    raft_broadcast(bus, ARK_BUS_MSG_RAFT_VOTE_REQ, buf, RAFT_VOTE_REQ_LEN);

    if (r->votes_granted >= raft_majority(r)) {
        raft_become_leader(bus);
    }
}

static void raft_send_append(ark_bus_t *bus, uint8_t slot)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint8_t  buf[RAFT_APPEND_HDR + RAFT_ENTRY_HDR + ARK_BUS_RAFT_ENTRY_MAX];
    uint32_t next = r->next_index[slot];
    uint32_t prev = next > 0u ? next - 1u : 0u;
    uint32_t prev_term = 0u;
    uint8_t  count = 0u;
    uint16_t len   = RAFT_APPEND_HDR;

    if (prev >= 1u && prev <= r->log_count) {
        prev_term = r->log[prev - 1u].term;
    }

    ark_wr32(buf + 0,  r->current_term);
    ark_wr32(buf + 4,  bus->self_id);
    ark_wr32(buf + 8,  prev);
    ark_wr32(buf + 12, prev_term);
    ark_wr32(buf + 16, r->commit_index);

    /* Send the next entry if the follower is behind; otherwise this is a bare
     * heartbeat (count 0). */
    if (next >= 1u && next <= r->log_count) {
        const ark_bus_raft_entry_t *e = &r->log[next - 1u];
        ark_wr32(buf + RAFT_APPEND_HDR + 0, e->index);
        ark_wr32(buf + RAFT_APPEND_HDR + 4, e->term);
        buf[RAFT_APPEND_HDR + 8] = (uint8_t)(e->len >> 8);
        buf[RAFT_APPEND_HDR + 9] = (uint8_t)(e->len & 0xFFu);
        ark_memcpy(buf + RAFT_APPEND_HDR + RAFT_ENTRY_HDR, e->data, e->len);
        len   = (uint16_t)(RAFT_APPEND_HDR + RAFT_ENTRY_HDR + e->len);
        count = 1u;
    }
    buf[20] = count;

    (void)ark_bus_emit(bus, r->members[slot], ARK_BUS_MSG_RAFT_APPEND,
                       ARK_BUS_TOPIC_NONE, buf, len, 0);
}

/* Advance commit_index to the highest entry from our term that a majority has
 * stored, then apply. */
static void raft_try_advance_commit(ark_bus_t *bus)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint32_t n;

    for (n = r->log_count; n > r->commit_index; n--) {
        uint8_t  i;
        uint32_t count = 1u;   /* the leader itself */
        if (n < 1u || n > r->log_count) {
            continue;
        }
        if (r->log[n - 1u].term != r->current_term) {
            continue;   /* only entries from our own term are committed directly */
        }
        for (i = 0; i < r->member_count; i++) {
            if (r->members[i] == bus->self_id) {
                continue;
            }
            if (r->match_index[i] >= n) {
                count++;
            }
        }
        if (count >= raft_majority(r)) {
            r->commit_index = n;
            raft_apply(bus);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Message handling                                                    */
/* ------------------------------------------------------------------ */

static void raft_handle_vote_req(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint8_t  rep[RAFT_VOTE_REP_LEN];
    uint32_t term, cand, last_idx, last_term;
    uint8_t  granted = 0u;

    if (f->payload_len < RAFT_VOTE_REQ_LEN) {
        return;
    }
    term      = ark_rd32(f->payload + 0);
    cand      = ark_rd32(f->payload + 4);
    last_idx  = ark_rd32(f->payload + 8);
    last_term = ark_rd32(f->payload + 12);

    if (term < r->current_term) {
        granted = 0u;   /* a stale candidate */
    } else {
        if (term > r->current_term) {
            raft_become_follower(bus, term);
        }
        /* Up-to-date test: the candidate's log must not be behind ours. */
        if ((last_term > raft_last_term(r)) ||
            (last_term == raft_last_term(r) && last_idx >= raft_last_index(r))) {
            if (r->voted_for == 0u || r->voted_for == cand) {
                r->voted_for = cand;
                granted = 1u;
                raft_reset_timer(r);
            }
        }
    }

    ark_wr32(rep + 0, r->current_term);
    ark_wr32(rep + 4, bus->self_id);
    rep[8] = granted;
    (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_RAFT_VOTE_REP,
                       ARK_BUS_TOPIC_NONE, rep, RAFT_VOTE_REP_LEN, 0);
}

static void raft_handle_vote_rep(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint32_t term;
    uint8_t  granted;

    if (f->payload_len < RAFT_VOTE_REP_LEN || r->role != ARK_BUS_RAFT_CANDIDATE) {
        return;
    }
    term    = ark_rd32(f->payload + 0);
    granted = f->payload[8];

    if (term > r->current_term) {
        raft_become_follower(bus, term);
        return;
    }
    if (term < r->current_term || !granted) {
        return;
    }
    r->votes_granted++;
    if (r->votes_granted >= raft_majority(r)) {
        raft_become_leader(bus);
    }
}

static void raft_handle_append(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint8_t  rep[RAFT_APPEND_REP_LEN];
    uint32_t term, leader, prev_idx, prev_term, leader_commit;
    uint8_t  count, success = 0u;
    uint32_t match;

    if (f->payload_len < RAFT_APPEND_HDR) {
        return;
    }
    term          = ark_rd32(f->payload + 0);
    leader        = ark_rd32(f->payload + 4);
    prev_idx      = ark_rd32(f->payload + 8);
    prev_term     = ark_rd32(f->payload + 12);
    leader_commit = ark_rd32(f->payload + 16);
    count         = f->payload[20];

    if (term < r->current_term) {
        /* Stale leader: tell it our term and drop the append. */
        ark_wr32(rep + 0, r->current_term);
        ark_wr32(rep + 4, bus->self_id);
        rep[8] = 0u;
        ark_wr32(rep + 9, r->log_count);
        (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_RAFT_APPEND_REP,
                           ARK_BUS_TOPIC_NONE, rep, RAFT_APPEND_REP_LEN, 0);
        return;
    }

    if (term > r->current_term) {
        raft_become_follower(bus, term);
    }
    if (r->role != ARK_BUS_RAFT_FOLLOWER) {
        /* We were candidate or leader but a newer/equal term arrived. */
        r->role = ARK_BUS_RAFT_FOLLOWER;
    }
    r->leader_id = leader;
    raft_reset_timer(r);   /* a valid leader message suppresses our own campaign */

    match = r->log_count;

    /* Consistency check on the entry just before the new ones. */
    if (prev_idx > 0u) {
        ark_bus_raft_entry_t *pe = raft_entry_at(r, prev_idx);
        if (!pe) {
            success = 0u;                 /* we are too short */
            match   = r->log_count;
            goto reply;
        }
        if (pe->term != prev_term) {
            success = 0u;                 /* conflict; leader will back up */
            match   = prev_idx > 0u ? prev_idx - 1u : 0u;
            goto reply;
        }
    }

    /* Append the entries, truncating any conflicting suffix. */
    {
        uint8_t  k;
        uint16_t off = RAFT_APPEND_HDR;
        for (k = 0; k < count; k++) {
            uint32_t eidx, eterm;
            uint16_t elen;
            if ((uint32_t)off + RAFT_ENTRY_HDR > f->payload_len) {
                break;
            }
            eidx  = ark_rd32(f->payload + off + 0);
            eterm = ark_rd32(f->payload + off + 4);
            elen  = (uint16_t)(((uint16_t)f->payload[off + 8] << 8) |
                               f->payload[off + 9]);
            off   = (uint16_t)(off + RAFT_ENTRY_HDR);
            if ((uint32_t)off + elen > f->payload_len ||
                eidx < 1u || eidx > ARK_BUS_RAFT_LOG_MAX) {
                break;
            }
            if (eidx <= r->log_count) {
                if (r->log[eidx - 1u].term == eterm) {
                    off = (uint16_t)(off + elen);   /* already have it */
                    continue;
                }
                r->log_count = eidx - 1u;           /* conflict: truncate */
            }
            if (eidx == r->log_count + 1u) {
                ark_bus_raft_entry_t *ne = &r->log[eidx - 1u];
                ne->index = eidx;
                ne->term  = eterm;
                ne->len   = elen;
                ark_memcpy(ne->data, f->payload + off, elen);
                r->log_count = eidx;
            }
            off = (uint16_t)(off + elen);
        }
    }

    match   = r->log_count;
    success = 1u;

    /* Advance our commit to the leader's, capped by what we actually hold. */
    if (leader_commit > r->commit_index) {
        uint32_t c = leader_commit < r->log_count ? leader_commit : r->log_count;
        if (c > r->commit_index) {
            r->commit_index = c;
            raft_apply(bus);
        }
    }

reply:
    ark_wr32(rep + 0, r->current_term);
    ark_wr32(rep + 4, bus->self_id);
    rep[8] = success;
    ark_wr32(rep + 9, match);
    (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_RAFT_APPEND_REP,
                       ARK_BUS_TOPIC_NONE, rep, RAFT_APPEND_REP_LEN, 0);
}

static void raft_handle_append_rep(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_raft_state_t *r = &bus->raft;
    uint32_t term, match;
    uint8_t  success;
    int      slot;

    if (f->payload_len < RAFT_APPEND_REP_LEN || r->role != ARK_BUS_RAFT_LEADER) {
        return;
    }
    term    = ark_rd32(f->payload + 0);
    success = f->payload[8];
    match   = ark_rd32(f->payload + 9);

    if (term > r->current_term) {
        raft_become_follower(bus, term);
        return;
    }
    if (term < r->current_term) {
        return;
    }

    slot = raft_member_slot(r, f->src_id);
    if (slot < 0) {
        return;
    }

    if (success) {
        if (match > r->match_index[slot]) {
            r->match_index[slot] = match;
        }
        if (match + 1u > r->next_index[slot]) {
            r->next_index[slot] = match + 1u;
        }
        raft_try_advance_commit(bus);
        /* Keep feeding this follower if it is still behind. */
        if (r->next_index[slot] <= r->log_count) {
            raft_send_append(bus, (uint8_t)slot);
        }
    } else {
        /* Walk the next index back and retry, using the follower's hint
         * (its match index + 1) when that jumps back further. */
        uint32_t hint = match + 1u;
        if (hint < 1u) {
            hint = 1u;
        }
        if (hint < r->next_index[slot]) {
            r->next_index[slot] = hint;
        } else if (r->next_index[slot] > 1u) {
            r->next_index[slot]--;
        }
        raft_send_append(bus, (uint8_t)slot);
    }
}

void ark_bus_raft_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    if (!bus || !f) {
        return;
    }
    switch (f->msg_type) {
    case ARK_BUS_MSG_RAFT_VOTE_REQ:
        raft_handle_vote_req(bus, f);
        break;
    case ARK_BUS_MSG_RAFT_VOTE_REP:
        raft_handle_vote_rep(bus, f);
        break;
    case ARK_BUS_MSG_RAFT_APPEND:
        raft_handle_append(bus, f);
        break;
    case ARK_BUS_MSG_RAFT_APPEND_REP:
        raft_handle_append_rep(bus, f);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle / drive                                                   */
/* ------------------------------------------------------------------ */

void ark_bus_raft_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    ark_memset(&bus->raft, 0, sizeof(bus->raft));
    bus->raft.role = ARK_BUS_RAFT_FOLLOWER;
    /* A provisional deadline; set_cluster() re-derives it once self_id is known
     * so the spread by id actually differs per node. */
    bus->raft.election_timeout_ms = (uint32_t)ARK_BUS_RAFT_ELECTION_MIN_MS;
    raft_reset_timer(&bus->raft);
}

void ark_bus_raft_tick(ark_bus_t *bus)
{
    ark_bus_raft_state_t *r;
    uint32_t now;

    if (!bus || bus->self_id == 0 || bus->raft.member_count == 0u) {
        return;
    }
    r   = &bus->raft;
    now = ark_bus_tick_ms();

    if (r->role == ARK_BUS_RAFT_LEADER) {
        if ((uint32_t)(now - r->last_heartbeat_ms) >=
            (uint32_t)ARK_BUS_RAFT_HEARTBEAT_MS) {
            uint8_t i;
            r->last_heartbeat_ms = now;
            for (i = 0; i < r->member_count; i++) {
                if (r->members[i] != bus->self_id) {
                    raft_send_append(bus, i);
                }
            }
        }
        return;
    }

    if ((int32_t)(now - r->election_deadline_ms) >= 0) {
        raft_start_election(bus);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int ark_bus_raft_set_cluster(ark_bus_t *bus, const uint32_t *ids, uint8_t count)
{
    ark_bus_raft_state_t *r;
    uint8_t i;
    int     saw_self = 0;

    if (!bus || !ids || count == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if (count > (uint8_t)ARK_BUS_RAFT_MAX_MEMBERS) {
        return ARK_BUS_E_FULL;
    }
    r = &bus->raft;
    r->member_count = count;
    for (i = 0; i < count; i++) {
        r->members[i] = ids[i];
        if (ids[i] == bus->self_id) {
            saw_self = 1;
        }
    }
    if (!saw_self) {
        r->member_count = 0;
        return ARK_BUS_E_INVAL;   /* we must be part of our own cluster */
    }
    for (i = 0; i < count; i++) {
        r->next_index[i]  = r->log_count + 1u;
        r->match_index[i] = 0u;
    }
    r->leader_id = 0;
    /* Re-derive the timeout here, not in init(): init() runs before start() has
     * installed self_id, so spreading it there would give every node the same
     * deadline and make them all campaign at once (split vote).  set_cluster()
     * is called after start(), so self_id is finally known. */
    r->election_timeout_ms =
        (uint32_t)ARK_BUS_RAFT_ELECTION_MIN_MS +
        (bus->self_id % 4u) * (uint32_t)ARK_BUS_RAFT_ELECTION_SPREAD_MS;
    raft_reset_timer(r);
    return ARK_BUS_OK;
}

int ark_bus_raft_set_apply_cb(ark_bus_t *bus, ark_bus_raft_apply_cb_t cb,
                              void *user)
{
    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    bus->raft.apply_cb   = cb;
    bus->raft.apply_user = user;
    return ARK_BUS_OK;
}

int ark_bus_raft_propose(ark_bus_t *bus, const void *data, uint16_t len)
{
    ark_bus_raft_state_t *r;
    ark_bus_raft_entry_t *e;
    uint8_t  i;
    uint32_t idx;

    if (!bus || (!data && len > 0u)) {
        return ARK_BUS_E_INVAL;
    }
    if (len > (uint16_t)ARK_BUS_RAFT_ENTRY_MAX) {
        return ARK_BUS_E_INVAL;
    }
    r = &bus->raft;
    if (r->role != ARK_BUS_RAFT_LEADER) {
        return ARK_BUS_E_STATE;   /* only the leader accepts a write */
    }
    if (r->log_count >= (uint32_t)ARK_BUS_RAFT_LOG_MAX) {
        return ARK_BUS_E_FULL;
    }

    idx = r->log_count + 1u;
    e   = &r->log[idx - 1u];
    e->index = idx;
    e->term  = r->current_term;
    e->len   = len;
    if (len > 0u) {
        ark_memcpy(e->data, data, len);
    }
    r->log_count = idx;

    for (i = 0; i < r->member_count; i++) {
        if (r->members[i] != bus->self_id) {
            raft_send_append(bus, i);
        }
    }
    raft_try_advance_commit(bus);   /* a single-node cluster commits at once */
    return (int)idx;
}

ark_bus_raft_role_t ark_bus_raft_role(const ark_bus_t *bus)
{
    return bus ? bus->raft.role : ARK_BUS_RAFT_FOLLOWER;
}

uint32_t ark_bus_raft_term(const ark_bus_t *bus)
{
    return bus ? bus->raft.current_term : 0u;
}

uint32_t ark_bus_raft_leader(const ark_bus_t *bus)
{
    return bus ? bus->raft.leader_id : 0u;
}

uint32_t ark_bus_raft_commit_index(const ark_bus_t *bus)
{
    return bus ? bus->raft.commit_index : 0u;
}

uint32_t ark_bus_raft_last_index(const ark_bus_t *bus)
{
    return bus ? bus->raft.log_count : 0u;
}

const ark_bus_raft_state_t *ark_bus_raft_state(const ark_bus_t *bus)
{
    return bus ? &bus->raft : NULL;
}

#endif /* ARK_BUS_RAFT_ENABLED */
