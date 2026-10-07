/**
* @file ark_vfs_procfs.c
* @brief Kernel state as a read-only filesystem: /proc/<name>.
*
* Why a filesystem: the first thing a Linux user asks a machine is "what are
* you doing", and the answer lives in `cat /proc/uptime`, `cat /proc/meminfo`,
* `cat /proc/tasks`.  The kernel already holds every byte of that; this driver
* only formats it so the same habits work here.
*
* Shape:
*   - flat and read-only.  /proc holds exactly the files in g_proc_names.
*     There is no /proc/<pid>/ hierarchy: a task here has no per-task facts
*     worth inventing a directory for, and a fake one would only teach the
*     wrong reflex.
*   - no storage, no cached state.  Content is produced from the live kernel
*     on every read, so what you cat is what is true at that instant.
*   - records, not one big buffer.  Each file is a sequence of small records
*     formatted one at a time, so an open file costs a handle and nothing
*     else.  A single output buffer would have to hold a full 48-task table
*     (~2 KB) and every fd would carry one; the small parts have no RAM for
*     that, and the driver should not need it.
*   - never a fabricated field.  /proc/uptime's second field is the kernel's
*     measured idle-tick counter; when a build has no CPU-load accounting to
*     measure it with, the field is omitted rather than printed as a zero.
*     MemTotal/MemFree describe the loadable-image pool the allocator manages
*     (see arkrtos_ptable_stats), which is the closest honest analogue - they
*     are not the whole SRAM; KernelRamWindow reports the reserved window.
*/
#include "ark_vfs.h"
#include "ark_vfs_priv.h"
#include "ark_vfs_port_arkrtos.h"

/* Feature gate: the whole unit disappears with the VFS namespace it lives
 * in, and independently with ARKRTOS_USE_PROCFS (a board with no RAM to
 * spare can drop /proc without touching anything else). */
#include "arkrtos_features.h"

#if (ARKRTOS_USE_VFS != 1) || (ARKRTOS_USE_PROCFS != 1)

/* An empty translation unit is not valid C; keep a harmless declaration. */
typedef int ark_vfs_procfs_disabled_tu;

#else

#include "arkrtos_task.h"
#include "arkrtos_cfg.h"      /* arkrtos_task_table / arkrtos_task_count */
#include "arkrtos_ptable.h"
#include "arkrtos_version.h"
#include "arkrtos_arch.h"     /* ARKRTOS_CPU_ARCH for /proc/cpuinfo */
#include "arkrtos_config.h"

/* ------------------------------------------------------------------
 * File table
 * ------------------------------------------------------------------ */

enum {
    PROC_VERSION = 0,
    PROC_UPTIME,
    PROC_MEMINFO,
    PROC_CPUINFO,
    PROC_TASKS
};

#define PROC_FILE_COUNT   (5u)

/* One formatted record must fit here.  Every line below is a fixed, short
 * template well inside this bound, so a record can never be truncated; the
 * value leaves room for the longest task line plus slack. */
#define PROC_LINE_MAX     (128u)

static const char *const g_proc_names[PROC_FILE_COUNT] = {
    "version", "uptime", "meminfo", "cpuinfo", "tasks"
};

/* ------------------------------------------------------------------
 * Handles.  The core keeps the pointer and never looks inside, so the
 * layout is entirely ours.  Directory cursors reuse the same type.
 * ------------------------------------------------------------------ */

struct ark_vfs_file {
    uint8_t   used;
    uint8_t   is_dir;
    uint8_t   kind;      /* PROC_* for a file */
    ark_off_t pos;       /* byte offset for a file; scan index for a dir */
};

static struct ark_vfs_file g_proc_handles[ARK_VFS_MAX_FDS];

static struct ark_vfs_file *proc_handle_alloc(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++) {
        if (g_proc_handles[i].used == 0u) {
            ark_p_memset(&g_proc_handles[i], 0,
                         (ark_size_t)sizeof(g_proc_handles[i]));
            g_proc_handles[i].used = 1u;
            return &g_proc_handles[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Tiny appenders.  They refuse to overrun @p cap; every record is written
 * from a fixed template far shorter than PROC_LINE_MAX, so the bound is a
 * guard against a future edit, not something reached in practice.
 * ------------------------------------------------------------------ */

static uint32_t t_ch(char *o, uint32_t cap, uint32_t at, char c)
{
    if (at < cap) { o[at] = c; return at + 1u; }
    return at;
}

static uint32_t t_s(char *o, uint32_t cap, uint32_t at, const char *s)
{
    while (*s != '\0') { at = t_ch(o, cap, at, *s); s++; }
    return at;
}

static uint32_t t_u(char *o, uint32_t cap, uint32_t at, uint32_t v)
{
    char     tmp[11];
    uint32_t n = 0u;

    if (v == 0u) { return t_ch(o, cap, at, '0'); }
    while (v != 0u) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0u) { at = t_ch(o, cap, at, tmp[--n]); }
    return at;
}

/** @brief Right-aligned decimal, padded with spaces to @p width. */
static uint32_t t_upad(char *o, uint32_t cap, uint32_t at, uint32_t v,
                       uint32_t width)
{
    char     tmp[11];
    uint32_t n = 0u;
    uint32_t i;

    if (v == 0u) { tmp[n++] = '0'; }
    else { while (v != 0u) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } }
    for (i = n; i < width; i++) { at = t_ch(o, cap, at, ' '); }
    while (n > 0u) { at = t_ch(o, cap, at, tmp[--n]); }
    return at;
}

/** @brief Left-aligned string, padded with spaces to @p width. */
static uint32_t t_spad(char *o, uint32_t cap, uint32_t at, const char *s,
                       uint32_t width)
{
    uint32_t n = 0u;

    while (s[n] != '\0') { at = t_ch(o, cap, at, s[n]); n++; }
    while (n < width) { at = t_ch(o, cap, at, ' '); n++; }
    return at;
}

static uint32_t t_hex8(char *o, uint32_t cap, uint32_t at, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        at = t_ch(o, cap, at, hx[(v >> (uint32_t)i) & 0xFu]);
    }
    return at;
}

/** @brief "SSS.FF" from a tick count.  1 tick = 500 us = 0.05 centiseconds,
 *  so centiseconds = ticks / 20 with no rounding noise (tick period is a
 *  whole number of microseconds and divides 1 cs evenly). */
static uint32_t t_secs(char *o, uint32_t cap, uint32_t at, uint32_t ticks)
{
    uint32_t centis = ticks / 20u;
    uint32_t whole  = centis / 100u;
    uint32_t frac   = centis % 100u;

    at = t_u(o, cap, at, whole);
    at = t_ch(o, cap, at, '.');
    at = t_ch(o, cap, at, (char)('0' + (frac / 10u)));
    at = t_ch(o, cap, at, (char)('0' + (frac % 10u)));
    return at;
}

static const char *arch_name(void)
{
#if defined(ARKRTOS_CPU_ARCH)
#if (ARKRTOS_CPU_ARCH == ARKRTOS_ARCH_CORTEX_M3)
    return "ARM Cortex-M3";
#elif (ARKRTOS_CPU_ARCH == ARKRTOS_ARCH_CORTEX_M4)
    return "ARM Cortex-M4";
#elif (ARKRTOS_CPU_ARCH == ARKRTOS_ARCH_CORTEX_M7)
    return "ARM Cortex-M7";
#endif
#endif
    return "unknown";     /* never guess a core we were not told */
}

static const char *task_state_name(uint32_t st)
{
    switch (st) {
    case ARKRTOS_TASK_INVALID: return "INVALID";
    case ARKRTOS_TASK_READY:   return "READY";
    case ARKRTOS_TASK_WAIT:    return "WAIT";
    case ARKRTOS_TASK_RUNNING: return "RUNNING";
    default:                   return "?";
    }
}

/* ------------------------------------------------------------------
 * Record formatters.  Each writes record @p idx and returns its length, or
 * 0 when there is no such record (end of file).
 * ------------------------------------------------------------------ */

static uint32_t fmt_version(uint32_t idx, char *o, uint32_t cap)
{
    uint32_t at = 0u;

    if (idx != 0u) { return 0u; }
    at = t_s(o, cap, at, "Ark RTOS arkrtos " ARKRTOS_VERSION_STRING);
    at = t_s(o, cap, at, " (build " __DATE__ " " __TIME__ ")\n");
    return at;
}

static uint32_t fmt_uptime(uint32_t idx, char *o, uint32_t cap)
{
    uint32_t at = 0u;

    if (idx != 0u) { return 0u; }
    at = t_secs(o, cap, at, arkrtos_kernel_get_tick());
#if (ARKRTOS_USE_CPU_LOAD == 1)
    /* Linux's second field: cumulative idle.  Only emitted when the kernel
     * actually measures it (see arkrtos_idle_ticks). */
    at = t_ch(o, cap, at, ' ');
    at = t_secs(o, cap, at, arkrtos_idle_ticks);
#endif
    at = t_ch(o, cap, at, '\n');
    return at;
}

static uint32_t fmt_meminfo(uint32_t idx, char *o, uint32_t cap)
{
    uint32_t at = 0u;
    uint32_t used = 0u;
    uint32_t total = 0u;
    uint32_t frag = 0u;
    const arkrtos_partition_table_t *pt = arkrtos_ptable_get();

    (void)arkrtos_ptable_stats(&used, &total, &frag);
    (void)frag;

    if (idx == 0u) {
        at = t_s(o, cap, at, "MemTotal:        ");
        at = t_upad(o, cap, at, total / 1024u, 8u);
        at = t_s(o, cap, at, " kB\n");
    } else if (idx == 1u) {
        at = t_s(o, cap, at, "MemFree:         ");
        at = t_upad(o, cap, at,
                    ((total >= used) ? (total - used) : 0u) / 1024u, 8u);
        at = t_s(o, cap, at, " kB\n");
    } else if (idx == 2u) {
        at = t_s(o, cap, at, "KernelRamWindow: ");
        at = t_upad(o, cap, at,
                    ((pt != NULL) ? pt->kernel_ram_size : 0u) / 1024u, 8u);
        at = t_s(o, cap, at, " kB\n");
    } else {
        return 0u;
    }
    return at;
}

static uint32_t fmt_cpuinfo(uint32_t idx, char *o, uint32_t cap)
{
    uint32_t at = 0u;

    if (idx == 0u) {
        at = t_s(o, cap, at, "processor\t: 0\n");
    } else if (idx == 1u) {
        at = t_s(o, cap, at, "model name\t: ");
        at = t_s(o, cap, at, arch_name());
        at = t_ch(o, cap, at, '\n');
    } else if (idx == 2u) {
        int any = 0;

        at = t_s(o, cap, at, "Features\t:");
#if (ARKRTOS_USE_FPU == 1)
        at = t_s(o, cap, at, " fpu"); any = 1;
#endif
#if (ARKRTOS_USE_MPU == 1)
        at = t_s(o, cap, at, " mpu"); any = 1;
#endif
#if (ARKRTOS_USE_PRIV == 1)
        at = t_s(o, cap, at, " priv"); any = 1;
#endif
        if (any == 0) { at = t_s(o, cap, at, " (none)"); }
        at = t_ch(o, cap, at, '\n');
    } else {
        return 0u;
    }
    return at;
}

static uint32_t fmt_tasks(uint32_t idx, char *o, uint32_t cap)
{
    uint32_t at = 0u;
    uint32_t n;
    int32_t  count = arkrtos_task_count;

    if (count < 0) { count = 0; }
    n = (uint32_t)count;

    if (idx == 0u) {
        return t_s(o, cap, at, "  ID STATE    PRIO ENTRY\n");
    }
    if ((idx >= 1u) && (idx <= n)) {
        uint32_t i = idx - 1u;

        at = t_upad(o, cap, at, i + 1u, 4u);
        at = t_ch(o, cap, at, ' ');
        at = t_spad(o, cap, at,
                    task_state_name((uint32_t)arkrtos_task_table[i].status), 8u);
        at = t_ch(o, cap, at, ' ');
        at = t_upad(o, cap, at, (uint32_t)arkrtos_task_table[i].priority, 4u);
        at = t_ch(o, cap, at, ' ');
        at = t_s(o, cap, at, "0x");
        at = t_hex8(o, cap, at,
                    (uint32_t)(uintptr_t)arkrtos_task_table[i].entry);
        at = t_ch(o, cap, at, '\n');
        return at;
    }
    if (idx == (n + 1u)) {
        at = t_s(o, cap, at, "tasks: ");
        at = t_u(o, cap, at, n);
        at = t_s(o, cap, at, " used / ");
        at = t_u(o, cap, at, (uint32_t)ARKRTOS_TASK_MAX_NUM);
        at = t_s(o, cap, at, " max\n");
        return at;
    }
    return 0u;
}

static uint32_t rec_count(uint8_t kind)
{
    switch (kind) {
    case PROC_VERSION: return 1u;
    case PROC_UPTIME:  return 1u;
    case PROC_MEMINFO: return 3u;
    case PROC_CPUINFO: return 3u;
    case PROC_TASKS:
        return 2u + ((arkrtos_task_count > 0) ? (uint32_t)arkrtos_task_count : 0u);
    default:           return 0u;
    }
}

static uint32_t rec_len(uint8_t kind, uint32_t idx, char *o, uint32_t cap)
{
    switch (kind) {
    case PROC_VERSION: return fmt_version(idx, o, cap);
    case PROC_UPTIME:  return fmt_uptime(idx, o, cap);
    case PROC_MEMINFO: return fmt_meminfo(idx, o, cap);
    case PROC_CPUINFO: return fmt_cpuinfo(idx, o, cap);
    case PROC_TASKS:   return fmt_tasks(idx, o, cap);
    default:           return 0u;
    }
}

static ark_off_t proc_total_len(uint8_t kind)
{
    char     line[PROC_LINE_MAX];
    ark_off_t sum = 0;
    uint32_t  c = rec_count(kind);
    uint32_t  i;

    for (i = 0u; i < c; i++) {
        sum += (ark_off_t)rec_len(kind, i, line, (uint32_t)sizeof(line));
    }
    return sum;
}

/* ------------------------------------------------------------------
 * The stream reader: walk the records and copy the window [pos, pos+n)
 * out of the concatenation, then advance *ppos.  Iterating from record 0
 * every call is cheap (a whole /proc/tasks is a few dozen short records)
 * and is what lets the driver hold no buffer at all.
 * ------------------------------------------------------------------ */

static ark_ssize_t proc_read_range(uint8_t kind, ark_off_t *ppos, void *buf,
                                   ark_size_t n)
{
    char     line[PROC_LINE_MAX];
    char    *b   = (char *)buf;
    ark_off_t pos = *ppos;
    ark_off_t base = 0;
    ark_size_t out = 0u;
    uint32_t  c = rec_count(kind);
    uint32_t  i;

    for (i = 0u; i < c; i++) {
        uint32_t  len = rec_len(kind, i, line, (uint32_t)sizeof(line));
        ark_off_t end;

        if (len == 0u) { continue; }       /* no such record */
        end = base + (ark_off_t)len;
        if (pos >= end) { base = end; continue; }

        {
            ark_off_t s = (pos > base) ? pos : base;

            while ((s < end) && (out < n)) {
                b[out++] = line[(uint32_t)(s - base)];
                s++;
            }
            if (s < end) { *ppos = s; return (ark_ssize_t)out; }  /* buffer full */
            base = end;
            pos  = base;
            if (out == n) { *ppos = base; return (ark_ssize_t)out; }
        }
    }
    *ppos = base;      /* end of stream */
    return (ark_ssize_t)out;
}

/* ------------------------------------------------------------------
 * fsdrv
 * ------------------------------------------------------------------ */

static int32_t procfs_mount(struct ark_vfs_mnt *mnt)
{
    uint32_t i;

    (void)mnt;
    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++) {
        g_proc_handles[i].used = 0u;
    }
    return ARK_VFS_OK;
}

static int32_t procfs_umount(struct ark_vfs_mnt *mnt)
{
    (void)mnt;
    return ARK_VFS_OK;
}

static int32_t procfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                           struct ark_vfs_file **out)
{
    struct ark_vfs_file *h;
    uint32_t i;

    (void)mnt;
    if (rel[0] == '\0') { return ARK_E_ISDIR; }
    for (i = 0u; rel[i] != '\0'; i++) {
        if (rel[i] == '/') { return ARK_E_NOENT; }   /* /proc is flat */
    }
    if ((flags & ARK_O_ACCMODE) != ARK_O_RDONLY) { return ARK_E_ROFS; }
    for (i = 0u; i < PROC_FILE_COUNT; i++) {
        if (ark_p_strcmp(rel, g_proc_names[i]) == 0) { break; }
    }
    if (i == PROC_FILE_COUNT) { return ARK_E_NOENT; }

    h = proc_handle_alloc();
    if (h == NULL) { return ARK_E_FULL; }
    h->is_dir = 0u;
    h->kind   = (uint8_t)i;
    h->pos    = 0;
    *out = h;
    return ARK_VFS_OK;
}

static int32_t procfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    (void)mnt;
    if (f == NULL) { return ARK_E_INVAL; }
    f->used = 0u;
    return ARK_VFS_OK;
}

static ark_ssize_t procfs_read(struct ark_vfs_file *f, void *buf, ark_size_t n)
{
    if ((f == NULL) || (f->is_dir != 0u)) { return ARK_E_BADF; }
    if (buf == NULL) { return ARK_E_INVAL; }
    if (n == 0u) { return 0; }
    return proc_read_range(f->kind, &f->pos, buf, n);
}

static int32_t procfs_seek(struct ark_vfs_file *f, ark_off_t off, int whence)
{
    ark_off_t size;
    ark_off_t target;

    if ((f == NULL) || (f->is_dir != 0u)) { return ARK_E_BADF; }
    size = proc_total_len(f->kind);
    if (whence == ARK_SEEK_SET)      { target = off; }
    else if (whence == ARK_SEEK_CUR) { target = f->pos + off; }
    else if (whence == ARK_SEEK_END) { target = size + off; }
    else                             { return ARK_E_INVAL; }
    if ((target < 0) || (target > size)) { return ARK_E_INVAL; }
    f->pos = target;
    return ARK_VFS_OK;
}

static int32_t procfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                           struct ark_vfs_stat *st)
{
    uint32_t i;

    (void)mnt;
    if (st == NULL) { return ARK_E_INVAL; }
    if (rel[0] == '\0') {
        st->mode  = (uint32_t)(ARK_S_IFDIR | ARK_S_DEFAULT_DIR);
        st->size  = 0;
        st->mtime = 0u;
        st->uid   = 0u;
        st->gid   = 0u;
        return ARK_VFS_OK;
    }
    for (i = 0u; rel[i] != '\0'; i++) {
        if (rel[i] == '/') { return ARK_E_NOENT; }
    }
    for (i = 0u; i < PROC_FILE_COUNT; i++) {
        if (ark_p_strcmp(rel, g_proc_names[i]) == 0) {
            st->mode  = (uint32_t)(ARK_S_IFREG | ARK_S_DEFAULT_FILE);
            st->size  = proc_total_len((uint8_t)i);
            st->mtime = 0u;
            st->uid   = 0u;
            st->gid   = 0u;
            return ARK_VFS_OK;
        }
    }
    return ARK_E_NOENT;
}

static int32_t procfs_opendir(struct ark_vfs_mnt *mnt, const char *rel,
                              struct ark_vfs_file **out)
{
    struct ark_vfs_file *h;

    (void)mnt;
    if (rel[0] != '\0') { return ARK_E_NOTDIR; }   /* no subdirectories */
    h = proc_handle_alloc();
    if (h == NULL) { return ARK_E_FULL; }
    h->is_dir = 1u;
    h->pos    = 0;
    *out = h;
    return ARK_VFS_OK;
}

static int32_t procfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    uint32_t i;

    if ((d == NULL) || (d->is_dir == 0u)) { return ARK_E_NOTDIR; }
    if (ent == NULL) { return ARK_E_INVAL; }
    i = (uint32_t)d->pos;
    if (i >= PROC_FILE_COUNT) { return ARK_E_NOENT; }   /* end of listing */
    d->pos = (ark_off_t)(i + 1u);
    if ((ark_p_strlen(g_proc_names[i]) + 1u) > (ark_size_t)ARK_VFS_NAME_MAX) {
        /* A name that cannot fit is skipped, not truncated into a wrong one.
         * With the fixed table above this is unreachable, and it stays here
         * so an edit cannot turn it into a silent lie. */
        return ARK_E_NOENT;
    }
    ark_p_strcpy(ent->name, (ark_size_t)ARK_VFS_NAME_MAX, g_proc_names[i]);
    ent->mode = (uint32_t)(ARK_S_IFREG | ARK_S_DEFAULT_FILE);
    ent->size = proc_total_len((uint8_t)i);
    ent->uid  = 0u;
    ent->gid  = 0u;
    return ARK_VFS_OK;
}

static int32_t procfs_closedir(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d)
{
    (void)mnt;
    if (d != NULL) { d->used = 0u; }
    return ARK_VFS_OK;
}

const ark_vfs_fsdrv_t ark_vfs_arkrtos_procfs = {
    "procfs",
    ARK_FS_CAP_READ | ARK_FS_CAP_DIR | ARK_FS_CAP_STAT,
    procfs_mount,
    procfs_umount,
    procfs_open,
    procfs_close,
    procfs_read,
    NULL,               /* write: /proc is generated, never stored */
    procfs_seek,
    procfs_stat,
    NULL,               /* unlink: nothing to remove                 */
    NULL,               /* mkdir:  /proc is flat and fixed           */
    NULL,               /* rename: a name IS the kernel fact         */
    procfs_opendir,
    procfs_readdir,
    procfs_closedir,
    NULL,               /* chmod: a generated file has no access bits */
    NULL                /* chown: and no owner to change             */
};

#endif  /* ARKRTOS_USE_VFS && ARKRTOS_USE_PROCFS */
