/* ============================================================
 * arkvim_undo.h - Undo / Redo Action Stack
 *
 * Records structural edits (insert/delete of a byte range) so they
 * can be reversed. Each action caches up to ARKVIM_UNDO_TEXT_MAX bytes
 * of the affected text; longer ranges still undo correctly because
 * the inverse operation uses the cached bytes for re-insertion and
 * the recorded length for deletion.
 *
 * Two stacks live inside the editor struct: one for undo, one for
 * redo. Any new edit clears the redo stack.
 * ============================================================ */
#ifndef ARKVIM_UNDO_H
#define ARKVIM_UNDO_H

#include <stdint.h>
#include <stdbool.h>
#include "arkvim_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARKVIM_ACT_INSERT,
    ARKVIM_ACT_DELETE
} arkvim_action_type_t;

typedef struct {
    arkvim_action_type_t type;
    uint32_t pos;                         /* logical offset of the edit */
    uint16_t len;                         /* affected length            */
    char     text[ARKVIM_UNDO_TEXT_MAX];     /* captured bytes (head)      */
} arkvim_edit_action_t;

typedef struct {
    arkvim_edit_action_t actions[ARKVIM_UNDO_MAX_DEPTH];
    int top;                              /* count of valid entries     */
} arkvim_action_stack_t;

struct arkvim_editor_s;   /* forward decl from arkvim_modes.h */

void undo_init(arkvim_action_stack_t *undo, arkvim_action_stack_t *redo);

/* Record an insertion that has just been applied at [pos, pos+len).
 * Captures the inserted bytes from the current buffer. */
void undo_push_insert(struct arkvim_editor_s *e, uint32_t pos, uint16_t len);

/* Record a deletion about to be applied at [pos, pos+len).
 * Captures the soon-to-be-deleted bytes from the current buffer. */
void undo_push_delete(struct arkvim_editor_s *e, uint32_t pos, uint16_t len);

bool undo_execute(struct arkvim_editor_s *e);   /* returns false if empty */
bool redo_execute(struct arkvim_editor_s *e);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_UNDO_H */
