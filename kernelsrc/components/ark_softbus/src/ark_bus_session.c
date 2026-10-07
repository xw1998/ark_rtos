/*
 * ark_bus_session.c - the pre-shared-key-authenticated X25519 handshake and
 * the per-peer session table.  See ark_bus_session.h for the contract.
 *
 * Wire format (payload of a HANDSHAKE_REQ / HANDSHAKE_REP frame; both are 52
 * bytes, big-endian where multi-byte, and both travel sealed by the
 * pre-shared key like every other frame -- no session key exists yet):
 *
 *   off  0        1        version (ARK_BUS_HS_VER)
 *   off  1        1        role: 1 = initiator (REQ), 2 = responder (REP)
 *   off  2        2        epoch, chosen by the initiator and echoed
 *   off  4       32        the sender's ephemeral X25519 public key
 *   off 36       16        confirmation tag
 *
 * The confirmation tag authenticates the exchange with the pre-shared key:
 *   auth_key  = HMAC-SHA256(psk, "arkrtos-softbus/hs-auth-v1")
 *   REQ  tag  = HMAC(auth_key, role(1) | idA(4) | idB(4) | epoch(2) | ephA(32))[0:16]
 *   REP  tag  = HMAC(auth_key, role(1) | idA(4) | idB(4) | epoch(2) | ephA(32) | ephB(32))[0:16]
 * where idA is the initiator, idB the responder.  Without the pre-shared key
 * an attacker cannot forge either tag, so it cannot complete the exchange even
 * though both ephemeral public keys are on the wire in the clear.
 *
 * The session key comes from the ephemeral shared secret, never the psk:
 *   shared  = X25519(eph_sk_local, eph_pk_peer)   (rejected if all zero)
 *   prk     = HKDF-Extract(salt = ephA | ephB, ikm = shared)
 *   mac_key = HKDF-Expand(prk, "arkrtos-softbus/sess-mac-v1", 32)
 *   enc_key = HKDF-Expand(prk, "arkrtos-softbus/sess-enc-v1", 32)
 * Both ends compute identical values.  The ephemeral secrets are wiped once
 * the session is ready, which is what gives the handshake forward secrecy.
 *
 * Nothing here allocates; the whole file is gated, so a build without the
 * handshake links none of it (proven by tools/footprint.sh).
 */
#include "ark_bus_session.h"

#if ARK_BUS_HANDSHAKE_ENABLED

#include "ark_bus_priv.h"
#include "ark_bus_crypto.h"
#include "ark_bus_hkdf.h"
#include "ark_bus_x25519.h"

#define ARK_BUS_HS_VER   1u
#define ARK_BUS_HS_REQ   1u
#define ARK_BUS_HS_REP   2u
#define ARK_BUS_HS_LEN   52u   /* version + role + epoch + pk + 16-byte tag */

/* Session states (ark_bus_session_t.state). */
#define HS_FREE     0u
#define HS_PENDING  1u   /* initiator, waiting for the reply */
#define HS_READY    2u   /* keys installed                       */
#define HS_REVOKED  3u   /* denied until a fresh handshake       */

static const char k_auth_label[] = "arkrtos-softbus/hs-auth-v1";
static const char k_mac_label[]  = "arkrtos-softbus/sess-mac-v1";
static const char k_enc_label[]  = "arkrtos-softbus/sess-enc-v1";

/* ------------------------------------------------------------------ */
/* Table helpers                                                       */
/* ------------------------------------------------------------------ */

static ark_bus_session_t *hs_find(ark_bus_t *bus, uint32_t peer)
{
    int i;
    if (!bus || peer == 0u) {
        return NULL;
    }
    for (i = 0; i < ARK_BUS_SESSION_PEERS; i++) {
        if (bus->sessions[i].state != HS_FREE &&
            bus->sessions[i].peer == peer) {
            return &bus->sessions[i];
        }
    }
    return NULL;
}

static const ark_bus_session_t *hs_find_c(const ark_bus_t *bus, uint32_t peer)
{
    return hs_find((ark_bus_t *)bus, peer);
}

/* A slot for `peer`: its own entry if it has one, else a free one, else a
 * revoked one (a revocation is not worth keeping over a live session we are
 * about to establish).  NULL when every slot is a live session. */
static ark_bus_session_t *hs_alloc(ark_bus_t *bus, uint32_t peer)
{
    ark_bus_session_t *e = hs_find(bus, peer);
    int i;
    if (e) {
        return e;
    }
    for (i = 0; i < ARK_BUS_SESSION_PEERS; i++) {
        if (bus->sessions[i].state == HS_FREE) {
            return &bus->sessions[i];
        }
    }
    for (i = 0; i < ARK_BUS_SESSION_PEERS; i++) {
        if (bus->sessions[i].state == HS_REVOKED) {
            return &bus->sessions[i];
        }
    }
    return NULL;
}

static void hs_wipe_ephemeral(ark_bus_session_t *e)
{
    ark_memset(e->eph_sk, 0, sizeof(e->eph_sk));
    ark_memset(e->eph_pk, 0, sizeof(e->eph_pk));
}

/* Move the current pair to "previous" (for the rollover window) and install
 * `mac`/`enc` as the current pair. */
static void hs_install_keys(ark_bus_session_t *e,
                            const uint8_t mac[32], const uint8_t enc[32])
{
    if (e->state == HS_READY) {
        ark_memcpy(e->pmac_key, e->mac_key, 32u);
        ark_memcpy(e->penc_key, e->enc_key, 32u);
        e->have_prev = 1u;
    }
    ark_memcpy(e->mac_key, mac, 32u);
    ark_memcpy(e->enc_key, enc, 32u);
    hs_wipe_ephemeral(e);
    e->state = HS_READY;
}

static void hs_finish(ark_bus_t *bus, ark_bus_session_t *e, int status)
{
    if (e->done) {
        /* The callback runs outside this entry's lifetime concerns: it may
         * query ark_bus_session_ready() and will see status == ARK_BUS_OK. */
        ark_bus_handshake_done_fn fn = e->done;
        void *ctx = e->done_ctx;
        uint32_t peer = e->peer;
        e->done = NULL;
        e->done_ctx = NULL;
        fn(bus, peer, status, ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Crypto helpers                                                      */
/* ------------------------------------------------------------------ */

/* session subkeys from the ephemeral shared secret, both sides identical. */
static void hs_derive_keys(const uint8_t shared[32],
                           const uint8_t eph_a[32], const uint8_t eph_b[32],
                           uint8_t mac[32], uint8_t enc[32])
{
    uint8_t salt[64];
    uint8_t prk[32];

    ark_memcpy(salt, eph_a, 32u);
    ark_memcpy(salt + 32, eph_b, 32u);
    ark_bus_hkdf_extract(salt, 64u, shared, 32u, prk);
    ark_bus_hkdf_expand(prk, (const uint8_t *)k_mac_label,
                        (uint32_t)(sizeof(k_mac_label) - 1u), mac, 32u);
    ark_bus_hkdf_expand(prk, (const uint8_t *)k_enc_label,
                        (uint32_t)(sizeof(k_enc_label) - 1u), enc, 32u);
    ark_memset(salt, 0, sizeof(salt));
    ark_memset(prk, 0, sizeof(prk));
}

static void hs_auth_key(const ark_bus_t *bus, uint8_t out[32])
{
    ark_bus_hmac_sha256(bus->crypto_key, bus->crypto_key_len,
                        (const uint8_t *)k_auth_label,
                        (uint32_t)(sizeof(k_auth_label) - 1u), out);
}

/* Constant-time compare of a 16-byte confirmation tag. */
static int hs_tag_ok(const uint8_t *got, const uint8_t *want)
{
    uint8_t diff = 0u;
    int     i;
    for (i = 0; i < 16; i++) {
        diff |= (uint8_t)(got[i] ^ want[i]);
    }
    return diff == 0u;
}

/* Compute the confirmation tag over role|idA|idB|epoch|ephA|ephB. */
static void hs_confirm(const uint8_t auth_key[32], uint8_t role,
                       uint32_t id_a, uint32_t id_b, uint16_t epoch,
                       const uint8_t eph_a[32], const uint8_t *eph_b,
                       uint8_t out[32])
{
    uint8_t hdr[11];
    hdr[0] = role;
    ark_wr32(hdr + 1, id_a);
    ark_wr32(hdr + 5, id_b);
    ark_wr16(hdr + 9, epoch);
    ark_hmac_sha256_3(auth_key, 32u, hdr, 11u, eph_a, 32u,
                      eph_b, eph_b ? 32u : 0u, out);
}

/* Fill 32 bytes of ephemeral-secret material.  Uses the HAL's random source;
 * with none, only proceeds when ARK_BUS_CRYPTO_ALLOW_WEAK_RNG is set, and then
 * mixes what little varies (tick, ids, epoch, a counter) -- documented as weak,
 * never presented as strong. */
static int hs_random(ark_bus_t *bus, uint32_t peer, uint16_t epoch,
                     uint8_t out[32])
{
    if (ark_bus_rng_bytes(out, 32u) == 0) {
        return ARK_BUS_OK;
    }
#if ARK_BUS_CRYPTO_ALLOW_WEAK_RNG
    {
        uint32_t x = ark_bus_tick_ms() ^ (bus->self_id * 2654435761u) ^
                     (peer * 40503u) ^ ((uint32_t)epoch << 16) ^
                     (uint32_t)(uintptr_t)bus;
        int i;
        for (i = 0; i < 32; i++) {
            x = x * 1664525u + 1013904223u;
            out[i] = (uint8_t)(x >> 24);
        }
        return ARK_BUS_OK;
    }
#else
    (void)bus; (void)peer; (void)epoch;
    return ARK_BUS_E_NOSYS;
#endif
}

/* ------------------------------------------------------------------ */
/* Key selection used by the crypto layer                              */
/* ------------------------------------------------------------------ */

int ark_bus_session_keys(const ark_bus_t *bus, uint32_t peer,
                         const uint8_t **mac_key, const uint8_t **enc_key)
{
    const ark_bus_session_t *e = hs_find_c(bus, peer);
    if (!e || e->state != HS_READY) {
        return 0;
    }
    if (mac_key) {
        *mac_key = e->mac_key;
    }
    if (enc_key) {
        *enc_key = e->enc_key;
    }
    return 1;
}

int ark_bus_session_prev_keys(const ark_bus_t *bus, uint32_t peer,
                              const uint8_t **mac_key, const uint8_t **enc_key)
{
    const ark_bus_session_t *e = hs_find_c(bus, peer);
    if (!e || e->state != HS_READY || !e->have_prev) {
        return 0;
    }
    if (mac_key) {
        *mac_key = e->pmac_key;
    }
    if (enc_key) {
        *enc_key = e->penc_key;
    }
    return 1;
}

int ark_bus_session_denied(const ark_bus_t *bus, uint32_t peer)
{
    const ark_bus_session_t *e = hs_find_c(bus, peer);
    return (e && e->state == HS_REVOKED) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int ark_bus_session_ready(const struct ark_bus *bus, uint32_t peer)
{
    return ark_bus_session_keys(bus, peer, NULL, NULL);
}

uint16_t ark_bus_session_epoch(const struct ark_bus *bus, uint32_t peer)
{
    const ark_bus_session_t *e = hs_find_c(bus, peer);
    return (e && e->state == HS_READY) ? e->epoch : 0u;
}

int ark_bus_session_count(const struct ark_bus *bus)
{
    int i, n = 0;
    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_SESSION_PEERS; i++) {
        if (bus->sessions[i].state == HS_READY) {
            n++;
        }
    }
    return n;
}

/* Build and send one handshake frame (role, epoch, our public key, tag). */
static int hs_send(ark_bus_t *bus, uint32_t peer, uint8_t role, uint16_t epoch,
                   const uint8_t eph_pk[32], uint32_t id_a, uint32_t id_b,
                   const uint8_t eph_a[32], const uint8_t *eph_b)
{
    uint8_t auth_key[32];
    uint8_t tag[32];
    uint8_t msg[ARK_BUS_HS_LEN];
    uint8_t mtype = (role == ARK_BUS_HS_REQ) ? ARK_BUS_MSG_HANDSHAKE_REQ
                                             : ARK_BUS_MSG_HANDSHAKE_REP;

    hs_auth_key(bus, auth_key);
    hs_confirm(auth_key, role, id_a, id_b, epoch, eph_a, eph_b, tag);
    ark_memset(auth_key, 0, sizeof(auth_key));

    msg[0] = ARK_BUS_HS_VER;
    msg[1] = role;
    ark_wr16(msg + 2, epoch);
    ark_memcpy(msg + 4, eph_pk, 32u);
    ark_memcpy(msg + 36, tag, 16u);

    return ark_bus_emit(bus, peer, mtype, 0u, msg, ARK_BUS_HS_LEN, 0u);
}

int ark_bus_handshake_start(struct ark_bus *bus, uint32_t peer,
                            ark_bus_handshake_done_fn done, void *ctx)
{
    ark_bus_session_t *e;
    uint16_t epoch;
    uint8_t  tag[32];
    int      rc;

    if (!bus || peer == 0u || peer == bus->self_id) {
        return ARK_BUS_E_INVAL;
    }
    if (!bus->crypto_key_set) {
        return ARK_BUS_E_STATE;   /* the psk is the trust anchor */
    }

    e = hs_alloc(bus, peer);
    if (!e) {
        return ARK_BUS_E_FULL;
    }
    ark_memset(e, 0, sizeof(*e));
    e->peer = peer;
    e->role = ARK_BUS_HS_REQ;
    e->state = HS_PENDING;
    e->done = done;
    e->done_ctx = ctx;

    epoch = (uint16_t)(bus->session_epoch + 1u);
    if (epoch == 0u) {
        epoch = 1u;   /* keep 0 as "no epoch" */
    }
    bus->session_epoch = epoch;
    e->epoch = epoch;

    rc = hs_random(bus, peer, epoch, e->eph_sk);
    if (rc != ARK_BUS_OK) {
        ark_memset(e, 0, sizeof(*e));
        return rc;
    }
    ark_bus_x25519_clamp(e->eph_sk);
    ark_bus_x25519_base(e->eph_pk, e->eph_sk);

    (void)tag;
    rc = hs_send(bus, peer, ARK_BUS_HS_REQ, epoch, e->eph_pk,
                 bus->self_id, peer, e->eph_pk, NULL);
    if (rc < 0) {
        ark_memset(e, 0, sizeof(*e));
        return rc;
    }
    e->retry_at_ms = ark_bus_tick_ms() + (uint32_t)ARK_BUS_HANDSHAKE_RETRY_MS;
    e->retries = 0u;
    return ARK_BUS_OK;
}

/* Handle an incoming HANDSHAKE_REQ (we are the responder). */
static void hs_on_req(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_session_t *e;
    uint8_t  auth_key[32];
    uint8_t  want[32];
    uint8_t  shared[32];
    uint8_t  mac[32], enc[32];
    uint16_t epoch;
    uint32_t id_a = f->src_id;   /* the initiator */
    uint32_t id_b = bus->self_id;
    const uint8_t *eph_a;

    if (f->payload_len < ARK_BUS_HS_LEN || f->payload[0] != ARK_BUS_HS_VER ||
        f->payload[1] != ARK_BUS_HS_REQ) {
        bus->stats.hs_fail++;
        return;
    }
    epoch = ark_rd16(f->payload + 2);
    eph_a = f->payload + 4;

    hs_auth_key(bus, auth_key);
    hs_confirm(auth_key, ARK_BUS_HS_REQ, id_a, id_b, epoch, eph_a, NULL, want);
    if (!hs_tag_ok(f->payload + 36, want)) {
        ark_memset(auth_key, 0, sizeof(auth_key));
        ark_memset(want, 0, sizeof(want));
        bus->stats.hs_fail++;
        return;
    }
    ark_memset(auth_key, 0, sizeof(auth_key));
    ark_memset(want, 0, sizeof(want));

    /* Ignore a stale request: a replayed REQ would otherwise tear down a newer
     * session.  Epochs are per-initiator, so this compares like for like. */
    e = hs_find(bus, f->src_id);
    if (e && e->state == HS_READY && e->epoch >= epoch) {
        return;
    }

    e = hs_alloc(bus, f->src_id);
    if (!e) {
        bus->stats.hs_fail++;
        return;   /* table full of live sessions: fail closed */
    }
    if (e->state == HS_READY) {
        ark_memcpy(e->pmac_key, e->mac_key, 32u);
        ark_memcpy(e->penc_key, e->enc_key, 32u);
        e->have_prev = 1u;
    } else {
        e->have_prev = 0u;
    }
    e->peer = f->src_id;
    e->role = ARK_BUS_HS_REP;
    e->epoch = epoch;
    e->done = NULL;
    e->done_ctx = NULL;

    if (hs_random(bus, f->src_id, epoch, e->eph_sk) != ARK_BUS_OK) {
        e->state = HS_FREE;
        bus->stats.hs_fail++;
        return;
    }
    ark_bus_x25519_clamp(e->eph_sk);
    ark_bus_x25519_base(e->eph_pk, e->eph_sk);

    if (ark_bus_x25519(shared, e->eph_sk, eph_a) != 0) {
        ark_memset(e, 0, sizeof(*e));   /* low-order point: refuse */
        bus->stats.hs_fail++;
        return;
    }
    hs_derive_keys(shared, eph_a, e->eph_pk, mac, enc);
    ark_memset(shared, 0, sizeof(shared));

    e->state = HS_READY;
    ark_memcpy(e->mac_key, mac, 32u);
    ark_memcpy(e->enc_key, enc, 32u);
    /* wipe the ephemeral secret now that the session key exists */
    ark_memset(e->eph_sk, 0, sizeof(e->eph_sk));

    /* Reply (still sealed with the pre-shared key). */
    if (hs_send(bus, f->src_id, ARK_BUS_HS_REP, epoch, e->eph_pk,
                id_a, id_b, eph_a, e->eph_pk) < 0) {
        ark_memset(e, 0, sizeof(*e));
        bus->stats.hs_fail++;
        return;
    }
    ark_memset(mac, 0, sizeof(mac));
    ark_memset(enc, 0, sizeof(enc));
    bus->stats.hs_ok++;
}

/* Handle an incoming HANDSHAKE_REP (we are the initiator). */
static void hs_on_rep(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_session_t *e = hs_find(bus, f->src_id);
    uint8_t  auth_key[32];
    uint8_t  want[32];
    uint8_t  shared[32];
    uint8_t  mac[32], enc[32];
    uint16_t epoch;
    uint32_t id_a = bus->self_id;   /* we are the initiator */
    uint32_t id_b = f->src_id;
    const uint8_t *eph_b;

    if (!e || e->state != HS_PENDING || e->role != ARK_BUS_HS_REQ) {
        return;   /* no outstanding request for this peer */
    }
    if (f->payload_len < ARK_BUS_HS_LEN || f->payload[0] != ARK_BUS_HS_VER ||
        f->payload[1] != ARK_BUS_HS_REP) {
        bus->stats.hs_fail++;
        hs_finish(bus, e, ARK_BUS_E_AUTH);
        ark_memset(e, 0, sizeof(*e));
        return;
    }
    epoch = ark_rd16(f->payload + 2);
    eph_b = f->payload + 4;

    if (epoch != e->epoch) {
        return;   /* not the reply to our current attempt */
    }

    hs_auth_key(bus, auth_key);
    hs_confirm(auth_key, ARK_BUS_HS_REP, id_a, id_b, epoch,
               e->eph_pk, eph_b, want);
    if (!hs_tag_ok(f->payload + 36, want)) {
        ark_memset(auth_key, 0, sizeof(auth_key));
        ark_memset(want, 0, sizeof(want));
        bus->stats.hs_fail++;
        hs_finish(bus, e, ARK_BUS_E_AUTH);
        ark_memset(e, 0, sizeof(*e));
        return;
    }
    ark_memset(auth_key, 0, sizeof(auth_key));
    ark_memset(want, 0, sizeof(want));

    if (ark_bus_x25519(shared, e->eph_sk, eph_b) != 0) {
        ark_memset(e, 0, sizeof(*e));
        bus->stats.hs_fail++;
        return;
    }
    hs_derive_keys(shared, e->eph_pk, eph_b, mac, enc);
    ark_memset(shared, 0, sizeof(shared));

    hs_install_keys(e, mac, enc);
    ark_memset(mac, 0, sizeof(mac));
    ark_memset(enc, 0, sizeof(enc));
    bus->stats.hs_ok++;
    hs_finish(bus, e, ARK_BUS_OK);
}

void ark_bus_session_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    if (!bus || !f) {
        return;
    }
    if (f->msg_type == ARK_BUS_MSG_HANDSHAKE_REQ) {
        hs_on_req(bus, f);
    } else if (f->msg_type == ARK_BUS_MSG_HANDSHAKE_REP) {
        hs_on_rep(bus, f);
    }
}

void ark_bus_session_tick(ark_bus_t *bus)
{
    int      i;
    uint32_t now;

    if (!bus) {
        return;
    }
    now = ark_bus_tick_ms();
    for (i = 0; i < ARK_BUS_SESSION_PEERS; i++) {
        ark_bus_session_t *e = &bus->sessions[i];
        if (e->state != HS_PENDING || e->role != ARK_BUS_HS_REQ) {
            continue;
        }
        if ((int32_t)(now - e->retry_at_ms) < 0) {
            continue;
        }
        if (e->retries >= (uint8_t)ARK_BUS_HANDSHAKE_RETRY_MAX) {
            bus->stats.hs_fail++;
            hs_finish(bus, e, ARK_BUS_E_TIMEOUT);
            ark_memset(e, 0, sizeof(*e));
            continue;
        }
        /* Resend the same request (same epoch, same ephemeral key). */
        (void)hs_send(bus, e->peer, ARK_BUS_HS_REQ, e->epoch, e->eph_pk,
                      bus->self_id, e->peer, e->eph_pk, NULL);
        e->retries++;
        e->retry_at_ms = now + (uint32_t)ARK_BUS_HANDSHAKE_RETRY_MS;
    }
}

int ark_bus_session_revoke(struct ark_bus *bus, uint32_t peer)
{
    ark_bus_session_t *e;
    if (!bus || peer == 0u) {
        return ARK_BUS_E_INVAL;
    }
    e = hs_find(bus, peer);
    if (!e) {
        return ARK_BUS_E_NOTFOUND;
    }
    ark_memset(e->mac_key, 0, 32u);
    ark_memset(e->enc_key, 0, 32u);
    ark_memset(e->pmac_key, 0, 32u);
    ark_memset(e->penc_key, 0, 32u);
    hs_wipe_ephemeral(e);
    e->have_prev = 0u;
    e->peer = peer;
    e->done = NULL;
    e->done_ctx = NULL;
    e->state = HS_REVOKED;
    return ARK_BUS_OK;
}

int ark_bus_session_forget(struct ark_bus *bus, uint32_t peer)
{
    ark_bus_session_t *e;
    if (!bus || peer == 0u) {
        return ARK_BUS_E_INVAL;
    }
    e = hs_find(bus, peer);
    if (!e) {
        return ARK_BUS_E_NOTFOUND;
    }
    ark_memset(e, 0, sizeof(*e));
    return ARK_BUS_OK;
}

#endif /* ARK_BUS_HANDSHAKE_ENABLED */
