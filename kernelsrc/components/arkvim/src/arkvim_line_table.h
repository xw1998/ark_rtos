/* ============================================================
 * arkvim_line_table.h - Line Offset Index
 *
 * The gap buffer stores text as a flat byte stream. To find line N
 * we would otherwise scan for N newlines. The line table caches the
 * start offset of every line so line lookups become O(1) and
 * (offset -> row/col) becomes O(log lines) via binary search.
 *
 * The table is rebuilt lazily (marked dirty on every edit) before
 * the first query that needs it.
 * ============================================================ */
#ifndef ARKVIM_LINE_TABLE_H
#define ARKVIM_LINE_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "arkvim_config.h"
#include "arkvim_gap_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t offsets[ARKVIM_LT_MAX_LINES];  /* start offset of each line */
    uint16_t count;                      /* number of lines           */
    bool     dirty;                      /* needs rebuild             */
} arkvim_line_table_t;

void     lt_init(arkvim_line_table_t *lt);
void     lt_mark_dirty(arkvim_line_table_t *lt);

/* Ensure the table reflects the buffer. Called internally by all
 * queries; cheap when not dirty. */
void     lt_rebuild(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb);

uint16_t lt_line_count(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb);
uint32_t lt_line_offset(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb, uint16_t n);
uint32_t lt_line_length(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb, uint16_t n);

/* offset -> (row, col). Both 0-based. */
void     lt_offset_to_pos(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb,
                          uint32_t offset, uint16_t *row, uint16_t *col);

/* (row, col) -> offset, clamped to the line's end. */
uint32_t lt_pos_to_offset(arkvim_line_table_t *lt, const arkvim_gap_buffer_t *gb,
                          uint16_t row, uint16_t col);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_LINE_TABLE_H */
