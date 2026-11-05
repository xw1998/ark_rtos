/* ============================================================
 * arkvim_fileio.h - File I/O with Pluggable Backend
 *
 * The editor never talks to a filesystem directly. Instead it calls
 * a backend registered by the integrator. The PC demo registers a
 * POSIX backend (arkvim_fileio_posix.c); an MCU build registers one
 * built on LittleFS / SPI Flash / etc.
 *
 * This mirrors the shell's platform.h philosophy: keep the editor
 * portable by isolating the only platform-specific piece.
 * ============================================================ */
#ifndef ARKVIM_FILEIO_H
#define ARKVIM_FILEIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration: the editor type is defined in arkvim_modes.h. */
struct arkvim_editor_s;
typedef struct arkvim_editor_s arkvim_editor_t;

/* Backend interface. Each function returns 0 on success, -1 on
 * failure. `load` clears the editor buffer and fills it; `save`
 * writes the editor buffer out; `cat` streams a file to the
 * terminal; `ls` lists filenames to the terminal. */
typedef struct {
    int (*load)(const char *filename, arkvim_editor_t *e);
    int (*save)(const char *filename, arkvim_editor_t *e);
    int (*cat)(const char *filename);
    int (*ls)(void);
} arkvim_fileio_backend_t;

/* Register the active backend. Pass NULL to disable persistence
 * (the editor still works as a scratch buffer). */
void arkvim_fileio_set_backend(const arkvim_fileio_backend_t *backend);
const arkvim_fileio_backend_t *arkvim_fileio_get_backend(void);

/* Editor-facing helpers. These route to the backend and report
 * status through the editor's status line. */
int  arkvim_fileio_load(arkvim_editor_t *e, const char *filename);
int  arkvim_fileio_save(arkvim_editor_t *e, const char *filename);
int  arkvim_fileio_cat(const char *filename);
int  arkvim_fileio_ls(void);

/* POSIX backend (PC simulation). Defined in arkvim_fileio_posix.c. */
extern const arkvim_fileio_backend_t arkvim_fileio_posix_backend;

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_FILEIO_H */
