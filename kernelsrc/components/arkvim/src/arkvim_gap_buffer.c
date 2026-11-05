/* ============================================================
 * arkvim_gap_buffer.c - Gap Buffer Implementation
 * ============================================================ */
#include "arkvim_gap_buffer.h"

#include <string.h>

static inline uint32_t gb_gap_size(const arkvim_gap_buffer_t *gb)
{
    return gb->gap_end - gb->gap_start;
}

/* Logical index -> physical index. */
static inline uint32_t gb_physical(const arkvim_gap_buffer_t *gb, uint32_t logical)
{
    return (logical < gb->gap_start) ? logical : logical + gb_gap_size(gb);
}

/* Move the gap so its left edge sits at `target` (a logical offset). */
static void gb_move_gap(arkvim_gap_buffer_t *gb, uint32_t target)
{
    if (target == gb->gap_start) {
        return;
    }
    if (target > gb->gap_start) {
        uint32_t move = target - gb->gap_start;
        memmove(&gb->buf[gb->gap_start], &gb->buf[gb->gap_end], move);
        gb->gap_start += move;
        gb->gap_end   += move;
    } else {
        uint32_t move = gb->gap_start - target;
        memmove(&gb->buf[gb->gap_end - move], &gb->buf[target], move);
        gb->gap_start -= move;
        gb->gap_end   -= move;
    }
}

bool gb_init(arkvim_gap_buffer_t *gb, char *mem, uint32_t capacity)
{
    if (gb == NULL || mem == NULL || capacity == 0) {
        return false;
    }
    gb->buf       = mem;
    gb->cap       = capacity;
    gb->gap_start = 0;
    gb->gap_end   = capacity;   /* whole array is gap initially */
    gb->len       = 0;
    return true;
}

uint32_t gb_length(const arkvim_gap_buffer_t *gb)
{
    return gb->len;
}

char gb_get(const arkvim_gap_buffer_t *gb, uint32_t pos)
{
    if (pos >= gb->len) {
        return '\0';
    }
    return gb->buf[gb_physical(gb, pos)];
}

bool gb_insert_char(arkvim_gap_buffer_t *gb, uint32_t pos, char c)
{
    if (gb == NULL || pos > gb->len) {
        return false;
    }
    if (gb_gap_size(gb) == 0) {
        return false;   /* backing array is full */
    }
    gb_move_gap(gb, pos);
    gb->buf[gb->gap_start++] = c;
    gb->len++;
    return true;
}

bool gb_insert_str(arkvim_gap_buffer_t *gb, uint32_t pos,
                   const char *str, uint32_t slen)
{
    uint32_t avail;
    if (gb == NULL || str == NULL || pos > gb->len) {
        return false;
    }
    if (slen == 0) {
        return true;
    }
    avail = gb_gap_size(gb);
    if (slen > avail) {
        slen = avail;          /* insert only what fits */
    }
    if (slen == 0) {
        return false;
    }
    gb_move_gap(gb, pos);
    memmove(&gb->buf[gb->gap_start], str, slen);
    gb->gap_start += slen;
    gb->len       += slen;
    return true;
}

bool gb_delete_at(arkvim_gap_buffer_t *gb, uint32_t pos)
{
    if (gb == NULL || pos >= gb->len) {
        return false;
    }
    gb_move_gap(gb, pos);
    gb->gap_end++;   /* absorb the char to the right of the gap */
    gb->len--;
    return true;
}

bool gb_delete_range(arkvim_gap_buffer_t *gb, uint32_t start, uint32_t end)
{
    if (gb == NULL || start >= end || end > gb->len) {
        return false;
    }
    /* Move the gap to `end`, then retract its left edge to `start`,
     * effectively dropping [start, end) into the gap. */
    gb_move_gap(gb, end);
    gb->gap_start = start;
    gb->len      -= (end - start);
    return true;
}

uint32_t gb_cursor(const arkvim_gap_buffer_t *gb)
{
    return gb->gap_start;
}

void gb_set_cursor(arkvim_gap_buffer_t *gb, uint32_t pos)
{
    if (pos > gb->len) {
        pos = gb->len;
    }
    gb_move_gap(gb, pos);
}

uint32_t gb_copy_range(const arkvim_gap_buffer_t *gb, uint32_t start, uint32_t end,
                       char *dst, uint32_t dst_cap)
{
    uint32_t total;
    uint32_t phys;

    if (gb == NULL || dst == NULL || dst_cap == 0 || start >= end || start >= gb->len) {
        if (dst != NULL && dst_cap > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    if (end > gb->len) {
        end = gb->len;
    }
    total = end - start;
    if (total > dst_cap - 1) {
        total = dst_cap - 1;
    }

    phys = gb_physical(gb, start);

    if (end <= gb->gap_start) {
        /* entirely left of the gap */
        memmove(dst, &gb->buf[phys], total);
    } else if (start >= gb->gap_start) {
        /* entirely right of the gap */
        memmove(dst, &gb->buf[phys], total);
    } else {
        /* straddles the gap: left part then right part */
        uint32_t left  = gb->gap_start - start;
        uint32_t right = total - left;
        memmove(dst, &gb->buf[phys], left);
        memmove(dst + left, &gb->buf[gb->gap_end], right);
    }

    dst[total] = '\0';
    return total;
}

void gb_clear(arkvim_gap_buffer_t *gb)
{
    if (gb == NULL) {
        return;
    }
    gb->gap_start = 0;
    gb->gap_end   = gb->cap;
    gb->len       = 0;
}
