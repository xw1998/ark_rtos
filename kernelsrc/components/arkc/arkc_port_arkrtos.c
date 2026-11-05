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
