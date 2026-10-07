/*
 * ark_bus_x25519.c - X25519 (RFC 7748) scalar multiplication.
 *
 * A faithful, allocation-free rendering of the curve25519 ladder: 16 limbs of
 * 16 bits in signed 64-bit words.  This is the well-known reference shape
 * (Tanja Lange / Daniel J. Bernstein's TweetNaCl is the canonical form and is
 * public domain); it is checked here against the RFC 7748 section 5.2 vectors.
 *
 * No tables, no heap, no libc.  The whole file is gated, so a build that does
 * not enable the handshake links none of it.
 */
#include "ark_bus_x25519.h"

#if ARK_BUS_HANDSHAKE_ENABLED

typedef long long i64;
typedef i64       gf[16];

#define FOR_(i, n) for (i = 0; i < (n); ++i)

static const gf k_121665 = { 0xDB41, 1 };

/* 1 when the 32-byte string is all zero (the low-order-point check).
 * `d | (0 - d)` has the top bit set exactly when d is non-zero, so the result
 * is that bit inverted and masked: 1 only when every byte was 0. */
static int is_zero32(const uint8_t *a)
{
    uint32_t d = 0u;
    int      i;
    for (i = 0; i < 32; i++) {
        d |= (uint32_t)a[i];
    }
    return (int)(~((d | (0u - d)) >> 31) & 1u);
}

static void car25519(gf o)
{
    int i;
    i64 c;
    FOR_(i, 16) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    i64 t, i, c = ~(b - 1);
    FOR_(i, 16) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    int i, j, b;
    gf  m, t;
    FOR_(i, 16) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    FOR_(j, 2) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    FOR_(i, 16) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    int i;
    FOR_(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A_(gf o, const gf a, const gf b)
{
    int i;
    FOR_(i, 16) o[i] = a[i] + b[i];
}

static void Z_(gf o, const gf a, const gf b)
{
    int i;
    FOR_(i, 16) o[i] = a[i] - b[i];
}

static void M_(gf o, const gf a, const gf b)
{
    i64 i, j, t[31];
    FOR_(i, 31) t[i] = 0;
    FOR_(i, 16) FOR_(j, 16) t[i + j] += a[i] * b[j];
    FOR_(i, 15) t[i] += 38 * t[i + 16];
    FOR_(i, 16) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S_(gf o, const gf a)
{
    M_(o, a, a);
}

static void inv25519(gf o, const gf i)
{
    gf  c;
    int a;
    FOR_(a, 16) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S_(c, c);
        if (a != 2 && a != 4) {
            M_(c, c, i);
        }
    }
    FOR_(a, 16) o[a] = c[a];
}

void ark_bus_x25519_clamp(uint8_t n[32])
{
    n[0]  &= 248u;
    n[31] &= 127u;
    n[31] |= 64u;
}

int ark_bus_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32])
{
    uint8_t z[32];
    i64     x[80];
    i64     r, i;
    gf      a, b, c, d, e, f;

    FOR_(i, 31) z[i] = n[i];
    z[31] = (uint8_t)((n[31] & 127u) | 64u);
    z[0] &= 248u;

    unpack25519(x, p);
    FOR_(i, 16) {
        b[i] = x[i];
        d[i] = 0;
        a[i] = 0;
        c[i] = 0;
    }
    a[0] = 1;
    d[0] = 1;

    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
        A_(e, a, c);
        Z_(a, a, c);
        A_(c, b, d);
        Z_(b, b, d);
        S_(d, e);
        S_(f, a);
        M_(a, c, a);
        M_(c, b, e);
        A_(e, a, c);
        Z_(a, a, c);
        S_(b, a);
        Z_(c, d, f);
        M_(a, c, k_121665);
        A_(a, a, d);
        M_(c, c, a);
        M_(a, d, f);
        M_(d, b, x);
        S_(b, e);
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
    }

    FOR_(i, 16) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M_(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);

    /* RFC 7748 (section 6.1): reject an all-zero shared secret, which a peer
     * can force by sending a low-order point; it would make the session key
     * public. */
    return is_zero32(q) ? -1 : 0;
}

void ark_bus_x25519_base(uint8_t q[32], const uint8_t n[32])
{
    /* The base point is u = 9. */
    uint8_t base[32];
    int     i;
    for (i = 0; i < 32; i++) {
        base[i] = 0;
    }
    base[0] = 9;
    (void)ark_bus_x25519(q, n, base);
}

#endif /* ARK_BUS_HANDSHAKE_ENABLED */
