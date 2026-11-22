/**
* @file arkrtos_shell_diag.c
* @brief 诊断：task / stack / sched / audit / guard / crash / fault /
*          log / syncinfo / rt / sec / utest。多读少写，命令本身就是取证。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

/* ============================================================
 * task：任务表概览
 * ============================================================ */
int cmd_task(int argc, char *argv[])
{
    int32 i;

    if(argc >= 2)
    {
        /* One suspended task, its saved context frame, and nothing else.
         * A blocked task is read-only here: the frame holds the return
         * address of the call that never came back, which is the only
         * honest answer to "where is it stuck". */
        /* Task ids are 1-based wherever the kernel names a task:
         * arkrtos_task_register(), the partition table's task column, the
         * guard table and the fault records all use 1..N, with 0 meaning
         * "no task".  The task table itself is 0-based, so the index is
         * id - 1.  Printing the raw index here is how one task used to
         * look like two different numbers depending on the command. */
        uint32 id = 0u;
        uint32 idx;
        volatile uint32 *sp;
        int32 n;

        if((parse_u32(argv[1], &id) != 0) ||
           (id == 0u) ||
           (id > (uint32)arkrtos_task_count) ||
           (arkrtos_task_table[id - 1u].stack_ptr == 0u))
        {
            ark_shell_printf("task: no such task (ids run 1..%d)\r\n",
                             (int)arkrtos_task_count);
            return 1;
        }
        idx = id - 1u;
        sp = (volatile uint32 *)arkrtos_task_table[idx].stack_ptr;
        ark_shell_printf("task %u sp=0x%08X st=%s frame words:\r\n",
                         id, (uint32)arkrtos_task_table[idx].stack_ptr,
                         task_state_name((uint32)arkrtos_task_table[idx].status));
        for(n = 0; n < 56; n++)
        {
            /* Raw, unfiltered: the frame layout depends on whether the task
             * owns a floating point extended frame, and guessing which word
             * is the PC is how a backtrace turns into a wrong answer. */
            ark_shell_printf("  sp+%-3u  0x%08X\r\n", (uint32)(n * 4), sp[n]);
        }
        if(argc >= 3)
        {
            /* Wake reason this task actually read on its last blocking
             * call, and how many times it blocked at all.  4 = HANDOFF
             * observed, 0 = a wake that carried no token claim. */
            extern volatile uint8 arkrtos_dbg_wake_last[];
            extern volatile uint8 arkrtos_dbg_wake_hits[];

            ark_shell_printf("  blocks=%u last_reason=%u\r\n",
                             (unsigned int)arkrtos_dbg_wake_hits[idx],
                             (unsigned int)arkrtos_dbg_wake_last[idx]);
        }
        return 0;
    }

#if ARKRTOS_USE_STACK_USAGE
    /* With stack accounting on, the column is a real measurement. */
    #define SHELL_TASK_PEAK_FMT  "%-8u"
    #define SHELL_TASK_PEAK_ARG  , arkrtos_task_table[i].stack_peak_low
#else
    /* Accounting compiled out: print "n/a", never a 0.  A zero in this
     * column reads as "this task used no stack", which is a claim the build
     * cannot make. */
    #define SHELL_TASK_PEAK_FMT  "%-8s"
    #define SHELL_TASK_PEAK_ARG  , "n/a"
#endif
    ark_shell_printf("\r\nid  prio  state     period  wait    peak/low  entry\r\n");
    for(i = 0; i < arkrtos_task_count; i++)
    {
        ark_shell_printf(" %-3d %-5u %-8s  %-6d  %-6d  " SHELL_TASK_PEAK_FMT
                         "  0x%08X\r\n",
                         (int)(i + 1),
                         (uint32)arkrtos_task_table[i].priority,
                         task_state_name((uint32)arkrtos_task_table[i].status),
                         arkrtos_task_table[i].period,
                         arkrtos_task_table[i].wait_time
                         SHELL_TASK_PEAK_ARG,
                         arkrtos_task_table[i].entry);
#undef SHELL_TASK_PEAK_FMT
#undef SHELL_TASK_PEAK_ARG
    }
    ark_shell_printf("tasks: %d used / %u max  (ids 1..%d)\r\n",
                     (int)arkrtos_task_count, (uint32)ARKRTOS_TASK_MAX_NUM,
                     (int)arkrtos_task_count);
    return 0;
}

/* ============================================================
 * stack：每个任务的栈用量（峰值法，真读数）
 *
 * 任务建立时整段栈被填成 0xcdcdcdcd，运行中写过的字就覆盖了图案；从栈底
 * 向上扫到第一个非图案字为止，"从未用过"的部分就出来了。再与上下文切换
 * 记录的最低 PSP 取较大值，两者的较大者就是峰值用量。
 *
 * 为什么要有这条命令：栈够不够只有量出来才叫够。APP_TASK_STACK_SIZE 是
 * 4 KB、控制台栈曾经只是"给到不崩"——这类数字靠猜，动一个局部数组就可能
 * 越界，而 Cortex-M 上越界不报错，只会安静地改掉别人的数据。
 *
 * 两种失败都必须是可见的，且都不许冒充 0：
 *   - 统计没编进来（ARKRTOS_USE_STACK_USAGE=0）：明说 n/a，不打印 0；
 *   - 统计编进来了，但某个任务的读数是 0：说明填充图案没生效（或统计被
 *     绕过），标 unmeasured。"用了 0 字节栈"这个断言不成立——任何跑过
 *     的任务都至少写过初始栈帧。
 *
 * stack          逐任务列出 total / used / free / used%
 * stack check    全部任务都测得到、且没有一个到危险线时返回 0
 * stack raw      把两个读数来源分开报（扫描 / 最低 PSP），诊断用
 * ============================================================ */
#define SHELL_STACK_WARN_PCT    75u   /* 已用 >= 75%：打印提醒 */
#define SHELL_STACK_DANGER_PCT  90u   /* 已用 >= 90%：判失败 */

#if ARKRTOS_USE_STACK_USAGE
int cmd_stack(int argc, char *argv[])
{
    int32  i;
    int    check = 0;
    int    raw = 0;
    uint32 unmeasured = 0u;
    uint32 danger = 0u;
    uint32 worst_pct = 0u;
    int32  worst_id = 0;

    if(argc >= 2)
    {
        if((argv[1][0] == 'c') && (argv[1][1] == 'h') && (argv[1][2] == 'e') &&
           (argv[1][3] == 'c') && (argv[1][4] == 'k') && (argv[1][5] == '\0'))
        {
            check = 1;
        }
        else if((argv[1][0] == 'r') && (argv[1][1] == 'a') &&
                (argv[1][2] == 'w') && (argv[1][3] == '\0'))
        {
            raw = 1;
        }
        else
        {
            ark_shell_printf("stack: unknown option '%s' "
                             "(usage: stack [check|raw])\r\n", argv[1]);
            return 1;
        }
    }

    if(raw != 0)
    {
        /* 合成值取两路较大者；两路各自打出来，才能分清是「栈真用满」还是
         * 「某一路读数本身有错」。这两件事的处置完全不同。 */
        ark_shell_printf("\r\nid  total    used     fill     psp      "
                         "pct_f  pct_p  entry\r\n");
        for(i = 0; i < arkrtos_task_count; i++)
        {
            uint32 out4[4];
            uint32 pf;
            uint32 pp;

            if(arkrtos_task_stack_info_raw(i + 1, out4) != 0)
            {
                ark_shell_printf(" %-3d n/a (no stack)\r\n", (int)(i + 1));
                continue;
            }
            pf = (out4[0] != 0u) ? ((out4[2] * 100u) / out4[0]) : 0u;
            pp = (out4[0] != 0u) ? ((out4[3] * 100u) / out4[0]) : 0u;
            ark_shell_printf(" %-3d %-8u %-8u %-8u %-8u %3u%%   %3u%%   0x%08X\r\n",
                             (int)(i + 1), (unsigned)out4[0], (unsigned)out4[1],
                             (unsigned)out4[2], (unsigned)out4[3],
                             (unsigned)pf, (unsigned)pp,
                             (unsigned)arkrtos_task_table[i].entry);
            /* 采样：底部/中间/上部各取一个字。扫描说“用过”与内存里到底
             * 写着什么必须能对照，否则分不清是栈真用过还是扫描走错地址。 */
            if(out4[0] >= 4096u)
            {
                const uint32 *pb = arkrtos_task_table[i].stack_bottom;
                uint32 half = out4[0] / 8u;

                ark_shell_printf("      bottom 0x%08X  [1/8]=0x%08X [4/8]=0x%08X "
                                 "[7/8]=0x%08X\r\n",
                                 (unsigned)(uintptr_t)pb,
                                 (unsigned)pb[half / 4u],
                                 (unsigned)pb[(out4[0] / 2u) / 4u],
                                 (unsigned)pb[(out4[0] * 7u / 8u) / 4u]);
            }
        }
        ark_shell_printf("stack raw: fill=scan of untouched fill pattern, "
                         "psp=lowest SP seen (take the larger)\r\n");
        return 0;
    }

    ark_shell_printf("\r\nid  total    used     free     pct    entry\r\n");
    for(i = 0; i < arkrtos_task_count; i++)
    {
        uint32 out3[3];
        uint32 pct;

        if((arkrtos_task_stack_info_internal(i + 1, out3) != 0) || (out3[0] == 0u))
        {
            ark_shell_printf(" %-3d %-8s %-8s %-8s %-6s 0x%08X\r\n",
                             (int)(i + 1), "n/a", "n/a", "n/a", "n/a",
                             (unsigned)arkrtos_task_table[i].entry);
            unmeasured++;
            continue;
        }
        /* 0 字节不可能：填充图案没生效才会读出 0。这是"开关在位、功能没
         * 生效"的唯一可检测形态，所以单独标出来而不是当成最省栈的任务。 */
        if(out3[1] == 0u)
        {
            ark_shell_printf(" %-3d %-8u %-8s %-8u %-6s 0x%08X\r\n",
                             (int)(i + 1), (unsigned)out3[0], "0?", (unsigned)out3[2],
                             "unmeas", (unsigned)arkrtos_task_table[i].entry);
            unmeasured++;
            continue;
        }

        pct = (out3[1] * 100u) / out3[0];
        ark_shell_printf(" %-3d %-8u %-8u %-8u %3u%%   0x%08X\r\n",
                         (int)(i + 1), (unsigned)out3[0], (unsigned)out3[1],
                         (unsigned)out3[2], (unsigned)pct,
                         (unsigned)arkrtos_task_table[i].entry);

        if((worst_id == 0) || (pct > worst_pct))
        {
            worst_pct = pct;
            worst_id  = i + 1;
        }
        if(pct >= SHELL_STACK_DANGER_PCT)
        {
            ark_shell_printf("      ^ task %d is at %u%% - raise its stack size\r\n",
                             (int)(i + 1), (unsigned)pct);
            danger++;
        }
        else if(pct >= SHELL_STACK_WARN_PCT)
        {
            ark_shell_printf("      ^ task %d is at %u%% - watch it\r\n",
                             (int)(i + 1), (unsigned)pct);
        }
    }

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 启动/idle 上下文：不是任务槽位，单独一行。水位口径是「布防时整段填
     * 图案、之后最低的非图案字」，所以 used 是一个真读数，不是估算。 */
    {
        uint32 ctx[4];

        if(arkrtos_stack_guard_context_info(ctx) == 0)
        {
            ark_shell_printf("stack ctx: idle/boot 0x%x..0x%x low=0x%x "
                             "used=%u free=%u\r\n",
                             (unsigned)ctx[0], (unsigned)ctx[1],
                             (unsigned)ctx[2],
                             (unsigned)(ctx[1] - ctx[2]),
                             (unsigned)ctx[3]);
        }
        else
        {
            /* 布防被拒时不留一行空输出：拿不到读数本身是个事实，要说出来。 */
            ark_shell_printf("stack ctx: not armed (watch_context refused: "
                             "alignment/size/top/bottom)\r\n");
        }
    }
    #endif
    ark_shell_printf("stack: %d task(s), worst %u%% (task %d), "
                     "%u unmeasured, %u at/over %u%%  (accounting on)\r\n",
                     (int)arkrtos_task_count, (unsigned)worst_pct, (int)worst_id,
                     (unsigned)unmeasured, (unsigned)danger,
                     (unsigned)SHELL_STACK_DANGER_PCT);

    if(check != 0)
    {
        /* 判词写成一行文本：串口上要能当闸禁，且打印读数本身。 */
        if(unmeasured != 0u)
        {
            ark_shell_printf("stack: check FAILED - %u task(s) could not be "
                             "measured (fill pattern not in effect?)\r\n",
                             (unsigned)unmeasured);
            return 1;
        }
        if(danger != 0u)
        {
            ark_shell_printf("stack: check FAILED - %u task(s) at/over %u%% "
                             "of their stack\r\n",
                             (unsigned)danger, (unsigned)SHELL_STACK_DANGER_PCT);
            return 1;
        }
        ark_shell_printf("stack: check ok - %d task(s) measured, worst %u%% "
                         "(task %d), none at/over %u%%\r\n",
                         (int)arkrtos_task_count, (unsigned)worst_pct,
                         (int)worst_id, (unsigned)SHELL_STACK_DANGER_PCT);
        return 0;
    }
    return 0;
}
#else
static int cmd_stack(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* 没有统计就不要给数字：这一版把 ARKRTOS_USE_STACK_USAGE 编掉了，
     * 任何字节数都只能是编的。 */
    ark_shell_printf("stack: accounting is compiled out "
                     "(ARKRTOS_USE_STACK_USAGE=0) - no numbers to show\r\n");
    return 1;
}
#endif /* ARKRTOS_USE_STACK_USAGE */

/* ============================================================
 * sched：调度器就绪队列自检
 *
 * 内核的就绪集现在是「256 位两级位图 + 每优先级双向循环链表」，
 * 选任务 O(1)。复杂度换来的风险是结构性的：一旦链表/位图与任务表
 * 真实状态不一致，调度器会静默选错任务——不报错、不崩溃。
 * 所以这里保留一条全表扫描的慢路径专门用来对账：
 *   sched         打印就绪队列概况 + 对账结果
 * 一致返回 0，不一致返回不一致条目数（可直接用于脚本判据）。
 * ============================================================ */
/* ============================================================
 * audit: does the shared state still satisfy its own invariants?
 * Read-only, so it is safe to run while the system keeps working.
 * ============================================================ */
#if ARKRTOS_USE_KERNEL_GUARD
/* ============================================================
 * guard: watchdog state and the per-slot heartbeat contract
 * ============================================================ */
static const char *guard_state_name(uint32 state)
{
    switch(state)
    {
    case ARKRTOS_TASK_INVALID: return "invalid";
    case ARKRTOS_TASK_READY:   return "ready";
    case ARKRTOS_TASK_WAIT:    return "wait";
    case ARKRTOS_TASK_RUNNING: return "run";
    default:                 return "-";
    }
}

static void guard_print_age(uint32 age_ms)
{
    if(age_ms == 0xFFFFFFFFu)
    {
        ark_shell_printf("-");
        return;
    }
    ark_shell_printf("%u", age_ms);
}

int cmd_guard(int argc, char *argv[])
{
    arkrtos_guard_status_t st;
    arkrtos_guard_task_t t;
    uint32 i;

    (void)argc;
    (void)argv;

    arkrtos_guard_status(&st);

    if(st.enabled == 0u)
    {
        sh_out("watchdog: disabled by the board (ARKRTOS_WDG_ENABLE = 0)\r\n");
    }
    else if(st.timeout_ms == 0u)
    {
        sh_out("watchdog: requested but NOT running (the port refused it)\r\n");
    }
    else
    {
        ark_shell_printf("watchdog: armed, hardware timeout %u ms\r\n", st.timeout_ms);
    }
    ark_shell_printf("feeds=%u starves=%u verdict=0x%X audit_mask=0x%X audit_runs=%u\r\n",
                     st.feeds, st.starves, st.verdict, st.audit_mask, st.audit_runs);
    ark_shell_printf("contracts: %u declared, %u violating now\r\n",
                     st.declared, st.violating);
    ark_shell_printf("\r\ntask slot period beat_ms svc_ms cpu_ms misses state\r\n");

    for(i = 0u; i < arkrtos_guard_task_slots(); i++)
    {
        if(arkrtos_guard_task_at(i, &t) != 0)
        {
            break;
        }
        if((t.task_id == 0) && (t.declared == 0u))
        {
            continue;
        }
        ark_shell_printf("%u %d %u ", (uint32)t.task_id, (int)t.slot, t.period_ms);
        guard_print_age(t.beat_age_ms);
        ark_shell_printf(" ");
        guard_print_age(t.svc_age_ms);
        ark_shell_printf(" ");
        guard_print_age(t.cpu_age_ms);
        ark_shell_printf(" %u %s\r\n", t.misses, guard_state_name(t.state));
    }

    if(st.declared == 0u)
    {
        sh_out("\r\nnote: no image declared a heartbeat period, so the kernel has\r\n"
               "      no health verdict to give for any slot (see arkrtos_heartbeat).\r\n");
    }

    return 0;
}

#endif /* ARKRTOS_USE_KERNEL_GUARD */
#if ARKRTOS_USE_CRASH_LOG
/* ============================================================
 * crash：跨复位崩溃账本
 * @details 这是唯一能在复位后活下来的那份计数：它放在共享 RAM 尾部的
 *          UNINIT 区（CRASH_LOG_BASE），上电清零、复位不清零。
 *          因此“镜像崩溃 -> 整机复位 -> 镜像又自启 -> 再崩”这条环
 *          能被记满上限并落成禁用，板子活下来。
 * @note 掉电即清零是设计取舍而不是遗漏：复位才是要数的那个事件，
 *       断电是操作者说“重新开始”。详见 arkrtos_crash.h。
 * ============================================================ */
#if ARKRTOS_USE_MINIAPP
/* MiniApp 日记的回填：日记里只存了路径哈希（定长的才能进 UNINIT 区），
 * 没存路径本身。要把一行读成人能用的东西，就得拿手上还知道的那份路径
 * 去对哈希——自启清单是唯一一份仍在板上的“路径 -> 哈希”对照表。
 * 对不上就不给路径：宁可显示哈希并标注未对上，也不猜一个像样的路径。 */
typedef struct {
    uint32 want;
    uint32 hit;
    char  *dst;
} mini_hash_lookup_t;

static int mini_hash_lookup_cb(const char *path, void *arg)
{
    mini_hash_lookup_t *m = (mini_hash_lookup_t *)arg;
    uint32 i;

    if(arkrtos_crash_hash_path(path) != m->want)
    {
        return 0;
    }

    /* 拷出来：path 落在 arkrtos_mini.c 的共用缓冲里，下一条会覆盖它。 */
    for(i = 0u; (i + 1u < ARKRTOS_MINI_PATH_MAX) && (path[i] != '\0'); i++)
    {
        m->dst[i] = path[i];
    }
    m->dst[i] = '\0';
    m->hit = 1u;

    return 1;   /* 找到了就停，别让后面的条目把答案冲掉 */
}
#endif /* ARKRTOS_USE_MINIAPP */

int cmd_crash(int argc, char *argv[])
{
    arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 i;
    uint32 shown = 0u;

    (void)argc;
    (void)argv;

    ark_shell_printf("journal: 0x%08X +%u bytes (uninitialized RAM: kept across reset, cleared by power-on)\r\n",
                     arkrtos_crash_area_base(), arkrtos_crash_area_size());
    ark_shell_printf("boots=%u seq=%u\r\n", arkrtos_crash_boots(), arkrtos_crash_seq());
    ark_shell_printf("\r\nslot type  faults last        hold      held_at\r\n");

    for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        uint32 cnt = arkrtos_crash_count(i);
        uint32 hold = arkrtos_crash_disabled(i);

        if((cnt == 0u) && (hold == 0u))
        {
            continue;
        }

        shown++;
        ark_shell_printf(" %u   %-4s  %-6u  %-10s  %-8s  %u\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         cnt,
                         arkrtos_crash_reason_name(arkrtos_crash_last_reason(i)),
                         arkrtos_crash_reason_name(hold),
                         arkrtos_crash_disabled_tick(i));
    }

    if(shown == 0u)
    {
        sh_out("\r\nno slot has a fault on record since the last power-on.\r\n");
    }
    else
    {
        sh_out("\r\nhold = the kernel will not autostart this slot. 'app start' / 'drv start'\r\n"
               "       is the explicit retry that clears it (the image must still validate).\r\n");
    }

#if ARKRTOS_USE_MINIAPP
    /* 小程序走自己的表（没有槽位号，身份是路径哈希），所以单独列一段：
     * 两套策略不能读成一套——看到的数字必须知道自己数的是什么。 */
    {
        char   path[ARKRTOS_MINI_PATH_MAX];
        uint32 j;
        uint32 mini_shown = 0u;

        ark_shell_printf("\r\nmini path  faults last        hold      held_at\r\n");

        for(j = 0u; j < ARKRTOS_CRASH_MINI_MAX; j++)
        {
            uint32 hash = 0u, cnt = 0u, last = 0u, hold = 0u, tick = 0u;
            mini_hash_lookup_t look;

            if(arkrtos_crash_mini_at(j, &hash, &cnt, &last, &hold, &tick) == 0u)
            {
                continue;
            }

            mini_shown++;
            path[0] = '\0';
            look.want = hash;
            look.hit  = 0u;
            look.dst  = path;
            (void)arkrtos_mini_autostart_foreach(mini_hash_lookup_cb, &look);

            if(look.hit != 0u)
            {
                ark_shell_printf(" %-10s  %-6u  %-10s  %-8s  %u\r\n",
                                 path, (unsigned)cnt,
                                 arkrtos_crash_reason_name(last),
                                 arkrtos_crash_reason_name(hold),
                                 (unsigned)tick);
            }
            else
            {
                ark_shell_printf(" #%08X    %-6u  %-10s  %-8s  %u\r\n",
                                 (unsigned)hash, (unsigned)cnt,
                                 arkrtos_crash_reason_name(last),
                                 arkrtos_crash_reason_name(hold),
                                 (unsigned)tick);
            }
        }

        if(mini_shown == 0u)
        {
            sh_out("no MiniApp has a fault on record since the last power-on.\r\n");
        }
        else
        {
            sh_out("path is known only while /mini.autostart still names it; '#' is the raw\r\n"
                   "       path hash otherwise. 'mini forget <path>' clears either.\r\n");
        }
    }
#endif /* ARKRTOS_USE_MINIAPP */

    return 0;
}

#endif /* ARKRTOS_USE_CRASH_LOG */
#if ARKRTOS_USE_AUDIT
int cmd_audit(int argc, char *argv[])
{
    arkrtos_audit_result_t r;
    uint32 bit;
    uint32 bad = 0u;

    (void)argc;
    (void)argv;

    arkrtos_audit_run(&r);

    for(bit = 1u; bit <= ARKRTOS_AUDIT_BIT_MAX; bit <<= 1)
    {
        const char *name = arkrtos_audit_name(bit);

        if(name == 0)
        {
            continue;
        }
        if((r.mask & bit) != 0u)
        {
            bad++;
        }
        ark_shell_printf("  %s: %s\r\n", name, ((r.mask & bit) != 0u) ? "BAD" : "ok");
    }

    ark_shell_printf("\r\nchecks=%u bad_groups=%u mask=0x%08X first_offender=%u\r\n",
                     r.checks, bad, r.mask, r.index);

    if(bad != 0u)
    {
        /* The groups above say which structure and which index. The cause is
         * not guessed here: a wrong cause costs a round of checking the wrong
         * thing, which is worse than an unanswered question. */
        sh_out("audit: INCONSISTENT (see the group lines above)\r\n");
        return (int)bad;
    }

    sh_out("audit: all structures self-consistent\r\n");
    return 0;
}

#endif /* ARKRTOS_USE_AUDIT */
int cmd_sched(int argc, char *argv[])
{
    int32  bad;
    int32  top;
    uint32 n;

    (void)argc;
    (void)argv;

    n   = arkrtos_sched_ready_count();
    top = arkrtos_ready_top();
    bad = arkrtos_sched_check();

    ark_shell_printf("\r\nready=%u top=%d\r\n", n, (int)top);
    if(bad != 0)
    {
        /* 光报一个数字没法定位：逐条打印「哪个任务、差在哪」。
         * 只看两种偏差——状态与就绪链不符（会漏调度或选到不该选的任务）。 */
        int32 i;

        for(i = 0; i < arkrtos_task_count; i++)
        {
            const arkrtos_task_t *p = &arkrtos_task_table[i];
            uint8 on = arkrtos_ready_contains(i);
            uint8 should = ((p->status == ARKRTOS_TASK_READY) ||
                            (p->status == ARKRTOS_TASK_RUNNING)) ? 1u : 0u;

            if(on != should)
            {
                ark_shell_printf("  #%d prio=%u status=%d on_ready=%u next=%d prev=%d\r\n",
                                 (int)i, (uint32)p->priority, (int)p->status,
                                 (uint32)on, (int)p->ready_next, (int)p->ready_prev);
            }
        }
        ark_shell_printf("sched: INCONSISTENT, %d mismatch(es)\r\n", (int)bad);
        return (int)bad;
    }

    sh_out("sched: consistent (bitmap/links == task table)\r\n");
    return 0;
}

/* dt: dump the compiled device tree (generated from dts/<board>.dts). */
int cmd_dt(int argc, char *argv[])
{
    uint32 i;
    uint32 total;

    (void)argc;
    (void)argv;

    total = arkrtos_dt_count();
    ark_shell_printf("\r\nmodel=%s nodes=%u\r\n",
                     arkrtos_dt_model(), (uint32)total);
    for(i = 0u; i < total; i++)
    {
        const arkrtos_dt_node_t *n = arkrtos_dt_node(i);

        if(n == 0)
        {
            continue;
        }
        if(n->is_device == 0u)
        {
            continue;
        }
        ark_shell_printf("  %s compat=%s reg=0x%x/0x%x %s\r\n",
                         n->name,
                         (n->compatible != 0) ? n->compatible : "-",
                         (uint32)n->reg_base, (uint32)n->reg_size,
                         (n->status_okay != 0u) ? "okay" : "disabled");
    }
    return 0;
}


#if (ARKRTOS_USE_FDT == 1)
#if (ARKRTOS_USE_FDT_OVERLAY == 1)
#include "arkrtos_fdt_overlay.h"
#endif
/* fdt: dump the tree parsed from the image .dtb.
 *
 * 与 dt 打同一套字段、同一个顺序：一份来自生成的 C 平表，一份来自启动时
 * 解析的标准 blob。两条独立数据通路打出同样的行，才能拿来做对账；如果
 * 格式各自一套，"看起来差不多"就没法变成判据。 */
/* fdt: 打印启动时解析出来的那棵树，并给出若干运行期入口。
 *
 * 为什么要有子命令：这一版之前 fdt 只能"把整棵树打出来"，于是"路径查询、
 * /chosen 的消费、运行期换树"三件事各自只有编译面，没有读数。现在它们
 * 都能在同一台机器上被看到，且失败时说清是哪一步失败。
 *
 *   fdt                 整棵树（与 dt 逐行同格式）
 *   fdt path <p>        按绝对路径查一个节点
 *   fdt phandle <n>     phandle -> 路径
 *   fdt chosen          /chosen/stdout-path 原文 + 解析结果
 *   fdt get <p> <prop>  打印一个属性的值（字符串 / 32 位单元）
 *   fdt overlay         把编进镜像的 demo fragment 合并进当前树
 *   fdt save <file>     把当前 blob 写成文件
 *   fdt load <file>     从文件读回一块 dtb 并换树（失败保留上一棵树）
 */
#if (ARKRTOS_USE_VFS == 1)
/* 动态装载的落地区：一块静态 RAM 加一块"文件原样"缓冲。
 * 4 KB 远大于任何一块真实板子的 dtb；超了 arkrtos_fdt_load 会拒绝，
 * 不会截断。 */
static uint8 s_fdt_arena[4096] __attribute__((aligned(8)));
static uint8 s_fdt_file[4096] __attribute__((aligned(8)));
#endif

static int fdt_dump(void)
{
    arkrtos_fdt_iter_t it;
    arkrtos_fdt_node_t n;
    const char *model;
    int32 rc;

    model = arkrtos_fdt_model();
    ark_shell_printf("\r\nmodel=%s nodes=%u\r\n",
                     (model != 0) ? model : "(none)",
                     (uint32)arkrtos_fdt_node_count());
    (void)arkrtos_fdt_iter_init(&it);
    while((rc = arkrtos_fdt_next(&it, &n)) == 1)
    {
        const char *compat = arkrtos_fdt_prop_str(&n, "compatible");
        uint32 reg0 = 0u;
        uint32 reg1 = 0u;

        (void)arkrtos_fdt_prop_u32(&n, "reg", 0u, &reg0);
        (void)arkrtos_fdt_prop_u32(&n, "reg", 1u, &reg1);
        /* 与 dt 相同的过滤口径：只报设备节点（有 compatible 的）。 */
        if(compat == 0)
        {
            continue;
        }
        ark_shell_printf("  %s compat=%s reg=0x%x/0x%x %s\r\n",
                         n.name, compat, (uint32)reg0, (uint32)reg1,
                         (arkrtos_fdt_node_enabled(&n) != 0) ? "okay" : "disabled");
    }
    if(rc < 0)
    {
        ark_shell_printf("fdt: walk stopped by error %d\r\n", (int)rc);
        return 1;
    }
    return 0;
}

static int fdt_show_path(const char *path)
{
    arkrtos_fdt_node_t n;
    const char *compat;
    uint32 reg0 = 0u;
    uint32 reg1 = 0u;
    int32 rc = arkrtos_fdt_node_by_path(path, &n);

    if(rc != ARKRTOS_FDT_OK)
    {
        ark_shell_printf("fdt: path %s rc=%d\r\n", path, (int)rc);
        return 1;
    }
    compat = arkrtos_fdt_prop_str(&n, "compatible");
    (void)arkrtos_fdt_prop_u32(&n, "reg", 0u, &reg0);
    (void)arkrtos_fdt_prop_u32(&n, "reg", 1u, &reg1);
    ark_shell_printf("path=%s name=%s depth=%u props=%u %s\r\n",
                     path, n.name, (uint32)n.depth, (uint32)n.n_props,
                     (arkrtos_fdt_node_enabled(&n) != 0) ? "okay" : "disabled");
    ark_shell_printf("  compat=%s reg=0x%x/0x%x\r\n",
                     (compat != 0) ? compat : "(none)",
                     (uint32)reg0, (uint32)reg1);
    return 0;
}

static int fdt_show_chosen(void)
{
    arkrtos_fdt_console_t c;
    const char *raw = arkrtos_fdt_stdout_path();
    uint32 len = 0u;
    const void *v = arkrtos_fdt_stdout_raw(&len);
    int32 rc = arkrtos_fdt_stdout_resolve(&c);

    if(v == 0)
    {
        ark_shell_printf("\r\nchosen stdout-path=(absent)\r\n");
        return 1;
    }
    if(raw != 0)
    {
        ark_shell_printf("\r\nchosen stdout-path=\"%s\"\r\n", raw);
    }
    else
    {
        /* 非字符串（句柄形态）：原文不能当字符串打，打长度。 */
        ark_shell_printf("\r\nchosen stdout-path=<%u byte cell>\r\n",
                         (uint32)len);
    }
    if(rc != ARKRTOS_FDT_OK)
    {
        /* 解析失败要明说"没有用它"，不能让读者以为控制台已经按它改了。 */
        ark_shell_printf("  resolve rc=%d -> NOT applied, board default kept\r\n",
                         (int)rc);
        return 1;
    }
    ark_shell_printf("  path=%s baud=%u bits=%u parity=%c alias=%u\r\n",
                     c.path, (uint32)c.baud, (uint32)c.bits,
                     (c.parity != 0) ? c.parity : '-', (uint32)c.from_alias);
    return 0;
}

#if (ARKRTOS_USE_VFS == 1)
static int fdt_save_to(const char *path)
{
    const void *blob = arkrtos_fdt_blob();
    uint32 size = arkrtos_fdt_size();
    int32 rc;

    if((blob == 0) || (size == 0u))
    {
        sh_out("fdt: no tree parsed, nothing to save\r\n");
        return 1;
    }
    rc = arkrtos_vfs_write_file(path, (const uint8 *)blob, size);
    ark_shell_printf("fdt: save %s rc=%d bytes=%u\r\n",
                     path, (int)rc, (uint32)size);
    return (rc == 0) ? 0 : 1;
}

static int fdt_load_from(const char *path)
{
    uint32 before = arkrtos_fdt_node_count();
    uint32 n = 0u;
    uint32 cap = (uint32)sizeof(s_fdt_arena);
    int32 rc;
    int32 frc;

    frc = arkrtos_vfs_read_file(path, s_fdt_file, (uint32)sizeof(s_fdt_file), &n);
    if(frc != 0)
    {
        ark_shell_printf("fdt: load %s read rc=%d (tree untouched, nodes=%u)\r\n",
                         path, (int)frc, (uint32)before);
        return 1;
    }
    rc = arkrtos_fdt_load(s_fdt_arena, cap, s_fdt_file, n);
    if(rc != ARKRTOS_FDT_OK)
    {
        /* 事务性：拒绝新 blob 时上一棵树还在，这里正是那条语义的读数。 */
        ark_shell_printf("fdt: load %s rc=%d bytes=%u -> refused, previous tree kept (nodes=%u)\r\n",
                         path, (int)rc, (uint32)n, (uint32)arkrtos_fdt_node_count());
        return 1;
    }
    ark_shell_printf("fdt: load %s rc=0 bytes=%u nodes=%u (was %u)\r\n",
                     path, (uint32)n, (uint32)arkrtos_fdt_node_count(), (uint32)before);
    return 0;
}
#endif /* ARKRTOS_USE_VFS */


/* fdt get <path> <prop>：把一个属性的**值**打出来。
 * 这是 overlay 之后"属性真的变了"的唯一读数——树 dump 只报设备节点，
 * 而 overlay 改的往往是 model / 自定义属性这种不是设备的东西。 */
static int fdt_show_prop(const char *path, const char *prop)
{
    arkrtos_fdt_node_t n;
    const void *v;
    uint32 len = 0u;
    uint32 i;
    int32 rc = arkrtos_fdt_node_by_path(path, &n);

    if(rc != ARKRTOS_FDT_OK)
    {
        ark_shell_printf("fdt: path %s rc=%d\r\n", path, (int)rc);
        return 1;
    }
    v = arkrtos_fdt_prop(&n, prop, &len);
    if(v == 0)
    {
        ark_shell_printf("fdt: %s has no property '%s'\r\n", path, prop);
        return 1;
    }
    ark_shell_printf("%s %s len=%u ", path, prop, (uint32)len);

    /* 字符串形态：以 NUL 收尾，且中间没有不可打印字节。
     * string list 逐段打印——两段之间用 "," 分开，不合并成一个长串。 */
    {
        const char *s = (const char *)v;
        uint32 k;
        uint32 printable = 1u;

        if((len == 0u) || (s[len - 1u] != '\0'))
        {
            printable = 0u;
        }
        for(k = 0u; (printable != 0u) && (k < len); k++)
        {
            uint8 c = (uint8)s[k];

            if((c != 0u) && ((c < 0x20u) || (c > 0x7eu)))
            {
                printable = 0u;
            }
        }
        if(printable != 0u)
        {
            ark_shell_printf("str=\"");
            for(k = 0u; k < len; )
            {
                sh_out(&s[k]);
                k += (uint32)strlen(&s[k]) + 1u;
                if(k < len)
                {
                    ark_shell_printf("\",\"");
                }
            }
            ark_shell_printf("\"\r\n");
            return 0;
        }
    }

    ark_shell_printf("cells=");
    for(i = 0u; ((i + 1u) * 4u) <= len; i++)
    {
        uint32 u = 0u;

        (void)arkrtos_fdt_prop_u32(&n, prop, i, &u);
        ark_shell_printf("%s0x%x", (i == 0u) ? "" : " ", (uint32)u);
    }
    if((len & 3u) != 0u)
    {
        ark_shell_printf(" (tail %u byte(s) not cells)", (uint32)(len & 3u));
    }
    ark_shell_printf("\r\n");
    return 0;
}

#if (ARKRTOS_USE_FDT_OVERLAY == 1)
/* 合并后的树放在这里：overlay 不碰当前树，只有成功后 load 才换树。
 * 8 KB 是 demo fragment 的两级余量；小了会明确报 OVERLAY_SIZE。 */
static uint8 s_fdt_ovl[8192] __attribute__((aligned(8)));

static int fdt_overlay_apply(void)
{
    uint32 frag_len = 0u;
    uint32 out_len = 0u;
    uint32 before = arkrtos_fdt_node_count();
    const void *blob = arkrtos_fdt_blob();
    uint32 blob_len = arkrtos_fdt_size();
    const void *frag = arkrtos_board_fdt_overlay_demo(&frag_len);
    int32 rc;

    if((frag == 0) || (frag_len == 0u))
    {
        sh_out("fdt: overlay demo fragment is not built into this image\r\n");
        return 1;
    }
    rc = arkrtos_fdt_overlay(s_fdt_ovl, (uint32)sizeof(s_fdt_ovl),
                             blob, blob_len, frag, frag_len, &out_len);
    if(rc != ARKRTOS_FDT_OK)
    {
        /* 拒绝时把"哪一类不支持"说清楚；这棵树没被动过。 */
        ark_shell_printf("fdt: overlay rc=%d (%s) -> refused, tree kept (nodes=%u)\r\n",
                         (int)rc, arkrtos_fdt_overlay_reason(rc), (uint32)before);
        return 1;
    }
    rc = arkrtos_fdt_load(s_fdt_ovl, (uint32)sizeof(s_fdt_ovl),
                          s_fdt_ovl, out_len);
    if(rc != ARKRTOS_FDT_OK)
    {
        ark_shell_printf("fdt: overlay merged %u bytes but init rc=%d -> tree kept\r\n",
                         (uint32)out_len, (int)rc);
        return 1;
    }
    ark_shell_printf("fdt: overlay ok bytes=%u nodes=%u (was %u) model=%s\r\n",
                     (uint32)out_len, (uint32)arkrtos_fdt_node_count(),
                     (uint32)before,
                     (arkrtos_fdt_model() != 0) ? arkrtos_fdt_model() : "(none)");
    return 0;
}
#endif /* ARKRTOS_USE_FDT_OVERLAY */

int cmd_fdt(int argc, char *argv[])
{
    if(arkrtos_fdt_ready() == 0)
    {
        sh_out("fdt: no device tree blob parsed at boot\r\n");
        return 1;
    }
    if(argc < 2)
    {
        return fdt_dump();
    }
    if(strcmp(argv[1], "path") == 0)
    {
        if(argc < 3)
        {
            sh_out("fdt path <absolute path>\r\n");
            return 1;
        }
        return fdt_show_path(argv[2]);
    }
    if(strcmp(argv[1], "phandle") == 0)
    {
        char  buf[ARKRTOS_FDT_PATH_MAX];
        uint32 ph = 0u;
        int32 rc;

        if((argc < 3) || (parse_u32(argv[2], &ph) != 0))
        {
            sh_out("fdt phandle <decimal>\r\n");
            return 1;
        }
        rc = arkrtos_fdt_path_of_phandle(ph, buf, (uint32)sizeof(buf));
        if(rc != ARKRTOS_FDT_OK)
        {
            ark_shell_printf("fdt: phandle %u rc=%d\r\n", (uint32)ph, (int)rc);
            return 1;
        }
        ark_shell_printf("phandle %u -> %s\r\n", (uint32)ph, buf);
        return 0;
    }
    if(strcmp(argv[1], "chosen") == 0)
    {
        return fdt_show_chosen();
    }
    if(strcmp(argv[1], "get") == 0)
    {
        if(argc < 4)
        {
            sh_out("fdt get <absolute path> <property>\r\n");
            return 1;
        }
        return fdt_show_prop(argv[2], argv[3]);
    }
#if (ARKRTOS_USE_FDT_OVERLAY == 1)
    if(strcmp(argv[1], "overlay") == 0)
    {
        return fdt_overlay_apply();
    }
#endif
#if (ARKRTOS_USE_VFS == 1)
    if(strcmp(argv[1], "save") == 0)
    {
        if(argc < 3)
        {
            sh_out("fdt save <path>\r\n");
            return 1;
        }
        return fdt_save_to(argv[2]);
    }
    if(strcmp(argv[1], "load") == 0)
    {
        if(argc < 3)
        {
            sh_out("fdt load <path>\r\n");
            return 1;
        }
        return fdt_load_from(argv[2]);
    }
#endif
    sh_out("fdt: unknown subcommand (path|phandle|chosen|get|overlay|save|load)\r\n");
    return 1;
}
#endif /* ARKRTOS_USE_FDT */

/* ============================================================
 * fault：故障记录
 * ============================================================ */
int cmd_fault(int argc, char *argv[])
{
    int32 n;
    int32 i;

    (void)argc;
    (void)argv;

    n = arkrtos_fault_record_count_internal();
    if(n <= 0)
    {
        sh_out("no fault recorded\r\n");
        return 0;
    }

    ark_shell_printf("\r\n#   tick      task  type\r\n");
    for(i = 0; i < n; i++)
    {
        const arkrtos_fault_record_t *r = arkrtos_fault_record_get(i);

        if(r == 0)
        {
            continue;
        }

        ark_shell_printf(" %-3d %-9u %-5d %s\r\n",
                         (int)i, r->tick, (int)r->task_id, fault_type_name(r->type));
    }
    return 0;
}

/* ============================================================
 * log：查看 / 调整运行期日志级别（内核与所有 App、驱动同时生效）
 * ============================================================ */
static void syncinfo_row(const char *tag, int32 idx, int32 total,
                          const arkrtos_sync_dbg_row_t *row)
{
    int32 i;

    if(idx == 0)
    {
        ark_shell_printf("\r\n%s  used  count  owner  creator  waiters\r\n", tag);
    }
    if(row->used == 0)
    {
        return;
    }
    ark_shell_printf(" %-3d %-5u %-6d %-6d %-8d ", (int)idx, (uint32)row->used,
                     (int)row->count, (int)row->owner_id, (int)row->creator_id);
    if(row->nwait == 0)
    {
        ark_shell_printf("-\r\n");
        return;
    }
    for(i = 0; i < row->nwait; i++)
    {
        ark_shell_printf("t%u%s", (unsigned int)row->waiter_id[i],
                         ((i + 1) < row->nwait) ? "," : "\r\n");
    }
    (void)total;
}

int cmd_syncinfo(int argc, char *argv[])
{
    arkrtos_sync_dbg_row_t row;
    int32 i;

    (void)argc;
    (void)argv;

    for(i = 0; i < (int32)arkrtos_sync_sem_num(); i++)
    {
        if(arkrtos_sync_sem_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("sem", i, (int32)arkrtos_sync_sem_num(), &row);
    }
    for(i = 0; i < (int32)arkrtos_sync_mtx_num(); i++)
    {
        if(arkrtos_sync_mtx_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("mtx", i, (int32)arkrtos_sync_mtx_num(), &row);
    }
    for(i = 0; i < (int32)arkrtos_sync_cond_num(); i++)
    {
        if(arkrtos_sync_cond_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("cond", i, (int32)arkrtos_sync_cond_num(), &row);
    }
    for(i = 0; i < (int32)arkrtos_sync_sem_num(); i++)
    {
        uint32 posts = 0u, takes = 0u, queued = 0u, last = 0u;
        uint32 woke = 0u, reason = 0u, also = 0u;
        uint32 qbefore = 0u;

        if(arkrtos_sync_sem_debug(i, &row) != 0)
        {
            break;
        }
        if(row.used == 0)
        {
            continue;
        }
        arkrtos_sync_sem_trace(i, &posts, &takes, &queued, &last);
        arkrtos_sync_sem_handoff(i, &woke, &reason, &also);
        qbefore = arkrtos_sync_sem_qbefore(i);
        ark_shell_printf("  sem %d: posts=%u takes=%u queued=%u last_waiter=t%u "
                         "qbefore=%u woke=t%u reason=%u\r\n",
                         (int)i, (unsigned int)posts, (unsigned int)takes,
                         (unsigned int)queued, (unsigned int)last,
                         (unsigned int)qbefore,
                         (unsigned int)woke, (unsigned int)reason);
    }
    ark_shell_printf("sync diagnostics: ghost=%u dup=%u\r\n",
                     (unsigned int)arkrtos_diag_sync_ghost,
                     (unsigned int)arkrtos_diag_sync_dup);
    ark_shell_printf("sync ghost  : n=%u idx=%d tid=t%u\r\n",
                     (unsigned int)dbg_sem_ghost_cnt,
                     (int)(int32)dbg_sem_ghost_idx,
                     (unsigned int)dbg_sem_ghost_tid);
    ark_shell_printf("sync appwait: hit=%u\r\n",
                     (unsigned int)dbg_sem_appq_hit);
    return 0;
}

#if ARKRTOS_USE_LOG
int cmd_log(int argc, char *argv[])
{
    const char *names[5] = { "off", "error", "warning", "info", "debug" };

    if(argc >= 2)
    {
        uint32 lvl;

        if((parse_u32(argv[1], &lvl) != 0) || (lvl > (uint32)ARKRTOS_LOG_DEBUG))
        {
            sh_out("usage: log <0..4>   (0=off 1=error 2=warn 3=info 4=debug)\r\n");
            return -1;
        }

        arkrtos_log_set_level(lvl);
    }

    ark_shell_printf("log level: %u (%s)\r\n",
                     arkrtos_log_get_level(), names[arkrtos_log_get_level() & 7u]);

    /* Console health.  lock_giveup counts output units written without the
     * console lock (they may have been spliced); fifo_refused counts bytes
     * the transmit pipe refused, i.e. output that never left the MCU. */
    ark_shell_printf("console   : lock_giveup=%u tx_drop=%u B fifo_full_retries=%u\r\n",
                     arkrtos_console_busy_count(), arkrtos_console_tx_drop_count(),
                     arkrtos_fifo_write_refused());
                     /* tx_drop is the number that matters: bytes that never
                      * left the MCU.  fifo_full_retries is attempts, not loss. */

    ark_shell_printf("fifo      : bad_header=%u bytes_lost=%u\r\n",
                     arkrtos_fifo_bad_magic(), arkrtos_fifo_bad_magic_bytes());

    /* Token conservation on the sync objects.  ghost counts wakes that carried
     * no ARKRTOS_WAKE_HANDOFF bit, i.e. successes the kernel refused to claim;
     * dup counts a task that tried to register twice in one waiter queue. */
    ark_shell_printf("sync      : ghost=%u dup=%u\r\n",
                     arkrtos_diag_sync_ghost, arkrtos_diag_sync_dup);
    /* Non-zero means a FIFO header was overwritten: input or output
     * through it is being dropped for good, not merely throttled. */

    return 0;
}

#endif /* ARKRTOS_USE_LOG */
/* ============================================================
 * rt：实时性度量（DWT 周期计数）
 * ------------------------------------------------------------
 * 回答的是"这套系统的时间上界在哪"这一类问题，不是把实时性证明出来：
 * 打出来的是本机本配置下测到的最大值，测不到更坏的情况只说明这段时间
 * 没发生更坏的情况。三条边界的完整说法见 arkrtos_rt.h 文件头。
 * ============================================================ */
#if (ARKRTOS_USE_RTSTAT == 1)
static const char *rt_unit_c = "cyc";
static const char *rt_unit_us = "us";

static void rt_print_row(const arkrtos_rt_row_t *r)
{
    if(r->n == 0u)
    {
        ark_shell_printf("  %-9s %-7s (no sample)\r\n", r->name, "-");
        return;
    }

    ark_shell_printf("  %-9s %-7u %-9u %-9u %-9u %-8u %-8u %-8u",
                     r->name, r->n,
                     r->min_c, r->avg_c, r->max_c,
                     r->min_us, r->avg_us, r->max_us);
    if(r->late_n != 0u)
    {
        ark_shell_printf("  late=%u", r->late_n);
    }
    if(r->skip_n != 0u)
    {
        ark_shell_printf("  skip=%u", r->skip_n);
    }
    ark_shell_printf("\r\n");
}

static int rt_show(void)
{
    arkrtos_rt_row_t rows[ARKRTOS_RT_ROWS];
    uint32 n;
    uint32 k;
    uint32 per_tick = arkrtos_rt_cycles_per_tick();
    int32  j_min = 0;
    int32  j_max = 0;
    uint32 j_n   = 0;

    ark_shell_printf("\r\nreal-time measurement (hardware cycle counter)\r\n");

    if(per_tick == 0u)
    {
        ark_shell_printf("  cycle counter unavailable - no measurement possible\r\n");
        ark_shell_printf("  (nothing is estimated from the tick period on purpose)\r\n");
        return -1;
    }

    {
        uint32 clk = arkrtos_port_get_cycle_clock();
        uint32 tick_us;

        /* 走内核同一套换算（rv32imac 上 64 位除法链接不过）。 */
        tick_us = arkrtos_rt_to_us(per_tick);
        /* ARM 上这个频率就是内核时钟（同源常量）；RISC-V / LoongArch 上是
         * 开机时用节拍时基量出来的。来源必须写清楚：一个是抄来的，一个是
         * 量出来的，读数的可信度不同。 */
        ark_shell_printf("  cycles per tick: %u  (cycle clock %u Hz %s, tick %u us)\r\n",
                         per_tick, clk,
                         (arkrtos_port_cycle_clock_measured() != 0) ? "[measured]" : "[board constant]",
                         tick_us);

        /* 先证明计数器本身在走再谈读数。调试器断开时会把 DEMCR.TRCENA 清掉，
         * 那之后 CYCCNT 就停在原地；这里重新武装一次并当场报出来。
         * （2026-11-20 修正：这里原来的判断写反了，alive == 0 时报的是
         *   "running"——读数页上会给出恰好相反的结论。） */
        if(arkrtos_rt_counter_alive() != 0)
        {
            ark_shell_printf("  counter: running\r\n");
        }
        else
        {
            ark_shell_printf("  counter: NOT running (it does not advance)"
                             " - numbers below are stale\r\n");
        }
    }

    ark_shell_printf("  %-9s %-7s %-9s %-9s %-9s %-8s %-8s %-8s\r\n",
                     "ROW", "N", "MIN(" , "AVG(", "MAX(", rt_unit_us, rt_unit_us, rt_unit_us);
    ark_shell_printf("  %-9s %-7s %-9s %-9s %-9s %-8s %-8s %-8s\r\n",
                     "", "", rt_unit_c, rt_unit_c, rt_unit_c, "MIN", "AVG", "MAX");

    n = arkrtos_rt_snapshot(rows, ARKRTOS_RT_ROWS);
    for(k = 0u; k < n; k++)
    {
        rt_print_row(&rows[k]);
    }

    if(arkrtos_rt_jitter(&j_min, &j_max, &j_n) != 0u)
    {
        ark_shell_printf("  jitter    n=%u  min=%d  max=%d  (signed cycles, + = late)\r\n",
                         j_n, j_min, j_max);
    }
    else
    {
        ark_shell_printf("  jitter    (no sample)\r\n");
    }

    #if (ARKRTOS_USE_TICKLESS == 1)
    {
        /* 开关生效的反假绿读数：没有这一行，"节拍没变慢"与"根本
         * 没在睡"在串口上无法区分。 */
        uint32 tl_isrs   = 0u;
        uint32 tl_sleeps = 0u;
        uint32 tl_ticks  = arkrtos_kernel_get_tick();

        arkrtos_tickless_stats(&tl_isrs, &tl_sleeps);
        /* 跳过的拍数不单独统计：每个中断至少贡献一拍，剩下的必然是补发
         * 路径走完的。所以这里用减法算，identity 是定义而不是测得量。 */
        ark_shell_printf("  tickless  isr=%u  sleeps=%u  skipped=%u of %u tick(s)"
                         "  cap=%u\r\n",
                         tl_isrs, tl_sleeps,
                         (tl_ticks > tl_isrs) ? (tl_ticks - tl_isrs) : 0u,
                         tl_ticks, (uint32)ARKRTOS_TICKLESS_MAX_TICKS);
    }
    #endif

    ark_shell_printf("  note: maxima measured on this board with this build;\r\n");
    ark_shell_printf("        they are not a formal WCET bound.  rows with no sample\r\n");
    ark_shell_printf("        mean nothing of that kind happened since the last reset\r\n");
    ark_shell_printf("        (crit.max fills only when the kernel really enters a\r\n");
    ark_shell_printf("        critical section).\r\n");
    return 0;
}

int cmd_rt(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "show";
    uint32 n = 200u;
    int ret;

    if(strcmp(sub, "show") == 0)
    {
        return rt_show();
    }

    if(strcmp(sub, "reset") == 0)
    {
        arkrtos_rt_reset();
        ark_shell_printf("real-time statistics cleared\r\n");
        return 0;
    }

    if(strcmp(sub, "run") == 0)
    {
        if(argc >= 3)
        {
            if((parse_u32(argv[2], &n) != 0) || (n < 1u) || (n > 5000u))
            {
                ark_shell_printf("usage: rt run [n]   (1 <= n <= 5000)\r\n");
                return -1;
            }
        }

        ark_shell_printf("measuring %u samples (wake, then preempt)...\r\n", n);

        /* 开测前重新武装计数器并清空统计：开机到现在的样本是在"计数器可能
         * 被调试器关掉"的窗口里采的，不能混进这一轮。 */
        arkrtos_rt_init();
        if(arkrtos_rt_cycles_per_tick() == 0u)
        {
            ark_shell_printf("cycle counter still not running - cannot measure\r\n");
            return -1;
        }

        ret = arkrtos_rt_measure_wake(n);
        if(ret != 0)
        {
            ark_shell_printf("wake scenario: sampler not ready (DWT unavailable)\r\n");
            return -1;
        }

        ret = arkrtos_rt_measure_preempt(n);
        if(ret != 0)
        {
            ark_shell_printf("preempt scenario: failed (reason code %d)\r\n",
                             (int)arkrtos_rt_last_error());
            ark_shell_printf("  see arkrtos_rt.h: 1/2 = semaphore create,"
                             " 5 = helper task register\r\n");
            return -1;
        }

        return rt_show();
    }

    ark_shell_printf("\r\nusage: rt [show | reset | run [n]]\r\n");
    return -1;
}
#endif /* ARKRTOS_USE_RTSTAT */

/* ============================================================
 * sec：安全基线自检（只读，不改任何结构）
 * ============================================================ */
#if (ARKRTOS_USE_SECCHECK == 1)
int cmd_sec(int argc, char *argv[])
{
    /* 报告里带 16 个项，别放栈上：内核任务栈不一定宽裕。 */
    static arkrtos_sec_report_t rep;
    const char *filter = (argc >= 2) ? argv[1] : "all";
    uint32 want = 0xFFFFFFFFu;
    uint32 i;
    uint32 fails;

    if(strcmp(filter, "all") == 0)
    {
        want = 0xFFFFFFFFu;
    }
    else if(strcmp(filter, "pass") == 0)
    {
        want = (uint32)ARKRTOS_SEC_PASS;
    }
    else if(strcmp(filter, "fail") == 0)
    {
        want = (uint32)ARKRTOS_SEC_FAIL;
    }
    else if((strcmp(filter, "find") == 0) || (strcmp(filter, "finding") == 0))
    {
        want = (uint32)ARKRTOS_SEC_FINDING;
    }
    else if(strcmp(filter, "skip") == 0)
    {
        want = (uint32)ARKRTOS_SEC_SKIP;
    }
    else
    {
        ark_shell_printf("usage: sec [all | pass | fail | find | skip]\r\n");
        return -1;
    }

    fails = arkrtos_sec_run(&rep);

    ark_shell_printf("sec: %u checks - pass %u, fail %u, finding %u, skip %u\r\n",
                     (unsigned)rep.count, (unsigned)rep.pass, (unsigned)rep.fail,
                     (unsigned)rep.finding, (unsigned)rep.skip);

    for(i = 0u; i < rep.count; i++)
    {
        const arkrtos_sec_item_t *p_it = &rep.item[i];

        if((want != 0xFFFFFFFFu) && (p_it->state != want))
        {
            continue;
        }

        ark_shell_printf("  [%s] %-18s ", arkrtos_sec_state_name(p_it->state), p_it->name);
        if((p_it->fmt != 0) && (p_it->fmt[0] != '\0'))
        {
            ark_shell_printf(p_it->fmt, (unsigned)p_it->v[0], (unsigned)p_it->v[1],
                             (unsigned)p_it->v[2], (unsigned)p_it->v[3]);
        }
        ark_shell_printf("\r\n");
    }

    if(fails == 0u)
    {
        ark_shell_printf("verdict: no hard failure (FINDING = known gap, SKIP = no path to see it)\r\n");
    }
    else
    {
        ark_shell_printf("verdict: %u hard failure(s) - this build is not what you think it is\r\n",
                         (unsigned)fails);
    }

    ark_shell_printf("note: read-only baseline; the privilege/MPU denials are exercised by an App"
                     " (HWSAFE_DEMO, APP_BAD), not from here\r\n");
    return (fails == 0u) ? 0 : -1;
}
#endif /* ARKRTOS_USE_SECCHECK */


/* ============================================================
 * utest：板端单元测试
 * ============================================================ */
#if ARKRTOS_USE_UTEST
static void utest_out(const char *s)
{
    sh_out(s);
    sh_out("\r\n");
}

int cmd_utest(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "run";

    /* 框架不依赖 printf：把每一行交给 shell 的控制台汇 */
    ark_utest_set_output(utest_out);

    if(strcmp(sub, "list") == 0)
    {
        (void)ark_utest_list();
        return 0;
    }
    if(strcmp(sub, "run") == 0)
    {
        ark_utest_summary_t s;
        uint32 failed = ark_utest_run((argc >= 3) ? argv[2] : (const char *)0, &s);
        /* 返回失败用例数，让脚本能靠返回值判定红绿 */
        return (failed == 0u) ? 0 : (int)failed;
    }

    ark_shell_printf("\r\nusage: utest [list | run [pattern]]\r\n");
    return -1;
}
#endif /* ARKRTOS_USE_UTEST */

int cmd_pool(int argc, char *argv[])
{
    uint32 largest = 0u;
    uint32 total = arkrtos_loader_pool_free(&largest);

    (void)argc;
    (void)argv;

    ark_shell_printf("\r\npool free : %u B (%uK)\r\n", total, total / 1024u);
    ark_shell_printf("largest   : %u B (%uK)\r\n", largest, largest / 1024u);

    /* 固定槽位模式下「池里还剩多少字节」并不回答「我还能装下一个镜像吗」：
     * 每个槽各占一块固定地址，能装多大事先已经由槽大小定死了。
     * 所以这里直接列出各槽自己还有没有位置，免得操作者拿一个与本次安装
     * 无关的数字去做决定。 */
    if(arkrtos_ptable_get()->layout_mode == (uint32)ARKRTOS_LAYOUT_MODE_FIXED)
    {
        const arkrtos_partition_table_t *pt = arkrtos_ptable_get();
        const arkrtos_cfg_slot_t *slots = arkrtos_layout_slots();
        uint32 count = arkrtos_layout_slot_count();
        uint32 i;

        sh_out("\r\nfixed slots\r\n");
        sh_out(" id  type  base        size     state\r\n");

        for(i = 0u; i < count; i++)
        {
            const char *state = "empty";
            uint32 j;

            for(j = 0u; (j < pt->slot_max) && (j < ARKRTOS_SLOT_ARRAY_MAX); j++)
            {
                if((pt->slot_type[j] != ARKRTOS_SLOT_FREE) &&
                   (pt->slot_base[j] == slots[i].base) &&
                   (pt->slot_size[j] == slots[i].size))
                {
                    state = slot_state_name(pt->slot_state[j]);
                    break;
                }
            }

            ark_shell_printf(" %-3u %-4s  0x%08X  %-8u  %s\r\n",
                             (unsigned)i, cfg_type_text(slots[i].type),
                             (unsigned)slots[i].base, (unsigned)slots[i].size,
                             state);
        }
    }

    return 0;
}

#endif /* SHELL_ENABLE */
