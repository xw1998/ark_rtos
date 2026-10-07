/*
 * ark_bus_compress.h - optional payload compression (ARK_BUS_FLAG_COMPRESSED).
 *
 * The wire header has always had a COMPRESSED flag, but nothing set or acted
 * on it: the byte was carried and ignored.  This file gives it meaning with a
 * small LZSS codec that needs no heap, no libc and no floating point, so it is
 * as portable as the rest of the core.
 *
 * When ARK_BUS_COMPRESS_ENABLED is 0 (the default) this header declares
 * nothing and the codec file compiles away; a frame that happens to carry the
 * flag is handled exactly as before.  When it is 1, ark_bus_emit() compresses
 * a payload whose flag is set and ark_bus_feed() inflates it again before
 * dispatch, so applications and callback code never see the compressed bytes.
 *
 * Honesty rules the codec follows:
 *   - compression is best-effort: if the result would not be smaller, the
 *     encoder says so (returns 0) and the caller must send the bytes as they
 *     are, with the flag cleared -- never a "compressed" frame that is bigger;
 *   - decompression is bounded by the caller's buffer: a truncated or hostile
 *     stream is rejected with an error, never expanded past out_cap and never
 *     delivered as if it were valid data.
 *
 * Both ends of a link must enable the feature; a peer that leaves it off reads
 * the flag as it always did.
 */
#ifndef ARK_BUS_COMPRESS_H
#define ARK_BUS_COMPRESS_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

#if ARK_BUS_COMPRESS_ENABLED

/*
 * Compress `in_len` bytes into `out`.  Returns the compressed size (>0) on
 * success, 0 when the data would not shrink (send it raw instead), or a
 * negative ARK_BUS_E_* code on bad arguments.
 */
int ark_bus_compress(const uint8_t *in, uint16_t in_len,
                     uint8_t *out, uint16_t out_cap);

/*
 * Inflate a stream produced by ark_bus_compress() into `out`.  Returns the
 * number of bytes written, or a negative ARK_BUS_E_* code: E_FORMAT for a
 * malformed/truncated stream, E_FULL when the result would exceed `out_cap`.
 * Nothing is ever written past `out_cap`.
 */
int ark_bus_decompress(const uint8_t *in, uint16_t in_len,
                       uint8_t *out, uint16_t out_cap);

#endif /* ARK_BUS_COMPRESS_ENABLED */

#endif /* ARK_BUS_COMPRESS_H */
