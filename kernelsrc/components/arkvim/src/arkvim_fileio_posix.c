/* ============================================================
 * arkvim_fileio_posix.c - POSIX File Backend (PC simulation)
 *
 * Uses stdio fread/fwrite so the editor can edit real files on the
 * host during development. On an MCU this file is excluded from the
 * build and replaced by a backend that talks to LittleFS etc.
 * ============================================================ */
#include "arkvim_fileio.h"
#include "arkvim_modes.h"      /* arkvim_editor_t + editor buffer access */
#include "arkvim_gap_buffer.h"
#include "arkvim_terminal.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || \
    defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)

#define ARKVIM_CHUNK 256

static int posix_load(const char *filename, arkvim_editor_t *e)
{
    FILE *fp;
    char  buf[ARKVIM_CHUNK];
    size_t n;

    fp = fopen(filename, "rb");
    if (fp == NULL) {
        return -1;   /* file does not exist; caller treats as new file */
    }

    gb_clear(&e->gb);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (!gb_insert_str(&e->gb, gb_length(&e->gb), buf, (uint32_t)n)) {
            break;   /* buffer full */
        }
    }
    fclose(fp);
    return 0;
}

static int posix_save(const char *filename, arkvim_editor_t *e)
{
    FILE *fp;
    char  buf[ARKVIM_CHUNK];
    uint32_t written = 0;
    uint32_t total = gb_length(&e->gb);

    fp = fopen(filename, "wb");
    if (fp == NULL) {
        return -1;
    }

    while (written < total) {
        uint32_t want = total - written;
        uint32_t got;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        got = gb_copy_range(&e->gb, written, written + want, buf, sizeof(buf));
        if (got == 0) {
            break;
        }
        if (fwrite(buf, 1, got, fp) != got) {
            fclose(fp);
            return -1;
        }
        written += got;
    }
    fclose(fp);
    return 0;
}

static int posix_cat(const char *filename)
{
    FILE *fp;
    char  buf[ARKVIM_CHUNK];
    size_t n;

    fp = fopen(filename, "rb");
    if (fp == NULL) {
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        arkvim_term_write(buf, (uint32_t)n);
    }
    fclose(fp);
    return 0;
}

/* Directory listing. POSIX uses dirent; Windows MinGW provides
 * <dirent.h> too, so one path covers both. */
#include <dirent.h>

static int posix_ls(void)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir(".");
    if (dir == NULL) {
        return -1;
    }
    arkvim_term_puts("\r\n");
    while ((ent = readdir(dir)) != NULL) {
        arkvim_term_puts("  ");
        arkvim_term_puts(ent->d_name);
        arkvim_term_puts("\r\n");
    }
    closedir(dir);
    return 0;
}

const arkvim_fileio_backend_t arkvim_fileio_posix_backend = {
    posix_load, posix_save, posix_cat, posix_ls
};

#else  /* non-POSIX build: no-op backend so the editor still links */

static int no_load(const char *f, arkvim_editor_t *e) { (void)f; (void)e; return -1; }
static int no_save(const char *f, arkvim_editor_t *e) { (void)f; (void)e; return -1; }
static int no_cat(const char *f) { (void)f; return -1; }
static int no_ls(void) { return -1; }

const arkvim_fileio_backend_t arkvim_fileio_posix_backend = {
    no_load, no_save, no_cat, no_ls
};

#endif
