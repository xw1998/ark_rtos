/* ============================================================
 * arkc_shell_cmd.c - The "cc" shell command
 *
 * This is the board-side command line the compiler never had: the
 * upstream STM32 port calls arkcc_stm32_compile("main.c") from main(),
 * with the output path fixed and no way to say -S or -D.  Here the
 * options are parsed from argv instead, in the same shape as the host
 * driver (src/main.c), and the result is reported with the compiler's
 * own return code rather than a guess.
 *
 * Four things this file gets right:
 *   1) the abort pad.  arkc calls ark_abort() when the arena runs out;
 *      upstream that is `for(;;)`, which would take the shell task with
 *      it.  setjmp/longjmp brings control back here so the prompt
 *      survives and the failure is reported as a failure.
 *   2) per-run reset.  Diagnostics counters and g_config are reset on
 *      every invocation: a previous run's error count must not make
 *      the next compile fail before it starts.
 *   3) options that this build cannot honour are refused, not ignored.
 *      A silently dropped -o would leave the user hunting for a file
 *      that was never written.
 *   4) no guessing about the backend: default is "arm" (the only one
 *      compiled in), and a wrong -target is reported by arkc itself.
 * ============================================================ */
#include "arkc_shell_cmd.h"
#include "arkc_port.h"

#include "include/compiler.h"
#include "include/common.h"
#include "include/config.h"
#include "include/diag.h"
#include "include/memory.h"
#include "include/platform.h"
#include "include/arkcc_config.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if (ARKRTOS_USE_APPPACK == 1)
#include "arkrtos_vfs.h"           /* removes the temporary .sct again */
#endif
#if (ARKRTOS_USE_APPPACK == 1)
#include "include/flash_fs.h"      /* fs_write_file / fs_last_error */
#include "arkrtos_apppack.h"       /* the four-pass diff itself (pure) */
#include "arkrtos_ptable.h"        /* four payloads + the image come from the pool */
#endif

/* --scatter / --vector-table / -link are the "layout" options and have two
 * users: direct running of an app (APPRUN) and relocatable packing (APPPACK,
 * whose four passes each need a layout).  With both switches off the command
 * surface and every image are byte-identical to the build that had neither. */
#if (ARKRTOS_USE_APPPACK == 1) || (ARKRTOS_USE_APPRUN == 1)
#define CC_LAYOUT_OPTS   1
#else
#define CC_LAYOUT_OPTS   0
#endif

/* Worst-case option storage; matches the fixed arrays in ArkOptions. */
#define CC_MAX_INCLUDE_DIRS   16
#define CC_MAX_DEFINES        64

/* Everything one compilation must start from a known state: the runtime
 * feature defaults (config_init() overwrites the whole struct, so it has to
 * run first), the diagnostic counters, and the NOLOAD compiler arena whose
 * buffers arkc does not clear itself.  A single `cc` needs this once;
 * `cc --pack` runs the compiler four times and needs it four times, so the
 * body lives in one place. */
static void cc_reset_state(const char *src, int opt_level)
{
    config_init();
    g_config.enable_fpu_hw = 1;   /* the H743 has a double-precision FPU */
    g_config.opt_level     = opt_level;

    arkc_port_zero_arena();
    diag_init(src, NULL);
}

static void cc_usage(void)
{
    ark_shell_printf(
        "usage: cc <file.c> [-o <out>] [-S] [-c] [-E] [-target arm]\r\n"
        "          [-O0|-O1|-O2] [-D<name>[=<value>]] [-I <dir>] [-v]\r\n"
#if (CC_LAYOUT_OPTS == 1)
        "          [--scatter <file.sct>] [--vector-table]\r\n"
        "          [--reloc-text|--no-reloc-text]\r\n"
        "          [-link <a.obj> [<b.obj> ...]]\r\n"
#endif
        "  no -o: writes <file>.bin (or prints to the console with -S/-E)\r\n"
        "  -o - : prints to the console (text as-is, binary as hex)\r\n"
#if (CC_LAYOUT_OPTS == 1)
        "  --scatter: link for that memory layout (needed for 'app run')\r\n"
        "  --vector-table: reserve 0x400 at the front of the image for the\r\n"
        "         Cortex-M vector table.  Compile with it the object that is\r\n"
        "         linked FIRST; -link fills the table into that hole.  Nothing\r\n"
        "         is ever shifted: arkc bakes absolute addresses, so a shifted\r\n"
        "         image would keep every string-literal pointer 0x400 too low\r\n"
        "  -link: link .obj files into one image; every argument up to the next\r\n"
        "         option is an input.  Add --scatter so the image carries a vector\r\n"
        "         table for that window ('app run' reads SP/entry from it)\r\n"
        "  --reloc-text: let the linker fix in-section absolute addresses (string\r\n"
        "         literals) per object.  ON by default for -link: every object was\r\n"
        "         compiled against another base, so leaving it off points those\r\n"
        "         addresses at 0x08000000.  --no-reloc-text keeps the old output\r\n"
#endif
#if (ARKRTOS_USE_APPPACK == 1)
        "          [--pack [-o <f.svcm>]] <file.c> [<file2.c> ...]\r\n"
#endif
        );
#if (ARKRTOS_USE_APPPACK == 1)
    ark_shell_printf(
        "  --pack: link the file(s) four times (nominal / ROM+d / RAM+d /\r\n"
        "         ROM+2d and RAM+d), diff the four into a relocation table\r\n"
        "         and write one loadable image.  Needs -o.  Install it with\r\n"
        "         'mini run', not 'app run'\r\n"
        "         Several sources: each is compiled to an in-memory object and the\r\n"
        "         objects are linked per pass (up to 4 sources).  arkc's objects\r\n"
        "         bake their bases at codegen time, so the four passes have to\r\n"
        "         recompile the source; -c objects cannot be reused here\r\n");
#endif
}

/* How much of the compiler arena this run consumed.  Printed on every path
 * that ends without a normal "done": when the pool is what ran out, the
 * number is the difference between "raise ARK_POOL_SIZE by 20%" and guessing.
 * mem_pool_used() is arkc's own bump pointer, so this is the real figure. */
static void cc_report_pool(void)
{
    ark_shell_printf("cc: pool %d/%d B\r\n",
                     mem_pool_used(), (int)ARK_POOL_SIZE);
}

/* The text of an option that this build must not swallow. */
static int cc_bad_option(const char *what, const char *arg)
{
    ark_shell_printf("cc: %s '%s' is not supported by this build\r\n",
                     what, (arg != NULL) ? arg : "");
    return 1;
}

#if (ARKRTOS_USE_APPPACK == 1)
/* ============================================================
 * --pack: four-pass differential packing (M2)
 *
 * One source file is linked four times, only the link bases differ:
 *     A = nominal                (its payload is what the image carries)
 *     B = ROM + d
 *     C = RAM + d
 *     D = ROM + 2d, RAM + d      (proof: C/A applied back must equal D)
 * All four live in memory (borrowed from the RAM pool), because writing them
 * into ramfs would need three file slots that a source + .sct + output already
 * fill, and they would have to be read back byte by byte.
 *
 * Why the four passes run on the board rather than packing on the host and
 * copying the result over: that is the claim under test.  The host copy
 * (tools/pack_app.py) stays as the byte-for-byte referee -- same algorithm in
 * two places, so a disagreement means one of them is wrong.
 *
 * Several sources: the differential needs the *source* recompiled per pass,
 * because arkc's object files bake the .text/.data bases at codegen time --
 * relinking one object set at four different bases changes nothing but the
 * vector table, so no relocation table could be derived from it.  Each pass
 * therefore recompiles every source into an object and links them, and the
 * objects live in RAM through the port's in-memory file registry (see
 * arkc_port.h): ramfs has 3 slots and one littlefs write costs ~10 s).
 *
 * Memory: 4 x 8 KB payloads + 16 KB of object slots/output, borrowed from the
 * pool and returned on every path -- unless ARKRTOS_PACK_IN_EXTRAM is on, in
 * which case the same buffers live in the external SDRAM (see below).
 * ============================================================ */

/* --pack 的缓冲放在哪：片内 RAM 池（默认）还是外置 SDRAM。
 *
 * 为什么要第二条路：池是 SLOT_RAM（64 KB 伙伴分配），一次借出受
 * SLOT_RAM_MAX_BLOCK=32 KB 限制，而 app run 的 16 KB 窗口平时就占着，于是
 * 只剩「4 x 8 KB 负载窗 + 16 KB 对象/镜像」这一种拼法。**负载一旦超过
 * 8 KB 就编不出来**——实测四个源（小程序 SDK 的启动桩 + main + 运行时 +
 * 一个 hello）合计 9.0 KB，报
 *   "hello.c produced 1728 B at +0x1B52, which does not fit the 8192 B window"
 * 而同一块板子上有 32 MB 外置 SDRAM，编译器自己的 arena（.ark_buf, 4 MB）
 * 就在里面。把打包的四份负载与对象/镜像缓冲一起挪进 SDRAM 之后，窗口可以
 * 开到 16 KB，池则完全不被占用（小程序装载要的 16 KB 不再和打包抢）。
 *
 * 默认 0：与引入这个开关之前逐字节相同。板级构建用 USE_SDRAM=1 打开它，
 * 因为 .extram 段只在 SDRAM 档的链接脚本里存在。 */
#ifndef ARKRTOS_PACK_IN_EXTRAM
#define ARKRTOS_PACK_IN_EXTRAM  0
#endif

/* 缓冲的落点：.extram（NOLOAD，SDRAMFS 分区）还是片内池。两种档的窗口大小
 * 也不同 —— SDRAM 有地方，就按小程序装载上限（SLOT_RAM_MAX_BLOCK = 32 KB，
 * 取 2 的幂）给到 16 KB。 */
#if (ARKRTOS_PACK_IN_EXTRAM == 1)
#define CC_PACK_EXTRAM_ATTR  __attribute__((section(".extram")))
#define CC_PACK_WIN_SIZE     16384u
#else
#define CC_PACK_EXTRAM_ATTR
#define CC_PACK_WIN_SIZE     8192u
#endif

/* 256 header + 1024 x 4 table + 8192 payload = 12544 -> one 16 KB block. */
#define CC_PACK_OUT_CAP      16384u

#if (ARKRTOS_PACK_IN_EXTRAM == 1)
/* 四份负载 + 对象/镜像缓冲，全部在 SDRAM 里静态常驻：一次打包用一块固定的
 * 112 KB（4 x 16 KB 负载窗 + 48 KB 对象/镜像区，见下面的 CC_PACK_OBJ_ARENA），
 * 不向池借，也就不存在"借了没还"（下一次打包失败的那种最坏形态）。
 * 代价是这 112 KB 一直占着 .extram —— 那块是 6 MB 的 SDRAMFS 分区，ramfs 用掉
 * 5.5 MB 之后仍装得下（实测 93%）。 */
#define CC_PACK_EXTRAM_SIZE  (4u * CC_PACK_WIN_SIZE + CC_PACK_OBJ_ARENA)
#endif
/* Objects of one pass, one slot per source. */
#define CC_PACK_MAX_SRCS     4
/* 对象槽的容量按这次真正编译的源数均分（cc_pack_vfiles 里算）。
 *
 * 槽太小会在**写对象**那一步失败，而写失败只报 -1，看上去像「编译失败」
 * ——这一轮就是这么撞上的：rt.c 编出 8660 B 的对象，3 个源均分 16 KB 只有
 * 5461 B 一槽，报的是 `compile rc=-1 (File too large)`。所以下面这两档
 * 的容量都是按**对象文件**的大小定的，不是按代码长度：一个 3 函数的对象
 * 带上 0x400 的向量表洞实测 4247 B，一个 1 函数的 hello.c 是 2753 B。
 * 超出就照实报 File too large，不截断。 */
#if (ARKRTOS_PACK_IN_EXTRAM == 1)
/* SDRAM 档：这块不再是从池里借来的 16 KB，而是 .extram 里实打实的 48 KB，
 * 所以可以按"每个源一个够用的槽"来定，不必四个源挤 16 KB。
 *
 * 48 KB 的来历是实测，不是估的：arkc 的对象文件不是代码长度——ARKOBJ2 还
 * 带符号表与重定位表，一个只有 1 个函数的 hello.c 就有 2753 B，而带
 * printf 的小程序运行时（rt.c，10 个函数）是 8660 B。四个源均分时槽是
 * 12288 B（3 个源时 16384 B），够 8660 再留 40% 余量。**别看代码长度
 * 定这个数**：代码 2.5 KB 的对象文件是 8.6 KB，按代码算会差三倍。 */
#define CC_PACK_OBJ_ARENA    49152u
static uint8_t s_cc_pack_extram[CC_PACK_EXTRAM_SIZE] CC_PACK_EXTRAM_ATTR;
#else
#define CC_PACK_OBJ_ARENA    CC_PACK_OUT_CAP            /* 与镜像缓冲共用 */
#endif
/* 池一次借出的两块各自都要落进 [MIN, MAX]（MAX = SLOT_RAM_MAX_BLOCK = 32 KB），
 * 所以「32 KB 负载窗口 + 16 KB 对象槽」不能合成一次借出（48 KB 上取整成 64 KB，
 * 直接被拒）；而 SLOT_RAM_TOTAL 只有 64 KB，app run 的 16 KB 窗口平时就占着，
 * 于是借出总量只能是 32 + 16 = 48 KB，两块都不能再涨。
 * 对象槽因此和镜像缓冲共用那 16 KB：镜像只在**四遍都跑完之后**才写进去，
 * 而对象在最后一遍链接完成后就不再被读，用途在时间上不重叠。这条依赖写成
 * 下面的 #error 而不是注释——顺序一旦被改动，这里先失败。 */
#if (ARKRTOS_PACK_IN_EXTRAM == 0)
#if (CC_PACK_OBJ_ARENA > CC_PACK_OUT_CAP)
#error "object slots share the image buffer; they must fit inside it"
#endif
#endif
#define CC_PACK_ARENA        (4u * CC_PACK_WIN_SIZE)
/* The nominal bases only end up in the image header (nominal_base /
 * nominal_ram_base); the loader adds (landing point - nominal) at run time, so
 * these values never have to be reached.  ARM's default bases keep them
 * comparable with a plain .sct. */
#define CC_PACK_ROM_BASE     0x08000000u
#define CC_PACK_RAM_BASE     0x20000000u
/* Base increment: must be at least one window (see arkrtos_apppack.h) so that
 * the four address ranges cannot overlap; 512 KB is far larger than 8 KB. */
#define CC_PACK_DELTA        0x00080000u
#define CC_PACK_SCT          "/tmp/pp.sct"

/* One output name per pass.  With capture armed these files are never created;
 * they exist so a failure line can say which pass it was, and so that a capture
 * that silently did not arm would leave visible files behind instead of an
 * empty buffer nobody notices. */
static const char *const s_cc_pack_out[4] = {
    "/tmp/ppA.bin", "/tmp/ppB.bin", "/tmp/ppC.bin", "/tmp/ppD.bin"
};
static const char s_cc_pack_tag[4] = { 'A', 'B', 'C', 'D' };

/* "0x%08X" without pulling printf's format machinery into the image for four
 * lines of text. */
static int cc_hex32(char *p, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    p[0] = '0';
    p[1] = 'x';
    for (i = 0; i < 8; i++) {
        p[2 + i] = hex[(v >> (28 - (4 * i))) & 0xFu];
    }
    return 10;
}

static int cc_str(char *p, const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        p[n] = s[n];
        n++;
    }
    return n;
}

/* Which pass moves which window, spelled out per window so that "what this
 * pass differs in" is readable without indexing arithmetic. */
static const uint32_t s_cc_pack_rom_add[4] = { 0u, CC_PACK_DELTA, 0u, 2u * CC_PACK_DELTA };
static const uint32_t s_cc_pack_ram_add[4] = { 0u, 0u, CC_PACK_DELTA, CC_PACK_DELTA };

/* Write one .sct.  The body is character-identical to the .sct that M1 verified
 * on the board, including the CRLF line ends (that file came from
 * `echo ... >>` on the shell, which is the only form ever proven to parse).
 *
 * rom/rom_len are not the pass constant any more: every object of a pass is
 * compiled against the base it will really be loaded at, because arkc bakes
 * absolute addresses into the code and the linker therefore cannot place an
 * object after the fact (see cc_pack). */
static int cc_pack_write_sct(uint32_t rom, uint32_t rom_len, uint32_t ram)
{
    char txt[256];
    int  n = 0;

    n += cc_str(txt + n, "LR_IROM1 ");
    n += cc_hex32(txt + n, rom);
    n += cc_str(txt + n, " ");
    n += cc_hex32(txt + n, rom_len);
    n += cc_str(txt + n, "\r\n{\r\nER_IROM1 ");
    n += cc_hex32(txt + n, rom);
    n += cc_str(txt + n, " ");
    n += cc_hex32(txt + n, rom_len);
    /* `*.o (RESET, +First)` 不能省：arkc 的散射校验要求 --vector-table 时布局里
     * 必须有一个装载 RESET 的执行区且它就在镜像基址上，否则直接报
     * "defines no RESET section" 并拒绝出图。打包路径的 sct 是程序自己写的，
     * 曾经漏过这一行：M2 的四个 pass 全部在 pass A 就停住（板上实测）。 */
    n += cc_str(txt + n, " { *.o (RESET, +First) .ANY (+RO) }\r\nER_IRAM1 ");
    n += cc_hex32(txt + n, ram);
    n += cc_str(txt + n, " 0x00002000 { .ANY (+RW +ZI) }\r\n}\r\n");

    return (fs_write_file(CC_PACK_SCT, (const unsigned char *)txt, n) == n) ? 0 : -1;
}

/* 内存对象槽的名字。注册进端口后由 fs_write_file / fs_read_file 直接服务，
 * 既不占 ramfs 槽也不碰 NOR。前缀 "/v/" 不是任何真实挂载点。 */
static const char *const s_cc_pack_obj[CC_PACK_MAX_SRCS] = {
    "/v/obj0", "/v/obj1", "/v/obj2", "/v/obj3"
};

/* 把这次 pass 的对象槽登记进端口。每遍都重登记：上一遍的对象长度必须归零，
 * 否则"没写成功"会读到上一遍的字节，而那就是一个错误答案。 */
static int cc_pack_vfiles(uint8_t *obj_arena, int n_srcs)
{
    int      i;
    uint32_t cap = (uint32_t)(CC_PACK_OBJ_ARENA / (uint32_t)((n_srcs > 0) ? n_srcs : 1));

    if (n_srcs > CC_PACK_MAX_SRCS) {
        return -1;
    }
    arkc_port_vfile_clear();
    for (i = 0; i < n_srcs; i++) {
        if (arkc_port_vfile_register(s_cc_pack_obj[i],
                                     obj_arena + (cap * (uint32_t)i),
                                     cap) != 0) {
            return -1;
        }
    }
    return 0;
}

/* 一份对象编失败 → 一行能定位的说明。单独成函数只为一件事：第一个对象可能
 * 编两遍（先平铺、再带洞），失败分支不能因此抄成两份、更不能只改一份。 */
static void cc_pack_obj_fail(char tag, const char *src, int got)
{
    if (got == -3) {
        ark_shell_printf("cc: pass %c: compiling %s aborted: %s\r\n",
                         tag, src, arkc_port_abort_message());
    } else if (got == -4) {
        ark_shell_printf("cc: pass %c: compiling %s produced no object\r\n",
                         tag, src);
    } else {
        ark_shell_printf("cc: pass %c: compiling %s failed\r\n", tag, src);
    }
    cc_report_pool();
}

/* 一遍里的编译器选项：所有 pass 只有散布文件（CC_PACK_SCT）不同，其余一致，
 * 所以填一次字段列表只写一处，免得两头漂。 */
static void cc_pack_fill(ArkOptions *o, const char *src, const char *out,
                         const char *target, int opt_level, int verbose,
                         const char *const *includes, int n_includes,
                         const char *const *defines, int n_defines,
                         int vector_table)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->input_path   = src;
    o->output_path  = out;
    o->verbose      = verbose;
    o->target       = target;
    o->opt_level    = opt_level;
    /* 四遍的差别就在这份 .sct 上：编译期就按它把 .text/.data 基址烤进代码，
     * 所以对象也只能在这一遍的 .sct 下编译（这正是不能复用 -c 产物的原因）。 */
    o->scatter_file = CC_PACK_SCT;
    /* 向量表属于**镜像**，不属于某个编译单元：每个对象都带 --vector-table 会
     * 让对象在代码最前面各留 0x400 的空洞（实测 a.obj 2516 -> 3540 B），链接
     * 出来的负载里就有两三段全零，入口落进洞里。
     *
     * 但也不是"谁都不带"：arkc 把绝对地址烤进指令，链接器不能再搬动对象，所以
     * 对象必须按真实落点编基址，而**排在第一个**的那个对象要带 --vector-table，
     * 由它腾出 0x400 的空洞，-link 再把表填进去（compiler.c 里的 gen-or-verify）。
     * 洞是 0x400 还是 0 由调用方按"是不是第一个对象"决定。 */
    o->vector_table = vector_table;
    for (i = 0; (i < n_includes) && (i < CC_MAX_INCLUDE_DIRS); i++) {
        o->include_dirs[i] = includes[i];
    }
    o->include_dir_count = n_includes;
    for (i = 0; (i < n_defines) && (i < CC_MAX_DEFINES); i++) {
        o->pre_defines[i] = defines[i];
    }
    o->pre_define_count = n_defines;
}

/* 编译一份源码到它的内存对象。捕获不参与：写入按注册表落进对象槽。
 * 返回 0 成功，-1 编译失败，-3 被 abort（池耗尽）。 */
static int cc_pack_obj(const char *src, const char *obj_name, const char *target,
                       int opt_level, int verbose,
                       const char *const *includes, int n_includes,
                       const char *const *defines, int n_defines,
                       int vector_table, uint32_t obj_cap)
{
    ArkOptions popts;
    jmp_buf    pad;
    int        rc;

    cc_pack_fill(&popts, src, obj_name, target, opt_level, verbose,
                 includes, n_includes, defines, n_defines, vector_table);
    popts.compile_only = 1;

    cc_reset_state(src, opt_level);

    arkc_port_set_abort_target(&pad);
    if (setjmp(pad) != 0) {
        arkc_port_set_abort_target(NULL);
        return -3;
    }
    rc = ark_compile(&popts);
    arkc_port_set_abort_target(NULL);

    if (rc != ARK_OK) {
        /* 只说 "failed" 会把 -6（布局没应用）与 -1（I/O）混成一件事，而这两种
         * 的下一步完全不同；ark_compile 的返回码是唯一能分开它们的读数。 */
        /* 不要一律印 fs_last_error()：它只对 I/O 失败有意义，
         * 而对 codegen/link 这些错误它还是上一次的残留文本
         * ——实测过 "compile rc=-6 (No such file or directory)"，把人往
         * "文件不存在" 上引。所以先把错误码翻成阶段名，
         * 只有 -1（I/O）和 -10（参数）才转 fs_last_error()。 */
        {
            const char *why;
            const char *fse;

            switch (rc) {
            case ARK_ERR_LEX:     why = "lex"; break;
            case ARK_ERR_PARSE:   why = "parse"; break;
            case ARK_ERR_SEMA:    why = "semantic analysis"; break;
            case ARK_ERR_IRGEN:   why = "IR generation"; break;
            case ARK_ERR_CODEGEN: why = "codegen, see the diagnostic above"; break;
            case ARK_ERR_ASM:     why = "assembler"; break;
            case ARK_ERR_LINK:    why = "link, see the diagnostic above"; break;
            case ARK_ERR_OOM:     why = "out of memory"; break;
            default:
                fse = fs_last_error();
                why = (fse != 0) ? fse : "I/O";
                break;
            }
            ark_shell_printf("cc:   %s: compile rc=%d: %s\r\n", src, rc, why);
        }
        /* "File too large" 在打包路径上几乎总是对象没塞进它的槽：
         * 写那一步失败只会得到 -1，看上去像“编译失败”
         * ——这一轮就是这么被误导的。所以把槽大小与它怎么算出来一起打出来。 */
        if ((rc == ARK_ERR_IO) && (fs_last_error() != 0) &&
            (strcmp(fs_last_error(), "File too large") == 0)) {
            ark_shell_printf("cc:   the object does not fit its %u B slot "
                             "(CC_PACK_OBJ_ARENA is split by the source count)\r\n",
                             (unsigned)obj_cap);
        }
        return -1;
    }
    if (arkc_port_vfile_size(obj_name) == 0u) {
        /* 编译成功却什么都没写：对象模式或注册表没接上。拿一个空对象去链接，
         * 得到的是少了这个编译单元的镜像——而且看不出来。 */
        return -4;
    }
    return 0;
}

/* 把这一遍的对象链接成负载，写进 win（捕获）。
 * 返回负载字节数（>0）或负码：-1 链接失败，-2 捕获已被占用，-3 被 abort。 */
static int cc_pack_link(const char *out_name, const char *target, int opt_level,
                        int verbose, const char *const *includes, int n_includes,
                        const char *const *defines, int n_defines,
                        const char *const *objs, int n_objs, uint8_t *win)
{
    ArkOptions popts;
    jmp_buf    pad;
    int        rc;
    int        i;

    cc_pack_fill(&popts, NULL, out_name, target, opt_level, verbose,
                 includes, n_includes, defines, n_defines, 1);
    popts.vector_table     = 1;      /* 镜像前 0x400 的向量表由这次链接生成 */
    popts.link_mode        = 1;
    popts.link_input_count = n_objs;
    for (i = 0; (i < n_objs) && (i < 32); i++) {
        popts.link_inputs[i] = objs[i];
    }

    cc_reset_state("link", opt_level);

    if (arkc_port_capture_begin(win, CC_PACK_WIN_SIZE) != 0) {
        return -2;
    }

    arkc_port_set_abort_target(&pad);
    if (setjmp(pad) != 0) {
        arkc_port_set_abort_target(NULL);
        (void)arkc_port_capture_end();   /* 必须收手，否则下一遍拒绝开始 */
        return -3;
    }
    rc = ark_compile(&popts);
    arkc_port_set_abort_target(NULL);

    if (rc != ARK_OK) {
        (void)arkc_port_capture_end();
        return -1;
    }
    return (int)arkc_port_capture_end();
}

/* 负载头两个字的读数（+0 初始 SP、+4 入口）。打包失败时，报错里说"没有可用
 * 的向量表"，而这两个数正是判断"是表没生成、还是基址不是这一遍的 .sct"的
 * 唯一证据，所以照原样打出来，不做解释也不做修正。 */
static uint32_t cc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cc_pack(const char *const *srcs, int n_srcs,
                   const char *out, const char *target,
                   int opt_level, int verbose,
                   const char *const *includes, int n_includes,
                   const char *const *defines, int n_defines,
                   int *fail_phase)
{
    uint8_t *img[4];
    uint8_t *obj_arena;
    uint8_t *outbuf;
    uint32_t idx = 0u;
    uint32_t code_base = 0u;
    uint32_t code_size = 0u;
    uint32_t ram_base = 0u;
    uint32_t ram_size = 0u;
    uint32_t len[4];
    uint32_t len4;
    uint32_t out_len = 0u;
    uint32_t n_ent;
    uint32_t t0;
    int32_t  rc;
    int      i;
    int      k;
    int      got;
    /* 第一个对象要不要留 0x400 的向量表洞（见下面 want_vt）。vt_known 是
     * 「已经问过第一个对象了」：四遍里只问一次，四遍都用同一个答案，
     * 否则同一份源码在四遍里落进不同的布局，差分就不再是同一个程序的差分。 */
    int      vt_first = 0;
    int      vt_known = 0;
    int      ret = 1;
    /* 失败落在哪一段。只在这里记，最后一次性交给调用方：调用方（ark make）
     * 有它自己的错误词汇，把"编译时池耗尽"翻成"link failed"就是错答案。 */
    int      phase = ARKC_PACK_PHASE_SETUP;
    arkrtos_apppack_win_t   win;
    arkrtos_apppack_stats_t st;

    for (i = 0; i < 4; i++) {
        img[i] = NULL;
        len[i] = 0u;
    }
    memset(&st, 0, sizeof(st));

#if (ARKRTOS_PACK_IN_EXTRAM == 1)
    /* SDRAM 档：缓冲是静态的，不借、也不还。idx 保持 -1，归还路径据此跳过。 */
    idx = -1;
    code_base = 0u;
    code_size = 0u;
    ram_base  = (uint32_t)(uintptr_t)s_cc_pack_extram;
    ram_size  = CC_PACK_EXTRAM_SIZE;
    for (i = 0; i < 4; i++) {
        img[i] = &s_cc_pack_extram[(uint32_t)i * CC_PACK_WIN_SIZE];
    }
    obj_arena = &s_cc_pack_extram[CC_PACK_ARENA];
    outbuf    = obj_arena;
#else
    /* One 32 KB code block holds the four payloads, one 16 KB RAM block holds
     * the object slots and then the image.  The two blocks are lent and
     * returned together (that is the only atomic lend the pool has). */
    rc = arkrtos_ptable_mini_alloc(CC_PACK_ARENA, CC_PACK_OUT_CAP, &idx,
                                   &code_base, &code_size, &ram_base, &ram_size);
    if (rc != 0) {
        ark_shell_printf("cc: --pack needs %u KB + %u KB from the RAM pool; "
                         "nothing free (the lend is refused, not shrunk)\r\n",
                         (unsigned)(CC_PACK_ARENA / 1024u),
                         (unsigned)(CC_PACK_OUT_CAP / 1024u));
        return 1;
    }
    if ((code_size < CC_PACK_ARENA) || (ram_size < CC_PACK_OUT_CAP)) {
        /* The pool rounds up to powers of two, so it only ever returns more.
         * Less means the bookkeeping and the allocation disagree, and writing
         * anyway would trample somebody else's block. */
        (void)arkrtos_ptable_mini_free(idx);
        ark_shell_printf("cc: --pack got smaller blocks than it asked for "
                         "(%u/%u B)\r\n", (unsigned)code_size, (unsigned)ram_size);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        img[i] = (uint8_t *)(void *)(code_base + ((uint32_t)i * CC_PACK_WIN_SIZE));
    }
    /* Same 16 KB, two uses in sequence: objects during the passes, image
     * after them (see the #error above CC_PACK_ARENA). */
    obj_arena = (uint8_t *)(void *)(ram_base);
    outbuf = (uint8_t *)(void *)(ram_base);
#endif

    t0 = platform_tick_ms();
    if (n_srcs == 1) {
        ark_shell_printf("cc: pack %s -> %s (4 passes, -O%d)\r\n",
                         srcs[0], out, opt_level);
    } else {
        ark_shell_printf("cc: pack %d sources -> %s (4 passes, -O%d)\r\n",
                         n_srcs, out, opt_level);
    }

    for (i = 0; i < 4; i++) {
        /* 这一遍的基址，以及"下一个对象从哪开始"。offset 只能在这个循环里累加：
         * 每个对象都必须按它最终的落点编基址（见下），所以顺序是硬约束。 */
        uint32_t rom = CC_PACK_ROM_BASE + s_cc_pack_rom_add[i];
        uint32_t ram = CC_PACK_RAM_BASE + s_cc_pack_ram_add[i];
        uint32_t off = 0u;
        phase = ARKC_PACK_PHASE_COMPILE;

        if (cc_pack_vfiles(obj_arena, n_srcs) != 0) {
            ark_shell_printf("cc: cannot register the in-memory object files "
                             "(the registry refused)\r\n");
            goto done;
        }
        /* Objects first: every source of this pass, then one link.
         *
         * Each object is compiled against the base it will really be loaded at.
         * arkc bakes absolute addresses into the code (movw/movt, literal
         * slots) and only external symbols become relocations, so the linker
         * cannot move an object afterwards -- a concatenation would leave one
         * TU's string literals pointing into another TU's bytes.  实测过：不按
         * 落点编基址时，b.c 的两条字符串指针落在 0x08000000/0x08000014，
         * 程序装载、打印不出任何东西、立刻退出。
         *
         * 第一个对象带不带 --vector-table 不是一个常量，取决于**它自己**：
         *
         *   定义了 Reset_Handler ⇒ 这是应用镜像，Cortex-M 从 +0 取 SP、+4 取
         *   入口，所以它要腾出 0x400 的空洞，-link 再把表填进去（不位移镜像，
         *   位移会让烤死的地址全部偏 0x400）。
         *
         *   没定义 ⇒ 这是平铺的小程序：它没有复位向量（被内核当任务起来，栈由
         *   mini run 从 RAM 块给，见 arkrtos_mini.c），入口就是负载 +0 的启动桩。
         *   给它留洞的结果是负载白长 0x400、洞没人填，打包时还得靠「+0 是不是
         *   启动桩的序言」去认入口——那不是判定，那是猜。
         *
         * 判断依据只能是刚编完那份对象的符号表（arkc 的标签表），所以第一遍的
         * 第一个对象先按平铺编一次、问一句、需要洞再重编一次。只有应用型打包
         * 会付这一遍，小程序不多编。 */
        for (k = 0; k < n_srcs; k++) {
            int want_vt = 0;

            if (cc_pack_write_sct(rom + off, CC_PACK_WIN_SIZE - off, ram) != 0) {
                ark_shell_printf("cc: cannot write %s: %s\r\n",
                                 CC_PACK_SCT, fs_last_error());
                goto done;
            }
            if ((k == 0) && (vt_known != 0)) {
                want_vt = vt_first;
            }
            got = cc_pack_obj(srcs[k], s_cc_pack_obj[k], target, opt_level, verbose,
                              includes, n_includes, defines, n_defines, want_vt,
                              (uint32_t)(CC_PACK_OBJ_ARENA / (uint32_t)n_srcs));
            if ((got >= 0) && (k == 0) && (vt_known == 0)) {
                vt_first = (arkc_port_last_object_has_symbol("Reset_Handler") != 0) ? 1 : 0;
                vt_known = 1;
                if (vt_first != 0) {
                    /* 应用镜像：按带洞的布局重编这一个对象。它变长 0x400，而
                     * 下一个对象的基址正是按它的 code_size 累加的，所以后面的
                     * 对象会自动落到洞之后——顺序不能调。 */
                    want_vt = 1;
                    got = cc_pack_obj(srcs[0], s_cc_pack_obj[0], target, opt_level,
                                      verbose, includes, n_includes, defines,
                                      n_defines, want_vt,
                                      (uint32_t)(CC_PACK_OBJ_ARENA / (uint32_t)n_srcs));
                }
            }
            if (got < 0) {
                cc_pack_obj_fail(s_cc_pack_tag[i], srcs[k], got);
                goto done;
            }
            {
                /* 对象的**长度**要用它自己头里的 code_size，不是文件长度：
                 * ARKOBJ2 还带着符号表和重定位表（实测 a.obj 文件 3540 B，
                 * 其中的代码只有 2590 B），链接器排布用的是代码长度。拿文件
                 * 长度累加会让下一个对象被编到 950 B 之外的基址上——那是
                 * "对象被搬了位置"，正是这一段要防的事。
                 *
                 * 长度由端口从**注册时那个指针**读（arkc_port_vfile_code_size），
                 * 不在这里按槽位偏移自己再算一遍：板上实测过两处算法一漂，
                 * 读到的就是隔壁槽的字节，第三槽读成 0、报"对象 0 字节"。 */
                uint32_t sz = arkc_port_vfile_code_size(s_cc_pack_obj[k]);
                uint32_t file_sz = (uint32_t)arkc_port_vfile_size(s_cc_pack_obj[k]);

                if (verbose != 0) {
                    ark_shell_printf("cc:   %c obj%d %u B code (%u B file) at +0x%X <- %s\r\n",
                                     s_cc_pack_tag[i], k, (unsigned)sz,
                                     (unsigned)file_sz, (unsigned)off, srcs[k]);
                }
                if ((sz == 0u) || (sz > (CC_PACK_WIN_SIZE - off))) {
                    /* 两种原因分开说：头读不出来（0）和对象比窗口还大是两件事，
                     * 混成一句"does not fit"会把人指向错误的方向。 */
                    if (sz == 0u) {
                        ark_shell_printf("cc: pass %c: %s produced no readable "
                                         "ARKOBJ2 object (%u B file): the object "
                                         "header is missing or was overwritten\r\n",
                                         s_cc_pack_tag[i], srcs[k], (unsigned)file_sz);
                    } else {
                        ark_shell_printf("cc: pass %c: %s produced %u B at +0x%X, "
                                         "which does not fit the %u B window\r\n",
                                         s_cc_pack_tag[i], srcs[k], (unsigned)sz,
                                         (unsigned)off, (unsigned)CC_PACK_WIN_SIZE);
                    }
                    goto done;
                }
                off += sz;
            }
        }
        phase = ARKC_PACK_PHASE_LINK;
        /* 链接用这一遍的**基址**布局：-link 只按它算 flash_base/SP，并把向量表
         * 填进第一个对象腾出的洞里。 */
        if (cc_pack_write_sct(rom, CC_PACK_WIN_SIZE, ram) != 0) {
            ark_shell_printf("cc: cannot write %s: %s\r\n", CC_PACK_SCT, fs_last_error());
            goto done;
        }
        got = cc_pack_link(s_cc_pack_out[i], target, opt_level, verbose,
                           includes, n_includes, defines, n_defines,
                           s_cc_pack_obj, n_srcs, img[i]);
        if (got < 0) {
            if (got == -2) {
                ark_shell_printf("cc: pass %c: a capture is already armed\r\n",
                                 s_cc_pack_tag[i]);
            } else if (got == -3) {
                ark_shell_printf("cc: pass %c aborted: %s\r\n", s_cc_pack_tag[i],
                                 arkc_port_abort_message());
                cc_report_pool();
            } else {
                ark_shell_printf("cc: pass %c failed\r\n", s_cc_pack_tag[i]);
                cc_report_pool();
            }
            goto done;
        }
        if (got == 0) {
            /* Linking succeeded but wrote nothing: either -o was dropped or
             * the capture is not wired to the write.  Neither can be packed. */
            ark_shell_printf("cc: pass %c produced no output bytes\r\n",
                             s_cc_pack_tag[i]);
            goto done;
        }
        if (((uint32_t)got < off) || (((uint32_t)got - off) > 16u)) {
            /* 链接产物的长度必须等于各对象长度之和：不相等说明有对象被链接器
             * 搬了位置，而它的代码里烤死的是原来的基址——这种镜像能装载、能跑，
             * 但每一处地址都是错的。宁可在打包时停下。 */
            ark_shell_printf("cc: pass %c: linked %u B but the objects total %u B: "
                             "an object was placed at a base its code cannot follow "
                             "(the only legal tail is the 2..16 B __arkcc_init_globals "
                             "stub/trampoline)\r\n",
                             s_cc_pack_tag[i], (unsigned)got, (unsigned)off);
            goto done;
        }
        len[i] = (uint32_t)got;
        diag_summary();
        ark_shell_printf("cc:   %c %u B\r\n", s_cc_pack_tag[i], (unsigned)len[i]);
    }

    phase = ARKC_PACK_PHASE_LAYOUT;
    if ((len[0] != len[1]) || (len[0] != len[2]) || (len[0] != len[3])) {
        /* A size that depends on the base means the link result depends on the
         * base, and a table derived from that is meaningless. */
        ark_shell_printf("cc: the four passes disagree on size (%u/%u/%u/%u): "
                         "the link result depends on the base, no table possible\r\n",
                         (unsigned)len[0], (unsigned)len[1],
                         (unsigned)len[2], (unsigned)len[3]);
        goto done;
    }

    /* The diff works on whole words, so pad to a multiple of four.  All four
     * are padded with 0: the padding word is identical in every pass and can
     * never be mistaken for an absolute address. */
    len4 = (len[0] + 3u) & ~3u;
    if ((len4 == 0u) || (len4 > CC_PACK_WIN_SIZE)) {
        ark_shell_printf("cc: image %u B does not fit the %u B window\r\n",
                         (unsigned)len[0], (unsigned)CC_PACK_WIN_SIZE);
        goto done;
    }
    for (i = 0; i < 4; i++) {
        uint32_t q;
        for (q = len[i]; q < len4; q++) {
            img[i][q] = 0u;
        }
    }
    if (vt_first != 0) {
        ark_shell_printf("cc:   A vector: SP 0x%X entry 0x%X (rom base 0x%X, "
                         "window %u B)\r\n",
                         (unsigned)cc_le32(img[0]),
                         (unsigned)(cc_le32(img[0] + 4u) & ~1u),
                         (unsigned)CC_PACK_ROM_BASE, (unsigned)CC_PACK_WIN_SIZE);
    } else {
        /* 平铺镜像的头两个字是代码，不是 SP/入口：照原来的话打出来会写成
         * "SP 0x0 entry 0x0"，看实录的人会以为入口丢了。这里打真实的第一条
         * 指令，入口位置由上一行的 entry mode 说明。 */
        ark_shell_printf("cc:   A +0 is 0x%04X (flat image, no vector table; "
                         "rom base 0x%X, window %u B)\r\n",
                         (unsigned)(cc_le32(img[0]) & 0xFFFFu),
                         (unsigned)CC_PACK_ROM_BASE, (unsigned)CC_PACK_WIN_SIZE);
    }

    /* 把入口形态打出来：这一段决定 -link 填不填表、apppack 从哪读入口，
     * 成败之后回看实录时要能一眼看出走的是哪一条。 */
    ark_shell_printf("cc:   entry mode: %s\r\n",
                     (vt_first != 0) ? "vector table at +0 (Reset_Handler found)"
                                     : "flat miniapp, entry at +0 (no Reset_Handler)");

    win.rom_base = CC_PACK_ROM_BASE;
    win.rom_size = CC_PACK_WIN_SIZE;
    win.ram_base = CC_PACK_RAM_BASE;
    win.ram_size = CC_PACK_WIN_SIZE;

    /* The passes are over: the object registry is no longer read, so the
     * image may take over the block the objects lived in. */
    arkc_port_vfile_clear();
    phase = ARKC_PACK_PHASE_PACK;
    rc = arkrtos_apppack_build(img[0], img[1], img[2], img[3], len4,
                               CC_PACK_DELTA, &win, ARKRTOS_APP_TYPE_MINIAPP, 1u,
                               outbuf, CC_PACK_OUT_CAP, &out_len, &st);
    if (rc != 0) {
        ark_shell_printf("cc: pack failed: %s (%d)\r\n",
                         arkrtos_apppack_error_name(rc), (int)rc);
        goto done;
    }
    phase = ARKC_PACK_PHASE_WRITE;
    if (fs_write_file(out, outbuf, (int)out_len) != (int)out_len) {
        ark_shell_printf("cc: cannot write %s: %s\r\n", out, fs_last_error());
        goto done;
    }

    n_ent = st.n_rom + st.n_ram + st.n_rom_movw + st.n_ram_movw;
    ark_shell_printf("cc: %s: payload %u + table %u x 4 = %u B\r\n",
                     out, (unsigned)st.image_size, (unsigned)n_ent,
                     (unsigned)out_len);
    /* The four kinds are reported separately on purpose: a class that stays 0
     * means this program never exercised it, so that class is unverified here
     * (that is what the anti-false-green checks look at). */
    ark_shell_printf("cc:   reloc rom %u / ram %u / rom_movw %u / ram_movw %u, "
                     "entry +0x%X\r\n",
                     (unsigned)st.n_rom, (unsigned)st.n_ram,
                     (unsigned)st.n_rom_movw, (unsigned)st.n_ram_movw,
                     (unsigned)st.entry_offset);
    ark_shell_printf("cc: packed in %u ms\r\n",
                     (unsigned)(platform_tick_ms() - t0));
    ret = 0;

done:
    /* Return before anything else: an unreturned lend makes the next pack or
     * mini run fail.  The temporary .sct goes too, or it, the source and the
     * image together take all three ramfs slots.  The registry is cleared so
     * the next run cannot read this run's objects. */
    arkc_port_vfile_clear();
#if (ARKRTOS_PACK_IN_EXTRAM == 1)
    /* 静态缓冲，没有借出可还。 */
#else
    (void)arkrtos_ptable_mini_free(idx);
#endif
    (void)arkrtos_vfs_remove(CC_PACK_SCT);
    if (fail_phase != NULL) {
        *fail_phase = (ret == 0) ? ARKC_PACK_PHASE_NONE : phase;
    }
    return ret;
}
#endif /* ARKRTOS_USE_APPPACK */

#if (ARKRTOS_USE_APPPACK == 1)
/* 给 ark make 的入口：同一份 cc_pack，只是把宏与头文件目录显式传进来。 */
int arkc_cc_pack(const char *const *srcs, int n_srcs, const char *out,
                 const char *const *defs, int n_defs,
                 const char *const *incs, int n_incs,
                 int opt_level, int verbose, int *fail_phase)
{
    if (fail_phase != NULL) {
        *fail_phase = ARKC_PACK_PHASE_NONE;
    }
    if ((srcs == NULL) || (n_srcs <= 0) || (out == NULL)) {
        /* 参数就不成立：还没进任何一段。 */
        if (fail_phase != NULL) {
            *fail_phase = ARKC_PACK_PHASE_SETUP;
        }
        return 1;
    }
    return cc_pack(srcs, n_srcs, out, "arm", opt_level, verbose,
                   incs, n_incs, defs, n_defs, fail_phase);
}
#endif

static int cmd_cc(int argc, char *argv[])
{
    ArkOptions opts;
    jmp_buf    pad;
    int        i;
    int        rc;
    uint8_t    verbose = 0u;
    const char *src = NULL;
#if (ARKRTOS_USE_APPPACK == 1)
    /* --pack compiles several translation units; everything else is still a
     * single-input command.  Keeping the list here (not in the packer) is what
     * lets the parser stay one loop.  Guarded so a build without --pack does
     * not even carry these two locals. */
    const char *srcs[CC_PACK_MAX_SRCS];
    int        n_srcs = 0;
#endif
    const char *out = NULL;
    const char *target = "arm";
#if (CC_LAYOUT_OPTS == 1)
    /* Only the app-run window needs a caller-chosen layout on the target, so
     * this option is part of that feature: a build without `app run` keeps a
     * byte-identical cc command (and image) instead of carrying an option it
     * cannot use. */
    const char *scatter = NULL;
    /* -link produces the image `app run` consumes: one bare file placed at the
     * window addresses.  Upstream `-link` collects every following non-option
     * argument as an input, and so does this, so `cc -link a.obj -o x.bin`
     * means the same thing on the board as on the host. */
    int        link_mode = 0;
    const char *link_inputs[32];
    int        link_input_count = 0;
    /* --vector-table: emit the table into a reserved 0x400 gap instead of
     * prepending it, so the linked addresses stay the ones that run. */
    int        vector_table = 0;
    /* --reloc-text: -1 = not on the command line (decided after parsing:
     * on for -link, off otherwise).  0/1 = what the caller asked for. */
    int        reloc_text = -1;
#endif
#if (ARKRTOS_USE_APPPACK == 1)
    /* --pack: same source, four layouts, one relocatable image.  Left at 0
     * the rest of cmd_cc behaves exactly as before this option existed. */
    int        pack_mode = 0;
#endif
    int        emit_asm = 0;
    int        compile_only = 0;
    int        preprocess_only = 0;
    int        opt_level = 1;
    const char *defines[CC_MAX_DEFINES];
    const char *includes[CC_MAX_INCLUDE_DIRS];
    int        n_defines = 0;
    int        n_includes = 0;
    uint32_t   t0;

    if (argc < 2) {
        cc_usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#if (CC_LAYOUT_OPTS == 1)
        if ((link_mode != 0) && (a[0] != '-')) {
            /* Objects after -link.  A second bare file used to be an error
             * ("more than one input file"); in link mode it is the point. */
            if (link_input_count >= (int)(sizeof(link_inputs) / sizeof(link_inputs[0]))) {
                ark_shell_printf("cc: more than %d -link inputs\r\n",
                                 (int)(sizeof(link_inputs) / sizeof(link_inputs[0])));
                return 1;
            }
            link_inputs[link_input_count++] = a;
            continue;
        }
#endif
        if (a[0] != '-') {
#if (ARKRTOS_USE_APPPACK == 1)
            /* Collected unconditionally: whether a second file is legal is
             * decided after the whole line is parsed (only --pack accepts
             * more than one), instead of depending on the option order. */
            if (n_srcs >= CC_PACK_MAX_SRCS) {
                ark_shell_printf("cc: more than %d input files\r\n",
                                 CC_PACK_MAX_SRCS);
                return 1;
            }
            srcs[n_srcs++] = a;
#else
            if (src != NULL) {
                ark_shell_printf("cc: more than one input file ('%s' after '%s')\r\n",
                                 a, src);
                return 1;
            }
#endif
            src = a;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            cc_usage();
            return 0;
        } else if (strcmp(a, "-v") == 0) {
            verbose = 1u;
        } else if (strcmp(a, "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(a, "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(a, "-E") == 0) {
            preprocess_only = 1;
        } else if (strcmp(a, "-o") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -o needs a file name\r\n");
                return 1;
            }
            out = argv[++i];
        } else if (strcmp(a, "-target") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -target needs a name\r\n");
                return 1;
            }
            target = argv[++i];
#if (CC_LAYOUT_OPTS == 1)
        } else if (strcmp(a, "--vector-table") == 0) {
            vector_table = 1;
        } else if (strcmp(a, "--reloc-text") == 0) {
            reloc_text = 1;
        } else if (strcmp(a, "--no-reloc-text") == 0) {
            reloc_text = 0;
        } else if (strcmp(a, "-link") == 0) {
            /* Turns the following bare arguments into inputs; nothing else is
             * needed here because the branch above already routes them. */
            link_mode = 1;
        } else if (strcmp(a, "--scatter") == 0) {
            /* Link for a given layout.  Without it the image is linked at the
             * ARM default bases, which is not where an app-run window sits, so
             * the program would be correct code at the wrong addresses. */
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: --scatter needs a file name\r\n");
                return 1;
            }
            scatter = argv[++i];
#if (ARKRTOS_USE_APPPACK == 1)
        } else if (strcmp(a, "--pack") == 0) {
            pack_mode = 1;
#endif
#endif
        } else if (strcmp(a, "-I") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -I needs a directory\r\n");
                return 1;
            }
            if (n_includes >= CC_MAX_INCLUDE_DIRS) {
                ark_shell_printf("cc: more than %d -I directories\r\n",
                                 CC_MAX_INCLUDE_DIRS);
                return 1;
            }
            includes[n_includes++] = argv[++i];
        } else if ((a[0] == '-') && (a[1] == 'O') &&
                   (a[2] >= '0') && (a[2] <= '2') && (a[3] == '\0')) {
            opt_level = a[2] - '0';
        } else if ((a[0] == '-') && (a[1] == 'D')) {
            const char *d = a + 2;
            if (d[0] == '\0') {
                /* "-D NAME" (space form) is what armcc accepts too. */
                if ((i + 1) >= argc) {
                    ark_shell_printf("cc: -D needs a macro name\r\n");
                    return 1;
                }
                d = argv[++i];
            }
            if (n_defines >= CC_MAX_DEFINES) {
                ark_shell_printf("cc: more than %d -D macros\r\n", CC_MAX_DEFINES);
                return 1;
            }
            defines[n_defines++] = d;
        } else {
            return cc_bad_option("option", a);
        }
    }

#if (CC_LAYOUT_OPTS == 1)
    if (link_mode != 0) {
        /* Refuse a source file instead of quietly compiling it: -link only
         * reads object files, and a `.c` handed to it would be copied
         * verbatim into the image (seen on the host: 676 bytes of source). */
        if (src != NULL) {
            ark_shell_printf("cc: -link takes objects, not source ('%s')\r\n", src);
            return 1;
        }
        if (link_input_count == 0) {
            ark_shell_printf("cc: -link needs at least one .obj\r\n");
            cc_usage();
            return 1;
        }
    } else
#endif
    if (src == NULL) {
        ark_shell_printf("cc: no input file\r\n");
        cc_usage();
        return 1;
    }
#if (ARKRTOS_USE_APPPACK == 1)
    if ((pack_mode == 0) && (n_srcs > 1)) {
        ark_shell_printf("cc: more than one input file ('%s' after '%s'); "
                         "only --pack links several sources\r\n",
                         srcs[n_srcs - 1], srcs[0]);
        return 1;
    }
#endif

#if (ARKRTOS_USE_APPPACK == 1)
    if (pack_mode != 0) {
        /* -S/-c/-E change what the compiler emits; -link/--scatter/
         * --vector-table decide the layout, and --pack decides it itself.
         * Accepting any of them would produce an image whose table describes
         * something the caller did not ask for. */
        if ((emit_asm != 0) || (compile_only != 0) || (preprocess_only != 0)) {
            ark_shell_printf("cc: --pack links an image; it takes no -S/-c/-E\r\n");
            return 1;
        }
#if (ARKRTOS_USE_APPRUN == 1)
        if ((link_mode != 0) || (scatter != NULL) || (vector_table != 0)) {
            ark_shell_printf("cc: --pack chooses its own layout; "
                             "drop -link/--scatter/--vector-table\r\n");
            return 1;
        }
#endif
        if (out == NULL) {
            /* No default name: a stray hello.svcm next to a hello.bin is
             * exactly the kind of file the user then installs by mistake. */
            ark_shell_printf("cc: --pack needs -o <file.svcm> (no default name)\r\n");
            return 1;
        }
        /* cc 命令自己不看阶段：它把 cc: 那几行原样打给用户，阶段只对
         * ark make 那种要翻成自己错误码的调用方有用。 */
        return cc_pack(srcs, n_srcs, out, target, opt_level, (int)verbose,
                       includes, n_includes, defines, n_defines, NULL);
    }
#endif
    /* The upstream STM32 entry point zeros this struct by hand because a
     * bare-metal build has no memset; a GCC build does, so memset here is
     * both shorter and complete. */
    memset(&opts, 0, sizeof(opts));
    opts.input_path     = src;
    opts.output_path    = out;
    opts.emit_asm       = emit_asm;
    opts.compile_only   = compile_only;
    opts.preprocess_only = preprocess_only;
    opts.verbose        = (int)verbose;
    opts.target         = target;
    opts.opt_level      = opt_level;
#if (CC_LAYOUT_OPTS == 1)
    opts.scatter_file   = scatter;
    opts.vector_table   = vector_table;
    if (link_mode != 0) {
        opts.link_mode        = 1;
        opts.link_input_count = link_input_count;
        for (i = 0; i < link_input_count; i++) {
            opts.link_inputs[i] = link_inputs[i];
        }
    }
#endif
    for (i = 0; i < n_includes && i < 16; i++) {
        opts.include_dirs[i] = includes[i];
    }
    opts.include_dir_count = n_includes;
    for (i = 0; i < n_defines && i < 64; i++) {
        opts.pre_defines[i] = defines[i];
    }
    opts.pre_define_count = n_defines;

    cc_reset_state(src, opt_level);
#if (CC_LAYOUT_OPTS == 1)
    /* After cc_reset_state(): config_init() rebuilds g_config, so a value set
     * before it is thrown away.  On board, -link always joins objects that
     * were compiled against another base, so correcting the in-section
     * absolute addresses is the only honest default; --no-reloc-text opts out
     * and reproduces the pre-0.8.0 output. */
    if (reloc_text < 0) {
        reloc_text = (link_mode != 0) ? 1 : 0;
    }
    g_config.enable_reloc_text = reloc_text;
#endif

#if (CC_LAYOUT_OPTS == 1)
    if (link_mode != 0) {
        ark_shell_printf("cc: link %d obj -> %s\r\n",
                         link_input_count,
                         (out != NULL) ? out : "(default)");
    } else
#endif
    ark_shell_printf("cc: %s -> %s (target %s, -O%d)%s\r\n",
                     src,
                     (out != NULL) ? out : "(default)",
                     target, opt_level,
                     (emit_asm != 0) ? ", -S" :
                     (compile_only != 0) ? ", -c" :
                     (preprocess_only != 0) ? ", -E" : "");
#if (CC_LAYOUT_OPTS == 1)
    if (scatter != NULL) {
        /* Name the layout on the same line as the files: which base a binary
         * was linked for is the first thing to check when it will not run. */
        ark_shell_printf("cc: scatter %s\r\n", scatter);
    }
    if (vector_table != 0) {
        ark_shell_printf("cc: vector table in the first 0x400 B\r\n");
    }
    if (link_mode != 0) {
        ark_shell_printf("cc: reloc-text %s\r\n", (reloc_text != 0) ? "on" : "off");
    }
#endif

    t0 = platform_tick_ms();
    arkc_port_set_abort_target(&pad);
    if (setjmp(pad) != 0) {
        /* Came back from ark_abort(). */
        /* Disarm before returning: this frame is about to go away, and a
         * pad pointing into a dead frame would corrupt the task on the next
         * fatal error. */
        arkc_port_set_abort_target(NULL);
        ark_shell_printf("cc: aborted: %s (%u ms)\r\n",
                         arkc_port_abort_message(),
                         (unsigned)(platform_tick_ms() - t0));
        cc_report_pool();
        return 1;
    }
    rc = ark_compile(&opts);
    arkc_port_set_abort_target(NULL);

    diag_summary();

    if (rc == ARK_OK) {
        cc_report_pool();
        ark_shell_printf("cc: done in %u ms\r\n",
                         (unsigned)(platform_tick_ms() - t0));
        return 0;
    }
    ark_shell_printf("cc: failed with code %d in %u ms\r\n",
                     rc, (unsigned)(platform_tick_ms() - t0));
    return 1;
}


int arkc_shell_register(int idx)
{
    if ((idx < 0) || ((idx + 1) > ARK_SHELL_MAX_COMMANDS)) {
        platform_uart_send_string("shell: no room for the cc command\r\n");
        return idx;
    }
    /* max_args counts the command name too, so 2 would allow only
     * "cc file.c" and reject every option as "too many arguments".  The
     * real limit is the shell's own argv array; cmd_cc reports what it
     * cannot honour. */
    g_cmd_table[idx++] = ARK_SHELL_CMD("cc", cmd_cc,
#if (CC_LAYOUT_OPTS == 1) && (ARKRTOS_USE_APPPACK == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--scatter f.sct] [--vector-table] [--pack -o x.svcm]",
#elif (CC_LAYOUT_OPTS == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--scatter f.sct] [--vector-table]",
#elif (ARKRTOS_USE_APPPACK == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--pack -o x.svcm]",
#else
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]",
#endif
        ARK_SHELL_MAX_ARGS);
    return idx;
}
