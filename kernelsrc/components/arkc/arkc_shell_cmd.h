/* ============================================================
 * arkc_shell_cmd.h - "cc" command for the Ark RTOS shell
 *
 * arkvim proved the pattern: the editor has no idea a shell exists, and
 * the shell gains one command by calling arkvim_shell_register().  The
 * compiler follows the same two functions.
 * ============================================================ */
#ifndef ARKC_SHELL_CMD_H
#define ARKC_SHELL_CMD_H

/* Install the "cc" command at g_cmd_table[idx]; returns the next free
 * index (unchanged when the command table is full). */
int arkc_shell_register(int idx);

#endif /* ARKC_SHELL_CMD_H */
