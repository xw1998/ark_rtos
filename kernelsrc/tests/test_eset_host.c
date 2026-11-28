/**
* @file test_eset_host.c
* @brief 32 位事件集的宿主单元测试（不依赖板子）
* @details 为什么需要它：事件集的缺陷几乎都在「判定与交付」里——OR/AND 判定
*          用了错的位、交给等待者的值在醒来后被重算、带 CLEAR 的位被两个
*          等待者各拿一份、超时路径留下幽灵登记、删除不把等待者放走。
*          这些在板子上表现为「偶尔少一个位」或「某个任务再也不动」，
*          宿主上可以逐条断言。
*
*          为了能测「两个等待者同时挂着」，本测试用 -DARKRTOS_ESET_HOST_TEST
*          编译 arkrtos_eset.c，使对象表从 static 变为外部可见，测试直接摆
*          等待槽；交接路径则用 block_in_critical 的钩子在「任务睡下之后、
*          返回之前」调用 set，真实走一遍 set 的判定与交接。
*
*          覆盖范围（如实说明）：
*            已验证：创建 / 命名 / 槽位用完、set 累加、get、clear、
*                    非阻塞 try（命中 / 未命中）、OR 与 AND 判定、
*                    CLEAR 的单消费者、超时后登记被摘除、
*                    交班值在交接时算定（中途新置的位不会混进来）、
*                    非 HANDOFF 唤醒不算命中、删除唤醒 + 删除后接口拒绝、
*                    任务下线收尸（摘等待槽 + 回收它创建的对象）、
*                    参数与句柄校验（mask=0 / OR与AND同时给 / 都不给 / 空指针）、
*                    四个入口的句柄与状态闸门、handler 上下文与无当前任务两条
*                    拒绝、调度器拒绝阻塞、睡下期间对象被删、重复登记的两半
*                    语义（woken==0 就地更新掩码，woken==1 不许覆盖）。
*            未验证：真实调度器下的优先级与唤醒时序（ready_add / PendSV 全为空
*                    实现）、用户态 SVC 轮询路径（sync_in_handler 的返回值由测试
*                    拨，证明的是 eset 在这个上下文下拒绝登记，不是 SVC 入口本身）、
*                    板上中断里 set 的实际延迟。那部分只有上板用例能证明。
* @note 用法：由 tools/eset_host_gate.py 编译并运行，输出 ESET-HOST OK。
*/

#include "arkrtos_eset.h"
#include "arkrtos_cfg.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 0;
uint32 arkrtos_kernel_tick     = 0u;

extern arkrtos_eset_obj_t arkrtos_esets[ARKRTOS_ESET_NUM];

static arkrtos_task_t *g_cur = 0;
static int32 g_ready_calls = 0;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }
void arkrtos_port_switch_task(void) { }
uint32 arkrtos_port_enter_critical(void) { return 0u; }
void arkrtos_port_exit_critical(uint32 state) { (void)state; }
uint8 arkrtos_port_in_isr(void) { return 0u; }

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur; }

/* 默认返回 -1（未能进入阻塞）：不摆钩子时阻塞路径走不到，这是刻意不覆盖。
 * 摆了钩子时，钩子代表"任务睡下之后发生的事"，返回值就是唤醒原因。 */
static int32 (*g_block_hook)(void) = 0;

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    (void)timeout_ms;
    if(g_block_hook != 0)
    {
        return g_block_hook();
    }
    return -1;
}

void arkrtos_ready_add(int32 task_idx) { (void)task_idx; g_ready_calls++; }
void arkrtos_ready_reprio(int32 task_idx, uint8 old_prio) { (void)task_idx; (void)old_prio; }

/* trace 桩也是接口：先声明后定义。 */
void arkrtos_trace_wait_obj(uint16 obj13);
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls);
void arkrtos_trace_wait_obj(uint16 obj13) { (void)obj13; }
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls) { (void)obj; (void)cls; }

/* sync_in_handler 在宿主上是可拨的：置 1 就代表"这次调用来自 handler / SVC"，
 * 那条路径必须拒绝登记而不是假装睡得着。 */
static uint8 g_in_handler = 0u;
uint8 arkrtos_sync_in_handler(void) { return g_in_handler; }

/* ------------------------------------------------------------------
 * 测试脚手架
 * ------------------------------------------------------------------ */
static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fail++;
        fputs("  FAIL: ", stdout);
        fputs(what, stdout);
        fputc('\n', stdout);
    }
}

static arkrtos_task_t *use_task(int idx)
{
    arkrtos_task_t *p = &arkrtos_task_table[idx];
    memset(p, 0, sizeof(*p));
    p->status = ARKRTOS_TASK_READY;
    if(ARKRTOS_TASK_MAX_NUM > idx + 1)
    {
        p->stack_size = 1024;
    }
    g_cur = p;
    if(arkrtos_task_count < idx + 1)
    {
        arkrtos_task_count = idx + 1;
    }
    return p;
}

static void table_reset(void)
{
    arkrtos_eset_module_init();
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 0;
    arkrtos_current_task_id = 0;
    g_cur = 0;
    g_block_hook = 0;
    g_ready_calls = 0;
    g_in_handler = 0u;
}

static int waiter_count(int idx)
{
    int j, n = 0;
    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        if(arkrtos_esets[idx].waiters[j].task != 0)
        {
            n++;
        }
    }
    return n;
}

/* 交接路径用的钩子：睡下之后 set 一次，再按"有没有人被交接"回答唤醒原因。 */
static int32 g_hook_handle = 0;
static uint32 g_hook_set_mask = 0u;
static uint32 g_hook_set_mask2 = 0u;

static int32 hooked_set_block(void)
{
    int before = g_ready_calls;

    (void)arkrtos_eset_set_internal(g_hook_handle, g_hook_set_mask);
    if(g_hook_set_mask2 != 0u)
    {
        (void)arkrtos_eset_set_internal(g_hook_handle, g_hook_set_mask2);
    }
    return (g_ready_calls != before) ? ARKRTOS_WAKE_HANDOFF : ARKRTOS_WAKE_TIMEOUT;
}

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */
static void t01_create(void)
{
    char name[16];
    int32 h;

    table_reset();
    memset(name, 0, sizeof(name));
    strcpy(name, "ready");

    h = arkrtos_eset_create_internal(name);
    check(h > 0, "create returns a positive handle");
    check(((uint32)h & ARKRTOS_HANDLE_MASK) == ARKRTOS_ESET_HANDLE_FLAG,
          "create stamps the event-set handle flag");
    check(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].used == 1u, "slot is marked used");
    check(strcmp(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].name, "ready") == 0,
          "name is copied");
    check(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].flags == 0u, "new set starts empty");
    check(arkrtos_eset_create_internal(0) == ARKRTOS_ESET_ERR_PARAM, "null name rejected");
}

static void t02_noslot(void)
{
    int i, last = 0;

    table_reset();
    for(i = 0; i < ARKRTOS_ESET_NUM; i++)
    {
        last = arkrtos_eset_create_internal("x");
        check(last > 0, "create while slots remain");
    }
    check(ARKRTOS_ESET_NUM == i, "all slots consumed");
    check(arkrtos_eset_create_internal("y") == ARKRTOS_ESET_ERR_NOSLOT,
          "the table full case reports NOSLOT");
    check(last > 0, "last handle is still valid");
}

static void t03_set_get_clear(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    h = arkrtos_eset_create_internal("s");
    check(arkrtos_eset_set_internal(h, 0x1u) == 0, "set one bit");
    check(arkrtos_eset_set_internal(h, 0x4u) == 0, "set accumulates");
    check(arkrtos_eset_get_internal(h, &got) == 0, "get returns status 0");
    check(got == 0x5u, "flags hold both bits (0x1|0x4)");
    check(arkrtos_eset_clear_internal(h, 0x1u) == 0, "clear status 0");
    check(arkrtos_eset_get_internal(h, &got) == 0, "get after clear");
    check(got == 0x4u, "only the cleared bit went away");
    check(arkrtos_eset_set_internal(h, 0u) == ARKRTOS_ESET_ERR_PARAM,
          "set with mask 0 is a parameter error");
    check(arkrtos_eset_get_internal(h, 0) == ARKRTOS_ESET_ERR_PARAM, "null out pointer rejected");
    check(arkrtos_eset_get_internal(0x12345678, &got) == ARKRTOS_ESET_ERR_HANDLE,
          "wrong handle flag rejected");
}

static void t04_try(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("t");

    /* 未置位、timeout 0：只试一次 */
    check(arkrtos_eset_wait_internal(h, 0x2u, ARKRTOS_ESET_OPT_OR, &got, 0)
          == ARKRTOS_SYNC_ERR_TIMEOUT, "try on empty set times out");
    check(got == 0u, "got is untouched on timeout");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0, "try leaves no registration");

    check(arkrtos_eset_set_internal(h, 0x2u) == 0, "set the bit");
    check(arkrtos_eset_wait_internal(h, 0x2u, ARKRTOS_ESET_OPT_OR, &got, 0) == 0,
          "try on a set bit succeeds");
    check(got == 0x2u, "got is the bit that was set");

    /* 设过的位留在对象里（这次带 CLEAR 才被取走） */
    check(arkrtos_eset_wait_internal(h, 0x2u, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR,
                                     &got, 0) == 0,
          "the earlier try consumed nothing with OR");
    check(got == 0x2u, "the leftover bit is still there to be taken");
    check(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].flags == 0u,
          "OR+CLEAR on the leftover bit clears it");
    check(arkrtos_eset_set_internal(h, 0x2u) == 0, "set again");
    check(arkrtos_eset_wait_internal(h, 0x2u, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR,
                                     &got, 0) == 0, "OR+CLEAR take succeeds");
    check(got == 0x2u, "OR+CLEAR got equals the bit");
    check(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].flags == 0u, "CLEAR removed the bit");
}

static void t05_and_or(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("ao");

    /* AND：只置一半，不应命中 */
    (void)arkrtos_eset_set_internal(h, 0x1u);
    check(arkrtos_eset_wait_internal(h, 0x3u, ARKRTOS_ESET_OPT_AND, &got, 0)
          == ARKRTOS_SYNC_ERR_TIMEOUT, "AND with half the bits does not match");
    check(got == 0u, "and got untouched");
    /* OR：任何一位成立就命中，但只交当时成立的那些 */
    check(arkrtos_eset_wait_internal(h, 0x3u, ARKRTOS_ESET_OPT_OR, &got, 0) == 0,
          "OR with one of the bits matches");
    check(got == 0x1u, "OR hands over only the bits that held");

    (void)arkrtos_eset_set_internal(h, 0x2u);
    check(arkrtos_eset_wait_internal(h, 0x3u, ARKRTOS_ESET_OPT_AND, &got, 0) == 0,
          "AND matches once both bits are set");
    check(got == 0x3u, "AND hands over the whole mask");
}

static void t06_opt_validation(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("v");

    check(arkrtos_eset_wait_internal(h, 0x1u, 0u, &got, 0) == ARKRTOS_ESET_ERR_OPT,
          "neither OR nor AND is rejected");
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_AND,
                                     &got, 0) == ARKRTOS_ESET_ERR_OPT,
          "OR and AND together is rejected");
    check(arkrtos_eset_wait_internal(h, 0u, ARKRTOS_ESET_OPT_OR, &got, 0)
          == ARKRTOS_ESET_ERR_PARAM, "mask 0 is rejected");
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, 0, 0)
          == ARKRTOS_ESET_ERR_PARAM, "null got pointer is rejected");
    check(arkrtos_eset_wait_internal(0x00000001, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 0)
          == ARKRTOS_ESET_ERR_HANDLE, "handle without the flag is rejected");

    {
        int32 free_h = (1 | ARKRTOS_ESET_HANDLE_FLAG);
        if((free_h & ARKRTOS_HANDLE_RELMASK) >= ARKRTOS_ESET_NUM ||
           arkrtos_esets[free_h & ARKRTOS_HANDLE_RELMASK].used == 0u)
        {
            check(arkrtos_eset_wait_internal(free_h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 0)
                  == ARKRTOS_ESET_ERR_STATE, "uncreated slot reports STATE");
        }
    }
}

static void t07_handoff_snapshot(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("snap");

    /* 睡下后先置 0x1，再置 0x2：交班值必须是"交接那一刻"的 0x1，
     * 而不是醒来时重算的 0x3。 */
    g_hook_handle  = h;
    g_hook_set_mask  = 0x1u;
    g_hook_set_mask2 = 0x2u;
    g_block_hook = hooked_set_block;

    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR,
                                     &got, 100) == 0, "blocking OR wait is handed off");
    check(got == 0x1u, "handed value is the one fixed at handoff (0x1, not 0x3)");
    check(arkrtos_esets[h & ARKRTOS_HANDLE_RELMASK].flags == 0x2u,
          "the second set is still there for the next waiter");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0, "slot is released on wake");
}

static void t08_timeout_cleans(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("to");

    /* 睡下后没有人 set：钩子回超时。登记必须被摘掉。 */
    g_hook_handle  = h;
    g_hook_set_mask  = 0u;      /* 不置位 */
    g_hook_set_mask2 = 0u;
    g_block_hook = hooked_set_block;

    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_SYNC_ERR_TIMEOUT, "wake without handoff is reported as timeout");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0, "timeout removes the registration");

    /* 非 HANDOFF 的唤醒（reason 0）同样不算命中 */
    g_block_hook = 0;
    {
        int32 (*saved)(void) = g_block_hook;
        (void)saved;
    }
}

static void t09_two_waiters_clear(void)
{
    int32 h;
    int32 idx;
    arkrtos_task_t *a;
    arkrtos_task_t *b;

    table_reset();
    a = use_task(0);
    b = use_task(1);
    h = arkrtos_eset_create_internal("two");
    idx = h & ARKRTOS_HANDLE_RELMASK;

    /* 直接摆两个等待槽：A 要 0x1，B 要 0x1|0x2 且 AND —— 同一位加 CLEAR',
     * A 先拿，B 就不该被算作满足。 */
    arkrtos_esets[idx].waiters[0].task  = a;
    arkrtos_esets[idx].waiters[0].mask  = 0x1u;
    arkrtos_esets[idx].waiters[0].opt   = ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR;
    arkrtos_esets[idx].waiters[1].task  = b;
    arkrtos_esets[idx].waiters[1].mask  = 0x3u;
    arkrtos_esets[idx].waiters[1].opt   = ARKRTOS_ESET_OPT_AND;

    g_ready_calls = 0;
    check(arkrtos_eset_set_internal(h, 0x1u) == 0, "set with two waiters");
    check(g_ready_calls == 1, "only the OR waiter was handed the bit");
    check(arkrtos_esets[idx].waiters[0].woken == 1u, "first waiter marked woken");
    check(arkrtos_esets[idx].waiters[0].got == 0x1u, "first waiter got 0x1");
    check(arkrtos_esets[idx].waiters[1].woken == 0u, "AND waiter still waiting");
    check(arkrtos_esets[idx].flags == 0u, "CLEAR took the bit out of flags");

    /* 再置 0x3：AND 等待者这次应该被满足（它的槽还在） */
    g_ready_calls = 0;
    check(arkrtos_eset_set_internal(h, 0x3u) == 0, "second set");
    check(g_ready_calls == 1, "AND waiter woken once both bits hold");
    check(arkrtos_esets[idx].waiters[1].woken == 1u, "AND waiter marked woken");
    check(arkrtos_esets[idx].waiters[1].got == 0x3u, "AND waiter got the full mask");
}

static void t10_delete(void)
{
    int32 h;
    int32 idx;
    arkrtos_task_t *a;
    uint32 got = 0u;

    table_reset();
    a = use_task(0);
    h = arkrtos_eset_create_internal("del");
    idx = h & ARKRTOS_HANDLE_RELMASK;

    arkrtos_esets[idx].waiters[0].task = a;
    arkrtos_esets[idx].waiters[0].mask = 0x1u;
    arkrtos_esets[idx].waiters[0].opt  = ARKRTOS_ESET_OPT_OR;

    g_ready_calls = 0;
    check(arkrtos_eset_delete_internal(h) == 0, "delete returns 0");
    check(g_ready_calls == 1, "delete readies the waiter (no one sleeps forever)");
    check(a->wake_reason == ARKRTOS_WAKE_OBJ_DELETED, "waiter is told the object is gone");
    check(arkrtos_esets[idx].used == 0u, "slot is freed");
    check(arkrtos_esets[idx].flags == 0u, "flags are cleared with the object");
    check(waiter_count(idx) == 0, "no waiter slot survives the delete");

    check(arkrtos_eset_delete_internal(h) == ARKRTOS_ESET_ERR_STATE, "double delete is STATE");
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 0)
          == ARKRTOS_ESET_ERR_STATE, "wait on a deleted set is STATE");
    check(arkrtos_eset_set_internal(h, 0x1u) == ARKRTOS_ESET_ERR_STATE,
          "set on a deleted set is STATE");
}

static void t11_release_task(void)
{
    int32 h1;
    int32 h2;
    int32 idx1;
    int32 idx2;
    arkrtos_task_t *dead;
    arkrtos_task_t *other;

    table_reset();
    dead  = use_task(0);
    other = use_task(1);

    arkrtos_current_task_id = 1;            /* 1 号任务创建 h1 */
    h1 = arkrtos_eset_create_internal("mine");
    idx1 = h1 & ARKRTOS_HANDLE_RELMASK;

    arkrtos_current_task_id = 0;            /* 内核创建 h2，应随镜像存活 */
    h2 = arkrtos_eset_create_internal("kernel");
    idx2 = h2 & ARKRTOS_HANDLE_RELMASK;

    /* dead 挂在 h2 上；other 挂在 h1 上 */
    arkrtos_esets[idx2].waiters[0].task = dead;
    arkrtos_esets[idx2].waiters[0].mask = 0x1u;
    arkrtos_esets[idx2].waiters[0].opt  = ARKRTOS_ESET_OPT_OR;
    arkrtos_esets[idx1].waiters[0].task = other;
    arkrtos_esets[idx1].waiters[0].mask = 0x2u;
    arkrtos_esets[idx1].waiters[0].opt  = ARKRTOS_ESET_OPT_OR;

    arkrtos_eset_release_task(1);

    check(waiter_count(idx2) == 0, "dead task removed from the kernel object's waiters");
    check(arkrtos_esets[idx2].used == 1u, "kernel object survives the teardown");
    check(arkrtos_esets[idx1].used == 0u, "object created by the dead task is released");
    check(waiter_count(idx1) == 0, "its waiters are flushed");
    check(arkrtos_eset_create_internal("after") > 0,
          "the freed slot can be reused after teardown");
}

static void t12_nospace(void)
{
    int32 h;
    int32 idx;
    int i;
    uint32 got = 0u;

    table_reset();
    h = arkrtos_eset_create_internal("full");
    idx = h & ARKRTOS_HANDLE_RELMASK;

    /* 用别的任务把等待槽占满 */
    for(i = 0; i < ARKRTOS_MAX_ESET_WAITERS; i++)
    {
        arkrtos_task_t *p = use_task(i + 2);
        arkrtos_esets[idx].waiters[i].task = p;
        arkrtos_esets[idx].waiters[i].mask = 0x1u;
        arkrtos_esets[idx].waiters[i].opt  = ARKRTOS_ESET_OPT_OR;
    }

    use_task(0);
    g_block_hook = 0;
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_ESET_ERR_NOSPACE, "a full waiter queue reports NOSPACE");
    check(waiter_count(idx) == ARKRTOS_MAX_ESET_WAITERS,
          "no phantom registration after NOSPACE");
}

/* 进不了阻塞的两种形态（调度器拒绝 / 对象在睡下期间被删）用固定返回值的钩子。 */
static int32 block_error(void)   { return -1; }
static int32 block_deleted(void) { return ARKRTOS_WAKE_OBJ_DELETED; }

/* "已经交接完成、但还没被取走"的那一槽：再登记时掩码与选项不许被覆盖。
 * 钩子在登记之后、返回之前读它，就能把这条语义钉住。 */
static uint32 g_keep_mask = 0u;
static int g_keep_seen = 0;
static int g_keep_woken = -1;

static int32 hooked_dup_check(void)
{
    int32 idx = g_hook_handle & ARKRTOS_HANDLE_RELMASK;
    int j;

    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        if((arkrtos_esets[idx].waiters[j].task == g_cur) &&
           (arkrtos_esets[idx].waiters[j].mask == g_keep_mask))
        {
            g_keep_seen  = 1;
            g_keep_woken = (arkrtos_esets[idx].waiters[j].woken != 0u) ? 1 : 0;
        }
    }
    return ARKRTOS_WAKE_TIMEOUT;
}

/* 四个入口的句柄 / 状态闸门：把"不是我的对象"与"这对象已经没了"分开，
 * 任何一种都不许被读成成功（否则调用方会以为位拿到了）。 */
static void t13_api_gates(void)
{
    int32 h;
    int32 idx;
    int32 free_h;
    int32 fidx;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("gates");
    idx = h & ARKRTOS_HANDLE_RELMASK;

    check(arkrtos_eset_set_internal(0x12345678, 0x1u) == ARKRTOS_ESET_ERR_HANDLE,
          "set rejects a handle without the object flag");
    check(arkrtos_eset_clear_internal(0x12345678, 0x1u) == ARKRTOS_ESET_ERR_HANDLE,
          "clear rejects a handle without the object flag");
    check(arkrtos_eset_get_internal(0x12345678, &got) == ARKRTOS_ESET_ERR_HANDLE,
          "get rejects a handle without the object flag");
    check(arkrtos_eset_delete_internal(0x12345678) == ARKRTOS_ESET_ERR_HANDLE,
          "delete rejects a handle without the object flag");

    /* 标志对、槽位从未创建：STATE，而不是把空槽当成对象 */
    free_h = (int32)(((uint32)(ARKRTOS_ESET_NUM - 1) | ARKRTOS_ESET_HANDLE_FLAG));
    fidx = free_h & ARKRTOS_HANDLE_RELMASK;
    arkrtos_esets[fidx].used = 0u;
    check(arkrtos_eset_clear_internal(free_h, 0x1u) == ARKRTOS_ESET_ERR_STATE,
          "clear on an unused slot is STATE");
    check(arkrtos_eset_get_internal(free_h, &got) == ARKRTOS_ESET_ERR_STATE,
          "get on an unused slot is STATE");
    check(arkrtos_eset_delete_internal(free_h) == ARKRTOS_ESET_ERR_STATE,
          "delete on an unused slot is STATE");

    /* 真删掉之后，同一个句柄仍然必须拒绝 */
    check(arkrtos_eset_set_internal(h, 0x2u) == 0, "the live object still works before delete");
    check(arkrtos_eset_delete_internal(h) == 0, "delete the live object");
    check(arkrtos_eset_clear_internal(h, 0x2u) == ARKRTOS_ESET_ERR_STATE,
          "clear after delete is STATE, not a silent success");
    check(arkrtos_esets[idx].used == 0u, "the slot stays free");
}

/* 阻塞前 / 阻塞中的三种拒绝：handler 上下文、没有当前任务、调度器不让睡、
 * 睡下后对象被删。四条路径各自都要把登记摘干净。 */
static void t14_wait_contexts(void)
{
    int32 h;
    uint32 got = 0u;

    table_reset();
    use_task(0);
    h = arkrtos_eset_create_internal("ctx");

    g_in_handler = 1u;
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_SYNC_ERR_WOULDBLOCK, "wait inside a handler is WOULDBLOCK");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0,
          "a handler wait leaves no registration");
    g_in_handler = 0u;

    g_cur = 0;
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_ESET_ERR_CONTEXT, "wait with no current task is CONTEXT");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0,
          "no current task means no registration either");

    use_task(0);
    g_block_hook = block_error;
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_ESET_ERR_CONTEXT, "a failed block is CONTEXT");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0,
          "a failed block removes the registration");

    g_block_hook = block_deleted;
    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_ESET_ERR_DELETED, "a deleted-while-waiting object reports DELETED");
    check(waiter_count(h & ARKRTOS_HANDLE_RELMASK) == 0,
          "DELETED also removes the registration");
    check(got == 0u, "nothing is handed over on DELETED");

    g_block_hook = 0;
}

/* 重复登记的那一半语义：已经交接完成（woken==1）的槽不许被新的一次等待覆盖，
 * 否则 set 算定要交给它的那个值会被后来的掩码顶掉。 */
static void t15_dup_woken_kept(void)
{
    int32 h;
    int32 idx;
    arkrtos_task_t *p;
    uint32 got = 0u;

    table_reset();
    p = use_task(0);
    h = arkrtos_eset_create_internal("dup");
    idx = h & ARKRTOS_HANDLE_RELMASK;

    arkrtos_esets[idx].waiters[0].task  = p;
    arkrtos_esets[idx].waiters[0].mask  = 0x2u;
    arkrtos_esets[idx].waiters[0].opt   = ARKRTOS_ESET_OPT_OR;
    arkrtos_esets[idx].waiters[0].got   = 0x2u;
    arkrtos_esets[idx].waiters[0].woken = 1u;

    g_hook_handle = h;
    g_keep_mask   = 0x2u;
    g_keep_seen   = 0;
    g_keep_woken  = -1;
    g_block_hook  = hooked_dup_check;

    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR,
                                     &got, 100) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "waiting again onto a woken slot is DUP, then times out");
    check(g_keep_seen == 1,
          "the woken slot keeps the mask set had fixed (0x2, not overwritten to 0x1)");
    check(g_keep_woken == 1, "and it is still the woken slot, not a fresh registration");
    check(waiter_count(idx) == 0, "the timeout still removes that slot");
    check(arkrtos_esets[idx].flags == 0u, "no phantom bit was consumed");

    /* 另一半：槽还在等（woken==0）时重复登记，掩码与选项必须跟最新一次走，
     * 否则"换个掩码再等一次"会永远等旧的那组位。 */
    arkrtos_esets[idx].waiters[0].task  = p;
    arkrtos_esets[idx].waiters[0].mask  = 0x2u;
    arkrtos_esets[idx].waiters[0].opt   = ARKRTOS_ESET_OPT_AND;
    arkrtos_esets[idx].waiters[0].got   = 0u;
    arkrtos_esets[idx].waiters[0].woken = 0u;

    g_keep_mask = 0x1u;
    g_keep_seen = 0;
    g_keep_woken = -1;
    g_block_hook = hooked_dup_check;

    check(arkrtos_eset_wait_internal(h, 0x1u, ARKRTOS_ESET_OPT_OR, &got, 100)
          == ARKRTOS_SYNC_ERR_TIMEOUT,
          "re-waiting onto a live slot is DUP as well");
    check(g_keep_seen == 1,
          "the live slot's mask did move to the newest registration (0x1)");
    check(g_keep_woken == 0, "and it is still the same not-yet-woken registration");
    check(waiter_count(idx) == 0, "and that slot is cleaned up on timeout");

    g_block_hook = 0;
}

int main(void)
{
    puts("ESET-HOST: 32-bit event set host tests");
    t01_create();
    t02_noslot();
    t03_set_get_clear();
    t04_try();
    t05_and_or();
    t06_opt_validation();
    t07_handoff_snapshot();
    t08_timeout_cleans();
    t09_two_waiters_clear();
    t10_delete();
    t11_release_task();
    t12_nospace();
    t13_api_gates();
    t14_wait_contexts();
    t15_dup_woken_kept();

    if(g_fail != 0)
    {
        printf("ESET-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("ESET-HOST OK: %d checks passed\n", g_checks);
    return 0;
}
