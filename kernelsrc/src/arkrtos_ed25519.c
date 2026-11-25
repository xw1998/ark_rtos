/**
* @file arkrtos_ed25519.c
* @brief Ed25519 signature verification (RFC 8032), verify-only.
* @details Written in-repo on purpose: the kernel already carries its own
*          SHA-256 rather than a crypto library, and verification is the only
*          operation a device needs (a device holds a public key and checks
*          what it is given; it never signs).  No secret ever enters this
*          file, so no constant-time care is needed for the arithmetic - what
*          matters is the *decision*: a signature is accepted only on the
*          exact encoding RFC 8032 defines, and every malformed encoding is
*          rejected rather than normalised.
*
*          Arithmetic: field elements are 16 limbs of 2^16 in ed_i64 (the
*          TweetNaCl representation), points are in extended coordinates with
*          the a = -1 formulas.  Scalars are reduced with plain long division
*          over 16-bit limbs: slower than a Barrett reduction, but the code is
*          short enough to read and the cost is paid once per image install.
*
*          Three rejections that are easy to miss and are all enforced here:
*            - the y coordinate of a public key / R must be canonical (< p),
*            - s must be canonical (< L),
*            - a recovered x = 0 with the sign bit set is not a point.
*          Verification is cofactorless (the RFC 8032 "strict" reading):
*          [s]B == R + [h]A is checked on encodings, so a signature that only
*          verifies up to the cofactor is refused.
*
*          Cost is the price of being short: one verification is 256 point
*          doublings plus ~128 additions, measured at 5.7 ms on a host x86
*          build at -O1.  It runs once per install (or once per streamed
*          image at the end), never in a hot path, and the 32-byte digest it
*          checks means the streaming installer does not have to buffer the
*          image to use it.
*
* @author xw
*/

#include "arkrtos_ed25519.h"

#if (ARKRTOS_USE_ED25519 == 1)

/* arkrtos_types.h stops at 32 bits on purpose (it is the App SDK's ABI
 * surface); 64-bit scratch stays local to this file. */
typedef unsigned long long ed_u64;
typedef signed long long ed_i64;

/* ===================================================================
 * SHA-512 (FIPS 180-4)
 * =================================================================== */

typedef struct
{
    ed_u64 h[8];
    ed_u64 len_lo;
    ed_u64 len_hi;
    uint8  buf[128];
    uint32 n;
} ed_sha512_t;

static const ed_u64 K512[80] =
{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static ed_u64 ed_ror64(ed_u64 x, uint32 n)
{
    return (x >> n) | (x << (64u - n));
}

static void ed_sha512_block(ed_sha512_t *c, const uint8 *p)
{
    ed_u64 w[80];
    ed_u64 a, b, cc, d, e, f, g, h;
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        w[i] = ((ed_u64)p[i * 8u] << 56) | ((ed_u64)p[i * 8u + 1u] << 48) |
               ((ed_u64)p[i * 8u + 2u] << 40) | ((ed_u64)p[i * 8u + 3u] << 32) |
               ((ed_u64)p[i * 8u + 4u] << 24) | ((ed_u64)p[i * 8u + 5u] << 16) |
               ((ed_u64)p[i * 8u + 6u] << 8) | (ed_u64)p[i * 8u + 7u];
    }
    for(i = 16u; i < 80u; i++)
    {
        ed_u64 s0 = ed_ror64(w[i - 15u], 1u) ^ ed_ror64(w[i - 15u], 8u) ^ (w[i - 15u] >> 7);
        ed_u64 s1 = ed_ror64(w[i - 2u], 19u) ^ ed_ror64(w[i - 2u], 61u) ^ (w[i - 2u] >> 6);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for(i = 0u; i < 80u; i++)
    {
        ed_u64 S1 = ed_ror64(e, 14u) ^ ed_ror64(e, 18u) ^ ed_ror64(e, 41u);
        ed_u64 ch = (e & f) ^ ((~e) & g);
        ed_u64 t1 = h + S1 + ch + K512[i] + w[i];
        ed_u64 S0 = ed_ror64(a, 28u) ^ ed_ror64(a, 34u) ^ ed_ror64(a, 39u);
        ed_u64 mj = (a & b) ^ (a & cc) ^ (b & cc);
        ed_u64 t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void ed_sha512_init(ed_sha512_t *c)
{
    c->h[0] = 0x6a09e667f3bcc908ULL;
    c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL;
    c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL;
    c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL;
    c->h[7] = 0x5be0cd19137e2179ULL;
    c->len_lo = 0u;
    c->len_hi = 0u;
    c->n = 0u;
}

static void ed_sha512_update(ed_sha512_t *c, const uint8 *data, uint32 len)
{
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        c->buf[c->n] = data[i];
        c->n++;
        if(c->n == 128u)
        {
            ed_sha512_block(c, c->buf);
            c->n = 0u;
            c->len_lo += 1024u;
            if(c->len_lo < 1024u)
            {
                c->len_hi++;
            }
        }
    }
}

static void ed_sha512_final(ed_sha512_t *c, uint8 out[64])
{
    ed_u64 bits_lo = c->len_lo + ((ed_u64)c->n * 8u);
    ed_u64 bits_hi = c->len_hi + ((bits_lo < ((ed_u64)c->n * 8u)) ? 1u : 0u);
    uint32 i;

    c->buf[c->n] = 0x80u;
    c->n++;
    if(c->n > 112u)
    {
        while(c->n < 128u)
        {
            c->buf[c->n] = 0u;
            c->n++;
        }
        ed_sha512_block(c, c->buf);
        c->n = 0u;
    }
    while(c->n < 112u)
    {
        c->buf[c->n] = 0u;
        c->n++;
    }
    for(i = 0u; i < 8u; i++)
    {
        c->buf[112u + i] = (uint8)(bits_hi >> (56u - (i * 8u)));
        c->buf[120u + i] = (uint8)(bits_lo >> (56u - (i * 8u)));
    }
    ed_sha512_block(c, c->buf);

    for(i = 0u; i < 8u; i++)
    {
        out[i * 8u]      = (uint8)(c->h[i] >> 56);
        out[i * 8u + 1u] = (uint8)(c->h[i] >> 48);
        out[i * 8u + 2u] = (uint8)(c->h[i] >> 40);
        out[i * 8u + 3u] = (uint8)(c->h[i] >> 32);
        out[i * 8u + 4u] = (uint8)(c->h[i] >> 24);
        out[i * 8u + 5u] = (uint8)(c->h[i] >> 16);
        out[i * 8u + 6u] = (uint8)(c->h[i] >> 8);
        out[i * 8u + 7u] = (uint8)(c->h[i]);
    }
}

/* ===================================================================
 * Field arithmetic: 16 limbs of 2^16, ed_i64 accumulators
 * =================================================================== */

typedef ed_i64 ed_fe[16];

static const uint8 K_D[32] =
{
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75,
    0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c,
    0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52
};

static const uint8 K_D2[32] =
{
    0x59, 0xf1, 0xb2, 0x26, 0x94, 0x9b, 0xd6, 0xeb,
    0x56, 0xb1, 0x83, 0x82, 0x9a, 0x14, 0xe0, 0x00,
    0x30, 0xd1, 0xf3, 0xee, 0xf2, 0x80, 0x8e, 0x19,
    0xe7, 0xfc, 0xdf, 0x56, 0xdc, 0xd9, 0x06, 0x24
};

static const uint8 K_SQRTM1[32] =
{
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4,
    0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b,
    0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b
};

static const uint8 K_EXP_INV[32] =      /* p - 2 */
{
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};

static const uint8 K_EXP_P58[32] =      /* (p - 5) / 8 = 2^252 - 3 */
{
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f
};

/* The encodings every Ed25519 implementation agrees on. */
static const uint8 K_BASE_ENC[32] =     /* base point, y = 4/5 */
{
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

/* p in 16-bit limbs: 2^255 - 19 */
static const uint32 K_P16[16] =
{
    0xffedu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu,
    0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0xffffu, 0x7fffu
};

/* The group order L in 16-bit limbs. */
static const uint32 K_L16[16] =
{
    0xd3edu, 0x5cf5u, 0x631au, 0x5812u, 0x9cd6u, 0xa2f7u, 0xf9deu, 0x14deu,
    0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x1000u
};

static void ed_fe_0(ed_fe o) { uint32 i; for(i = 0u; i < 16u; i++) { o[i] = 0; } }
static void ed_fe_1(ed_fe o) { ed_fe_0(o); o[0] = 1; }

static void ed_fe_copy(ed_fe o, const ed_fe a)
{
    uint32 i;
    for(i = 0u; i < 16u; i++) { o[i] = a[i]; }
}

/* Carry-propagate a limb vector in place.  Works for negative limbs too:
 * an arithmetic shift gives the floor, so the remainder is always in
 * [0, 2^16) and the carry can be negative. */
static void ed_fe_carry(ed_fe o)
{
    ed_i64 c;
    int32 k;

    /* Same shape as TweetNaCl's car25519: add 2^16 first so the shift never
     * has to floor a negative number, and fold the 2^256 == 38 wrap into the
     * last limb.  Written this way one pass is enough for anything below
     * 2^256, which is what the field multiplication relies on (it runs this
     * twice to be sure). */
    for(k = 0; k < 16; k++)
    {
        o[k] += ((ed_i64)1 << 16);
        c = o[k] >> 16;
        if(k < 15)
        {
            o[k + 1] += c - 1;
        }
        else
        {
            o[0] += 38 * (c - 1);
        }
        o[k] -= (c << 16);
    }
}

static void ed_fe_add(ed_fe o, const ed_fe a, const ed_fe b)
{
    uint32 i;
    for(i = 0u; i < 16u; i++) { o[i] = a[i] + b[i]; }
}

static void ed_fe_sub(ed_fe o, const ed_fe a, const ed_fe b)
{
    uint32 i;
    for(i = 0u; i < 16u; i++) { o[i] = a[i] - b[i]; }
    ed_fe_carry(o);
    ed_fe_carry(o);
}

static void ed_fe_mul(ed_fe o, const ed_fe a, const ed_fe b)
{
    ed_i64 t[31];
    uint32 i, j;

    for(i = 0u; i < 31u; i++) { t[i] = 0; }

    for(i = 0u; i < 16u; i++)
    {
        for(j = 0u; j < 16u; j++)
        {
            t[i + j] += a[i] * b[j];
        }
    }
    /* 2^256 == 38 (mod p) */
    for(i = 0u; i < 15u; i++)
    {
        t[i] += 38 * t[i + 16u];
    }
    for(i = 0u; i < 16u; i++) { o[i] = t[i]; }
    ed_fe_carry(o);
    ed_fe_carry(o);
}

static void ed_fe_sq(ed_fe o, const ed_fe a)
{
    ed_fe_mul(o, a, a);
}

static void ed_fe_neg(ed_fe o, const ed_fe a)
{
    ed_fe z;
    ed_fe_0(z);
    ed_fe_sub(o, z, a);
}

/* o = a ^ (little-endian exponent of n bytes) */
static void ed_fe_pow(ed_fe o, const ed_fe a, const uint8 *e, uint32 nbytes)
{
    ed_fe r, base;
    int32 i, bit;

    ed_fe_1(r);
    ed_fe_copy(base, a);

    for(i = (int32)nbytes - 1; i >= 0; i--)
    {
        for(bit = 7; bit >= 0; bit--)
        {
            ed_fe_sq(r, r);
            if((((uint32)e[i] >> (uint32)bit) & 1u) != 0u)
            {
                ed_fe_mul(r, r, base);
            }
        }
    }
    ed_fe_copy(o, r);
}

static void ed_fe_invert(ed_fe o, const ed_fe a)
{
    ed_fe_pow(o, a, K_EXP_INV, 32u);
}

/* Is the value >= p?  Input must already be carried (limbs in [0,2^16)). */
static int32 ed_fe_ge_p(const ed_fe a)
{
    int32 i;
    for(i = 15; i >= 0; i--)
    {
        if((uint32)a[i] > K_P16[i]) { return 1; }
        if((uint32)a[i] < K_P16[i]) { return 0; }
    }
    return 1;                     /* equal counts as >= */
}

static void ed_fe_sub_p(ed_fe o)
{
    uint32 i;
    ed_i64 borrow = 0;

    for(i = 0u; i < 16u; i++)
    {
        ed_i64 v = o[i] - (ed_i64)K_P16[i] - borrow;
        if(v < 0)
        {
            v += 65536;
            borrow = 1;
        }
        else
        {
            borrow = 0;
        }
        o[i] = v;
    }
}

static void ed_fe_tobytes(uint8 out[32], const ed_fe a)
{
    ed_fe t;
    uint32 i;

    ed_fe_copy(t, a);
    ed_fe_carry(t);
    ed_fe_carry(t);

    /* After two carries the value is < 2^256 = 2p + 38, so at most two
     * subtractions are needed; ask for three so the loop condition is the
     * honest one and a future range change cannot silently underflow. */
    for(i = 0u; (i < 3u) && (ed_fe_ge_p(t) != 0); i++)
    {
        ed_fe_sub_p(t);
    }

    for(i = 0u; i < 16u; i++)
    {
        out[i * 2u]      = (uint8)(t[i] & 0xFF);
        out[i * 2u + 1u] = (uint8)((t[i] >> 8) & 0xFF);
    }
}

static void ed_fe_frombytes(ed_fe o, const uint8 s[32])
{
    uint32 i;
    for(i = 0u; i < 16u; i++)
    {
        o[i] = (ed_i64)s[i * 2u] | ((ed_i64)s[i * 2u + 1u] << 8);
    }
}

/* Low bit of the canonical encoding: the definition of "negative" here. */
static int32 ed_fe_isneg(const ed_fe a)
{
    uint8 b[32];
    ed_fe_tobytes(b, a);
    return (int32)(b[0] & 1u);
}

static int32 ed_fe_iszero(const ed_fe a)
{
    uint8 b[32];
    uint32 i;
    uint8 acc = 0u;

    ed_fe_tobytes(b, a);
    for(i = 0u; i < 32u; i++) { acc |= b[i]; }
    return (acc == 0u) ? 1 : 0;
}

/* ===================================================================
 * Group: extended coordinates, a = -1
 * =================================================================== */

typedef struct
{
    ed_fe X;
    ed_fe Y;
    ed_fe Z;
    ed_fe T;
} ed_ge_p3;

static void ed_fe_load(ed_fe o, const uint8 s[32])
{
    ed_fe_frombytes(o, s);
    ed_fe_carry(o);
    ed_fe_carry(o);
}

static int32 ed_fe_eq(const ed_fe a, const ed_fe b)
{
    uint8 x[32];
    uint8 y[32];
    uint32 i;
    uint8 acc = 0u;

    ed_fe_tobytes(x, a);
    ed_fe_tobytes(y, b);
    for(i = 0u; i < 32u; i++) { acc |= (uint8)(x[i] ^ y[i]); }
    return (acc == 0u) ? 1 : 0;
}

static void ed_ge_identity(ed_ge_p3 *p)
{
    ed_fe_0(p->X);
    ed_fe_1(p->Y);
    ed_fe_1(p->Z);
    ed_fe_0(p->T);
}

static void ed_ge_copy(ed_ge_p3 *o, const ed_ge_p3 *a)
{
    ed_fe_copy(o->X, a->X);
    ed_fe_copy(o->Y, a->Y);
    ed_fe_copy(o->Z, a->Z);
    ed_fe_copy(o->T, a->T);
}

/* Unified addition for a = -1 twisted Edwards (RFC 8032 §5.1.4).
 * Every source read of p and q happens before the first store to o, so
 * o may alias either argument. */
static void ed_ge_add(ed_ge_p3 *o, const ed_ge_p3 *p, const ed_ge_p3 *q)
{
    ed_fe a, b, c, d, e, f, g, h;
    ed_fe ta, tb, d2;

    ed_fe_load(d2, K_D2);

    ed_fe_sub(ta, p->Y, p->X);
    ed_fe_sub(tb, q->Y, q->X);
    ed_fe_mul(a, ta, tb);

    ed_fe_add(ta, p->Y, p->X);
    ed_fe_add(tb, q->Y, q->X);
    ed_fe_mul(b, ta, tb);

    ed_fe_mul(c, p->T, q->T);
    ed_fe_mul(c, c, d2);

    ed_fe_mul(d, p->Z, q->Z);
    ed_fe_add(d, d, d);

    ed_fe_sub(e, b, a);
    ed_fe_sub(f, d, c);
    ed_fe_add(g, d, c);
    ed_fe_add(h, b, a);

    ed_fe_mul(o->X, e, f);
    ed_fe_mul(o->Y, g, h);
    ed_fe_mul(o->T, e, h);
    ed_fe_mul(o->Z, f, g);
}

/* Doubling is the unified law with p == q.  Two multiplications are spent
 * on the same operands that a dedicated law would also need, and using one
 * formula for both cases removes a whole class of "the double is right, the
 * add is not" bugs from the port. */
static void ed_ge_double(ed_ge_p3 *o, const ed_ge_p3 *p)
{
    ed_ge_add(o, p, p);
}

static void ed_ge_negate(ed_ge_p3 *o, const ed_ge_p3 *p)
{
    ed_ge_copy(o, p);
    ed_fe_neg(o->X, p->X);
    ed_fe_neg(o->T, p->T);
}

/* Decode a point.  Returns 0 on success, -1 on any of the three encodings
 * RFC 8032 requires a verifier to refuse:
 *   - y >= p (non-canonical),
 *   - x^2 * v != u and != -u (not on the curve),
 *   - x == 0 with the sign bit set (-0 is not an encoding of 0). */
static int32 ed_ge_frombytes(ed_ge_p3 *p, const uint8 s[32])
{
    ed_fe u, v, v3, v7, t, x, y, nu, sq;
    uint8 t2[32];
    int32 sign;
    uint32 i;

    for(i = 0u; i < 32u; i++) { t2[i] = s[i]; }
    sign = (int32)(t2[31] >> 7);
    t2[31] &= 0x7Fu;

    ed_fe_load(y, t2);
    if(ed_fe_ge_p(y) != 0) { return -1; }

    ed_fe_sq(t, y);                        /* t  = y^2        */
    ed_fe_1(u);
    ed_fe_sub(u, t, u);                    /* u  = y^2 - 1    */

    ed_fe_load(sq, K_D);
    ed_fe_mul(v, t, sq);                   /* v  = d*y^2      */
    ed_fe_1(t);
    ed_fe_add(v, v, t);                    /* v  = d*y^2 + 1  */

    ed_fe_sq(v3, v);
    ed_fe_mul(v3, v3, v);                  /* v3 = v^3        */
    ed_fe_sq(v7, v3);
    ed_fe_mul(v7, v7, v);                  /* v7 = v^7        */

    ed_fe_mul(t, u, v7);
    ed_fe_pow(x, t, K_EXP_P58, 32u);       /* (u*v^7)^((p-5)/8) */
    ed_fe_mul(x, x, u);
    ed_fe_mul(x, x, v3);                   /* x  = u*v^3*(u*v^7)^((p-5)/8) */

    ed_fe_sq(t, x);
    ed_fe_mul(t, t, v);                    /* t  = v*x^2      */

    if(ed_fe_eq(t, u) == 0)
    {
        ed_fe_neg(nu, u);
        if(ed_fe_eq(t, nu) == 0) { return -1; }
        ed_fe_load(sq, K_SQRTM1);
        ed_fe_mul(x, x, sq);
    }

    if((ed_fe_iszero(x) != 0) && (sign != 0)) { return -1; }
    if(ed_fe_isneg(x) != sign)
    {
        ed_fe_neg(x, x);
    }

    ed_fe_copy(p->X, x);
    ed_fe_copy(p->Y, y);
    ed_fe_1(p->Z);
    ed_fe_mul(p->T, x, y);
    return 0;
}

static void ed_ge_tobytes(uint8 s[32], const ed_ge_p3 *p)
{
    ed_fe zi, x, y;

    ed_fe_invert(zi, p->Z);
    ed_fe_mul(x, p->X, zi);
    ed_fe_mul(y, p->Y, zi);

    ed_fe_tobytes(s, y);
    s[31] ^= (uint8)(ed_fe_isneg(x) << 7);
}

static void ed_ge_base(ed_ge_p3 *p)
{
    (void)ed_ge_frombytes(p, K_BASE_ENC);
}

/* [k]P, k little-endian 32 bytes, MSB first. */
static void ed_ge_scalarmult(ed_ge_p3 *o, const uint8 k[32], const ed_ge_p3 *p)
{
    ed_ge_p3 r;
    int32 i;

    ed_ge_identity(&r);
    for(i = 255; i >= 0; i--)
    {
        ed_ge_double(&r, &r);
        if((((uint32)k[i >> 3] >> (uint32)(i & 7)) & 1u) != 0u)
        {
            ed_ge_add(&r, &r, p);
        }
    }
    ed_ge_copy(o, &r);
}

/* ===================================================================
 * Scalars: reduce modulo the group order L
 * =================================================================== */

/* 1 if a >= L.  a is 16 limbs, low first, each in [0, 2^16). */
static int32 ed_u16_ge_l(const uint32 a[16])
{
    int32 i;

    for(i = 15; i >= 0; i--)
    {
        if(a[i] > K_L16[i]) { return 1; }
        if(a[i] < K_L16[i]) { return 0; }
    }
    return 1;
}

static void ed_u16_sub_l(uint32 a[16])
{
    int32 i;
    ed_i64 borrow = 0;

    for(i = 0; i < 16; i++)
    {
        ed_i64 v = (ed_i64)a[i] - (ed_i64)K_L16[i] - borrow;
        if(v < 0)
        {
            v += 65536;
            borrow = 1;
        }
        else
        {
            borrow = 0;
        }
        a[i] = (uint32)v;
    }
}

/* Plain schoolbook long division: shift in one bit, subtract L if the
 * running remainder reached it.  512 iterations x 16 limbs - a few thousand
 * cycles, paid once per image install.  Readable beats fast here. */
static void ed_sc_reduce(uint8 out[32], const uint8 in[64])
{
    uint32 r[16];
    uint32 carry, nv;
    int32 i, j;

    for(j = 0; j < 16; j++) { r[j] = 0u; }

    for(i = 511; i >= 0; i--)
    {
        carry = ((uint32)in[i >> 3] >> (uint32)(i & 7)) & 1u;
        for(j = 0; j < 16; j++)
        {
            nv = (r[j] << 1) | carry;
            carry = nv >> 16;
            r[j] = nv & 0xFFFFu;
        }
        if(ed_u16_ge_l(r) != 0)
        {
            ed_u16_sub_l(r);
        }
    }

    for(j = 0; j < 16; j++)
    {
        out[j * 2]      = (uint8)(r[j] & 0xFFu);
        out[j * 2 + 1]  = (uint8)((r[j] >> 8) & 0xFFu);
    }
}

/* 0 if s < L, -1 otherwise.  s is the 32-byte little-endian scalar. */
static int32 ed_sc_check(const uint8 s[32])
{
    uint32 a[16];
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        a[i] = (uint32)s[i * 2u] | ((uint32)s[i * 2u + 1u] << 8);
    }
    return (ed_u16_ge_l(a) != 0) ? -1 : 0;
}

/* ===================================================================
 * Verification
 * =================================================================== */

int32 arkrtos_ed25519_verify(const uint8 sig[ARKRTOS_ED25519_SIG_LEN],
                             const uint8 *msg,
                             uint32 len,
                             const uint8 pub[ARKRTOS_ED25519_PUB_LEN])
{
    ed_ge_p3 A, R, sB, hA, sum, B;
    ed_sha512_t ctx;
    uint8 h[64];
    uint8 hh[32];
    uint8 enc[32];
    uint32 i;

    if((sig == 0) || (pub == 0)) { return -1; }
    if((msg == 0) && (len != 0u)) { return -1; }

    if(ed_ge_frombytes(&A, pub) != 0) { return -1; }
    if(ed_ge_frombytes(&R, sig) != 0) { return -1; }
    if(ed_sc_check(&sig[32]) != 0) { return -1; }

    ed_sha512_init(&ctx);
    ed_sha512_update(&ctx, &sig[0], 32u);
    ed_sha512_update(&ctx, pub, 32u);
    ed_sha512_update(&ctx, msg, len);
    ed_sha512_final(&ctx, h);
    ed_sc_reduce(hh, h);

    ed_ge_base(&B);
    ed_ge_scalarmult(&sB, &sig[32], &B);
    ed_ge_negate(&hA, &A);
    ed_ge_scalarmult(&hA, &hh[0], &hA);
    ed_ge_add(&sum, &sB, &hA);

    ed_ge_tobytes(enc, &sum);
    for(i = 0u; i < 32u; i++)
    {
        if(enc[i] != sig[i]) { return -1; }
    }

    /* Same reason as everywhere else in the kernel: R is decoded but never
     * used, and a compiler must not be able to drop the decode above. */
    (void)R;
    return 0;
}

#endif /* ARKRTOS_USE_ED25519 == 1 */
