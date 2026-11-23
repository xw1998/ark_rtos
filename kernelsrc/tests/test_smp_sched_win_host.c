/**
* @file test_smp_sched_win_host.c
* @brief 次核「装窗口 + 补特权级」恢复路径（R3）的宿主用例
* @details 被验的是 arkrtos_smp_sched.c 里 ARK_SMP_WIN 那一段（PAGING 与
*          PRIV 同时为 1 才编），也就是次核每次 ertn 之前必须做的两件事：
*
*            1. 目标任务的栈如果落在 App 窗口 VA 上，就把该任务绑定的
*               空间装到**本核**（每核一份窗口状态，不是全局一份）；
*            2. 把「恢复后是不是用户态」这件事交给 port 层的定级函数，
*               本模块只表达意图。
*
*          钉死的都是"错了也看不出来"的地方：
*            - 装窗口失败时必须把 s_win_space 复位成「没装过」，并且把
*              返回码如实记下来（假成功会让下一个任务取指跑飞）；
*            - 请求是一次性消费：同一份 pending 只装一次，第二次 prepare
*              不许重复装（重复装=每次都 flush TLB）；
*            - 特权任务且栈在恒等映射里时不装窗口（那是内核任务，窗口里
*              装着什么由上一个用户任务决定，动它等于把别人的页换给内核）；
*            - 特权任务但栈在窗口里时必须装（R3 的判决用地址不用特权级）；
*            - 核号越界时什么都不做（宁可不动，也不替别的核改窗口）。
*
*          测试之间必须互不污染：本模块的状态是静态的（没有 reset 接口），
*          所以每个用例开头把上一次迁出去的任务全部交还并 poll 干净，
*          读数一律按"相对本次开头"的增量判，不写死绝对值。
*
*          明确没验证：TLB 真的换了、用户态真的进不去、真次核异常入口的
*          压栈/ertn。这里全是对 port 的调用与自己的账目，port 是桩。
*
* @author xw
* @date 2026.11.23
*/
#include <stdio.h>
#include <string.h>

#include "arkrtos_smp_sched.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_spin.h"
#include "arkrtos_mpu.h"

#if (ARKRTOS_USE_SMP_SCHED != 1)
#error "test_smp_sched_win_host.c must be compiled with -DARKRTOS_USE_SMP_SCHED=1"
#endif

#if ((ARKRTOS_USE_PAGING != 1) || (ARKRTOS_USE_PRIV != 1))
#error "本用例专测「分页 + 特权级」都开时的窗口恢复路径"
#endif

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

#define CK_I(got, want)                                                   \
    do {                                                                  \
        long g_ = (long)(got);                                            \
        long w_ = (long)(want);                                           \
        g_checks++;                                                       \
        if (g_ != w_) {                                                   \
            printf("FAIL %s:%d  got %ld want %ld\n", __FILE__, __LINE__,  \
                   g_, w_);                                               \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------
 * 任务表桩（与 test_smp_sched_host.c 同一套约定）
 * ------------------------------------------------------------ */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count = 0;
int32          arkrtos_current_task_id = 0;

void arkrtos_ready_add(int32 task_idx)
{
    (void)task_idx;
}

void arkrtos_ready_del(int32 task_idx)
{
    (void)task_idx;
}

void arkrtos_delay_disarm(int32 task_idx)
{
    (void)task_idx;
}

/* ------------------------------------------------------------
 * port 桩：窗口安装 / 窗口判定 / 帧定级
 * ------------------------------------------------------------ */

#define WIN_VA      0x02000000ul
#define WIN_LEN     0x00200000ul

static uint32 g_cpu = 1u;
static uint32 g_install_calls;
static uint32 g_install_last_space;
static int32  g_install_rc;

static uint32 g_priv_calls;
static uint32 g_priv_last_want;
static void  *g_priv_last_frame;

/* 任务下标 -> 绑定空间。真正的实现在 arkrtos_paging.c，本用例不链接它。 */
static uint32 g_space[ARKRTOS_TASK_MAX_NUM];

uint32 arkrtos_port_cpu_id(void)
{
    return g_cpu;
}

void arkrtos_port_wfi(void) { }

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,
                               uint32 new_value)
{
    if (*p_addr != expect)
    {
        return 0u;
    }
    *p_addr = new_value;
    return 1u;
}

void arkrtos_port_spin_hint(void) { }
void arkrtos_port_dmb(void)       { }

void arkrtos_port_start_timer(uint32 tick_period_us)
{
    (void)tick_period_us;
}

void arkrtos_port_enable_irq(void) { }

int32 arkrtos_port_paging_install_space_window(uint32 space)
{
    g_install_calls++;
    g_install_last_space = space;
    return g_install_rc;
}

/* 与 QEMU 板一致的口径：[WIN_VA, WIN_VA+WIN_LEN) 内且整段不越界才算。 */
int32 arkrtos_port_paging_va_in_window(unsigned long va, uint32 len)
{
    if (va < WIN_VA)
    {
        return 0;
    }
    if ((va - WIN_VA) >= WIN_LEN)
    {
        return 0;
    }
    if (len > (uint32)(WIN_LEN - (va - WIN_VA)))
    {
        return 0;
    }
    return 1;
}

void arkrtos_port_frame_apply_priv_auto(void *frame, uint32 want_user)
{
    g_priv_calls++;
    g_priv_last_frame = frame;
    g_priv_last_want  = want_user;
}

uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task)
{
    int32 idx;

    if (p_task == 0)
    {
        return 0u;
    }
    idx = (int32)(p_task - arkrtos_task_table);
    if ((idx < 0) || (idx >= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return 0u;
    }
    return g_space[idx];
}

/* ------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------ */

#define NTASK  6

/* 读数快照的三个槽：安装成功次数 / 最近返回码 / 当前窗口空间 */
static uint32 win_stat(uint32 slot)
{
    uint32 out[3];

    out[0] = 0xDEAD0001u;
    out[1] = 0xDEAD0002u;
    out[2] = 0xDEAD0003u;
    if (arkrtos_smp_sched_win_stat(out) != 0)
    {
        printf("FAIL win_stat refused\n");
        g_fails++;
        return 0u;
    }
    return out[slot];
}

/* 把上一次用例迁到次核的任务全部要回来：本模块没有 reset 接口，
 * 状态只能靠正常路径收干净。 */
static void drain_core1(void)
{
    int32 i;

    for (i = 1; i <= NTASK; i++)
    {
        if (arkrtos_smp_sched_task_back(i) == 0)
        {
            (void)arkrtos_smp_sched_switch(1u, 0x9000u);
            arkrtos_smp_sched_poll();
        }
    }
}

static void fixture(void)
{
    int32 i;

    arkrtos_task_count = NTASK;
    drain_core1();

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    memset(g_space, 0, sizeof(g_space));

    g_cpu             = 1u;
    g_install_calls   = 0u;
    g_install_last_space = 0xFFFFFFFFu;
    g_install_rc      = 0;
    g_priv_calls      = 0u;
    g_priv_last_want  = 0xFFFFFFFFu;
    g_priv_last_frame = 0;

    for (i = 0; i < NTASK; i++)
    {
        arkrtos_task_table[i].status     = ARKRTOS_TASK_READY;
        arkrtos_task_table[i].priority   = 5u;
        arkrtos_task_table[i].stack_ptr  = (unsigned long)(0x4000u + (uint32)i * 0x100u);
        arkrtos_task_table[i].is_priv    = 0u;
        /* 默认栈在恒等映射里（非窗口）：这是绝大多数内核任务的样子。 */
        arkrtos_task_table[i].stack_bottom = (uint32 *)(0x00010000u + (uint32)i * 0x1000u);
        arkrtos_task_table[i].stack_size   = 0x800u;
    }

    /* 把次核窗口带回一个已知状态（空间 0）：本模块没有 reset 接口，
     * 上一个用例装着的空间会决定这一次"要不要再装"，不定下来就等于
     * 让用例之间互相串味。走的是正常路径：迁一个绑定空间 0 的任务过去。 */
    {
        uint32 frame[4];

        g_space[5] = 0u;
        (void)arkrtos_smp_sched_task_to(6, 1u);
        (void)arkrtos_smp_sched_switch(1u, 0x8000u);
        arkrtos_smp_sched_prepare((void *)frame);
        drain_core1();
    }

    /* drain/归一 的最后一次又一次切换会留下"恢复前要做的事"的请求，
     * 在这里消费掉，否则它会算进下一个用例的第一次 prepare。 */
    {
        uint32 frame[4];

        arkrtos_smp_sched_prepare((void *)frame);
    }

    g_install_calls = 0u;
    g_priv_calls    = 0u;
    g_install_rc    = 0;
    g_priv_last_want = 0xFFFFFFFFu;
    g_priv_last_frame = 0;
}

/* ------------------------------------------------------------
 * 0. 初始化前：prepare 什么也不做，读数如实说「没装过」
 * ------------------------------------------------------------ */

static void test_before_init(void)
{
    uint32 frame[4];
    int    i;

    /* 这一条必须在任何 switch 之前跑：本模块的状态是懒建的，一旦有人切过
     * 一次，"初始化前"就不存在了。所以这里刻意不调 fixture()（它会切换）。 */

    /* 初始化前 prepare 直接返回：此刻每核状态还没建，动帧只会写坏现场。 */
    for (i = 0; i < 4; i++)
    {
        frame[i] = 0x1000u + (uint32)i;
    }
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 0u);
    CK_I(g_priv_calls, 0u);
    for (i = 0; i < 4; i++)
    {
        CK_I(frame[i], 0x1000u + (uint32)i);
    }

    /* 读数接口自己会建状态：不能因为"还没人调过 switch"就给三个 0。 */
    CK_I(win_stat(0), 0u);
    CK_I(win_stat(1), 0);
    CK_I(win_stat(2), 0xFFFFFFFFu);
    CK_I(arkrtos_smp_sched_win_stat(0), -1);
}

/* ------------------------------------------------------------
 * 1. 用户任务：装它绑定的空间，意图是用户态
 * ------------------------------------------------------------ */

static void test_user_task_window(void)
{
    uint32 frame[4];
    uint32 base;
    int    i;

    fixture();

    base = win_stat(0);

    g_space[2] = 3u;                       /* 下标 2（任务号 3）属于空间 3 */
    CK_I(arkrtos_smp_sched_task_to(3, 1u), 0);

    CK_I(arkrtos_smp_sched_switch(1u, 0xA000u), 0x4200u);
    CK_I(arkrtos_smp_sched_cur(1u), 2);

    /* 切换只登记意图，不装窗口：此刻 sp 还在被打断任务的栈上。 */
    CK_I(g_install_calls, 0u);
    CK_I(win_stat(0), base);

    for (i = 0; i < 4; i++)
    {
        frame[i] = 0x2000u + (uint32)i;
    }
    arkrtos_smp_sched_prepare((void *)frame);

    CK_I(g_install_calls, 1u);
    CK_I(g_install_last_space, 3u);
    CK_I(g_priv_calls, 1u);
    CK_I(g_priv_last_want, 1u);        /* 用户任务：want_user = 1 */
    CK(g_priv_last_frame == (void *)frame);

    CK_I(win_stat(0), base + 1u);
    CK_I(win_stat(1), 0);
    CK_I(win_stat(2), 3u);

    /* 一次性消费：同一份 pending 不许装第二次（重复装=白白冲一次 TLB）。 */
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 1u);
    CK_I(g_priv_calls, 1u);
}

/* ------------------------------------------------------------
 * 2. 装窗口失败：如实记返回码、窗口复位成「没装过」
 * ------------------------------------------------------------ */

static void test_install_failure(void)
{
    uint32 frame[4];
    uint32 base;

    fixture();

    base = win_stat(0);

    /* 空间 4 与夹具留下的窗口（空间 0）不同：不同才走 install 分支 ——
     * 这一条用例要测的正是"install 失败时账怎么记"。 */
    g_space[2] = 4u;
    CK_I(arkrtos_smp_sched_task_to(3, 1u), 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xA000u), 0x4200u);

    g_install_rc = -5;
    arkrtos_smp_sched_prepare((void *)frame);

    CK_I(g_install_calls, 1u);
    CK_I(win_stat(0), base);                       /* 没有"装成功"的账 */
    CK_I(win_stat(1), (uint32)(int32)-5);          /* 返回码如实报 */
    CK_I(win_stat(2), 0xFFFFFFFFu);                /* 复位成「没装过」 */

    /* 特权级请求与装窗口是两件事：装失败也要照常定级，否则内核任务会被
     * 以 PLV3 恢复。这里用户任务的意图已经消费掉，故仍只记一次。 */
    CK_I(g_priv_calls, 1u);
    CK_I(g_priv_last_want, 1u);        /* 用户任务：want_user = 1 */

    /* 失败后 pending 已清，再调一次不许重复装。 */
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 1u);
    CK_I(g_priv_calls, 1u);
}

/* ------------------------------------------------------------
 * 3. 特权任务的两种栈：恒等映射不装窗口、窗口内要装
 * ------------------------------------------------------------ */

static void test_priv_task(void)
{
    uint32 frame[4];
    uint32 base;

    fixture();

    /* 3a. 内核任务：栈在恒等映射里 → 不动窗口，只把 PPLV 写回 0 */
    arkrtos_task_table[3].is_priv = 1u;
    g_space[3] = 2u;
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xA000u), 0x4300u);

    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 0u);
    CK_I(g_priv_calls, 1u);
    CK_I(g_priv_last_want, 0u);        /* 内核任务：want_user = 0（回升 PLV0） */
    CK_I(win_stat(2), 0u);                  /* 没动窗口：还是夹具归一的空间 0 */

    /* 3b. 特权任务但栈在 App 窗口里（R3 实测踩过的那个形态）→ 必须装。
     *     判决用地址不用特权级：它的每一次压栈都要经窗口翻译。 */
    base = win_stat(0);

    arkrtos_task_table[4].is_priv = 1u;
    arkrtos_task_table[4].stack_bottom = (uint32 *)(WIN_VA + 0x1000u);
    arkrtos_task_table[4].stack_size   = 0x800u;
    g_space[4] = 2u;

    CK_I(arkrtos_smp_sched_task_to(5, 1u), 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xB000u), 0x4400u);

    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 1u);
    CK_I(g_install_last_space, 2u);
    CK_I(g_priv_calls, 2u);
    CK_I(g_priv_last_want, 0u);        /* 仍是内核任务 */
    CK_I(win_stat(0), base + 1u);
    CK_I(win_stat(2), 2u);
}

/* ------------------------------------------------------------
 * 4. 同一个空间不必重复装；核号越界时什么都不做
 * ------------------------------------------------------------ */

static void test_same_space_and_bad_cpu(void)
{
    uint32 frame[4];

    fixture();

    g_space[2] = 1u;
    CK_I(arkrtos_smp_sched_task_to(3, 1u), 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xA000u), 0x4200u);
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 1u);

    /* 换一个绑定到同一空间的任务：窗口已经是它，不该再装一次。 */
    g_space[3] = 1u;
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xB000u), 0x4300u);
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 1u);
    CK_I(g_priv_calls, 2u);
    CK_I(win_stat(2), 1u);

    /* port 报一个越界核号：prepare 必须直接返回，不许替别的核改窗口。 */
    g_cpu = (uint32)ARKRTOS_CPU_NUM;
    g_install_calls = 0u;
    g_priv_calls    = 0u;
    arkrtos_smp_sched_prepare((void *)frame);
    CK_I(g_install_calls, 0u);
    CK_I(g_priv_calls, 0u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_before_init();
    test_user_task_window();
    test_install_failure();
    test_priv_task();
    test_same_space_and_bad_cpu();

    if (g_fails != 0)
    {
        printf("SMP-SCHED-WIN-HOST FAIL  %d check(s), %d failure(s)\n",
               g_checks, g_fails);
        return 1;
    }
    printf("SMP-SCHED-WIN-HOST OK  %d checks\n", g_checks);
    return 0;
}
