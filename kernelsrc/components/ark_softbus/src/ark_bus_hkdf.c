/*
 * ark_bus_hkdf.c - HKDF-SHA256 (RFC 5869) over the HMAC the crypto layer has.
 *
 * Nothing allocates: expand() streams one HMAC at a time straight into the
 * caller's output, using the three-segment HMAC helper (T(i-1) | info | i)
 * rather than copying the segments into a scratch buffer.  See ark_bus_hkdf.h.
 */
#include "ark_bus_hkdf.h"

#if ARK_BUS_HANDSHAKE_ENABLED

#include "ark_bus_crypto.h"
#include "ark_bus_priv.h"

void ark_bus_hkdf_extract(const uint8_t *salt, uint16_t salt_len,
                          const uint8_t *ikm, uint32_t ikm_len,
                          uint8_t prk[32])
{
    static const uint8_t zero[32] = {0};

    /* RFC 5869: a missing salt is HashLen zero bytes, not "no salt". */
    if (!salt || salt_len == 0u) {
        ark_bus_hmac_sha256(zero, 32u, ikm, ikm_len, prk);
    } else {
        ark_bus_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

void ark_bus_hkdf_expand(const uint8_t prk[32],
                         const uint8_t *info, uint32_t info_len,
                         uint8_t *out, uint32_t out_len)
{
    uint8_t  t[32];
    uint32_t done = 0u;
    uint32_t n    = 0u;   /* bytes in t that are valid (0 before the 1st block) */
    uint8_t  counter = 1u;

    if (!out || out_len == 0u) {
        return;
    }
    /* The RFC caps the output at 255 * HashLen; clamp rather than overflow. */
    if (out_len > 255u * 32u) {
        out_len = 255u * 32u;
    }

    while (done < out_len) {
        uint32_t take;

        /* T(i) = HMAC(prk, T(i-1) | info | i); T(0) is empty, so the first
         * block hashes only info and the counter. */
        ark_hmac_sha256_3(prk, 32u, t, n, info, info_len, &counter, 1u, t);
        n = 32u;

        take = out_len - done;
        if (take > 32u) {
            take = 32u;
        }
        ark_memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    ark_memset(t, 0, sizeof(t));
}

#endif /* ARK_BUS_HANDSHAKE_ENABLED */
