/* ============================================================
 * arkvim.c - ArkVim Shell-Agnostic Public API
 *
 * Pure editor entry points with no shell dependency. The host
 * provides a arkvim_port_t (see arkvim_port.h) and a file backend
 * (see arkvim_fileio.h), then calls arkvim_main() or the individual
 * functions below.
 *
 * arkvim_editor_run() lives in arkvim_modes.c; arkvim_fileio_cat() and
 * arkvim_fileio_ls() live in arkvim_fileio.c. This file provides only
 * the glue that has no natural home elsewhere:
 *   - arkvim_register_posix_backend()  (PC file backend install)
 *   - arkvim_main()                    (generic argc/argv dispatch)
 *
 * Shell-specific command registration (e.g. for Ark Shell) lives
 * in ports/arkvim_port_arkshell.c, NOT here.
 * ============================================================ */
#include "arkvim.h"
#include "arkvim_modes.h"
#include "arkvim_fileio.h"
#include "arkvim_terminal.h"
#include "arkvim_port.h"

#include <string.h>

/* ============================================================
 * arkvim_register_posix_backend - install the POSIX file backend
 * ============================================================ */
void arkvim_register_posix_backend(void)
{
    arkvim_fileio_set_backend(&arkvim_fileio_posix_backend);
}

/* ============================================================
 * arkvim_main - generic argc/argv dispatcher
 *
 * Any shell can wire its command handler straight to this:
 *
 *   int my_cmd(int argc, char *argv[]) { return arkvim_main(argc, argv); }
 *
 * Recognised subcommands:
 *   arkvim [file]   open the editor
 *   cat <file>   print a file
 *   ls           list files
 * ============================================================ */
int arkvim_main(int argc, char *argv[])
{
    const char *cmd;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return -1;
    }

    cmd = argv[0];

    if (strcmp(cmd, "arkvim") == 0) {
        const char *filename = (argc >= 2) ? argv[1] : NULL;
        arkvim_editor_run(filename);
        return 0;
    }

    if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            arkvim_port_send_string("\r\nUsage: cat <filename>\r\n");
            return -1;
        }
        arkvim_port_send_string("\r\n");
        if (arkvim_fileio_cat(argv[1]) != 0) {
            arkvim_port_send_string("cat: ");
            arkvim_port_send_string(argv[1]);
            arkvim_port_send_string(": not found\r\n");
        } else {
            arkvim_port_send_string("\r\n");
        }
        return 0;
    }

    if (strcmp(cmd, "ls") == 0) {
        if (arkvim_fileio_ls() != 0) {
            arkvim_port_send_string("\r\nls: no filesystem\r\n");
        }
        return 0;
    }

    return -1;   /* unknown subcommand */
}
