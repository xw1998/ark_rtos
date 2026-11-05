/* ============================================================
 * arkvim_undo.c - Undo / Redo Implementation
 * ============================================================ */
#include "arkvim_undo.h"
#include "arkvim_modes.h"      /* full editor type + buffer access */
#include "arkvim_gap_buffer.h"
#include "arkvim_line_table.h"
#include "arkvim_renderer.h"

#include <string.h>

void undo_init(arkvim_action_stack_t *undo, arkvim_action_stack_t *redo)
{
    if (undo != NULL) {
        undo->top = 0;
    }
    if (redo != NULL) {
        redo->top = 0;
    }
}

static void stack_push(arkvim_action_stack_t *s, const arkvim_edit_action_t *act)
{
    if (s->top >= ARKVIM_UNDO_MAX_DEPTH) {
        /* Drop the oldest entry to make room. */
        memmove(&s->actions[0], &s->actions[1],
                (ARKVIM_UNDO_MAX_DEPTH - 1) * sizeof(arkvim_edit_action_t));
        s->top = ARKVIM_UNDO_MAX_DEPTH - 1;
    }
    s->actions[s->top++] = *act;
}

static bool stack_pop(arkvim_action_stack_t *s, arkvim_edit_action_t *act)
{
    if (s->top == 0) {
        return false;
    }
    s->top--;
    *act = s->actions[s->top];
    return true;
}

static void clear_redo(arkvim_editor_t *e)
{
    e->redo.top = 0;
}

void undo_push_insert(arkvim_editor_t *e, uint32_t pos, uint16_t len)
{
    arkvim_edit_action_t act;
    uint16_t cap = len;

    act.type = ARKVIM_ACT_INSERT;
    act.pos  = pos;
    act.len  = len;
    if (cap > ARKVIM_UNDO_TEXT_MAX) {
        cap = ARKVIM_UNDO_TEXT_MAX;
    }
    gb_copy_range(&e->gb, pos, pos + cap, act.text, ARKVIM_UNDO_TEXT_MAX);

    stack_push(&e->undo, &act);
    clear_redo(e);
}

void undo_push_delete(arkvim_editor_t *e, uint32_t pos, uint16_t len)
{
    arkvim_edit_action_t act;
    uint16_t cap = len;

    act.type = ARKVIM_ACT_DELETE;
    act.pos  = pos;
    act.len  = len;
    if (cap > ARKVIM_UNDO_TEXT_MAX) {
        cap = ARKVIM_UNDO_TEXT_MAX;
    }
    gb_copy_range(&e->gb, pos, pos + cap, act.text, ARKVIM_UNDO_TEXT_MAX);

    stack_push(&e->undo, &act);
    clear_redo(e);
}

bool undo_execute(arkvim_editor_t *e)
{
    arkvim_edit_action_t act;
    arkvim_edit_action_t inverse;

    if (!stack_pop(&e->undo, &act)) {
        return false;
    }

    inverse.type = (act.type == ARKVIM_ACT_INSERT) ? ARKVIM_ACT_DELETE : ARKVIM_ACT_INSERT;
    inverse.pos  = act.pos;
    inverse.len  = act.len;

    if (act.type == ARKVIM_ACT_INSERT) {
        /* Reverse an insert: delete the inserted range. */
        gb_delete_range(&e->gb, act.pos, act.pos + act.len);
        e->cursor = act.pos;
        /* inverse captures nothing useful (deletion), reuse act.text. */
        memcpy(inverse.text, act.text, ARKVIM_UNDO_TEXT_MAX);
    } else {
        /* Reverse a delete: re-insert the captured bytes. */
        gb_insert_str(&e->gb, act.pos, act.text, act.len);
        e->cursor = act.pos + act.len;
        memcpy(inverse.text, act.text, ARKVIM_UNDO_TEXT_MAX);
    }

    stack_push(&e->redo, &inverse);
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
    return true;
}

bool redo_execute(arkvim_editor_t *e)
{
    arkvim_edit_action_t act;
    arkvim_edit_action_t inverse;

    if (!stack_pop(&e->redo, &act)) {
        return false;
    }

    inverse.type = (act.type == ARKVIM_ACT_INSERT) ? ARKVIM_ACT_DELETE : ARKVIM_ACT_INSERT;
    inverse.pos  = act.pos;
    inverse.len  = act.len;
    memcpy(inverse.text, act.text, ARKVIM_UNDO_TEXT_MAX);

    if (act.type == ARKVIM_ACT_INSERT) {
        /* Redo an insert: re-insert the bytes. */
        gb_insert_str(&e->gb, act.pos, act.text, act.len);
        e->cursor = act.pos + act.len;
    } else {
        /* Redo a delete: delete the range. */
        gb_delete_range(&e->gb, act.pos, act.pos + act.len);
        e->cursor = act.pos;
    }

    stack_push(&e->undo, &inverse);
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
    return true;
}
