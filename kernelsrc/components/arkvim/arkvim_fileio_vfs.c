/* ============================================================
 * arkvim_fileio_vfs.c - Ark RTOS VFS Backend for ArkVim
 *
 * Four backend entry points (load / save / cat / ls) implemented on
 * top of the kernel VFS facade (arkrtos_vfs.h).  Paths typed into the
 * editor are resolved by the shell itself (arkrtos_shell_resolve_path),
 * so "." / ".." / "~" mean what they mean at the shell prompt and the
 * editor never grows a second, slightly different path syntax.
 *
 * Two deliberate refusals live here:
 *   1) load refuses a file that does not fit the editor buffer instead
 *      of loading a truncated head;
 *   2) once a load of an existing file has failed, save refuses until
 *      a load succeeds.  `:w` writes the whole buffer back, so writing
 *      after a half-finished load would replace a whole file with an
 *      empty or partial one.  An error message is always cheaper than
 *      the file.
 * ============================================================ */
#include "arkvim_fileio.h"
#include "arkvim_fileio_vfs.h"
#include "arkvim_modes.h"
#include "arkvim_gap_buffer.h"
#include "arkvim_terminal.h"

#include "arkrtos_shell.h"
#include "arkrtos_vfs.h"

#include <string.h>

/* Read chunk for streaming load/cat.  Small enough for the shell task
 * stack, large enough that a 16 KB file is not 64 calls of overhead. */
#define VFS_IO_CHUNK      256u

/* ark_vfs.h: ARK_E_NOENT.  Kept as a local name because this file must
 * not take a dependency on the upstream header. */
#define VFS_E_NOENT       (-1)

/* arkrtos_vfs_write_file() takes the whole file in one buffer, so the
 * editor content has to be materialised first.  One extra byte leaves
 * room for the terminator gb_copy_range() always writes. */
static char s_save_scratch[ARKVIM_GB_MAX_SIZE + 1u];

/* Set when a load of an *existing* file failed part way.  Cleared by a
 * successful load and by a load that found no file at all (a new file
 * has nothing to lose).  While set, save refuses to write. */
static uint8_t s_load_failed;

/* ------------------------------------------------------------
 * Path resolution
 * ------------------------------------------------------------ */
static int vfs_resolve(const char *in, char *out, uint32_t cap)
{
    if ((in == NULL) || (in[0] == '\0') || (out == NULL) || (cap == 0u)) {
        return -1;
    }
    if (arkrtos_shell_resolve_path(in, out, cap) != 0) {
        return -1;
    }
    return (out[0] == '/') ? 0 : -1;
}

/* ------------------------------------------------------------
 * Probe (called by the command layer BEFORE entering the editor)
 * ------------------------------------------------------------ */
int arkvim_vfs_probe(const char *filename, uint32_t *size)
{
    char     path[ARKRTOS_VFS_PATH_MAX];
    uint32_t fsize = 0;
    uint32_t fmode = 0;
    int32_t  rc;

    if (size != NULL) { *size = 0u; }
    if (vfs_resolve(filename, path, sizeof(path)) != 0) {
        return ARKVIM_VFS_ETOOLONG;
    }

    rc = arkrtos_vfs_app_stat(path, &fsize, &fmode);
    if (rc == VFS_E_NOENT) {
        return ARKVIM_VFS_OK;            /* new file, editor creates it */
    }
    if (rc != 0) {
        return ARKVIM_VFS_EIO;
    }
    if ((fmode & ARKRTOS_VFS_S_IFDIR) != 0u) {
        return ARKVIM_VFS_EISDIR;
    }
    if ((fmode & ARKRTOS_VFS_S_IFREG) == 0u) {
        return ARKVIM_VFS_EIO;           /* a device node is not a text file */
    }
    /* Report the real size before any refusal: the caller prints it in
     * the "refusing to open" message, and returning early used to leave
     * the caller's size at 0, which is a wrong answer in a message that
     * is supposed to explain why the file cannot be opened. */
    if (size != NULL) { *size = fsize; }
    /* The gap buffer fills to at most ARKVIM_GB_MAX_SIZE bytes and the
     * line table keeps at most ARKVIM_LT_MAX_LINES lines: a file that
     * exceeds either would come back silently shortened. */
    if (fsize >= (uint32_t)ARKVIM_GB_MAX_SIZE) {
        return ARKVIM_VFS_ETOOBIG;
    }
    return ARKVIM_VFS_OK;
}

/* ------------------------------------------------------------
 * load
 * ------------------------------------------------------------ */
static int vfs_load(const char *filename, arkvim_editor_t *e)
{
    char    path[ARKRTOS_VFS_PATH_MAX];
    uint8_t chunk[VFS_IO_CHUNK];
    int32_t fd;
    int32_t n;
    int     rc;

    /* The command layer probes first; probing again keeps the backend
     * correct for any future caller that forgets to. */
    rc = arkvim_vfs_probe(filename, NULL);
    if (rc != ARKVIM_VFS_OK) {
        return -1;
    }
    if (vfs_resolve(filename, path, sizeof(path)) != 0) {
        return -1;
    }

    s_load_failed = 0u;
    fd = arkrtos_vfs_open_read(path);
    if (fd < 0) {
        if (fd == VFS_E_NOENT) {
            gb_clear(&e->gb);
            return -1;                   /* new file: [New File] is correct */
        }
        s_load_failed = 1u;
        return -1;
    }

    gb_clear(&e->gb);
    for (;;) {
        n = arkrtos_vfs_read(fd, chunk, (uint32_t)sizeof(chunk));
        if (n < 0) {
            (void)arkrtos_vfs_close(fd);
            s_load_failed = 1u;
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (!gb_insert_str(&e->gb, gb_length(&e->gb),
                           (const char *)chunk, (uint32_t)n)) {
            /* Buffer or line table is full.  The file on disk is longer
             * than what we hold, so saving must stay forbidden. */
            (void)arkrtos_vfs_close(fd);
            s_load_failed = 1u;
            return -1;
        }
    }
    (void)arkrtos_vfs_close(fd);
    return 0;
}

/* ------------------------------------------------------------
 * save
 * ------------------------------------------------------------ */
static int vfs_save(const char *filename, arkvim_editor_t *e)
{
    char     path[ARKRTOS_VFS_PATH_MAX];
    uint32_t total = gb_length(&e->gb);
    uint32_t got;

    if (s_load_failed != 0u) {
        /* The buffer does not hold the whole file; writing it back would
         * destroy the rest.  Report, do not write. */
        return -1;
    }
    if (vfs_resolve(filename, path, sizeof(path)) != 0) {
        return -1;
    }
    if (total > (uint32_t)ARKVIM_GB_MAX_SIZE) {
        return -1;
    }
    got = gb_copy_range(&e->gb, 0u, total, s_save_scratch,
                        (uint32_t)sizeof(s_save_scratch));
    if (got != total) {
        return -1;
    }
    if (arkrtos_vfs_write_file(path, (const uint8 *)s_save_scratch, total) != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------
 * cat
 * ------------------------------------------------------------ */
static int vfs_cat(const char *filename)
{
    char    path[ARKRTOS_VFS_PATH_MAX];
    uint8_t chunk[VFS_IO_CHUNK];
    int32_t fd;
    int32_t n;

    if (vfs_resolve(filename, path, sizeof(path)) != 0) {
        return -1;
    }
    fd = arkrtos_vfs_open_read(path);
    if (fd < 0) {
        return -1;
    }
    for (;;) {
        n = arkrtos_vfs_read(fd, chunk, (uint32_t)sizeof(chunk));
        if (n < 0) {
            (void)arkrtos_vfs_close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        arkvim_term_write((const char *)chunk, (uint32_t)n);
    }
    (void)arkrtos_vfs_close(fd);
    return 0;
}

/* ------------------------------------------------------------
 * ls
 * ------------------------------------------------------------ */
typedef struct {
    uint32_t count;
} vfs_ls_ctx_t;

static int vfs_ls_cb(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    vfs_ls_ctx_t *ctx = (vfs_ls_ctx_t *)arg;

    arkvim_term_puts("  ");
    arkvim_term_puts(name);
    if ((st != NULL) && ((st->mode & ARKRTOS_VFS_S_IFDIR) != 0u)) {
        arkvim_term_puts("/");
    }
    arkvim_term_puts("\r\n");
    ctx->count++;
    return 0;
}

static int vfs_ls(void)
{
    char         path[ARKRTOS_VFS_PATH_MAX];
    vfs_ls_ctx_t ctx;

    if (vfs_resolve(".", path, sizeof(path)) != 0) {
        return -1;
    }
    ctx.count = 0u;
    arkvim_term_puts("\r\n");
    if (arkrtos_vfs_list(path, vfs_ls_cb, &ctx) != 0) {
        return -1;
    }
    if (ctx.count == 0u) {
        arkvim_term_puts("  (empty)\r\n");
    }
    return 0;
}

const arkvim_fileio_backend_t arkvim_fileio_vfs_backend = {
    vfs_load, vfs_save, vfs_cat, vfs_ls
};
