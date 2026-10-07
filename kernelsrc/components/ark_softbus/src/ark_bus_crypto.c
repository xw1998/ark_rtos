/*
 * ark_bus_crypto.c - frame authentication: truncated HMAC-SHA256.
 *
 * See ark_bus_crypto.h for the contract.  The whole file is inside one #if, so
 * a build that does not enable the feature links none of it (proven by
 * tools/footprint.sh).  Nothing here allocates and nothing here is secret but
 * the key: SHA-256 is FIPS 180-4 and HMAC is RFC 2104, checked against the
 * published vectors in tests/test_ark_bus.c.
 */
#include "ark_bus_crypto.h"

#if ARK_BUS_CRYPTO_ENABLED

#include "ark_bus_priv.h"
#if ARK_BUS_ENCRYPT_ENABLED
#include "ark_bus_chacha20.h"
#endif

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[8];
    uint64_t bits;      /* message length in bits so far          */
    uint8_t  buf[64];
    uint16_t n;         /* bytes held in buf                      */
} sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int      i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1  = rr(e, 6) ^ rr(e, 11) ^ rr(e, 25);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = h + s1 + ch + K256[i] + w[i];
        uint32_t s0  = rr(a, 2) ^ rr(a, 13) ^ rr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2  = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bits = 0u;
    c->n    = 0u;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, uint32_t len)
{
    while (len > 0u) {
        uint32_t take = 64u - (uint32_t)c->n;
        if (take > len) {
            take = len;
        }
        ark_memcpy(c->buf + c->n, p, take);
        c->n = (uint16_t)(c->n + take);
        p += take;
        len -= take;
        if (c->n == 64u) {
            sha256_compress(c, c->buf);
            c->bits += 512u;
            c->n = 0u;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bits + (uint64_t)c->n * 8u;
    uint32_t z    = ((uint32_t)c->n + 1u) & 63u;
    uint32_t zeros = (z <= 56u) ? (56u - z) : (120u - z);
    uint8_t  tail[72];
    uint32_t t = 0u;
    int      i;

    tail[t++] = 0x80u;
    while (zeros-- > 0u) {
        tail[t++] = 0u;
    }
    for (i = 7; i >= 0; i--) {
        tail[t++] = (uint8_t)(bits >> (i * 8));
    }
    sha256_update(c, tail, t);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void ark_bus_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    if (data && len) {
        sha256_update(&c, data, len);
    }
    sha256_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 (RFC 2104)                                              */
/* ------------------------------------------------------------------ */

/* One key, up to three message segments a|b|c, so the header and the payload
 * (and, for HKDF, the counter) can be authenticated without a scratch copy of
 * any of them.  A NULL/zero-length segment is skipped.  `out` may alias `a`. */
static void hmac_sha256_3(const uint8_t *key, uint16_t klen,
                          const uint8_t *a, uint32_t alen,
                          const uint8_t *b, uint32_t blen,
                          const uint8_t *c3, uint32_t clen,
                          uint8_t out[32])
{
    uint8_t    k[64];
    uint8_t    ipad[64];
    uint8_t    opad[64];
    uint8_t    inner[32];
    sha256_ctx c;
    int        i;

    ark_memset(k, 0, sizeof(k));
    if (klen > 64u) {
        ark_bus_sha256(key, klen, k);   /* long key: hash it to 32 bytes */
    } else {
        ark_memcpy(k, key, klen);
    }
    for (i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }

    sha256_init(&c);
    sha256_update(&c, ipad, 64u);
    if (a && alen) {
        sha256_update(&c, a, alen);
    }
    if (b && blen) {
        sha256_update(&c, b, blen);
    }
    if (c3 && clen) {
        sha256_update(&c, c3, clen);
    }
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64u);
    sha256_update(&c, inner, 32u);
    sha256_final(&c, out);
}

/* Two segments, the common case in the seal/open path. */
static void hmac_sha256_2(const uint8_t *key, uint16_t klen,
                          const uint8_t *a, uint32_t alen,
                          const uint8_t *b, uint32_t blen,
                          uint8_t out[32])
{
    hmac_sha256_3(key, klen, a, alen, b, blen, (const uint8_t *)0, 0u, out);
}
#if ARK_BUS_HANDSHAKE_ENABLED
void ark_hmac_sha256_3(const uint8_t *key, uint16_t klen,
                       const uint8_t *a, uint32_t alen,
                       const uint8_t *b, uint32_t blen,
                       const uint8_t *c3, uint32_t clen,
                       uint8_t out[32])
{
    hmac_sha256_3(key, klen, a, alen, b, blen, c3, clen, out);
}
#endif

void ark_bus_hmac_sha256(const uint8_t *key, uint16_t klen,
                         const uint8_t *data, uint32_t len, uint8_t out[32])
{
    hmac_sha256_3(key, klen, data, len, (const uint8_t *)0, 0u,
                  (const uint8_t *)0, 0u, out);
}

/* ------------------------------------------------------------------ */
/* The seam: seal an outgoing payload, open an incoming frame          */
/* ------------------------------------------------------------------ */

/* The header bytes a tag covers: the fields an attacker would want to rewrite
 * (src_id above all), plus the wire payload length so a truncation cannot hide
 * inside a shorter frame.  When the replay window is enabled the sequence
 * number joins them (core.c reserves it before sealing), so a captured frame
 * cannot be renumbered to slip past the window; with the window off `seq` is
 * not hashed and the tag is byte-for-byte what it was before. */
static void auth_input(uint32_t src, uint32_t dst, uint8_t msg_type,
                       uint8_t flags, uint16_t topic_id, uint16_t plen,
                       uint16_t seq, uint8_t hdr[16])
{
    hdr[0]  = (uint8_t)(src >> 24); hdr[1] = (uint8_t)(src >> 16);
    hdr[2]  = (uint8_t)(src >> 8);  hdr[3] = (uint8_t)(src);
    hdr[4]  = (uint8_t)(dst >> 24); hdr[5] = (uint8_t)(dst >> 16);
    hdr[6]  = (uint8_t)(dst >> 8);  hdr[7] = (uint8_t)(dst);
    hdr[8]  = msg_type;
    hdr[9]  = flags;
    hdr[10] = (uint8_t)(topic_id >> 8); hdr[11] = (uint8_t)(topic_id);
    hdr[12] = (uint8_t)(plen >> 8);     hdr[13] = (uint8_t)(plen);
    hdr[14] = (uint8_t)(seq >> 8);      hdr[15] = (uint8_t)(seq);
}

int ark_bus_crypto_seal(ark_bus_t *bus, uint8_t msg_type, uint8_t flags,
                        uint16_t topic_id, uint32_t src, uint32_t dst,
                        uint16_t seq, const uint8_t *payload, uint16_t len,
                        uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    uint8_t        hdr[16];
    uint8_t        mac[32];
    const uint8_t *mac_key;
    uint16_t       mac_klen;
    uint16_t       alen = 14u;   /* header bytes hashed */
    uint16_t       body;         /* bytes between header and tag on the wire */
#if ARK_BUS_ENCRYPT_ENABLED
    const uint8_t *enc_key;      /* cipher key: the peer's, else the group's */
#endif
#if ARK_BUS_HANDSHAKE_ENABLED
    const uint8_t *s_mac = NULL;
    const uint8_t *s_enc = NULL;
    int            use_session = 0;
#endif

    if (!bus || !out || !out_len) {
        return ARK_BUS_E_INVAL;
    }
    if (!bus->crypto_key_set) {
        return ARK_BUS_E_STATE;   /* caller only seals when a key is installed */
    }

#if ARK_BUS_HANDSHAKE_ENABLED
    /* Prefer the peer's session key.  A handshake frame is the exception: it
     * always travels under the pre-shared key so a rotation is readable no
     * matter which keys either end currently holds. */
    use_session = (ARK_BUS_IS_HANDSHAKE_TYPE(msg_type) == 0) &&
                  ark_bus_session_keys(bus, dst, &s_mac, &s_enc);
#endif

#if ARK_BUS_ENCRYPT_ENABLED
    /* Encrypt-then-MAC: the ciphertext (behind a nonce counter) is what the
     * tag covers, so a tampered ciphertext can never be decrypted into
     * something that looks valid.  The MAC key is the derived subkey, never
     * the pre-shared key directly. */
    body    = (uint16_t)(ARK_BUS_CRYPTO_NONCE_LEN + len);
    mac_klen = 32u;
#if ARK_BUS_HANDSHAKE_ENABLED
    if (use_session) {
        mac_key = s_mac;
        enc_key = s_enc;
    } else
#endif
    {
        mac_key = bus->crypto_mac_key;
        enc_key = bus->crypto_enc_key;
    }
#else
    body    = len;
#if ARK_BUS_HANDSHAKE_ENABLED
    if (use_session) {
        mac_key  = s_mac;
        mac_klen = 32u;
    } else
#endif
    {
        mac_key  = bus->crypto_key;
        mac_klen = bus->crypto_key_len;
    }
#endif

#if ARK_BUS_REPLAY_ENABLED
    alen = 16u;   /* the sequence number is authenticated */
#endif

    if ((uint32_t)body + ARK_BUS_CRYPTO_TAG_LEN > (uint32_t)cap) {
        return ARK_BUS_E_NOMEM;
    }

#if ARK_BUS_ENCRYPT_ENABLED
    {
        uint8_t  nonce[12];
        uint64_t ctr = ++bus->crypto_enc_counter;   /* 1, 2, 3, ... never 0 */
        int      k;

        /* Nonce = src_id (4 bytes, in the header and authenticated) followed by
         * the per-sender 64-bit counter.  Only the counter travels: the
         * receiver pairs it with the src_id it already verified. */
        for (k = 0; k < 8; k++) {
            out[k] = (uint8_t)(ctr >> (56 - 8 * k));
        }
        nonce[0] = (uint8_t)(src >> 24); nonce[1] = (uint8_t)(src >> 16);
        nonce[2] = (uint8_t)(src >> 8);  nonce[3] = (uint8_t)(src);
        for (k = 0; k < 8; k++) {
            nonce[4 + k] = (uint8_t)(ctr >> (56 - 8 * k));
        }
        /* Block counter 1: block 0 is reserved should this ever grow into an
         * AEAD construction (RFC 8439 uses block 0 for the Poly1305 key). */
        ark_bus_chacha20_xor(enc_key, 1u, nonce, payload,
                             out + ARK_BUS_CRYPTO_NONCE_LEN, len);
    }
#else
    if (len) {
        ark_memcpy(out, payload, len);
    }
#endif

    /* `flags` already carries ARK_BUS_FLAG_AUTHENTICATED (set by emit), so the
     * bytes hashed here are exactly the bytes the wire header will carry.  The
     * tag covers the header and everything after it up to the tag: ciphertext
     * (or plaintext) and, when encrypting, the nonce counter. */
    auth_input(src, dst, msg_type, flags, topic_id,
               (uint16_t)(body + ARK_BUS_CRYPTO_TAG_LEN), seq, hdr);
    hmac_sha256_2(mac_key, mac_klen, hdr, alen, out, body, mac);
    ark_memcpy(out + body, mac, ARK_BUS_CRYPTO_TAG_LEN);
    *out_len = (uint16_t)(body + ARK_BUS_CRYPTO_TAG_LEN);
    return ARK_BUS_OK;
}

int ark_bus_crypto_open(const ark_bus_t *bus, ark_bus_frame_t *f,
                        uint8_t *scratch, uint16_t cap)
{
    uint16_t       alen = 14u;
    uint16_t       body;        /* bytes between header and tag */
    /* The keys a frame could have been sealed under, in the order they are
     * tried.  At most the peer's current session key, its previous one (a
     * rotation still in flight) and the group key. */
    const uint8_t *mac_src[3];
    uint16_t       mac_len[3];
#if ARK_BUS_ENCRYPT_ENABLED
    const uint8_t *enc_src[3];
    uint16_t       plain_len;   /* decrypted length (ENCRYPT only) */
#endif
    int            ncand = 0;
    int            hit   = -1;
    int            k;

    if (!bus || !f) {
        return ARK_BUS_E_INVAL;
    }

    if (!bus->crypto_key_set) {
        /* An unkeyed node can neither verify a tag nor trust the bytes behind
         * one, so a tagged frame is refused rather than dispatched with its
         * tag still counted as payload.  Plain frames pass untouched. */
        return ((f->flags & ARK_BUS_FLAG_AUTHENTICATED) != 0u)
                   ? ARK_BUS_E_AUTH : ARK_BUS_OK;
    }

    /* A keyed node will not take a frame that carries no tag: accepting one
     * would let an attacker strip the tag and forge src_id. */
    if ((f->flags & ARK_BUS_FLAG_AUTHENTICATED) == 0u ||
        f->payload_len < ARK_BUS_CRYPTO_TAG_LEN) {
        return ARK_BUS_E_AUTH;
    }

#if ARK_BUS_ENCRYPT_ENABLED
    if (f->payload_len <
        (uint16_t)(ARK_BUS_CRYPTO_NONCE_LEN + ARK_BUS_CRYPTO_TAG_LEN)) {
        return ARK_BUS_E_AUTH;   /* too short to even hold nonce + tag */
    }
    body      = (uint16_t)(f->payload_len - ARK_BUS_CRYPTO_TAG_LEN);
    plain_len = (uint16_t)(body - ARK_BUS_CRYPTO_NONCE_LEN);
#else
    body      = (uint16_t)(f->payload_len - ARK_BUS_CRYPTO_TAG_LEN);
#endif

#if ARK_BUS_REPLAY_ENABLED
    alen = 16u;
#endif

#if ARK_BUS_HANDSHAKE_ENABLED
    /* A handshake frame is always the group key (there may be no session yet,
     * and a rotation must be readable regardless of the keys held). */
    if (ARK_BUS_IS_HANDSHAKE_TYPE(f->msg_type) == 0) {
        const uint8_t *m;
        const uint8_t *e2;
        if (ark_bus_session_keys(bus, f->src_id, &m, &e2)) {
            mac_src[ncand] = m; mac_len[ncand] = 32u;
#if ARK_BUS_ENCRYPT_ENABLED
            enc_src[ncand] = e2;
#endif
            ncand++;
        }
        if (ark_bus_session_prev_keys(bus, f->src_id, &m, &e2)) {
            mac_src[ncand] = m; mac_len[ncand] = 32u;
#if ARK_BUS_ENCRYPT_ENABLED
            enc_src[ncand] = e2;
#endif
            ncand++;
        }
    }
#endif
#if ARK_BUS_ENCRYPT_ENABLED
    mac_src[ncand] = bus->crypto_mac_key; mac_len[ncand] = 32u;
    enc_src[ncand] = bus->crypto_enc_key;
#else
    mac_src[ncand] = bus->crypto_key;     mac_len[ncand] = bus->crypto_key_len;
#endif
    ncand++;

    /* Constant-time compare for each candidate: a byte-at-a-time early exit
     * would leak how much of a guessed tag is right. */
    for (k = 0; k < ncand && hit < 0; k++) {
        uint8_t hdr[16];
        uint8_t mac[32];
        uint8_t diff = 0u;
        uint8_t i;

        auth_input(f->src_id, f->dst_id, f->msg_type, f->flags, f->topic_id,
                   f->payload_len, f->seq, hdr);
        hmac_sha256_2(mac_src[k], mac_len[k], hdr, alen, f->payload, body, mac);
        for (i = 0u; i < (uint8_t)ARK_BUS_CRYPTO_TAG_LEN; i++) {
            diff |= (uint8_t)(mac[i] ^ f->payload[body + i]);
        }
        if (diff == 0u) {
            hit = k;
        }
    }
    if (hit < 0) {
        return ARK_BUS_E_AUTH;
    }

#if ARK_BUS_ENCRYPT_ENABLED
    {
        uint8_t  nonce[12];
        uint64_t ctr = 0u;
        int      j;

        if (plain_len > cap) {
            return ARK_BUS_E_NOMEM;
        }
        for (j = 0; j < 8; j++) {
            ctr = (ctr << 8) | f->payload[j];
        }
        /* Rebuild the nonce the sender used: the src_id it authenticated, plus
         * the counter it carried.  Decryption is the same XOR. */
        nonce[0] = (uint8_t)(f->src_id >> 24); nonce[1] = (uint8_t)(f->src_id >> 16);
        nonce[2] = (uint8_t)(f->src_id >> 8);  nonce[3] = (uint8_t)(f->src_id);
        for (j = 0; j < 8; j++) {
            nonce[4 + j] = (uint8_t)(ctr >> (56 - 8 * j));
        }
        ark_bus_chacha20_xor(enc_src[hit], 1u, nonce,
                             f->payload + ARK_BUS_CRYPTO_NONCE_LEN, scratch,
                             plain_len);
        f->payload     = scratch;
        f->payload_len = plain_len;
    }
#else
    (void)scratch;
    (void)cap;
    f->payload_len = body;
#endif

    f->flags = (uint8_t)(f->flags & (uint8_t)~ARK_BUS_FLAG_AUTHENTICATED);
    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Key management                                                      */
/* ------------------------------------------------------------------ */

int ark_bus_crypto_set_key(struct ark_bus *bus, const uint8_t *key,
                           uint16_t len)
{
    if (!bus || !key || len == 0u || len > (uint16_t)ARK_BUS_CRYPTO_KEY_MAX) {
        return ARK_BUS_E_INVAL;
    }
    ark_memset(bus->crypto_key, 0, sizeof(bus->crypto_key));
    ark_memcpy(bus->crypto_key, key, len);
    bus->crypto_key_len = (uint8_t)len;
    bus->crypto_key_set = 1u;
#if ARK_BUS_ENCRYPT_ENABLED
    /* Key separation: derive the MAC and cipher subkeys from the pre-shared
     * key so one secret never feeds two constructions directly.  Both ends
     * derive them the same way, so only the pre-shared key has to be agreed.
     * The counter starts at 0 and is pre-incremented on first use, so the
     * first frame carries nonce counter 1 (0 is never used). */
    {
        static const char mac_label[] = "arkrtos-softbus/mac-v1";
        static const char enc_label[] = "arkrtos-softbus/enc-v1";
        ark_bus_hmac_sha256(bus->crypto_key, len,
                            (const uint8_t *)mac_label,
                            (uint32_t)(sizeof(mac_label) - 1u),
                            bus->crypto_mac_key);
        ark_bus_hmac_sha256(bus->crypto_key, len,
                            (const uint8_t *)enc_label,
                            (uint32_t)(sizeof(enc_label) - 1u),
                            bus->crypto_enc_key);
        bus->crypto_enc_counter = 0u;
    }
#endif
    return ARK_BUS_OK;
}

void ark_bus_crypto_clear_key(struct ark_bus *bus)
{
    if (!bus) {
        return;
    }
    ark_memset(bus->crypto_key, 0, sizeof(bus->crypto_key));
    bus->crypto_key_len = 0u;
    bus->crypto_key_set = 0u;
#if ARK_BUS_ENCRYPT_ENABLED
    ark_memset(bus->crypto_mac_key, 0, sizeof(bus->crypto_mac_key));
    ark_memset(bus->crypto_enc_key, 0, sizeof(bus->crypto_enc_key));
    bus->crypto_enc_counter = 0u;
#endif
}

int ark_bus_crypto_key_set(const struct ark_bus *bus)
{
    return (bus && bus->crypto_key_set) ? 1 : 0;
}

#endif /* ARK_BUS_CRYPTO_ENABLED */
