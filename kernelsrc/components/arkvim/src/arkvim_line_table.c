/* ============================================================
 * arkvim_line_table.c - Line Offset Index Implementation
 * ============================================================ */
#include "arkvim_line_table.h"
#include "arkvim_config.h"

void lt_init(arkvim_line_table_t *lt)
{
    if (lt == NULL) {
        return;
    }
    lt->count = 0;
    lt->dirty = true;
}

void lt_mark_dirty(arkvim_line_table_t *lt)
{
    if (lt != NULL) {
        lt->dirty = true;
    }
}

void lt_rebuild(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb)
{
    uint32_t i;

    if (lt == NULL || gb == NULL || !lt->dirty) {
        return;
    }

    lt->offsets[0] = 0;
    lt->count = 1;

    for (i = 0; i < gb->len && lt->count < ARKVIM_LT_MAX_LINES; i++) {
        if (gb_get(gb, i) == '\n') {
            lt->offsets[lt->count++] = i + 1;
        }
    }
    lt->dirty = false;
}

uint16_t lt_line_count(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb)
{
    if (lt == NULL || gb == NULL) {
        return 0;
    }
    lt_rebuild(lt, gb);
    return lt->count;
}

uint32_t lt_line_offset(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb, uint16_t n)
{
    if (lt == NULL || gb == NULL) {
        return 0;
    }
    lt_rebuild(lt, gb);
    if (n >= lt->count) {
        return gb->len;
    }
    return lt->offsets[n];
}

uint32_t lt_line_length(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb, uint16_t n)
{
    uint32_t start;
    uint32_t end;

    if (lt == NULL || gb == NULL) {
        return 0;
    }
    lt_rebuild(lt, gb);
    if (n >= lt->count) {
        return 0;
    }
    start = lt->offsets[n];
    end   = (n + 1 < lt->count) ? lt->offsets[n + 1] : gb->len;
    return end - start;
}

void lt_offset_to_pos(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb,
                      uint32_t offset, uint16_t *row, uint16_t *col)
{
    uint16_t lo, hi, mid;

    if (lt == NULL || gb == NULL) {
        if (row != NULL) { *row = 0; }
        if (col != NULL) { *col = 0; }
        return;
    }
    lt_rebuild(lt, gb);

    if (offset > gb->len) {
        offset = gb->len;
    }

    /* Binary search for the last line whose offset <= `offset`. */
    lo = 0;
    hi = lt->count;
    while (lo < hi) {
        mid = (uint16_t)((lo + hi) / 2);
        if (lt->offsets[mid] <= offset) {
            lo = (uint16_t)(mid + 1);
        } else {
            hi = mid;
        }
    }
    if (row != NULL) {
        *row = (lo > 0) ? (uint16_t)(lo - 1) : 0;
    }
    if (col != NULL) {
        *col = (uint16_t)(offset - lt->offsets[(lo > 0) ? lo - 1 : 0]);
    }
}

uint32_t lt_pos_to_offset(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb,
                          uint16_t row, uint16_t col)
{
    uint32_t offset;
    uint32_t line_end;

    if (lt == NULL || gb == NULL) {
        return 0;
    }
    lt_rebuild(lt, gb);
    if (row >= lt->count) {
        return gb->len;
    }
    offset   = lt->offsets[row] + col;
    line_end = (row + 1 < lt->count) ? lt->offsets[row + 1] : gb->len;
    if (offset > line_end) {
        offset = line_end;
    }
    return offset;
}
