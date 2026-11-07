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

#if (ARKRTOS_USE_APPPACK == 1) || (ARKRTOS_USE_HELLO == 1)
#include "arkrtos_vfs.h"           /* removes the temporary .sct again */
#endif
#if (ARKRTOS_USE_APPPACK == 1)
#include "include/flash_fs.h"      /* fs_write_file / fs_last_error */
#include "arkrtos_apppack.h"       /* the four-pass diff itself (pure) */
#include "arkrtos_ptable.h"        /* four payloads + the image come from the pool */
#endif
#if (ARKRTOS_USE_HELLO == 1)
#include "arkrtos_apprun.h"        /* the demo hands its image to 'app run' */
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
 * pool and returned on every path.
 * ============================================================ */

/* One payload may not exceed the app-run code window; four of them are one
 * 32 KB block of the pool. */
#define CC_PACK_WIN_SIZE     8192u
/* 256 header + 1024 x 4 table + 8192 payload = 12544 -> one 16 KB block. */
#define CC_PACK_OUT_CAP      16384u
/* Objects of one pass, one slot per source. */
#define CC_PACK_MAX_SRCS     4
/* 对象槽不是每槽固定 4 KB：一个 3 函数的对象带上 0x400 的向量表洞实测 4247 B
 * （arkc 0.8.0 十项限制同步之后涨到这里，之前约 3.2 KB），固定 4 KB 会在**写
 * 对象**这一步失败，而写失败只报 -1，看上去像「编译失败」。整块只有 16 KB
 * （它和镜像缓冲共用，见下），所以容量按这次真正编译的源数均分：1 个源时一槽
 * 16 KB，4 个源时每槽 4 KB。超出就照实报 File too large，不截断。 */
#define CC_PACK_OBJ_ARENA    CC_PACK_OUT_CAP            /* 与镜像缓冲共用 */
/* 池一次借出的两块各自都要落进 [MIN, MAX]（MAX = SLOT_RAM_MAX_BLOCK = 32 KB），
 * 所以「32 KB 负载窗口 + 16 KB 对象槽」不能合成一次借出（48 KB 上取整成 64 KB，
 * 直接被拒）；而 SLOT_RAM_TOTAL 只有 64 KB，app run 的 16 KB 窗口平时就占着，
 * 于是借出总量只能是 32 + 16 = 48 KB，两块都不能再涨。
 * 对象槽因此和镜像缓冲共用那 16 KB：镜像只在**四遍都跑完之后**才写进去，
 * 而对象在最后一遍链接完成后就不再被读，用途在时间上不重叠。这条依赖写成
 * 下面的 #error 而不是注释——顺序一旦被改动，这里先失败。 */
#if (CC_PACK_OBJ_ARENA > CC_PACK_OUT_CAP)
#error "object slots share the image buffer; they must fit inside it"
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
                       int vector_table)
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
        ark_shell_printf("cc:   %s: compile rc=%d (%s)\r\n", src, rc,
                         fs_last_error());
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
         * 第一个对象带 --vector-table：它腾出 0x400 的空洞，-link 把向量表填
         * 进去（不位移镜像，位移会让烤死的地址全部偏 0x400）。 */
        for (k = 0; k < n_srcs; k++) {
            if (cc_pack_write_sct(rom + off, CC_PACK_WIN_SIZE - off, ram) != 0) {
                ark_shell_printf("cc: cannot write %s: %s\r\n",
                                 CC_PACK_SCT, fs_last_error());
                goto done;
            }
            got = cc_pack_obj(srcs[k], s_cc_pack_obj[k], target, opt_level, verbose,
                              includes, n_includes, defines, n_defines,
                              (k == 0) ? 1 : 0);
            if (got < 0) {
                if (got == -3) {
                    ark_shell_printf("cc: pass %c: compiling %s aborted: %s\r\n",
                                     s_cc_pack_tag[i], srcs[k],
                                     arkc_port_abort_message());
                } else if (got == -4) {
                    ark_shell_printf("cc: pass %c: compiling %s produced no object\r\n",
                                     s_cc_pack_tag[i], srcs[k]);
                } else {
                    ark_shell_printf("cc: pass %c: compiling %s failed\r\n",
                                     s_cc_pack_tag[i], srcs[k]);
                }
                cc_report_pool();
                goto done;
            }
            {
                /* 对象的**长度**要用它自己头里的 code_size，不是文件长度：
                 * ARKOBJ2 还带着符号表和重定位表（实测 a.obj 文件 3540 B，
                 * 其中的代码只有 2590 B），链接器排布用的是代码长度。拿文件
                 * 长度累加会让下一个对象被编到 950 B 之外的基址上——那是
                 * "对象被搬了位置"，正是这一段要防的事。 */
                uint32_t sz = cc_le32(obj_arena +
                                      ((CC_PACK_OBJ_ARENA / (uint32_t)n_srcs) * (uint32_t)k) + 8u);
                uint32_t file_sz = (uint32_t)arkc_port_vfile_size(s_cc_pack_obj[k]);

                if (verbose != 0) {
                    ark_shell_printf("cc:   %c obj%d %u B code (%u B file) at +0x%X <- %s\r\n",
                                     s_cc_pack_tag[i], k, (unsigned)sz,
                                     (unsigned)file_sz, (unsigned)off, srcs[k]);
                }
                if ((sz == 0u) || (sz > (CC_PACK_WIN_SIZE - off))) {
                    ark_shell_printf("cc: pass %c: %s produced %u B at +0x%X, "
                                     "which does not fit the %u B window\r\n",
                                     s_cc_pack_tag[i], srcs[k], (unsigned)sz,
                                     (unsigned)off, (unsigned)CC_PACK_WIN_SIZE);
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
    ark_shell_printf("cc:   A vector: SP 0x%X entry 0x%X (rom base 0x%X, "
                     "window %u B)\r\n",
                     (unsigned)cc_le32(img[0]),
                     (unsigned)(cc_le32(img[0] + 4u) & ~1u),
                     (unsigned)CC_PACK_ROM_BASE, (unsigned)CC_PACK_WIN_SIZE);

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
    (void)arkrtos_ptable_mini_free(idx);
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


#if (ARKRTOS_USE_HELLO == 1)
/* ============================================================
 * `hello` - one-command POSIX hello world demo
 *
 * Three steps behind one word: the sources land in the VFS /tmp, the
 * on-board arkc compiles and links them, and `app run` executes the
 * image in the fixed window.  It is one command instead of three
 * because on a serial console "forgot the second line" and "typed it
 * wrong" look exactly alike; every step reports its own failure with
 * the step number and the demo stops there.
 *
 * The program is ordinary C: write(1, ...) reaches this chip's console,
 * sizeof("literal") is the character count (C11 6.5.3.4), and the entry
 * follows the vector-table contract app run checks.
 * ============================================================ */
static const char SRC_HELLO[] =
    "/* hello.c -- POSIX hello world, compiled and run by arkc on this chip. */\r\n"
    "typedef unsigned int u32;\r\n"
    "\r\n"
    "extern void __arkcc_init_globals(void);\r\n"
    "int __svc(0x1A) svc_shell(u32 *p);\r\n"
    "int __svc(0x1B) svc_thread(u32 *p);\r\n"
    "\r\n"
    "int write(int fd, const void *buf, unsigned int len);\r\n"
    "int main(void);\r\n"
    "\r\n"
    "/* The two messages are written as literals, not as const arrays: the ARM\r\n"
    " * backend currently emits no initialised data section, so a global\r\n"
    " * \"static const char msg[] = ...\" would be linked as zeros.  Inline\r\n"
    " * literals do land in the image (measured: at +0x400 of this one). */\r\n"
    "\r\n"
    "/* fd 1 is this chip's console.  The SVC below prints a NUL-terminated\r\n"
    " * string, so write() copies the caller's bytes into a small buffer and\r\n"
    " * terminates it -- that keeps the POSIX contract (any buffer, any length)\r\n"
    " * instead of quietly requiring a string literal. */\r\n"
    "int write(int fd, const void *buf, unsigned int len)\r\n"
    "{\r\n"
    "    static char chunk[64];\r\n"
    "    const char *p = (const char *)buf;\r\n"
    "    u32 q[3];\r\n"
    "    unsigned int done = 0u;\r\n"
    "\r\n"
    "    (void)fd;\r\n"
    "    while (done < len) {\r\n"
    "        unsigned int n = len - done;\r\n"
    "        unsigned int i;\r\n"
    "        if (n > 63u) {\r\n"
    "            n = 63u;\r\n"
    "        }\r\n"
    "        for (i = 0u; i < n; i++) {\r\n"
    "            chunk[i] = p[done + i];\r\n"
    "        }\r\n"
    "        chunk[n] = 0;\r\n"
    "        q[0] = 3u; q[1] = (u32)chunk; q[2] = 0u;\r\n"
    "        svc_shell(q);\r\n"
    "        done += n;\r\n"
    "    }\r\n"
    "    return (int)len;\r\n"
    "}\r\n"
    "\r\n"
    "int main(void)\r\n"
    "{\r\n"
    "    write(1, \"hello, world\\r\\n\",\r\n"
    "          sizeof(\"hello, world\\r\\n\") - 1);\r\n"
    "    write(1, \"written by arkc running on the STM32H743 itself\\r\\n\",\r\n"
    "          sizeof(\"written by arkc running on the STM32H743 itself\\r\\n\") - 1);\r\n"
    "    return 0;\r\n"
    "}\r\n"
    "\r\n"
    "void Reset_Handler(void)\r\n"
    "{\r\n"
    "    u32 q[3];\r\n"
    "\r\n"
    "    __arkcc_init_globals();\r\n"
    "    (void)main();\r\n"
    "    q[0] = 3u; q[1] = 0u; q[2] = 0u;      /* 3 = exit this task */\r\n"
    "    svc_thread(q);\r\n"
    "    for (;;) {\r\n"
    "    }\r\n"
    "}\r\n";

static const char SRC_HELLO_SCT[] =
    "; hello.sct -- the app-run window (ARKRTOS_APPRUN_*, see arkrtos_partition.h)\r\n"
    "; The load-region size sets the initial SP, and app run requires SP to be the\r\n"
    "; window top (0x24074000).  One exec region on purpose: the loader copies\r\n"
    "; exactly the file bytes, so every initialised byte must sit inside the 8 KB\r\n"
    "; code window.  (A .sct with a separate RAM region still compiles, but the RW\r\n"
    "; data falls outside the load region and never reaches the board.)\r\n"
    "; --vector-table also requires the RESET selector to come first.\r\n"
    "LR_IROM1 0x24070000 0x00004000\r\n"
    "{\r\n"
    "  ER_IROM1 0x24070000 0x00004000 {\r\n"
    "   *.o (RESET, +First)\r\n"
    "   .ANY (+RO)\r\n"
    "   .ANY (+RW +ZI)\r\n"
    "  }\r\n"
    "}\r\n";

/* Write (or overwrite) one demo file.  A leftover file from the last run
 * would otherwise be appended to, not replaced, so remove it first. */
static int hello_put(const char *path, const char *text)
{
    int32 rc;

    (void)arkrtos_vfs_remove(path);
    rc = arkrtos_vfs_write_file(path, (const uint8 *)text, (uint32)strlen(text));
    if (rc != 0)
    {
        /* write_file returns 0 on success and never a length: comparing it
         * with strlen() would report a written file as failed. */
        ark_shell_printf("hello: cannot write %s (rc=%d)\r\n", path, (int)rc);
        return -1;
    }
    return 0;
}

static int cmd_hello(int argc, char *argv[])
{
    char  *cc_argv[8];
    int    cc_argc = 0;
    int    rc;
    uint32 task = 0u;

    (void)argc;
    (void)argv;

    ark_shell_printf("\r\n[hello] 1/3 sources -> /tmp/hello.c, /tmp/hello.sct\r\n");
    if (hello_put("/tmp/hello.c", SRC_HELLO) != 0) {
        return 1;
    }
    if (hello_put("/tmp/hello.sct", SRC_HELLO_SCT) != 0) {
        return 1;
    }
    (void)arkrtos_vfs_remove("/tmp/hello.bin");

    ark_shell_printf("[hello] 2/3 cc: compile + link for the app-run window\r\n");
    cc_argv[cc_argc++] = "cc";
    cc_argv[cc_argc++] = "/tmp/hello.c";
    cc_argv[cc_argc++] = "--scatter";
    cc_argv[cc_argc++] = "/tmp/hello.sct";
    cc_argv[cc_argc++] = "--vector-table";
    cc_argv[cc_argc++] = "-o";
    cc_argv[cc_argc++] = "/tmp/hello.bin";
    if (cmd_cc(cc_argc, cc_argv) != 0) {
        ark_shell_printf("[hello] compile/link failed, nothing was run\r\n");
        return 1;
    }

    ark_shell_printf("[hello] 3/3 app run /tmp/hello.bin\r\n");
    rc = arkrtos_apprun_run_ex("/tmp/hello.bin", &task);
    if (rc != 0) {
        const char *name = arkrtos_apprun_error_name(rc);
        ark_shell_printf("[hello] app run failed, rc=%d%s%s\r\n",
                         (int)rc, (name != 0) ? " - " : "",
                         (name != 0) ? name : "");
        return 1;
    }
    ark_shell_printf("[hello] started as task %u; the program prints and exits on its own\r\n",
                     (unsigned)task);
    return 0;
}
#endif /* ARKRTOS_USE_HELLO */

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
#if (ARKRTOS_USE_HELLO == 1)
    if ((idx + 1) > ARK_SHELL_MAX_COMMANDS) {
        platform_uart_send_string("shell: no room for the hello command\r\n");
        return idx;
    }
#endif
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
#if (ARKRTOS_USE_HELLO == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("hello", cmd_hello,
        "hello: compile+link+run the POSIX hello world on this chip",
        ARK_SHELL_MAX_ARGS);
#endif
    return idx;
}
