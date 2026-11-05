/* ============================================================
 * arkvim_gap_buffer.h - Gap Buffer Text Storage
 *
 * A gap buffer keeps a single contiguous char array with a "hole"
 * (the gap) at the cursor. Insert/delete at the cursor are O(1);
 * moving the cursor copies characters across the gap, which is
 * acceptable because human editing is strongly local.
 *
 * The buffer owns no heap memory: it operates on a caller-provided
 * backing array, so it is safe for bare-metal MCUs.
 * ============================================================ */
#ifndef ARKVIM_GAP_BUFFER_H
#define ARKVIM_GAP_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    *buf;         /* physical backing array            */
    uint32_t cap;         /* physical capacity                 */
    uint32_t gap_start;   /* logical cursor / gap left edge    */
    uint32_t gap_end;     /* gap right edge (exclusive)        */
    uint32_t len;         /* logical length (excludes gap)     */
} arkvim_gap_buffer_t;

/* Initialise on a caller-owned backing array. */
bool    gb_init(arkvim_gap_buffer_t *gb, char *mem, uint32_t capacity);

/* Logical length and byte at a logical position. */
uint32_t gb_length(const arkvim_gap_buffer_t *gb);
char    gb_get(const arkvim_gap_buffer_t *gb, uint32_t pos);

/* Insert at a logical position. */
bool    gb_insert_char(arkvim_gap_buffer_t *gb, uint32_t pos, char c);
bool    gb_insert_str(arkvim_gap_buffer_t *gb, uint32_t pos,
                      const char *str, uint32_t slen);

/* Delete characters. */
bool    gb_delete_at(arkvim_gap_buffer_t *gb, uint32_t pos);
bool    gb_delete_range(arkvim_gap_buffer_t *gb, uint32_t start, uint32_t end);

/* Cursor (= gap_start). */
uint32_t gb_cursor(const arkvim_gap_buffer_t *gb);
void    gb_set_cursor(arkvim_gap_buffer_t *gb, uint32_t pos);

/* Copy a logical range into dst (NUL-terminated). Returns bytes
 * copied (excluding the NUL). */
uint32_t gb_copy_range(const arkvim_gap_buffer_t *gb, uint32_t start, uint32_t end,
                       char *dst, uint32_t dst_cap);

/* Reset to empty. */
void    gb_clear(arkvim_gap_buffer_t *gb);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_GAP_BUFFER_H */
