/**
* @file arkrtos_smp_sched.c
* @brief 调度分核（AMP）实现：每核运行队列 + 显式迁移 + 次核切换
* @details 五件事，按调用者分：
*            - CPU0 任务侧：arkrtos_smp_sched_task_to / _task_back（迁移）；
*            - CPU0 节拍侧：arkrtos_smp_sched_poll（收回归还的任务）；
*            - 次核启动侧：arkrtos_smp_sched_enter（起本核节拍，等第一拍）；
*            - 次核异常侧：arkrtos_smp_sched_switch（保存帧、选下一个、返回帧）；
*            - CPU0 一致性侧：arkrtos_smp_sched_owned_by_other（自检跳过）。
*
*          **全局调度器一行不改**：本文件只碰两样共享东西——任务表里的
*          stack_ptr（迁出去的任务归次核独占，CPU0 不碰）与任务归属。
*          任务从次核回到 CPU0 走的是 arkrtos_ready_add()，与普通唤醒同一条路。
*
*          **锁**：每核一把自旋锁（arkrtos_spinlock_t），保护该核的运行队列
*          与两头登记（s_handoff / s_back）。次核在异常里持锁时 CPU0 若正好
*          在 poll，会自旋等一会儿——临界区只有几条数组操作，不关中断、
*          不调用任何可能阻塞的东西。
*
*          **为什么迁移只接受 READY**：RUNNING 任务要把帧让出来才能搬走，
*          这需要在源核的切换点插入逻辑，语义与验证量都远超一次交付；
*          WAIT/DELAY 任务的等待语义挂在全局结构上，搬走就是错的。宁可
*          明确返回 -3，也不做"搬过去看起来能跑"。
*
* @note 开关默认 0：关档下本文件是一个空翻译单元，不产生任何符号与代码。
*/

#include "arkrtos_smp_sched.h"

#if (ARKRTOS_USE_SMP_SCHED == 1)

#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_spin.h"

/* ============================================================
 * 每核状态
 * ============================================================ */

/* 本核当前运行的任务下标，NIL = 空闲 */
static int32  s_cur[ARKRTOS_CPU_NUM];

/* 本核空闲帧（第一次节拍把空闲上下文的帧存在这里，切回 idle 时用它） */
static uint32 s_idle_sp[ARKRTOS_CPU_NUM];

/* 本核时间片计数：>0 时同一任务可以继续跑 */
static uint32 s_slice[ARKRTOS_CPU_NUM];

/* 本核节拍计数（与全局 arkrtos_kernel_tick 是两回事） */
static uint32 s_tick[ARKRTOS_CPU_NUM];

/* 本核读数：切换次数 / 迁入 / 迁出 */
static uint32 s_switch_cnt[ARKRTOS_CPU_NUM];
static uint32 s_mig_in[ARKRTOS_CPU_NUM];
static uint32 s_mig_out[ARKRTOS_CPU_NUM];

/* 本核运行队列（数组当 FIFO，长度上限就是任务表大小） */
static uint8  s_q[ARKRTOS_CPU_NUM][ARKRTOS_TASK_MAX_NUM];
static uint8  s_qn[ARKRTOS_CPU_NUM];

/* 任务归属：0 = CPU0（默认），x = 归次核 x。
 * 不用 TCB 里的字段：TCB 是布局敏感结构，加一个字段会让默认关档的
 * 结构尺寸也变，与"关档逐位不变"的承诺冲突。 */
static uint8  s_owner[ARKRTOS_TASK_MAX_NUM];

/* 交接登记：CPU0 → 次核（要归还的任务下标），与 次核 → CPU0（已归还） */
static int32  s_handoff[ARKRTOS_CPU_NUM];
static int32  s_back[ARKRTOS_CPU_NUM];

/* 每核一把锁：保护本核队列与两头的登记 */
static arkrtos_spinlock_t s_lock[ARKRTOS_CPU_NUM];

static uint8 s_inited = 0u;
#if (ARKRTOS_USE_SMP_BALANCE == 1)
/* 候选表：任务下标（0 起）的定长数组，-1 = 空槽。 */
static int32  s_auto[ARKRTOS_SMP_BALANCE_MAX];
static uint32 s_auto_n = 0u;

/* 策略读数：自动迁出 / 自动要回 / 次核空闲但无可迁候选 / 本策略节拍数 */
static uint32 s_bal_out  = 0u;
static uint32 s_bal_back = 0u;
static uint32 s_bal_idle = 0u;
static uint32 s_bal_tick = 0u;
#endif


/* ============================================================
 * 内部工具（调用方必须已持本核锁）
 * ============================================================ */

static void sms_lazy_init(void)
{
    uint32 i;

    if(s_inited != 0u)
    {
        return;
    }

    for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        s_cur[i]        = ARKRTOS_SMP_SCHED_NIL;
        s_idle_sp[i]    = 0u;
        s_slice[i]      = (uint32)ARKRTOS_TIME_SLICE_TICKS;
        s_tick[i]       = 0u;
        s_switch_cnt[i] = 0u;
        s_mig_in[i]     = 0u;
        s_mig_out[i]    = 0u;
        s_qn[i]         = 0u;
        s_handoff[i]    = ARKRTOS_SMP_SCHED_NIL;
        s_back[i]       = ARKRTOS_SMP_SCHED_NIL;
        arkrtos_spin_init(&s_lock[i]);
    }
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_owner[i] = 0u;
    }
#if (ARKRTOS_USE_SMP_BALANCE == 1)
    for(i = 0u; i < (uint32)ARKRTOS_SMP_BALANCE_MAX; i++)
    {
        s_auto[i] = -1;
    }
    s_auto_n   = 0u;
    s_bal_out  = 0u;
    s_bal_back = 0u;
    s_bal_idle = 0u;
    s_bal_tick = 0u;
#endif
    s_inited = 1u;
}

static int32 sms_q_find(uint32 cpu, int32 task_idx)
{
    uint8 i;

    for(i = 0u; i < s_qn[cpu]; i++)
    {
        if((int32)s_q[cpu][i] == task_idx)
        {
            return (int32)i;
        }
    }
    return -1;
}

static void sms_q_remove_at(uint32 cpu, uint32 pos)
{
    uint32 i;

    for(i = pos; (i + 1u) < (uint32)s_qn[cpu]; i++)
    {
        s_q[cpu][i] = s_q[cpu][i + 1u];
    }
    if(s_qn[cpu] > 0u)
    {
        s_qn[cpu]--;
    }
}

static int32 sms_q_push(uint32 cpu, int32 task_idx)
{
    if((uint32)s_qn[cpu] >= (uint32)ARKRTOS_TASK_MAX_NUM)
    {
        return -1;
    }
    s_q[cpu][s_qn[cpu]] = (uint8)task_idx;
    s_qn[cpu]++;
    return 0;
}

/* 取本核队列里优先级最高的任务（数值最小者；同优先级取队首 = FIFO 轮转） */
static int32 sms_pick(uint32 cpu)
{
    uint8 i;
    uint8 best_prio = 255u;
    int32 best      = ARKRTOS_SMP_SCHED_NIL;

    for(i = 0u; i < s_qn[cpu]; i++)
    {
        int32 idx = (int32)s_q[cpu][i];

        if((idx < 0) || (idx >= arkrtos_task_count))
        {
            continue;                     /* 防御：坏下标不许进调度决策 */
        }
        if(arkrtos_task_table[idx].priority < best_prio)
        {
            best_prio = arkrtos_task_table[idx].priority;
            best      = idx;
        }
    }
    return best;
}

/* 把任务从本核队列摘掉（若在），并置回 CPU0 归属、登记归还 */
static void sms_release_locked(uint32 cpu, int32 idx)
{
    int32 pos = sms_q_find(cpu, idx);

    if(pos >= 0)
    {
        sms_q_remove_at(cpu, (uint32)pos);
    }
    s_owner[idx] = 0u;
    s_back[cpu]  = idx;                   /* 每核同时只登记一个归还者 */
    s_mig_out[cpu]++;
}

/* ============================================================
 * 次核启动
 * ============================================================ */

void arkrtos_smp_sched_enter(uint32 cpu)
{
    sms_lazy_init();

    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        /* 核号非法：什么都不调度，空转。与 arkrtos_smp_secondary_main 的
         * 越界处理同一取向——宁可停在那里，也不要替别人跑任务。 */
        for(;;)
        {
            arkrtos_port_wfi();
        }
    }

    s_cur[cpu]   = ARKRTOS_SMP_SCHED_NIL;
    s_slice[cpu] = (uint32)ARKRTOS_TIME_SLICE_TICKS;

    /* 本核节拍：TCFG/TVAL 与 ECFG.LIE[11] 都是每核的，互不影响。
     * 全局 arkrtos_kernel_tick 只由 CPU0 推进，这里起的是"次核自己的表"。 */
    arkrtos_port_start_timer((uint32)ARKRTOS_TICK_PERIOD_US);
    arkrtos_port_enable_irq();

    for(;;)
    {
        arkrtos_port_wfi();
    }
}

/* ============================================================
 * CPU0 侧：迁移与回收
 * ============================================================ */

int32 arkrtos_smp_sched_task_to(int32 task_id, uint32 cpu)
{
    int32 idx;
    int32 rc;

    sms_lazy_init();

    if(cpu == 0u)
    {
        return -2;                        /* 回 CPU0 走 _task_back */
    }
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return -1;
    }
    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }

    idx = task_id - 1;
    if(idx >= arkrtos_task_count)
    {
        return -1;
    }
    if(arkrtos_task_table[idx].status != ARKRTOS_TASK_READY)
    {
        return -3;
    }
    if(s_owner[idx] != 0u)
    {
        return -4;
    }
    if(idx == (arkrtos_current_task_id - 1))
    {
        /* 正在 CPU0 上跑的任务不能迁：本内核的约定是"运行中的任务仍挂在
         * 就绪链上"（阻塞类接口都是先 ready_del 自己），所以它同时满足
         * READY + 在就绪链 + 未迁出。真迁过去就是两核共用一份栈，必然
         * 毁栈。让调用方等它阻塞或让出后再来。 */
        return -6;
    }

    arkrtos_ready_del(idx);               /* 先摘 CPU0 就绪链，再交出归属 */
    arkrtos_delay_disarm(idx);            /* 幂等；READY 任务本来就不在延时链上 */

    arkrtos_spin_lock(&s_lock[cpu]);
    rc = sms_q_push(cpu, idx);
    if(rc == 0)
    {
        s_owner[idx] = (uint8)cpu;
        s_mig_in[cpu]++;
    }
    arkrtos_spin_unlock(&s_lock[cpu]);

    if(rc != 0)
    {
        /* 队列满：把任务挂回 CPU0 就绪链，不能让它凭空消失。 */
        arkrtos_ready_add(idx);
        return -5;
    }

    return 0;
}

int32 arkrtos_smp_sched_task_back(int32 task_id)
{
    int32 idx;
    uint32 cpu;

    sms_lazy_init();

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    idx = task_id - 1;
    if(idx >= arkrtos_task_count)
    {
        return -1;
    }

    cpu = (uint32)s_owner[idx];
    if(cpu == 0u)
    {
        return -4;
    }

    arkrtos_spin_lock(&s_lock[cpu]);
    s_handoff[cpu] = idx;                 /* 幂等：重复登记同一个任务没有副作用 */
    arkrtos_spin_unlock(&s_lock[cpu]);

    return 0;
}

#if (ARKRTOS_USE_SMP_BALANCE == 1)
/* ============================================================
 * 自动负载均衡（AMP 策略层）
 * @details 两条规则，都只在 CPU0 的节拍里做判断，都不改全局调度器：
 *          1) 某个次核空闲（队列空、且没在跑任务），而 CPU0 手上还有
 *             两个以上就绪任务时，把候选表里第一个 READY 候选迁过去。
 *             "两个以上"是有意的：迁完 CPU0 至少还留一个，不出现
 *             为了填次核把 CPU0 自己饿成 idle。
 *          2) CPU0 一件就绪任务都没有、而某个次核排了两个以上任务时，
 *             要回一个（迁回由 CPU0 每拍 poll 收走，语义与显式迁移
 *             完全相同）。
 *          两条都要求候选表登记：没登记的任务一概不动。
 *
 *          **这不是对称调度**：次核仍然不许发 SVC、不许调内核 API。
 *          自动迁移一个会发 SVC 的任务会让它当场致命停机，所以候选
 *          只能由调用方显式登记，本模块不猜。
 * ============================================================ */

static int32 sms_auto_find(int32 task_idx)
{
    uint32 i;

    for(i = 0u; i < (uint32)ARKRTOS_SMP_BALANCE_MAX; i++)
    {
        if(s_auto[i] == task_idx)
        {
            return (int32)i;
        }
    }
    return -1;
}

int32 arkrtos_smp_sched_auto_add(int32 task_id)
{
    uint32 i;
    int32  idx;

    sms_lazy_init();

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    idx = task_id - 1;
    if(idx >= arkrtos_task_count)
    {
        return -1;
    }
    if(sms_auto_find(idx) >= 0)
    {
        return -3;
    }

    for(i = 0u; i < (uint32)ARKRTOS_SMP_BALANCE_MAX; i++)
    {
        if(s_auto[i] < 0)
        {
            s_auto[i] = idx;
            s_auto_n++;
            return 0;
        }
    }
    return -2;
}

int32 arkrtos_smp_sched_auto_del(int32 task_id)
{
    uint32 i;
    int32  idx;

    sms_lazy_init();

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    idx = task_id - 1;
    for(i = 0u; i < (uint32)ARKRTOS_SMP_BALANCE_MAX; i++)
    {
        if(s_auto[i] == idx)
        {
            s_auto[i] = -1;
            s_auto_n--;
            return 0;
        }
    }
    return -4;
}

uint32 arkrtos_smp_sched_auto_count(void)
{
    return s_auto_n;
}

/**
* @brief 策略主体：在 CPU0 的节拍里跑一次
* @return 本次自动迁出的任务数
* @note 只在 CPU0 调用；它读 CPU0 的就绪集、改次核队列，两者都已有锁。
*/
static int32 sms_balance_step(void)
{
    uint32 cpu;
    int32  moved = 0;
    uint32 ready;

    s_bal_tick++;

    ready = arkrtos_sched_ready_count();

    for(cpu = 1u; cpu < (uint32)ARKRTOS_CPU_NUM; cpu++)
    {
        uint32 i;
        int32  target = -1;

        /* 规则 1：次核空闲 + CPU0 有两个以上就绪任务 => 迁一个过去 */
        if((s_qn[cpu] == 0u) && (s_cur[cpu] == ARKRTOS_SMP_SCHED_NIL) &&
           (ready >= 2u))
        {
            for(i = 0u; i < (uint32)ARKRTOS_SMP_BALANCE_MAX; i++)
            {
                int32 idx = s_auto[i];

                if(idx < 0)
                {
                    continue;
                }
                if((s_owner[idx] != 0u) ||
                   (idx == (arkrtos_current_task_id - 1)) ||
                   (arkrtos_task_table[idx].status != ARKRTOS_TASK_READY) ||
                   (arkrtos_ready_contains(idx) == 0u))
                {
                    continue;
                }
                target = idx;
                break;
            }

            if(target >= 0)
            {
                if(arkrtos_smp_sched_task_to(target + 1, cpu) == 0)
                {
                    s_bal_out++;
                    moved++;
                    ready--;
                }
            }
            else
            {
                s_bal_idle++;
            }
            continue;
        }

        /* 规则 2：CPU0 没有就绪任务、次核排了两个以上 => 要回一个 */
        if((ready == 0u) && (s_qn[cpu] >= 2u))
        {
            int32 pending = -1;

            for(i = (uint32)s_qn[cpu]; i > 0u; i--)
            {
                int32 idx = (int32)s_q[cpu][i - 1u];

                if((idx >= 0) && (sms_auto_find(idx) >= 0))
                {
                    pending = idx;
                    break;
                }
            }

            if((pending >= 0) &&
               (arkrtos_smp_sched_task_back(pending + 1) == 0))
            {
                s_bal_back++;
                ready++;
            }
        }
    }

    return moved;
}

int32 arkrtos_smp_sched_balance_stat(uint32 *out)
{
    if(out == 0)
    {
        return -1;
    }
    out[0] = s_bal_out;
    out[1] = s_bal_back;
    out[2] = s_bal_idle;
    out[3] = s_bal_tick;
    return 0;
}
#endif /* ARKRTOS_USE_SMP_BALANCE == 1 */

void arkrtos_smp_sched_poll(void)
{
    uint32 cpu;

    if(s_inited == 0u)
    {
        return;
    }

    for(cpu = 1u; cpu < (uint32)ARKRTOS_CPU_NUM; cpu++)
    {
        int32 idx;

        if(s_back[cpu] == ARKRTOS_SMP_SCHED_NIL)
        {
            continue;
        }

        arkrtos_spin_lock(&s_lock[cpu]);
        idx = s_back[cpu];
        s_back[cpu] = ARKRTOS_SMP_SCHED_NIL;
        if((idx >= 0) && (idx < (int32)ARKRTOS_TASK_MAX_NUM))
        {
            s_owner[idx] = 0u;
        }
        arkrtos_spin_unlock(&s_lock[cpu]);

        if((idx >= 0) && (idx < arkrtos_task_count) &&
           (arkrtos_task_table[idx].status == ARKRTOS_TASK_READY))
        {
            arkrtos_ready_add(idx);
        }
    }
#if (ARKRTOS_USE_SMP_BALANCE == 1)
    (void)sms_balance_step();
#endif
}

/* ============================================================
 * 次核侧：节拍与切换
 * ============================================================ */

void arkrtos_smp_sched_tick(uint32 cpu)
{
    if(cpu < (uint32)ARKRTOS_CPU_NUM)
    {
        s_tick[cpu]++;
    }
}

/* ---- 第 1 段：处理交还请求 --------------------------------------
 * s_handoff 是 CPU0 登记的"把这个任务还我"。任务可能正在跑（cur），
 * 也可能只是躺在队列里（还没轮到）。两种都要处理，但处理完不立刻
 * 决定谁上——统一走后面的选择逻辑。
 * *p_cur / *p_handed 反映这一步之后的状态。 */
static void sms_take_handoff(uint32 cpu, uint32 frame, int32 *p_cur, uint8 *p_handed)
{
    int32 cur = *p_cur;

    if(s_handoff[cpu] != ARKRTOS_SMP_SCHED_NIL)
    {
        int32 h = s_handoff[cpu];

        if((h >= 0) && (h < (int32)ARKRTOS_TASK_MAX_NUM))
        {
            if(h == cur)
            {
                /* 正在跑：先存帧，再交给 CPU0，本核回到空闲 */
                arkrtos_task_table[h].stack_ptr = frame;
                #if (ARKRTOS_USE_STACK_USAGE == 1)
                if(frame < arkrtos_task_table[h].stack_peak_low)
                {
                    arkrtos_task_table[h].stack_peak_low = frame;
                }
                #endif
                s_cur[cpu]     = ARKRTOS_SMP_SCHED_NIL;
                s_handoff[cpu] = ARKRTOS_SMP_SCHED_NIL;
                sms_release_locked(cpu, h);
                cur       = ARKRTOS_SMP_SCHED_NIL;
                *p_handed = 1u;
            }
            else if(sms_q_find(cpu, h) >= 0)
            {
                /* 还在队列里等：直接摘出来还给 CPU0 */
                s_handoff[cpu] = ARKRTOS_SMP_SCHED_NIL;
                sms_release_locked(cpu, h);
            }
            else
            {
                /* 既不在跑也不在本核队列：登记留着，下一拍再看（不吞请求） */
            }
        }
        else
        {
            s_handoff[cpu] = ARKRTOS_SMP_SCHED_NIL;   /* 坏下标：清掉 */
        }
    }

    *p_cur = cur;
}

/* ---- 第 2 段：保存当前上下文 ------------------------------------ */
static void sms_save_cur(uint32 cpu, int32 cur, uint32 frame, uint8 handed)
{
    if(handed != 0u)
    {
        return;      /* 交还那一路已经存过帧，这里不能再存 */
    }

    if(cur >= 0)
    {
        arkrtos_task_table[cur].stack_ptr = frame;
        #if (ARKRTOS_USE_STACK_USAGE == 1)
        if(frame < arkrtos_task_table[cur].stack_peak_low)
        {
            arkrtos_task_table[cur].stack_peak_low = frame;
        }
        #endif
        /* 被切下但仍可运行：放回队尾，保证同优先级轮转 */
        if(sms_q_find(cpu, cur) < 0)
        {
            (void)sms_q_push(cpu, cur);
        }
    }
    else
    {
        s_idle_sp[cpu] = frame;
    }
}

/* ---- 第 3 段：选择下一个 ----------------------------------------
 * *p_stay = 1 表示"本拍原地返回"（本函数已解锁，调用方不要再解锁）。
 * 三条短路都在这里返回 next 本身不动状态。 */
static int32 sms_pick_next(uint32 cpu, int32 cur, uint8 handed, uint8 *p_stay)
{
    int32 next = sms_pick(cpu);

    *p_stay = 0u;

    /* 空闲 -> 空闲：本核没有可跑的东西，帧已在第 2 步存进 s_idle_sp，
     * 直接原地返回。这一条不只是省一次切换：不返回的话会计一次假切换，
     * 让 s_switch_cnt 在纯空闲时每拍自增（读数看着像在疯狂切换）。
     * 只对 handed == 0 成立：交还那一路 cur 刚被置成 NIL，但 frame 属于
     * 被交还的任务，必须走后面的 else 分支取 s_idle_sp 回来。 */
    if((handed == 0u) && (next == ARKRTOS_SMP_SCHED_NIL) &&
       (cur == ARKRTOS_SMP_SCHED_NIL))
    {
        s_slice[cpu] = (uint32)ARKRTOS_TIME_SLICE_TICKS;
        arkrtos_spin_unlock(&s_lock[cpu]);
        *p_stay = 1u;
        return next;
    }

    if((next >= 0) && (next == cur))
    {
        /* 当前任务仍是最优：时间片未尽就不切；尽了就挪到队尾再选一次 */
        if(s_slice[cpu] > 0u)
        {
            s_slice[cpu]--;
            arkrtos_spin_unlock(&s_lock[cpu]);
            *p_stay = 1u;
            return next;
        }

        s_slice[cpu] = (uint32)ARKRTOS_TIME_SLICE_TICKS;
        {
            int32 pos = sms_q_find(cpu, cur);

            if(pos >= 0)
            {
                sms_q_remove_at(cpu, (uint32)pos);
                (void)sms_q_push(cpu, cur);
            }
        }
        next = sms_pick(cpu);
        if(next == cur)
        {
            arkrtos_spin_unlock(&s_lock[cpu]);
            *p_stay = 1u;
            return next;
        }
    }

    return next;
}

/* ---- 第 4 段：落地切换（含解锁） -------------------------------- */
static uint32 sms_apply_next(uint32 cpu, int32 next)
{
    uint32 ret;

    if(next >= 0)
    {
        int32 pos = sms_q_find(cpu, next);

        if(pos >= 0)
        {
            sms_q_remove_at(cpu, (uint32)pos);   /* 上 CPU 的从队列里摘掉 */
        }
        s_slice[cpu] = (uint32)ARKRTOS_TIME_SLICE_TICKS;
        s_switch_cnt[cpu]++;
        s_cur[cpu] = next;
        ret = arkrtos_task_table[next].stack_ptr;
    }
    else
    {
        s_switch_cnt[cpu]++;
        s_cur[cpu] = ARKRTOS_SMP_SCHED_NIL;
        ret = s_idle_sp[cpu];
    }

    arkrtos_spin_unlock(&s_lock[cpu]);

    return ret;
}

uint32 arkrtos_smp_sched_switch(uint32 cpu, uint32 frame)
{
    int32 cur;
    int32 next;
    uint32 ret;
    uint8 handed = 0u;
    uint8 stay = 0u;

    if(cpu == 0u || cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return frame;                     /* 核号非法：原样返回，不碰状态 */
    }

    sms_lazy_init();
    s_tick[cpu]++;

    arkrtos_spin_lock(&s_lock[cpu]);

    cur = s_cur[cpu];

    sms_take_handoff(cpu, frame, &cur, &handed);
    sms_save_cur(cpu, cur, frame, handed);

    next = sms_pick_next(cpu, cur, handed, &stay);
    if(stay != 0u)
    {
        return frame;                     /* 第 3 段已解锁并决定原地不动 */
    }

    ret = sms_apply_next(cpu, next);

    /* 防御：栈指针为 0 时 ertn 会跳到 0。宁可留在原地（同一帧）也不要
     * 让核跑飞——这是"宁可停住，也不给看似权威的错答案"的同一条原则。 */
    if(ret == 0u)
    {
        return frame;
    }
    return ret;
}

/* ============================================================
 * 查询
 * ============================================================ */

int32 arkrtos_smp_sched_cur(uint32 cpu)
{
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return ARKRTOS_SMP_SCHED_NIL;
    }
    return s_cur[cpu];
}

uint8 arkrtos_smp_sched_owned_by_other(int32 task_idx)
{
    if((task_idx < 0) || (task_idx >= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return 0u;
    }
    return (s_owner[task_idx] != 0u) ? 1u : 0u;
}

int32 arkrtos_smp_sched_selftest(uint32 *out)
{
    uint32 i;
    uint32 owned = 0u;
    int32  cur;

    if((out == 0) || ((uint32)ARKRTOS_CPU_NUM < 2u))
    {
        return -1;
    }

    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if(s_owner[i] != 0u)
        {
            owned++;
        }
    }

    cur = s_cur[1];
    out[0] = (cur < 0) ? 0xFFFFFFFFu : (uint32)cur;
    out[1] = (uint32)s_qn[1];
    out[2] = s_switch_cnt[1];
    out[3] = s_mig_in[1];
    out[4] = s_mig_out[1];
    out[5] = s_tick[1];
    out[6] = (s_back[1] != ARKRTOS_SMP_SCHED_NIL) ? 1u : 0u;
    out[7] = owned;
    return 0;
}

#endif /* ARKRTOS_USE_SMP_SCHED == 1 */
