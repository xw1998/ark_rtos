/**
* @brief Ark RTOS 硬件抽象层接口
* @details 定义内核与硬件的完整解耦接口，分为四个层次：
*          1) CPU指令层 - CPU核心指令封装（WFI/ISB/DSB等）
*          2) 上下文层 - 任务上下文结构与栈帧初始化
*          3) 中断控制层 - 中断开关、优先级、任务切换触发
*          4) 定时器层 - 系统节拍定时器与微秒延时
*          5) 板级层 - 板卡初始化与设备注册
*
*          所有架构相关实现均在 port/ 目录下，
*          内核代码不包含任何MCU头文件，不直接操作任何寄存器。
* @author xw
* @date 2026.05.03
*/

#ifndef __ARKRTOS_HAL_H__
#define __ARKRTOS_HAL_H__

#include "arkrtos_types.h"
#include "arkrtos_config.h"

/* ============================================================
 * CPU指令层
 * @brief CPU核心指令封装，由port层实现
 * ============================================================ */
void arkrtos_port_wfi(void);
void arkrtos_port_wfe(void);
void arkrtos_port_nop(void);
void arkrtos_port_isb(void);
void arkrtos_port_dsb(void);
void arkrtos_port_dmb(void);

#define ARKRTOS_WFI()            arkrtos_port_wfi()
#define ARKRTOS_WFE()            arkrtos_port_wfe()
#define ARKRTOS_NOP()            arkrtos_port_nop()
#define ARKRTOS_ISB()            arkrtos_port_isb()
#define ARKRTOS_DSB()            arkrtos_port_dsb()
#define ARKRTOS_DMB()            arkrtos_port_dmb()

/* ============================================================
 * 中断控制层
 * @brief 中断开关与任务切换触发，由port层实现
 * ============================================================ */
void arkrtos_port_disable_irq(void);
void arkrtos_port_enable_irq(void);
void arkrtos_port_switch_task(void);
/* Is the CPU currently executing inside an exception (IRQ or SVC)? 1 = yes.
 * A user-mode waiter can never yield inside an SVC handler (PendSV cannot
 * preempt SVC), so sync primitives take the "register only + WOULDBLOCK"
 * path for them and let the user-mode wrapper do the yielding. */
uint8 arkrtos_sync_in_handler(void);

/**
* @brief 在异常处理程序内部直接恢复目标任务上下文并异常返回
* @param stack_ptr 目标任务栈指针（指向其保存的 R4-R11[/EXC_RETURN]）
* @details 故障恢复等“已经处于异常处理程序中、不能依赖 PendSV”的场景专用：
*          Cortex-M 上 HardFault 优先级高于 PendSV，故障处理程序活动期间
*          PendSV 不会被服务，必须由本函数直接完成恢复 + 异常返回。
*          本函数不返回；stack_ptr 为 0 时调用方应自行停机，不要调用。
*/
void arkrtos_port_resume_task(unsigned long stack_ptr);

uint8  arkrtos_port_in_isr(void);
uint32 arkrtos_port_syscall_num(void *p_exc_ctx);

/**
* @brief 读取系统调用上下文中的第 idx 个参数（0~3）
* @param p_exc_ctx 系统调用入口传给内核的上下文指针
* @param idx       参数序号（0 对应 a0/r0）
* @return 参数值
* @details 各架构的栈帧/trap 帧布局不同，由 port 层负责解析，
*          内核只通过本接口与上面的宏访问参数，不感知任何寄存器布局。
*/
unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx);

/**
* @brief 写回系统调用返回值（第 0 个参数寄存器）
* @param p_exc_ctx 系统调用上下文指针
* @param value     返回值
*/
void   arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value);

#if (ARKRTOS_USE_LINUX_ABI == 1)
/**
* @brief 本次异常是否是 Linux ABI 系统调用，是则返回它的系统调用号
* @param p_exc_ctx 系统调用入口传给内核的上下文指针
* @return Linux 系统调用号；不是 Linux ABI 调用时返回 0xFFFFFFFF
* @details 两个 ABI 在同一颗核上用同一条陷入指令，只有号的位置不同：
*          - LoongArch：Ark 的号在 `syscall` 立即数里（非 0），Linux 用
*            `syscall 0` + $a7，因此「立即数为 0」是分界；
*          - RISC-V：两边都经 a7，只能按号段让位（0x10..0x22 归 Ark）。
*          - Cortex-M / LA32：恒返回 0xFFFFFFFF（没有 Linux ABI 载体）。
*          由 port 层判定，内核不感知任何寄存器布局。
*/
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx);

/**
* @brief 按 Linux 约定写回返回值（第 0 个参数寄存器，带符号扩展）
* @param p_exc_ctx 系统调用上下文指针
* @param value     返回值；负值表示 -errno
* @details 与 arkrtos_port_svc_set_ret 的区别只有符号扩展：Linux 的错误
*          是负数，64 位下若只写低 32 位，程序会把 0x00000000FFFFFFEA
*          读成一个很大的正数。宁可让返回值正确，也不给看似成功的错答案。
*/
void   arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value);

/**
* @brief 读取用户态 trap 帧里的一个寄存器（信号帧保存/恢复用）
* @param p_exc_ctx 系统调用入口传给内核的上下文指针
* @param id        寄存器编号：0 = 返回地址（era / pc）、1 = 栈指针、
*                  2 = 链接寄存器（ra / lr）、3 = 线程指针（tp）
* @return 寄存器当前值；框架不认识该编号时返回 0
* @details 与 ARKRTOS_SVC_ARG 同一口径：寄存器布局只在 port 层解析，
*          内核只按语义编号取值。信号投递要在用户栈上铺一帧、把
*          「被打断的位置」原样存下来，这一对原语就是那条缝。
*/
unsigned long arkrtos_port_svc_user_reg(void *p_exc_ctx, uint32 id);

/**
* @brief 写回用户态 trap 帧里的一个寄存器（信号投递/返回用）
* @param p_exc_ctx 系统调用上下文指针
* @param id        寄存器编号，同 arkrtos_port_svc_user_reg
* @param value     新值
* @return 0 = 已写入；-1 = 编号不可用或端口没有这个能力（一个字节都不动）
* @details 端口没有该能力时由 arkrtos_linux_abi.c 里的弱定义顶住，
*          信号相关的号一律回 -ENOSYS，不假装投递成功。
*/
int32 arkrtos_port_svc_set_user_reg(void *p_exc_ctx, uint32 id,
                                    unsigned long value);

/**
* @brief trap 帧在内存里的字节数（信号快照用）
* @return 帧长；含 FPU 时是 528，LSX 档 1040，默认档 256
* @details 信号要「原样保存被打断的现场」，而现场不止四个寄存器：把
*          帧整块存进用户栈，返回时整块装回去，才不会被处理函数打断
*          一半的调用约定（$t0 一类的临时寄存器）害死。
*/
uint32 arkrtos_port_svc_frame_bytes(void);

/**
* @brief 在用户栈上给信号帧找一块**不和 trap 帧重叠**的地方
* @param p_exc_ctx 系统调用上下文指针
* @param bytes     这一帧要多少字节（头 + 快照）
* @return 帧基址（16 字节对齐）；栈不够放时返回 0
* @details 内核的 trap 帧就铺在用户 sp 下面 LA_FR_SIZE 字节处，这一块
*          是内核的、不是用户栈的「空闲区」。信号帧若从 sp 直接往下放
*          就会整块盖在 trap 帧上——实测症状是返回时 frame 里的 $t5 变成
*          了信号 magic，随后一条 ret 跳到 0x210（= 帧长 528）。所以
*          让端口算这个地址：帧长、对齐、保留区都只在端口里出现一次。
*/
unsigned long arkrtos_port_svc_frame_reserve(void *p_exc_ctx, uint32 bytes);

/**
* @brief 把整份 trap 帧拷进内核缓冲（信号投递用）
* @param p_exc_ctx 系统调用上下文指针
* @param p_dst     目标缓冲，至少 arkrtos_port_svc_frame_bytes() 字节
* @details 只做整块拷贝，不改帧。
*/
void arkrtos_port_svc_frame_export(void *p_exc_ctx, void *p_dst);

/**
* @brief 把内核缓冲里的帧整块装回当前上下文（rt_sigreturn 用）
* @param p_exc_ctx 系统调用上下文指针
* @param p_src     源缓冲，至少 arkrtos_port_svc_frame_bytes() 字节
* @return 0 = 已装回；-1 = 端口没有这个能力
* @details 唯一被丢弃的字段是 PRMD 的特权级两位：特权级由当前上下文
*          决定，用户写在栈上的快照不许把它抬高（否则一个用户程序可以
*          自己铺一帧 PRMD=PLV0 再 sigreturn）。
*/
int32 arkrtos_port_svc_frame_import(void *p_exc_ctx, const void *p_src);

/**
* @brief 把当前任务的 trap 帧改写成「新镜像的第一条指令」（execve 的落地）
* @param p_exc_ctx 当前系统调用的上下文指针
* @param entry     新入口（必须在 App 窗口内、4 字节对齐）
* @param sp        新用户栈指针（指向 argc）
* @param tp        新镜像的 TLS 块地址（0 = 这份镜像没有预置 TLS）
* @return 0 = 帧已改写；-1 = 参数不可用（此时一个字节都不动）
* @details 这张帧就是 ertn 要恢复的那一张：改完之后内核直接返回用户态，
*          **不再回到旧镜像**。通用寄存器一律清零（Linux 在 execve 之后
*          不保留任何寄存器），PRMD 只留 PPLV、把 PIE 置 1。
*          port 没有「同任务换镜像」能力时（目前 RISC-V / Cortex-M / LA32），
*          由 arkrtos_linux_abi.c 里的弱定义顶住，execve 回 -ENOSYS。
*/
int32  arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entry,
                                   unsigned long sp, unsigned long tp);

/**
* @brief 把当前系统调用「退回重放」（ERA 退回一条指令）
* @param p_exc_ctx 系统调用上下文指针
* @return 0 = 已退回，被唤醒后从用户态重新发起同一次调用；
*         -1 = 本 port 不支持（调用方必须改走非阻塞语义，不许假装睡过）
* @details 阻塞形态的 wait4 需要它：本内核在 SVC 处理程序里没有真正睡眠的
*          通路（置 WAIT + 请求切换，切换在异常出口结算，函数本身立刻返回），
*          所以「等子进程退出」只能做成「睡下 -> 被唤醒 -> 重放这条系统调用
*          -> 重新扫一遍」。这与 Linux 的可重启系统调用（ERESTARTSYS）是
*          同一手法：参数寄存器在帧里本来就没被改写，重放读到的就是同一组值。
*/
int32  arkrtos_port_svc_restart(void *p_exc_ctx);
#endif


/* 系统调用上下文统一访问宏：内核代码只使用这三个宏 */
#define ARKRTOS_SVC_ARG(p_ctx, idx)    arkrtos_port_svc_get_arg((void *)(p_ctx), (uint32)(idx))
#define ARKRTOS_SVC_RET(p_ctx, val)    arkrtos_port_svc_set_ret((void *)(p_ctx), (uint32)(val))
#define ARKRTOS_SVC_NUM(p_ctx)         arkrtos_port_syscall_num((void *)(p_ctx))

#define ARKRTOS_DISABLE_IRQ()    arkrtos_port_disable_irq()
#define ARKRTOS_ENABLE_IRQ()     arkrtos_port_enable_irq()
#define ARKRTOS_SWITCH_TASK()    arkrtos_port_switch_task()

/**
* @brief 进入临界区（保存中断使能状态并关中断）
* @return 进入前的中断状态，必须原样传给 arkrtos_port_exit_critical
* @details 与 ARKRTOS_DISABLE_IRQ 的区别：本接口保存并恢复状态，
*          支持嵌套，且适配 RISC-V/LoongArch 等需要保存状态寄存器的架构。
*          新代码建议优先使用本接口。
*/
uint32 arkrtos_port_enter_critical(void);

/**
* @brief 退出临界区（恢复此前保存的中断状态）
* @param state arkrtos_port_enter_critical 的返回值
*/
void   arkrtos_port_exit_critical(uint32 state);

#define ARKRTOS_ENTER_CRITICAL()       arkrtos_port_enter_critical()
#define ARKRTOS_EXIT_CRITICAL(state)   arkrtos_port_exit_critical(state)

/* ============================================================
 * 原子操作与多核支持
 * @brief 自旋锁（arkrtos_spin.h）依赖的架构相关原语，由 port 层实现：
 *        - ARM Cortex-M：LDREX/STREX 独占访问
 *        - RISC-V：AMO 原子指令或 LR/SC 指令对
 *        - LoongArch：LL/SC 指令对
 *        单核移植可仅返回成功，但为将来 SMP 扩展建议直接实现原子语义。
 * ============================================================ */

/**
* @brief 比较并交换（原子操作）
* @param p_addr    目标地址（必须 4 字节对齐）
* @param expect    期望的当前值
* @param new_value 期望成立时写入的新值
* @return 1=交换成功，0=当前值与 expect 不符（未修改）
*/
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value);

/**
* @brief 获取当前 CPU 编号（单核恒返回 0，多核返回硬件核号）
* @return CPU 编号
*/
uint32 arkrtos_port_cpu_id(void);

/**
* @brief 自旋等待提示（降低自旋总线压力，可插入 NOP/WFE/PAUSE）
*/
void   arkrtos_port_spin_hint(void);

#if (ARKRTOS_USE_SMP == 1)
/**
* @brief Start a secondary CPU (SMP scaffolding only).
* @param cpu hardware core id, 1..ARKRTOS_CPU_NUM-1
* @return 0 = start request accepted; -1 = this port has no secondary-core path
* @note A single-core port returns -1 rather than pretending the core is up.
*       The started core must enter arkrtos_smp_secondary_main(cpu) as its
*       first C code, with its stack already in place.
*/
int32  arkrtos_port_cpu_start(uint32 cpu);

/**
* @brief Send an inter-processor interrupt (SMP scaffolding only).
* @param cpu target core id, already checked online by the caller
* @return 0 = sent; -1 = this port has no IPI path
*/
int32  arkrtos_port_send_ipi(uint32 cpu);
#endif /* ARKRTOS_USE_SMP == 1 */


/* ============================================================
 * 上下文层
 * @brief 任务上下文操作，由port层实现
 *        不同CPU架构的寄存器布局、栈帧格式、
 *        上下文保存/恢复方式完全不同，
 *        全部由port层定义和实现。
 * ============================================================ */

/**
* @brief 设置进程栈指针PSP
* @param val PSP值
*/
void arkrtos_port_set_psp(uint32 val);

/* ============================================================
 * 任务热切换（A/B 双缓冲）的端口层
 * @brief 内核只认这四个入口，帧形状与恢复路径按架构各自实现
 * @details 热切换做的就是「改写一份硬件栈帧，让异常返回落到新入口」。而
 *          「栈帧长什么样、从哪恢复到哪」是每个架构自己的事——Cortex-M 是
 *          PSP 上 8 个字的基本帧（打开 FPU 后，用过 FPU 的任务是 104 字节的
 *          扩展帧），RISC-V 是 mepc/mstatus 收尾的一段，LoongArch 是
 *          ERA/PRMD 收尾的一段。内核只调下面三个函数，内核里不出现任何
 *          「按架构改寄存器」的分支。
 *          声明与定义都只在 ARKRTOS_USE_TASK_SWAP == 1 时存在。
 * ============================================================ */
#if (ARKRTOS_USE_TASK_SWAP == 1)

/**
* @brief 这一帧占多少字节；0 = 本架构不认识这一帧（拒绝而不是猜）
* @param p_exc_ctx SVC/trap 的帧指针
* @param avail     p_exc_ctx 到旧块末尾还剩多少字节（越界时调用方给 0）
* @return 32 / 104（Cortex-M 基本帧 / FPU 扩展帧）、144（RV32IMAC）、
*         256（LA64）；0 = 形状不认识或放不下一帧
* @details 形状判断与「一帧多大」是同一件事，所以只留这一个入口：内核拿它
*          同时做「是不是这个任务的帧」与「边界在哪」两步判断，端口层不必
*          把同一份判据写两遍。
*/
uint32 arkrtos_port_swap_frame_bytes(const void *p_exc_ctx, uint32 avail);

/**
* @brief 把帧就地改写成「新镜像入口的第一行」
* @param p_exc_ctx 帧指针（已经在新的数据块里）
* @param entry     新入口地址
* @details 只改与「从哪里取指、用哪段栈、以什么特权级返回」有关的那些字，
*          其余寄存器按架构语义保留或清零，见各端口的实现注释。
*/
void arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry);

/**
* @brief 让「本次异常返回」用新帧
* @param p_old_ctx 旧帧（换下来的那一份）
* @param p_new_ctx 新帧（改写好的那一份）
* @details Cortex-M 把 PSP 挪过去；RISC-V / LoongArch 的恢复帧是 trap C 入口
*          的返回值，端口在这里记账，由各自的 trap 出口取走。
*/
void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx);

#endif /* ARKRTOS_USE_TASK_SWAP == 1 */

/**
* @brief 获取CONTROL寄存器值
* @return CONTROL寄存器当前值
*/
uint32 arkrtos_port_get_control(void);

/**
* @brief 设置CONTROL寄存器值
* @param val 要写入的值
*/
void arkrtos_port_set_control(uint32 val);

/* Set the Thread-mode privilege of the context that resumes when the
 * current exception returns. Must be called from Handler mode (the switch
 * path is the only caller): CONTROL.nPRIV is writable there and takes
 * effect on the return to Thread mode. CONTROL.SPSEL is deliberately left
 * alone - the hardware restores it from EXC_RETURN, not from CONTROL.
 * @param is_priv 1 = privileged Thread mode, 0 = unprivileged
 */
void arkrtos_port_set_thread_priv(uint32 is_priv);

/**
* @brief 初始化任务栈帧
* @param stack_top  栈顶地址（高地址，已8字节对齐）
* @param entry      任务入口函数
* @return           初始化后的栈指针（将存入task->stack_ptr）
* @details 不同架构的异常栈帧格式不同：
*          - ARM Cortex-M: 压入xPSR/PC/LR/R12/R3-R0(硬件) + R4-R11(软件)
*          - RISC-V: 压入mepc/mstatus/x1-x31
*          此函数由port层实现，内核无需关心具体布局
*/
unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void));

#if (ARKRTOS_USE_ELF_TLS == 1)
/**
* @brief 把初始栈帧里的线程指针（TLS 用）写成指定值
* @param stack_ptr arkrtos_port_stack_init() 返回的帧指针
* @param tp        TLS 块起始地址
* @details 只有实现了「TP 写入」的端口才有这个函数（目前是 LoongArch64，
*          写的是帧里 $r2 的槽）。装载器在 ARKRTOS_USE_ELF_TLS=1 且镜像带
*          PT_TLS 时调用它；关档时这个声明与实现都不存在，端口体积不变。
*          其它端口打开 ARKRTOS_USE_ELF_TLS 是编译期错误，不是空操作。
*/
void arkrtos_port_task_set_tp(unsigned long stack_ptr, unsigned long tp);
#endif

/**
* @brief 切换到空闲任务上下文
* @details 设置PSP/CONTROL寄存器，切换到PSP栈模式
*          不同架构的特权级切换机制不同，由port层实现
* @param psp        空闲任务栈顶地址
* @param use_priv   是否使用特权分离模式
*/
void arkrtos_port_enter_idle(unsigned long stack_ptr, uint32 use_priv);

/**
* @brief Enable task switching and request the first switch
* @details Must be called after arkrtos_port_enter_idle() (CPU on PSP) and
*          arkrtos_port_start_timer(). Switching requests issued before this
*          call are ignored on purpose.
*/
void   arkrtos_port_switch_enable(void);

/* ============================================================
 * 定时器层
 * @brief 系统节拍定时器与微秒延时，由port层实现
 * ============================================================ */

/**
* @brief 获取系统主频(Hz)
* @return 系统主频，单位Hz
* @details 不同平台获取方式不同：
*          - STM32: SystemCoreClock变量
*          - 其他MCU: 可能是宏或函数
*          内核通过此接口统一获取，不直接依赖SystemCoreClock
*/
uint32 arkrtos_port_get_system_clock(void);

/**
* @brief 获取系统节拍定时器当前计数值（自由递减计数器）
* @return 当前计数值
* @details ARM 为 SysTick->VAL，RISC-V 可为 mtime 低 32 位，
*          LoongArch 可为恒定频率定时器计数，由 port 层映射。
*/
uint32 arkrtos_port_get_timer_counter(void);

/* Free-running CPU cycle counter (DWT CYCCNT), for latency measurement.
 * arkrtos_port_cyccnt_init() returns 0 when the core can serve it, -1 when
 * it cannot (then arkrtos_port_get_cyccnt() returns 0 forever and callers
 * must report "not available" instead of inventing a resolution that is
 * three orders of magnitude coarser).  Called once, at ARKRTOS_USE_RTSTAT
 * init; nothing else in the kernel reads these two unless that switch is on. */
int32  arkrtos_port_cyccnt_init(void);
uint32 arkrtos_port_get_cyccnt(void);

/**
* @brief 周期计数器的频率（Hz），用来把周期差折算成时间
* @return 每秒多少个 get_cyccnt() 计数；0 = 未知（调用方必须报"不可用"）
* @details ARM 上 DWT CYCCNT 与内核时钟同源，返回 get_system_clock()；
*          RISC-V / LoongArch 上 mcycle / TSC 与节拍时基（mtime / rdtime）
*          不同源，频率由 cyccnt_init() 在开机时用已知时基实测校准，
*          所以这里返回的是"量出来的比值"，不是抄一个常数。
*          0 是合法返回值：校准失败就报不可用，不得拿节拍频率顶替
*          （两者差一个数量级，会把所有微秒读数算错）。
*/
uint32 arkrtos_port_get_cycle_clock(void);

/**
* @brief 周期计数器频率的来源
* @return 0 = 板级常量（ARM：与内核时钟同源）；1 = 运行期实测校准
*/
int32  arkrtos_port_cycle_clock_measured(void);

/**
* @brief 获取系统节拍定时器重载值（计数上限）
* @return 重载值
* @details ARM 为 SysTick->LOAD，其他架构为等效周期值。
*/
uint32 arkrtos_port_get_timer_reload(void);

/**
* @brief 启动系统节拍定时器
* @param tick_period_us 节拍周期（微秒）
*/
void arkrtos_port_start_timer(uint32 tick_period_us);

/**
* @brief 以原始计数编程节拍定时器（tickless 用）
* @param counts 一个"长节拍"的计数值（= n 个正常节拍）
* @details 与 arkrtos_port_start_timer(us) 的区别只在于单位：这里给的是
*          时钟计数，port 层不再做微秒换算，避免 n*TICK_PERIOD_US 的整数
*          除法丢相位。计数值必须落在定时器自身位宽内（Cortex-M 的 SysTick
*          是 24 位），越界由调用者负责（arkrtos_tickless_idle 会先夹住）。
*/
void arkrtos_port_start_timer_counts(uint32 counts);

/**
* @brief 读取并清除节拍定时器的"计到过 0"标志
* @return 1 = 自上次读取以来计数器到过 0；0 = 没有
* @details ARM 上是 SysTick->CTRL 的 COUNTFLAG（读 CTRL 会顺带清它）。
*          tickless 用它区分"睡满了整段"和"被别的中断提前叫醒"：前者
*          计数器已经回卷，后者没有。没有这个位的架构返回 0，调用方按
*          "提前唤醒"处理（结果仍然正确，只是少省一次）。
*/
uint32 arkrtos_port_timer_countflag_take(void);

/**
* @brief 一次 tickless 睡眠最多能编程多少个计数
* @return 定时器位宽允许的上限（ARM SysTick = 24 位，RISC-V mtimecmp 用
*          32 位的一半，LoongArch TCFG = 30 位）
* @details tickless 用它夹住睡眠长度。把上限放在 port 层，是因为"计数器
*          位宽"和"有没有回卷标志"都是 port 的私事；内核那边只关心
*          "这次能睡多长"。
*/
uint32 arkrtos_port_timer_sleep_max(void);

/**
* @brief 自上次 arkrtos_port_start_timer_counts() 以来实际走过了多少个计数
* @param programmed 当时编程下去的计数值
* @return 已走过的计数（>= 1）；计数器不动时返回 1
* @details ARM 用 SysTick 的 COUNTFLAG + VAL（24 位）；RISC-V 的 mtimecmp
*          是"比较值"、没有回卷标志，所以 port 在编程时留一个时基快照，
*          用差值算；LoongArch 的 TCFG 是周期定时器，同理用 rdtime 快照
*          按两个时钟的频率比折算成 TCFG 计数。返回值允许超过 programmed
*          （被拖得太久），调用方按整拍补。
*/
uint32 arkrtos_port_timer_elapsed_counts(uint32 programmed);


/**
* @brief 微秒级忙等延时
* @param us 延时微秒数
* @details 使用硬件定时器实现精确延时，
*          由port层根据具体定时器实现
*/
void arkrtos_port_delay_us(uint32 us);

/* ============================================================
 * Watchdog
 * @brief Independent watchdog, implemented by the board layer
 * ============================================================ */

/**
* @brief Start the independent watchdog with an approximate timeout.
* @param timeout_ms Nominal timeout in milliseconds (0 = reject).
* @return 0 on success, -1 when the port has no watchdog or cannot
*         represent the requested timeout.
* @details Called at most once, by whoever owns the decision to enable it
*          (ARKRTOS_WDG_ENABLE). On STM32 the IWDG cannot be stopped once it
*          runs, so a wrong timeout here is not recoverable in software.
*          The timeout the hardware actually got is reported by
*          arkrtos_port_wdg_timeout_ms() - callers must read that rather than
*          assume the number they asked for.
*/
int32 arkrtos_port_wdg_init(uint32 timeout_ms);

/**
* @brief Reload the watchdog counter.
* @details Deliberately does nothing else: the kernel guard decides *whether*
*          to feed, this only performs the register write.
*/
void arkrtos_port_wdg_feed(void);

/**
* @brief Timeout the watchdog is actually running with (0 = not started).
*/
uint32 arkrtos_port_wdg_timeout_ms(void);


/* ============================================================
 * 板级层
 * @brief 板卡初始化与设备注册，由board层实现
 * ============================================================ */

/**
* @brief 板卡硬件初始化
* @details 初始化CPU协处理器（如FPU）、时钟等
*/
void arkrtos_port_board_init(void);

/**
* @brief 中断控制器初始化
* @details 配置NVIC优先级分组，设置PendSV/SysTick/SVC优先级
*/
void arkrtos_port_irq_init(void);

/**
* @brief 设置空闲任务MPU区域
* @details 当 ARKRTOS_USE_MPU=1 时由port层实现
* @param task_func 任务函数地址
* @param stack_addr 栈地址
* @param stack_size 栈大小
*/
void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);

/* ============================================================
 * MPU接口层
 * @brief MPU区域配置，由port层实现
*        当ARKRTOS_USE_MPU=0时，这些函数被宏替换为空
 * ============================================================ */
#if (ARKRTOS_USE_MPU == 1)

void arkrtos_port_mpu_init(void);
void arkrtos_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void arkrtos_port_mpu_set_app(const arkrtos_arch_mpu_t *p_mpu);
void arkrtos_port_mpu_reset(void);

/* Encode one region into the architecture independent context.
 * @param p_mpu target context
 * @param idx   region index (0 .. ARKRTOS_MPU_REGION_MAX-1)
 * @param base  region base address (rounded down to the region size)
 * @param size  bytes to cover (rounded up to a power of two)
 * @param mem   memory attribute kind
 * @return 0 on success, -1 on bad parameters */
int32 arkrtos_port_mpu_encode(arkrtos_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size,
                            arkrtos_mpu_mem_t mem);

/* Re-apply the idle-task context captured by arkrtos_port_mpu_set_region(). */
void arkrtos_port_mpu_set_idle(void);

#else

#define arkrtos_port_mpu_init()
#define arkrtos_port_mpu_set_region(rom_addr, rom_size, ram_addr, ram_size)
#define arkrtos_port_mpu_set_app(p_mpu)
#define arkrtos_port_mpu_reset()
#define arkrtos_port_mpu_encode(p_mpu, idx, base, size, mem)   (-1)
#define arkrtos_port_mpu_set_idle()

#endif

/* ============================================================
 * 分页隔离后端（MMU）
 * @brief 当 ARKRTOS_USE_PAGING=1 时，隔离契约（arkrtos_mpu_* 五个入口）
 *        由 arkrtos_paging.c 实现，底层落到这四个 port 接口。
 *        窗口后端（arkrtos_port_mpu_*）此时不参与编译，两者互斥，
 *        由 arkrtos_config.h 的三条 #error 拦下。
 *
 *        与窗口后端的区别必须写清楚：窗口后端"改一次寄存器就换了权限"，
 *        分页后端动的是地址翻译，撤销一段映射的代价远大于改寄存器。
 *        因此这里的 check() 是**审计**语义（这段跨度有没有映射、权限够不够），
 *        返回 -1 表示"这段地址在这套静态映射下过不去"，调用方负责计数上报，
 *        不得把它当成可通过。
 * ============================================================ */
#if (ARKRTOS_USE_PAGING == 1)

/** @brief 建立/确认分页映射
 *  @return 0 = 映射可用；-1 = 本 port 不具备分页通路
 *  @details LoongArch 的静态映射在 la_mmu_init() 里建好（开中断之前），
 *           本函数只报告那份映射是否已经生效，不重建它。 */
int32 arkrtos_port_paging_init(void);

/** @brief 审计一段地址跨度
 *  @param base 起始虚拟地址
 *  @param size 字节数
 *  @param mem  期望的内存属性（取自 arkrtos_mpu_mem_t）
 *  @return 0 = 已映射且权限满足；-1 = 未映射 / 权限不足 / 该属性本 port 无对应窗口 */
int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrtos_mpu_mem_t mem);

/** @brief 追加一条动态映射（VA -> PA）
 *  @return 0 = 已建立；-1 = 本 port 未实现
 *  @details 动态映射是登记欠账，这里不假装支持。 */
int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem);

/** @brief 撤销一条动态映射（U-15）
 *  @param va 登记的虚拟地址（4 KB 对齐）
 *  @return 0 = 已撤销（该页条目写成无效，同对另一页保持原样）；
 *          -1 = 未 4 KB 对齐；-2 = 落在静态跨度里（静态映射从不撤销）；
 *          -4 = 这个 VA 本来就没登记过
 *  @details 撤销靠「按同一个绝对索引把这条条目重写空」完成：动态条目落
 *           MTLB 的绝对槽位，所以不需要 invtlb —— 覆盖后旧翻译就不存在了。
 *           两半都空了才把槽位标成空闲，否则只清被撤的那一半。 */
int32 arkrtos_port_paging_unalias(uint32 va);

/** @brief 当前占用了几条动态映射槽位（pmap 读数用）
 *  @return 已占用槽位数（0..上限） */
uint32 arkrtos_port_paging_dyn_used(void);

/** @brief 让一段地址的翻译失效
 *  @details 静态映射从不撤销，所以本 port 是空操作；留这个口是为了让
 *           将来真做动态重映射时有地方落，而不是到时候改契约。 */
void arkrtos_port_paging_flush(uint32 base, uint32 size);

/** @brief 把一个 App 地址空间装进 App 窗口（U-1）
 *  @param space 0 .. arkrtos_port_paging_space_count()-1
 *  @return 0 = 已是该空间或切换成功；-1 = 空间号越界 / 本 port 无 TLB 通路
 *  @details 本 port 没有硬件页表遍历，所以「每进程页表」= 每个 App 一组
 *           条目、切换时重写 App 窗口那一条 PS=20 条目。空间号即帧号：
 *           space 0 是 PA = ARKRTOS_QEMU_APP_BASE（行为与之前完全一致），
 *           space n 是 ARKRTOS_QEMU_APP_BASE + n * APP_SIZE。
 *           内核的恒等映射与 UART 条目一条都不动。 */
int32 arkrtos_port_paging_activate(uint32 space);

/** @brief 调度器用的「装进窗口」：必要时推迟到恢复现场那一刻（U-2）
 *  @param space 目标空间号
 *  @return 0 = 已装上 / 已记下待装；-1 = 空间号越界 / 本 port 无 TLB 通路
 *  @details 与 activate() 只差一条，但那条是致命的：调度器调它的时候内核
 *           还跑在**被换下任务自己的栈上**，而 App 任务的栈就在 App 窗口里。
 *           此刻重写窗口条目，紧接着的一次 ld.d / 函数返回就会从另一个 App
 *           的帧里取值 —— 实测症状是两个 hello 并发跑时 era=0xcdcdcdcd
 *           （栈填充图案）停机。所以本 port 在「调用者 sp 落在 App 窗口内」
 *           时只记下空间号，由 arkrtos_context.S 的恢复现场路径在读目标帧
 *           之前补上这一刀（la_switch_prepare）。内核任务的 sp 在恒等映射
 *           里，那种情况立即生效，与 activate() 完全等价。 */
int32 arkrtos_port_paging_switch_to(uint32 space);

/** @brief 当前装进 App 窗口的空间号 */
uint32 arkrtos_port_paging_space(void);

/** @brief 空间号对应的物理帧基址；越界返回 0（不猜一个地址出去） */
uint32 arkrtos_port_paging_space_frame(uint32 space);

/** @brief 板级给了几个 App 地址空间（>= 1） */
uint32 arkrtos_port_paging_space_count(void);

/** @brief Give a space's frames back to the pool (U-3 leftover)
 *  @param space 0 .. arkrtos_port_paging_space_count()-1
 *  @return 0 = returned (also when the space never took any frames);
 *          -1 = bad space; -2 = this build has no frame pool behind the
 *          window; -3 = the caller is standing inside the window VA
 *          (nothing touched); -4 = the pool refused (its own -1/-3 passed on)
 *  @details 空间回收：把该空间从帧池拿走的帧还回去，并且**先把窗口里属于它的
 *           条目撤掉**。不撤的话，帧已经回到池子、旧条目还指着那块物理页 ——
 *           它被别的空间拿走之后两个空间就共享内存，而且没有任何报错。
 *           只有页级窗口（ARKRTOS_USE_PAGING_PAGES=1）有帧可还；PS=20 的静态
 *           窗口是板级固定地址，没有可回收的东西，返回 -2。
 *           归还之后空间仍然存在（space_count 不变），下一次 activate() 会
 *           重新要帧 —— 语义是「这个空间空着」，不是「这个空间没了」。 */
int32 arkrtos_port_paging_space_release(uint32 space);

/** @brief 读回一个 VA 当前由哪条 TLB 条目翻译（诊断用）
 *  @return 1 = 命中（index/ppn/ps 被写）；0 = 未命中（输出参数不动）
 *  @details 只读，不建映射。用于分辨「tlbwr 没生效」与「生效了但查找命中
 *           另一条陈旧条目」——地址空间切换静默失效时就靠它定位。 */
uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index, uint32 *ppn, uint32 *ps);

#if ((ARKRTOS_USE_SMP == 1) && (ARKRTOS_USE_PRIV == 1))
/** @brief 让当前核（次核）打开分页：按静态快照重放条目，再置 CRMD.PG (R2)
 *  @return 0  = opened, and one identity entry read back correctly
 *          -1 = nothing to replay (primary never filled the snapshot)
 *          -2 = snapshot overflowed (would open PG with entries missing)
 *          -3 = PG is on but an identity VA does not translate identically
 *  @details 次核不重跑 la_mmu_init()：那一趟会从帧池给 space 0 拿 2 MB、
 *           铺 256 条窗口条目，那是引导核的 App 地址空间。这里重放的是
 *           **引导核真写下去的静态条目**（内核两条 PS=22、内核 UART、
 *           App UART 别名、帧池），并换掉 TLBRENTRY 指向本核自己的重填
 *           入口与故障栈。
 *           -3 与「没打开」必须分开：CRMD.PG 已置位但翻译不对，在串口上
 *           与「没打开」长得一样，只有读回能把它们分开。
 *  @note 必须在中断关闭时调用（写 CSR 与重放条目之间不能被节拍打断）。 */
int32 arkrtos_port_paging_enable_secondary(void);
#else
/* 关档（或单核）不出符号：调用方拿到 -1 就是「这台机器没有这条路」，
 * 不能读成「已经打开了」。 */
#define arkrtos_port_paging_enable_secondary()         (-1)
#endif

#if ((ARKRTOS_USE_PAGING == 1) && (ARKRTOS_USE_PRIV == 1))
/** @brief 在**当前核**上把某个地址空间铺进 App 窗口（R3：跨核共享 VA）
 *  @param space 0 .. arkrtos_port_paging_space_count()-1
 *  @return 0 = 已铺好且读回首尾两页自证通过
 *          -1 = 空间号越界
 *          -2 = 本档没有页级窗口（静态 PS=20 窗口在次核快照里已带着）
 *          -3 = 帧池拒绝（该空间还没拿到那 2 MB；半张窗口不许留）
 *          -4 = 按需档不支持（次核缺页路径未验证）
 *          -5 = 铺完读回对不上（「装上了但映射不对」与「没装」分开报）
 *  @details 与 arkrtos_port_paging_activate() 的分工：那个走引导核的切换
 *           路径并更新 s_pg_app_space；这个只写当前核的 TLB 条目（ASID 档
 *           下连同 CSR.ASID），不碰引导核的记账。次核切到用户任务前调用。
 *           边界：次核拿到的是「此刻的条目」，引导核之后的 COW / 换空间 /
 *           mprotect 重写它看不到 —— 那是 R4（失效回执）的事。 */
int32 arkrtos_port_paging_install_space_window(uint32 space);

/** @brief 这段地址是不是落在 App 窗口 VA 里（R3）
 *  @param va  起始 VA；len 长度（字节）
 *  @return 1 = 整段都在窗口里（访问它要经窗口翻译）；0 = 不在 / 跨出窗口 /
 *          本档的窗口是恒等映射（PAGES=0，装不装都一样）
 *  @details 用途只有一个：决定「切到这个任务之前要不要装窗口」。判据不是
 *          特权级而是**地址**——栈在窗口里的特权任务同样要装窗口，否则它
 *          的每一次压栈都落在别的空间的物理页上。 */
int32 arkrtos_port_paging_va_in_window(unsigned long va, uint32 len);

/** @brief 在目标空间里重造一份**未运行过的**任务初始帧（R3）
 *  @param space     任务要绑定到的空间号
 *  @param frame     帧起始（task->stack_ptr；不在 App 窗口里时是空操作）
 *  @param stack_top 任务的栈顶（TCB 里的 stack_top）
 *  @param entry     任务的入口（TCB 里的 entry）
 *  @return  0 = 已经在对的位置（或不需要动）；+1 = 这不是栈顶那一份初始帧，
 *               看过了、一个字都没动（fork 的子进程就是这种）；
 *          -1 = 参数/空间号非法；-3 = 帧池拒绝；-5 = 装窗口失败；
 *          -6 = 帧重造好了但恢复原窗口失败（必须当失败读数）
 *  @details 任务注册时构造初始帧是「按 stack_top 直接写」的，若该栈落在
 *           App 窗口 VA 上而此刻窗口装的是别的空间，这一写就进了别的物理页。
 *           任务第一次被恢复时 ERA/PRMD 是错的，ertn 跳到垃圾地址。
 *
 *          重造而不是搬字节：注册那一写本来就落在错的页上，没有可搬的内容；
 *          初始帧的全部内容就是 stack_init 写的那四个槽，其余全零。
 *          只在页级窗口档有实际动作；静态 PS=20 窗口是恒等映射，返回 0。 */
int32 arkrtos_port_paging_relocate_frame(uint32 space, void *frame,
                                         unsigned long stack_top, void *entry);
#else
#define arkrtos_port_paging_install_space_window(space) (-1)
#define arkrtos_port_paging_va_in_window(va, len) \
    ((void)(va), (void)(len), 0)
#define arkrtos_port_paging_relocate_frame(space, frame, stack_top, entry) \
    ((void)(space), (void)(frame), (void)(stack_top), (void)(entry), 0)
#endif

#if (ARKRTOS_USE_PRIV == 1)
/** @brief 把一个特权级写进任意一份栈帧（R3：次核切换用）
 *  @param frame   帧起始（各架构自己的帧布局）
 *  @param is_priv 1 = 恢复成特权态（PLV0 / Thread privileged），0 = 用户态
 *  @details 与 arkrtos_port_set_thread_priv() 不同：那个写的是「下一次引导核
 *           切换要用的」全局状态，这个直接写进交出去的那一帧。帧可能落在
 *           App 窗口里，调用方必须已经装好该帧所属空间。 */
void arkrtos_port_frame_apply_priv(void *frame, uint32 is_priv);

/** @brief 按「帧自己的返回地址」决定恢复特权级（R3：次核切换用）
 *  @param frame     即将被恢复的帧
 *  @param want_user 1 = 这个任务应该跑在用户态；0 = 内核任务
 *  @details 与 arkrtos_port_frame_apply_priv() 的唯一差别：want_user 只是
 *          必要条件，还要看帧里的 ERA 落在 App 窗口 VA 里才写 PLV3。
 *          为什么必须多这一条：用户任务卡在系统调用里被换下时，帧是从
 *          内核态存的（ERA 在内核镜像里），只看任务属性会把 PPLV 写成 3，
 *          恢复后以内核身份在 PLV3 上取指 ⇒ Page Privilege error 死循环。
 *          调用方仍须先把帧所属空间铺进本核窗口。 */
void arkrtos_port_frame_apply_priv_auto(void *frame, uint32 want_user);
#else
#define arkrtos_port_frame_apply_priv(frame, is_priv)   do { (void)(frame); (void)(is_priv); } while(0)
#define arkrtos_port_frame_apply_priv_auto(frame, want_user) \
    do { (void)(frame); (void)(want_user); } while(0)
#endif

/** @brief Allocate a run of contiguous 4 KB page frames (U-3)
 *  @param count  frames wanted (> 0)
 *  @param p_base on success, physical base of the first frame
 *  @return 0 = allocated; -1 = bad argument; -2 = no run that long
 *  @details Bookkeeping only: it does not install any mapping and does not
 *           touch the App window.  See ARKRTOS_USE_PAGING_FRAMES. */
int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base);

/** @brief Return a run of contiguous 4 KB page frames
 *  @return 0 = returned; -1 = outside the pool / unaligned / bad count;
 *          -3 = the run contains a frame that was not allocated (nothing moved)
 *  @details All-or-nothing: one stray free frame refuses the whole run, so a
 *           frame can never end up owned by two callers. */
int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count);

/** @brief Total frames in the board pool (0 when there is none) */
uint32 arkrtos_port_paging_frame_total(void);

/** @brief Frames currently allocated from the board pool */
uint32 arkrtos_port_paging_frame_used(void);

/** @brief How many 8 KB pairs the App window is made of (0 = no window)
 *  @details One PS=12 TLB entry covers two adjacent 4 KB pages, so the
 *           window's accounting unit is a pair, not a page. The number is
 *           the same for every space (the VA side never moves). */
uint32 arkrtos_port_paging_pair_total(void);

/** @brief How many of those pairs a space currently has mapped
 *  @return pair count; 0 for a bad space or a space that was never installed
 *  @details In the demand build this counts the pairs whose two frames have
 *           actually been paid for; in the page-level build an installed
 *           space owns the whole 2 MB run, so it is every pair. */
uint32 arkrtos_port_paging_pair_count(uint32 space);

/** @brief Physical base of one pair; 0 = that pair is not mapped
 *  @param space address space
 *  @param idx   0 .. arkrtos_port_paging_pair_total()-1
 *  @details Read-only: it never faults a pair in, so asking "what is
 *           mapped" cannot change the answer. A pair that has not been
 *           touched yet reports 0 instead of a made-up address. */
uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx);

/** @brief Copy one space's whole window content into another (U-19, fork)
 *  @param src source space, @param dst destination space (must be empty)
 *  @return 0 = copied; -1 = bad space / same space; -2 = dst already has
 *          frames (never overwritten); -3 = frame pool refused, rolled back
 *  @details Copies the pairs the source actually owns, frame by frame,
 *           through the pool's identity mapping - the App window is NOT
 *           switched, because the caller is running inside it. A pair the
 *           source has swapped out is copied in from the swap area, so the
 *           child sees the same bytes the parent would see, resident.
 *           On failure every frame this call took is returned to the pool.
 *  @note 只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */
int32 arkrtos_port_paging_space_clone(uint32 src, uint32 dst);

/** @brief Write a few words into a space by physical address (U-19, fork)
 *  @param space target space, @param va window VA (8-byte aligned)
 *  @param words source words (8 bytes each), @param n word count
 *  @return 0 = written; -1 = out of range, or that pair has no frames
 *  @details No demand faulting here on purpose: the caller must write into
 *           a pair the parent already made resident, otherwise it rolls
 *           back. Kernel paths do not quietly allocate frames behind the
 *           bookkeeping's back.
 *  @note 在 ARKRTOS_USE_LINUX_FORK == 1 或 ARKRTOS_USE_USHELL_ISO == 1 时存在：
 *        fork 用它复制父子栈，SEC-G08 的 upcall 用它写桩页、初始帧与参数区。 */
int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va,
                                      const unsigned long *words, uint32 n);

/** @brief Turn the parent trap frame into the child's first frame (U-19)
 *  @param p_exc_ctx parent's syscall trap frame (also its VA in the child)
 *  @param space     child space
 *  @return 0 = written; <0 = arkrtos_port_paging_write_space's reason
 *  @details Copies every register slot verbatim and rewrites a0 to 0, so
 *           the child resumes at the same PC with the same stack and sees
 *           clone() returning 0. ERA/sp/tp/fp and the FPU/vector area are
 *           inherited - that IS fork.
 *  @note 只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */
int32 arkrtos_port_paging_clone_frame(void *p_exc_ctx, uint32 space);

#if (ARKRTOS_USE_LINUX_THREAD == 1)
/** @brief Build a new thread's first trap frame on its own user stack (U-25)
 *  @param p_exc_ctx  parent's syscall trap frame (the template)
 *  @param space      the (shared) space the thread is bound to
 *  @param child_sp   the child's user stack top (16-byte aligned, non-zero)
 *  @param child_tls  value for the child's $tp when CLONE_SETTLS was given,
 *                    ignored otherwise
 *  @param p_frame_va may be 0; on success the VA the frame was written at
 *                    (child_sp - frame size), which is the child's stack_ptr
 *  @return 0 = written; -1 = bad argument / out of window / that pair refused
 *  @details Unlike fork this frame does NOT live at the parent's VA: the
 *           child runs on a different stack, so the frame goes at
 *           child_sp - LA_FR_SIZE, exactly where the trap entry would have
 *           put it (the entry does `sp -= 1040`), with SP = child_sp, A0 = 0
 *           and every other register inherited from the parent.
 *
 *           The pair holding that VA is taken from the pool and zeroed first:
 *           a fresh pair may carry someone else's bytes, and a thread must not
 *           start on a window into another task's memory.  Pages the child
 *           spills below the frame are demand-faulted as usual.
 *  @note Only exists when ARKRTOS_USE_LINUX_THREAD == 1. */
int32 arkrtos_port_paging_thread_frame(void *p_exc_ctx, uint32 space,
                                       unsigned long child_sp,
                                       unsigned long child_tls,
                                       unsigned long *p_frame_va);
#endif

#if (ARKRTOS_USE_PAGING_SHM == 1)
/** @brief Create MAP_SHARED anonymous pages (U-19c)
 *  @param va  window VA (4 KB aligned), @param len byte count (> 0, 4 KB aligned)
 *  @return 0 = created; -1 = bad argument, out of range, that range already
 *          holds private content, or the frame pool refused
 *  @details The pairs are allocated and zeroed right here instead of on the
 *           first touch: a shared mapping must have an identity before fork,
 *           otherwise parent and child each fault in their own frame and stop
 *           seeing each other.  Uniquely for this port the granularity is one
 *           8 KB pair, so a 4 KB mapping owns the whole pair and a private
 *           mapping that would land in it is refused (all-or-nothing).
 *  @note 关档时回 -1（宏），mmap 对 MAP_SHARED 回 -ENOSYS，与打开本开关之前一致。 */
int32 arkrtos_port_paging_map_shared(unsigned long va, unsigned long len);
#else
#define arkrtos_port_paging_map_shared(va, len)        (-1)
#endif

#if (ARKRTOS_USE_PAGING_MPROTECT == 1)
/** @brief Set or clear the write bit of every 4 KB page in a window range (U-22)
 *  @param va       window VA, 4 KB aligned
 *  @param len      byte count (> 0, 4 KB aligned)
 *  @param writable 1 = restore write access, 0 = make read-only
 *  @return 0 = done; -1 = bad argument or out of the window
 *  @details The per-page bit is recorded even for a pair that has no frame
 *           yet (the demand path reads the ledger when it installs the
 *           entry), so protecting an untouched page is not a silent no-op.
 *           Only the write bit changes: this page-table format has no NR/NX
 *           bit, so PROT_EXEC cannot be enforced and is not pretended to be.
 *  @note 关档时回 -1（宏）：mprotect 只接受并记账，条目仍是可写。 */
int32 arkrtos_port_paging_protect(unsigned long va, unsigned long len, uint32 writable);

/** @brief mprotect readout (U-22)
 *  @param which 0 = protect calls, 1 = write violations caught, 2 = read-only pages
 *  @return the reading; 0 for an unknown selector
 *  @details 只有这一档才有这个符号：关档时一个「永远返回 0 的统计函数」会被
 *           读成「一次违规都没拦住」，那是错答案。 */
uint32 arkrtos_port_paging_mprotect_stat(uint32 which);
#else
#define arkrtos_port_paging_protect(va, len, wr)       (-1)
#define arkrtos_port_paging_mprotect_stat(which)       (0u)
#endif

/** @brief Page replacement readout (U-16) - only exists when the swap switch is on
 *  @param which 0 = swap-outs, 1 = swap-ins, 2 = slots in use, 3 = failures,
 *               4 = resident pairs, 5 = slot count
 *  @return the reading; 0 for an unknown selector
 *  @details 关档时连符号都不给（宏在 arkrtos_mpu.h 里回 0）：一个「永远返回 0
 *           的统计函数」会被读成「一次都没换出过」，那是错答案。 */
#if (ARKRTOS_USE_PAGING_SWAP == 1)
uint32 arkrtos_port_paging_swap_stat(uint32 which);
#endif

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/** @brief Discard a window span so the next touch starts from zeroes (MAP_FIXED)
 *  @param va  window VA, 4 KB aligned
 *  @param len byte count (> 0, 4 KB aligned)
 *  @return 0 = done; -1 = bad argument / outside the window / would split a
 *          MAP_SHARED pair; -2 = frame pool empty (the ledger is unchanged)
 *  @details MAP_FIXED means "replace whatever is there".  On the demand tier a
 *           mapping is a pair of frames plus one 8 KB entry, so this call
 *           zeroes an owned pair in place, copies a COW-shared pair first (the
 *           other space keeps its bytes), frees a swap slot if the pair was
 *           swapped out, clears the mprotect read-only bit for the covered
 *           pages, and installs the entry writable again.  Frames are not
 *           returned to the pool - see the implementation note.
 *  @note 只在 ARKRTOS_USE_PAGING_DEMAND == 1 时存在；关档时 mmap 对 MAP_FIXED
 *        仍回 -ENOSYS（不偷偷降级成「在别处分配」）。 */
int32 arkrtos_port_paging_discard(unsigned long va, unsigned long len);
#else
#define arkrtos_port_paging_discard(va, len)           (-1)
#endif

/** @brief Virtual base of the App window (same VA for every address space)
 *  @return the window VA base (0 when the board has no paging window)
 *  @details The physical side of the window differs per space and comes from
 *           arkrtos_port_paging_space_frame(); the VA side never moves.  Code
 *           that pokes the window must use this instead of the frame base:
 *           with ARKRTOS_USE_PAGING_PAGES=1 the frames come from the pool and
 *           no longer coincide with the window VA. */
unsigned long arkrtos_port_paging_window_va(void);

/** @brief Hardware ASID currently in CSR.ASID (U-7); 0 when ASID is off
 *  @details 关档（ARKRTOS_USE_PAGING_ASID=0）时恒为 0：本 port 把每个 App 空间
 *           摊在一组固定的 TLB way 上、切换靠重写条目，压根没有 ASID 这个
 *           概念，返回一个非零值就是编的。 */
#if (ARKRTOS_USE_PAGING_ASID == 1)
unsigned long arkrtos_port_paging_asid_current(void);
uint32 arkrtos_port_paging_asid_of(uint32 space);
uint32 arkrtos_port_paging_window_way(uint32 space);

/** @brief 只写一次 CSR.ASID 并返回换之前的值（诊断用，U-7b）
 *  @param asid 目标 ASID（只取低 10 位）
 *  @return 换之前的 CSR.ASID
 *  @details 与 arkrtos_port_paging_activate() 的差别只有一条但很要紧：
 *           activate 会读回校验、缺什么补什么，所以拿它测「条目还在不在」
 *           测不出来。本函数只做一次 csrwr，不记账不补铺，调用方负责换回
 *           去。生产路径不该用它。 */
unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid);

/** @brief 按 VA 让翻译失效（invtlb op=3：不看 ASID，global 条目不动）
 *  @param va 要失效的虚拟地址
 *  @details 留着它是为了做对照：op=6 的「定向」只有跟 op=3 的连带行为摆
 *           在一起才算证据。生产路径没有调用方。 */
void arkrtos_port_paging_inval_va(unsigned long va);
#else
/* 关档不出符号：一个「永远返回 0 的函数」会被读成「ASID 就是 0」，
 * 而真相是本 port 压根没有 ASID 概念（切换靠重写整组条目）。宏回 0
 * 只保证调用方能编过，语义由调用方自己解释。 */
#define arkrtos_port_paging_asid_current()              (0UL)
#define arkrtos_port_paging_asid_of(space)              (0u)
#define arkrtos_port_paging_window_way(space)           (0u)
#endif

#else

#define arkrtos_port_paging_init()                 (-1)
#define arkrtos_port_paging_check(base, size, mem) (-1)
#define arkrtos_port_paging_alias(va, pa, mem)     (-1)
#define arkrtos_port_paging_unalias(va)            (-1)
#define arkrtos_port_paging_dyn_used()             (0u)
#define arkrtos_port_paging_activate(space)        (-1)
#define arkrtos_port_paging_switch_to(space)       (-1)
#define arkrtos_port_paging_space()                (0u)
#define arkrtos_port_paging_space_frame(space)     (0u)
#define arkrtos_port_paging_space_count()          (1u)
#define arkrtos_port_paging_space_release(space)   (-1)
#define arkrtos_port_paging_lookup(va, index, ppn, ps) (0u)
#define arkrtos_port_paging_flush(base, size)
#define arkrtos_port_paging_frame_alloc(count, p_base)  (-1)
#define arkrtos_port_paging_frame_free(base, count)     (-1)
#define arkrtos_port_paging_frame_total()               (0u)
#define arkrtos_port_paging_frame_used()                (0u)
#define arkrtos_port_paging_pair_total()                (0u)
#define arkrtos_port_paging_pair_count(space)           (0u)
#define arkrtos_port_paging_pair_pa(space, idx)         (0u)
#define arkrtos_port_paging_space_clone(src, dst)       (-1)
#define arkrtos_port_paging_write_space(sp, va, w, n)   (-1)
#define arkrtos_port_paging_clone_frame(p, sp)          (-1)
#define arkrtos_port_paging_thread_frame(p, sp, csp, ctls, pf) \
    ((void)(p), (void)(sp), (void)(csp), (void)(ctls), (void)(pf), (-1))
#define arkrtos_port_paging_map_shared(va, len)         (-1)
#define arkrtos_port_paging_protect(va, len, wr)        (-1)
#define arkrtos_port_paging_mprotect_stat(which)        (0u)
#define arkrtos_port_paging_discard(va, len)           (-1)
#define arkrtos_port_paging_window_va()                 (0u)
#define arkrtos_port_paging_asid_current()              (0UL)
#define arkrtos_port_paging_asid_of(space)              (0u)
#define arkrtos_port_paging_window_way(space)           (0u)

#endif


/* ============================================================
 * 自旋锁 API（内核/驱动侧）
 * @brief arkrtos_spin.h 基于本文件的原子接口实现，此处统一引入，
 *        便于内核与板级驱动代码只包含 arkrtos_hal.h 即可使用。
 * ============================================================ */
/* ============================================================
 * Flash 接口层
 * @brief 片内 Flash 擦除/编程（供 Loader 写入 App 镜像）
 *        由 board 层实现（如 board/stm32f427/drvflash.c）。
 *        Flash 已映射到地址空间，读取不需要本组接口。
 * ============================================================ */

/**
* @brief 擦除 Flash 区间（按扇区擦除，自动对齐到扇区边界）
* @param addr 起始地址
* @param size 字节长度
* @return 0=成功，-1=失败
*/
int32  arkrtos_port_flash_erase(uint32 addr, uint32 size);

/**
* @brief 写入 Flash（按字节编程，写入前该区间必须已擦除）
* @param addr 起始地址
* @param data 数据指针
* @param len  字节长度
* @return 0=成功，-1=失败
*/
int32  arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len);

/**
* @brief 获取指定地址所在扇区的大小（字节）
* @param addr Flash 地址
* @return 扇区大小；地址非法返回 0
*/
uint32 arkrtos_port_flash_sector_size(uint32 addr);

/**
* @brief 片内 Flash 的保护状态（只读，不解锁、不写选项字节）
* @details 由板级实现；内核的安全基线自检用它把"读保护/写保护到底什么档"
*          摆出读数，而不是报一句"没通路"。
*/
typedef struct
{
    uint32 rdp_level;    /* 0 = 无读保护（谁拿到片子都能读走固件）, 1 = 读保护生效 */
    uint32 wrp_mask_lo;  /* 扇区 0..11 的写保护位，1 = 该扇区被保护 */
    uint32 wrp_mask_hi;  /* 扇区 12..23 的写保护位（低 12 位有效） */
} arkrtos_flash_prot_t;

/**
* @brief 读取片内 Flash 的保护状态
* @param p_out 输出（三个字段全部由实现填写）
* @return 0=读取成功；其他=读不到（板级未实现 / 该芯片没有这些寄存器）
* @note 只读操作：不解除 OPTLOCK、不擦除、不改变任何保护位。
*/
int32  arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out);

/**
* @brief 信任根：从板级一次性槽取镜像公钥（SEC-G12）
* @param pp_key 输出：公钥字节指针（实现持有，内核只读，不释放）
* @param p_len  输出：公钥字节数（Ed25519 为 32）
* @return 0=取到；其他=这块板没有 OTP 公钥槽（内核退回落编译进来的公钥）
* @note 实现可以直接指向一块只读的常量，也可以解密后放进静态缓冲；
*       但**不得**在每次调用返回不同内容——同一块板子的信任根必须是一样的。
*       没实现（weak 默认）就返回 -1，等价于"全走编译期公钥"。
*/
int32  arkrtos_port_trust_pubkey(const uint8 **pp_key, uint32 *p_len);

/**
* @brief 信任根：读/写板级公钥摘要槽（SEC-G12）
* @param p_digest 32 字节缓冲
* @param write   0=读（把槽内 32 字节复制进 p_digest）
*                1=写（把 p_digest 内容烧进槽；一次性槽应在此之后拒绝再写）
* @return 0=成功；其他=该板没有这个槽 / 读不出来 / 写不进去
* @note 一次性槽的语义是"烧了就烧了"，但内核**不依赖**它真的只写一次：
*       内核自己每次开机都会比对，写不进去就锁存。所以弱实现（比如内部的
*       .noinit + 备份寄存器）也能用，只是保护强度低，须在板级文档里写明。
*/
int32  arkrtos_port_trust_pin(uint8 *p_digest, uint32 write);

#include "arkrtos_spin.h"

#endif
