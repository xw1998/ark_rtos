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

    if(g_fail != 0)
    {
        printf("SYNC-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("SYNC-HOST OK (%d checks)\n", g_checks);
    return 0;
}
