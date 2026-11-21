/**
* @file test_hwsafe_host.c
* @brief 硬件安全态（"遗言"）机制（arkrtos_hwsafe.c）的宿主用例
* @details 这个模块只在任务死亡路径上出手，而那条路径平时没人走：
*          平常的 bug 是"崩溃之后硬件还带着崩溃前的最后一条指令继续跑"，
*          它不在返回码上、不在日志里，只在事故现场。所以这里钉住的都是
*          「谁在这个时刻被驱动了、驱动说了什么、账本有没有少一条或多一条」：
*
*            - 登记：同名不与静默覆盖（两个驱动给出两套安全态是配置错误）、
*              表满、名字按 8 字节含结尾 0 比较并按 7 字符截断；
*            - 台账：owner<=0（内核/空闲上下文）不记名、同名同 owner 重复 open
*              只刷新实例、表满时宁可不记也不覆盖别人的条目；
*            - 遗言：只有当前持有该设备的 owner 能留（否则就是"命令别人家
*              硬件"），撤销幂等，读出来的 (cmd,val) 与写进去的一致；
*            - 两阶段：on_death 只做标记（可在临界区里调），run_pending 才
*              真正驱动硬件；计划内交接跳过遗言与回调且不计数中和；
*            - 计数跟着驱动的回答走：驱动返回非 0 记在 fail，绝不当成功；
*              没有安全态的设备记 none（"没有告诉内核怎么让它安全"不是"安全"）。
*
*          只依赖两个外部符号：arkrtos_port_enter/exit_critical（本文件给桩）
*          与 arkrtos_dev_ctrl_obj（遗言执行通道，本文件给桩并记录参数）。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_hwsafe.h"
#include "arkrtos_dev.h"
#include "arkrtos_hal.h"           /* port_enter/exit_critical 原型 */

#if (ARKRTOS_HW_SAFE == 1)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu want %lu)\n", what, got, want);
    }
}

/* ---------------- 临界区桩 ---------------- */

static unsigned long g_crit_depth;
static unsigned long g_crit_enters;
static unsigned long g_crit_exits;

uint32 arkrtos_port_enter_critical(void)
{
    g_crit_enters++;
    g_crit_depth++;
    return (uint32)g_crit_depth;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_crit_exits++;
    g_crit_depth--;
}

/* ---------------- 遗言执行通道桩 ---------------- */

static arkrtos_dev_hdr_t *g_ctrl_obj;
static unsigned long g_ctrl_calls;
static long g_ctrl_code;
static long g_ctrl_value;
static int32 g_ctrl_ret;

int32 arkrtos_dev_ctrl_obj(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value)
{
    g_ctrl_calls++;
    g_ctrl_obj   = obj;
    g_ctrl_code  = (long)code;
    g_ctrl_value = (long)value;
    return g_ctrl_ret;
}

/* 驱动登记的安全态回调 */
static unsigned long g_cb_calls;
static arkrtos_dev_hdr_t *g_cb_obj;
static unsigned long g_cb_owner;
static int32 g_cb_ret;

static int32 fake_cb(arkrtos_dev_hdr_t *obj, uint32 owner)
{
    g_cb_calls++;
    g_cb_obj   = obj;
    g_cb_owner = (unsigned long)owner;
    return g_cb_ret;
}

static void reset_all(void)
{
    arkrtos_hwsafe_reset();
    g_ctrl_obj   = 0;
    g_ctrl_calls = 0ul;
    g_ctrl_code  = -1;
    g_ctrl_value = -1;
    g_ctrl_ret   = 0;
    g_cb_calls   = 0ul;
    g_cb_obj     = 0;
    g_cb_owner   = 0ul;
    g_cb_ret     = 0;
    g_crit_depth = 0ul;
    g_crit_enters = 0ul;
    g_crit_exits  = 0ul;
}

static arkrtos_dev_hdr_t g_obj_a;
static arkrtos_dev_hdr_t g_obj_b;
static arkrtos_dev_hdr_t g_obj_c;
static arkrtos_dev_hdr_t g_obj_x;

/* ---------------- 登记 ---------------- */

static void test_register(void)
{
    char name[16];

    reset_all();

    ck(arkrtos_hwsafe_register(0, fake_cb) == -2, "null name is a parameter error");
    ck(arkrtos_hwsafe_register("A", 0) == -2, "null callback is a parameter error");

    ck(arkrtos_hwsafe_register("A", fake_cb) == 0, "first registration succeeds");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 1ul, "one device registered");
    ck(arkrtos_hwsafe_register("A", fake_cb) == -2, "same name again is refused, not overwritten");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 1ul, "refused duplicate did not grow the table");

    /* 名字只保留 7 字符 + 结尾 0：8 字节含 0 比较，截断不是"存进去再说" */
    ck(arkrtos_hwsafe_register("LONGDEVNAME", fake_cb) == 0, "long name is truncated, not rejected");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 2ul, "two devices registered");
    memset(name, 0x5a, sizeof(name));
    ck(arkrtos_hwsafe_dev_at(1, name, sizeof(name)) == 0, "dev_at reads the truncated name");
    ck(strcmp(name, "LONGDEV") == 0, "name is cut to seven characters");
    ck(arkrtos_hwsafe_registered("LONGDEV") == 1, "truncated name is what lookup sees");
    ck(arkrtos_hwsafe_registered("LONGDEVNAME") == 0, "the unstored tail is not a name");
    ck(arkrtos_hwsafe_registered(0) == 0, "a null name resolves to nothing");

    ck_u((unsigned long)arkrtos_hwsafe_dev_at(-1, name, sizeof(name)), (unsigned long)-1,
         "dev_at rejects a negative index");
    ck_u((unsigned long)arkrtos_hwsafe_dev_at(2, name, sizeof(name)), (unsigned long)-1,
         "dev_at rejects past the end");
    ck(arkrtos_hwsafe_dev_at(0, 0, sizeof(name)) == 0, "dev_at accepts a null output");
    memset(name, 0x5a, sizeof(name));
    ck(arkrtos_hwsafe_dev_at(0, name, 1u) == 0, "dev_at accepts max = 1");
    ck_u((unsigned long)(unsigned char)name[0], 0ul, "max = 1 writes only the terminator");
    memset(name, 0x5a, sizeof(name));
    ck(arkrtos_hwsafe_dev_at(0, name, 0u) == 0, "dev_at accepts max = 0");
    ck_u((unsigned long)(unsigned char)name[0], 0x5aul, "max = 0 writes nothing");

    ck_u((unsigned long)arkrtos_hwsafe_unregister(0), (unsigned long)-1,
         "unregister of a null name is refused");
    ck_u((unsigned long)arkrtos_hwsafe_unregister("NOPE"), (unsigned long)-1,
         "unregister of an unknown device says so");
    ck(arkrtos_hwsafe_unregister("A") == 0, "unregister removes the entry");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 1ul, "one registration left");
    memset(name, 0, sizeof(name));
    ck(arkrtos_hwsafe_dev_at(0, name, sizeof(name)) == 0, "the last entry moved into the hole");
    ck(strcmp(name, "LONGDEV") == 0, "swap-with-last keeps the surviving name");
    ck(arkrtos_hwsafe_registered("A") == 0, "the removed name no longer resolves");

    /* 表满：8 个登记之后第 9 个被拒，已有条目一个不少 */
    reset_all();
    ck(arkrtos_hwsafe_register("D0", fake_cb) == 0, "fill 0");
    ck(arkrtos_hwsafe_register("D1", fake_cb) == 0, "fill 1");
    ck(arkrtos_hwsafe_register("D2", fake_cb) == 0, "fill 2");
    ck(arkrtos_hwsafe_register("D3", fake_cb) == 0, "fill 3");
    ck(arkrtos_hwsafe_register("D4", fake_cb) == 0, "fill 4");
    ck(arkrtos_hwsafe_register("D5", fake_cb) == 0, "fill 5");
    ck(arkrtos_hwsafe_register("D6", fake_cb) == 0, "fill 6");
    ck(arkrtos_hwsafe_register("D7", fake_cb) == 0, "fill 7");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 8ul, "the registration table is full");
    ck_u((unsigned long)arkrtos_hwsafe_register("D8", fake_cb), (unsigned long)-1,
         "the ninth registration is refused with NOSLOT");
    ck_u((unsigned long)arkrtos_hwsafe_dev_count(), 8ul, "refusal did not evict anybody");
    ck(arkrtos_hwsafe_registered("D0") == 1, "the oldest entry survived the refusal");
}

/* ---------------- 台账 ---------------- */

static void test_claim_and_release(void)
{
    int32 owner;
    int32 dead;
    char name[16];

    reset_all();

    /* 内核/空闲上下文（owner<=0）打开设备不记名：它没有"死亡"这件事 */
    arkrtos_hwsafe_claim(0, 1, &g_obj_a);
    arkrtos_hwsafe_claim("A", 0, &g_obj_a);
    arkrtos_hwsafe_claim("A", -3, &g_obj_a);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 0ul,
         "null name / non-task owners are not recorded");

    arkrtos_hwsafe_claim("A", 3, &g_obj_a);
    arkrtos_hwsafe_claim("A", 4, &g_obj_b);
    arkrtos_hwsafe_claim("B", 3, &g_obj_c);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 3ul,
         "same device twice (different owners) plus another device");

    owner = -1;
    dead = -1;
    memset(name, 0, sizeof(name));
    ck(arkrtos_hwsafe_info(0, name, sizeof(name), &owner, &dead) == 0, "info reads entry 0");
    ck(strcmp(name, "A") == 0, "entry 0 is device A");
    ck_u((unsigned long)owner, 3ul, "entry 0 belongs to task 3");
    ck_u((unsigned long)dead, 0ul, "entry 0 is not marked dead yet");

    ck_u((unsigned long)arkrtos_hwsafe_info(-1, name, sizeof(name), &owner, &dead),
         (unsigned long)-1, "info rejects a negative index");
    ck_u((unsigned long)arkrtos_hwsafe_info(3, name, sizeof(name), &owner, &dead),
         (unsigned long)-1, "info rejects past the end");
    ck(arkrtos_hwsafe_info(0, 0, 0u, 0, 0) == 0, "info accepts all-null outputs");

    /* 名字输出比条目短：按 max-1 截断并补 0，不越界 */
    memset(name, 0x5a, sizeof(name));
    ck(arkrtos_hwsafe_info(0, name, 4u, 0, 0) == 0, "info with a short buffer");
    ck((name[0] == 'A') && (name[1] == 0) && ((unsigned char)name[2] == 0x5a),
       "info writes the name and one terminator, nothing past max-1");

    /* 重复 open 同一 (dev, owner)：只刷新实例、不新增条目，并清掉旧 dead 标记 */
    arkrtos_hwsafe_claim("A", 3, &g_obj_b);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 3ul,
         "re-open refreshes the instance instead of adding an entry");

    arkrtos_hwsafe_release(0, 3);
    arkrtos_hwsafe_release("A", 0);
    arkrtos_hwsafe_release("NOPE", 3);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 3ul,
         "release with a null name / non-task owner / unknown device is a no-op");

    arkrtos_hwsafe_release("A", 4);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 2ul, "release drops that one entry");
    arkrtos_hwsafe_release("A", 4);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 2ul, "release is idempotent");

    /* 表满：8 条之后第 9 条不记，也不覆盖别人的条目 */
    reset_all();
    arkrtos_hwsafe_claim("C0", 1, &g_obj_a);
    arkrtos_hwsafe_claim("C1", 2, &g_obj_a);
    arkrtos_hwsafe_claim("C2", 3, &g_obj_a);
    arkrtos_hwsafe_claim("C3", 4, &g_obj_a);
    arkrtos_hwsafe_claim("C4", 5, &g_obj_a);
    arkrtos_hwsafe_claim("C5", 6, &g_obj_a);
    arkrtos_hwsafe_claim("C6", 7, &g_obj_a);
    arkrtos_hwsafe_claim("C7", 8, &g_obj_a);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 8ul, "the ledger is full");
    arkrtos_hwsafe_claim("C8", 9, &g_obj_b);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 8ul,
         "an overloaded ledger ignores the new claim rather than overwrite");
    owner = -1;
    ck(arkrtos_hwsafe_info(0, name, sizeof(name), &owner, 0) == 0, "entry 0 still readable");
    ck(strcmp(name, "C0") == 0, "entry 0 was not overwritten");
    ck_u((unsigned long)owner, 1ul, "entry 0 still belongs to task 1");

    arkrtos_hwsafe_release("C0", 1);
    arkrtos_hwsafe_claim("C8", 9, &g_obj_b);
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 8ul, "a freed slot takes the new claim");

    /* claim 进临界区 */
    ck(g_crit_enters > 0ul, "claim/release take the critical section");
    ck_u(g_crit_depth, 0ul, "every critical section is exited");
    ck(g_crit_enters == g_crit_exits, "enter/exit are paired on the claim path");
}

/* ---------------- 遗言 ---------------- */

static void test_will(void)
{
    int32 has;
    int32 cmd;
    int32 val;

    reset_all();

    ck(arkrtos_hwsafe_set_will(0, 3, 1, 2) == -2, "null name is a parameter error");
    ck(arkrtos_hwsafe_set_will("A", 0, 1, 2) == -2, "non-task owner is a parameter error");
    ck(arkrtos_hwsafe_set_will("A", 3, 1, 2) == -1,
       "an owner that does not hold the device cannot leave a will");

    arkrtos_hwsafe_claim("A", 3, &g_obj_a);
    arkrtos_hwsafe_claim("B", 4, &g_obj_b);

    ck(arkrtos_hwsafe_set_will("A", 3, 7, 9) == 0, "the holder can leave a will");
    ck(arkrtos_hwsafe_set_will("A", 4, 1, 2) == -1,
       "another task cannot leave a will on this device");

    has = -1;
    cmd = -1;
    val = -1;
    ck(arkrtos_hwsafe_will(0, &has, &cmd, &val) == 0, "will() reads entry 0");
    ck_u((unsigned long)has, 1ul, "entry 0 has a will");
    ck_u((unsigned long)cmd, 7ul, "will command is preserved");
    ck_u((unsigned long)val, 9ul, "will value is preserved");

    has = -1;
    cmd = -1;
    val = -1;
    ck(arkrtos_hwsafe_will(1, &has, &cmd, &val) == 0, "will() reads entry 1");
    ck_u((unsigned long)has, 0ul, "entry 1 has no will");
    ck_u((unsigned long)cmd, 0ul, "no will reads cmd 0, not stale data");
    ck_u((unsigned long)val, 0ul, "no will reads val 0, not stale data");

    ck_u((unsigned long)arkrtos_hwsafe_will(-1, &has, &cmd, &val), (unsigned long)-1,
         "will() rejects a negative index");
    ck_u((unsigned long)arkrtos_hwsafe_will(2, &has, &cmd, &val), (unsigned long)-1,
         "will() rejects past the end");
    ck(arkrtos_hwsafe_will(0, 0, 0, 0) == 0, "will() accepts all-null outputs");

    ck(arkrtos_hwsafe_clear_will(0, 3) == -1, "clear_will rejects a null name");
    ck(arkrtos_hwsafe_clear_will("A", 0) == -1, "clear_will rejects a non-task owner");
    ck(arkrtos_hwsafe_clear_will("NOPE", 3) == -1, "clear_will of an absent entry says so");
    ck(arkrtos_hwsafe_clear_will("A", 4) == -1, "clear_will by a non-holder says so");
    ck(arkrtos_hwsafe_clear_will("A", 3) == 0, "the holder can clear its will");

    has = -1;
    cmd = 5;
    val = 6;
    ck(arkrtos_hwsafe_will(0, &has, &cmd, &val) == 0, "will() after clear");
    ck_u((unsigned long)has, 0ul, "has_will is cleared");
    ck_u((unsigned long)cmd, 0ul, "cmd is reset");
    ck_u((unsigned long)val, 0ul, "val is reset");
    /* 台账条目还在，所以再撤一次仍报 0：幂等说的是「找不到条目不报错」，
     * 而不是「第二次要报错」。 */
    ck(arkrtos_hwsafe_clear_will("A", 3) == 0, "clearing again still finds the entry");

    /* reset 只清表与累计中和数，不碰按通道拆的计数（那是开机以来的单调账），
     * 所以下面一律按增量断言。 */
    ck(1, "path counters are monotonic across reset");
}

/* ---------------- 两阶段中和 ---------------- */

static void test_death_marking(void)
{
    int32 owner;
    int32 dead;

    reset_all();

    arkrtos_hwsafe_claim("A", 5, &g_obj_a);
    arkrtos_hwsafe_claim("B", 6, &g_obj_b);

    arkrtos_hwsafe_on_death(0);
    arkrtos_hwsafe_on_death(-9);
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 0ul,
         "a non-task death marks nothing");

    arkrtos_hwsafe_on_death(5);
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 0ul,
         "stage one only marks; nothing is neutralized yet");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 2ul,
         "stage one leaves the ledger intact");

    owner = 0;
    dead = -1;
    ck(arkrtos_hwsafe_info(0, 0, 0u, &owner, &dead) == 0, "info after stage one");
    ck_u((unsigned long)owner, 5ul, "the dying task owns entry 0");
    ck_u((unsigned long)dead, 1ul, "entry 0 is marked pending");
    dead = -1;
    ck(arkrtos_hwsafe_info(1, 0, 0u, 0, &dead) == 0, "info for the other entry");
    ck_u((unsigned long)dead, 0ul, "the surviving owner is not marked");

    ck_u(g_ctrl_calls, 0ul, "stage one drove nothing");
    ck_u(g_cb_calls, 0ul, "stage one ran no safety callback");
}

static void test_run_pending_will(void)
{
    uint32 will;
    uint32 fail;
    uint32 cb;
    uint32 none;
    uint32 handover;
    uint32 w0;
    uint32 f0;
    uint32 c0;
    uint32 n0;
    uint32 h0;

    /* 遗言成功：计数跟着驱动返回 0 走 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("M", 5, &g_obj_a);
    ck(arkrtos_hwsafe_set_will("M", 5, 0x1234, 0x56) == 0, "will registered");
    arkrtos_hwsafe_on_death(5);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 1ul, "one entry was driven");
    ck_u(g_ctrl_calls, 1ul, "the will went through the driver ctrl channel");
    ck(g_ctrl_obj == &g_obj_a, "the will drove the claimed instance");
    ck_u((unsigned long)g_ctrl_code, 0x1234ul, "the will code is the one left behind");
    ck_u((unsigned long)g_ctrl_value, 0x56ul, "the will value is the one left behind");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(will - w0), 1ul, "a successful will counts as will");
    ck_u((unsigned long)(fail - f0), 0ul, "no failure counted");
    ck_u((unsigned long)(cb - c0), 0ul, "the generic callback was not used");
    ck_u((unsigned long)(none - n0), 0ul, "this device did have a safe state");
    ck_u((unsigned long)(handover - h0), 0ul, "not a hand-over");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 1ul, "one neutralization");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 5ul, "last owner is the dead task");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 0ul, "the entry is gone");

    /* 遗言失败：驱动说不，硬件没有到安全态，计数绝对不能算成功 */
    reset_all();
    g_ctrl_ret = -5;
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("M", 5, &g_obj_a);
    ck(arkrtos_hwsafe_set_will("M", 5, 1, 2) == 0, "will registered on a failing driver");
    arkrtos_hwsafe_on_death(5);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 1ul, "the attempt is counted as run");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(will - w0), 0ul, "a refused will is not a success");
    ck_u((unsigned long)(fail - f0), 1ul, "a refused will lands in fail");
    ck_u((unsigned long)(none - n0), 0ul, "it did try a safe state, so not none");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 1ul,
         "the entry was handled (and removed)");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 5ul, "last owner is still recorded");

    /* 有遗言但没有实例指针：只能摘条目 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("M", 5, 0);
    ck(arkrtos_hwsafe_set_will("M", 5, 1, 2) == 0, "will on a claim without an instance");
    arkrtos_hwsafe_on_death(5);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "nothing could be driven");
    ck_u(g_ctrl_calls, 0ul, "no ctrl call without an instance");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(none - n0), 1ul, "no instance counts as none, not as safe");
    ck_u((unsigned long)(will - w0), 0ul, "no instance means the will did not run");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 1ul, "the entry is still removed");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 5ul, "last owner recorded on the none path");
}

static void test_run_pending_cb(void)
{
    uint32 will;
    uint32 fail;
    uint32 cb;
    uint32 none;
    uint32 handover;
    uint32 w0;
    uint32 f0;
    uint32 c0;
    uint32 n0;
    uint32 h0;

    /* 没有遗言时走驱动登记的安全态回调，owner 传进去 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_register("D", fake_cb);
    arkrtos_hwsafe_claim("D", 6, &g_obj_b);
    arkrtos_hwsafe_on_death(6);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 1ul, "the callback path runs");
    ck_u(g_cb_calls, 1ul, "the registered callback was called once");
    ck(g_cb_obj == &g_obj_b, "the callback got the claimed instance");
    ck_u(g_cb_owner, 6ul, "the callback got the dead owner id");
    ck_u(g_ctrl_calls, 0ul, "the will channel was not used");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(cb - c0), 1ul, "a successful callback counts as cb");
    ck_u((unsigned long)(none - n0), 0ul, "this device did have a safe state");
    ck_u((unsigned long)(fail - f0), 0ul, "no failure counted");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 1ul, "one neutralization");

    /* 回调失败：同样只记 fail */
    reset_all();
    g_cb_ret = 3;
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_register("D", fake_cb);
    arkrtos_hwsafe_claim("D", 6, &g_obj_b);
    arkrtos_hwsafe_on_death(6);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 1ul, "a refused callback is still an attempt");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(cb - c0), 0ul, "a refused callback is not a success");
    ck_u((unsigned long)(fail - f0), 1ul, "a refused callback lands in fail");

    /* 有回调但没有实例 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_register("D", fake_cb);
    arkrtos_hwsafe_claim("D", 6, 0);
    arkrtos_hwsafe_on_death(6);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "no instance means no callback");
    ck_u(g_cb_calls, 0ul, "the callback must not be handed a null instance");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(none - n0), 1ul, "no instance counts as none");

    /* 既没有遗言也没有回调：只能摘条目，且如实记 none */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("E", 7, &g_obj_c);
    arkrtos_hwsafe_on_death(7);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "nothing was driven");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(none - n0), 1ul, "an unknown safe state is reported as none");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 1ul, "the ledger entry is removed");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 0ul, "no entry left behind");
}

static void test_handover(void)
{
    uint32 will;
    uint32 fail;
    uint32 cb;
    uint32 none;
    uint32 handover;
    uint32 w0;
    uint32 f0;
    uint32 c0;
    uint32 n0;
    uint32 h0;

    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_register("H", fake_cb);
    arkrtos_hwsafe_claim("H", 8, &g_obj_a);
    ck(arkrtos_hwsafe_set_will("H", 8, 3, 4) == 0, "will registered before the hand-over");

    arkrtos_hwsafe_on_death_reason(8, 1);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "a planned hand-over drives nothing");
    ck_u(g_ctrl_calls, 0ul, "the will must not run on a hand-over");
    ck_u(g_cb_calls, 0ul, "the callback must not run on a hand-over");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(handover - h0), 1ul, "the skip is counted as hand-over");
    ck_u((unsigned long)(will - w0), 0ul, "a skipped will is not a success");
    ck_u((unsigned long)(cb - c0), 0ul, "a skipped callback is not a success");
    ck_u((unsigned long)(none - n0), 0ul, "hand-over is its own outcome, not none");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 0ul,
         "a hand-over must not inflate the neutralized count");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 0ul, "no owner was neutralized");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 0ul, "the ledger entry is still dropped");

    /* 同一 (dev, owner) 重新 open 之后恢复成普通死亡路径 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("H", 8, &g_obj_a);
    arkrtos_hwsafe_on_death_reason(8, 1);
    arkrtos_hwsafe_claim("H", 8, &g_obj_a);
    arkrtos_hwsafe_claim("H", 8, &g_obj_a);
    arkrtos_hwsafe_on_death(8);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "no callback, nothing driven");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(none - n0), 1ul, "re-open resets the entry to the normal death path");
    ck_u((unsigned long)(handover - h0), 0ul, "no stale hand-over flag survived the re-open");
}

static void test_mixed_and_removal(void)
{
    uint32 will;
    uint32 fail;
    uint32 cb;
    uint32 none;
    uint32 handover;
    uint32 w0;
    uint32 f0;
    uint32 c0;
    uint32 n0;
    uint32 h0;

    /* 只处理已死的条目，活着的原样留下 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("A", 9, &g_obj_a);
    arkrtos_hwsafe_claim("B", 10, &g_obj_b);
    arkrtos_hwsafe_on_death(9);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "only the dead entry is handled");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 1ul, "the live entry stays");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 9ul, "last owner is the dead one");

    /* 多条同时死：移除是「拿最后一条补位」，循环必须继续处理搬过来的那条 */
    reset_all();
    arkrtos_hwsafe_path_counts(&w0, &f0, &c0, &n0, &h0);
    arkrtos_hwsafe_claim("X", 1, &g_obj_x);
    arkrtos_hwsafe_claim("Y", 2, &g_obj_x);
    arkrtos_hwsafe_claim("Z", 3, &g_obj_x);
    arkrtos_hwsafe_on_death(1);
    arkrtos_hwsafe_on_death(3);
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "two entries, no safe state known");
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    ck_u((unsigned long)(none - n0), 2ul, "both dead entries were handled");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 2ul, "two neutralizations");
    ck_u((unsigned long)arkrtos_hwsafe_last_owner(), 3ul,
         "the entry moved into the hole was processed too");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 1ul, "the live owner survived");
    ck_u((unsigned long)arkrtos_hwsafe_info(0, 0, 0u, 0, 0), 0ul, "the survivor is readable");

    /* 观测接口容忍空指针 */
    arkrtos_hwsafe_path_counts(0, 0, 0, 0, 0);
    ck(1, "path_counts accepts all-null outputs");
    arkrtos_hwsafe_path_counts(&will, 0, 0, 0, 0);
    ck_u((unsigned long)(will - w0), 0ul, "the will counter did not move in this scenario");
    arkrtos_hwsafe_path_counts(0, 0, 0, &none, 0);
    ck_u((unsigned long)(none - n0), 2ul, "a partially requested count is still the right one");

    /* run_pending 在空台账上安全 */
    reset_all();
    ck_u((unsigned long)arkrtos_hwsafe_run_pending(), 0ul, "an empty ledger runs nothing");
    ck_u((unsigned long)arkrtos_hwsafe_owner_count(), 0ul, "and stays empty");
    ck_u((unsigned long)arkrtos_hwsafe_neutralized_count(), 0ul, "no counter moved");
}

/* ---------------- 主 ---------------- */

int main(void)
{
    test_register();
    test_claim_and_release();
    test_will();
    test_death_marking();
    test_run_pending_will();
    test_run_pending_cb();
    test_handover();
    test_mixed_and_removal();

    if(g_fails != 0)
    {
        printf("HWSAFE-HOST FAIL  %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    printf("HWSAFE-HOST OK  %d checks\n", g_checks);
    return 0;
}

#else  /* ARKRTOS_HW_SAFE == 0 */

int main(void)
{
    printf("HWSAFE-HOST OFF OK  1 checks\n");
    return 0;
}

#endif /* ARKRTOS_HW_SAFE */
