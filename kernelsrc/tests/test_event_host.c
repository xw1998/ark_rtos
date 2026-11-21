/**
* @file test_event_host.c
* @brief 事件对象（arkrtos_event.c）的宿主单元测试
* @details 事件是本仓库里唯一「自动复位」的同步对象：set 只记一个标记，wait
*          要么消费标记要么睡下去，两者之间没有计数也没有归属。它的破坏方式
*          因此都很安静：
*
*            - 先 set 后 wait 若把标记当成人来就丢，事件就永远收不到；
*            - 等待者登记上了却没进阻塞、或进了阻塞没登记，set 遍历不到它，
*              那个任务就永久睡死（这是本模块最贵的一类缺陷）；
*            - 等待队列满时若「跳过登记仍睡下去」，同样是永久阻塞；
*            - 唤醒原因不带交接位却当成拿到了事件，调用方会以为自己等到了；
*            - 创建者任务下线时不收尸，表在几次启动/停止之后就被占满；
*            - 内核建的对象（creator 0）必须活过任何一次任务下线。
*
*          被测的是真实的 arkrtos_event.c；任务表、阻塞、就绪队列与 trace 全部
*          由本文件桩住。「睡下之后发生什么」由可编程事件队列安排（见下方
*          「可编程阻塞桩」），所以这里证明的是状态机与账目，不是实时性。
*
*          两档都编：默认档（ARKRTOS_USE_WAITMULTI=0）与多对象探测档（=1）。
*          ready_internal 只在后者编入，是两段代码，必须分别编一次才看得见。
*
*          覆盖范围（如实说明）：
*            已验证：创建参数/句柄编码/表满/槽位复用、等待的三道闸门、
*                    先 set 后 wait 的消费语义、timeout==0 的只试一次与摘残留、
*                    用户态只登记（WOULDBLOCK）与重复登记容忍、等待队列满、
*                    交接唤醒/超时/对象删除/假醒/进不了阻塞五条唤醒路径、
*                    多等待者同时交接与唤醒后的账目、set 的静默拒绝、
*                    set_from_isr 恒成功、任务下线时摘人并连带删掉自己建的对象、
*                    内核对象幸存、多对象就绪探测（仅 multi 档）。
*            未验证：真实调度器与节拍下的时序（谁先跑、切换开销、ISR 抢占窗口）、
*                    两个任务真的并发等同一个事件、以及只有上板才有的路径。
* @note 用法：由 tools/coverage_gate.py 的两条 case 编译并运行，输出
*       EVENT-HOST OK（默认档）或 EVENT-HOST MULTI OK（WAITMULTI=1 档）。
*/

#include "arkrtos_event.h"
#include "arkrtos_cfg.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_trace.h"
#include "mdk_trace_arkrtos.h"

#include <stdio.h>
#include <string.h>

/* 这条 #error 是「派生宏真的落到了对象结构里」的编译期证据：如果某个头文件
 * 还在用旧的固定长度，这里先炸，不会等到运行时才看出来。 */
#if (ARKRTOS_USE_WAIT_UNBOUNDED == 1)
#if (ARKRTOS_WQ_SLOTS != ARKRTOS_TASK_MAX_NUM)
#error "unbounded waiters: ARKRTOS_WQ_SLOTS must equal ARKRTOS_TASK_MAX_NUM"
#endif
#else
#if (ARKRTOS_WQ_SLOTS != ARKRTOS_MAX_SYNC_WAITERS)
#error "default waiters: ARKRTOS_WQ_SLOTS must equal ARKRTOS_MAX_SYNC_WAITERS"
#endif
#endif

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 0;

/* 当前任务可以换：归属类判定（谁建的对象、谁在等）需要「换一个人来试」。 */
static arkrtos_task_t *g_cur = 0;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }

/* 置位后必须触发切换：原先只置 READY 不切，被唤醒的任务要等下一次 tick。 */
static int g_switch_calls = 0;
void arkrtos_port_switch_task(void) { g_switch_calls++; }

/* 1 = 当前处于异常（SVC/IRQ）上下文。用户态分支（只登记 + WOULDBLOCK）
 * 只在这条路上走到，所以它必须是可编程的。 */
static uint8 g_host_in_isr = 0u;
uint8 arkrtos_port_in_isr(void) { return g_host_in_isr; }

/* 本用例不链接 arkrtos_sync.c：它的 in_handler 就是读 IPSR，这里直接跟桩走。 */
uint8 arkrtos_sync_in_handler(void) { return g_host_in_isr; }

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur; }

/* 就绪队列桩：记「谁被放回就绪队列」与次数。 */
static int g_ready_calls = 0;
static int g_ready_last  = -1;
void arkrtos_ready_add(int32 task_idx)
{
    g_ready_calls++;
    g_ready_last = (int)task_idx;
}

/* trace 桩也是接口：先声明后定义（-Wmissing-prototypes）。 */
void arkrtos_trace_wait_obj(uint16 obj13);
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls);
void arkrtos_trace_wait_obj(uint16 obj13) { (void)obj13; }
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls) { (void)obj; (void)cls; }

/* 阻塞桩：真正的实现放在测试脚手架之后（它要用 g_evq）。 */
int32 arkrtos_task_block_in_critical(uint32 timeout_ms);

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

#define CK_EQ(a, b, msg)                                                     \
    do {                                                                     \
        long va = (long)(a);                                                 \
        long vb = (long)(b);                                                 \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %ld, want %ld)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

/* ------------------------------------------------------------------
 * 可编程阻塞桩
 *
 * 宿主上没有调度器，「睡下去以后会发生什么」只能由测试来安排。约定是：
 *   调用 wait 之前，把「睡下之后发生什么」按顺序压进事件队列；
 *   桩取出一个事件执行它，再把任务的 wake_reason 交回被测代码。
 *   队列为空时返回 -1（进不了阻塞），与最朴素的桩一致。
 *
 * 桩模拟的是「睡下之后别人动了一下」那一刻，不推进节拍、不切任务，只按真实
 * 路径改事件表与任务状态，所以它证明的是状态机与账目，不是实时性。
 * ------------------------------------------------------------------ */
#define HOST_EV_QUEUE_LEN 8

enum {
    HOST_EV_HANDOFF = 1,        /* 睡眠期间事件被置位：真交接（set 走全路径） */
    HOST_EV_TIMEOUT,            /* 等超时：wake_reason = 1 */
    HOST_EV_DELETED,            /* 等期间对象被删：wake_reason = 2 */
    HOST_EV_GHOST,              /* 假醒：wake_reason 保持 0，不带交接位 */
    HOST_EV_ODD_REASON,         /* 原因 3：既不是超时也不是交接位 */
    HOST_EV_REFUSED             /* 进不了阻塞（-1）：内核上下文 / 调度器锁定 */
};

typedef struct {
    uint8 kind;
    int32 handle;
} host_ev_t;

static host_ev_t g_evq[HOST_EV_QUEUE_LEN];
static int      g_evq_n    = 0;
static int      g_evq_head = 0;
static int      g_block_calls = 0;

static void host_ev_push(uint8 kind, int32 handle)
{
    if(g_evq_head >= g_evq_n)
    {
        g_evq_head = 0;
        g_evq_n    = 0;
    }
    if(g_evq_n >= HOST_EV_QUEUE_LEN)
    {
        g_checks++;
        g_fails++;
        fputs("  FAIL: host event queue overflow\n", stdout);
        return;
    }
    g_evq[g_evq_n].kind   = kind;
    g_evq[g_evq_n].handle = handle;
    g_evq_n++;
}

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    host_ev_t ev;
    arkrtos_task_t *p = g_cur;

    g_block_calls++;
    if(p != 0)
    {
        /* 真实路径里这一句与登记同处一个临界区：先把状态摆成「睡着」。 */
        p->status      = ARKRTOS_TASK_WAIT;
        p->wait_time   = (int32)timeout_ms;
        p->wake_reason = (uint8)ARKRTOS_WAKE_NORMAL;
    }
    if(g_evq_head >= g_evq_n)
    {
        return -1;              /* 没安排事件：与最朴素的桩一致 */
    }
    ev = g_evq[g_evq_head++];
    switch(ev.kind)
    {
    case HOST_EV_HANDOFF:
        /* 走真实的 set：置标记、摘等待者、置 READY、加就绪队列、触发切换。 */
        arkrtos_event_set_internal(ev.handle);
        break;
    case HOST_EV_TIMEOUT:
        return ARKRTOS_WAKE_TIMEOUT;
    case HOST_EV_DELETED:
        return ARKRTOS_WAKE_OBJ_DELETED;
    case HOST_EV_GHOST:
        return ARKRTOS_WAKE_NORMAL;
    case HOST_EV_ODD_REASON:
        return 3;
    case HOST_EV_REFUSED:
        return -1;
    default:
        return -1;
    }
    return (p != 0) ? (int32)p->wake_reason : -1;
}

/* ------------------------------------------------------------------
 * 测试脚手架
 * ------------------------------------------------------------------ */
static void reset_all(void)
{
    int32 i;

    for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
    {
        memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    arkrtos_task_count      = 0;
    arkrtos_current_task_id = 0;
    g_cur           = 0;
    g_host_in_isr   = 0u;
    g_evq_n         = 0;
    g_evq_head      = 0;
    g_block_calls   = 0;
    g_switch_calls  = 0;
    g_ready_calls   = 0;
    g_ready_last    = -1;
    arkrtos_event_module_init();
}

/* 让下标 idx 的任务成为「当前任务」：id = idx + 1。 */
static void use_task(int32 idx)
{
    arkrtos_task_table[idx].status        = ARKRTOS_TASK_READY;
    arkrtos_task_table[idx].priority      = 5u;
    arkrtos_task_table[idx].base_priority = 5u;
    g_cur = &arkrtos_task_table[idx];
    arkrtos_current_task_id = idx + 1;
    if(arkrtos_task_count < idx + 1)
    {
        arkrtos_task_count = idx + 1;
    }
}

static void no_task(void)
{
    g_cur = 0;
    arkrtos_current_task_id = 0;
}

/* 当前任务以「用户态等待者」身份登记到 h 上（返回 WOULDBLOCK）。 */
static void register_waiter(int32 h)
{
    int32 rc;

    g_host_in_isr = 1u;
    rc = arkrtos_event_wait_internal(h, 100);
    g_host_in_isr = 0u;
    CK_EQ(rc, ARKRTOS_SYNC_ERR_WOULDBLOCK, "register a user-mode waiter");
}

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */
static void test_create(void)
{
    int32 i;
    int32 h[ARKRTOS_EVENT_NUM];
    int32 idx;

    reset_all();

    CK_EQ(arkrtos_event_create_internal(0), ARKRTOS_EVENT_ERR_PARAM,
          "create(NULL) is rejected");

    use_task(0);
    h[0] = arkrtos_event_create_internal("e0");
    ck(h[0] > 0, "create returns a handle");
    CK_EQ((int32)((uint32)h[0] & (uint32)ARKRTOS_HANDLE_MASK),
          ARKRTOS_EVENT_HANDLE_FLAG,
          "the handle carries the event type flag");
    idx = h[0] & ARKRTOS_HANDLE_RELMASK;
    ck(idx >= 0 && idx < ARKRTOS_EVENT_NUM, "the handle index is inside the table");
    CK_EQ(idx, 0, "the first object lands in the lowest free slot");

    h[1] = arkrtos_event_create_internal("e1");
    ck(h[1] > h[0], "the second object gets the next slot");

    /* 名字只用前 15 个字符：这里给一个超长名字，验证不会越界写。 */
    h[2] = arkrtos_event_create_internal("0123456789abcdefghij");
    ck(h[2] > h[1], "a long name still creates an object (truncated to 15)");

    /* 表满：剩下的槽位一次填满，再多一个必须报 NOSLOT。 */
    for(i = 3; i < ARKRTOS_EVENT_NUM; i++)
    {
        h[i] = arkrtos_event_create_internal("fill");
        ck(h[i] > 0, "filling the table keeps succeeding");
        CK_EQ(h[i] & ARKRTOS_HANDLE_RELMASK, i, "filled slots are consecutive");
    }
    CK_EQ(arkrtos_event_create_internal("overflow"), ARKRTOS_EVENT_ERR_NOSLOT,
          "a full table reports NOSLOT");

    /* creator 归属：当前任务建的对象，下线时一起走。 */
    reset_all();
    no_task();
    h[0] = arkrtos_event_create_internal("kernel");
    ck(h[0] > 0, "an object created with no current task is accepted");

    use_task(1);
    h[1] = arkrtos_event_create_internal("app");
    ck(h[1] > 0, "an object created by a task is accepted");

    use_task(0);        /* 换一个人来触发下线收尸 */
    arkrtos_task_count = 3;
    arkrtos_event_release_task(2);
    CK_EQ(arkrtos_event_wait_internal(h[1], 0), ARKRTOS_EVENT_ERR_STATE,
          "the creator's event is gone after that task leaves");
    CK_EQ(arkrtos_event_wait_internal(h[0], 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "a kernel object survives any task leaving");

    /* 槽位复用：被收尸的槽位必须能被新对象拿到。 */
    h[2] = arkrtos_event_create_internal("again");
    CK_EQ(h[2] & ARKRTOS_HANDLE_RELMASK, h[1] & ARKRTOS_HANDLE_RELMASK,
          "the freed slot is reused");
}

static void test_wait_guards(void)
{
    int32 h;
    int32 bad_flag;
    int32 oor;

    reset_all();
    use_task(0);

    /* 未创建：idx 合法但 used==0。 */
    CK_EQ(arkrtos_event_wait_internal(ARKRTOS_EVENT_HANDLE_FLAG, 0),
          ARKRTOS_EVENT_ERR_STATE, "an unused slot reports STATE");
    CK_EQ(arkrtos_event_wait_internal(ARKRTOS_EVENT_HANDLE_FLAG, 100),
          ARKRTOS_EVENT_ERR_STATE, "an unused slot reports STATE when blocking too");

    h = arkrtos_event_create_internal("wg");
    ck(h > 0, "guard case creates an object");

    bad_flag = h + 0x00100000;
    CK_EQ(arkrtos_event_wait_internal(bad_flag, 0), ARKRTOS_EVENT_ERR_HANDLE,
          "a handle with the wrong type flag is rejected");

    oor = (int32)ARKRTOS_EVENT_NUM | ARKRTOS_EVENT_HANDLE_FLAG;
    CK_EQ(arkrtos_event_wait_internal(oor, 0), ARKRTOS_EVENT_ERR_HANDLE,
          "an out-of-range index is rejected");
    CK_EQ(g_block_calls, 0, "neither guard reached the blocking path");
}

static void test_flag_consume(void)
{
    int32 h;

    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("fc");
    ck(h > 0, "flag case creates an object");

    arkrtos_event_set_internal(h);
    CK_EQ(g_switch_calls, 1, "set always asks for a switch");

    CK_EQ(arkrtos_event_wait_internal(h, 100), 0,
          "a set made before the wait is not lost");
    CK_EQ(g_block_calls, 0, "the waiting task never went to sleep");

    CK_EQ(arkrtos_event_wait_internal(h, 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "the flag was consumed by the first wait");

    arkrtos_event_set_internal(h);
    CK_EQ(arkrtos_event_wait_internal(h, 0), 0, "the next set is takeable once");
    CK_EQ(arkrtos_event_wait_internal(h, 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "and only once");
}

static void test_try_once(void)
{
    int32 h;

    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("to");
    ck(h > 0, "try-once case creates an object");

    CK_EQ(arkrtos_event_wait_internal(h, 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "0 timeout with nothing set reports TIMEOUT");
    CK_EQ(g_block_calls, 0, "0 timeout never enters the blocking path");

    /* 用户态轮询已经把人登记进去了，再来一次只试一次必须把残留摘掉：
     * 否则下一次 set 会唤醒一个根本不再等的人。 */
    register_waiter(h);
    register_waiter(h);         /* 重复登记是正常情况，不能占满队列 */
    CK_EQ(arkrtos_event_wait_internal(h, 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "try-once still reports TIMEOUT");

    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 0,
          "the stale registration was dropped: set wakes nobody");
    CK_EQ(arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
          "the task was never put to sleep");
    CK_EQ(arkrtos_event_wait_internal(h, 0), 0,
          "the set is still parked in the flag for the next poll");
}

static void test_handler_poll(void)
{
    int32 h;

    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("hp");
    ck(h > 0, "handler case creates an object");

    g_host_in_isr = 1u;
    CK_EQ(arkrtos_event_wait_internal(h, 100), ARKRTOS_SYNC_ERR_WOULDBLOCK,
          "a user-mode waiter only registers and reports WOULDBLOCK");
    CK_EQ(g_block_calls, 0, "a user-mode waiter never sleeps inside the handler");
    CK_EQ(arkrtos_event_wait_internal(h, 100), ARKRTOS_SYNC_ERR_WOULDBLOCK,
          "polling again is not an error either");
    g_host_in_isr = 0u;

    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 1, "set wakes the registered user-mode waiter");
    CK_EQ(arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
          "the waiter is marked ready");
    CK_EQ(arkrtos_task_table[0].wake_reason, ARKRTOS_WAKE_HANDOFF,
          "the wake carries the handoff bit");
    CK_EQ(arkrtos_task_table[0].wait_time, 0, "the wait time is cleared");

    CK_EQ(arkrtos_event_wait_internal(h, 0), 0,
          "set leaves the flag set, so the next poll finds the event");
}

static void test_queue_full(void)
{
    int32 h;
    int32 i;

    reset_all();
    no_task();
    h = arkrtos_event_create_internal("qf");
    ck(h > 0, "queue case creates an object");

#if (ARKRTOS_WQ_SLOTS < ARKRTOS_TASK_MAX_NUM)
    for(i = 0; i < ARKRTOS_WQ_SLOTS; i++)
    {
        use_task(i);
        register_waiter(h);
    }
    use_task(ARKRTOS_WQ_SLOTS);
    CK_EQ(arkrtos_event_wait_internal(h, 100), ARKRTOS_EVENT_ERR_NOSPACE,
          "a full waiter queue reports NOSPACE instead of sleeping forever");
    CK_EQ(g_block_calls, 0, "the refused waiter never slept");

    no_task();
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, ARKRTOS_WQ_SLOTS,
          "set hands the event to every registered waiter");
    CK_EQ(arkrtos_task_table[ARKRTOS_WQ_SLOTS].status, ARKRTOS_TASK_READY,
          "the refused task was never marked waiting");
#else
    /* 无界档下等待槽位等于任务表容量，队列满不可能发生。 */
    ck(1, "unbounded waiter queue: full-queue case not applicable");
#endif
}

static void test_block_paths(void)
{
    int32 h;

    /* 1) 没有当前任务：登记之前就要拒。 */
    reset_all();
    no_task();
    h = arkrtos_event_create_internal("bp");
    ck(h > 0, "block case creates an object");
    CK_EQ(arkrtos_event_wait_internal(h, 100), ARKRTOS_EVENT_ERR_CONTEXT,
          "no current task means CONTEXT");
    CK_EQ(g_block_calls, 0, "no current task never reaches the blocking path");

    /* 2) 正常交接：睡下之后事件被置位。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp2");
    host_ev_push(HOST_EV_HANDOFF, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), 0, "a handoff wakes the waiter");
    CK_EQ(g_block_calls, 1, "the waiter did sleep once");
    CK_EQ(arkrtos_task_table[0].status, ARKRTOS_TASK_READY, "it is ready again");
    CK_EQ(arkrtos_task_table[0].wake_reason, ARKRTOS_WAKE_HANDOFF,
          "the wake carries the handoff bit");
    CK_EQ(arkrtos_task_table[0].wait_time, 0, "the wait time is cleared");
    CK_EQ(g_ready_calls, 1, "the waiter was put back on the ready queue");
    CK_EQ(g_ready_last, 0, "the ready queue got the right task");
    CK_EQ(g_switch_calls, 1, "set asked for a switch after the handoff");

    /* 3) 超时：必须摘人，否则下一次 set 会唤醒一个不再等的人。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp3");
    host_ev_push(HOST_EV_TIMEOUT, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), ARKRTOS_SYNC_ERR_TIMEOUT,
          "a timeout is reported as TIMEOUT");
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 0, "a timed-out waiter was removed from the queue");

    /* 4) 对象被删除：报错，并且不把这次唤醒当成拿到了事件。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp4");
    host_ev_push(HOST_EV_DELETED, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), ARKRTOS_EVENT_ERR_DELETED,
          "an object deleted while waiting reports DELETED");
    /* 登记留在队列里是有意的：真实路径上对象整块被清掉，这里对象还活着，
     * 所以下一次 set 仍能找到这个等待者。 */
    g_ready_calls = 0;
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 1, "the waiter was not needed to be removed by hand");

    /* 5) 进不了阻塞：报 CONTEXT 且撤销登记。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp5");
    host_ev_push(HOST_EV_REFUSED, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), ARKRTOS_EVENT_ERR_CONTEXT,
          "a refused block reports CONTEXT");
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 0, "the refused waiter was removed from the queue");

    /* 6) 假醒：原因 0，不带交接位，不能当成拿到了事件。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp6");
    host_ev_push(HOST_EV_GHOST, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), ARKRTOS_SYNC_ERR_TIMEOUT,
          "a wake with reason 0 is not a handoff");
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 0, "the ghost waiter was removed from the queue");

    /* 7) 原因 3：既不是超时值也不是交接位，同样不认。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp7");
    host_ev_push(HOST_EV_ODD_REASON, h);
    CK_EQ(arkrtos_event_wait_internal(h, 50), ARKRTOS_SYNC_ERR_TIMEOUT,
          "an unknown reason without the handoff bit is not a handoff");
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 0, "the odd waiter was removed from the queue");

    /* 8) 唤醒原因确实是 set 写的那个位，而不是桩的中间值。 */
    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("bp8");
    host_ev_push(HOST_EV_HANDOFF, h);
    CK_EQ(arkrtos_event_wait_internal(h, 0 + 1000), 0, "a long wait hands off too");
    CK_EQ(arkrtos_task_table[0].wait_time, 0, "handoff clears the wait time");
}

static void test_set_wakes(void)
{
    int32 h;
    int32 i;

    reset_all();
    h = arkrtos_event_create_internal("sw");
    ck(h > 0, "set case creates an object");

    for(i = 0; i < 3; i++)
    {
        use_task(i);
        register_waiter(h);
    }

    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 3, "every registered waiter is handed the event");
    CK_EQ(g_switch_calls, 1, "one switch for one set");
    CK_EQ(g_ready_last, 2, "the last waiter on the list is the last woken");
    for(i = 0; i < 3; i++)
    {
        CK_EQ(arkrtos_task_table[i].status, ARKRTOS_TASK_READY,
              "every waiter is marked ready");
        CK_EQ(arkrtos_task_table[i].wake_reason, ARKRTOS_WAKE_HANDOFF,
              "every waiter got the handoff bit");
        CK_EQ(arkrtos_task_table[i].wait_time, 0,
              "every waiter had its wait time cleared");
    }

    /* 队列已被清空：再 set 只置标记，不再叫醒任何人。 */
    arkrtos_event_set_internal(h);
    CK_EQ(g_ready_calls, 3, "a second set wakes nobody");
    CK_EQ(g_switch_calls, 2, "but it still asks for a switch");

    /* 静默拒绝：坏标志 / 越界 / 未创建都不许切任务。 */
    g_switch_calls = 0;
    arkrtos_event_set_internal(h + 0x00100000);
    arkrtos_event_set_internal((int32)ARKRTOS_EVENT_NUM | ARKRTOS_EVENT_HANDLE_FLAG);
    CK_EQ(g_switch_calls, 0, "a bad handle is rejected silently");

    reset_all();
    CK_EQ(arkrtos_event_wait_internal(ARKRTOS_EVENT_HANDLE_FLAG, 0),
          ARKRTOS_EVENT_ERR_STATE, "the reset table has no objects");
    g_switch_calls = 0;
    arkrtos_event_set_internal(ARKRTOS_EVENT_HANDLE_FLAG);
    CK_EQ(g_switch_calls, 0, "set on an unused slot is rejected silently");
}

static void test_set_from_isr(void)
{
    int32 h;

    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("isr");
    ck(h > 0, "isr case creates an object");

    register_waiter(h);
    CK_EQ(arkrtos_event_set_from_isr(h), 0, "set_from_isr reports success");
    CK_EQ(g_ready_calls, 1, "set_from_isr hands the event over");
    CK_EQ(g_switch_calls, 1, "set_from_isr still asks for a switch");

    /* 中断里不报错：坏句柄也只能静默失败，因为 ISR 上下文没有返回值通道。 */
    CK_EQ(arkrtos_event_set_from_isr(h + 0x00100000), 0,
          "set_from_isr always reports success");
    CK_EQ(arkrtos_event_set_from_isr(ARKRTOS_EVENT_HANDLE_FLAG), 0,
          "set_from_isr on an unused slot also reports success");
}

static void test_release_task(void)
{
    int32 hk;
    int32 ha;

    reset_all();
    no_task();
    hk = arkrtos_event_create_internal("kernel");
    ck(hk > 0, "kernel object created");

    use_task(0);
    ha = arkrtos_event_create_internal("app");
    ck(ha > 0, "app object created");
    arkrtos_task_count = 4;

    /* 非法 id：一个都不许动。 */
    register_waiter(hk);
    arkrtos_event_release_task(0);
    arkrtos_event_release_task(5);
    arkrtos_event_set_internal(hk);
    CK_EQ(g_ready_calls, 1, "an out-of-range id changes nothing");
    /* set 只留标记：先消费掉，下面才有干净的等待者可摘 */
    CK_EQ(arkrtos_event_wait_internal(hk, 0), 0,
          "the flag left by set is consumable");

    /* 摘人：只摘这个任务，对象本身还在。 */
    register_waiter(hk);
    arkrtos_event_release_task(1);
    arkrtos_event_set_internal(hk);
    CK_EQ(g_ready_calls, 1, "the dead task was removed from the waiter list");
    CK_EQ(arkrtos_event_wait_internal(hk, 0), 0,
          "the kernel object itself is still usable");

    /* 连带收尸：创建者下线，对象整块清掉（含等待者）。 */
    reset_all();
    no_task();
    hk = arkrtos_event_create_internal("kernel2");
    ck(hk > 0, "kernel object recreated");
    use_task(1);
    ha = arkrtos_event_create_internal("app2");
    ck(ha > 0, "app object recreated");
    arkrtos_task_count = 4;

    use_task(0);
    register_waiter(ha);        /* 另一个任务在等这个 App 的事件 */
    arkrtos_event_release_task(2);
    CK_EQ(arkrtos_event_wait_internal(ha, 0), ARKRTOS_EVENT_ERR_STATE,
          "the creator's object is gone");
    arkrtos_event_set_internal(ha);
    CK_EQ(g_ready_calls, 0, "its waiters were flushed without a wake");
    CK_EQ(arkrtos_event_wait_internal(hk, 0), ARKRTOS_SYNC_ERR_TIMEOUT,
          "the kernel object is untouched");
}

#if (ARKRTOS_USE_WAITMULTI == 1)
static void test_ready_internal(void)
{
    int32 h;

    reset_all();
    use_task(0);
    h = arkrtos_event_create_internal("rd");
    ck(h > 0, "ready case creates an object");

    CK_EQ(arkrtos_event_ready_internal(h), 0, "a fresh object is not ready");
    arkrtos_event_set_internal(h);
    CK_EQ(arkrtos_event_ready_internal(h), 1, "a set object reports ready");
    CK_EQ(arkrtos_event_ready_internal(h), 1,
          "probing does not consume the flag");
    CK_EQ(arkrtos_event_wait_internal(h, 0), 0, "the flag is still takeable");
    CK_EQ(arkrtos_event_ready_internal(h), 0, "and gone after the take");

    CK_EQ(arkrtos_event_ready_internal(h + 0x00100000), -1,
          "a handle with the wrong type flag is rejected");
    CK_EQ(arkrtos_event_ready_internal((int32)ARKRTOS_EVENT_NUM
                                       | ARKRTOS_EVENT_HANDLE_FLAG), -1,
          "an out-of-range index is rejected");
    CK_EQ(arkrtos_event_ready_internal(1 | ARKRTOS_EVENT_HANDLE_FLAG), -1,
          "an unused slot is rejected");
}
#endif /* ARKRTOS_USE_WAITMULTI == 1 */

int main(void)
{
    reset_all();

    test_create();
    test_wait_guards();
    test_flag_consume();
    test_try_once();
    test_handler_poll();
    test_queue_full();
    test_block_paths();
    test_set_wakes();
    test_set_from_isr();
    test_release_task();
#if (ARKRTOS_USE_WAITMULTI == 1)
    test_ready_internal();
#endif

    if(g_fails != 0)
    {
        printf("EVENT-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
#if (ARKRTOS_USE_WAITMULTI == 1)
    printf("EVENT-HOST MULTI OK %d checks\n", g_checks);
#else
    printf("EVENT-HOST OK %d checks\n", g_checks);
#endif
    return 0;
}
