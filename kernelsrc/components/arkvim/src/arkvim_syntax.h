/* ============================================================
 * arkvim_syntax.h - Minimal C Syntax Highlighter
 *
 * Tags each byte of a source line with an ARKVIM_ATTR_* value. A
 * single boolean carries block-comment state across lines so that
 * block-comment spans are coloured correctly.
 * ============================================================ */
#ifndef ARKVIM_SYNTAX_H
#define ARKVIM_SYNTAX_H

#include <stdint.h>
#include <stdbool.h>
#include "arkvim_renderer.h"   /* ARKVIM_ATTR_* */

#ifdef __cplusplus
extern "C" {
#endif

/* Highlight one logical line (no trailing newline).
 *  line       - input bytes
 *  len        - number of bytes
 *  out_attr   - output attribute for each byte (len entries)
 *  in_block   - in/out block-comment state carried across lines */
void syntax_highlight_line(const char *line, int len,
                           uint8_t *out_attr, bool *in_block);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_SYNTAX_H */
