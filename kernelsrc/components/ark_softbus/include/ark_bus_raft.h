/*
 * ark_bus_raft.h - a small Raft consensus layer: elect one leader and replicate
 * a log across the cluster so every member applies the same entries in the same
 * order (a replicated state machine).
 *
 * The core can already reach a peer, move an object and run a group operation,
 * but none of that makes a group of nodes *agree* on one value.  Agreement is
 * what lets a cluster of gateways pick a coordinator, keep a shared config, or
 * hand a task to exactly one node.  That is this layer (the design guide's
 * part 9 and appendix B).
 *
 * It is crash-fault tolerant, not Byzantine: a minority of nodes may stop, and
 * a majority still makes progress; nodes that lie are out of scope.  It is
 * deliberately small so it fits the same MCU-first budget as everything else:
 * no heap, no libc, a fixed log, one entry in flight per follower.  Entries live
 * in RAM, not flash -- this is demonstration-grade Raft, and a full log refuses
 * a proposal rather than losing an entry.
 *
 * Compile-gated (ARK_BUS_RAFT_ENABLED) and off by default; a build that does
 * not ask for it links none of this and its behaviour is unchanged.
 */
#ifndef ARK_BUS_RAFT_H
#define ARK_BUS_RAFT_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_protocol.h"

#if ARK_BUS_RAFT_ENABLED

struct ark_bus;

typedef enum {
    ARK_BUS_RAFT_FOLLOWER  = 0,
    ARK_BUS_RAFT_CANDIDATE = 1,
    ARK_BUS_RAFT_LEADER    = 2
} ark_bus_raft_role_t;

/* One replicated log entry.  Indices are 1-based (index 0 means "nothing"). */
typedef struct {
    uint32_t term;
    uint32_t index;
    uint16_t len;
    uint8_t  data[ARK_BUS_RAFT_ENTRY_MAX];
} ark_bus_raft_entry_t;

/* Called once per entry when it becomes committed, in index order. */
typedef void (*ark_bus_raft_apply_cb_t)(uint32_t index, const void *data,
                                        uint16_t len, void *user);

typedef struct {
    ark_bus_raft_entry_t log[ARK_BUS_RAFT_LOG_MAX];

    uint32_t members[ARK_BUS_RAFT_MAX_MEMBERS];     /* cluster, including self */
    uint8_t  member_count;
    uint32_t next_index[ARK_BUS_RAFT_MAX_MEMBERS];  /* leader's view per member */
    uint32_t match_index[ARK_BUS_RAFT_MAX_MEMBERS]; /* leader's view per member */

    ark_bus_raft_role_t role;
    uint32_t current_term;
    uint32_t voted_for;    /* 0 = no vote cast this term   */
    uint32_t leader_id;    /* 0 = unknown                  */
    uint32_t commit_index; /* highest committed index      */
    uint32_t last_applied; /* highest applied index        */
    uint32_t log_count;    /* entries stored; indices 1..log_count */

    uint8_t  votes_granted;
    uint32_t election_timeout_ms;
    uint32_t election_deadline_ms;
    uint32_t last_heartbeat_ms;

    ark_bus_raft_apply_cb_t apply_cb;
    void                   *apply_user;
} ark_bus_raft_state_t;

/* Called by ark_bus_init(). */
void ark_bus_raft_init(struct ark_bus *bus);

/* Drives elections and leader heartbeats; called from ark_bus_poll(). */
void ark_bus_raft_tick(struct ark_bus *bus);

/* Handle an incoming Raft message; called from dispatch. */
void ark_bus_raft_handle(struct ark_bus *bus, const ark_bus_frame_t *f);

/*
 * Declare the cluster: `ids` are the node ids that take part, `self` included.
 * Returns ARK_BUS_OK, ARK_BUS_E_INVAL on an empty/oversized list or a list that
 * does not contain ourselves, or ARK_BUS_E_FULL past ARK_BUS_RAFT_MAX_MEMBERS.
 */
int ark_bus_raft_set_cluster(struct ark_bus *bus, const uint32_t *ids,
                             uint8_t count);

/* Register the state-machine callback invoked for each committed entry. */
int ark_bus_raft_set_apply_cb(struct ark_bus *bus, ark_bus_raft_apply_cb_t cb,
                              void *user);

/*
 * Propose an entry.  Only the leader accepts: on any other role this returns
 * ARK_BUS_E_STATE, so a caller never silently writes to a follower.  On success
 * it returns the entry's index (positive); the entry is committed only once a
 * majority has stored it, at which point the apply callback fires.  A full log
 * or a too-large payload is refused (ARK_BUS_E_FULL / ARK_BUS_E_INVAL).
 */
int ark_bus_raft_propose(struct ark_bus *bus, const void *data, uint16_t len);

/* Introspection. */
ark_bus_raft_role_t ark_bus_raft_role(const struct ark_bus *bus);
uint32_t ark_bus_raft_term(const struct ark_bus *bus);
uint32_t ark_bus_raft_leader(const struct ark_bus *bus);
uint32_t ark_bus_raft_commit_index(const struct ark_bus *bus);
uint32_t ark_bus_raft_last_index(const struct ark_bus *bus);
const ark_bus_raft_state_t *ark_bus_raft_state(const struct ark_bus *bus);

#endif /* ARK_BUS_RAFT_ENABLED */

#endif /* ARK_BUS_RAFT_H */
