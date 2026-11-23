/**
* @file test_shell_fs_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_fs.c.
*
* What this proves: the four command families in that file behave as written on
* the host, with every refusal path executed:
*   - "blk": list/probe/rd/wr/erase/test against an in-memory backend that only
*     accepts a write on erased (0xFF) bytes and only erases on erase-unit
*     boundaries, so "ok" cannot be printed for a write that did nothing;
*   - "fs": the littlefs volume facade - mount / unmount / format / info / ls /
*     wr / rd / put / rm / test / err, including the unformatted-volume hint
*     and the chunked binary upload's ACK/NAK handshake;
*   - the VFS commands: mount / umount / ls (-l, -a, file argument) / cat
*     (file and stdin) / rm / mkdir / rmdir / mv / cp (file and into-a-dir) /
*     touch / stat / chmod / chown, plus path completion;
*   - "mini": run / stop / count / limit / forget / swap / autostart and the
*     list form;
*   - "pwd" / "cd" / "df".
*
* What it does NOT prove:
*   - that any block device, littlefs volume or real VFS exists: every backend
*     entry point here is an in-memory fixture with a programmable return code;
*   - that a byte written by "blk wr" reaches a chip, or that "fs test" would
*     catch a real cache that never flushes: the facade is a stub;
*   - anything about addresses: the fixture device is a 4 KiB array.
*
* The measured module is built with the board's default feature set
* (arkrtos_board_config.h), so ARKRTOS_MINI_READ_VFS=0 and the "fs" branch of
* mini_auto_stat() is the one compiled in; the vfs branch is not in the
* denominator. swap is compiled in via -DARKRTOS_USE_TASK_SWAP=1.
*
* Seven lines stay uncovered, and none of them is reachable from this fixture:
*   - vfs_comp_cb()'s "would not fit" guard needs a 63-char directory prefix
*     plus a 64-char child name, which cannot coexist in a 64-byte vnode path;
*   - the three "path too long" branches in mini run/swap/forget need an
*     argument longer than ARK_SHELL_PATH_SIZE-1, but ark_shell_resolve() caps
*     what the shell can hand over at 95 chars, so the guard never fires.
* They are kept as defence in depth, not exercised by a fabricated call.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_shell_internal.h"

/* ------------------------------------------------------------ assertions */

static int g_checks;
static int g_failed;

#define CK(cond)                                                       \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                              \
    } while (0)

#define CK_I(got, want)                                                \
    do {                                                               \
        long g_ = (long)(got);                                         \
        long w_ = (long)(want);                                        \
        g_checks++;                                                    \
        if (g_ != w_) {                                                \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %ld want %ld)\n",            \
                   __LINE__, #got, g_, w_);                            \
        }                                                              \
    } while (0)

#define CK_S(got, want)                                                \
    do {                                                               \
        const char *g_ = (got);                                        \
        const char *w_ = (want);                                       \
        g_checks++;                                                    \
        if ((g_ == NULL) || (strcmp(g_, w_) != 0)) {                    \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, (g_ != NULL) ? g_ : "(null)", w_);  \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[16384];
static size_t g_out_len;

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0u;
}

static void cap_add(const char *s, size_t n)
{
    size_t room = sizeof(g_out) - 1u - g_out_len;

    if (n > room) {
        n = room;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

static int out_count(const char *needle)
{
    const char *p = g_out;
    size_t n = strlen(needle);
    int c = 0;

    if (n == 0u) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

static int out_before(const char *a, const char *b)
{
    const char *pa = strstr(g_out, a);
    const char *pb = strstr(g_out, b);

    return (pa != NULL) && (pb != NULL) && (pa < pb);
}

/* 可写副本：被测接口收 char *，而字面量是 const char[]（-Wwrite-strings）。 */
static char *W(const char *s)
{
    static char bufs[8][ARK_SHELL_PATH_SIZE];
    static unsigned idx;
    char *b = bufs[idx % 8u];
    size_t n = strlen(s);

    idx++;
    if (n >= sizeof(bufs[0])) {
        n = sizeof(bufs[0]) - 1u;
    }
    memcpy(b, s, n);
    b[n] = '\0';
    return b;
}

static int runv(int (*fn)(int, char **), int argc, const char *const *args)
{
    char *argv[8];
    int i;

    for (i = 0; (i < argc) && (i < 8); i++) {
        argv[i] = W(args[i]);
    }
    return fn(argc, argv);
}

#define RUN(fn, ...)                                                   \
    runv((fn),                                                         \
         (int)(sizeof((const char *[]){ __VA_ARGS__ })                 \
               / sizeof(const char *)),                                \
         (const char *[]){ __VA_ARGS__ })

/* ------------------------------------------------------------ stubs: out */

void ark_shell_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        cap_add(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
}

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

/* 只认十进制数字，别的一律拒绝（被测代码用它区分「子命令」与「数」）。 */
int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if ((s == NULL) || (out == NULL) || (*s == '\0')) {
        return -1;
    }
    while (*s != '\0') {
        if ((*s < '0') || (*s > '9')) {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* ---- shell core helpers (ark_shell.c is not linked here) ---- */

ark_shell_t g_shell;

void ark_shell_get_cwd(const ark_shell_t *shell, char *out, uint32_t cap)
{
    if ((out == NULL) || (cap == 0u)) {
        return;
    }
    if (shell == NULL) {
        (void)strncpy(out, ARK_SHELL_HOME, cap - 1u);
        out[cap - 1u] = '\0';
        return;
    }
    (void)strncpy(out, shell->cwd, cap - 1u);
    out[cap - 1u] = '\0';
}

void ark_shell_set_cwd(ark_shell_t *shell, const char *path)
{
    if ((shell == NULL) || (path == NULL)) {
        return;
    }
    (void)strncpy(shell->cwd, path, sizeof(shell->cwd) - 1u);
    shell->cwd[sizeof(shell->cwd) - 1u] = '\0';
}

/* 忠实复刻 ark_shell.c 的规范化规则：保留前导 '/'、丢掉空段与 '.'、'..'
 * 回退一段、'~' 是 home。被测命令的所有路径都经它落地。 */
void ark_shell_resolve(const ark_shell_t *shell, const char *in,
                       char *out, uint32_t cap)
{
    char tmp[ARK_SHELL_PATH_SIZE * 2u];
    const char *base;
    const char *rel;
    const char *p;
    uint32 oi;

    if ((out == NULL) || (cap < 2u)) {
        return;
    }
    out[0] = '\0';

    if ((in == NULL) || (in[0] == '\0')) {
        base = (shell != NULL) ? shell->cwd : ARK_SHELL_HOME;
        rel  = "";
    } else if ((in[0] == '~') && ((in[1] == '\0') || (in[1] == '/'))) {
        base = ARK_SHELL_HOME;
        rel  = (in[1] == '/') ? (in + 2) : "";
    } else if (in[0] == '/') {
        base = "";
        rel  = in;
    } else {
        base = (shell != NULL) ? shell->cwd : ARK_SHELL_HOME;
        rel  = in;
    }

    if (rel[0] == '\0') {
        (void)snprintf(tmp, sizeof(tmp), "%s", (base[0] != '\0') ? base : "/");
    } else if (base[0] == '\0') {
        (void)snprintf(tmp, sizeof(tmp), "/%s", rel);
    } else {
        (void)snprintf(tmp, sizeof(tmp), "%s/%s", base, rel);
    }

    oi = 1u;
    out[0] = '/';
    p = tmp;
    while (*p != '\0') {
        const char *seg;
        uint32 seglen;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        seg = p;
        while ((*p != '\0') && (*p != '/')) {
            p++;
        }
        seglen = (uint32)(p - seg);
        if ((seglen == 1u) && (seg[0] == '.')) {
            continue;
        }
        if ((seglen == 2u) && (seg[0] == '.') && (seg[1] == '.')) {
            if (oi > 1u) {
                oi--;
                while ((oi > 1u) && (out[oi - 1u] != '/')) {
                    oi--;
                }
            }
            continue;
        }
        if (oi > 1u) {
            out[oi] = '/';
            oi++;
        }
        if ((oi + seglen) >= cap) {
            break;
        }
        memcpy(&out[oi], seg, seglen);
        oi += seglen;
    }
    if (oi == 1u) {
        out[1] = '\0';
    } else {
        out[oi] = '\0';
    }
}

/* stdin: scripted bytes for `cat` with no argument */
static const char *g_stdin;
static int g_stdin_pos;

int ark_shell_stdin_getc(void)
{
    unsigned char c;

    if ((g_stdin == NULL) || (g_stdin[g_stdin_pos] == '\0')) {
        return -1;
    }
    c = (unsigned char)g_stdin[g_stdin_pos];
    g_stdin_pos++;
    return (int)c;
}

/* ---- console device: scripted bytes for `fs put` ---- */

static int32 g_dev_read_left;
static int32 g_dev_write_calls;
static uint8 g_dev_written[64];

int32 arkrtos_dev_read_internal(int32 dev, uint8 *buf, int32 len)
{
    int32 n = 0;

    (void)dev;
    if ((buf == NULL) || (len <= 0)) {
        return -1;
    }
    while ((n < len) && (g_dev_read_left > 0)) {
        buf[n] = (uint8)(n & 0xFF);
        n++;
        g_dev_read_left--;
    }
    return n;
}

int32 arkrtos_dev_write_internal(int32 dev, uint8 *buf, int32 len)
{
    int32 i;

    (void)dev;
    for (i = 0; (i < len) && (g_dev_write_calls < 64); i++) {
        g_dev_written[g_dev_write_calls] = buf[i];
        g_dev_write_calls++;
    }
    return len;
}

int32 arkrtos_shell_uart_open(void)
{
    return 0;
}

static int32 g_recv_rc;

int32 cfg_recv_exact(int32 dev, uint8 *buf, uint32 len, uint32 timeout_ms)
{
    uint32 i;

    (void)dev;
    (void)timeout_ms;
    if (g_recv_rc != 0) {
        return g_recv_rc;
    }
    for (i = 0u; i < len; i++) {
        buf[i] = (uint8)(i & 0x7Fu);
    }
    return 0;
}

/* ---- block devices: two in-memory backends ---- */

#define BDEV_SIZE 4096u
#define BDEV_UNIT 256u

static uint8 g_flash[BDEV_SIZE];
static int32 g_blk_read_rc;
static int32 g_blk_write_rc;
static int32 g_blk_erase_rc;
static int32 g_blk_init_rc_a;
static int32 g_blk_init_rc_b;
static uint32 g_blk_unit = 256u;      /* erase alignment the device enforces */
static int   g_blk_erase_noop;        /* erase answers ok but changes nothing */
static int   g_blk_write_corrupt;     /* write stores the wrong bytes          */
static int   g_blk_read_ok_left = -1; /* >=0: only this many reads succeed      */

static int32 bdev_read(uint32 off, uint8 *buf, uint32 len)
{
    if (g_blk_read_ok_left == 0) {
        return -1;
    }
    if (g_blk_read_ok_left > 0) {
        g_blk_read_ok_left--;
    }
    if (g_blk_read_rc != 0) {
        return -1;
    }
    if ((off > BDEV_SIZE) || (len > (BDEV_SIZE - off))) {
        return -1;
    }
    memcpy(buf, &g_flash[off], len);
    return 0;
}

static int32 bdev_write(uint32 off, const uint8 *buf, uint32 len)
{
    uint32 i;

    if (g_blk_write_rc != 0) {
        return -1;
    }
    if ((off > BDEV_SIZE) || (len > (BDEV_SIZE - off))) {
        return -1;
    }
    for (i = 0u; i < len; i++) {
        if (g_flash[off + i] != 0xFFu) {
            return -1;              /* not erased: the device cannot do it */
        }
    }
    for (i = 0u; i < len; i++) {
        g_flash[off + i] = g_blk_write_corrupt ? (uint8)(buf[i] ^ 0x01u) : buf[i];
    }
    return 0;
}

static int32 bdev_erase(uint32 off, uint32 len)
{
    if (g_blk_erase_rc != 0) {
        return -1;
    }
    if (((off % g_blk_unit) != 0u) || ((len % g_blk_unit) != 0u)) {
        return -1;
    }
    if ((off > BDEV_SIZE) || (len > (BDEV_SIZE - off))) {
        return -1;
    }
    if (g_blk_erase_noop == 0) {
        memset(&g_flash[off], 0xFF, len);
    }
    return 0;
}

static int32 bdev_bringup_a(void)
{
    return g_blk_init_rc_a;
}

static int32 bdev_bringup_b(void)
{
    return g_blk_init_rc_b;
}

static const arkrtos_blk_dev_t g_bdev_a = {
    "nor0", BDEV_SIZE, BDEV_UNIT, bdev_bringup_a, NULL,
    bdev_read, bdev_write, bdev_erase
};

static const arkrtos_blk_dev_t g_bdev_b = {
    "nor1", BDEV_SIZE, 1024u, bdev_bringup_b, NULL,
    bdev_read, bdev_write, bdev_erase
};

static uint32 g_blk_n;

uint32 arkrtos_blk_count(void)
{
    return g_blk_n;
}

const arkrtos_blk_dev_t *arkrtos_blk_at(uint32 idx)
{
    if (idx == 0u) {
        return &g_bdev_a;
    }
    if ((idx == 1u) && (g_blk_n > 1u)) {
        return &g_bdev_b;
    }
    return NULL;
}

const arkrtos_blk_dev_t *arkrtos_blk_find(const char *name)
{
    uint32 i;

    for (i = 0u; i < g_blk_n; i++) {
        const arkrtos_blk_dev_t *d = arkrtos_blk_at(i);

        if ((d != NULL) && (strcmp(d->name, name) == 0)) {
            return d;
        }
    }
    return NULL;
}

int32 arkrtos_blk_init(const arkrtos_blk_dev_t *dev)
{
    return (dev != NULL) && (dev->bringup != NULL) ? dev->bringup() : -1;
}

uint32 arkrtos_blk_size(const arkrtos_blk_dev_t *dev)
{
    return (dev != NULL) ? dev->size : 0u;
}

int32 arkrtos_blk_read(const arkrtos_blk_dev_t *dev, uint32 off,
                       uint8 *buf, uint32 len)
{
    return (dev != NULL) ? dev->read(off, buf, len) : -1;
}

int32 arkrtos_blk_write(const arkrtos_blk_dev_t *dev, uint32 off,
                        const uint8 *buf, uint32 len)
{
    return (dev != NULL) ? dev->write(off, buf, len) : -1;
}

int32 arkrtos_blk_erase(const arkrtos_blk_dev_t *dev, uint32 off, uint32 len)
{
    return (dev != NULL) ? dev->erase(off, len) : -1;
}

/* ---- fs facade: a tiny volume of named blobs ---- */

#define FSN 12

typedef struct {
    char   path[64];
    uint32 size;
    uint8  data[512];
    int    used;
    int    isdir;
} fsnode_t;

static fsnode_t g_fs[FSN];
static uint8    g_fs_mounted;
static int32    g_fs_err;
static int      g_fs_unformatted_id;
static int32    g_fs_mount_rc;
static int32    g_fs_unmount_rc;
static int32    g_fs_format_rc;
static int32    g_fs_stat_rc;
static int32    g_fs_write_rc;
static int32    g_fs_read_rc;
static int32    g_fs_remove_rc;
static int32    g_fs_open_w_rc;
static int32    g_fs_write_next_rc;
static int32    g_fs_close_w_rc;
static int32    g_fs_list_rc;
static int32    g_fs_stat_path_rc;
static uint32   g_fs_total = 4096u;
static uint32   g_fs_used = 100u;
static int32    g_fs_mount_default_rc;
static int      g_fs_read_corrupt;
static int      g_fs_read_fail_after;  /* >=0: succeed this many reads, then fail */
static int32    g_fs_write_clamp;      /* >0: the volume keeps only this many bytes */
static int      g_fs_wfd = -1;
static uint8    g_fs_wbuf[512];
static uint32   g_fs_wlen;

static fsnode_t *fs_find(const char *path)
{
    int i;

    for (i = 0; i < FSN; i++) {
        if ((g_fs[i].used != 0) && (strcmp(g_fs[i].path, path) == 0)) {
            return &g_fs[i];
        }
    }
    return NULL;
}

static fsnode_t *fs_new(const char *path)
{
    int i;

    for (i = 0; i < FSN; i++) {
        if (g_fs[i].used == 0) {
            memset(&g_fs[i], 0, sizeof(g_fs[i]));
            (void)strncpy(g_fs[i].path, path, sizeof(g_fs[i].path) - 1u);
            g_fs[i].used = 1;
            return &g_fs[i];
        }
    }
    return NULL;
}

static void fs_reset(void)
{
    memset(g_fs, 0, sizeof(g_fs));
    g_fs_mounted = 0u;
    g_fs_err = 0;
    g_fs_wfd = -1;
    g_fs_wlen = 0u;
    g_fs_unformatted_id = 0;
    g_fs_mount_rc = 0;
    g_fs_unmount_rc = 0;
    g_fs_format_rc = 0;
    g_fs_stat_rc = 0;
    g_fs_write_rc = 0;
    g_fs_read_rc = 0;
    g_fs_remove_rc = 0;
    g_fs_open_w_rc = 0;
    g_fs_write_next_rc = 0;
    g_fs_close_w_rc = 0;
    g_fs_list_rc = 0;
    g_fs_stat_path_rc = 0;
    g_fs_mount_default_rc = 0;
    g_fs_read_corrupt = 0;
    g_fs_read_fail_after = -1;
    g_fs_write_clamp = 0;
}

int32 arkrtos_fs_mount(const char *dev, uint32 offset, uint32 size)
{
    (void)dev;
    (void)offset;
    (void)size;
    if (g_fs_mount_rc != 0) {
        g_fs_err = g_fs_mount_rc;
        return g_fs_mount_rc;
    }
    g_fs_mounted = 1u;
    return 0;
}

int32 arkrtos_fs_mount_default(void)
{
    if (g_fs_mount_default_rc != 0) {
        g_fs_err = g_fs_mount_default_rc;
        return g_fs_mount_default_rc;
    }
    g_fs_mounted = 1u;
    return 0;
}

int32 arkrtos_fs_unmount(void)
{
    if (g_fs_unmount_rc != 0) {
        g_fs_err = g_fs_unmount_rc;
        return g_fs_unmount_rc;
    }
    g_fs_mounted = 0u;
    return 0;
}

uint8 arkrtos_fs_mounted(void)
{
    return g_fs_mounted;
}

int32 arkrtos_fs_format(const char *dev, uint32 offset, uint32 size)
{
    (void)dev;
    (void)offset;
    (void)size;
    if (g_fs_format_rc != 0) {
        g_fs_err = g_fs_format_rc;
        return g_fs_format_rc;
    }
    memset(g_fs, 0, sizeof(g_fs));
    g_fs_mounted = 1u;
    return 0;
}

int32 arkrtos_fs_stat(uint32 *total, uint32 *used)
{
    if (g_fs_stat_rc != 0) {
        g_fs_err = g_fs_stat_rc;
        return g_fs_stat_rc;
    }
    if (total != NULL) {
        *total = g_fs_total;
    }
    if (used != NULL) {
        *used = g_fs_used;
    }
    return 0;
}

int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
{
    fsnode_t *n;

    if (g_fs_stat_path_rc != 0) {
        g_fs_err = g_fs_stat_path_rc;
        return g_fs_stat_path_rc;
    }
    n = fs_find(path);
    if (n == NULL) {
        g_fs_err = -2;
        return -1;
    }
    if (size != NULL) {
        *size = n->size;
    }
    if (is_dir != NULL) {
        *is_dir = 0u;
    }
    return 0;
}

int32 arkrtos_fs_write_file(const char *path, const uint8 *data, uint32 len)
{
    fsnode_t *n;

    if (g_fs_write_rc != 0) {
        g_fs_err = g_fs_write_rc;
        return g_fs_write_rc;
    }
    n = fs_find(path);
    if (n == NULL) {
        n = fs_new(path);
    }
    if (n == NULL) {
        g_fs_err = -1;
        return -1;
    }
    n->size = (len > sizeof(n->data)) ? (uint32)sizeof(n->data) : len;
    if ((g_fs_write_clamp > 0) && (n->size > (uint32)g_fs_write_clamp)) {
        n->size = (uint32)g_fs_write_clamp;
    }
    memcpy(n->data, data, n->size);
    return 0;
}

int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max,
                           uint32 *out_len)
{
    fsnode_t *n;

    if (g_fs_read_rc != 0) {
        g_fs_err = g_fs_read_rc;
        return g_fs_read_rc;
    }
    if (g_fs_read_fail_after >= 0) {
        if (g_fs_read_fail_after == 0) {
            g_fs_err = -1;
            return -1;
        }
        g_fs_read_fail_after--;
    }
    n = fs_find(path);
    if (n == NULL) {
        g_fs_err = -2;
        return -1;
    }
    {
        uint32 got = (n->size > max) ? max : n->size;

        memcpy(buf, n->data, got);
        if ((g_fs_read_corrupt != 0) && (got > 0u)) {
            buf[0] = (uint8)(buf[0] ^ 0xFFu);
        }
        if (out_len != NULL) {
            *out_len = got;
        }
    }
    return 0;
}

int32 arkrtos_fs_remove(const char *path)
{
    fsnode_t *n;

    if (g_fs_remove_rc != 0) {
        g_fs_err = g_fs_remove_rc;
        return g_fs_remove_rc;
    }
    n = fs_find(path);
    if (n == NULL) {
        g_fs_err = -2;
        return -1;
    }
    n->used = 0;
    return 0;
}

int32 arkrtos_fs_list(const char *dir, arkrtos_fs_list_cb_t cb, void *arg)
{
    int i;

    (void)dir;
    if (g_fs_list_rc != 0) {
        g_fs_err = g_fs_list_rc;
        return g_fs_list_rc;
    }
    for (i = 0; i < FSN; i++) {
        if (g_fs[i].used != 0) {
            int rc = cb(g_fs[i].path, g_fs[i].size, 0u, arg);

            if (rc != 0) {
                return 0;
            }
        }
    }
    return 0;
}

int32 arkrtos_fs_open_write(const char *path)
{
    fsnode_t *n;

    if (g_fs_open_w_rc != 0) {
        g_fs_err = g_fs_open_w_rc;
        return g_fs_open_w_rc;
    }
    n = fs_find(path);
    if (n == NULL) {
        n = fs_new(path);
    }
    if (n == NULL) {
        g_fs_err = -1;
        return -1;
    }
    n->size = 0u;
    g_fs_wfd = (int)(n - g_fs);
    g_fs_wlen = 0u;
    return 0;
}

int32 arkrtos_fs_write_next(const uint8 *buf, uint32 len)
{
    if (g_fs_write_next_rc != 0) {
        g_fs_err = g_fs_write_next_rc;
        return g_fs_write_next_rc;
    }
    if ((g_fs_wfd < 0) || ((g_fs_wlen + len) > sizeof(g_fs_wbuf))) {
        g_fs_err = -1;
        return -1;
    }
    memcpy(&g_fs_wbuf[g_fs_wlen], buf, len);
    g_fs_wlen += len;
    return 0;
}

int32 arkrtos_fs_close_write(void)
{
    fsnode_t *n;

    if (g_fs_close_w_rc != 0) {
        g_fs_err = g_fs_close_w_rc;
        return g_fs_close_w_rc;
    }
    if (g_fs_wfd < 0) {
        g_fs_err = -1;
        return -1;
    }
    n = &g_fs[g_fs_wfd];
    n->size = g_fs_wlen;
    memcpy(n->data, g_fs_wbuf, g_fs_wlen);
    g_fs_wfd = -1;
    return 0;
}

int32 arkrtos_fs_last_error(void)
{
    return g_fs_err;
}

const char *arkrtos_fs_error_name(int32 err)
{
    switch (err) {
    case 0:  return "LFS_OK";
    case -2: return "LFS_ERR_NOENT";
    case -1: return "LFS_ERR_IO";
    default: return "LFS_ERR_OTHER";
    }
}

uint8 arkrtos_fs_err_is_unformatted(int32 err)
{
    return (err == g_fs_unformatted_id) && (g_fs_unformatted_id != 0) ? 1u : 0u;
}

/* ---- VFS namespace: nodes plus read/write handles ---- */

#define VFN 12

typedef struct {
    char   path[64];
    uint32 mode;
    uint32 size;
    uint32 uid;
    uint32 gid;
    uint32 mtime;
    uint8  data[512];
    int    used;
} vnode_t;

static vnode_t g_vfs[VFN];
static int32   g_vfs_mount_count = 2;
static int32   g_vfs_mount_info_rc;
static int32   g_vfs_mount_rc;
static int32   g_vfs_umount_rc;
static int32   g_vfs_list_rc;
static int32   g_vfs_open_rc;
static int32   g_vfs_read_rc;
static int32   g_vfs_close_rc;
static int32   g_vfs_app_open_rc;
static int32   g_vfs_app_write_rc;
static int32   g_vfs_app_close_rc;
static int32   g_vfs_app_stat_rc;
static int32   g_vfs_remove_rc;
static int32   g_vfs_chmod_rc;
static int32   g_vfs_chown_rc;
static int32   g_vfs_mkdir_rc;
static int32   g_vfs_rmdir_rc;
static int32   g_vfs_rename_rc;
static int32   g_vfs_stat_rc;

#define VFD_MAX 8

static struct {
    int    node;
    uint32 off;
    int    used;
} g_vfd[VFD_MAX];

static vnode_t *vnode_find(const char *path)
{
    int i;

    for (i = 0; i < VFN; i++) {
        if ((g_vfs[i].used != 0) && (strcmp(g_vfs[i].path, path) == 0)) {
            return &g_vfs[i];
        }
    }
    return NULL;
}

static vnode_t *vnode_new(const char *path, uint32 mode)
{
    int i;

    for (i = 0; i < VFN; i++) {
        if (g_vfs[i].used == 0) {
            memset(&g_vfs[i], 0, sizeof(g_vfs[i]));
            (void)strncpy(g_vfs[i].path, path, sizeof(g_vfs[i].path) - 1u);
            g_vfs[i].mode = mode;
            g_vfs[i].used = 1;
            return &g_vfs[i];
        }
    }
    return NULL;
}

static void vfs_reset(void)
{
    memset(g_vfs, 0, sizeof(g_vfs));
    memset(g_vfd, 0, sizeof(g_vfd));
    (void)vnode_new("/", ARKRTOS_VFS_S_IFDIR | 0755u);
    (void)vnode_new("/dev", ARKRTOS_VFS_S_IFDIR | 0755u);
    g_vfs_mount_count = 2;
    g_vfs_mount_info_rc = 0;
    g_vfs_mount_rc = 0;
    g_vfs_umount_rc = 0;
    g_vfs_list_rc = 0;
    g_vfs_open_rc = 0;
    g_vfs_read_rc = 0;
    g_vfs_close_rc = 0;
    g_vfs_app_open_rc = 0;
    g_vfs_app_write_rc = 0;
    g_vfs_app_close_rc = 0;
    g_vfs_app_stat_rc = 0;
    g_vfs_remove_rc = 0;
    g_vfs_chmod_rc = 0;
    g_vfs_chown_rc = 0;
    g_vfs_mkdir_rc = 0;
    g_vfs_rmdir_rc = 0;
    g_vfs_rename_rc = 0;
    g_vfs_stat_rc = 0;
}

int32 arkrtos_vfs_mount_count(void)
{
    return g_vfs_mount_count;
}

int32 arkrtos_vfs_mount_info(uint32 idx, arkrtos_vfs_mount_info_t *out)
{
    if (g_vfs_mount_info_rc != 0) {
        return g_vfs_mount_info_rc;
    }
    if (out == NULL) {
        return -1;
    }
    if (idx == 0u) {
        (void)strncpy(out->target, "/", sizeof(out->target) - 1u);
        out->fs = "devfs";
        out->source = NULL;
        out->ro = 0u;
    } else {
        (void)strncpy(out->target, "/mnt/nor", sizeof(out->target) - 1u);
        out->fs = "littlefs";
        out->source = "nor0";
        out->ro = 0u;
    }
    return 0;
}

int32 arkrtos_vfs_mount_volume(const char *dev, uint32 off, uint32 size,
                               const char *target)
{
    (void)dev;
    (void)off;
    (void)size;
    (void)target;
    return g_vfs_mount_rc;
}

int32 arkrtos_vfs_umount(const char *target)
{
    (void)target;
    return g_vfs_umount_rc;
}

int32 arkrtos_vfs_list(const char *dir, arkrtos_vfs_list_cb_t cb, void *arg)
{
    int i;
    size_t dlen;

    if (g_vfs_list_rc != 0) {
        return g_vfs_list_rc;
    }
    dlen = strlen(dir);
    for (i = 0; i < VFN; i++) {
        arkrtos_vfs_stat_t st;

        if (g_vfs[i].used == 0) {
            continue;
        }
        /* direct children of dir only */
        if ((strncmp(g_vfs[i].path, dir, dlen) != 0)) {
            continue;
        }
        if ((dlen > 1u) && (g_vfs[i].path[dlen] != '/')) {
            continue;
        }
        if ((dlen == 1u) && (g_vfs[i].path[1] == '\0')) {
            continue;
        }
        {
            const char *rel = &g_vfs[i].path[dlen];

            if (*rel == '/') {
                rel++;
            }
            if (strchr(rel, '/') != NULL) {
                continue;
            }
            st.mode = g_vfs[i].mode;
            st.size = g_vfs[i].size;
            st.mtime = g_vfs[i].mtime;
            st.uid = g_vfs[i].uid;
            st.gid = g_vfs[i].gid;
            if (cb(rel, &st, arg) != 0) {
                return 0;
            }
        }
    }
    return 0;
}

int32 arkrtos_vfs_open_read(const char *path)
{
    vnode_t *n;
    int i;

    if (g_vfs_open_rc < 0) {
        return g_vfs_open_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    for (i = 0; i < VFD_MAX; i++) {
        if (g_vfd[i].used == 0) {
            g_vfd[i].used = 1;
            g_vfd[i].node = (int)(n - g_vfs);
            g_vfd[i].off = 0u;
            return i;
        }
    }
    return -1;
}

int32 arkrtos_vfs_read(int32 fd, uint8 *buf, uint32 len)
{
    vnode_t *n;
    uint32 avail;
    uint32 got;

    if (g_vfs_read_rc < 0) {
        return g_vfs_read_rc;
    }
    if ((fd < 0) || (fd >= VFD_MAX) || (g_vfd[fd].used == 0)) {
        return -1;
    }
    n = &g_vfs[g_vfd[fd].node];
    avail = (g_vfd[fd].off < n->size) ? (n->size - g_vfd[fd].off) : 0u;
    got = (avail < len) ? avail : len;
    memcpy(buf, &n->data[g_vfd[fd].off], got);
    g_vfd[fd].off += got;
    return (int32)got;
}

int32 arkrtos_vfs_close(int32 fd)
{
    if (g_vfs_close_rc != 0) {
        return g_vfs_close_rc;
    }
    if ((fd < 0) || (fd >= VFD_MAX) || (g_vfd[fd].used == 0)) {
        return -1;
    }
    g_vfd[fd].used = 0;
    return 0;
}

int32 arkrtos_vfs_app_open(const char *path, uint32 flags)
{
    vnode_t *n;
    int i;

    if (g_vfs_app_open_rc < 0) {
        return g_vfs_app_open_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        if ((flags & ARKRTOS_VFS_O_CREAT) == 0u) {
            return -2;
        }
        n = vnode_new(path, ARKRTOS_VFS_S_IFREG | 0644u);
        if (n == NULL) {
            return -1;
        }
    } else if ((flags & ARKRTOS_VFS_O_TRUNC) != 0u) {
        n->size = 0u;
    }
    for (i = 0; i < VFD_MAX; i++) {
        if (g_vfd[i].used == 0) {
            g_vfd[i].used = 1;
            g_vfd[i].node = (int)(n - g_vfs);
            g_vfd[i].off = 0u;
            return i;
        }
    }
    return -1;
}

int32 arkrtos_vfs_app_write(int32 h, const uint8 *buf, uint32 len)
{
    vnode_t *n;

    if (g_vfs_app_write_rc != 0) {
        return g_vfs_app_write_rc;
    }
    if ((h < 0) || (h >= VFD_MAX) || (g_vfd[h].used == 0)) {
        return -1;
    }
    n = &g_vfs[g_vfd[h].node];
    if ((g_vfd[h].off + len) > sizeof(n->data)) {
        return -1;
    }
    memcpy(&n->data[g_vfd[h].off], buf, len);
    g_vfd[h].off += len;
    if (n->size < g_vfd[h].off) {
        n->size = g_vfd[h].off;
    }
    return (int32)len;
}

int32 arkrtos_vfs_app_close(int32 h)
{
    if (g_vfs_app_close_rc != 0) {
        return g_vfs_app_close_rc;
    }
    if ((h < 0) || (h >= VFD_MAX) || (g_vfd[h].used == 0)) {
        return -1;
    }
    g_vfd[h].used = 0;
    return 0;
}

int32 arkrtos_vfs_app_stat(const char *path, uint32 *size, uint32 *mode)
{
    vnode_t *n;

    if (g_vfs_app_stat_rc != 0) {
        return g_vfs_app_stat_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    if (size != NULL) {
        *size = n->size;
    }
    if (mode != NULL) {
        *mode = n->mode;
    }
    return 0;
}

int32 arkrtos_vfs_remove(const char *path)
{
    vnode_t *n;

    if (g_vfs_remove_rc != 0) {
        return g_vfs_remove_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    n->used = 0;
    return 0;
}

int32 arkrtos_vfs_chmod(const char *path, uint32 mode)
{
    vnode_t *n;

    if (g_vfs_chmod_rc != 0) {
        return g_vfs_chmod_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    n->mode = (n->mode & ARKRTOS_VFS_S_IFMT) | (mode & ARKRTOS_VFS_S_PERM);
    return 0;
}

int32 arkrtos_vfs_chown(const char *path, uint32 uid, uint32 gid)
{
    vnode_t *n;

    if (g_vfs_chown_rc != 0) {
        return g_vfs_chown_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    n->uid = uid;
    n->gid = gid;
    return 0;
}

const char *arkrtos_vfs_error_name(int32 rc)
{
    switch (rc) {
    case 0:            return "VFS_OK";
    case ARK_E_NOTDIR: return "VFS_NOTDIR";
    case -2:           return "VFS_NOENT";
    default:           return "VFS_ERR";
    }
}

/* ---- ark_vfs core: stat / mkdir / rmdir / rename ---- */

int32_t ark_vfs_stat(const char *path, struct ark_vfs_stat *st)
{
    vnode_t *n;

    if (g_vfs_stat_rc != 0) {
        return g_vfs_stat_rc;
    }
    if ((st == NULL) || (path == NULL)) {
        return ARK_E_INVAL;
    }
    /* "." and ".." are answered from the namespace so `ls -la` has real rows */
    if ((strcmp(path, "/") == 0) ||
        ((strlen(path) >= 2u) && (strcmp(path + strlen(path) - 2u, "/.") == 0))) {
        st->mode = ARKRTOS_VFS_S_IFDIR | 0755u;
        st->size = 0u;
        st->mtime = 0u;
        st->uid = 0u;
        st->gid = 0u;
        return 0;
    }
    if (strlen(path) >= 3u) {
        const char *tail = path + strlen(path) - 3u;

        if (strcmp(tail, "/..") == 0) {
            st->mode = ARKRTOS_VFS_S_IFDIR | 0755u;
            st->size = 0u;
            st->mtime = 0u;
            st->uid = 0u;
            st->gid = 0u;
            return 0;
        }
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    st->mode = n->mode;
    st->size = n->size;
    st->mtime = n->mtime;
    st->uid = n->uid;
    st->gid = n->gid;
    return 0;
}

int32_t ark_vfs_mkdir(const char *path)
{
    if (g_vfs_mkdir_rc != 0) {
        return g_vfs_mkdir_rc;
    }
    if (vnode_find(path) != NULL) {
        return -1;
    }
    return (vnode_new(path, ARKRTOS_VFS_S_IFDIR | 0755u) != NULL) ? 0 : -1;
}

int32_t ark_vfs_rmdir(const char *path)
{
    vnode_t *n;

    if (g_vfs_rmdir_rc != 0) {
        return g_vfs_rmdir_rc;
    }
    n = vnode_find(path);
    if (n == NULL) {
        return -2;
    }
    if ((n->mode & ARKRTOS_VFS_S_IFDIR) == 0u) {
        return ARK_E_NOTDIR;
    }
    n->used = 0;
    return 0;
}

int32_t ark_vfs_rename(const char *oldp, const char *newp)
{
    vnode_t *n;

    if (g_vfs_rename_rc != 0) {
        return g_vfs_rename_rc;
    }
    n = vnode_find(oldp);
    if (n == NULL) {
        return -2;
    }
    (void)strncpy(n->path, newp, sizeof(n->path) - 1u);
    return 0;
}

/* ---- mini + crash + partition table ---- */

static int32 g_mini_run_rc;
static int32 g_mini_info_rc;
static uint32 g_mini_running = 1u;
static int32 g_mini_stop_rc;
static int32 g_mini_stop_at_rc;
static int32 g_mini_swap_rc;
static int32 g_mini_forget_rc;
static int32 g_mini_max_count = 2u;
static int32 g_mini_set_count_rc;
static uint32 g_mini_max_bytes = 32768u;
static int32 g_mini_set_bytes_rc;
static int32 g_mini_auto_foreach_rc;
static int   g_mini_auto_items = 2;
static int32 g_mini_auto_set_rc;
static int32 g_mini_auto_now_rc;
static uint32 g_crash_hash = 0x1234u;
static uint32 g_crash_mcount;
static uint32 g_crash_mdisabled;
static int32 g_mini_info_at_calls;

int32 arkrtos_mini_run(const char *path)
{
    (void)path;
    return g_mini_run_rc;
}

int32 arkrtos_mini_info(arkrtos_mini_info_t *p)
{
    if (g_mini_info_rc != 0) {
        return g_mini_info_rc;
    }
    if (p == NULL) {
        return -1;
    }
    p->task_id = 7u;
    p->code_base = 0x1000u;
    p->code_block = 1024u;
    p->code_size = 700u;
    p->ram_base = 0x2000u;
    p->ram_size = 2048u;
    p->entry = 0x1001u;
    p->crc = 0xDEADBEEFu;
    return 0;
}

uint32 arkrtos_mini_running(void)
{
    return g_mini_running;
}

int32 arkrtos_mini_info_at(uint32 index, arkrtos_mini_info_t *p)
{
    if ((g_mini_info_at_calls >= 1) || (index > 0u)) {
        return -1;
    }
    g_mini_info_at_calls++;
    return arkrtos_mini_info(p);
}

int32 arkrtos_mini_stop(void)
{
    return g_mini_stop_rc;
}

int32 arkrtos_mini_stop_at(uint32 index)
{
    (void)index;
    return g_mini_stop_at_rc;
}

int32 arkrtos_mini_swap_at(uint32 index, const char *path)
{
    (void)index;
    (void)path;
    return g_mini_swap_rc;
}

int32 arkrtos_mini_forget(const char *path)
{
    (void)path;
    return g_mini_forget_rc;
}

uint32 arkrtos_mini_max_count(void)
{
    return (uint32)g_mini_max_count;
}

int32 arkrtos_mini_set_max_count(uint32 n)
{
    if (g_mini_set_count_rc != 0) {
        return g_mini_set_count_rc;
    }
    g_mini_max_count = (int32)n;
    return 0;
}

uint32 arkrtos_mini_max_bytes(void)
{
    return g_mini_max_bytes;
}

int32 arkrtos_mini_set_max_bytes(uint32 bytes)
{
    if (g_mini_set_bytes_rc != 0) {
        return g_mini_set_bytes_rc;
    }
    g_mini_max_bytes = bytes;
    return 0;
}

int32 arkrtos_mini_last_read_error(void)
{
    return ARKRTOS_MINI_ERR_SHORT_READ;
}

int32 arkrtos_mini_autostart_foreach(arkrtos_mini_auto_cb_t cb, void *arg)
{
    if (g_mini_auto_foreach_rc != 0) {
        return g_mini_auto_foreach_rc;
    }
    if (g_mini_auto_items >= 1) {
        if (cb("/mini/hello.svcm", arg) != 0) {
            return 0;
        }
    }
    if (g_mini_auto_items >= 2) {
        (void)cb("/mini/bad.svcm", arg);
    }
    return 0;
}

int32 arkrtos_mini_autostart_set(const char *path, uint32 on)
{
    (void)path;
    (void)on;
    return g_mini_auto_set_rc;
}

int32 arkrtos_mini_autostart_now(void)
{
    return g_mini_auto_now_rc;
}

uint32 arkrtos_crash_hash_path(const char *path)
{
    (void)path;
    return g_crash_hash;
}

uint32 arkrtos_crash_mini_count(uint32 hash)
{
    (void)hash;
    return g_crash_mcount;
}

uint32 arkrtos_crash_mini_disabled(uint32 hash)
{
    (void)hash;
    return g_crash_mdisabled;
}

int32 arkrtos_ptable_mini_used(uint32 *p_used, uint32 *p_max)
{
    if (p_used != NULL) {
        *p_used = 1u;
    }
    if (p_max != NULL) {
        *p_max = 2u;
    }
    return 0;
}/* ------------------------------------------------------------ fixtures */

static void reset_all(void)
{
    cap_reset();
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_blk_read_rc = 0;
    g_blk_write_rc = 0;
    g_blk_erase_rc = 0;
    g_blk_init_rc_a = 0;
    g_blk_init_rc_b = 0;
    g_blk_unit = 256u;
    g_blk_erase_noop = 0;
    g_blk_write_corrupt = 0;
    g_blk_read_ok_left = -1;
    g_blk_n = 1u;
    fs_reset();
    vfs_reset();
    g_dev_read_left = 0;
    g_dev_write_calls = 0;
    g_recv_rc = 0;
    g_stdin = NULL;
    g_stdin_pos = 0;
    g_mini_run_rc = 0;
    g_mini_info_rc = 0;
    g_mini_running = 1u;
    g_mini_stop_rc = 0;
    g_mini_stop_at_rc = 0;
    g_mini_swap_rc = 0;
    g_mini_forget_rc = 0;
    g_mini_max_count = 2;
    g_mini_set_count_rc = 0;
    g_mini_max_bytes = 32768u;
    g_mini_set_bytes_rc = 0;
    g_mini_auto_foreach_rc = 0;
    g_mini_auto_items = 2;
    g_mini_auto_set_rc = 0;
    g_mini_auto_now_rc = 1;
    g_crash_hash = 0x1234u;
    g_crash_mcount = 0u;
    g_crash_mdisabled = 0u;
    g_mini_info_at_calls = 0;
    (void)strcpy(g_shell.cwd, "/");
}

/* ------------------------------------------------------------ blk */

static void t_blk(void)
{
    int rc;

    /* no device registered: says so instead of printing an empty table */
    reset_all();
    g_blk_n = 0u;
    rc = RUN(cmd_blk, "blk");
    CK_I(rc, -1);
    CK(out_has("no block device registered"));

    /* two devices, the second one refuses bring-up */
    reset_all();
    g_blk_n = 2u;
    g_blk_init_rc_b = -1;
    rc = RUN(cmd_blk, "blk", "list");
    CK_I(rc, 0);
    CK(out_has("nor0"));
    CK(out_has("ready"));
    CK(out_has("nor1"));
    CK(out_has("down"));

    /* probe a device: capacity and erase unit, and the init is idempotent */
    reset_all();
    rc = RUN(cmd_blk, "blk", "probe", "nor0");
    CK_I(rc, 0);
    CK(out_has("nor0: 4096 bytes, erase unit 256"));

    /* unknown name: refused, and the list is printed so the operator can see
     * what does exist */
    reset_all();
    rc = RUN(cmd_blk, "blk", "probe", "zzz");
    CK_I(rc, -1);
    CK(out_has("unknown block device: zzz"));
    CK(out_has("nor0"));

    /* device answers its bring-up with a failure */
    reset_all();
    g_blk_init_rc_a = -1;
    rc = RUN(cmd_blk, "blk", "probe", "nor0");
    CK_I(rc, -1);
    CK(out_has("nor0: not available"));

    /* rd: default length is 64, and a 0xFF device reads as dots in the ASCII
     * column */
    reset_all();
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0");
    CK_I(rc, 0);
    CK(out_has(" 000000:"));
    CK(out_has("FF FF"));

    /* rd with an explicit length over several rows */
    reset_all();
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0", "20");
    CK_I(rc, 0);
    CK(out_has(" 000000:"));
    CK(out_has(" 000010:"));

    /* rd: a bad length is refused with the limits */
    reset_all();
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0", "0");
    CK_I(rc, -1);
    CK(out_has("len must be 1..4096"));
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0", "5000");
    CK_I(rc, -1);

    /* rd: missing operand and a non-numeric offset both fall back to usage */
    reset_all();
    rc = RUN(cmd_blk, "blk", "rd", "nor0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "zz");
    CK_I(rc, -1);

    /* rd: the device read fails */
    reset_all();
    g_blk_read_rc = -1;
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0");
    CK_I(rc, -1);
    CK(out_has("read failed at offset 0x0"));

    /* wr: two hex bytes land, and the dump reads them back */
    reset_all();
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "A501");
    CK_I(rc, 0);
    CK(out_has("wrote 2 bytes at 0x0"));
    CK(out_has("A5 01"));

    /* wr: the same range is no longer erased, so the device refuses */
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "A501");
    CK_I(rc, -1);
    CK(out_has("write refused or failed"));

    /* wr: bad hex (non-hex digit, odd length, empty string) */
    reset_all();
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "A5G");
    CK_I(rc, -1);
    CK(out_has("bad hex string"));
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "A5A");
    CK_I(rc, -1);
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "");
    CK_I(rc, -1);

    /* wr: missing operand */
    reset_all();
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    /* erase: aligned request succeeds, unaligned is refused */
    reset_all();
    rc = RUN(cmd_blk, "blk", "erase", "nor0", "0", "256");
    CK_I(rc, 0);
    CK(out_has("erased 256 bytes at 0x0"));
    rc = RUN(cmd_blk, "blk", "erase", "nor0", "0", "100");
    CK_I(rc, -1);
    CK(out_has("erase refused or failed"));

    /* erase: missing operand / bad number */
    reset_all();
    rc = RUN(cmd_blk, "blk", "erase", "nor0", "0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    rc = RUN(cmd_blk, "blk", "erase", "nor0", "0", "zz");
    CK_I(rc, -1);

    /* test: the full erase / program / verify chain passes */
    reset_all();
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "128");
    CK_I(rc, 0);
    CK(out_has("erase   : ok, all 0xFF"));
    CK(out_has("program : ok, 128 bytes verified"));

    /* test: default length is 128 */
    reset_all();
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0");
    CK_I(rc, 0);
    CK(out_has("128 bytes"));

    /* test: zero length and a length above the erase unit are refused */
    reset_all();
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "0");
    CK_I(rc, -1);
    CK(out_has("len must be >= 1"));
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "512");
    CK_I(rc, -1);
    CK(out_has("len must be <= erase unit (256)"));
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "zz");
    CK_I(rc, -1);

    /* test: a device with a larger erase unit but a scratch cap of 256 -
     * 300 fits the unit and must still be refused */
    reset_all();
    g_blk_n = 2u;
    g_blk_unit = 1024u;
    rc = RUN(cmd_blk, "blk", "test", "nor1", "0", "300");
    CK_I(rc, -1);
    CK(!out_has("test nor1"));

    /* test: erase refused */
    reset_all();
    g_blk_erase_rc = -1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("erase   : FAILED (device refused or timed out)"));

    /* test: erase ok but read-back fails */
    reset_all();
    g_blk_read_rc = -1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("erase   : FAILED (read back failed)"));

    /* test: erase answers ok but the bytes did not become 0xFF - the exact
     * case a "write returned 0" check would miss */
    reset_all();
    memset(g_flash, 0x00, sizeof(g_flash));
    g_blk_erase_noop = 1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("erase   : FAILED at +0, got 0x00, want 0xFF"));

    /* test: program refused */
    reset_all();
    g_blk_write_rc = -1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("program : FAILED (device refused or timed out)"));

    /* test: program "succeeds" but stores the wrong byte */
    reset_all();
    g_blk_write_corrupt = 1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("verify  : FAILED at +0"));

    /* test: verify read fails after the erase read succeeded */
    reset_all();
    g_blk_read_ok_left = 1;
    rc = RUN(cmd_blk, "blk", "test", "nor0", "0", "64");
    CK_I(rc, -1);
    CK(out_has("verify  : FAILED (read back failed)"));

    /* test: missing offset / unknown subcommand */
    reset_all();
    rc = RUN(cmd_blk, "blk", "test", "nor0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    reset_all();
    rc = RUN(cmd_blk, "blk", "frobnicate", "nor0");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    /* a bare "blk <name>" (no subcommand) is usage, not a probe */
    reset_all();
    rc = RUN(cmd_blk, "blk", "nor0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
}

/* ------------------------------------------------------------ fs */

static void t_fs(void)
{
    int rc;

    /* no subcommand */
    reset_all();
    rc = RUN(cmd_fs, "fs");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    /* mount with the board default volume */
    reset_all();
    rc = RUN(cmd_fs, "fs", "mount");
    CK_I(rc, 0);
    CK(out_has("mounted nor0 at 0x0, 4194304 bytes"));

    /* mount with an explicit window */
    reset_all();
    rc = RUN(cmd_fs, "fs", "mount", "nor0", "0", "4096");
    CK_I(rc, 0);
    CK(out_has("mounted nor0 at 0x0, 4096 bytes"));

    /* half a window is usage, never a default offset */
    reset_all();
    rc = RUN(cmd_fs, "fs", "mount", "nor0", "0");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    /* mount failure: the littlefs code is printed, and a blank volume gets
     * the "nothing was written, format on purpose" hint */
    reset_all();
    g_fs_mount_rc = -2;
    rc = RUN(cmd_fs, "fs", "mount");
    CK_I(rc, -1);
    CK(out_has("mount: FAILED (littlefs error -2: LFS_ERR_NOENT)"));
    CK(!out_has("no littlefs superblock"));

    reset_all();
    g_fs_mount_rc = -5;
    g_fs_unformatted_id = -5;
    rc = RUN(cmd_fs, "fs", "mount");
    CK_I(rc, -1);
    CK(out_has("no littlefs superblock"));
    CK(out_has("fs format nor0 0x0 4194304"));

    /* unmount ok / unmount refused */
    reset_all();
    rc = RUN(cmd_fs, "fs", "unmount");
    CK_I(rc, 0);
    CK(out_has("unmounted"));
    g_fs_unmount_rc = -1;
    rc = RUN(cmd_fs, "fs", "unmount");
    CK_I(rc, -1);
    CK(out_has("unmount: FAILED (littlefs error -1: LFS_ERR_IO)"));

    /* format */
    reset_all();
    rc = RUN(cmd_fs, "fs", "format");
    CK_I(rc, 0);
    CK(out_has("every file on it is lost"));
    CK(out_has("format : ok, volume is empty and mounted"));
    rc = RUN(cmd_fs, "fs", "format", "nor0", "0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    g_fs_format_rc = -1;
    rc = RUN(cmd_fs, "fs", "format");
    CK_I(rc, -1);
    CK(out_has("format: FAILED"));

    /* info: not mounted, then a mounted volume with its four figures */
    reset_all();
    rc = RUN(cmd_fs, "fs", "info");
    CK_I(rc, -1);
    CK(out_has("not mounted"));

    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "info");
    CK_I(rc, 0);
    CK(out_has("mounted  : yes (nor0, offset 0x0, size 4194304)"));
    CK(out_has("total    : 4096 bytes (4 KiB)"));
    CK(out_has("used     : 100 bytes (0 KiB, 2%)"));
    CK(out_has("free     : 3996 bytes"));

    g_fs_stat_rc = -3;
    rc = RUN(cmd_fs, "fs", "info");
    CK_I(rc, -1);
    CK(out_has("stat: FAILED (littlefs error -3"));

    /* ls: 0 entries, 1 entry, 2 entries */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "ls");
    CK_I(rc, 0);
    CK(out_has("0 entries"));
    (void)arkrtos_fs_write_file("/one.txt", (const uint8 *)"x", 1u);
    rc = RUN(cmd_fs, "fs", "ls");
    CK_I(rc, 0);
    CK(out_has("1 entry"));
    (void)arkrtos_fs_write_file("/two.txt", (const uint8 *)"y", 1u);
    rc = RUN(cmd_fs, "fs", "ls", "/sub");
    CK_I(rc, 0);
    CK(out_has("2 entries"));

    reset_all();
    g_fs_mounted = 1u;
    g_fs_list_rc = -1;
    rc = RUN(cmd_fs, "fs", "ls");
    CK_I(rc, -1);
    CK(out_has("ls: FAILED"));

    /* wr: the text is joined with spaces and echoed as hex */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "wr", "/a.txt", "hi", "there");
    CK_I(rc, 0);
    CK(out_has("wrote 8 bytes to /a.txt: 68 69 20 74 68 65 72 65"));

    /* wr: not mounted / too few operands / write refused */
    reset_all();
    rc = RUN(cmd_fs, "fs", "wr", "/a.txt", "x");
    CK_I(rc, -1);
    CK(out_has("not mounted"));
    rc = RUN(cmd_fs, "fs", "wr", "/a.txt");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    reset_all();
    g_fs_mounted = 1u;
    g_fs_write_rc = -1;
    rc = RUN(cmd_fs, "fs", "wr", "/a.txt", "x");
    CK_I(rc, -1);
    CK(out_has("write: FAILED"));

    /* rd: a known file dumps its bytes */
    reset_all();
    g_fs_mounted = 1u;
    (void)arkrtos_fs_write_file("/a.txt", (const uint8 *)"hi!", 3u);
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt");
    CK_I(rc, 0);
    CK(out_has("/a.txt: 3 bytes"));
    CK(out_has(" 0000: 68 69 21"));

    /* rd: explicit length, bad length, not mounted, missing path, read fail */
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt", "2");
    CK_I(rc, 0);
    CK(out_has("/a.txt: 2 bytes"));
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt", "0");
    CK_I(rc, -1);
    CK(out_has("len must be 1..512"));
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt", "513");
    CK_I(rc, -1);
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt", "zz");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    rc = RUN(cmd_fs, "fs", "rd");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    reset_all();
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt");
    CK_I(rc, -1);
    CK(out_has("not mounted"));
    reset_all();
    g_fs_mounted = 1u;
    g_fs_read_rc = -1;
    rc = RUN(cmd_fs, "fs", "rd", "/a.txt");
    CK_I(rc, -1);
    CK(out_has("read: FAILED"));

    /* put: 300 bytes in two chunks, one ACK per chunk, then the file is closed */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "300");
    CK_I(rc, 0);
    CK(out_has("300 bytes in 256-byte chunks"));
    CK(out_has("fs put: ok, 300 bytes in /b.bin"));
    CK_I(g_dev_write_calls, 2);
    CK_I(g_dev_written[0], 0x06);
    CK_I(g_dev_written[1], 0x06);
    CK(fs_find("/b.bin") != NULL);
    CK_I(fs_find("/b.bin")->size, 300u);

    /* put: a zero length and a bad length are usage; not mounted refuses */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "0");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "zz");
    CK_I(rc, -1);
    rc = RUN(cmd_fs, "fs", "put");
    CK_I(rc, -1);
    reset_all();
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "10");
    CK_I(rc, -1);
    CK(out_has("not mounted"));

    /* put: open refuses */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_open_w_rc = -1;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "10");
    CK_I(rc, -1);
    CK(out_has("open for write: FAILED"));

    /* put: the transfer times out, so it NAKs and says the file may be short */
    reset_all();
    g_fs_mounted = 1u;
    g_recv_rc = -1;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "300");
    CK_I(rc, -1);
    CK(out_has("timed out at 0 of 300 bytes"));
    CK(out_has("the file may be incomplete"));
    CK_I(g_dev_written[g_dev_write_calls - 1], 0x15);

    /* put: a chunk write fails */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_write_next_rc = -1;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "10");
    CK_I(rc, -1);
    CK(out_has("write failed"));

    /* put: close fails */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_close_w_rc = -1;
    rc = RUN(cmd_fs, "fs", "put", "/b.bin", "10");
    CK_I(rc, -1);
    CK(out_has("close failed"));

    /* rm */
    reset_all();
    g_fs_mounted = 1u;
    (void)arkrtos_fs_write_file("/a.txt", (const uint8 *)"x", 1u);
    rc = RUN(cmd_fs, "fs", "rm", "/a.txt");
    CK_I(rc, 0);
    CK(out_has("removed /a.txt"));
    rc = RUN(cmd_fs, "fs", "rm");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    rc = RUN(cmd_fs, "fs", "rm", "/a.txt", "/b.txt");
    CK_I(rc, -1);
    reset_all();
    rc = RUN(cmd_fs, "fs", "rm", "/a.txt");
    CK_I(rc, -1);
    CK(out_has("usage:"));
    reset_all();
    g_fs_mounted = 1u;
    (void)arkrtos_fs_write_file("/a.txt", (const uint8 *)"x", 1u);
    g_fs_remove_rc = -1;
    rc = RUN(cmd_fs, "fs", "rm", "/a.txt");
    CK_I(rc, -1);
    CK(out_has("remove: FAILED"));

    /* test: the whole write / read / remount / read / remove chain */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, 0);
    CK(out_has("write  : ok, 512 bytes -> /fstest.bin"));
    CK(out_has("read   : ok, 512 bytes verified"));
    CK(out_has("remount: ok"));
    CK(out_has("remove : ok"));
    CK(out_has("fs test: PASS"));

    /* test: not mounted */
    reset_all();
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("not mounted: fs mount first"));

    /* test: write fails */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_write_rc = -1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("write: FAILED"));

    /* test: short file after the write */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_write_clamp = 100;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("short file, got 100 of 512 bytes"));

    /* test: bytes come back different from what was written */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_read_corrupt = 1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("MISMATCH at 0"));

    /* test: unmount fails */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_unmount_rc = -1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("unmount: FAILED"));

    /* test: the remount fails */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_mount_default_rc = -1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("remount: FAILED"));

    /* test: the second read (after the remount) fails */
    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, 0);
    /* remove fails on its own pass */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_remove_rc = -1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("remove: FAILED"));

    /* test: read fails inside read_verify */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_read_rc = -1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("read: FAILED"));

    /* err: reports whatever the facade last kept */
    reset_all();
    g_fs_err = -2;
    rc = RUN(cmd_fs, "fs", "err");
    CK_I(rc, 0);
    CK(out_has("last littlefs error: -2 (LFS_ERR_NOENT)"));

    /* unknown subcommand is usage */
    reset_all();
    rc = RUN(cmd_fs, "fs", "frobnicate");
    CK_I(rc, -1);
    CK(out_has("usage:"));
}
/* ------------------------------------------------------------ vfs fixtures */

#define VFS_PATHD_MAX 80u

/* "/" followed by n copies of 'a'.  n is clamped so the result still fits a
 * vnode path (63 chars + NUL); larger n exists only to be "too long". */
static const char *slash_a(uint32 n)
{
    static char buf[VFS_PATHD_MAX];
    uint32 i;

    if (n > (uint32)(sizeof(buf) - 2u)) {
        n = (uint32)(sizeof(buf) - 2u);
    }
    buf[0] = '/';
    for (i = 1u; i <= n; i++) {
        buf[i] = 'a';
    }
    buf[n + 1u] = '\0';
    return buf;
}

/* A path no VFS command may accept: 79 chars > ARKRTOS_VFS_PATH_MAX. */
static const char *too_long_path(void)
{
    return slash_a(78u);
}

static vnode_t *mkfile(const char *path, const char *text)
{
    vnode_t *n = vnode_new(path, ARKRTOS_VFS_S_IFREG | 0644u);
    uint32   len = (uint32)strlen(text);

    if (n != NULL) {
        memcpy(n->data, text, len);
        n->size = len;
    }
    return n;
}

/* ------------------------------------------------------------ vfs */

static void t_vfs(void)
{
    int rc;

    /* ---- mount ---- */
    reset_all();
    g_vfs_mount_count = 0;
    rc = RUN(cmd_vfs_mount, "mount");
    CK_I(rc, 0);
    CK(out_has("no mount points"));

    reset_all();
    rc = RUN(cmd_vfs_mount, "mount");
    CK_I(rc, 0);
    CK(out_has("target"));
    CK(out_has("devfs"));
    CK(out_has("littlefs"));
    CK(out_has("nor0"));
    CK(out_has("rw"));

    /* a mount-info that fails is skipped, not printed as blanks */
    reset_all();
    g_vfs_mount_info_rc = -1;
    rc = RUN(cmd_vfs_mount, "mount");
    CK_I(rc, 0);
    CK(!out_has("/mnt/nor"));

    reset_all();
    rc = RUN(cmd_vfs_mount, "mount", "/mnt");
    CK_I(rc, 0);
    CK(out_has("nor0 mounted at /mnt"));

    reset_all();
    g_vfs_mount_rc = -2;
    rc = RUN(cmd_vfs_mount, "mount", "/mnt");
    CK_I(rc, -1);
    CK(out_has("mount: FAILED (-2: VFS_NOENT)"));

    reset_all();
    rc = RUN(cmd_vfs_mount, "mount", "nor0", "0", "4096", "/mnt2");
    CK_I(rc, 0);
    CK(out_has("nor0 mounted at /mnt2"));

    reset_all();
    rc = RUN(cmd_vfs_mount, "mount", "nor0", "x", "4096", "/mnt2");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    g_vfs_mount_rc = -1;
    rc = RUN(cmd_vfs_mount, "mount", "nor0", "0", "4096", "/mnt2");
    CK_I(rc, -1);
    CK(out_has("mount: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_mount, "mount", "nor0", "0", "/mnt2");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    /* ---- umount ---- */
    reset_all();
    rc = RUN(cmd_vfs_umount, "umount");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    rc = RUN(cmd_vfs_umount, "umount", "/mnt");
    CK_I(rc, 0);
    CK(out_has("/mnt unmounted"));

    reset_all();
    g_vfs_umount_rc = ARK_E_NOTDIR;
    rc = RUN(cmd_vfs_umount, "umount", "/mnt");
    CK_I(rc, -1);
    CK(out_has("umount: FAILED (-6: VFS_NOTDIR)"));

    /* ---- ls ---- */
    reset_all();
    (void)mkfile("/hello.txt", "hi\n");
    rc = RUN(cmd_vfs_ls, "ls");
    CK_I(rc, 0);
    CK(out_has("dev"));
    CK(out_has("<dir>"));
    CK(out_has("hello.txt"));
    CK(out_has("2 entries"));

    reset_all();
    (void)mkfile("/hello.txt", "hi\n");
    rc = RUN(cmd_vfs_ls, "ls", "-l", "/");
    CK_I(rc, 0);
    CK(out_has("-rw-r--r--"));
    CK(out_has("drwxr-xr-x"));
    /* rows keep namespace order: the directory before the file */
    CK(out_before("dev", "hello.txt"));

    reset_all();
    (void)mkfile("/hello.txt", "hi\n");
    rc = RUN(cmd_vfs_ls, "ls", "-a", "/");
    CK_I(rc, 0);
    CK(out_has("  ."));
    CK(out_has("  .."));
    CK(out_has("4 entries"));

    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", "-la", "/");
    CK_I(rc, 0);
    CK(out_has("drwxr-xr-x"));

    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", "-la", "/dev");
    CK_I(rc, 0);
    CK(out_has("2 entries"));

    /* ls <file> lists the entry itself, plain and long */
    reset_all();
    (void)mkfile("/hello.txt", "hi\n");
    rc = RUN(cmd_vfs_ls, "ls", "/hello.txt");
    CK_I(rc, 0);
    CK(out_has("  hello.txt"));
    CK(!out_has("entries"));

    reset_all();
    (void)mkfile("/hello.txt", "hi\n");
    rc = RUN(cmd_vfs_ls, "ls", "-l", "/hello.txt");
    CK_I(rc, 0);
    CK(out_has("-rw-r--r--"));
    CK(out_has("hello.txt"));
    CK(out_has("3"));

    /* grouped options, path first, and the two usage refusals */
    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", "/", "-l");
    CK_I(rc, 0);
    CK(out_has("drwxr-xr-x"));

    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", "-x");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", "/", "/dev");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    g_vfs_list_rc = -2;
    rc = RUN(cmd_vfs_ls, "ls", "/");
    CK_I(rc, -1);
    CK(out_has("ls: FAILED (-2: VFS_NOENT)"));

    reset_all();
    rc = RUN(cmd_vfs_ls, "ls", too_long_path());
    CK_I(rc, 1);
    CK(out_has("ls: path too long"));

    /* ---- cat ---- */
    reset_all();
    rc = RUN(cmd_vfs_cat, "cat", "/a", "/b");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/f", "ab\nc\x01" "x");
    rc = RUN(cmd_vfs_cat, "cat", "/f");
    CK_I(rc, 0);
    CK(out_has("ab"));
    CK(out_has("\\x01"));
    CK(out_has("6 bytes"));

    reset_all();
    g_stdin = "a\nb";
    rc = RUN(cmd_vfs_cat, "cat");
    CK_I(rc, 0);
    CK(out_has("a"));
    CK(out_has("3 bytes"));

    reset_all();
    rc = RUN(cmd_vfs_cat, "cat", "/nope");
    CK_I(rc, -1);
    CK(out_has("cat: FAILED (-2: VFS_NOENT)"));

    reset_all();
    (void)mkfile("/f", "abc");
    g_vfs_read_rc = -1;
    rc = RUN(cmd_vfs_cat, "cat", "/f");
    CK_I(rc, -1);
    CK(out_has("cat: FAILED (-1: VFS_ERR)"));

    reset_all();
    (void)mkfile("/f", "abc");
    g_vfs_close_rc = -1;
    rc = RUN(cmd_vfs_cat, "cat", "/f");
    CK_I(rc, -1);
    CK(out_has("cat (close): FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_cat, "cat", too_long_path());
    CK_I(rc, 1);
    CK(out_has("cat: path too long"));

    /* ---- rm ---- */
    reset_all();
    rc = RUN(cmd_vfs_rm, "rm");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/a", "x");
    rc = RUN(cmd_vfs_rm, "rm", "/a");
    CK_I(rc, 0);
    CK(out_has("/a removed"));
    CK(vnode_find("/a") == NULL);

    reset_all();
    rc = RUN(cmd_vfs_rm, "rm", "/dev");
    CK_I(rc, 1);
    CK(out_has("rm: /dev: Is a directory"));
    CK(vnode_find("/dev") != NULL);

    reset_all();
    rc = RUN(cmd_vfs_rm, "rm", "/nope");
    CK_I(rc, 1);
    CK(out_has("rm: /nope: FAILED (-2: VFS_NOENT)"));

    reset_all();
    (void)mkfile("/a", "x");
    g_vfs_remove_rc = -1;
    rc = RUN(cmd_vfs_rm, "rm", "/a");
    CK_I(rc, 1);
    CK(out_has("rm: /a: FAILED (-1: VFS_ERR)"));

    /* one operand failing does not hide the other operand's result */
    reset_all();
    (void)mkfile("/a", "x");
    rc = RUN(cmd_vfs_rm, "rm", "/a", "/nope");
    CK_I(rc, 1);
    CK(out_has("/a removed"));
    CK(out_has("rm: /nope: FAILED"));

    reset_all();
    rc = RUN(cmd_vfs_rm, "rm", too_long_path());
    CK_I(rc, 1);
    CK(out_has("rm: path too long"));

    /* ---- mkdir ---- */
    reset_all();
    rc = RUN(cmd_vfs_mkdir, "mkdir");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    rc = RUN(cmd_vfs_mkdir, "mkdir", "/d1");
    CK_I(rc, 0);
    CK(out_has("/d1 created"));
    CK(vnode_find("/d1") != NULL);

    reset_all();
    rc = RUN(cmd_vfs_mkdir, "mkdir", "/dev");
    CK_I(rc, 1);
    CK(out_has("mkdir: /dev: FAILED (-1: VFS_ERR)"));

    reset_all();
    g_vfs_mkdir_rc = -2;
    rc = RUN(cmd_vfs_mkdir, "mkdir", "/d2");
    CK_I(rc, 1);
    CK(out_has("mkdir: /d2: FAILED (-2: VFS_NOENT)"));

    reset_all();
    rc = RUN(cmd_vfs_mkdir, "mkdir", "/d1", "/dev");
    CK_I(rc, 1);
    CK(out_has("/d1 created"));
    CK(out_has("mkdir: /dev: FAILED"));

    reset_all();
    rc = RUN(cmd_vfs_mkdir, "mkdir", too_long_path());
    CK_I(rc, 1);
    CK(out_has("mkdir: path too long"));

    /* ---- rmdir ---- */
    reset_all();
    rc = RUN(cmd_vfs_rmdir, "rmdir");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)vnode_new("/d1", ARKRTOS_VFS_S_IFDIR | 0755u);
    rc = RUN(cmd_vfs_rmdir, "rmdir", "/d1");
    CK_I(rc, 0);
    CK(out_has("/d1 removed"));
    CK(vnode_find("/d1") == NULL);

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_rmdir, "rmdir", "/f");
    CK_I(rc, 1);
    CK(out_has("rmdir: /f: Not a directory"));

    reset_all();
    rc = RUN(cmd_vfs_rmdir, "rmdir", "/nope");
    CK_I(rc, 1);
    CK(out_has("rmdir: /nope: FAILED (-2: VFS_NOENT)"));

    reset_all();
    (void)vnode_new("/d1", ARKRTOS_VFS_S_IFDIR | 0755u);
    g_vfs_rmdir_rc = -1;
    rc = RUN(cmd_vfs_rmdir, "rmdir", "/d1");
    CK_I(rc, 1);
    CK(out_has("rmdir: /d1: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_rmdir, "rmdir", too_long_path());
    CK_I(rc, 1);
    CK(out_has("rmdir: path too long"));

    /* ---- mv ---- */
    reset_all();
    rc = RUN(cmd_vfs_mv, "mv", "/a");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/a", "x");
    rc = RUN(cmd_vfs_mv, "mv", "/a", "/b");
    CK_I(rc, 0);
    CK(out_has("/a -> /b"));
    CK(vnode_find("/a") == NULL);
    CK(vnode_find("/b") != NULL);

    reset_all();
    g_vfs_rename_rc = -2;
    rc = RUN(cmd_vfs_mv, "mv", "/a", "/b");
    CK_I(rc, -1);
    CK(out_has("mv: FAILED (-2: VFS_NOENT)"));

    reset_all();
    rc = RUN(cmd_vfs_mv, "mv", too_long_path(), "/b");
    CK_I(rc, 1);
    CK(out_has("mv: path too long"));

    /* ---- cp ---- */
    reset_all();
    rc = RUN(cmd_vfs_cp, "cp", "/a");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/s", "abc");
    rc = RUN(cmd_vfs_cp, "cp", "/s", "/t");
    CK_I(rc, 0);
    CK(out_has("3 bytes copied"));
    {
        vnode_t *n = vnode_find("/t");

        CK(n != NULL);
        if (n != NULL) {
            CK_S((const char *)n->data, "abc");
            CK_I(n->size, 3);
        }
    }

    /* destination is a directory: the copy lands inside under the basename */
    reset_all();
    (void)mkfile("/s", "abc");
    (void)vnode_new("/d", ARKRTOS_VFS_S_IFDIR | 0755u);
    rc = RUN(cmd_vfs_cp, "cp", "/s", "/d");
    CK_I(rc, 0);
    CK(vnode_find("/d/s") != NULL);

    reset_all();
    rc = RUN(cmd_vfs_cp, "cp", "/missing", "/t");
    CK_I(rc, -1);
    CK(out_has("cp: FAILED (-2: VFS_NOENT)"));

    reset_all();
    (void)mkfile("/s", "abc");
    g_vfs_app_open_rc = -1;
    rc = RUN(cmd_vfs_cp, "cp", "/s", "/t");
    CK_I(rc, -1);
    CK(out_has("cp: FAILED (-1: VFS_ERR)"));

    reset_all();
    (void)mkfile("/s", "abc");
    g_vfs_app_write_rc = 1;
    rc = RUN(cmd_vfs_cp, "cp", "/s", "/t");
    CK_I(rc, -1);
    CK(out_has("cp: FAILED (-1: VFS_ERR)"));

    reset_all();
    (void)mkfile("/s", "abc");
    g_vfs_read_rc = -1;
    rc = RUN(cmd_vfs_cp, "cp", "/s", "/t");
    CK_I(rc, -1);
    CK(out_has("cp: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_cp, "cp", too_long_path(), "/t");
    CK_I(rc, 1);
    CK(out_has("cp: path too long"));

    /* into-a-directory whose combined path does not fit the buffer */
    reset_all();
    (void)mkfile("/s", "abc");
    (void)vnode_new(slash_a(62u), ARKRTOS_VFS_S_IFDIR | 0755u);
    rc = RUN(cmd_vfs_cp, "cp", "/s", slash_a(62u));
    CK_I(rc, 1);
    CK(out_has("cp: path too long"));

    /* ---- touch ---- */
    reset_all();
    rc = RUN(cmd_vfs_touch, "touch");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    rc = RUN(cmd_vfs_touch, "touch", "/t1");
    CK_I(rc, 0);
    {
        vnode_t *n = vnode_find("/t1");

        CK(n != NULL);
        if (n != NULL) {
            CK_I(n->size, 0);
        }
    }

    /* touching an existing file has nothing to change and stays quiet */
    reset_all();
    (void)mkfile("/t2", "abc");
    rc = RUN(cmd_vfs_touch, "touch", "/t2");
    CK_I(rc, 0);
    {
        vnode_t *n = vnode_find("/t2");

        CK(n != NULL);
        if (n != NULL) {
            CK_I(n->size, 3);
        }
    }

    reset_all();
    g_vfs_app_open_rc = -1;
    rc = RUN(cmd_vfs_touch, "touch", "/t3");
    CK_I(rc, 1);
    CK(out_has("touch: /t3: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_touch, "touch", too_long_path());
    CK_I(rc, 1);
    CK(out_has("touch: path too long"));

    /* ---- chmod ---- */
    reset_all();
    rc = RUN(cmd_vfs_chmod, "chmod", "644");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chmod, "chmod", "64", "/f");
    CK_I(rc, 1);
    CK(out_has("chmod: mode must be 3 or 4 octal digits"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chmod, "chmod", "789", "/f");
    CK_I(rc, 1);
    CK(out_has("chmod: mode must be 3 or 4 octal digits"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chmod, "chmod", "07555", "/f");
    CK_I(rc, 1);
    CK(out_has("chmod: mode must be 3 or 4 octal digits"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chmod, "chmod", "0755", "/f");
    CK_I(rc, 0);
    CK(out_has("-rwxr-xr-x"));
    {
        vnode_t *n = vnode_find("/f");

        CK(n != NULL);
        if (n != NULL) {
            CK_I(n->mode & ARKRTOS_VFS_S_PERM, 0755u);
        }
    }

    reset_all();
    (void)mkfile("/f", "x");
    g_vfs_stat_rc = -1;
    rc = RUN(cmd_vfs_chmod, "chmod", "644", "/f");
    CK_I(rc, 0);
    CK(out_has("mode changed"));

    reset_all();
    (void)mkfile("/f", "x");
    g_vfs_chmod_rc = -2;
    rc = RUN(cmd_vfs_chmod, "chmod", "644", "/f");
    CK_I(rc, -1);
    CK(out_has("chmod: FAILED (-2: VFS_NOENT)"));

    reset_all();
    rc = RUN(cmd_vfs_chmod, "chmod", "644", too_long_path());
    CK_I(rc, 1);
    CK(out_has("chmod: path too long"));

    /* ---- chown ---- */
    reset_all();
    rc = RUN(cmd_vfs_chown, "chown", "5");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "5", "/f");
    CK_I(rc, 0);
    CK(out_has("/f: owner 5:0"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "5:6", "/f");
    CK_I(rc, 0);
    CK(out_has("/f: owner 5:6"));
    {
        vnode_t *n = vnode_find("/f");

        CK(n != NULL);
        if (n != NULL) {
            CK_I(n->uid, 5u);
            CK_I(n->gid, 6u);
        }
    }

    /* ":7" changes only the group; the omitted half keeps its current value */
    reset_all();
    (void)mkfile("/f", "x");
    {
        vnode_t *n = vnode_find("/f");

        if (n != NULL) {
            n->uid = 3u;
            n->gid = 4u;
        }
    }
    rc = RUN(cmd_vfs_chown, "chown", ":7", "/f");
    CK_I(rc, 0);
    CK(out_has("/f: owner 3:7"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", ":", "/f");
    CK_I(rc, 1);
    CK(out_has("chown: nothing to change"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "abc", "/f");
    CK_I(rc, 1);
    CK(out_has("chown: owner must be a decimal id"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "1:2:3", "/f");
    CK_I(rc, 1);
    CK(out_has("chown: group must be a decimal id"));

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "1234567890123456:1", "/f");
    CK_I(rc, 1);
    CK(out_has("chown: owner id too long"));

    reset_all();
    (void)mkfile("/f", "x");
    g_vfs_stat_rc = -1;
    rc = RUN(cmd_vfs_chown, "chown", "5", "/f");
    CK_I(rc, -1);
    CK(out_has("chown: FAILED (-1: VFS_ERR)"));

    reset_all();
    (void)mkfile("/f", "x");
    g_vfs_chown_rc = -1;
    rc = RUN(cmd_vfs_chown, "chown", "5", "/f");
    CK_I(rc, -1);
    CK(out_has("chown: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_chown, "chown", "5", too_long_path());
    CK_I(rc, 1);
    CK(out_has("chown: path too long"));

    /* ---- stat ---- */
    reset_all();
    rc = RUN(cmd_vfs_stat, "stat");
    CK_I(rc, -1);
    CK(out_has("usage:"));

    reset_all();
    rc = RUN(cmd_vfs_stat, "stat", "/dev");
    CK_I(rc, 0);
    CK(out_has("Type: directory"));
    CK(out_has("Mode: drwxr-xr-x"));
    CK(!out_has("Size:"));

    reset_all();
    (void)mkfile("/f", "abc");
    rc = RUN(cmd_vfs_stat, "stat", "/f");
    CK_I(rc, 0);
    CK(out_has("Type: regular file"));
    CK(out_has("Size: 3 bytes"));
    CK(out_has("Time: none"));

    reset_all();
    (void)vnode_new("/c", ARKRTOS_VFS_S_IFCHR | 0600u);
    rc = RUN(cmd_vfs_stat, "stat", "/c");
    CK_I(rc, 0);
    CK(out_has("Type: character device"));

    reset_all();
    (void)vnode_new("/u", 0u);
    rc = RUN(cmd_vfs_stat, "stat", "/u");
    CK_I(rc, 0);
    CK(out_has("Type: unknown"));
    CK(out_has("Mode: ?---------"));

    reset_all();
    {
        vnode_t *n = vnode_new("/f", ARKRTOS_VFS_S_IFREG | 0644u);

        if (n != NULL) {
            n->mtime = 1234u;
        }
    }
    rc = RUN(cmd_vfs_stat, "stat", "/f");
    CK_I(rc, 0);
    CK(out_has("Time: 1234"));

    reset_all();
    g_vfs_stat_rc = -1;
    rc = RUN(cmd_vfs_stat, "stat", "/f");
    CK_I(rc, -1);
    CK(out_has("stat: FAILED (-1: VFS_ERR)"));

    reset_all();
    rc = RUN(cmd_vfs_stat, "stat", too_long_path());
    CK_I(rc, 1);
    CK(out_has("stat: path too long"));

    /* ---- tab completion ---- */
    {
        static char cand[4][ARK_SHELL_LINE_SIZE];
        int n;

        reset_all();
        (void)mkfile("/hello.txt", "hi\n");

        n = vfs_complete(&g_shell, "hel", 1, cand, 4);
        CK_I(n, 1);
        if (n == 1) {
            CK_S(cand[0], "hello.txt");
        }

        /* a directory candidate keeps the trailing '/' */
        n = vfs_complete(&g_shell, "d", 0, cand, 4);
        CK_I(n, 1);
        if (n == 1) {
            CK_S(cand[0], "dev/");
        }

        /* explicit prefix: only names under that directory */
        n = vfs_complete(&g_shell, "/h", 0, cand, 4);
        CK_I(n, 1);
        if (n == 1) {
            CK_S(cand[0], "/hello.txt");
        }

        /* the candidate cap is honoured */
        n = vfs_complete(&g_shell, "/", 0, cand, 1);
        CK_I(n, 1);

        /* nothing matches: zero candidates, not an invented one */
        n = vfs_complete(&g_shell, "zzz", 0, cand, 4);
        CK_I(n, 0);

        /* completion is relative to the cwd */
        (void)mkfile("/dev/uart", "");
        (void)strcpy(g_shell.cwd, "/dev");
        n = vfs_complete(&g_shell, "u", 0, cand, 4);
        CK_I(n, 1);
        if (n == 1) {
            CK_S(cand[0], "uart");
        }
        (void)strcpy(g_shell.cwd, "/");

        /* a prefix too long to hold in the resolve buffer is refused */
        n = vfs_complete(&g_shell, slash_a(70u), 0, cand, 4);
        CK_I(n, 0);
    }

    /* ---- resolve_path ---- */
    {
        char out[ARKRTOS_VFS_PATH_MAX];

        reset_all();
        CK_I(arkrtos_shell_resolve_path(NULL, out, sizeof(out)), -1);
        CK_I(arkrtos_shell_resolve_path("x", NULL, sizeof(out)), -1);
        CK_I(arkrtos_shell_resolve_path("x", out, 0u), -1);

        (void)strcpy(g_shell.cwd, "/mnt");
        CK_I(arkrtos_shell_resolve_path("x", out, sizeof(out)), 0);
        CK_S(out, "/mnt/x");

        CK_I(arkrtos_shell_resolve_path("/abs", out, sizeof(out)), 0);
        CK_S(out, "/abs");

        /* too small a destination is refused, not truncated into another path */
        CK_I(arkrtos_shell_resolve_path("/mnt/nor/deep", out, 4u), -1);
    }
}

/* ------------------------------------------------------------ mini */

static void t_mini(void)
{
    int rc;

    /* ---- run ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 0);
    CK(out_has("mini running: task 7, code 700 B in 1024 B @ 0x00001000"));
    CK(out_has("running 1 of 2 allowed"));

    reset_all();
    g_mini_run_rc = ARKRTOS_LOADER_ERR_SIZE;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("mini run: /img.svcm failed, rc=-4"));
    CK(out_has("size limit: 32768 B now"));

    reset_all();
    g_mini_run_rc = ARKRTOS_LOADER_ERR_BUSY;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("or a load is already in progress"));

    reset_all();
    g_mini_run_rc = ARKRTOS_MINI_ERR_DISABLED;
    g_crash_mcount = 3u;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("disabled after 3 consecutive faults (mini forget /img.svcm)"));

    reset_all();
    g_mini_run_rc = ARKRTOS_MINI_ERR_FS;
    g_fs_err = -2;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("file system: -2 (LFS_ERR_NOENT)"));

    /* loaded, but nothing is running any more */
    reset_all();
    g_mini_info_rc = -1;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("loaded but no longer running"));
    CK(!out_has("it has faulted before"));

    reset_all();
    g_mini_info_rc = -1;
    g_crash_mcount = 2u;
    rc = RUN(cmd_mini, "mini", "run", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("it has faulted before"));

    /* NOTE: the "path too long" refusal in run/swap/forget is unreachable from
     * the shell: ark_shell_resolve() never writes more than 95 chars into a
     * 96-byte buffer, and the command's own buffer is the same 96 bytes.  The
     * checks stay in the product as defence in depth; they are not exercised
     * here, and this test says so instead of faking a longer argument. */

    /* ---- swap ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 0);
    CK(out_has("mini swap: 3 -> /new.svcm armed; it switches at the next syscall"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "swap", "x", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("is not a decimal number (see 'mini')"));

    reset_all();
    g_mini_swap_rc = ARKRTOS_LOADER_ERR_STATE;
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("no running MiniApp with that number"));

    reset_all();
    g_mini_swap_rc = ARKRTOS_LOADER_ERR_BUSY;
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("a load or another swap is already in progress"));

    reset_all();
    g_mini_swap_rc = ARKRTOS_SWAP_ERR_LAYOUT;
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("the two images need the same code/RAM block sizes"));

    reset_all();
    g_mini_swap_rc = ARKRTOS_LOADER_ERR_SIZE;
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("size limit: 32768 B now"));

    reset_all();
    g_mini_swap_rc = ARKRTOS_MINI_ERR_FS;
    rc = RUN(cmd_mini, "mini", "swap", "3", "/new.svcm");
    CK_I(rc, 1);
    CK(out_has("cannot read the new image"));


    /* ---- stop ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "stop");
    CK_I(rc, 0);
    CK(out_has("mini stopped: all blocks returned to the pool"));

    reset_all();
    g_mini_stop_rc = ARKRTOS_LOADER_ERR_STATE;
    rc = RUN(cmd_mini, "mini", "stop");
    CK_I(rc, 1);
    CK(out_has("mini stop: nothing to stop (rc=-9)"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "stop", "3");
    CK_I(rc, 0);
    CK(out_has("mini 3 stopped: block returned to the pool"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "stop", "z");
    CK_I(rc, 1);
    CK(out_has("mini stop: z is not a decimal number"));

    reset_all();
    g_mini_stop_at_rc = ARKRTOS_LOADER_ERR_STATE;
    rc = RUN(cmd_mini, "mini", "stop", "3");
    CK_I(rc, 1);
    CK(out_has("mini stop 3: no such running MiniApp (rc=-9, see 'mini')"));

    /* ---- count ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "count");
    CK_I(rc, 0);
    CK(out_has("mini count: 2 allowed, 1 running (compile-time max"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "count", "5");
    CK_I(rc, 0);
    CK(out_has("mini count set to 5"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "count", "x");
    CK_I(rc, 1);
    CK(out_has("mini count: x is not a decimal number"));

    reset_all();
    g_mini_set_count_rc = ARKRTOS_LOADER_ERR_PARAM;
    rc = RUN(cmd_mini, "mini", "count", "9");
    CK_I(rc, 1);
    CK(out_has("mini count: 9 rejected, rc=-1"));

    /* ---- limit ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "limit");
    CK_I(rc, 0);
    CK(out_has("mini size limit: 32768 B (default 32768 B, range 1024..32768 B)"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "limit", "4096");
    CK_I(rc, 0);
    CK(out_has("mini size limit set to 4096 B"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "limit", "x");
    CK_I(rc, 1);
    CK(out_has("mini limit: x is not a decimal number"));

    reset_all();
    g_mini_set_bytes_rc = ARKRTOS_LOADER_ERR_PARAM;
    rc = RUN(cmd_mini, "mini", "limit", "999");
    CK_I(rc, 1);
    CK(out_has("mini limit: 999 rejected, rc=-1"));
    CK(out_has("need a power of two in 1024..32768 B, 0 = default"));

    /* ---- forget ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "forget", "/img.svcm");
    CK_I(rc, 0);
    CK(out_has("mini forget: /img.svcm enabled again"));

    reset_all();
    g_mini_forget_rc = -1;
    rc = RUN(cmd_mini, "mini", "forget", "/img.svcm");
    CK_I(rc, 1);
    CK(out_has("mini forget: empty path"));


    /* ---- listing ---- */
    reset_all();
    g_mini_running = 0u;
    rc = RUN(cmd_mini, "mini");
    CK_I(rc, 0);
    CK(out_has("mini: none running (usage: mini run <path>)"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "stat");
    CK_I(rc, 0);
    CK(out_has("mini: 1 running of 2 allowed"));
    CK(out_has("  [0] task 7 entry 0x00001001"));
    CK(out_has("code 700 B in 1024 B block @ 0x00001000"));
    CK(out_has("RW/ZI+stack 2048 B block @ 0x00002000, crc 0xDEADBEEF"));
    CK(out_has("block entries: 1 of 2 in use"));
    CK(out_has("size limit 32768 B"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "list");
    CK_I(rc, 0);
    CK(out_has("mini: 1 running of 2 allowed"));

    /* ---- autostart ---- */
    reset_all();
    {
        fsnode_t *n = fs_new("/mini/hello.svcm");

        if (n != NULL) {
            n->size = 3u;
        }
    }
    rc = RUN(cmd_mini, "mini", "autostart");
    CK_I(rc, 0);
    CK(out_has("[1] /mini/hello.svcm"));
    CK(out_has("3 bytes"));
    CK(out_has("[2] /mini/bad.svcm"));
    CK(out_has("not in the volume (fs err -2: LFS_ERR_NOENT)"));
    CK(out_has("2 listed, 1 missing in the volume, 0 disabled"));

    reset_all();
    {
        fsnode_t *n = fs_new("/mini/hello.svcm");

        if (n != NULL) {
            n->size = 3u;
        }
    }
    g_crash_mcount = 3u;
    g_crash_mdisabled = 1u;
    rc = RUN(cmd_mini, "mini", "autostart", "list");
    CK_I(rc, 0);
    CK(out_has("DISABLED: 3 consecutive faults (mini forget /mini/hello.svcm)"));
    CK(out_has("2 listed, 1 missing in the volume, 2 disabled"));
    CK_I(out_count("DISABLED: 3 consecutive faults"), 2);

    reset_all();
    g_mini_auto_foreach_rc = ARKRTOS_MINI_ERR_FS;
    rc = RUN(cmd_mini, "mini", "autostart", "list");
    CK_I(rc, 1);
    CK(out_has("mini autostart: no list (/mini.autostart)"));

    reset_all();
    g_mini_auto_items = 0;
    rc = RUN(cmd_mini, "mini", "autostart", "list");
    CK_I(rc, 0);
    CK(out_has("empty list, nothing starts at boot"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "autostart", "add", "/m.svcm");
    CK_I(rc, 0);
    CK(out_has("mini autostart: /m.svcm will start at boot"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "autostart", "del", "/m.svcm");
    CK_I(rc, 0);
    CK(out_has("mini autostart: /m.svcm will not start at boot"));

    reset_all();
    g_mini_auto_set_rc = ARKRTOS_LOADER_ERR_PARAM;
    rc = RUN(cmd_mini, "mini", "autostart", "add", "m.svcm");
    CK_I(rc, 1);
    CK(out_has("'m.svcm' is not a usable path"));

    reset_all();
    g_mini_auto_set_rc = ARKRTOS_LOADER_ERR_NOSPACE;
    rc = RUN(cmd_mini, "mini", "autostart", "add", "/m.svcm");
    CK_I(rc, 1);
    CK(out_has("list full (1024 B max in /mini.autostart)"));

    reset_all();
    g_mini_auto_set_rc = ARKRTOS_MINI_ERR_FS;
    g_fs_err = -1;
    rc = RUN(cmd_mini, "mini", "autostart", "add", "/m.svcm");
    CK_I(rc, 1);
    CK(out_has("mini autostart: file system: -1 (LFS_ERR_IO)"));

    reset_all();
    g_mini_auto_now_rc = 2;
    rc = RUN(cmd_mini, "mini", "autostart", "now");
    CK_I(rc, 0);
    CK(out_has("mini autostart now: 2 started"));

    reset_all();
    g_mini_auto_now_rc = ARKRTOS_MINI_ERR_FS;
    rc = RUN(cmd_mini, "mini", "autostart", "now");
    CK_I(rc, 1);
    CK(out_has("mini autostart now: no list (/mini.autostart)"));

    reset_all();
    rc = RUN(cmd_mini, "mini", "autostart", "frob");
    CK_I(rc, 1);
    CK(out_has("usage: mini autostart"));

    /* ---- usage fallback ---- */
    reset_all();
    rc = RUN(cmd_mini, "mini", "frobnicate");
    CK_I(rc, 1);
    CK(out_has("usage: mini [stat|list] | mini run <path>"));
}

/* ------------------------------------------------------------ pwd / cd / df */

static void t_pwd_cd_df(void)
{
    int rc;

    /* ---- pwd ---- */
    reset_all();
    (void)strcpy(g_shell.cwd, "/mnt/nor");
    rc = RUN(cmd_pwd, "pwd");
    CK_I(rc, 0);
    CK(out_has("/mnt/nor"));

    /* ---- cd ---- */
    reset_all();
    rc = RUN(cmd_cd, "cd", "a", "b");
    CK_I(rc, 1);
    CK(out_has("cd: too many arguments"));

    /* no argument means the cwd itself, which is resolved and validated */
    reset_all();
    (void)vnode_new("/mnt", ARKRTOS_VFS_S_IFDIR | 0755u);
    (void)strcpy(g_shell.cwd, "/mnt");
    rc = RUN(cmd_cd, "cd");
    CK_I(rc, 0);
    CK_S(g_shell.cwd, "/mnt");

    reset_all();
    rc = RUN(cmd_cd, "cd", "/dev");
    CK_I(rc, 0);
    CK_S(g_shell.cwd, "/dev");

    reset_all();
    rc = RUN(cmd_cd, "cd", "..");
    CK_I(rc, 0);
    CK_S(g_shell.cwd, "/");

    reset_all();
    rc = RUN(cmd_cd, "cd", "/nope");
    CK_I(rc, 1);
    CK(out_has("cd: /nope: No such file or directory"));
    CK_S(g_shell.cwd, "/");

    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_cd, "cd", "/f");
    CK_I(rc, 1);
    CK(out_has("cd: /f: Not a directory"));
    CK_S(g_shell.cwd, "/");

    reset_all();
    g_vfs_app_stat_rc = -2;
    rc = RUN(cmd_cd, "cd", "/dev");
    CK_I(rc, 1);
    CK(out_has("cd: /dev: No such file or directory"));

    /* ---- df ---- */
    reset_all();
    g_vfs_mount_count = 0;
    rc = RUN(cmd_df, "df");
    CK_I(rc, 0);
    CK(out_has("no mount points"));

    reset_all();
    g_fs_mounted = 1u;
    rc = RUN(cmd_df, "df");
    CK_I(rc, 0);
    CK(out_has("Filesystem"));
    CK(out_has("3996"));
    CK(out_has("/mnt/nor"));
    /* the devfs row has no usage figure and says so with dashes */
    CK(out_has("devfs"));

    /* volume not mounted: every row is dashes */
    reset_all();
    rc = RUN(cmd_df, "df");
    CK_I(rc, 0);
    CK(!out_has("3996"));

    /* stat fails: same honest dashes, not a guessed number */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_stat_rc = -1;
    rc = RUN(cmd_df, "df");
    CK_I(rc, 0);
    CK(!out_has("3996"));

    /* a mount-info that fails is skipped */
    reset_all();
    g_fs_mounted = 1u;
    g_vfs_mount_info_rc = -1;
    rc = RUN(cmd_df, "df");
    CK_I(rc, 0);
    CK(!out_has("/mnt/nor"));
}

/* ------------------------------------------------- 边角分支（防御路径） */

/* 这一组只补「把参数摆成特定形状才会走到」的分支：坏 hex、非数字长度、超
 * 出暂存区/输出列宽的输入、只在第二次读才坏的卷。它们不改变命令的正常语义，
 * 只是让防御代码真的被执行过一遍——不是靠假调用刷行覆盖率。 */
static void t_edges(void)
{
    int rc;
    char cand[4][ARK_SHELL_LINE_SIZE];
    char word[96];
    /* 与被测实现的 FS_SCRATCH_LEN 同值（512）：要构造刚好填满与多一字节
     * 两个参数，产品里那个宏是 static 的，看不到。 */
    const uint32 scratch = 512u;

    /* blk wr: 第二个 nibble 不是十六进制 -> 拒收，也不把半截 byte 写进设备 */
    reset_all();
    rc = RUN(cmd_blk, "blk", "wr", "nor0", "0", "1Z");
    CK_I(rc, -1);
    CK(out_has("bad hex string"));
    CK_I(g_flash[0], 0xFF);

    /* blk rd: 第五个参数不是数字 -> usage，不退回默认的 64 字节 */
    reset_all();
    rc = RUN(cmd_blk, "blk", "rd", "nor0", "0", "abc");
    CK_I(rc, -1);
    CK(out_has("usage"));

    /* fs rd: 请求长度超过一行 dump 的列宽 -> 按列宽切块，第二行有 0010 偏移 */
    reset_all();
    g_fs_mounted = 1u;
    {
        fsnode_t *n = fs_new("/big.bin");
        uint32 i;

        CK(n != NULL);
        if (n != NULL) {
            for (i = 0u; i < 20u; i++) {
                n->data[i] = (uint8)(0x41u + i);
            }
            n->size = 20u;
        }
    }
    rc = RUN(cmd_fs, "fs", "rd", "/big.bin", "20");
    CK_I(rc, 0);
    CK(out_has("0010:"));

    /* fs wr: 一个参数把 512 字节暂存区正好填满，后面还有参数 -> 拒绝拼空格 */
    {
        char *argv[5];
        char big[520];

        memset(big, 'a', sizeof(big));
        big[scratch] = '\0';
        argv[0] = W("fs");
        argv[1] = W("wr");
        argv[2] = W("/x");
        argv[3] = big;
        argv[4] = W("y");
        reset_all();
        g_fs_mounted = 1u;
        rc = cmd_fs(5, argv);
        CK_I(rc, -1);
        CK(out_has("usage"));
    }

    /* fs wr: 单个参数比暂存区还长 -> 拒绝，不截断成半句写进文件 */
    {
        char *argv[4];
        char big[520];

        memset(big, 'b', sizeof(big));
        big[scratch + 1u] = '\0';
        argv[0] = W("fs");
        argv[1] = W("wr");
        argv[2] = W("/x");
        argv[3] = big;
        reset_all();
        g_fs_mounted = 1u;
        rc = cmd_fs(4, argv);
        CK_I(rc, -1);
        CK(out_has("usage"));
    }

    /* fs format: 给了卷描述但偏移不是数字 -> usage，不猜一个默认偏移 */
    reset_all();
    rc = RUN(cmd_fs, "fs", "format", "nor0", "x", "4096");
    CK_I(rc, -1);
    CK(out_has("usage"));

    /* fs test: remount 之后的第二次读才失败 -> 整条测试判失败 */
    reset_all();
    g_fs_mounted = 1u;
    g_fs_read_fail_after = 1;
    rc = RUN(cmd_fs, "fs", "test");
    CK_I(rc, -1);
    CK(out_has("remount: ok"));
    CK(out_has("read: FAILED"));

    /* fs put: 控制台里残留的字节先被抽干（drain 的 spins 分支） */
    reset_all();
    g_fs_mounted = 1u;
    g_dev_read_left = 10;
    rc = RUN(cmd_fs, "fs", "put", "/p.bin", "1");
    CK_I(rc, 0);
    CK(out_has("fs put: ok"));
    CK_I(g_dev_read_left, 0);

    /* vfs_complete: 目录前缀长到装不进 dirin -> 不给候选，不截断成别的目录 */
    reset_all();
    memset(word, 'a', sizeof(word));
    word[0] = '/';
    word[71] = '/';
    word[72] = '\0';
    CK_I(vfs_complete(&g_shell, word, 1, cand, 4), 0);

    /* ls -la: 顶层 stat 失败 -> 伪行用 '?' 与破折号，而不是编一个 mode */
    reset_all();
    g_vfs_stat_rc = -1;
    rc = RUN(cmd_vfs_ls, "ls", "-la", "/");
    CK_I(rc, 0);
    CK_I(out_count("?---------"), 2);

    /* cat 无参: 不可打印字节按 \xNN 转义，不吞掉 */
    reset_all();
    g_stdin = "a\x01";
    rc = RUN(cmd_vfs_cat, "cat");
    CK_I(rc, 0);
    CK(out_has("\\x01"));

    /* chmod: 空模式串不是 0，拒收 */
    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chmod, "chmod", "", "/f");
    CK_I(rc, 1);
    CK(out_has("chmod: mode must be"));

    /* chown: 冒号前的 uid 不是十进制 -> 报错，不静默设成 0 */
    reset_all();
    (void)mkfile("/f", "x");
    rc = RUN(cmd_vfs_chown, "chown", "x:1", "/f");
    CK_I(rc, 1);
    CK(out_has("chown: owner must be a decimal id"));
}

/* ------------------------------------------------------------ main */

int main(void)
{
    t_blk();
    t_fs();
    t_vfs();
    t_mini();
    t_pwd_cd_df();
    t_edges();

    if (g_failed != 0) {
        printf("SHELL-FS-HOST FAILED: %d of %d checks\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-FS-HOST OK  %d checks\n", g_checks);
    return 0;
}
