/* ============================================================
 * arkvim_fileio_vfs.h - Ark RTOS VFS Backend for ArkVim
 *
 * The editor itself never touches a filesystem: it calls the four
 * function pointers of arkvim_fileio_backend_t.  This backend fills
 * them with Ark RTOS VFS calls, so `vim /tmp/demo.c` edits a real
 * file in the kernel namespace.
 *
 * Why the extra probe function
 * ---------------------------
 * The editor keeps the whole file in a fixed gap buffer
 * (ARKVIM_GB_MAX_SIZE).  `:w` writes the buffer back as the whole
 * file, so opening a file that does not fit would silently replace
 * it with its truncated head on the first save.  The command layer
 * therefore asks arkvim_vfs_probe() BEFORE entering the editor and
 * refuses the open when it cannot be honoured.  Refusing is the only
 * answer that does not lose data.
 * ============================================================ */
#ifndef ARKVIM_FILEIO_VFS_H
#define ARKVIM_FILEIO_VFS_H

#include <stdint.h>

#include "arkvim_fileio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe results.  Only ARKVIM_VFS_OK may be followed by a call to
 * arkvim_editor_run(); every other value must be reported to the user
 * and the open abandoned. */
#define ARKVIM_VFS_OK         0
#define ARKVIM_VFS_ETOOLONG  (-1)   /* resolved path does not fit      */
#define ARKVIM_VFS_ETOOBIG   (-2)   /* file larger than the buffer     */
#define ARKVIM_VFS_EISDIR    (-3)   /* path is a directory             */
#define ARKVIM_VFS_EIO       (-4)   /* stat failed for a reason other
                                     * than "no such file"             */

/**
* @brief Check whether filename can be opened by the editor.
* @param filename  path as typed by the user; relative paths are
*                  resolved against the shell's current directory
* @param size      optional; receives the file size (0 for a new file)
* @return ARKVIM_VFS_OK, or one of the ARKVIM_VFS_E* codes above
* @note  A missing file is OK: the editor opens an empty buffer and
*        `:w` creates it, which is what vim does.
*/
int arkvim_vfs_probe(const char *filename, uint32_t *size);

/** The backend object to pass to arkvim_fileio_set_backend(). */
extern const arkvim_fileio_backend_t arkvim_fileio_vfs_backend;

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_FILEIO_VFS_H */
