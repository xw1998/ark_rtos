/*
 * ark_bus_compress.c - LZSS payload codec for ARK_BUS_FLAG_COMPRESSED.
 *
 * Format (deliberately tiny and self-terminating):
 *
 *   A stream is a sequence of groups.  Each group starts with one control
 *   byte; its low 8 bits describe the up to eight tokens that follow, bit 0
 *   first.  A set bit means "one literal byte follows"; a clear bit means
 *   "one 2-byte back-reference follows".  A back-reference packs a 12-bit
 *   distance and a 4-bit length:
 *
 *       byte0 = distance >> 4
 *       byte1 = ((distance & 0x0F) << 4) | (length - 3)
 *
 *   so distance is 1..4096 and match length is 3..18.  A group may be short:
 *   the stream ends as soon as the input is exhausted, and the decoder stops
 *   when it runs out of input mid-group rather than reading past the end.
 *
 * The encoder is a plain greedy matcher over the last 4 KiB seen.  It never
 * emits a stream as large as its input: when the result would not be smaller
 * (no useful match anywhere) it returns 0 and the caller sends the payload
 * uncompressed, flag cleared.  No heap, no libc, no float.
 */
#include "ark_bus_compress.h"

#if ARK_BUS_COMPRESS_ENABLED

#define LZSS_MIN_MATCH  3u
#define LZSS_MAX_MATCH  18u
#define LZSS_WINDOW     4096u

int ark_bus_compress(const uint8_t *in, uint16_t in_len,
                     uint8_t *out, uint16_t out_cap)
{
    uint16_t in_pos  = 0;
    uint16_t out_pos = 0;

    if (!in || !out) {
        return ARK_BUS_E_INVAL;
    }
    if (in_len == 0) {
        return 0;
    }

    while (in_pos < in_len) {
        uint16_t ctrl_pos;
        uint8_t  ctrl = 0;
        uint8_t  bit;

        if (out_pos >= out_cap) {
            return 0;               /* no room even for a control byte */
        }
        ctrl_pos = out_pos++;
        out[ctrl_pos] = 0;          /* patched once the group is complete */

        for (bit = 0; bit < 8u && in_pos < in_len; bit++) {
            uint16_t best_len = 0;
            uint16_t best_dist = 0;
            uint16_t start = (in_pos > LZSS_WINDOW)
                             ? (uint16_t)(in_pos - LZSS_WINDOW) : 0u;
            uint16_t maxlen = (uint16_t)(in_len - in_pos);
            uint16_t j;

            if (maxlen > LZSS_MAX_MATCH) {
                maxlen = LZSS_MAX_MATCH;
            }

            if (maxlen >= LZSS_MIN_MATCH) {
                for (j = start; j < in_pos; j++) {
                    uint16_t l = 0;
                    if (in[j] != in[in_pos]) {
                        continue;
                    }
                    while (l < maxlen && in[j + l] == in[in_pos + l]) {
                        l++;
                    }
                    if (l > best_len) {
                        best_len  = l;
                        best_dist = (uint16_t)(in_pos - j);
                        if (best_len == maxlen) {
                            break;
                        }
                    }
                }
            }

            if (best_len >= LZSS_MIN_MATCH) {
                if ((uint16_t)(out_pos + 2u) > out_cap) {
                    return 0;
                }
                out[out_pos++] =
                    (uint8_t)((best_dist >> 4) & 0xFFu);
                out[out_pos++] = (uint8_t)(((best_dist & 0x0Fu) << 4) |
                                           (best_len - LZSS_MIN_MATCH));
                in_pos = (uint16_t)(in_pos + best_len);
            } else {
                if ((uint16_t)(out_pos + 1u) > out_cap) {
                    return 0;
                }
                ctrl |= (uint8_t)(1u << bit);
                out[out_pos++] = in[in_pos++];
            }
        }
        out[ctrl_pos] = ctrl;
    }

    /* Only report a win when the stream is actually smaller; otherwise the
     * caller sends the payload raw. */
    return (out_pos < in_len) ? (int)out_pos : 0;
}

int ark_bus_decompress(const uint8_t *in, uint16_t in_len,
                       uint8_t *out, uint16_t out_cap)
{
    uint16_t in_pos  = 0;
    uint16_t out_pos = 0;

    if (!in || !out) {
        return ARK_BUS_E_INVAL;
    }

    while (in_pos < in_len) {
        uint8_t ctrl = in[in_pos++];
        uint8_t bit;

        for (bit = 0; bit < 8u; bit++) {
            if (in_pos >= in_len) {
                break;              /* stream ended with the group half used */
            }

            if ((ctrl & (uint8_t)(1u << bit)) != 0u) {
                if (out_pos >= out_cap) {
                    return ARK_BUS_E_FULL;
                }
                out[out_pos++] = in[in_pos++];
            } else {
                uint16_t dist;
                uint16_t len;
                uint16_t src;
                uint16_t k;

                if ((uint16_t)(in_pos + 2u) > in_len) {
                    return ARK_BUS_E_FORMAT;
                }
                dist = (uint16_t)(((uint16_t)in[in_pos] << 4) |
                                  ((uint16_t)in[in_pos + 1] >> 4));
                len  = (uint16_t)((in[in_pos + 1] & 0x0Fu) + LZSS_MIN_MATCH);
                in_pos = (uint16_t)(in_pos + 2u);

                if (dist == 0u || dist > out_pos) {
                    return ARK_BUS_E_FORMAT;   /* points before the output */
                }
                if ((uint16_t)(out_pos + len) > out_cap) {
                    return ARK_BUS_E_FULL;
                }
                /* Copy byte by byte: a match may overlap the bytes it is
                 * producing (distance < length), which is the whole point of
                 * run-length style matches. */
                src = (uint16_t)(out_pos - dist);
                for (k = 0; k < len; k++) {
                    out[out_pos++] = out[src++];
                }
            }
        }
    }

    return (int)out_pos;
}

#endif /* ARK_BUS_COMPRESS_ENABLED */
