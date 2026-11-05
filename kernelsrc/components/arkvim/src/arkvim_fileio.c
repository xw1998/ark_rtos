/* ============================================================
 * arkvim_fileio.c - Backend Dispatch
 * ============================================================ */
#include "arkvim_fileio.h"
#include <stddef.h>

static const arkvim_fileio_backend_t *s_backend = NULL;

void arkvim_fileio_set_backend(const arkvim_fileio_backend_t *backend)
{
    s_backend = backend;
}

const arkvim_fileio_backend_t *arkvim_fileio_get_backend(void)
{
    return s_backend;
}

int arkvim_fileio_load(arkvim_editor_t *e, const char *filename)
{
    if (s_backend == NULL || s_backend->load == NULL) {
        return -1;
    }
    return s_backend->load(filename, e);
}

int arkvim_fileio_save(arkvim_editor_t *e, const char *filename)
{
    if (s_backend == NULL || s_backend->save == NULL) {
        return -1;
    }
    return s_backend->save(filename, e);
}

int arkvim_fileio_cat(const char *filename)
{
    if (s_backend == NULL || s_backend->cat == NULL) {
        return -1;
    }
    return s_backend->cat(filename);
}

int arkvim_fileio_ls(void)
{
    if (s_backend == NULL || s_backend->ls == NULL) {
        return -1;
    }
    return s_backend->ls();
}
