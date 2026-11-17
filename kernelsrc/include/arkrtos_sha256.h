/**
* @file arkrtos_sha256.h
* @brief SHA-256 (FIPS 180-4), shared by the image/payload signature code and
*        the console login (SEC-G01).
* @details This used to be a static block inside arkrtos_sign.c, which meant the
*          only way to hash a runtime string was to turn the *image signature*
*          switch on.  The console login needs a hash without needing an image
*          signing key, so the primitive moved here - one source, two callers,
*          instead of two copies that could drift (a hash that disagrees with
*          itself reads as "the password is right but the login says no").
*
*          It is header-only *on purpose*.  A separate .c was tried first and
*          cost 32 bytes of text in the default build: with the block in its
*          own translation unit gcc could no longer inline it into the HMAC
*          wrappers, so the default image stopped being byte-identical to the
*          one before the change.  Keeping the block in the includer's TU
*          restores that identity; the price is one machine-code copy per TU
*          that includes this header (only when a feature that hashes is on).
*
*          Include it **inside** the feature guard of the caller:
*            - arkrtos_sign.c  under ARKRTOS_USE_IMAGE_SIGN
*            - arkrtos_shell_auth.c under ARKRTOS_USE_SHELL_AUTH
*          A build with none of those on never includes it at all.
*
*          Not constant time in the input length (it is a hash, not a MAC
*          comparison); callers that compare digests must do the comparison
*          byte-wise without early exit - see arkrtos_shell_auth.c.
*
* @author xw
*/
#ifndef __ARKRTOS_SHA256_H__
#define __ARKRTOS_SHA256_H__

#include "arkrtos_types.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/** @brief Digest length in bytes. */
#define ARKRTOS_SHA256_LEN          (32u)

/* Callers use a subset of the four entry points (the signer never asks for a
 * one-shot hash, the login never streams), so mark them unused to keep -Wall
 * quiet without forcing every caller to reference all of them. */
#if defined(__GNUC__)
#define ARKRTOS_SHA256_UNUSED       __attribute__((unused))
#else
#define ARKRTOS_SHA256_UNUSED
#endif

/** @brief Incremental hash context (caller owns it; may live on the stack). */
typedef struct
{
    uint32 state[8];
    unsigned long long bitlen;
    uint8  buf[64];
    uint32 buflen;
} arkrtos_sha256_t;

#define ARKRTOS_ROR(x, n)  (((x) >> (n)) | ((x) << (32u - (n))))
#define ARKRTOS_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define ARKRTOS_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define ARKRTOS_EP0(x)     (ARKRTOS_ROR(x,2u)  ^ ARKRTOS_ROR(x,13u) ^ ARKRTOS_ROR(x,22u))
#define ARKRTOS_EP1(x)     (ARKRTOS_ROR(x,6u)  ^ ARKRTOS_ROR(x,11u) ^ ARKRTOS_ROR(x,25u))
#define ARKRTOS_SG0(x)     (ARKRTOS_ROR(x,7u)  ^ ARKRTOS_ROR(x,18u) ^ ((x) >> 3u))
#define ARKRTOS_SG1(x)     (ARKRTOS_ROR(x,17u) ^ ARKRTOS_ROR(x,19u) ^ ((x) >> 10u))

static const uint32 arkrtos_sha256_k[64] = {
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

static void arkrtos_sha256_block(arkrtos_sha256_t *c, const uint8 *p)
{
    uint32 w[64];
    uint32 a;
    uint32 b;
    uint32 cc;
    uint32 d;
    uint32 e;
    uint32 f;
    uint32 g;
    uint32 h;
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        w[i] = ((uint32)p[i * 4u] << 24) | ((uint32)p[(i * 4u) + 1u] << 16) |
               ((uint32)p[(i * 4u) + 2u] << 8) | (uint32)p[(i * 4u) + 3u];
    }
    for(i = 16u; i < 64u; i++)
    {
        w[i] = ARKRTOS_SG1(w[i - 2u]) + w[i - 7u] + ARKRTOS_SG0(w[i - 15u]) + w[i - 16u];
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

    for(i = 0u; i < 64u; i++)
    {
        uint32 t1 = h + ARKRTOS_EP1(e) + ARKRTOS_CH(e, f, g) + arkrtos_sha256_k[i] + w[i];
        uint32 t2 = ARKRTOS_EP0(a) + ARKRTOS_MAJ(a, b, cc);

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

ARKRTOS_SHA256_UNUSED
static void arkrtos_sha256_init(arkrtos_sha256_t *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0ull;
    c->buflen = 0u;
}

ARKRTOS_SHA256_UNUSED
static void arkrtos_sha256_update(arkrtos_sha256_t *c, const uint8 *data, uint32 len)
{
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        c->buf[c->buflen++] = data[i];
        if(c->buflen == 64u)
        {
            arkrtos_sha256_block(c, c->buf);
            c->bitlen += 512ull;
            c->buflen = 0u;
        }
    }
}

ARKRTOS_SHA256_UNUSED
static void arkrtos_sha256_final(arkrtos_sha256_t *c, uint8 out[32])
{
    uint32 i = c->buflen;

    /* Pad: 0x80, then zeros, then the 64 bit big endian bit count. */
    c->bitlen += (unsigned long long)c->buflen * 8ull;
    c->buf[i++] = 0x80u;
    if(i > 56u)
    {
        while(i < 64u) { c->buf[i++] = 0u; }
        arkrtos_sha256_block(c, c->buf);
        i = 0u;
    }
    while(i < 56u) { c->buf[i++] = 0u; }
    for(i = 0u; i < 8u; i++)
    {
        c->buf[56u + i] = (uint8)(c->bitlen >> (56u - (i * 8u)));
    }
    arkrtos_sha256_block(c, c->buf);

    for(i = 0u; i < 8u; i++)
    {
        out[i * 4u]      = (uint8)(c->state[i] >> 24);
        out[(i * 4u) + 1u] = (uint8)(c->state[i] >> 16);
        out[(i * 4u) + 2u] = (uint8)(c->state[i] >> 8);
        out[(i * 4u) + 3u] = (uint8)(c->state[i]);
    }
}

/** @brief One shot: digest of a single buffer (console login, small payloads). */
ARKRTOS_SHA256_UNUSED
static void arkrtos_sha256_hash(const uint8 *data, uint32 len, uint8 out[32])
{
    arkrtos_sha256_t c;

    arkrtos_sha256_init(&c);
    if((data != 0) && (len != 0u))
    {
        arkrtos_sha256_update(&c, data, len);
    }
    arkrtos_sha256_final(&c, out);
}

#if (defined(__cplusplus))
}
#endif

#endif /* __ARKRTOS_SHA256_H__ */
