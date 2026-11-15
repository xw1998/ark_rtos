/**
* @file main.c
* @brief Ark RTOS 在 QEMU `virt`（RISC-V RV32）上的验证示例
* @details 这不是一个"跑个空任务证明能启动"的最小例子，而是把内核真正会
*          依赖硬件的四条链路各自压一遍边：
*
*          1) **节拍**：CLINT 机器定时器每 500 us 进一次 trap，
*             tick 与 ms 时间必须同步增长（读数不一致就说明节拍换算错了）；
*          2) **任务切换**：两个任务 + 空闲任务三方轮转，靠 CLINT 软件中断
*             （PendSV 的对应物）完成，切换次数必须随节拍增长；
*          3) **同步原语**：信号量 create/post/wait 全部经 `ecall` 进内核，
*             这是对 trap 帧 a7/a0 约定与 mepc+4 的端到端验证；
*          4) **阻塞态唤醒**：任务用 arkrtos_task_wait() 挂进延时链，
*             到期由节拍唤醒 —— 走的是"trap 里改调度状态、软件中断里换栈"
*             这条最容易出问题的组合路径；
*          5) **U 模式 + PMP 内存保护**：任务 C 以 is_priv = 0 运行，
*             它读 CLINT（不在任何 PMP 窗口里）必须被打回并重启 ——
*             这条负对照是整套保护机制唯一的现场证据，不是推断。
*
*          打印全部走板级轮询 16550，不依赖 libc 的 printf。
*
* @note 启动顺序严格照 arkrtos_init.c 文件头的八步：
*       board_init → cfg_load → irq_init → kernel_module_init →
*       注册任务 → start_idle（内部再起节拍并开门闩）
* @author xw
*/

#include "arkrtos_cfg.h"
#include "arkrtos_init.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_sync.h"      /* 内核侧 *_internal 接口 */
#include "arkrtos_config.h"
#include "arkrtos.h"
#include "arkrtos_board.h"

/* 任务栈：RISC-V 没有硬件压栈，只有软件帧，512 字（2 KB）很宽裕。
 * 栈必须是静态的（QEMU 上也没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
#define IDLE_STACK_WORDS     256u

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
static uint32 s_stack_u[TASK_STACK_WORDS];
static uint32 s_idle_stack[IDLE_STACK_WORDS];

static int32 s_sem = -1;

/* 统计变量：让读数本身可被观察，而不是"看起来在跑" */
static volatile uint32 s_a_loops = 0u;
static volatile uint32 s_b_loops = 0u;

/**
* @brief 任务 A：周期性打印节拍与毫秒时间，并释放一次信号量
* @details 500 ms 一个周期。两次打印之间的差值就是节拍读数，能直接看出
*          定时器是不是按 10 MHz / 500 us 在走。
*/
static void task_a(void)
{
    uint32 tick_prev = 0u;
    uint32 ms_prev   = 0u;
    uint32 round     = 0u;

    for(;;)
    {
        uint32 tick = arkrtos_kernel_get_tick();
        /* 毫秒由节拍换算，不经 SVC：内核任务跑在内核态，SVC 是给 App 的
         * 入口；在调度器内部拿 SVC 换毫秒只是多一次 ecall 往返。 */
        uint32 ms   = (tick * ARKRTOS_TICK_PERIOD_US) / 1000u;

        arkrtos_board_puts("[A] round=");
        arkrtos_board_putu(round);
        arkrtos_board_puts(" tick=");
        arkrtos_board_putu(tick);
        arkrtos_board_puts(" d_tick=");
        arkrtos_board_putu(tick - tick_prev);
        arkrtos_board_puts(" ms=");
        arkrtos_board_putu(ms);
        arkrtos_board_puts(" d_ms=");
        arkrtos_board_putu(ms - ms_prev);
        arkrtos_board_puts("\n");

        tick_prev = tick;
        ms_prev   = ms;
        round++;
        s_a_loops++;

        /* SVC 通路自检。内核任务跑在内核态，本可以直接读节拍；这里每轮
         * 仍走一次 ecall（arkrtos_get_time_ms），把 「trap → SVC_Server →
         * 返回 a0」整条链也当成被测对象：它坏了，ms_svc 就会和 ms 对不上。 */
        {
            uint32 ms_svc = arkrtos_get_time_ms();

            arkrtos_board_puts(" ms_svc=");
            arkrtos_board_putu(ms_svc);
            arkrtos_board_puts((ms_svc + 2u < ms || ms + 2u < ms_svc) ? " SVC_MISMATCH" : " svc_ok");
            arkrtos_board_puts("\n");
        }

        /* 释放信号量：唤醒任务 B */
        (void)arkrtos_sem_post_internal(s_sem);

        /* 阻塞 500 ms：挂进延时链，由节拍唤醒 */
        arkrtos_task_wait_internal(500u);
    }
}

/**
* @brief 任务 B：等信号量，拿到就打印一次
* @details 与 A 的 500 ms 周期一一对应。B 的 round 落后 A 一拍是正常的
*          （A 先 post 再 sleep，B 被唤醒后要等下一次调度）。
*/
static void task_b(void)
{
    uint32 round = 0u;

    for(;;)
    {
        if(arkrtos_sem_wait_internal(s_sem, -1) == 0)
        {
            round++;
            s_b_loops++;

            arkrtos_board_puts("[B] woke, round=");
            arkrtos_board_putu(round);
            arkrtos_board_puts(" tick=");
            arkrtos_board_putu(arkrtos_kernel_get_tick());
            arkrtos_board_puts("\n");
        }
    }
}

/* ============================================================
 * 任务 C：U 模式负对照（内存保护自检）
 * @details 这个任务以 is_priv = 0 注册，运行在 **U 模式**。三件事各验一条：
 *          1) 打印走板级给定的外设窗（UART 4 KB RW）—— 证明窗口本身是通的，
 *             不是"什么都没给、所以什么都没跑"；
 *          2) 读毫秒时间走 arkrtos_get_time_ms()（照旧是 ecall），
 *             证明 U 模式的 trap 入口（mcause = 8）也接得住；
 *          3) 故意读 CLINT（0x02000000）—— 该地址不在任何 PMP 窗口里。
 *             M 模式读它是合法的（PMP 不管 M），U 模式读它必然被拒
 *             （mcause = 5，mtval = 目标地址）。真被拦住时下面那行
 *             "UNEXPECTED / no isolation" 永远打不出来，这是本用例的防假绿。
 *          每次故障后内核记 MEMFAULT 并把任务重启，round 接着涨，
 *          所以连续三条 [U] round=N 本身就是"拦住了 + 能恢复"的证据。
 * @note 本任务由内核注册（rom_start = 0），因此拿到的是内核代码窗 + 整片
 *       内核 RAM —— 隔离的是**特权级**（U 碰不到窗口外的地址、执行不了
 *       csrw），不是任务间隔离。任务间隔离要等 App 镜像走 loader 拿到
 *       自己的 slot，见 docs/QEMU模拟器移植与验证.md 的未完成项。
 * ============================================================ */
#define U_FAULT_ROUNDS       3u
#define RV_CLINT_PROBE_ADDR  0x02000000u

static volatile uint32 s_u_rounds = 0u;

/* port 层的 ecall 来源计数（mcause=8 来自 U，11 来自 M）。放在这里读，
 * 是为了让"这个任务真的在 U 模式"变成可读数字，而不是靠"没崩"推断。 */
extern volatile uint32 g_rv_ecall_u;
extern volatile uint32 g_rv_ecall_m;

static void task_u(void)
{
    uint32 round = s_u_rounds + 1u;

    s_u_rounds = round;

    arkrtos_board_puts("[U] round=");
    arkrtos_board_putu(round);
    arkrtos_board_puts(" t=");
    arkrtos_board_putu(arkrtos_get_time_ms());
    arkrtos_board_puts(" ms ecall_u=");
    arkrtos_board_putu(g_rv_ecall_u);
    arkrtos_board_puts(" ecall_m=");
    arkrtos_board_putu(g_rv_ecall_m);
    arkrtos_board_puts("\n");

    if(round <= U_FAULT_ROUNDS)
    {
        volatile uint32 *p_probe = (volatile uint32 *)RV_CLINT_PROBE_ADDR;
        uint32 v;

        arkrtos_board_puts("[U] reading CLINT (outside every PMP window) ...\n");

        /* 这一句是整条用例的判据：U 模式下它必须 trap，永不返回。 */
        v = *p_probe;

        arkrtos_board_puts("[U] UNEXPECTED: read=");
        arkrtos_board_puthex(v);
        arkrtos_board_puts(" -> no isolation\n");
    }

    /* 第四条负对照：写 CSR。特权级隔离除了“碰不到窗口外地址”，还有一条
     * “改不了控制寄存器”。U 模式执行任何 csr 写指令会被硬件报 mcause=2
     * （非法指令），port 层把它归入越权处理（与 PMP 拒绝同一个出口）。
     * 没接这条之前实测：U 任务只要碰一下内核内部函数就 FATAL: mcause=0x2。 */
    if(round == (U_FAULT_ROUNDS + 1u))
    {
        arkrtos_board_puts("[U] writing mstatus (privileged CSR) ...\n");

        /* 这一句必须在 U 模式 trap，永不返回。 */
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");

        arkrtos_board_puts("[U] UNEXPECTED: csrsi mstatus ok -> no isolation\n");
    }

    /* 尾部循环的两条约束，都是实测踩出来的：
     *  a) 不能用 arkrtos_task_delay() 当循环体：它是 SVC → port 忙等，而 SVC
     *     在 trap 里执行时 mstatus.MIE = 0，忙等全程关中断；实测 200 ms 忙等
     *     让节拍只按 ~5 Hz 前进、A/B 永远排不上（8 s 宿主只跑出 18 个 tick）。
     *  b) 不能用 arkrtos_task_wait_internal()：它是内核内部函数，开头就
     *     ARKRTOS_DISABLE_IRQ()（csrrc），U 模式一执行就是 mcause=2。
     * 所以走 App 侧的 SVC 接口 arkrtos_task_wait()，一次 2 ms 忙等保留下来
     * 当 SVC 通路的轻量探针（ecall_u 仍会随轮次增长），真正的让出 CPU 靠它。 */
    for(;;)
    {
        arkrtos_task_delay(2000u);
        arkrtos_task_wait(1000u);
    }
}

/**
* @brief 进入空闲上下文并启动调度
* @details 顺序不可换：
*          1) enter_idle：把 sp 换到空闲栈（RISC-V 上必须是汇编）；
*          2) start_timer：节拍开始走；
*          3) switch_enable：打开切换门闩并立刻置一次软件中断，
*             第一个任务在这一步之后才开始运行；
*          4) 之后进入 WFI 循环，CPU 只在有中断时醒来。
*/
static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0xFu;      /* RISC-V 栈 16 字节对齐（ABI 硬要求） */

    arkrtos_port_enter_idle(sp, 1u);

    arkrtos_port_start_timer(ARKRTOS_TICK_PERIOD_US);

    arkrtos_port_switch_enable();

    for(;;)
    {
        ARKRTOS_WFI();
    }
}

int main(void)
{
    int32 id_a;
    int32 id_b;
    int32 id_u;

    arkrtos_port_board_init();

    arkrtos_cfg_load();

    /* 任务先注册、再开中断：irq_init() 一开 mstatus.MIE，节拍就可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);
    id_u = arkrtos_task_register(task_u, s_stack_u, sizeof(s_stack_u), 12u, 0u);

    /* 把它降到 U 模式：内核注册出来的任务默认 is_priv = 1（见 arkrtos_cfg.c），
     * 这里显式改 0 —— 与 loader / apprun 把 App 置 0 是同一处语义，
     * 也是本目标上"特权隔离真的在起作用"的唯一入口。 */
    if(id_u > 0)
    {
        arkrtos_task_table[id_u - 1].is_priv = 0u;
    }

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

    /* 信号量表由 kernel_module_init() 里的同步模块初始化，所以在这里创建 */
    s_sem = arkrtos_sem_create_internal("qemu_sem", 0);
    if(s_sem < 0)
    {
        arkrtos_board_puts("[boot] sem_create FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    arkrtos_board_puts("[boot] sem=");
    arkrtos_board_putu((uint32)s_sem);
    arkrtos_board_puts(" taskA=");
    arkrtos_board_putu((uint32)id_a);
    arkrtos_board_puts(" taskB=");
    arkrtos_board_putu((uint32)id_b);
    arkrtos_board_puts(" taskU=");
    arkrtos_board_putu((uint32)id_u);
    arkrtos_board_puts(" mpu=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_MPU);
    arkrtos_board_puts(" priv=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_PRIV);
    arkrtos_board_puts(" tick_period_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    if((id_a < 0) || (id_b < 0) || (id_u < 0))
    {
        arkrtos_board_puts("[boot] task register FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    arkrtos_start_idle();

    return 0;
}
