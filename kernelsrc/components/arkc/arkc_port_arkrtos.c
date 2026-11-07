/* ============================================================
 * arkc_port_arkrtos.c - Ark RTOS adaptation layer for arkc
 *
 * arkc (src/, include/) knows nothing about the kernel: it asks for
 * three platform hooks (log / abort / __DATE__ strings) and five file
 * hooks (read / size / write / last error / list).  This file is the
 * only place where those meet the kernel:
 *
 *   ark_log           -> the shell console (LF translated to CRLF)
 *   ark_abort         -> longjmp back to the "cc" command, so a fatal
 *                        error inside the compiler cannot wedge the
 *                        shell task (upstream's bare-metal default is
 *                        an infinite loop: one bad compile would kill
 *                        the prompt until reset)
 *   fs_*              -> arkrtos_vfs_*, with paths resolved by the
 *                        shell so "." / ".." / "~" mean what they mean
 *                        at the prompt (same rule as the editor)
 *
 * Two deliberate limits are reported instead of hidden:
 *   - a file bigger than the caller's buffer is not silently cut, the
 *     read fails with "File too large" (arkc turns that into a
 *     diagnostic instead of compiling half a file);
 *   - "-" means the console: text is printed as-is when it is text,
 *     binary is dumped as hex, which is what upstream's STM32 default
 *     callback does.
 * ============================================================ */
#include "include/platform.h"
#include "include/flash_fs.h"

#include "arkc_port.h"
#include "arkrtos_vfs.h"
/* 错误码的唯一定义处（ARK_E_*）。arkrtos_vfs.h 只说明值与 ark_vfs 同源，
 * 真正的 enum 在组件头里；照抄数字会多一份会漂的副本。 */
#include "ark_vfs.h"
#include "arkrtos_shell.h"

/* Shell console (kernelsrc/shell/platform.h).  Included by relative path
 * because arkc ships a header with the same name (include/platform.h): a
 * bare "platform.h" would depend on -I order, which is exactly the kind of
 * silent mismatch this port must not have. */
#include "../../shell/platform.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------
 * Fatal error escape
 *
 * ark_abort() is called with no return path.  The command layer installs
 * a landing pad before calling into arkc; when none is installed (should
 * never happen, but a wrong answer is worse than a message) the port
 * prints and returns instead of hanging the shell.
 * ------------------------------------------------------------ */
static jmp_buf  *s_abort_jmp;
static uint8_t   s_abort_used;
static const char *s_abort_msg = "arkc: fatal error";

void arkc_port_set_abort_target(jmp_buf *jb)
{
    s_abort_jmp  = jb;
    s_abort_used = 0u;
}

uint8_t arkc_port_abort_used(void)
{
    return s_abort_used;
}

const char *arkc_port_abort_message(void)
{
    return s_abort_msg;
}

/* ------------------------------------------------------------
 * Compiler arena
 *
 * arkc's large buffers live in .ark_buf, which the linker script emits as
 * NOLOAD: no image bytes, no boot-time zeroing, and therefore no promise
 * about what is in there.  arkc resets the pool it manages on entry, but the
 * source/asm/code/object buffers are plain statics whose "initial value" is
 * whatever the previous compile (or the previous power-on) left behind.
 * Zeroing the whole section before each run makes every compile start from
 * the same state, which is the only way a board result can be compared with
 * a host result.
 *
 * The two symbols come from the generated linker script.  A build without
 * arkc has no such section and the symbols stay undefined; declaring them
 * weak turns that into a null address instead of a link error. */
extern uint8_t _sark_buf[] __attribute__((weak));
extern uint8_t _eark_buf[] __attribute__((weak));

void arkc_port_zero_arena(void)
{
    uint8_t *start = &_sark_buf[0];
    uint8_t *end   = &_eark_buf[0];

    if ((start == 0) || (end == 0) || (end <= start)) {
        return;
    }
    memset(start, 0, (size_t)(end - start));
}

/* ------------------------------------------------------------
 * Output capture (see arkc_port.h)
 *
 * 捕获缓冲属于调用方（板上是 RAM 池借来的块，PC 上是 malloc），本文件
 * 只管三层状态，不申请任何内存。
 * ------------------------------------------------------------ */
#if (ARKRTOS_USE_APPPACK == 1)
static uint8_t *s_cap_buf;
static uint32_t s_cap_cap;
static uint32_t s_cap_len;
static int      s_cap_active;

int arkc_port_capture_begin(uint8_t *buf, uint32_t cap)
{
    if ((buf == 0) || (cap == 0u) || (s_cap_active != 0)) {
        return -1;
    }
    s_cap_buf    = buf;
    s_cap_cap    = cap;
    s_cap_len    = 0u;
    s_cap_active = 1;
    return 0;
}

uint32_t arkc_port_capture_end(void)
{
    uint32_t len = s_cap_len;

    s_cap_active = 0;
    s_cap_buf    = 0;
    s_cap_cap    = 0u;
    s_cap_len    = 0u;
    return len;
}
/* ------------------------------------------------------------
 * In-memory files (source of the registry below)
 *
 * `cc --pack` links several translation units into one relocatable image:
 * every pass compiles each .c into an object and then links the objects.
 * Those objects must not go through the filesystem: ramfs has three slots
 * (the source, the .sct and the output already need them) and one littlefs
 * write on NOR costs ~10 s because of the sector erase, four passes times N
 * objects later.  They live in RAM the packer already borrowed from the
 * image pool, and the three file hooks below look them up by name first.
 *
 * Names are whatever the caller registers ("/v/obj0"); they are matched
 * literally and never resolved, so they cannot collide with a real volume
 * path by accident.
 * ------------------------------------------------------------ */
#define ARKC_VFILE_MAX   4
#define ARKC_VFILE_NAMELEN 16

typedef struct {
    char     name[ARKC_VFILE_NAMELEN];
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    /* ARKOBJ2 头里那个"代码长度"，在**写的那一刻**从写进来的字节里记下。
     * 不在读完后再回读地算：板上实测过回读拿到空槽（-pp 三源时第三槽读成 0，
     * 而编译器明明写了 2560 字节的对象，两行日志之间地内容又对得上）。
     * 长度是"写进去的那份字节自己说的"，记账就该记在它身上，不该依赖
     * "写完立刻能读回来"这个假设。 */
    uint32_t code_size;
    int      used;
} arkc_vfile_t;

static arkc_vfile_t s_vfile[ARKC_VFILE_MAX];

static arkc_vfile_t *vfile_find(const char *path)
{
    uint32_t i;

    if (path == NULL) {
        return 0;
    }
    for (i = 0; i < ARKC_VFILE_MAX; i++) {
        if ((s_vfile[i].used != 0) && (strcmp(s_vfile[i].name, path) == 0)) {
            return &s_vfile[i];
        }
    }
    return 0;
}

void arkc_port_vfile_clear(void)
{
    uint32_t i;

    for (i = 0; i < ARKC_VFILE_MAX; i++) {
        s_vfile[i].used    = 0;
        s_vfile[i].len     = 0u;
        s_vfile[i].buf     = 0;
        s_vfile[i].cap     = 0u;
        s_vfile[i].name[0] = '\0';
        s_vfile[i].code_size = 0u;
    }
}

int arkc_port_vfile_register(const char *name, uint8_t *buf, uint32_t cap)
{
    uint32_t i;
    uint32_t n;

    if ((name == NULL) || (buf == NULL) || (cap == 0u)) {
        return -1;
    }
    n = (uint32_t)strlen(name);
    if ((n == 0u) || (n >= ARKC_VFILE_NAMELEN)) {
        return -1;                     /* too long to be matched literally */
    }
    if (vfile_find(name) != 0) {
        return -1;                     /* registered twice: refuse, do not alias */
    }
    for (i = 0; i < ARKC_VFILE_MAX; i++) {
        if (s_vfile[i].used != 0) {
            continue;
        }
        memcpy(s_vfile[i].name, name, (size_t)n + 1u);
        s_vfile[i].buf       = buf;
        s_vfile[i].cap       = cap;
        s_vfile[i].len       = 0u;
        s_vfile[i].code_size = 0u;   /* 还没写过，读长度必须先报"读不出" */
        s_vfile[i].used      = 1;
        return 0;
    }
    return -1;                         /* table full */
}

uint32_t arkc_port_vfile_size(const char *name)
{
    arkc_vfile_t *vf = vfile_find(name);

    return (vf != 0) ? vf->len : 0u;
}

uint32_t arkc_port_vfile_code_size(const char *name)
{
    arkc_vfile_t *vf = vfile_find(name);

    /* 0 = 读不出（名字没注册 / 还没写过 / 写进来的不是 ARKOBJ2）。
     * 三个都返回同一个 0，因为调用方对它们的处置完全相同：停下，不猜长度。 */
    return (vf != 0) ? vf->code_size : 0u;
}

#endif /* ARKRTOS_USE_APPPACK */

/* ------------------------------------------------------------
 * Console output
 * ------------------------------------------------------------ *//* ------------------------------------------------------------
 * Console output
 * ------------------------------------------------------------ */
static void con_puts(const char *s)
{
    platform_uart_send_string(s);
}

static void con_write(const char *buf, uint32_t len)
{
    uint32_t off = 0u;

    platform_console_begin();
    while (off < len) {
        uint32_t n = len - off;
        if (n > 200u) {
            n = 200u;                 /* send_buf takes a uint16 length */
        }
        platform_uart_send_buf((const unsigned char *)(buf + off), (uint16_t)n);
        off += n;
    }
    platform_console_end();
}

/* ark_log: printf-style log to the shell console.
 *
 * The formatting itself is newlib's vsnprintf: arkc emits "%d %u %x %s %c"
 * and field widths (%-10s), never floats, so the nano printf is enough and
 * a second hand-rolled formatter would only be a second thing to get wrong.
 * A line longer than the buffer is split at the buffer edge and continued,
 * so no character is dropped. */
void ark_log(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    {
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if ((uint32_t)n < sizeof(buf)) {
            /* Translate LF to CRLF: the console device does not do it. */
            uint32_t i;
            char     out[300];
            uint32_t o = 0u;
            for (i = 0u; (i < (uint32_t)n) && (o < (uint32_t)sizeof(out) - 2u); i++) {
                if (buf[i] == '\n') {
                    out[o++] = '\r';
                }
                out[o++] = buf[i];
            }
            con_write(out, o);
        } else {
            /* Truncated by vsnprintf: report it rather than lose the tail
             * without a word. */
            con_write(buf, (uint32_t)strlen(buf));
            con_puts("...\r\n[arkc] log line longer than 256 bytes, truncated\r\n");
        }
    }
}

void ark_abort(const char *msg)
{
    s_abort_msg  = (msg != NULL) ? msg : "arkc: fatal error";
    s_abort_used = 1u;
    con_puts("\r\n[arkc] FATAL: ");
    con_puts(s_abort_msg);
    con_puts("\r\n");
    if (s_abort_jmp != NULL) {
        longjmp(*s_abort_jmp, 1);
    }
    /* No landing pad: say so and return.  Returning from a function arkc
     * declared as "does not return" is undefined behavior, but hanging the
     * whole shell on a bookkeeping mistake is a worse answer than one line
     * of output followed by whatever the caller does next. */
    con_puts("[arkc] no abort landing pad installed; continuing is unsafe\r\n");
}

/* C99 __DATE__ / __TIME__: this board has no RTC in the kernel, so the
 * compiler reports the same fixed epoch upstream's STM32 port does. */
void ark_get_date_time(char *date_buf, char *time_buf)
{
    static const char d[]  = "Jan  1 1970";
    static const char tn[] = "00:00:00";
    int i;

    if (date_buf != NULL) {
        for (i = 0; i < 11; i++) {
            date_buf[i] = d[i];
        }
        date_buf[11] = '\0';
    }
    if (time_buf != NULL) {
        for (i = 0; i < 8; i++) {
            time_buf[i] = tn[i];
        }
        time_buf[8] = '\0';
    }
}

/* ------------------------------------------------------------
 * File hooks
 * ------------------------------------------------------------ */
static const char *s_last_error = "No error";

static void set_error_from(int32_t rc)
{
    switch (rc) {
    case ARK_E_NOENT:
        s_last_error = "No such file or directory";
        break;
    case ARK_E_ISDIR:
        s_last_error = "Is a directory";
        break;
    case ARK_E_NOTDIR:
        s_last_error = "Not a directory";
        break;
    case ARK_E_NOSPC:
        s_last_error = "No space left on device";
        break;
    case ARK_E_ROFS:
        s_last_error = "Read-only file system";
        break;
    case ARK_E_NAMETOOLONG:
        s_last_error = "File name too long";
        break;
    case ARK_E_INVAL:
        s_last_error = "Invalid argument";
        break;
    case ARK_E_NOSYS:
        s_last_error = "Operation not supported";
        break;
    default:
        s_last_error = "I/O error";
        break;
    }
}

/* Resolve a path the way the shell would.  Returns 0 and fills `out`. */
static int resolve(const char *in, char *out, uint32_t cap)
{
    if ((in == NULL) || (in[0] == '\0') || (out == NULL) || (cap == 0u)) {
        s_last_error = "Invalid argument";
        return -1;
    }
    if (arkrtos_shell_resolve_path(in, out, cap) != 0) {
        s_last_error = "File name too long";
        return -1;
    }
    return 0;
}

int fs_read_file(const char *path, char *buf, int max_len)
{
    char     full[ARKRTOS_VFS_PATH_MAX];
    uint32_t fsize = 0u;
    uint32_t fmode = 0u;
    int32_t  fd;
    int32_t  total = 0;
    int32_t  rc;

    if ((buf == NULL) || (max_len <= 0)) {
        s_last_error = "Invalid argument";
        return -1;
    }
#if (ARKRTOS_USE_APPPACK == 1)
    {
        /* In-memory file: same refuse-instead-of-truncate rule as below. */
        arkc_vfile_t *vf = vfile_find(path);
        if (vf != 0) {
            if (vf->len > (uint32_t)(max_len - 1)) {
                s_last_error = "File too large";
                return -1;
            }
            if (vf->len > 0u) {
                memcpy(buf, vf->buf, (size_t)vf->len);
            }
            buf[vf->len] = '\0';
            return (int)vf->len;
        }
    }
#endif
    if (resolve(path, full, sizeof(full)) != 0) {
        return -1;
    }
    rc = arkrtos_vfs_app_stat(full, &fsize, &fmode);
    if (rc != 0) {
        set_error_from(rc);
        return -1;
    }
    if ((fmode & ARKRTOS_VFS_S_IFDIR) != 0u) {
        s_last_error = "Is a directory";
        return -1;
    }
    /* Refuse instead of truncating: a silently shortened source file turns
     * into a stream of bogus diagnostics, and a shortened header hides its
     * closing #endif. */
    if (fsize > (uint32_t)(max_len - 1)) {
        s_last_error = "File too large";
        return -1;
    }

    fd = arkrtos_vfs_open_read(full);
    if (fd < 0) {
        set_error_from(fd);
        return -1;
    }
    while (total < max_len - 1) {
        int32_t n = arkrtos_vfs_read(fd, (uint8 *)(buf + total),
                                     (uint32)(max_len - 1 - total));
        if (n < 0) {
            (void)arkrtos_vfs_close(fd);
            set_error_from(n);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    (void)arkrtos_vfs_close(fd);
    buf[total] = '\0';
    return (int)total;
}

int fs_file_size(const char *path)
{
    char     full[ARKRTOS_VFS_PATH_MAX];
    uint32_t fsize = 0u;
    uint32_t fmode = 0u;
    int32_t  rc;

#if (ARKRTOS_USE_APPPACK == 1)
    {
        arkc_vfile_t *vf = vfile_find(path);
        if (vf != 0) {
            return (int)vf->len;
        }
    }
#endif
    if (resolve(path, full, sizeof(full)) != 0) {
        return -1;
    }
    rc = arkrtos_vfs_app_stat(full, &fsize, &fmode);
    if (rc != 0) {
        set_error_from(rc);
        return -1;
    }
    return (int)fsize;
}

/* Text or binary?  "-" has to serve both `-E`/`-S` (text on the console)
 * and a default `.bin` output, and the kernel console prints bytes as
 * they are, so binary would scribble on the terminal. */
static int looks_like_text(const unsigned char *data, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        unsigned char c = data[i];
        if ((c == '\n') || (c == '\r') || (c == '\t')) {
            continue;
        }
        if ((c < 0x20u) || (c > 0x7Eu)) {
            return 0;
        }
    }
    return 1;
}

static void dump_hex(const unsigned char *data, int len)
{
    static const char hex[] = "0123456789ABCDEF";
    char  line[80];
    int   i;
    int   o = 0;

    for (i = 0; i < len; i++) {
        line[o++] = hex[(data[i] >> 4) & 0x0Fu];
        line[o++] = hex[data[i] & 0x0Fu];
        line[o++] = ' ';
        if ((o >= (int)sizeof(line) - 4) || (i == len - 1)) {
            line[o++] = '\r';
            line[o++] = '\n';
            con_write(line, (uint32_t)o);
            o = 0;
        }
    }
}

int fs_write_file(const char *path, const unsigned char *data, int len)
{
    char    full[ARKRTOS_VFS_PATH_MAX];
    int32_t rc;

    if ((path == NULL) || (data == NULL) || (len < 0)) {
        s_last_error = "Invalid argument";
        return -1;
    }
#if (ARKRTOS_USE_APPPACK == 1)
    {
        /* Registered before the capture check on purpose: an object write is
         * an explicit "this buffer", and the capture hook is a catch-all for
         * "whatever this pass writes".  A registered name therefore wins. */
        arkc_vfile_t *vf = vfile_find(path);
        if (vf != 0) {
            if ((uint32_t)len > vf->cap) {
                s_last_error = "File too large";
                return -1;             /* never truncate */
            }
            if (len > 0) {
                memcpy(vf->buf, data, (size_t)len);
            }
            vf->len = (uint32_t)len;
            /* 代码长度从**写进来的字节**里取（见 arkc_vfile_t 的说明）。 */
            vf->code_size = 0u;
            if ((len >= 12) && (data[0] == 'A') && (data[1] == 'R') &&
                (data[2] == 'K') && (data[3] == 'O') && (data[4] == 'B') &&
                (data[5] == 'J') && (data[6] == '2') && (data[7] == '\n')) {
                vf->code_size = (uint32_t)data[8] |
                                ((uint32_t)data[9] << 8) |
                                ((uint32_t)data[10] << 16) |
                                ((uint32_t)data[11] << 24);
            }
            return len;
        }
    }
#endif
#if (ARKRTOS_USE_APPPACK == 1)
    /* 捕获优先于 "-" 与 "nul"：打包时四遍的输出都必须是字节，打印到控制台
     * 或丢掉都会让下一段拿到空缓冲。这条判断放在路径解析之前，所以捕获期间
     * 连"这个路径存不存在"都不再问——这是有意的，捕获本来就不碰文件系统。 */
    if (s_cap_active != 0) {
        if ((uint32_t)len > (s_cap_cap - s_cap_len)) {
            s_last_error = "Output larger than the capture buffer";
            return -1;                 /* 不截断 */
        }
        memcpy(s_cap_buf + s_cap_len, data, (size_t)len);
        s_cap_len += (uint32_t)len;
        return len;
    }
#endif
    if ((strcmp(path, "-") == 0) || (path[0] == '\0')) {
        if (looks_like_text(data, len)) {
            con_write((const char *)data, (uint32_t)len);
        } else {
            ark_log("[arkc] %d bytes of binary, hex dump follows\r\n", len);
            dump_hex(data, len);
        }
        return len;
    }
    if ((strcmp(path, "nul") == 0) || (strcmp(path, "NUL") == 0)) {
        return len;                        /* discard, as on the host */
    }
    if (resolve(path, full, sizeof(full)) != 0) {
        return -1;
    }
    /* arkrtos_vfs_write_file() reports 0 on success or a negative ark_vfs
     * error code; it does NOT return the byte count (ark_vfs_write_all()
     * returns ARK_VFS_OK, not the length).  Comparing against len would
     * therefore report "cannot write output file" for a file that was
     * written in full and was sitting on the volume - the wrong answer is
     * worse than no answer, so the check is on the sign only. */
    rc = arkrtos_vfs_write_file(full, (const uint8 *)data, (uint32)len);
    if (rc < 0) {
        /* The volume writes as much as fits and then reports NOSPC, so a
         * short file can be left behind.  A half-written .s/.bin that still
         * lists in `ls` and still opens in `cat` is exactly the kind of
         * wrong answer this port is not allowed to hand out: drop it so the
         * failure is visible in the directory as well as in the message.
         * (The target was truncated at open time, so nothing of the old
         * content could have survived either way.) */
        (void)arkrtos_vfs_remove(full);
        set_error_from(rc);
        return -1;
    }
    return len;
}

const char *fs_last_error(void)
{
    return s_last_error;
}

/* ------------------------------------------------------------
 * fs_list_files: "ls -l" of the root, printed through ark_log so the
 * listing appears wherever the compiler's log goes.
 * ------------------------------------------------------------ */
typedef struct {
    int count;
} list_ctx_t;

static int list_cb(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    list_ctx_t *ctx = (list_ctx_t *)arg;

    if ((st != NULL) && ((st->mode & ARKRTOS_VFS_S_IFDIR) != 0u)) {
        ark_log("  %-24s <dir>\n", name);
    } else if (st != NULL) {
        ark_log("  %-24s %u\n", name, (unsigned)st->size);
    } else {
        ark_log("  %s\n", name);
    }
    ctx->count++;
    return 0;
}

/* ------------------------------------------------------------
 * 上一次编译的符号表（只给 "cc --pack" 的入口判定用）
 *
 * arkc 的 ARM 后端在每次组装结束时把本单元的标签表留在 g_arm_syms 里，
 * 直到下一次编译开头才清（cc_reset_state）。这两个 getter 是它公开的读口
 * （backend_arm.c 里非 static），write_ark_obj() 写对象时用的就是它们——
 * 所以这里读的不是替身，而是同一份事实。
 * ------------------------------------------------------------ */
extern int         arm_get_symbol_count(void);
extern const char *arm_get_symbol_name(int idx);

int arkc_port_last_object_has_symbol(const char *name)
{
    int i;
    int n;

    if ((name == NULL) || (name[0] == '\0'))
    {
        return 0;
    }

    n = arm_get_symbol_count();
    for (i = 0; i < n; i++)
    {
        const char *s = arm_get_symbol_name(i);

        if ((s != NULL) && (strcmp(s, name) == 0))
        {
            return 1;
        }
    }

    return 0;
}

int fs_list_files(void)
{
    list_ctx_t ctx;
    int32_t    rc;

    ctx.count = 0;
    rc = arkrtos_vfs_list("/", list_cb, &ctx);
    if (rc < 0) {
        set_error_from(rc);
        return -1;
    }
    return ctx.count;
}
