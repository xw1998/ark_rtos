/**
* @file test_fdt_console_host.c
* @brief Host-side unit test for arkrtos_fdt_console.c (/chosen/stdout-path).
*
* What this proves: the console decision.  The module reads the tree's answer
* and refuses to pretend a mismatch is fine, so every branch is a *reason
* code*, not "some step failed":
*   - out == 0 is BAD_PATH and nothing is read at all;
*   - the tree not being initialised or not having /chosen/stdout-path is
*     NO_STDOUT, a malformed value is BAD_PATH, and a resolved path that does
*     not name a node is NO_NODE (distinct: "the tree said something, but the
*     node is not there");
*   - status != okay is DISABLED -- the tree's own word, honoured;
*   - compatible / reg / clock-frequency are each compared with the board's
*     expectation when the expectation is non-zero, and a *missing*
*     clock-frequency counts as CLOCK (an expected value is not optional);
*   - a stdout-path baud that contradicts the node's current-speed is BAUD:
*     the tree is self-contradictory and we refuse to pick the "more
*     believable" one;
*   - reason() maps every code, and an unknown code answers "?".
*
* The five FDT queries are stubs (a table of programmable answers), so this
* proves the decision layer only.  It does NOT prove DTB parsing (that is
* arkrtos_fdt.c's own case) and it does NOT prove that a real UART ends up
* printing at the right speed -- that is a board reading.
*
* ARKRTOS_USE_FDT_CONSOLE defaults to 0; this TU is compiled with it at 1.
*
* @author xw
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fdt_console.h"

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
        if ((g_ == 0) || (strcmp(g_, w_) != 0)) {                      \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, (g_ != 0) ? g_ : "(null)", w_);     \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------- FDT stubs */

static int32                    g_stdout_rc;
static arkrtos_fdt_console_t    g_stdout;
static int                      g_node_rc;
static int                      g_enabled;
static const char              *g_compat;
static uint32                   g_reg;
static int                      g_has_clock;
static uint32                   g_clock;
static int                      g_has_speed;
static uint32                   g_speed;
static const char              *g_last_path;

/* The header declares these; the test supplies the only definitions. */
int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out);
int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out);
int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node);
const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name);
int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out);

int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out)
{
    if (g_stdout_rc == ARKRTOS_FDT_OK) {
        *out = g_stdout;
    }
    return g_stdout_rc;
}

int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out)
{
    g_last_path = path;
    if (g_node_rc == ARKRTOS_FDT_OK) {
        out->name = "uart";
        out->depth = 2u;
        out->n_props = 3u;
        out->props_off = 0u;
    }
    return g_node_rc;
}

int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node)
{
    (void)node;
    return g_enabled;
}

const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name)
{
    (void)node;
    if (strcmp(name, "compatible") == 0) {
        return g_compat;
    }
    return 0;
}

int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out)
{
    (void)node;
    (void)idx;
    if (strcmp(name, "reg") == 0) {
        *out = g_reg;
        return 0;
    }
    if (strcmp(name, "clock-frequency") == 0) {
        if (g_has_clock == 0) {
            return -1;
        }
        *out = g_clock;
        return 0;
    }
    if (strcmp(name, "current-speed") == 0) {
        if (g_has_speed == 0) {
            return -1;
        }
        *out = g_speed;
        return 0;
    }
    return -1;
}

/* --------------------------------------------------------------- fixtures */

static void reset_stubs(void)
{
    g_stdout_rc = ARKRTOS_FDT_OK;
    memset(&g_stdout, 0, sizeof(g_stdout));
    strcpy(g_stdout.path, "/soc/serial@1fe001e0");
    g_stdout.baud = 115200u;
    g_stdout.bits = 8u;
    g_stdout.parity = 'n';
    g_stdout.stop = 1u;
    g_stdout.from_alias = 1u;
    g_node_rc = ARKRTOS_FDT_OK;
    g_enabled = 1;
    g_compat = "ns16550a";
    g_reg = 0x1FE001E0u;
    g_has_clock = 1;
    g_clock = 25000000u;
    g_has_speed = 0;
    g_speed = 0u;
    g_last_path = 0;
}

/* -------------------------------------------------------------- testcases */

static void test_out_null(void)
{
    reset_stubs();
    CK_I(arkrtos_console_from_fdt(0, 0), ARKRTOS_CONSOLE_BAD_PATH);
    CK_I(arkrtos_console_from_fdt(0, 0), ARKRTOS_CONSOLE_BAD_PATH);
    CK_I(g_last_path == 0, 1);           /* 一个 FDT 查询都不该发生 */
}

static void test_stdout_errors(void)
{
    arkrtos_console_bind_t out;

    reset_stubs();
    g_stdout_rc = ARKRTOS_FDT_ERR_NOT_INIT;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_NO_STDOUT);
    g_stdout_rc = ARKRTOS_FDT_ERR_NOT_FOUND;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_NO_STDOUT);
    g_stdout_rc = ARKRTOS_FDT_ERR_PATH;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_BAD_PATH);
    g_stdout_rc = ARKRTOS_FDT_ERR_ARG;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_BAD_PATH);
    /* 失败时 out 仍是清空后的形状（不是残留） */
    CK_I(out.path[0], '\0');
    CK_I(out.reg_base, 0u);
    CK_I(out.enabled, 0u);
}

static void test_node_missing(void)
{
    arkrtos_console_bind_t out;

    reset_stubs();
    g_node_rc = ARKRTOS_FDT_ERR_NOT_FOUND;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_NO_NODE);
    /* 树里说到的路径已经填进 out 了：报错时也能把"读到了什么"打出来 */
    CK_S(out.path, "/soc/serial@1fe001e0");
    CK_I(out.baud, 115200u);
    CK_I(out.bits, 8u);
    CK_I((int)out.parity, 'n');
    CK_I(out.from_alias, 1u);
    CK_S(g_last_path, "/soc/serial@1fe001e0");
}

static void test_disabled(void)
{
    arkrtos_console_bind_t out;

    reset_stubs();
    g_enabled = 0;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_DISABLED);
    CK_I(out.enabled, 0u);
    /* 读出来的字段先填，再因 status 拒绝：报错时也能打出读到了什么 */
    CK_S(out.compatible, "ns16550a");
}

static void test_ok_no_expect(void)
{
    arkrtos_console_bind_t out;

    reset_stubs();
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);
    CK_S(out.path, "/soc/serial@1fe001e0");
    CK_S(out.compatible, "ns16550a");
    CK_I(out.reg_base, 0x1FE001E0u);
    CK_I(out.clock_hz, 25000000u);
    CK_I(out.enabled, 1u);
    CK_I(out.baud, 115200u);
    /* 节点没写 clock-frequency：只解析不核对时不算错 */
    reset_stubs();
    g_has_clock = 0;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);
    CK_I(out.clock_hz, 0u);
    /* 节点没写 compatible：字段保持空串 */
    reset_stubs();
    g_compat = 0;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);
    CK_S(out.compatible, "");
}

static void test_expect_mismatches(void)
{
    arkrtos_console_bind_t out;
    arkrtos_console_expect_t   expect;

    reset_stubs();
    memset(&expect, 0, sizeof(expect));
    expect.compatible = "ns16550a";
    expect.reg_base = 0x1FE001E0u;
    expect.clock_hz = 25000000u;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_OK);

    /* compatible 不同：这是第一个判据 */
    reset_stubs();
    expect.compatible = "snps,dw-apb-uart";
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_COMPAT);

    /* 只比前缀/后缀/大小写都不算命中 */
    reset_stubs();
    expect.compatible = "ns16550";
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_COMPAT);
    reset_stubs();
    expect.compatible = "NS16550A";
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_COMPAT);

    /* reg 不同 */
    reset_stubs();
    expect.compatible = "ns16550a";
    expect.reg_base = 0x1FE00200u;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_ADDR);

    /* 期望为 0 的项不检查 */
    reset_stubs();
    expect.reg_base = 0u;
    expect.clock_hz = 0u;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_OK);

    /* 时钟不同 */
    reset_stubs();
    expect.clock_hz = 100000000u;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_CLOCK);

    /* 期望了时钟但节点没写：缺失也算不一致，不"跳过" */
    reset_stubs();
    g_has_clock = 0;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_CLOCK);

    /* 全对 */
    reset_stubs();
    expect.clock_hz = 25000000u;
    CK_I(arkrtos_console_from_fdt(&expect, &out), ARKRTOS_CONSOLE_OK);
}

static void test_baud_conflict(void)
{
    arkrtos_console_bind_t out;

    /* stdout-path 说 115200，节点说 9600：树自相矛盾 */
    reset_stubs();
    g_has_speed = 1;
    g_speed = 9600u;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_BAUD);

    /* 两者一致：不是冲突 */
    reset_stubs();
    g_has_speed = 1;
    g_speed = 115200u;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);

    /* 节点写 0：视为没写，不参与比较 */
    reset_stubs();
    g_has_speed = 1;
    g_speed = 0u;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);

    /* stdout-path 没写波特率：没有可比的对象 */
    reset_stubs();
    g_stdout.baud = 0u;
    g_has_speed = 1;
    g_speed = 9600u;
    CK_I(arkrtos_console_from_fdt(0, &out), ARKRTOS_CONSOLE_OK);
}

/* out 后面放一段哨兵：拷贝超长时绝不能写进这里。 */
static struct
{
    arkrtos_console_bind_t b;
    uint8                      guard[16];
} g_box;

static void guard_fill(void)
{
    uint32 i;

    for (i = 0u; i < (uint32)sizeof(g_box.guard); i++) {
        g_box.guard[i] = 0xA5u;
    }
}

static int guard_intact(void)
{
    uint32 i;

    for (i = 0u; i < (uint32)sizeof(g_box.guard); i++) {
        if (g_box.guard[i] != 0xA5u) {
            return 0;
        }
    }
    return 1;
}

static void test_long_strings(void)
{
    arkrtos_console_bind_t *out = &g_box.b;
    static char long_compat[80];
    uint32 i;

    for (i = 0u; i < (uint32)sizeof(long_compat) - 1u; i++) {
        long_compat[i] = 'y';
    }
    long_compat[sizeof(long_compat) - 1u] = '\0';

    /* 顶格路径（127 字符 + NUL）：刚好装下，不截断、不越界 */
    reset_stubs();
    for (i = 0u; i < ARKRTOS_FDT_PATH_MAX - 1u; i++) {
        g_stdout.path[i] = 'x';
    }
    g_stdout.path[ARKRTOS_FDT_PATH_MAX - 1u] = '\0';
    guard_fill();
    CK_I(arkrtos_console_from_fdt(0, out), ARKRTOS_CONSOLE_OK);
    CK_I(out->path[0], 'x');
    CK_I(out->path[ARKRTOS_FDT_PATH_MAX - 2u], 'x');
    CK_I((uint32)strlen(out->path), ARKRTOS_FDT_PATH_MAX - 1u);
    CK_I(guard_intact(), 1);

    /* 超长 compatible：32 字节缓冲装不下 79 字符，拷贝放弃（返回 -1 被忽略），
     * 已写的字节是真的、且不越过结构尾部 */
    reset_stubs();
    g_compat = long_compat;
    guard_fill();
    CK_I(arkrtos_console_from_fdt(0, out), ARKRTOS_CONSOLE_OK);
    CK_I(out->compatible[0], 'y');
    /* 截断后仍是合法 C 字符串：末尾必须是 NUL */
    CK_I((int)out->compatible[sizeof(out->compatible) - 1u], 0);
    CK_I((uint32)strlen(out->compatible), (uint32)sizeof(out->compatible) - 1u);
    CK_I(guard_intact(), 1);
}

static void test_reason_names(void)
{
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_OK), "ok");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_NO_STDOUT), "no_stdout_path");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_BAD_PATH), "bad_value");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_NO_NODE), "node_missing");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_DISABLED), "node_disabled");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_COMPAT), "compatible_mismatch");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_ADDR), "reg_mismatch");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_BAUD), "baud_conflict");
    CK_S(arkrtos_console_reason(ARKRTOS_CONSOLE_CLOCK), "clock_mismatch");
    CK_S(arkrtos_console_reason(-99), "?");
    CK_S(arkrtos_console_reason(1), "?");
}

int main(void)
{
    test_out_null();
    test_stdout_errors();
    test_node_missing();
    test_disabled();
    test_ok_no_expect();
    test_expect_mismatches();
    test_baud_conflict();
    test_long_strings();
    test_reason_names();

    if (g_failed != 0) {
        printf("FDT-CONSOLE-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("FDT-CONSOLE-HOST OK  %d checks\n", g_checks);
    return 0;
}
