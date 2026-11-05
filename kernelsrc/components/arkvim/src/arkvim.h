/* ============================================================
 * arkvim.h - ArkVim Public API (Shell-Agnostic)
 *
 * ArkVim is a modal text editor designed to run inside any shell
 * or as a standalone program. It does NOT depend on a specific
 * shell. The host provides a arkvim_port_t (see arkvim_port.h) and then
 * calls the functions below.
 *
 * Typical integration:
 *   1. host fills in a static const arkvim_port_t (send/recv/tick)
 *   2. host calls arkvim_port_init(&port)
 *   3. host calls arkvim_fileio_set_backend(...)  (POSIX or MCU FS)
 *   4. host's own command handler calls arkvim_main(argc, argv)
 *      or arkvim_editor_run(filename)
 *
 * A ready-made Ark Shell adapter (arkvim_register_commands +
 * arkvim_register_posix_backend) lives in ports/arkvim_port_arkshell.c.
 * ============================================================ */
#ifndef ARKVIM_H
#define ARKVIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Port / backend setup (host calls these once at boot) ---- */

/* arkvim_port_init() is declared in arkvim_port.h. */

/* Register the POSIX file backend so the editor can read/write real
 * files. PC builds only. On MCU, register your own backend via
 * arkvim_fileio_set_backend() instead. */
void arkvim_register_posix_backend(void);

/* ---- Editor entry points (shell-agnostic) ---- */

/* Open the editor on a file. If filename is NULL, open a scratch
 * buffer. Blocks until the user quits (:q / :wq / :q!). */
void arkvim_editor_run(const char *filename);

/* Print a file to the terminal via the installed port. Returns 0 on
 * success, non-zero if the file cannot be opened. */
int  arkvim_fileio_cat(const char *filename);

/* List files in the current directory via the installed backend.
 * Returns 0 on success, non-zero if no filesystem is registered. */
int  arkvim_fileio_ls(void);

/* Generic argc/argv entry: dispatches "arkvim", "cat", "ls" subcommands.
 * Any shell can wire this directly to its own command handler:
 *
 *   int my_cmd_handler(int argc, char *argv[]) {
 *       return arkvim_main(argc, argv);
 *   }
 *
 * Returns 0 on success, non-zero on error. argc==0 or argv[0] not
 * recognised returns -1.
 *
 * Recognised forms:
 *   arkvim_main(2, {"arkvim","file.txt"})   -> open editor on file.txt
 *   arkvim_main(1, {"arkvim"})              -> open editor, scratch buffer
 *   arkvim_main(2, {"cat","file.txt"})   -> cat the file
 *   arkvim_main(1, {"ls"})               -> list directory
 */
int  arkvim_main(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_H */
