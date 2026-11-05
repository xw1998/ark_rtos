/* arkcc - diagnostic message table (generated)
 *
 * DO NOT EDIT: regenerate with `python tools/gen_diag_table.py`.
 * Source of truth: c_compiler/armcc5/data/diagnostic_messages.csv and driver_diagnostic_codes.csv
 * (armcc 5.06u7 reverse engineering). Only the codes arkc actually emits are
 * carried; see tools/gen_diag_table.py for the list and for the rules that
 * keep unrenderable placeholders out of the table.
 */
#ifndef DIAG_MSG_GEN_H
#define DIAG_MSG_GEN_H

/* armcc frontend text for #<code>, or NULL when arkc does not carry it. */
const char *diag_msg_lookup(int code);

/* Non-zero when armcc prints this code with the "-D" tag whatever the
 * severity (measured; see tools/gen_diag_table.py). Warnings get the tag from
 * src/diag.c without consulting this table. */
int diag_msg_has_d_tag(int code);

/* armcc driver text for e.g. "C3900U", or NULL when not carried. */
const char *diag_driver_lookup(const char *code);

/* Severity letter armcc attaches to a driver code ('U','E','W','I'),
 * or 0 when the code is unknown. */
char diag_driver_severity(const char *code);

#endif /* DIAG_MSG_GEN_H */
