/**
* @file main.c
* @brief Ark RTOS 在 LoongArch32 上的示例（**只编译/链接，不运行**）
* @details 与 la64 的示例逐行同构，用来证明 32 位帧布局（128 字节、4 字节槽）
*          与 LA32 指令选择能编译、能链接成一份完整的 ELF。
*
*          **没有运行过**：QEMU 9.2.3 不含 loongarch32 系统模拟器，环境里也没有
*          LA32 板子。因此下面这套「节拍/切换/SVC/唤醒」的断言在本目标上
*          全部未经执行验证——它证明的是「能构建」，不是「能跑」。保留完整
*          示例而不写成空壳，是为了让链接面与实际使用面一致：真正接上 LA32
*          载体时，第一件事就是照 la64 的方式看这四组读数。
*
*          它压的四条链路（la64 上已验证，本目标待验证）：
*          内核真正会依赖硬件的四条链路各自压一遍边：
*
*          1) **节拍**：TCFG 周期定时器每 500 us 进一次异常，
*             tick 与 ms 时间必须同步增长（读数不一致就说明节拍换算错了）；
*          2) **任务切换**：两个任务 + 空闲任务三方轮转，走「就地切换」
*             （LoongArch 没有可用软中断，见 port 文件头）完成，切换次数必须随节拍增长；
*          3) **同步原语**：信号量 create/post/wait 全部经 `syscall` 进内核，
*             这是对 syscall 指令编码解号与 ERA+4 的端到端验证；
*          4) **阻塞态唤醒**：任务用 arkrtos_task_wait() 挂进延时链，
*             到期由节拍唤醒 —— 走的是"异常里改调度状态、异常出口结算切换"
*             这条最容易出问题的组合路径。
*
*          打印全部走板级轮询 ns16550a，不依赖 libc 的 printf。
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

/* 任务栈：LoongArch 没有硬件压栈，只有软件帧，512 字（4 KB）很宽裕。
 * 栈必须是静态的（QEMU 上也没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
#define IDLE_STACK_WORDS     256u

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
static uint32 s_idle_stack[IDLE_STACK_WORDS];

static int32 s_sem = -1;

/* 统计变量：让读数本身可被观察，而不是"看起来在跑" */
static volatile uint32 s_a_loops = 0u;
static volatile uint32 s_b_loops = 0u;

/**
* @brief 任务 A：周期性打印节拍与毫秒时间，并释放一次信号量
* @details 500 ms 一个周期。两次打印之间的差值就是节拍读数，能直接看出
*          定时器是不是按 25 MHz / 500 us 在走。
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
         * 入口；在调度器内部拿 SVC 换毫秒只是多一次 syscall 往返。 */
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

        tick_prev = tick;
        ms_prev   = ms;
        round++;
        s_a_loops++;

        /* SVC 通路自检。内核任务跑在内核态，本可以直接读节拍；这里每轮
         * 仍走一次 syscall（arkrtos_get_time_ms），把 「异常 → SVC_Server →
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

/**
* @brief 进入空闲上下文并启动调度
* @details 顺序不可换：
*          1) enter_idle：把 sp 换到空闲栈（必须是汇编）；
*          2) start_timer：节拍开始走；
*          3) switch_enable：打开切换门闩并立刻请求一次就地切换，
*             第一个任务在这一步之后才开始运行；
*          4) 之后进入 WFI 循环，CPU 只在有中断时醒来。
*/
static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(unsigned long)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0xFu;      /* LoongArch 栈 16 字节对齐（ABI 硬要求） */

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

    arkrtos_port_board_init();

    arkrtos_cfg_load();

    /* 任务先注册、再开中断：irq_init() 一开 mstatus.MIE，节拍就可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

    /* 信号量表由 kernel_module_init() 里的同步模块初始化，所以在这里创建 */
    s_sem = arkrtos_sem_create_internal("la32_sem", 0);
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
    arkrtos_board_puts(" tick_period_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    if((id_a < 0) || (id_b < 0))
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
