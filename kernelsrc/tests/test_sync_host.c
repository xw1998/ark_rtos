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
*            未验证：阻塞与唤醒（等待者真正睡下、post/unlock 转交令牌、超时唤醒、
*                    优先级继承的完整链）需要真实调度器与节拍，测试里的
*                    block_in_critical 直接返回 -1，timeout>0 的分支走不到。
*                    那部分只有上板用例能证明。
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
uint8 arkrtos_port_in_isr(void) { return 0u; }

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur; }

/* 恒返回 -1（未能进入阻塞）：阻塞路径因此走不到。这不是覆盖了阻塞，是刻意不覆盖。 */
int32 arkrtos_task_block_in_critical(uint32 timeout_ms) { (void)timeout_ms; return -1; }

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

    if(g_fail != 0)
    {
        printf("SYNC-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("SYNC-HOST OK (%d checks)\n", g_checks);
    return 0;
}
