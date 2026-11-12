/* ============================================================
 * arkmake_shell_cmd.c - 板上 "make" 命令
 *
 *     make [<makefile>] [-f <makefile>] [-o <out>] [-v] [-j]
 *     make clean [<makefile>] [-f <makefile>] [-o <out>]
 *
 * 它做三件事，顺序不能变：
 *   1) 先解析命令行。选项写错要在一毫秒内报出来——一次构建三十多秒，
 *      等它跑完才发现 -o 拼错是最浪费时间的一种失败。
 *   2) 读并解析工程描述。解析失败时工程结构无意义，绝不能"尽量构建"。
 *   3) 交给 arkmake_build()，由它打日志并调用同一个 cc_pack；
 *      或者（clean）按同一份工程描述算出产物路径并删掉它。
 *
 * 为什么 makefile 读进一块静态缓冲而不是 malloc：板上没有可靠的堆
 * （_sbrk 恒失败），而且这块缓冲只在构建期间有用，放在 .bss 里既不占
 * 镜像字节也不占任务栈。同理 project_t 也是静态的——它有两千多字节，
 * 放栈上会吃掉 shell 任务的余量，而 shell 栈一旦溢出是整机死，不是
 * 一条错误信息。
 * ============================================================ */
#include "arkmake_shell_cmd.h"
#include "arkmake_port.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"

/* clean 要按与构建完全相同的命名空间删产物：构建写盘走的是
 * arkrtos_shell_resolve_path() + arkrtos_vfs_*，clean 若改用别的入口，
 * 同一条 OUT 在两边会指向两份不同的东西。 */
#include "arkrtos_shell.h"
#include "arkrtos_vfs.h"

#include <string.h>

/* 一份工程描述在这块板上的合理上限：ARKMAKE_MAX_SRCS 份源码名 + 16 个宏 +
 * 8 个头文件目录，写全了也不到 1 KB。2048 给注释和空行留了足够余量。 */
#define MAKE_TEXT_CAP  2048
#define MAKE_ERR_CAP   96

/* 静态而不是栈：见文件头。mkproj 只在 cmd_make 的一次调用内有意义，
 * 但零初始化一次的成本（2 KB memset）远小于栈溢出的代价。 */
static char              s_make_text[MAKE_TEXT_CAP];
static char              s_make_err[MAKE_ERR_CAP];
static arkmake_project_t s_make_proj;

static void make_usage(void)
{
    ark_shell_printf("make: build a multi-source image from a project file\r\n"
                     "      make [<makefile>] [-f <makefile>] [-o <out>] [-v] [-j]\r\n"
                     "      make clean [<makefile>] [-f <makefile>] [-o <out>]\r\n"
                     "      keys: TARGET OUT SRCS DEFS INCS, one 'NAME = value' per line\r\n");
}

/* 读并解析一份 makefile，再套上 -o 覆盖。成功返回 0；失败时已经把
 * 原因打出去了（解析层的 err 一定带行号），调用方只负责返回失败。
 *
 * 构建和 clean 共用这一段：clean 也必须先知道 OUT/TARGET 才能定位产物，
 * 而"解析失败就什么都别做"这条对两者同样成立。 */
static int make_load_project(const char *file, const char *out)
{
    int rc;

    memset(&s_make_proj, 0, sizeof(s_make_proj));
    s_make_err[0] = '\0';
    rc = arkmake_parse_file(arkmake_port_read, NULL, file,
                            s_make_text, sizeof(s_make_text),
                            &s_make_proj, s_make_err, sizeof(s_make_err));
    if (rc != ARKMAKE_OK) {
        /* 解析层的 err 一定带行号；读文件失败那种情况 err 是一句人话。
         * 两种都直接给出去，不自己编一句更笼统的。 */
        ark_shell_printf("make: %s: %s\r\n", file,
                         (s_make_err[0] != '\0') ? s_make_err
                                                 : arkmake_error_name(rc));
        return 1;
    }

    /* -o 覆盖工程里的 OUT：覆盖的前提是装得下，不截断。 */
    if (out != NULL) {
        size_t n = strlen(out);

        if ((n + 1u) > sizeof(s_make_proj.out)) {
            ark_shell_printf("make: -o path is %u bytes, the slot holds %u\r\n",
                             (unsigned)(n + 1u),
                             (unsigned)sizeof(s_make_proj.out));
            return 1;
        }
        memcpy(s_make_proj.out, out, n + 1u);
    }
    return 0;
}

/* 产物路径：OUT 有值就用它，否则与构建一样按 TARGET 推 <target>.svcm。
 * 两条路必须是同一个函数，否则 clean 会去删一个构建从来不写的名字。 */
static int make_out_path(char *buf, size_t cap)
{
    size_t n;

    if (s_make_proj.out[0] != '\0') {
        n = strlen(s_make_proj.out);
        if ((n + 1u) > cap) {
            return -1;
        }
        memcpy(buf, s_make_proj.out, n + 1u);
        return 0;
    }
    return (arkmake_default_out(&s_make_proj, buf, cap) == ARKMAKE_OK) ? 0 : -1;
}

/* `make clean`：删掉这份工程构建出的镜像。
 *
 * 只删成品，因为板上没有中间产物可删——对象文件活在 cc_pack 的编译池
 * （或静态 arena）里，构建一结束就没了，从来不会落到卷上。这一点必须
 * 说出来：不说的话，用户会以为 clean 之后源码旁边还有一堆 .o 需要管。 */
static int make_clean(void)
{
    char   out[ARKMAKE_PATH_MAX];
    char   full[ARKRTOS_VFS_PATH_MAX];
    uint32 size = 0u;
    uint32 mode = 0u;

    if (make_out_path(out, sizeof(out)) != 0) {
        ark_shell_printf("make: clean: cannot determine the image name\r\n");
        return 1;
    }
    if (arkrtos_shell_resolve_path(out, full, sizeof(full)) != 0) {
        ark_shell_printf("make: clean: %s: path does not fit\r\n", out);
        return 1;
    }
    if (arkrtos_vfs_app_stat(full, &size, &mode) != 0) {
        /* 没东西可删不算失败（GNU make 的 clean 也是这个语义），但必须
         * 说出来是"没找到"，不能安静地什么都不做。 */
        ark_shell_printf("make: clean: %s: no such image\r\n", out);
        return 0;
    }
    if ((mode & ARKRTOS_VFS_S_IFDIR) != 0u) {
        ark_shell_printf("make: clean: %s: is a directory\r\n", out);
        return 1;
    }
    if (arkrtos_vfs_remove(full) != 0) {
        ark_shell_printf("make: clean: %s: remove failed\r\n", out);
        return 1;
    }
    ark_shell_printf("make: clean: removed %s (%u bytes)\r\n",
                     out, (unsigned)size);
    ark_shell_printf("make: clean: no object files to remove"
                     " (they live in the compile pool)\r\n");
    return 0;
}

int cmd_make(int argc, char *argv[])
{
    arkmake_ops_t ops;
    arkmake_ctx_t ctx;
    const char   *file = "makefile";
    const char   *out = NULL;
    int           verbose = 0;
    int           clean = 0;
    int           jobs = 0;
    int           i;
    int           rc;

    /* ---- 1) 命令行 ---- */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if ((a[0] == '-') && (a[1] == 'f') && (a[2] == '\0')) {
            if ((i + 1) >= argc) {
                ark_shell_printf("make: -f needs a file name\r\n");
                return 1;
            }
            file = argv[++i];
        } else if ((a[0] == '-') && (a[1] == 'o') && (a[2] == '\0')) {
            if ((i + 1) >= argc) {
                ark_shell_printf("make: -o needs an output path\r\n");
                return 1;
            }
            out = argv[++i];
        } else if ((a[0] == '-') && (a[1] == 'v') && (a[2] == '\0')) {
            verbose = 1;
        } else if ((a[0] == '-') && (a[1] == 'j')) {
            /* -j / -jN：认得出来，但**不假装它生效**。板上只有一个核、
             * 一个编译池（cc_pack 的槽是静态 arena），并行没有可并的东西；
             * 静默忽略会让用户以为构建被加速了，报错退出又会挡住那些在
             * 桌面 makefile 里照抄 -j 的人。所以先记下来，构建前明说一句。 */
            const char *q = a + 2;

            if (*q != '\0') {
                const char *digits = q;

                while ((*q >= '0') && (*q <= '9')) {
                    q++;
                }
                if ((q == digits) || (*q != '\0')) {
                    ark_shell_printf("make: -j takes a number (%s)\r\n", a);
                    make_usage();
                    return 1;
                }
            }
            jobs = 1;
        } else if ((a[0] == '-') && (a[1] == '\0')) {
            ark_shell_printf("make: unknown option %s\r\n", a);
            make_usage();
            return 1;
        } else if (a[0] == '-') {
            /* 不静默忽略：一个没被认出来的选项会让用户以为它生效了。 */
            ark_shell_printf("make: unknown option %s\r\n", a);
            make_usage();
            return 1;
        } else if ((a[0] == 'c') && (strcmp(a, "clean") == 0)) {
            /* `make clean`：与 GNU make 同名同义，只删这次构件的成品。 */
            clean = 1;
        } else if ((i + 1) == argc) {
            file = a;
        } else {
            ark_shell_printf("make: only one makefile may be named (%s)\r\n", a);
            return 1;
        }
    }

    /* ---- 2) 读 + 解析 ---- */
    if (make_load_project(file, out) != 0) {
        return 1;
    }

    /* ---- 2b) clean ---- */
    if (clean) {
        return make_clean();
    }

    if (jobs) {
        ark_shell_printf("make: -j has no effect: one core and one compile pool,"
                         " the build is serial by construction\r\n");
    }

    /* ---- 3) 构建 ---- */
    ctx.verbose = verbose;
    arkmake_port_ops(&ops);
    rc = arkmake_build(&s_make_proj, &ops, &ctx);
    if (rc != ARKMAKE_OK) {
        /* cc_pack 已经打过具体原因（哪一遍、哪个对象、哪个尺寸不符），
         * 这里只补一句"这次构建整体算失败"和错误分类，不重复猜原因。 */
        ark_shell_printf("make: %s (%d)\r\n", arkmake_error_name(rc), rc);
        return 1;
    }
    ark_shell_printf("make: ok\r\n");
    return 0;
}

int arkmake_shell_register(int idx)
{
    if ((idx < 0) || ((idx + 1) > ARK_SHELL_MAX_COMMANDS)) {
        /* 与 cc / vim 同一个约定：装不下就原样返回，由调用方的哨兵落到
         * 正确位置；静默少一条命令比越界写表好得多。 */
        ark_shell_printf("shell: no room for the make command\r\n");
        return idx;
    }
    g_cmd_table[idx++] = ARK_SHELL_CMD("make", cmd_make,
        "Build (or clean) a multi-source image from a project file:"
        " make [clean] [<makefile>] [-f f] [-o out] [-v] [-j]",
        ARK_SHELL_MAX_ARGS);
    return idx;
}
