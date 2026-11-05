/* ============================================================
 * arkvim_shell_cmd.c - The "vim" shell command
 *
 * One command, four things to get right:
 *
 *   1) size gate.  The editor holds the file in a fixed buffer and
 *      `:w` writes that buffer back as the whole file.  A file that
 *      does not fit is refused here, before the editor is entered:
 *      opening it would end with the disk file replaced by its head.
 *   2) path gate.  The path is resolved and stat'ed first, so
 *      "directory", "not readable" and "no such file" are three
 *      different answers instead of one [New File].
 *   3) one-time init.  Port and backend are installed on first use.
 *   4) the editor runs in the shell task.  When it returns, the shell
 *      redraws its prompt and keeps reading the console.
 * ============================================================ */
#include "arkvim.h"
#include "arkvim_fileio.h"
#include "arkvim_fileio_vfs.h"
#include "arkvim_shell_cmd.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"

#include "platform.h"

#include <stdint.h>

void arkvim_port_init_arkrtos(void);

static uint8_t s_inited;

void arkvim_shell_init(void)
{
    if (s_inited != 0u) {
        return;
    }
    arkvim_port_init_arkrtos();
    arkvim_fileio_set_backend(&arkvim_fileio_vfs_backend);
    s_inited = 1u;
}

static int cmd_vim(int argc, char *argv[])
{
    const char *name;
    uint32_t    size = 0u;
    int         rc;

    if (argc < 2) {
        platform_uart_send_string("vim: usage: vim <file>\r\n");
        return 1;
    }
    name = argv[1];

    rc = arkvim_vfs_probe(name, &size);
    switch (rc) {
    case ARKVIM_VFS_OK:
        break;
    case ARKVIM_VFS_ETOOLONG:
        platform_uart_send_string("vim: path too long\r\n");
        return 1;
    case ARKVIM_VFS_ETOOBIG:
        ark_shell_printf("vim: %s is %u B, editor buffer is %u B: "
                         "refusing to open it truncated\r\n",
                         name, (unsigned)size, (unsigned)ARKVIM_GB_MAX_SIZE);
        return 1;
    case ARKVIM_VFS_EISDIR:
        ark_shell_printf("vim: %s is a directory\r\n", name);
        return 1;
    default:
        ark_shell_printf("vim: cannot stat %s (file system error)\r\n", name);
        return 1;
    }

    arkvim_shell_init();
    arkvim_editor_run(name);
    return 0;
}

int arkvim_shell_register(int idx)
{
    if ((idx < 0) || ((idx + 1) > ARK_SHELL_MAX_COMMANDS)) {
        platform_uart_send_string("shell: no room for the vim command\r\n");
        return idx;
    }
    g_cmd_table[idx++] = ARK_SHELL_CMD("vim", cmd_vim,
        "Modal text editor: vim <file>", 2);
    return idx;
}
