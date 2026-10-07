/*
 * ark_bus_chacha20.c - ChaCha20 (RFC 8439), straight from the paper.
 *
 * See ark_bus_chacha20.h for the contract.  The state is 16 little-endian
 * uint32 words: four constants, eight key words, the block counter and three
 * nonce words.  Twenty rounds (ten double rounds) of quarter-rounds mix it,
 * then the original state is added back.  The keystream is XORed with the
 * message; decryption is the same call.
 *
 * This file is inside one #if so a build without ARK_BUS_ENCRYPT_ENABLED links
 * none of it.  No tables, no allocation, no libc.
 */
#include "ark_bus_chacha20.h"

#if ARK_BUS_ENCRYPT_ENABLED

#define CHACHA_CONST0 0x61707865u  /* "expa" */
#define CHACHA_CONST1 0x3320646eu  /* "nd 3" */
#define CHACHA_CONST2 0x79622d32u  /* "2-by" */
#define CHACHA_CONST3 0x6b206574u  /* "te k" */

static uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

/* RFC 8439 section 2.1 quarter round, inlined so the compiler sees one scope
 * per round and needs no helper call. */
#define QR(a, b, c, d)                              \
    do {                                            \
        (a) += (b); (d) ^= (a); (d) = rotl32((d), 16); \
        (c) += (d); (b) ^= (c); (b) = rotl32((b), 12); \
        (a) += (b); (d) ^= (a); (d) = rotl32((d), 8);  \
        (c) += (d); (b) ^= (c); (b) = rotl32((b), 7);  \
    } while (0)

/* Read/write one little-endian word, the only place wire order shows up. */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Build the 16-word state from key/counter/nonce. */
static void chacha_state(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], uint32_t st[16])
{
    int i;

    st[0] = CHACHA_CONST0;
    st[1] = CHACHA_CONST1;
    st[2] = CHACHA_CONST2;
    st[3] = CHACHA_CONST3;
    for (i = 0; i < 8; i++) {
        st[4 + i] = rd_le32(key + 4 * i);
    }
    st[12] = counter;
    st[13] = rd_le32(nonce);
    st[14] = rd_le32(nonce + 4);
    st[15] = rd_le32(nonce + 8);
}

static void chacha_block_raw(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    int      i;

    for (i = 0; i < 16; i++) {
        x[i] = in[i];
    }

    for (i = 0; i < 10; i++) {
        /* Column rounds. */
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        /* Diagonal rounds. */
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }

    for (i = 0; i < 16; i++) {
        wr_le32(out + 4 * i, x[i] + in[i]);
    }
}

void ark_bus_chacha20_block(const uint8_t key[32], uint32_t counter,
                            const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t st[16];
    chacha_state(key, counter, nonce, st);
    chacha_block_raw(st, out);
}

void ark_bus_chacha20_xor(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], const uint8_t *in,
                          uint8_t *out, uint32_t len)
{
    uint8_t  ks[64];
    uint32_t st[16];
    uint32_t off = 0u;

    chacha_state(key, counter, nonce, st);

    while (off < len) {
        uint32_t take = len - off;
        uint32_t i;

        if (take > 64u) {
            take = 64u;
        }
        chacha_block_raw(st, ks);
        for (i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        }
        /* Next block: bump the block counter word and rebuild the state so the
         * keystream never repeats for one (key, nonce). */
        st[12]++;
        off += take;
    }
}

#endif /* ARK_BUS_ENCRYPT_ENABLED */
