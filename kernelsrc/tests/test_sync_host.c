/**
* @file test_sync_host.c
* @brief 信号量 / 互斥锁 / 条件变量的宿主单元测试（不依赖板子）
* @details 为什么需要它：「令牌守恒」是 docs/同步原语与令牌守恒.md 里写下的契约，
*          而它的破坏方式几乎都在计数与归属判定里——wait 超时却把计数减了、
*          post 唤醒失败却把计数加了、非持有者解锁成功、重复加锁被当成递归。
*          这些在板子上表现为"偶尔死一次"，在宿主上可以逐条断言。
*
*          与 sbuf/mq 的宿主测试不同，这里 task_get_current() 返回**可设置的**
*          真实指针，所以归属类判定（谁能解锁、谁算重复加锁）能被真正走到。
*
*          覆盖范围（如实说明）：
*            已验证：信号量初始计数与逐个取用、计数为 0 时 0 超时返回 TIMEOUT、
*                    post 归还计数、删除后接口一律拒绝（含重复删除）、
*                    互斥锁归属（持有者加锁被拒、非持有者解锁被拒、解锁后可再取）、
*                    被删除的锁不计为仍被持有、条件变量 signal 无人等待不算错误、
*                    非法句柄被拒。
*            已验证（2026-11-21 起，用可编程阻塞桩）：等待者真正入队后被 post /
*                    unlock / signal 交接令牌、超时唤醒后把自己摘出队列、假醒只记
*                    幽灵不认令牌、等待期间对象被删返回 DELETED、进不了阻塞报错并
*                    撤销登记、用户态等待者只登记（post 改为加计数）、等待队列满报
*                    NOSPACE（仅默认有界档；无界档下满队列不可能，对应用例跳过）、条件变量返回前重新持锁与递归深度恢复、任务下线时把
*                    自己建的对象一起带走。
*            未验证：真实调度器与节拍下的时序（谁先跑、切换开销、ISR 抢占窗口），
*                    以及只有上板才有的路径。桩只模拟「睡下之后别人动了一下」那一
*                    刻，不模拟时间流逝和任务切换，所以这里证明的是状态机与账目，
*                    不是实时性。
* @note 用法：由 tools/sync_host_gate.py 编译并运行，输出 SYNC-HOST OK。
*/

#include "arkrtos_sync.h"
#include "arkrtos_cfg.h"

#include <stdio.h>
#include <string.h>

/* 等待者容量：开关决定每个对象有多少个等待槽。这条 #error 是「派生宏真的
 * 落到了每个对象结构里」的编译期证据——如果某个头文件还在用旧的固定长度，
 * 下面的 sizeof 断言会在运行时把它抓出来。 */
#if (ARKRTOS_USE_WAIT_UNBOUNDED == 1)
#if (ARKRTOS_WQ_SLOTS != ARKRTOS_TASK_MAX_NUM)
#error "unbounded waiters: ARKRTOS_WQ_SLOTS must equal ARKRTOS_TASK_MAX_NUM"
#endif
#else
#if (ARKRTOS_WQ_SLOTS != ARKRTOS_MAX_SYNC_WAITERS)
#error "default waiters: ARKRTOS_WQ_SLOTS must equal ARKRTOS_MAX_SYNC_WAITERS"
#endif
#endif

#define WQ_SLOTS_OF(t, field)  ((int)(sizeof(((t *)0)->field) / sizeof(((t *)0)->field[0])))

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 0;
uint32 arkrtos_kernel_tick     = 0u;

/* 当前任务可以换：归属类判定需要"换一个人来试"。 */
static arkrtos_task_t *g_cur = 0;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }
void arkrtos_port_switch_task(void) { }
uint32 arkrtos_port_enter_critical(void) { return 0u; }
void arkrtos_port_exit_critical(uint32 state) { (void)state; }
/* 1 = 当前处于异常（SVC/IRQ）上下文。用户态分支（只登记 + WOULDBLOCK）
 * 只在这条路上走到，所以它必须是可编程的。 */
static uint8 g_host_in_isr = 0u;
uint8 arkrtos_port_in_isr(void) { return g_host_in_isr; }

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur; }

/* 阻塞桩：真正的实现放在测试脚手架之后（它要用 check / g_fail）。
 * 约定见下面「可编程阻塞桩」一节：事件队列为空时仍然返回 -1。 */
int32 arkrtos_task_block_in_critical(uint32 timeout_ms);

void arkrtos_ready_add(int32 task_idx) { (void)task_idx; }
void arkrtos_ready_reprio(int32 task_idx, uint8 old_prio) { (void)task_idx; (void)old_prio; }

/* trace 桩也是接口：先声明后定义。 */
void arkrtos_trace_wait_obj(uint16 obj13);
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls);
void mdk_trace_arkrtos_mutex_acquire(uint16 obj);
void mdk_trace_arkrtos_mutex_release(uint16 obj);
void arkrtos_trace_wait_obj(uint16 obj13) { (void)obj13; }
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls) { (void)obj; (void)cls; }
void mdk_trace_arkrtos_mutex_acquire(uint16 obj) { (void)obj; }
void mdk_trace_arkrtos_mutex_release(uint16 obj) { (void)obj; }

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

static void use_task(int idx, uint8 prio)
{
    arkrtos_task_table[idx].priority      = prio;
    arkrtos_task_table[idx].base_priority = prio;
    arkrtos_task_table[idx].status        = ARKRTOS_TASK_READY;
    g_cur = &arkrtos_task_table[idx];
    arkrtos_current_task_id = idx + 1;
}

/* ------------------------------------------------------------------
 * 可编程阻塞桩（2026-11-21）
 *
 * 宿主上没有调度器，「睡下去以后会发生什么」只能由测试来安排。旧桩恒返回 -1，
 * 于是入队 / 交接 / 超时 / 假醒 / 对象删除这几条链在宿主上一行都走不到。
 * 现在的约定是：
 *   调用 wait / lock / cond_wait 之前，把「睡下之后发生什么」按顺序压进事件队列；
 *   桩取出一个事件执行它，再把任务的 wake_reason 交回被测代码。
 *   队列为空时仍返回 -1（进不了阻塞），所以原有的断言一条都不用改。
 *
 * 桩模拟的是「睡下之后被别人动了一下」那一刻，不模拟时间流逝：它不推进节拍、
 * 不切任务，只按真实路径改对象表与任务状态（含 appq 标记与优先级继承），
 * 所以它证明的是状态机与账目，不是实时性。
 * ------------------------------------------------------------------ */
#define HOST_EV_QUEUE_LEN 8

enum {
    HOST_EV_NONE = 0,
    HOST_EV_TIMEOUT,             /* 等超时：wake_reason = ARKRTOS_WAKE_TIMEOUT */
    HOST_EV_SPURIOUS,            /* 假醒：wake_reason 保持 0，人还在队列里 */
    HOST_EV_BLOCK_REFUSED,       /* 进不了阻塞（-1）：内核上下文 / 调度器锁定 */
    HOST_EV_SEM_POST,
    HOST_EV_SEM_DELETE,
    HOST_EV_MTX_UNLOCK,          /* 由 owner_idx 指定的任务解锁（含真正交接） */
    HOST_EV_MTX_UNLOCK_AS_TIMEOUT, /* 同上，但把唤醒原因回填成「超时」（竞态分支） */
    HOST_EV_MTX_UNLOCK_AS_GHOST,   /* 同上，但唤醒原因保持 0（锁已易主却不是交接） */
    HOST_EV_MTX_DELETE,
    HOST_EV_COND_SIGNAL,
    HOST_EV_COND_BROADCAST,
    HOST_EV_COND_DELETE
};

typedef struct {
    uint8 kind;
    int32 handle;
    int32 owner_idx;
} host_ev_t;

static host_ev_t g_evq[HOST_EV_QUEUE_LEN];
static int      g_evq_n    = 0;
static int      g_evq_head = 0;
static int      g_block_calls = 0;

static void host_ev_push(uint8 kind, int32 handle, int32 owner_idx)
{
    if(g_evq_head >= g_evq_n)
    {
        g_evq_head = 0;
        g_evq_n    = 0;
    }
    if(g_evq_n >= HOST_EV_QUEUE_LEN)
    {
        g_checks++;
        g_fail++;
        fputs("  FAIL: host event queue overflow\n", stdout);
        return;
    }
    g_evq[g_evq_n].kind      = kind;
    g_evq[g_evq_n].handle    = handle;
    g_evq[g_evq_n].owner_idx = owner_idx;
    g_evq_n++;
}

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    host_ev_t ev;
    arkrtos_task_t *p = g_cur;

    g_block_calls++;
    if(p != 0)
    {
        /* 真实路径里这一句与入队同处一个临界区：先把状态摆成「睡着」。 */
        p->status      = ARKRTOS_TASK_WAIT;
        p->wait_time   = (int32)timeout_ms;
        p->wake_reason = 0u;    /* 「没人认领这次唤醒」必须是可区分的初值 */
    }
    if(g_evq_head >= g_evq_n)
    {
        return -1;              /* 没安排事件：与旧桩一致，进不了阻塞 */
    }
    ev = g_evq[g_evq_head++];
    switch(ev.kind)
    {
    case HOST_EV_TIMEOUT:
        return ARKRTOS_WAKE_TIMEOUT;
    case HOST_EV_SPURIOUS:
        return 0;
    case HOST_EV_BLOCK_REFUSED:
        return -1;
    case HOST_EV_SEM_POST:
        (void)arkrtos_sem_post_internal(ev.handle);
        break;
    case HOST_EV_SEM_DELETE:
        (void)arkrtos_sem_delete_internal(ev.handle);
        break;
    case HOST_EV_MTX_DELETE:
        (void)arkrtos_mtx_delete_internal(ev.handle);
        break;
    case HOST_EV_MTX_UNLOCK:
    case HOST_EV_MTX_UNLOCK_AS_TIMEOUT:
    case HOST_EV_MTX_UNLOCK_AS_GHOST:
        {
            /* 解锁必须由持有者发起：临时把当前任务换成它，再换回来。 */
            arkrtos_task_t *saved = g_cur;
            g_cur = &arkrtos_task_table[ev.owner_idx];
            (void)arkrtos_mtx_unlock_internal(ev.handle);
            g_cur = saved;
        }
        if(ev.kind == HOST_EV_MTX_UNLOCK_AS_TIMEOUT)
        {
            return ARKRTOS_WAKE_TIMEOUT;
        }
        if(ev.kind == HOST_EV_MTX_UNLOCK_AS_GHOST)
        {
            return 0;
        }
        break;
    case HOST_EV_COND_SIGNAL:
        (void)arkrtos_cond_signal_internal(ev.handle);
        break;
    case HOST_EV_COND_BROADCAST:
        (void)arkrtos_cond_broadcast_internal(ev.handle);
        break;
    case HOST_EV_COND_DELETE:
        (void)arkrtos_cond_delete_internal(ev.handle);
        break;
    default:
        return -1;
    }
    return (p != 0) ? (int32)p->wake_reason : -1;
}

#if defined(ARKRTOS_SYNC_HOST_TEST)
/* 宿主上直接摆出等待关系：阻塞路径要真调度器，宿主拿不到，
 * 但链式继承的判定只依赖「谁持有哪把锁、谁在等谁」这张图。 */
extern arkrtos_sem_obj_t arkrtos_sems[ARKRTOS_SEM_NUM];
extern arkrtos_cond_obj_t arkrtos_conds[ARKRTOS_COND_NUM];
extern arkrtos_mtx_obj_t arkrtos_mtxs[ARKRTOS_MTX_NUM];
void arkrtos_mtx_propagate(void);
void arkrtos_mtx_recalc_priority(int32 idx);
#endif

int main(void)
{
    int32 sem;
    int32 mtx;
    int32 cond;
    arkrtos_sync_dbg_row_t row;
    int32 i;

    for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
    {
        memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    use_task(0, 5);          /* 优先级 5 的持有者 */
    use_task(1, 3);          /* 优先级 3 的另一个任务 */

    /* ================= 信号量 ================= */
    use_task(0, 5);
    sem = arkrtos_sem_create_internal("s1", 2);
    check(sem >= 0, "sem create returns a handle");
    check(arkrtos_sync_sem_debug(sem & ARKRTOS_HANDLE_RELMASK, &row) == 0, "sem debug row readable");
    check(row.used == 1, "the new semaphore is marked used");
    check(row.count == 2, "the initial count is what the creator asked for");

    check(arkrtos_sem_wait_internal(sem, 0) == 0, "wait takes the first token");
    check(arkrtos_sem_wait_internal(sem, 0) == 0, "wait takes the second token");
    check(arkrtos_sync_sem_debug(sem & ARKRTOS_HANDLE_RELMASK, &row) == 0, "sem debug row still readable");
    check(row.count == 0, "count is 0 after both tokens are taken");

    check(arkrtos_sem_wait_internal(sem, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "0 timeout with no token reports TIMEOUT (not success)");
    check(arkrtos_sync_sem_debug(sem & ARKRTOS_HANDLE_RELMASK, &row) == 0, "debug row readable after timeout");
    check(row.count == 0, "a timed-out wait neither creates nor destroys a token");

    check(arkrtos_sem_post_internal(sem) == 0, "post succeeds with no waiter");
    check(arkrtos_sync_sem_debug(sem & ARKRTOS_HANDLE_RELMASK, &row) == 0, "debug row readable after post");
    check(row.count == 1, "post with no waiter parks the token in the count");
    check(arkrtos_sem_wait_internal(sem, 0) == 0, "the parked token can be taken");

    /* 非法句柄：类型标志 / 越界索引 */
    check(arkrtos_sem_wait_internal(sem + 0x00100000, 0) == -1,
          "a handle with the wrong type flag is rejected");
    check(arkrtos_sem_post_internal(sem + (int32)ARKRTOS_SEM_NUM) == -1,
          "an out-of-range index is rejected");

    /* 删除：之后一律拒绝，且重复删除也是拒绝 */
    check(arkrtos_sem_delete_internal(sem) == 0, "sem delete succeeds");
    check(arkrtos_sem_wait_internal(sem, 0) == -1, "wait on a deleted semaphore is rejected");
    check(arkrtos_sem_post_internal(sem) == -1, "post on a deleted semaphore is rejected");
    check(arkrtos_sem_delete_internal(sem) == -1, "deleting it twice is rejected");

    /* 初始计数为 0：立刻超时 */
    sem = arkrtos_sem_create_internal("s2", 0);
    check(sem >= 0, "sem create with count 0 succeeds");
    check(arkrtos_sem_wait_internal(sem, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "count 0 means the first wait times out");
    check(arkrtos_sem_post_internal(sem) == 0, "post makes exactly one token");
    check(arkrtos_sem_wait_internal(sem, 0) == 0, "that token is takeable once");
    check(arkrtos_sem_wait_internal(sem, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "and only once");
    (void)arkrtos_sem_delete_internal(sem);

    /* ================= 互斥锁 ================= */
    use_task(0, 5);
    mtx = arkrtos_mtx_create_internal("m1");
    check(mtx >= 0, "mtx create returns a handle");

    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "a free mutex locks immediately");
    check(arkrtos_sync_mtx_debug(mtx & ARKRTOS_HANDLE_RELMASK, &row) == 0, "mtx debug row readable");
    check(row.owner_id == 1, "the owner is recorded as task 1");

    check(arkrtos_mtx_lock_internal(mtx, 0) == -1,
          "re-locking by the owner is rejected (not recursive)");
    check(arkrtos_sync_mtx_debug(mtx & ARKRTOS_HANDLE_RELMASK, &row) == 0, "debug row readable after re-lock");
    check(row.owner_id == 1, "the rejected re-lock did not steal ownership");

    /* 换个人来：非持有者拿不到，也不能解锁 */
    use_task(1, 3);
    check(arkrtos_mtx_lock_internal(mtx, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "a held mutex reports TIMEOUT to another task");
    check(arkrtos_mtx_unlock_internal(mtx) == -1,
          "a non-owner cannot unlock");
    check(arkrtos_sync_mtx_debug(mtx & ARKRTOS_HANDLE_RELMASK, &row) == 0, "debug row readable after bad unlock");
    check(row.owner_id == 1, "the failed unlock left the owner alone");

    /* 持有者解锁：之后别人能拿到 */
    use_task(0, 5);
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "the owner unlocks successfully");
    check(arkrtos_sync_mtx_debug(mtx & ARKRTOS_HANDLE_RELMASK, &row) == 0, "debug row readable after unlock");
    check(row.owner_id == 0, "the mutex is free after unlock");
    use_task(1, 3);
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "another task can take the free mutex");

    /* 删除持有的锁：不计为仍然被持有；重复删除拒绝 */
    check(arkrtos_mtx_delete_internal(mtx) == 0, "deleting a held mutex succeeds");
    check(arkrtos_mtx_lock_internal(mtx, 0) == -1, "lock on a deleted mutex is rejected");
    check(arkrtos_mtx_unlock_internal(mtx) == -1, "unlock on a deleted mutex is rejected");
    check(arkrtos_mtx_delete_internal(mtx) == -1, "deleting it twice is rejected");

#if defined(ARKRTOS_SYNC_HOST_TEST)
    /* ============ 多级优先级继承（链式传播） ============
     * 图：A(3) 等 L1（B(5) 持有），B 又在等 L2（C(7) 持有）。
     * 只提升一层的话 C 仍停在 7，A 就还在等一个被 B 挡住的锁；
     * 正确结果是最低优先级 3 沿链传到 C。 */
    {
        int32 l1, l2;
        int32 i1, i2;

        use_task(0, 3);      /* A：最高优先级（数值最小） */
        use_task(1, 5);      /* B：中间 */
        use_task(2, 7);      /* C：最低 */

        l1 = arkrtos_mtx_create_internal("l1");
        l2 = arkrtos_mtx_create_internal("l2");
        check(l1 >= 0 && l2 >= 0, "both chain mutexes are created");
        i1 = l1 & ARKRTOS_HANDLE_RELMASK;
        i2 = l2 & ARKRTOS_HANDLE_RELMASK;

        arkrtos_mtxs[i1].owner         = &arkrtos_task_table[1];
        arkrtos_mtxs[i1].orig_priority = arkrtos_task_table[1].priority;
        arkrtos_mtxs[i2].owner         = &arkrtos_task_table[2];
        arkrtos_mtxs[i2].orig_priority = arkrtos_task_table[2].priority;
        arkrtos_mtxs[i1].waiters[0]    = &arkrtos_task_table[0];   /* A 等 L1 */
        arkrtos_mtxs[i2].waiters[0]    = &arkrtos_task_table[1];   /* B 等 L2 */

        arkrtos_mtx_recalc_priority(-1);

        check(arkrtos_task_table[1].priority == 3,
              "the direct owner inherits the highest waiter priority");
        check(arkrtos_task_table[2].priority == 3,
              "the second-level owner inherits too (chain, not one hop)");

        /* 拆掉等待关系：必须整体回到基准，不能滞留在被提升的优先级上 */
        arkrtos_mtxs[i1].waiters[0] = 0;
        arkrtos_mtxs[i2].waiters[0] = 0;
        arkrtos_mtx_recalc_priority(-1);
        check(arkrtos_task_table[1].priority == 5, "dropping the waiter restores B to its base priority");
        check(arkrtos_task_table[2].priority == 7, "and C too (no permanent boost)");

        arkrtos_mtxs[i1].owner = 0;
        arkrtos_mtxs[i2].owner = 0;
        (void)arkrtos_mtx_delete_internal(l1);
        (void)arkrtos_mtx_delete_internal(l2);
    }
#endif /* ARKRTOS_SYNC_HOST_TEST */

#if (ARKRTOS_USE_MTX_RECURSIVE == 1)
    /* ============ 递归互斥锁（ARKRTOS_USE_MTX_RECURSIVE=1） ============ */
    use_task(0, 5);
    mtx = arkrtos_mtx_create_recursive_internal("mr");
    check(mtx >= 0, "recursive mtx create returns a handle");
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "recursive: the first lock succeeds");
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "recursive: the same task locks again");
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "recursive: and a third time (nest = 3)");

    /* 递归不等于放开互斥：别人仍然拿不到，也不能替我解锁 */
    use_task(1, 3);
    check(arkrtos_mtx_lock_internal(mtx, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "a recursive mutex is still exclusive to its owner");
    check(arkrtos_mtx_unlock_internal(mtx) == -1,
          "a non-owner cannot unlock a recursive mutex");

    /* 退一层不等于释放：嵌套 3 层时解一次，锁仍归原持有者 */
    use_task(0, 5);
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "unlocking one level reports success");
    use_task(1, 3);
    check(arkrtos_mtx_lock_internal(mtx, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
          "one unlock out of three leaves the mutex held");
    use_task(0, 5);
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "the second unlock peels another level");
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "the third unlock really releases it");

    use_task(1, 3);
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "a freed recursive mutex is takeable by anyone");
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "and that task can release it");
    check(arkrtos_mtx_delete_internal(mtx) == 0, "the recursive mutex deletes cleanly");
    check(arkrtos_mtx_lock_internal(mtx, 0) == -1, "a deleted recursive mutex rejects lock");

    /* 关键的反假绿：开关打开也不能把普通锁悄悄变成递归锁 */
    use_task(0, 5);
    mtx = arkrtos_mtx_create_internal("m2");
    check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "a normal mutex still locks");
    check(arkrtos_mtx_lock_internal(mtx, 0) == ARKRTOS_SYNC_ERR_RECURSIVE,
          "a normal mutex stays non-recursive even with the feature compiled in");
    check(arkrtos_mtx_unlock_internal(mtx) == 0, "the normal mutex unlocks");
    (void)arkrtos_mtx_delete_internal(mtx);
#endif /* ARKRTOS_USE_MTX_RECURSIVE == 1 */

    /* ================= 等待者容量账目 ================= */
    /* 三种同步对象的等待槽位必须都等于同一个 ARKRTOS_WQ_SLOTS。漏掉一处
     * （例如结构里还是固定 4、循环却按槽位数走）在这种表驱动的内核里就是
     * 静默越界或永远填不满，宿主上量尺寸最便宜。 */
    check(WQ_SLOTS_OF(arkrtos_sem_obj_t, waiters) == (int)ARKRTOS_WQ_SLOTS,
          "semaphore waiter slots match ARKRTOS_WQ_SLOTS");
    check(WQ_SLOTS_OF(arkrtos_mtx_obj_t, waiters) == (int)ARKRTOS_WQ_SLOTS,
          "mutex waiter slots match ARKRTOS_WQ_SLOTS");
    check(WQ_SLOTS_OF(arkrtos_cond_obj_t, waiters) == (int)ARKRTOS_WQ_SLOTS,
          "cond waiter slots match ARKRTOS_WQ_SLOTS");
    printf("WQ-SLOTS=%d unbounded=%d\n", (int)ARKRTOS_WQ_SLOTS,
           (int)ARKRTOS_USE_WAIT_UNBOUNDED);

    /* ================= 条件变量 ================= */
    use_task(0, 5);
    cond = arkrtos_cond_create_internal("c1");
    check(cond >= 0, "cond create returns a handle");
    check(arkrtos_cond_signal_internal(cond) == ARKRTOS_SYNC_OK,
          "signal with nobody waiting is not an error (POSIX)");
    check(arkrtos_cond_broadcast_internal(cond) == ARKRTOS_SYNC_OK,
          "broadcast with nobody waiting is not an error either");
    check(arkrtos_cond_delete_internal(cond) == 0, "cond delete succeeds");
    check(arkrtos_cond_signal_internal(cond) == -1, "signal on a deleted cond is rejected");
    check(arkrtos_cond_delete_internal(cond) == -1, "deleting it twice is rejected");

    /* ============ 任务下线收尸（arkrtos_sync_release_task） ============ */
    /* 2026-11-17：-Wall -Wextra 在这份模块上抓到 p_tsk 声明了却从未赋值——
     * 三张表的等待队列都在拿一个野指针做比较。后果不是"报错"，而是静默：
     * 死任务永远留在别人的等待队列里（之后再被唤醒就是踩一个已经不存在的
     * 任务），它持有的锁也永远不会交出去。下面这几条就是钉住修好后的行为。 */
    {
        int32 mh = arkrtos_mtx_create_internal("mdead");
        int32 mi = mh & ARKRTOS_HANDLE_RELMASK;
        int32 sh = arkrtos_sem_create_internal("sdead", 0);
        int32 si = sh & ARKRTOS_HANDLE_RELMASK;
        arkrtos_sync_dbg_row_t d;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        use_task(1, 3);      /* 等着那把锁的任务：优先级高于死任务 */
        use_task(2, 6);      /* 要下线的任务：持锁，同时也排在信号量上 */
        use_task(3, 9);      /* 不相干的任务：它的登记必须活下来 */

        check((mh >= 0) && (sh >= 0), "both objects for the teardown case were created");

        arkrtos_mtxs[mi].owner         = &arkrtos_task_table[2];
        arkrtos_mtxs[mi].orig_priority = arkrtos_task_table[2].priority;
        arkrtos_mtxs[mi].waiters[0]    = &arkrtos_task_table[1];
        arkrtos_task_table[1].status   = ARKRTOS_TASK_WAIT;
        arkrtos_sems[si].waiters[0]    = &arkrtos_task_table[2];
        arkrtos_sems[si].waiters[1]    = &arkrtos_task_table[3];

        arkrtos_sync_release_task(3);   /* 下标 2 的任务 = 任务 id 3 */

        check(arkrtos_mtxs[mi].owner == &arkrtos_task_table[1],
              "a mutex held by a dying task is handed to the highest-priority waiter");
        check(arkrtos_task_table[1].status == ARKRTOS_TASK_READY,
              "the handover puts the receiver back on the ready list");
        check((arkrtos_task_table[1].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0,
              "the wake carries the HANDOFF reason, not a plain one");
        check(arkrtos_mtxs[mi].waiters[0] == 0,
              "the receiver is no longer queued on that mutex");
        check(arkrtos_sems[si].waiters[0] == 0,
              "the dying task is swept out of a semaphore queue it sat on");
        check(arkrtos_sems[si].waiters[1] == &arkrtos_task_table[3],
              "another task's registration on the same semaphore survives");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.nwait == 1) &&
              (d.waiter_id[0] == 0u) && (d.waiter_id[1] == 4u),
              "the debug view agrees: the dead slot is empty and task 4 is still queued");
        check((arkrtos_sync_mtx_debug(mi, &d) == 0) && (d.owner_id == 2),
              "the debug view reports the mutex owner as task 2");
    }

    /* ================= 只读台账（shell syncinfo 的后端） ================= */
    {
        uint32 bp, bt, bq2, bl, wp, wt, wq2, wl;
        int32 si;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        use_task(0, 5);
        sem = arkrtos_sem_create_internal("tgl", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        arkrtos_sync_sem_trace(si, &bp, &bt, &bq2, &bl);
        check(arkrtos_sem_post_internal(sem) == 0, "ledger: post succeeds");
        check(arkrtos_sem_wait_internal(sem, 0) == 0, "ledger: the token is taken back");
        arkrtos_sync_sem_trace(si, &wp, &wt, &wq2, &wl);
        check((wp == bp + 1u) && (wt == bt + 1u),
              "the per-object ledger counts exactly one post and one take");
        check((wq2 == bq2) && (wl == bl),
              "a fast-path take records no queue traffic");

        wp = 0xA5A5A5A5u;
        arkrtos_sync_sem_trace(-1, &wp, 0, 0, 0);
        arkrtos_sync_sem_trace(ARKRTOS_SEM_NUM, &wp, 0, 0, 0);
        check(wp == 0xA5A5A5A5u,
              "an out-of-range index writes nothing back into the caller's outputs");
        wp = 0xA5A5A5A5u;
        arkrtos_sync_sem_handoff(-1, &wp, 0, 0);
        arkrtos_sync_sem_handoff(ARKRTOS_SEM_NUM, &wp, 0, 0);
        check(wp == 0xA5A5A5A5u,
              "the handoff getter refuses an out-of-range index the same way");
        check((arkrtos_sync_sem_qbefore(-1) == 0u) &&
              (arkrtos_sync_sem_qbefore(ARKRTOS_SEM_NUM) == 0u),
              "qbefore answers 0 for a bad index instead of reading another object");
        check(arkrtos_sync_sem_num() == (uint32)ARKRTOS_SEM_NUM,
              "the semaphore count getter matches the config");
        check(arkrtos_sync_mtx_num() == (uint32)ARKRTOS_MTX_NUM,
              "the mutex count getter matches the config");
        check(arkrtos_sync_cond_num() == (uint32)ARKRTOS_COND_NUM,
              "the cond count getter matches the config");

        /* 调试视图对「非法下标 / 空输出指针」必须明确报错，不能返 0 加垃圾 */
        check(arkrtos_sync_sem_debug(-1, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "sem debug rejects a negative index");
        check(arkrtos_sync_sem_debug(ARKRTOS_SEM_NUM, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "sem debug rejects the first out-of-range index");
        check(arkrtos_sync_sem_debug(si, 0) == ARKRTOS_SYNC_ERR_PARAM,
              "sem debug rejects a NULL row");
        check(arkrtos_sync_mtx_debug(-1, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "mtx debug rejects a negative index");
        check(arkrtos_sync_mtx_debug(ARKRTOS_MTX_NUM, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "mtx debug rejects the first out-of-range index");
        check(arkrtos_sync_mtx_debug(0, 0) == ARKRTOS_SYNC_ERR_PARAM,
              "mtx debug rejects a NULL row");
        check(arkrtos_sync_cond_debug(-1, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "cond debug rejects a negative index");
        check(arkrtos_sync_cond_debug(ARKRTOS_COND_NUM, &row) == ARKRTOS_SYNC_ERR_PARAM,
              "cond debug rejects the first out-of-range index");
        check(arkrtos_sync_cond_debug(0, 0) == ARKRTOS_SYNC_ERR_PARAM,
              "cond debug rejects a NULL row");
        (void)arkrtos_sem_delete_internal(sem);
    }

    /* ================= 信号量：阻塞 / 交接 / 超时 / 假醒 / 删除 ================= */
    {
        arkrtos_sync_dbg_row_t d;
        uint32 g0, appq0, dup0, woke, reason, also;
        int32 si;
        int32 n;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        /* --- 睡下去以后被 post：令牌直接交给我，不进计数 --- */
        use_task(0, 3);
        sem = arkrtos_sem_create_internal("bs", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        host_ev_push(HOST_EV_SEM_POST, sem, 0);
        check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_OK,
              "a post during the block ends the wait with success");
        check(g_block_calls == 1, "the wait really went through block_in_critical");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 0) && (d.nwait == 0),
              "a handover on post leaves the count at 0 and the queue empty (one owner per token)");
        check(arkrtos_task_table[0].status == ARKRTOS_TASK_READY,
              "the woken task is back on the ready list");
        check((arkrtos_task_table[0].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0,
              "the wake carries the HANDOFF reason");
        check(arkrtos_task_table[0].wait_time == 0u,
              "a delivered wake clears the wait time");
        arkrtos_sync_sem_handoff(si, &woke, &reason, &also);
        check((woke == 1u) && ((reason & ARKRTOS_WAKE_HANDOFF) != 0),
              "the handoff ledger names the task that got the token");
        check(arkrtos_sync_sem_qbefore(si) == 1u,
              "the ledger recorded a queue depth of one just before the post");

        /* --- 超时：必须报超时并把自己摘掉 --- */
        use_task(0, 3);
        sem = arkrtos_sem_create_internal("bs2", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        host_ev_push(HOST_EV_TIMEOUT, 0, 0);
        check(arkrtos_sem_wait_internal(sem, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a timeout wake reports TIMEOUT, never a token");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 0) && (d.nwait == 0),
              "the timed-out waiter is taken out of the queue and no token is invented");
        check(arkrtos_sem_wait_internal(sem, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "the semaphore is still empty afterwards");

        /* --- 假醒：wake_reason 0 且自己还在队列里 → 幽灵计数 + 超时 --- */
        sem = arkrtos_sem_create_internal("bs3", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        g0 = arkrtos_diag_sync_ghost;
        host_ev_push(HOST_EV_SPURIOUS, 0, 0);
        check(arkrtos_sem_wait_internal(sem, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a wake nobody claimed is reported as TIMEOUT, not as a token");
        check(arkrtos_diag_sync_ghost == g0 + 1u,
              "and it is counted as exactly one ghost wake");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.nwait == 0),
              "the ghost wake also drops the stale queue entry");

        /* --- 进不了阻塞（内核上下文 / 调度器锁定） --- */
        host_ev_push(HOST_EV_BLOCK_REFUSED, 0, 0);
        check(arkrtos_sem_wait_internal(sem, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "refusing to block is an error, not a success");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.nwait == 0),
              "and it leaves no queue entry behind");

        /* --- 等待期间对象被删除 --- */
        sem = arkrtos_sem_create_internal("bs4", 0);
        host_ev_push(HOST_EV_SEM_DELETE, sem, 0);
        check(arkrtos_sem_wait_internal(sem, 50) == ARKRTOS_SYNC_ERR_DELETED,
              "deleting the object wakes the waiter with DELETED");
        check(arkrtos_sem_post_internal(sem) == ARKRTOS_SYNC_ERR_HANDLE,
              "after the delete the handle is dead");

        /* --- 中断侧 post 走同一条路 --- */
        use_task(0, 3);
        sem = arkrtos_sem_create_internal("isr", 0);
        check(arkrtos_sem_post_from_isr(sem) == 0,
              "post from an ISR goes through the same counting path");
        check(arkrtos_sem_wait_internal(sem, 0) == 0, "and the token is takeable");
        (void)arkrtos_sem_delete_internal(sem);

        /* --- 用户态等待者：只登记不阻塞，post 只加计数 --- */
        use_task(0, 3);
        sem = arkrtos_sem_create_internal("app", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        appq0 = dbg_sem_appq_hit;
        g_host_in_isr = 1u;
        g_block_calls = 0;
        check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode waiter only registers and reports WOULDBLOCK");
        g_host_in_isr = 0u;
        check(g_block_calls == 0, "and it never reaches block_in_critical");
        check(dbg_sem_appq_hit == appq0 + 1u, "the user-mode hit counter records it");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.nwait == 1),
              "the registration is visible in the debug view");
        check(arkrtos_sem_post_internal(sem) == 0, "post with only a user-mode waiter succeeds");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 1) && (d.nwait == 1),
              "post parks the token in the count instead of handing it to a task nobody will resume");
        check(arkrtos_sem_wait_internal(sem, 0) == 0,
              "the polling wrapper then takes it from the fast path");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 0) && (d.nwait == 0),
              "taking it clears both the count and the stale registration");
        (void)arkrtos_sem_delete_internal(sem);

        /* --- 队首不是我：即使计数为正也不许插队 --- */
        use_task(1, 9);
        sem = arkrtos_sem_create_internal("fair", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        g_host_in_isr = 1u;
        check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "the queue head registers as a user-mode waiter");
        g_host_in_isr = 0u;
        /* 现在把令牌放进去：post 看到用户态标记，只加计数、不动队列 */
        check(arkrtos_sem_post_internal(sem) == 0, "a post fills the count while the head keeps its slot");
        use_task(0, 3);
        check(arkrtos_sem_wait_internal(sem, 0) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "the fast path does not jump the queue even when the count is positive");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 1) && (d.nwait == 1),
              "and it leaves both the count and the head alone");
        use_task(1, 9);
        check(arkrtos_sem_wait_internal(sem, 0) == 0, "the queue head takes its own token");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.count == 0) && (d.nwait == 0),
              "and the object is empty again");
        (void)arkrtos_sem_delete_internal(sem);

        /* --- 等待队列满：多出来的等待者必须报错，不许静默丢弃 --- */
#if (ARKRTOS_USE_WAIT_UNBOUNDED != 1)
        use_task(0, 5);
        sem = arkrtos_sem_create_internal("full", 0);
        si = sem & ARKRTOS_HANDLE_RELMASK;
        g_host_in_isr = 1u;
        for(n = 0; n < (int)ARKRTOS_WQ_SLOTS; n++)
        {
            use_task(10 + n, (uint8)(20 + n));
            check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
                  "each user-mode task registers on the semaphore");
        }
        /* 同一个任务再登记一次：-2 视为「已登记」，不是错误 */
        use_task(10, 20);
        dup0 = arkrtos_diag_sync_dup;
        check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "registering twice is treated as already queued, not as a duplicate error");
        check(arkrtos_diag_sync_dup == dup0 + 1u, "and the duplicate counter records it");
        g_host_in_isr = 0u;
        use_task(30, 40);
        check(arkrtos_sem_wait_internal(sem, 100) == ARKRTOS_SYNC_ERR_NOSPACE,
              "a full waiter queue is reported to the caller instead of being silently dropped");
        check((arkrtos_sync_sem_debug(si, &d) == 0) && (d.nwait == (int32)ARKRTOS_WQ_SLOTS),
              "the queue really is full and holds exactly the registered tasks");
        (void)arkrtos_sem_delete_internal(sem);
        for(n = 0; n < (int)ARKRTOS_WQ_SLOTS; n++)
        {
            arkrtos_sync_release_task(11 + n);   /* 清掉 appq 标记，别让后面的用例看到假用户态 */
        }
#endif
    }

    /* ================= 互斥锁：阻塞 / 交接 / 超时 / 假醒 / 删除 ================= */
    {
        arkrtos_sync_dbg_row_t d;
        uint32 g0;
        int32 mi;
        int32 n;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        /* --- 等锁 → 持有者解锁 → 直接交接 --- */
        use_task(1, 5);
        mtx = arkrtos_mtx_create_internal("mb");
        mi = mtx & ARKRTOS_HANDLE_RELMASK;
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the owner takes the mutex first");
        use_task(0, 3);
        host_ev_push(HOST_EV_MTX_UNLOCK, mtx, 1);
        check(arkrtos_mtx_lock_internal(mtx, 100) == 0,
              "an unlock during the block ends the wait with the lock in hand");
        check((arkrtos_task_table[0].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0,
              "and the wake carries the HANDOFF reason");
        check((arkrtos_sync_mtx_debug(mi, &d) == 0) && (d.owner_id == 1) && (d.nwait == 0),
              "the waiter became the owner and left the queue");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "the new owner can release it");

        /* --- 超时：继承必须回滚 --- */
        use_task(1, 5);
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the owner takes it again");
        use_task(0, 3);
        host_ev_push(HOST_EV_TIMEOUT, 0, 0);
        check(arkrtos_mtx_lock_internal(mtx, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a timeout wake means the lock was not obtained");
        check(arkrtos_task_table[0].priority == 3,
              "the waiter keeps its own priority");
        check(arkrtos_task_table[1].priority == 5,
              "the owner's inherited priority is rolled back to its base priority");
        check((arkrtos_sync_mtx_debug(mi, &d) == 0) && (d.owner_id == 2) && (d.nwait == 0),
              "the lock is still owned by the other task and the queue is empty");

        /* --- 假醒：锁没易主 → 报超时，不计幽灵（幽灵只算「锁已归我却没交接」） --- */
        g0 = arkrtos_diag_sync_ghost;
        host_ev_push(HOST_EV_SPURIOUS, 0, 0);
        check(arkrtos_mtx_lock_internal(mtx, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a spurious wake without ownership is a TIMEOUT");
        check(arkrtos_diag_sync_ghost == g0,
              "and it is not counted as a ghost (the ghost is about ownership without handover)");

        /* --- 竞态：超时与交接同时到，锁已归我 → 按成功 --- */
        host_ev_push(HOST_EV_MTX_UNLOCK_AS_TIMEOUT, mtx, 1);
        check(arkrtos_mtx_lock_internal(mtx, 50) == ARKRTOS_SYNC_OK,
              "a handover that races the timeout is a success, not a leaked lock");
        check((arkrtos_sync_mtx_debug(mi, &d) == 0) && (d.owner_id == 1),
              "ownership really moved to the waiter");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "and it can be released");

        /* --- 假醒但锁已易主：算幽灵，且不许当成成功 --- */
        use_task(1, 5);
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the owner takes it for the ghost case");
        use_task(0, 3);
        g0 = arkrtos_diag_sync_ghost;
        host_ev_push(HOST_EV_MTX_UNLOCK_AS_GHOST, mtx, 1);
        check(arkrtos_mtx_lock_internal(mtx, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a wake that moved the lock but claims no handover is refused");
        check(arkrtos_diag_sync_ghost == g0 + 1u, "and it is counted as a ghost wake");
        check(arkrtos_mtxs[mi].owner == &arkrtos_task_table[0],
              "the unlock did move ownership: the refusal is about the wake reason, not the lock");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "the stray ownership is released here");

        /* --- 等待期间被删除 --- */
        use_task(1, 5);
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the owner takes it before the delete case");
        use_task(0, 3);
        host_ev_push(HOST_EV_MTX_DELETE, mtx, 0);
        check(arkrtos_mtx_lock_internal(mtx, 50) == ARKRTOS_SYNC_ERR_DELETED,
              "deleting the mutex wakes the waiter with DELETED");
        check(arkrtos_task_table[1].priority == 5,
              "the deleted mutex no longer boosts its former owner");
        check(arkrtos_mtx_lock_internal(mtx, 0) == ARKRTOS_SYNC_ERR_HANDLE,
              "and the handle is dead");

        /* --- 递归锁被阻塞后交接：接锁方一定是「第一次持有」 --- */
        use_task(1, 5);
        mtx = arkrtos_mtx_create_recursive_internal("mbr");
        mi = mtx & ARKRTOS_HANDLE_RELMASK;
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the recursive mutex is locked once");
        use_task(0, 3);
        host_ev_push(HOST_EV_MTX_UNLOCK, mtx, 1);
        check(arkrtos_mtx_lock_internal(mtx, 100) == ARKRTOS_SYNC_OK,
              "a recursive mutex can be handed over to a waiter");
        check(arkrtos_mtxs[mi].nest == 1u,
              "a handover is always the receiver's first hold (nest = 1)");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "and one unlock frees it");
        (void)arkrtos_mtx_delete_internal(mtx);

        /* --- 等待队列满：登记失败 + 继承回滚 --- */
#if (ARKRTOS_USE_WAIT_UNBOUNDED != 1)
        use_task(1, 30);   /* 持有者优先级更低，等待者才会触发继承 */
        mtx = arkrtos_mtx_create_internal("mfull");
        mi = mtx & ARKRTOS_HANDLE_RELMASK;
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the owner takes the queue-full mutex");
        g_host_in_isr = 1u;
        for(n = 0; n < (int)ARKRTOS_WQ_SLOTS; n++)
        {
            use_task(10 + n, (uint8)(20 + n));
            check(arkrtos_mtx_lock_internal(mtx, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
                  "each user-mode task registers on the mutex");
        }
        g_host_in_isr = 0u;
        check(arkrtos_task_table[1].priority == 20,
              "the owner inherited the highest waiter priority while the queue filled");
        use_task(30, 10);   /* 比所有已登记的等待者都高：失败时必须把自己的提权还回去 */
        check(arkrtos_mtx_lock_internal(mtx, 100) == ARKRTOS_SYNC_ERR_NOSPACE,
              "a full waiter queue is an error, not a silent no-op");
        check(arkrtos_task_table[1].priority == 20,
              "the failed registration rolled its own boost back (the real waiters still hold it at 20)");
        check((arkrtos_sync_mtx_debug(mi, &d) == 0) && (d.nwait == (int32)ARKRTOS_WQ_SLOTS),
              "the queue still holds exactly the registered tasks");
        for(n = 0; n < (int)ARKRTOS_WQ_SLOTS; n++)
        {
            arkrtos_sync_release_task(11 + n);
        }
        (void)arkrtos_mtx_delete_internal(mtx);
#endif
    }

    /* ================= 条件变量：等的全过程 ================= */
    {
        arkrtos_sync_dbg_row_t d;
        uint32 g0;
        int32 ci;
        int32 mi2;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        /* --- 参数闸门：非法句柄 / 没持锁 / timeout 0 / 异常上下文 / 无当前任务 --- */
        use_task(0, 5);
        cond = arkrtos_cond_create_internal("cw");
        mtx = arkrtos_mtx_create_internal("cwm");
        ci = cond & ARKRTOS_HANDLE_RELMASK;
        mi2 = mtx & ARKRTOS_HANDLE_RELMASK;
        check(arkrtos_cond_wait_internal(cond + 0x00100000, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "cond wait rejects a cond handle with the wrong type flag");
        check(arkrtos_cond_wait_internal(cond, mtx + 0x00100000, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "cond wait rejects a mutex handle with the wrong type flag");
        check(arkrtos_cond_wait_internal(cond + (int32)ARKRTOS_COND_NUM, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "cond wait rejects an out-of-range cond index");
        check(arkrtos_cond_wait_internal(cond, mtx + (int32)ARKRTOS_MTX_NUM, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "cond wait rejects an out-of-range mutex index");
        check(arkrtos_cond_wait_internal(cond, mtx, 0) == ARKRTOS_SYNC_ERR_PARAM,
              "timeout 0 is rejected: a condition wait cannot be a poll");
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "waiting without holding the mutex is rejected");
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the caller takes the mutex");
        g_host_in_isr = 1u;
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "a condition wait from a handler is rejected (no yield is possible there)");
        g_host_in_isr = 0u;
        g_cur = 0;
        arkrtos_current_task_id = 0;
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "cond wait without a current task is rejected");
        use_task(0, 5);

        /* --- 等待中被 signal：返回 0 且重新持锁 --- */
        host_ev_push(HOST_EV_COND_SIGNAL, cond, 0);
        check(arkrtos_cond_wait_internal(cond, mtx, 100) == ARKRTOS_SYNC_OK,
              "a signal during the wait ends it with success");
        check((arkrtos_sync_mtx_debug(mi2, &d) == 0) && (d.owner_id == 1),
              "the waiter holds the mutex again when cond_wait returns");
        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 0),
              "and it is no longer queued on the condition variable");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "the caller can release the mutex");

        /* --- 等待中超时：仍必须重新持锁（POSIX 契约） --- */
        check(arkrtos_mtx_lock_internal(mtx, 0) == 0, "the caller takes the mutex again");
        host_ev_push(HOST_EV_TIMEOUT, 0, 0);
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a cond timeout is reported as TIMEOUT");
        check((arkrtos_sync_mtx_debug(mi2, &d) == 0) && (d.owner_id == 1),
              "but the mutex is held again on the way out (the contract does not depend on the reason)");
        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 0),
              "and the queue entry is gone");

        /* --- 假醒：没人 signal 我 → 超时 + 幽灵 --- */
        g0 = arkrtos_diag_sync_ghost;
        host_ev_push(HOST_EV_SPURIOUS, 0, 0);
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a wake nobody signalled is reported as TIMEOUT");
        check(arkrtos_diag_sync_ghost == g0 + 1u, "and is counted as a ghost wake");
        check((arkrtos_sync_mtx_debug(mi2, &d) == 0) && (d.owner_id == 1),
              "the mutex is still held after that ghost wake");

        /* --- 进不了阻塞：报错但锁必须还给我 --- */
        host_ev_push(HOST_EV_BLOCK_REFUSED, 0, 0);
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_PARAM,
              "refusing to block is an error");
        check((arkrtos_sync_mtx_debug(mi2, &d) == 0) && (d.owner_id == 1),
              "and the mutex is re-acquired on that path too (no lock leak)");
        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 0),
              "the queue entry is withdrawn");

        /* --- 等待中条件变量被删除 --- */
        host_ev_push(HOST_EV_COND_DELETE, cond, 0);
        check(arkrtos_cond_wait_internal(cond, mtx, 50) == ARKRTOS_SYNC_ERR_DELETED,
              "deleting the cond while waiting ends the wait with DELETED");
        check((arkrtos_sync_mtx_debug(mi2, &d) == 0) && (d.owner_id == 1),
              "and the mutex is still held (a deleted cond does not change the lock contract)");
        check(arkrtos_cond_signal_internal(cond) == ARKRTOS_SYNC_ERR_HANDLE,
              "the cond handle is dead afterwards");
        check(arkrtos_mtx_unlock_internal(mtx) == 0, "the mutex is released at the end of this block");

        /* --- 递归锁：等待期间整份释放，醒来恢复嵌套深度 --- */
        {
            int32 rm;
            int32 c2;

            use_task(0, 5);
            rm = arkrtos_mtx_create_recursive_internal("crm");
            c2 = arkrtos_cond_create_internal("crmc");
            check((rm >= 0) && (c2 >= 0), "the recursive mutex and its cond are created");
            check(arkrtos_mtx_lock_internal(rm, 0) == 0, "recursive: first lock");
            check(arkrtos_mtx_lock_internal(rm, 0) == 0, "recursive: second lock");
            check(arkrtos_mtx_lock_internal(rm, 0) == 0, "recursive: third lock (nest = 3)");
            host_ev_push(HOST_EV_COND_SIGNAL, c2, 0);
            check(arkrtos_cond_wait_internal(c2, rm, 100) == ARKRTOS_SYNC_OK,
                  "a recursive cond wait succeeds");
            check(arkrtos_mtxs[rm & ARKRTOS_HANDLE_RELMASK].nest == 3u,
                  "the nesting depth is restored after the wait (the lock was released as a whole)");
            check(arkrtos_mtxs[rm & ARKRTOS_HANDLE_RELMASK].owner == &arkrtos_task_table[0],
                  "and the same task owns it again");
            (void)arkrtos_mtx_delete_internal(rm);
            (void)arkrtos_cond_delete_internal(c2);
        }

        /* --- 用户态三件套：enqueue / poll / abort --- */
        use_task(0, 5);
        cond = arkrtos_cond_create_internal("uc");
        ci = cond & ARKRTOS_HANDLE_RELMASK;
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_OK,
              "enqueue registers the user-mode waiter");
        check(arkrtos_cond_poll_internal(cond) == 0, "poll says the waiter is still queued");
        check(arkrtos_cond_abort_internal(cond) == ARKRTOS_SYNC_OK, "abort withdraws it");
        check(arkrtos_cond_poll_internal(cond) == 1, "poll now says it is not queued");
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_OK, "enqueue again");
        check(arkrtos_cond_signal_internal(cond) == ARKRTOS_SYNC_OK,
              "signal picks the queued waiter");
        check(arkrtos_cond_poll_internal(cond) == 1, "the signalled waiter is out of the queue");
        check((arkrtos_task_table[0].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0,
              "and it was readied with HANDOFF");

        /* --- 两个用户态等待者 + broadcast：两个都要出队 --- */
        use_task(11, 5);
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_OK, "task 12 enqueues");
        use_task(12, 7);
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_OK, "task 13 enqueues too");
        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 2),
              "both are visible in the debug view");
        check(arkrtos_cond_broadcast_internal(cond) == ARKRTOS_SYNC_OK, "broadcast wakes everyone");
        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 0),
              "the queue is empty afterwards");
        check(((arkrtos_task_table[11].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0) &&
              ((arkrtos_task_table[12].wake_reason & ARKRTOS_WAKE_HANDOFF) != 0),
              "both waiters were readied with HANDOFF");

        /* --- 三件套的参数闸门 --- */
        check(arkrtos_cond_enqueue_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_PARAM,
              "enqueue rejects a handle with the wrong type flag");
        check(arkrtos_cond_enqueue_internal(cond + (int32)ARKRTOS_COND_NUM) == ARKRTOS_SYNC_ERR_PARAM,
              "enqueue rejects an out-of-range index");
        check(arkrtos_cond_poll_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_PARAM,
              "poll rejects a handle with the wrong type flag");
        check(arkrtos_cond_poll_internal(cond + (int32)ARKRTOS_COND_NUM) == ARKRTOS_SYNC_ERR_PARAM,
              "poll rejects an out-of-range index");
        check(arkrtos_cond_abort_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_PARAM,
              "abort rejects a handle with the wrong type flag");
        check(arkrtos_cond_abort_internal(cond + (int32)ARKRTOS_COND_NUM) == ARKRTOS_SYNC_ERR_PARAM,
              "abort rejects an out-of-range index");
        g_cur = 0;
        arkrtos_current_task_id = 0;
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_ERR_PARAM,
              "enqueue without a current task is rejected");
        check(arkrtos_cond_poll_internal(cond) == ARKRTOS_SYNC_ERR_PARAM,
              "poll without a current task is rejected");
        check(arkrtos_cond_abort_internal(cond) == ARKRTOS_SYNC_ERR_PARAM,
              "abort without a current task is rejected");
        use_task(0, 5);
        (void)arkrtos_cond_delete_internal(cond);
    }

    /* ================= 任务下线：自己建的对象随它消失 ================= */
    {
        arkrtos_sync_dbg_row_t d;
        int32 sh;
        int32 mh;
        int32 ci;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        use_task(0, 5);
        use_task(3, 6);          /* 这个任务随后被拆掉（任务 id 4） */
        sh = arkrtos_sem_create_internal("own_s", 3);
        mh = arkrtos_mtx_create_internal("own_m");
        cond = arkrtos_cond_create_internal("own_c");   /* 条件变量不随创建者消失 */
        ci = cond & ARKRTOS_HANDLE_RELMASK;
        check((sh >= 0) && (mh >= 0) && (cond >= 0),
              "the doomed task creates a semaphore, a mutex and a cond");
        check((arkrtos_sync_sem_debug(sh & ARKRTOS_HANDLE_RELMASK, &d) == 0) && (d.used == 1),
              "the semaphore is live before the teardown");

        /* 死任务同时排在条件变量与自己的信号量上：两处都要被摘掉，别人留下 */
        use_task(1, 3);
        arkrtos_conds[ci].waiters[0] = &arkrtos_task_table[3];
        arkrtos_conds[ci].waiters[1] = &arkrtos_task_table[1];
        arkrtos_sems[sh & ARKRTOS_HANDLE_RELMASK].waiters[0] = &arkrtos_task_table[3];
        arkrtos_sync_release_task(4);

        check((arkrtos_sync_cond_debug(ci, &d) == 0) && (d.nwait == 1) && (d.waiter_id[1] == 2u),
              "the dead task is swept out of the cond queue and the other waiter survives");
        check(arkrtos_sem_post_internal(sh) == ARKRTOS_SYNC_ERR_HANDLE,
              "the semaphore it created is gone with it");
        check(arkrtos_mtx_lock_internal(mh, 0) == ARKRTOS_SYNC_ERR_HANDLE,
              "and so is the mutex");
        check(arkrtos_cond_signal_internal(cond) == ARKRTOS_SYNC_OK,
              "the cond it created survives (only sems and mutexes are owned objects)");
        (void)arkrtos_cond_delete_internal(cond);
    }

    /* ================= 第二批：其余错误闸门与清盘分支 ================= */
    {
        arkrtos_sync_dbg_row_t d;
        int32 sh;
        int32 mh;
        int32 k;

        for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
        {
            memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
        arkrtos_sync_module_init();
        g_evq_n = 0;
        g_evq_head = 0;
        g_block_calls = 0;
        g_host_in_isr = 0u;

        /* ---- 无当前任务：现成令牌照取，等不到才报 CONTEXT ---- */
        use_task(0, 5);
        sh = arkrtos_sem_create_internal("nc_s", 1);
        check(sh >= 0, "semaphore for the no-current-task cases");
        g_cur = 0;
        arkrtos_current_task_id = 0;
        check(arkrtos_sem_wait_internal(sh, 0) == ARKRTOS_SYNC_OK,
              "a parked token is taken even with no current task");
        check((arkrtos_sync_sem_debug(sh & ARKRTOS_HANDLE_RELMASK, &d) == 0) && (d.count == 0u),
              "and the count actually moved");
        check(arkrtos_sem_wait_internal(sh, 5) == ARKRTOS_SYNC_ERR_CONTEXT,
              "a blocking wait with no current task reports CONTEXT");
        check((arkrtos_sync_sem_debug(sh & ARKRTOS_HANDLE_RELMASK, &d) == 0) && (d.nwait == 0u),
              "the refused waiter is not left in the queue");
        use_task(0, 5);

        /* ---- 句柄闸门：错类型位 / 越界 ---- */
        check(arkrtos_sem_post_internal(sh + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "post rejects a handle with the wrong type flag");
        check(arkrtos_sem_delete_internal(sh + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "sem delete rejects a handle with the wrong type flag");
        check(arkrtos_sem_delete_internal(sh + (int32)ARKRTOS_SEM_NUM) == ARKRTOS_SYNC_ERR_HANDLE,
              "sem delete rejects an out-of-range index");
        (void)arkrtos_sem_delete_internal(sh);

        /* ---- 表满：三个对象表都要明确拒绝，不许静默复用 ---- */
        {
            int32 sems[ARKRTOS_SEM_NUM];
            int32 all = 1;

            for(k = 0; k < ARKRTOS_SEM_NUM; k++)
            {
                sems[k] = arkrtos_sem_create_internal("fill", 0);
                if(sems[k] < 0)
                {
                    all = 0;
                }
            }
            check(all == 1, "every semaphore slot can be filled");
            check(arkrtos_sem_create_internal("over", 0) == ARKRTOS_SYNC_ERR_NOSLOT,
                  "the next semaphore is refused with NOSLOT");
            for(k = 0; k < ARKRTOS_SEM_NUM; k++)
            {
                (void)arkrtos_sem_delete_internal(sems[k]);
            }
        }
        {
            int32 mtxs[ARKRTOS_MTX_NUM];
            int32 all = 1;

            for(k = 0; k < ARKRTOS_MTX_NUM; k++)
            {
                mtxs[k] = arkrtos_mtx_create_internal("mfill");
                if(mtxs[k] < 0)
                {
                    all = 0;
                }
            }
            check(all == 1, "every mutex slot can be filled");
            check(arkrtos_mtx_create_internal("mover") == ARKRTOS_SYNC_ERR_NOSLOT,
                  "the next mutex is refused with NOSLOT");
            check(arkrtos_mtx_create_recursive_internal("mover") == ARKRTOS_SYNC_ERR_NOSLOT,
                  "the recursive entry is refused too");
            for(k = 0; k < ARKRTOS_MTX_NUM; k++)
            {
                (void)arkrtos_mtx_delete_internal(mtxs[k]);
            }
        }
        {
            int32 conds[ARKRTOS_COND_NUM];
            int32 all = 1;

            for(k = 0; k < ARKRTOS_COND_NUM; k++)
            {
                conds[k] = arkrtos_cond_create_internal("cfill");
                if(conds[k] < 0)
                {
                    all = 0;
                }
            }
            check(all == 1, "every cond slot can be filled");
            check(arkrtos_cond_create_internal("cover") == ARKRTOS_SYNC_ERR_NOSLOT,
                  "the next cond is refused with NOSLOT");
            for(k = 0; k < ARKRTOS_COND_NUM; k++)
            {
                (void)arkrtos_cond_delete_internal(conds[k]);
            }
        }

        /* ---- 互斥锁的其余闸门 ---- */
        mh = arkrtos_mtx_create_internal("mg");
        check(mh >= 0, "mutex for the remaining gate cases");
        check(arkrtos_mtx_lock_internal(mh + 0x00100000, 0) == ARKRTOS_SYNC_ERR_HANDLE,
              "lock rejects a handle with the wrong type flag");
        check(arkrtos_mtx_unlock_internal(mh + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "unlock rejects a handle with the wrong type flag");
        check(arkrtos_mtx_delete_internal(mh + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "mtx delete rejects a handle with the wrong type flag");
        check(arkrtos_mtx_delete_internal(mh + (int32)ARKRTOS_MTX_NUM) == ARKRTOS_SYNC_ERR_HANDLE,
              "mtx delete rejects an out-of-range index");
        g_cur = 0;
        arkrtos_current_task_id = 0;
        check(arkrtos_mtx_lock_internal(mh, 0) == ARKRTOS_SYNC_ERR_CONTEXT,
              "lock with no current task reports CONTEXT");
        use_task(0, 5);
        (void)arkrtos_mtx_delete_internal(mh);

        /* ---- 条件变量的其余闸门 ---- */
        cond = arkrtos_cond_create_internal("cg");
        check(cond >= 0, "cond for the remaining gate cases");
        check(arkrtos_cond_signal_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "signal rejects a handle with the wrong type flag");
        check(arkrtos_cond_broadcast_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "broadcast rejects a handle with the wrong type flag");
        check(arkrtos_cond_broadcast_internal(cond + (int32)ARKRTOS_COND_NUM) == ARKRTOS_SYNC_ERR_HANDLE,
              "broadcast rejects an out-of-range index");
        check(arkrtos_cond_delete_internal(cond + 0x00100000) == ARKRTOS_SYNC_ERR_HANDLE,
              "cond delete rejects a handle with the wrong type flag");

        /* 登记队列满：用户态 enqueue 直接拒绝 */
        for(k = 0; k < ARKRTOS_WQ_SLOTS; k++)
        {
            arkrtos_task_table[30 + k].status = ARKRTOS_TASK_WAIT;
            arkrtos_conds[cond & ARKRTOS_HANDLE_RELMASK].waiters[k] = &arkrtos_task_table[30 + k];
        }
        use_task(9, 4);
        check(arkrtos_cond_enqueue_internal(cond) == ARKRTOS_SYNC_ERR_PARAM,
              "enqueue on a full cond queue reports PARAM");
        for(k = 0; k < ARKRTOS_WQ_SLOTS; k++)
        {
            arkrtos_conds[cond & ARKRTOS_HANDLE_RELMASK].waiters[k] = 0;
            arkrtos_task_table[30 + k].status = ARKRTOS_TASK_INVALID;
        }
        use_task(0, 5);
        check(arkrtos_cond_delete_internal(cond) == ARKRTOS_SYNC_OK,
              "the gate cond can be deleted");

        /* ---- 优先级继承：等待者更急时 propagate 当场抬持有者 ---- */
        mh = arkrtos_mtx_create_internal("pri");
        check(arkrtos_mtx_lock_internal(mh, 0) == ARKRTOS_SYNC_OK, "owner takes the mutex");
        arkrtos_task_table[30].priority      = 2;
        arkrtos_task_table[30].base_priority = 2;
        arkrtos_task_table[30].status        = ARKRTOS_TASK_WAIT;
        arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].waiters[0] = &arkrtos_task_table[30];
        arkrtos_mtx_propagate();
        check(arkrtos_task_table[0].priority == 2,
              "propagate lifts the owner to the waiter's priority");
        arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].waiters[0] = 0;
        arkrtos_task_table[30].status = ARKRTOS_TASK_INVALID;
        arkrtos_task_table[0].priority = 5;
        check(arkrtos_mtx_unlock_internal(mh) == ARKRTOS_SYNC_OK, "the owner then unlocks");
        (void)arkrtos_mtx_delete_internal(mh);

        /* ---- 条件变量：交接出去的锁拿不回来 ⇒ PARAM，锁留在被交接者手里 ---- */
        mh   = arkrtos_mtx_create_internal("cw");
        cond = arkrtos_cond_create_internal("cw");
        check((mh >= 0) && (cond >= 0), "mutex + cond for the handover case");
        check(arkrtos_mtx_lock_internal(mh, 0) == ARKRTOS_SYNC_OK, "the waiter holds the mutex");
        arkrtos_task_table[31].priority      = 3;
        arkrtos_task_table[31].base_priority = 3;
        arkrtos_task_table[31].status        = ARKRTOS_TASK_WAIT;
        arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].waiters[0] = &arkrtos_task_table[31];
        host_ev_push(HOST_EV_TIMEOUT, 0, 0);
        check(arkrtos_cond_wait_internal(cond, mh, 10) == ARKRTOS_SYNC_ERR_PARAM,
              "a cond wait that cannot re-take the lock reports PARAM");
        check(arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].owner == &arkrtos_task_table[31],
              "the re-take failure does not steal the lock back from the receiver");
        check(arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].waiters[0] == 0,
              "and the caller is not left in the mutex queue");
        {
            arkrtos_task_t *saved = g_cur;

            g_cur = &arkrtos_task_table[31];
            arkrtos_current_task_id = 32;
            check(arkrtos_mtx_unlock_internal(mh) == ARKRTOS_SYNC_OK,
                  "the receiver can drop the lock afterwards");
            g_cur = saved;
            arkrtos_current_task_id = 1;
        }
        check(arkrtos_cond_delete_internal(cond) == ARKRTOS_SYNC_OK,
              "the handover cond can be deleted");
        (void)arkrtos_mtx_delete_internal(mh);

        /* ---- 删除对象时，已在下线途中的等待者不许回到就绪队列 ---- */
        sh = arkrtos_sem_create_internal("ghostq", 0);
        use_task(9, 4);
        g_host_in_isr = 1u;
        check(arkrtos_sem_wait_internal(sh, 10) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode waiter only registers");
        g_host_in_isr = 0u;
        arkrtos_task_table[9].status = ARKRTOS_TASK_INVALID;
        check(arkrtos_sem_delete_internal(sh) == ARKRTOS_SYNC_OK,
              "deleting the semaphore succeeds with a waiter in teardown");
        check(arkrtos_task_table[9].status == ARKRTOS_TASK_INVALID,
              "the teardown task is not put back on the ready list");
        arkrtos_sync_release_task(10);
        use_task(0, 5);

        /* ---- 下线任务持有的锁：无人接手就直接清干净 ---- */
        use_task(1, 3);
        mh = arkrtos_mtx_create_internal("orphan");
        use_task(3, 6);
        check(arkrtos_mtx_lock_internal(mh, 0) == ARKRTOS_SYNC_OK,
              "the doomed task takes a mutex somebody else created");
        arkrtos_sync_release_task(4);
        check(arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].owner == 0,
              "the mutex it held is left unowned, not pointing at a dead task");
        check(arkrtos_mtxs[mh & ARKRTOS_HANDLE_RELMASK].nest == 0,
              "and the recursion depth is cleared with it");
        (void)arkrtos_mtx_delete_internal(mh);
        use_task(0, 5);
    }

    if(g_fail != 0)
    {
        printf("SYNC-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("SYNC-HOST OK (%d checks)\n", g_checks);
    return 0;
}
