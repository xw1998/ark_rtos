/**
* @file test_fdt_res_host.c
* @brief Host-side unit test for arkrtos_fdt_res.c (timer / timebase check).
*
* What this proves: the decision layer of "what frequency is the kernel
* running its timer at".  The module reads two facts out of the tree --
* /cpus/timebase-frequency and a timer node found by compatible -- and
* compares them with the board constants.  Every failure is a distinct
* reason code, because "the tree did not write it" and "the tree disagrees"
* are different facts with different fixes:
*   - out or expect NULL is NO_CPUS (nothing can be read);
*   - the tree not being initialised, or /cpus missing, is NO_CPUS;
*   - a missing timebase-frequency is NO_TIMEBASE (only when the caller
*     actually expects one -- expecting 0 means "do not check");
*   - a differing timebase is TIMEBASE_DIFF;
*   - timer_compat == 0 means "do not look for a timer node" and stops
*     after the timebase;
*   - a failed iterator is NO_TIMER, and so is a walk that yields only
*     disabled nodes or nodes with a different compatible;
*   - a found node's missing reg / reg mismatch / missing clock-frequency /
*     clock mismatch are four separate codes, and the values read so far
*     stay in out for the log line.
*
* The seven FDT queries are stubs (a table of programmable nodes), so this
* proves the decision layer only.  It does NOT prove DTB parsing (that is
* arkrtos_fdt.c's own case) and it does NOT prove that the timer really
* ticks at that frequency -- that is a board reading.
*
* ARKRTOS_USE_FDT_RES defaults to 0; this TU is compiled with it at 1.
*
* @author xw
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fdt_res.h"

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

#define MAX_NODES 6

typedef struct
{
    const char *name;
    int         enabled;
    const char *compat;    /* 0 = property absent */
    int         has_reg;
    uint32      reg;
    int         has_hz;
    uint32      hz;
} fake_node_t;

static int          g_ready;
static int          g_cpus_rc;
static int          g_has_timebase;
static uint32       g_timebase;
static int          g_iter_rc;
static int          g_next_err;   /* when set, next() fails after the table */
static fake_node_t  g_nodes[MAX_NODES];
static int          g_node_count;
static int          g_walk;       /* index for next() */
static const char  *g_seen_path;  /* last path given to node_by_path */
static int          g_prop_blocked; /* make prop_u32/prop_str miss */

/* The header declares these; the test supplies the only definitions. */
int arkrtos_fdt_ready(void);
int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out);
int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it);
int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out);
int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node);
const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name);
int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out);

int arkrtos_fdt_ready(void)
{
    return g_ready;
}

int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out)
{
    g_seen_path = path;
    if (g_cpus_rc != ARKRTOS_FDT_OK) {
        return g_cpus_rc;
    }
    out->name = "cpus";
    out->depth = 1u;
    out->n_props = 1u;
    out->props_off = 0u;
    return ARKRTOS_FDT_OK;
}

int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it)
{
    it->off = 0u;
    it->depth = 0u;
    g_walk = 0;
    return g_iter_rc;
}

int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out)
{
    (void)it;
    if (g_walk >= g_node_count) {
        return g_next_err;
    }
    out->name = g_nodes[g_walk].name;
    out->depth = 2u;
    out->n_props = 3u;
    out->props_off = (uint32)g_walk;
    g_walk++;
    return 1;
}

int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node)
{
    uint32 i = node->props_off;

    if (i >= (uint32)MAX_NODES) {
        return 0;
    }
    return g_nodes[i].enabled;
}

const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name)
{
    (void)name;
    if (g_prop_blocked != 0) {
        return 0;
    }
    if (node->props_off >= (uint32)MAX_NODES) {
        return 0;
    }
    return g_nodes[node->props_off].compat;
}

int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out)
{
    uint32 i = node->props_off;

    (void)idx;
    if (node->name != 0 && strcmp(node->name, "cpus") == 0) {
        if (strcmp(name, "timebase-frequency") == 0) {
            if (g_has_timebase == 0) {
                return -1;
            }
            *out = g_timebase;
            return 0;
        }
        return -1;
    }
    if (i >= (uint32)MAX_NODES) {
        return -1;
    }
    if (strcmp(name, "reg") == 0) {
        if (g_nodes[i].has_reg == 0) {
            return -1;
        }
        *out = g_nodes[i].reg;
        return 0;
    }
    if (strcmp(name, "clock-frequency") == 0) {
        if (g_nodes[i].has_hz == 0) {
            return -1;
        }
        *out = g_nodes[i].hz;
        return 0;
    }
    return -1;
}

/* --------------------------------------------------------------- fixtures */

static void reset_stubs(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    g_ready = 1;
    g_cpus_rc = ARKRTOS_FDT_OK;
    g_has_timebase = 1;
    g_timebase = 10000000u;
    g_iter_rc = ARKRTOS_FDT_OK;
    g_next_err = 0;
    g_node_count = 0;
    g_walk = 0;
    g_seen_path = 0;
    g_prop_blocked = 0;
}

static void add_node(const char *name, int enabled, const char *compat,
                     int has_reg, uint32 reg, int has_hz, uint32 hz)
{
    fake_node_t *n = &g_nodes[g_node_count];

    n->name = name;
    n->enabled = enabled;
    n->compat = compat;
    n->has_reg = has_reg;
    n->reg = reg;
    n->has_hz = has_hz;
    n->hz = hz;
    g_node_count++;
}

static arkrtos_fdt_res_expect_t expect_all(void)
{
    arkrtos_fdt_res_expect_t e;

    e.expect_timebase = 10000000u;
    e.timer_compat = "riscv,clint0";
    e.expect_timer_reg = 0x02000000u;
    e.expect_timer_hz = 25000000u;
    return e;
}

/* ------------------------------------------------------------------ cases */

static void test_null_arguments(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    CK_I(arkrtos_fdt_res_check(0, &out), ARKRTOS_RES_NO_CPUS);
    CK_I(arkrtos_fdt_res_check(&e, 0), ARKRTOS_RES_NO_CPUS);
    /* 两个都空时也不读树 */
    CK_I(g_seen_path == 0, 1);
}

static void test_not_ready_or_no_cpus(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    g_ready = 0;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_CPUS);
    CK_I(g_seen_path == 0, 1);   /* ready 检查在取节点之前 */

    reset_stubs();
    g_cpus_rc = ARKRTOS_FDT_ERR_NOT_FOUND;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_CPUS);
    CK_S(g_seen_path, "/cpus");
}

static void test_timebase_absent(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    g_has_timebase = 0;

    /* 期望里有值：没写 = 一个独立原因码 */
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMEBASE);
    CK_I(out.has_timebase, 0u);
    CK_I(out.timebase, 0u);

    /* 期望为 0：这一项不检查，于是「没写」不是错 */
    e.expect_timebase = 0u;
    e.timer_compat = 0;          /* 也不找定时器节点 */
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_OK);
    CK_I(out.has_timebase, 0u);
}

static void test_timebase_mismatch(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    g_timebase = 24000000u;
    e.timer_compat = 0;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_TIMEBASE_DIFF);
    /* 读到的值仍要写进 out（报错也好打印） */
    CK_I(out.has_timebase, 1u);
    CK_I(out.timebase, 24000000u);
}

static void test_timebase_ok_no_timer(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    e.timer_compat = 0;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_OK);
    CK_I(out.timebase, 10000000u);
    CK_I(out.has_timebase, 1u);
    CK_I(out.has_timer, 0u);
}

static void test_timer_not_found(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    /* 迭代器起不来 */
    reset_stubs();
    g_iter_rc = ARKRTOS_FDT_ERR_NOT_INIT;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMER);
    CK_I(out.has_timer, 0u);

    /* 走完了也没匹配：名字对但 disabled、compatible 完全不同 */
    reset_stubs();
    add_node("clint@2000000", 0, "riscv,clint0", 1, 0x02000000u, 1, 25000000u);
    add_node("uart@10000000", 1, "ns16550a", 1, 0x10000000u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMER);
    CK_I(out.has_timer, 0u);

    /* 没有 compatible 属性的节点（str_eq 的空指针守卫走这里） */
    reset_stubs();
    add_node("broken", 1, 0, 0, 0u, 0, 0u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMER);

    /* 遍历本身报错（blob 坏）也归到「没找到」 */
    reset_stubs();
    add_node("uart@10000000", 1, "ns16550a", 1, 0x10000000u, 1, 25000000u);
    g_next_err = ARKRTOS_FDT_ERR_FORMAT;
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMER);
}

static void test_timer_found_ok(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    reset_stubs();
    add_node("disabled@0", 0, "riscv,clint0", 1, 0x02000000u, 1, 25000000u);
    add_node("clint@2000000", 1, "riscv,clint0", 1, 0x02000000u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_OK);
    CK_I(out.has_timer, 1u);
    CK_S(out.timer_node, "clint@2000000");
    CK_S(out.timer_compat, "riscv,clint0");
    CK_I(out.has_timer_reg, 1u);
    CK_I(out.timer_reg, 0x02000000u);
    CK_I(out.has_timer_hz, 1u);
    CK_I(out.timer_hz, 25000000u);
}

static void test_timer_reg_codes(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    /* 没写 reg */
    reset_stubs();
    add_node("clint@2000000", 1, "riscv,clint0", 0, 0u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_TIMER_NO_REG);
    CK_I(out.has_timer, 1u);
    CK_I(out.has_timer_reg, 0u);

    /* 写了但不同 */
    reset_stubs();
    add_node("clint@2000000", 1, "riscv,clint0", 1, 0x02100000u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_TIMER_REG_DIFF);
    CK_I(out.timer_reg, 0x02100000u);

    /* 期望为 0：不检查 reg，即使没写也算 OK */
    reset_stubs();
    e.expect_timer_reg = 0u;
    e.expect_timer_hz = 0u;
    add_node("clint@2000000", 1, "riscv,clint0", 0, 0u, 0, 0u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_OK);
    CK_I(out.has_timer_reg, 0u);
    CK_I(out.has_timer_hz, 0u);
}

static void test_timer_hz_codes(void)
{
    arkrtos_fdt_res_t      out;
    arkrtos_fdt_res_expect_t e = expect_all();

    /* 没写 clock-frequency */
    reset_stubs();
    add_node("clint@2000000", 1, "riscv,clint0", 1, 0x02000000u, 0, 0u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_TIMER_NO_HZ);
    CK_I(out.has_timer_reg, 1u);
    CK_I(out.has_timer_hz, 0u);

    /* 写了但不同 */
    reset_stubs();
    add_node("clint@2000000", 1, "riscv,clint0", 1, 0x02000000u, 1, 100000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_TIMER_HZ_DIFF);
    CK_I(out.timer_hz, 100000000u);
}

static void test_long_node_strings(void)
{
    arkrtos_fdt_res_t       out;
    arkrtos_fdt_res_expect_t e = expect_all();
    static char             long_name[100];
    static char             long_compat[80];
    uint32                  i;

    for (i = 0u; i < (uint32)sizeof(long_name) - 1u; i++) {
        long_name[i] = 'n';
    }
    long_name[sizeof(long_name) - 1u] = '\0';
    for (i = 0u; i < (uint32)sizeof(long_compat) - 1u; i++) {
        long_compat[i] = 'c';
    }
    long_compat[sizeof(long_compat) - 1u] = '\0';

    /* 节点名 99 字符 → 64 字节缓冲按 63 + NUL 截断；compatible 同理
     * 截到 31 + NUL。截断只发生在"显示用"的副本里，不影响比较：
     * 这里把 expect 换成窄字符串以便先匹配上，再单独看副本长度。 */
    reset_stubs();
    add_node(long_name, 1, "riscv,clint0", 1, 0x02000000u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_OK);
    CK_I((uint32)strlen(out.timer_node), (uint32)sizeof(out.timer_node) - 1u);
    CK_S(out.timer_compat, "riscv,clint0");

    /* compatible 也超长：匹配需要一个前缀完全相同的 expect…这里改为
     * 直接看「匹配失败」这条路径上的副本：默认 compat 是空的，
     * 用 g_prop_blocked 让节点无 compatible，副本应为空串。 */
    reset_stubs();
    add_node("clint@2000000", 1, long_compat, 1, 0x02000000u, 1, 25000000u);
    CK_I(arkrtos_fdt_res_check(&e, &out), ARKRTOS_RES_NO_TIMER);
    CK_S(out.timer_compat, "");   /* 没匹配上，out 仍是初始值 */
}

static void test_reason_names(void)
{
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_OK), "ok");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_NO_CPUS), "no_cpus_node");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_NO_TIMEBASE), "timebase_absent");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_TIMEBASE_DIFF), "timebase_mismatch");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_NO_TIMER), "timer_node_absent");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_TIMER_NO_REG), "timer_reg_absent");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_TIMER_REG_DIFF), "timer_reg_mismatch");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_TIMER_NO_HZ), "timer_clock_absent");
    CK_S(arkrtos_fdt_res_reason(ARKRTOS_RES_TIMER_HZ_DIFF), "timer_clock_mismatch");
    CK_S(arkrtos_fdt_res_reason(-99), "?");
    CK_S(arkrtos_fdt_res_reason(1), "?");
}

int main(void)
{
    test_null_arguments();
    test_not_ready_or_no_cpus();
    test_timebase_absent();
    test_timebase_mismatch();
    test_timebase_ok_no_timer();
    test_timer_not_found();
    test_timer_found_ok();
    test_timer_reg_codes();
    test_timer_hz_codes();
    test_long_node_strings();
    test_reason_names();

    if (g_failed != 0) {
        printf("FDT-RES-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("FDT-RES-HOST OK  %d checks\n", g_checks);
    return 0;
}
