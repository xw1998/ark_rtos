/* ============================================================
 * arkvim_shell_cmd.h - Register the "vim" command with the shell
 *
 * arkrtos_shell.c owns the command table.  It calls
 * arkvim_shell_register() once, at the end of its own registration,
 * when ARKRTOS_USE_ARKVIM is on.  Keeping the wiring in this header
 * means the shell never needs to know how the editor is built.
 * ============================================================ */
#ifndef ARKVIM_SHELL_CMD_H
#define ARKVIM_SHELL_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief Install the editor's port and file backend (idempotent).
* @note  Called by arkvim_shell_register(); exposed for hosts that
*        drive arkvim_main() themselves.
*/
void arkvim_shell_init(void);

/**
* @brief Append the "vim" command to g_cmd_table.
* @param idx first free slot in g_cmd_table
* @return the next free slot (idx unchanged when the table is full)
*/
int arkvim_shell_register(int idx);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_SHELL_CMD_H */
