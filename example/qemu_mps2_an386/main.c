/**
* @file main.c
* @brief Ark RTOS 在 QEMU `mps2-an386`（Cortex-M4）上的验证示例
* @details 这是 ARM 家族在 QEMU 上的第一档 bring-up，被测链路只有三条，
*          每一条都给出可对账的读数：
*
*          1) **节拍**：SysTick 每 500 us 进一次异常（25 MHz / 500 us =
*             12500 个计数）。任务 A 每 500 ms 打印一次 tick 与 ms，
*             `d_tick` 必须稳定为 1000、`d_ms` 稳定为 500；
*          2) **任务切换**：A / B / idle 三方轮转，靠 PendSV 完成。
*             B 只在 A post 之后才打印，所以 B 的每一行都是「一次真实切换」；
*          3) **SVC 通路**：A 每轮调一次 arkrtos_get_time_ms()，它走 SVC 指令
*             进内核、读节拍、再返回 —— 读数与紧邻的本地换算对不上就会打出
*             SVC_MISMATCH。这是对本 port 的 SVC 栈帧解析（PC-2 取立即数）
*             唯一的端到端验证。
*
*          本档 MPU / 特权分离 / 文件系统 / Shell 全关（见板级配置），所以
*          这里没有它们的用例 —— 不是"跑过了"，是"这一档不测"。
*
* @note 启动顺序严格照 arkrtos_init.h 文件头的八步：
*       board_init → cfg_load → 注册任务 → irq_init → kernel_module_init
*       → start_idle（内部再起节拍并开门闩）
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

/* Cortex-M 的硬件异常帧只有 32 字节（不带 FPU 扩展帧时），内核侧的软件帧
 * 再存 R4-R11 与 EXC_RETURN，一个任务 1 KB 已经很宽裕；取 2 KB 与其它
 * QEMU 目标同档，避免"换个目标就换一套余量"的口径漂移。 */
#define TASK_STACK_WORDS     512u
#define IDLE_STACK_WORDS     256u

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
static uint32 s_idle_stack[IDLE_STACK_WORDS];

static int32 s_sem = -1;

/* 让读数本身可被观察，而不是"看起来在跑" */
static volatile uint32 s_a_rounds = 0u;
static volatile uint32 s_b_rounds = 0u;

/**
* @brief 任务 A：周期性打印节拍与毫秒时间，并释放一次信号量
*/
static void task_a(void)
{
    uint32 tick_prev = 0u;
    uint32 ms_prev   = 0u;
    uint32 round     = 0u;

    for(;;)
    {
        uint32 tick = arkrtos_kernel_get_tick();
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
        s_a_rounds++;

        /* SVC 通路自检：两侧都紧贴同一时刻取值，容差只需要 ±1 ms。
         * 中间若隔一整行打印，16550 上一轮就有 2~3 ms，会打出假失配。 */
        {
            uint32 ms_now = (arkrtos_kernel_get_tick() * ARKRTOS_TICK_PERIOD_US) / 1000u;
            uint32 ms_svc = arkrtos_get_time_ms();

            arkrtos_board_puts(" ms_svc=");
            arkrtos_board_putu(ms_svc);
            arkrtos_board_puts((ms_svc + 1u < ms_now || ms_now + 1u < ms_svc)
                               ? " SVC_MISMATCH" : " svc_ok");
            arkrtos_board_puts("\n");
        }

        (void)arkrtos_sem_post_internal(s_sem);

        /* 阻塞 500 ms：挂进延时链，由节拍唤醒 */
        arkrtos_task_wait_internal(500u);
    }
}

/**
* @brief 任务 B：等信号量，拿到就打印一次
* @details B 的 round 落后 A 一拍是正常的（A 先 post 再 sleep）。
*/
static void task_b(void)
{
    uint32 round = 0u;

    for(;;)
    {
        if(arkrtos_sem_wait_internal(s_sem, -1) == 0)
        {
            round++;
            s_b_rounds++;

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
*          1) enter_idle：把 sp 换到空闲栈（Cortex-M 上是切 PSP + CONTROL）；
*          2) start_timer：节拍开始走；
*          3) switch_enable：打开切换门闩并立刻置一次 PendSV，
*             第一个任务在这一步之后才开始运行；
*          4) 之后进入 WFI 循环。
*/
static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0x7u;      /* Cortex-M 要求 8 字节对齐（异常压栈时还会再对齐到 8） */

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 必须在 enter_idle 之前布防：这一刻 idle 栈还没被当作栈用，
     * 整段填图案是安全的；进入之后 main 的活帧就搬到这块栈上了。 */
    arkrtos_stack_guard_watch_context((uint32)(&s_idle_stack[0]),
                                      (uint32)sizeof(s_idle_stack), sp);
    #endif

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

    /* 任务先注册、再开中断：irq_init() 之后节拍随时可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

    s_sem = arkrtos_sem_create_internal("mps2", 0);

    arkrtos_board_puts("\n[ark] mps2-an386 cortex-m4 bring-up\n");
    arkrtos_board_puts("[ark] task_a=");
    arkrtos_board_putu((uint32)id_a);
    arkrtos_board_puts(" task_b=");
    arkrtos_board_putu((uint32)id_b);
    arkrtos_board_puts(" sem=");
    arkrtos_board_putu((uint32)s_sem);
    arkrtos_board_puts(" sysclk=");
    arkrtos_board_putu(arkrtos_port_get_system_clock());
    arkrtos_board_puts(" tick_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    arkrtos_start_idle();

    return 0;
}
