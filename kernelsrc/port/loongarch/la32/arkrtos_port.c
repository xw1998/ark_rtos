/**
* @file arkrtos_port.c
* @brief Ark RTOS LoongArch32 移植层实现
* @details 实现 arkrtos_hal.h 声明的全部硬件抽象接口。与 la64 版逐函数对应，
*          必须不同的只有两处：**帧槽 4 字节（帧 128 字节）**、以及
*          **`rdtime` 在 LA32 上按寄存器对返回**。
*
*          ## 验证状态（必读）
*
*          **本移植只验证到「编译 + 链接产出 ELF」，没有在任何目标上运行过。**
*          环境里没有 LA32 的执行载体：QEMU 9.2.3 只有 `qemu-system-loongarch64`，
*          没有 loongarch32 系统模拟器，也没有 LA32 板子。因此本目录代码
*          **不得**表述为「已通过」「已验证」，只能是「编译通过、未上目标」。
*          文档里用到的中断/定时器/PRMD 语义全部沿用 la64 的实测结论，
*          那些结论本身是在 LA64 上量的，LA32 硬件是否逐位相同没有证据。
*
*          ## 与 Cortex-M / RISC-V 移植的对应关系
*
*          | Cortex-M              | RISC-V                  | 本移植                       |
*          |-----------------------|-------------------------|------------------------------|
*          | SVC 异常              | `ecall`（号经 a7）      | `syscall code`（号在指令里） |
*          | PendSV 切换           | CLINT 软件中断          | **就地切换**（无软中断可用） |
*          | SysTick 节拍          | CLINT 机器定时器        | TCFG/TICLR 定时器            |
*          | 硬件自动压栈异常帧    | trap_entry 手写 144 字节| eentry 手写 128 字节帧       |
*          | PRIMASK               | mstatus.MIE             | CRMD.IE                      |
*          | IPSR != 0 判在中断里  | 软件计数 s_in_trap      | 软件计数 s_in_trap           |
*
*          ## 为什么没有「挂起切换」
*
*          LoongArch 的 CSR.IPI 只向**其他核**发中断：单核上 `csrwr` 自触发
*          实测不产生任何中断（docs 里有探针记录），所以没有 RISC-V 中
*          MSIP/PendSV 那样的「置挂起位、异常返回后再进一次 trap」的机制。
*          于是切换分成两种落点，共用同一种 128 字节帧：
*
*          - **任务上下文调用**（arkrtos_port_switch_task）：直接在调用者栈上
*            构造同格式帧并 ertn，见 arkrtos_context.S 的
*            `arkrtos_port_switch_task`。此时若在异常内部则只置
*            `s_switch_pending`，由异常出口结算。
*          - **异常内部调用**（节拍 / SVC 处理程序里）：置 `s_switch_pending`，
*            la_trap_c() 在返回前调一次 arkrtos_sched_is_switching() /
*            activate()，等价于 RISC-V 那次「异常返回后立刻进的软中断」。
*
*          ## 已经知道的边界（不得当成「已有隔离」对外表述）
*
*          1) 只跑 PLV0，没有页表 / 地址窗口隔离，任务之间没有内存隔离；
*          2) 异常处理程序与被中断的任务共用同一个栈（没有独立 handler 栈）；
*          3) trap 不可嵌套：进异常时 CRMD.IE 已被硬件清零，处理程序全程关中断；
*          4) `syscall` 之后 ERA 由本层加 4（硬件不会自己前进，不加上就是死循环）；
*          5) 指针宽度与内核的 uint32 一致（LA32），**不存在 la64 那种
*             「64 位指针 / 32 位地址字段」的截断问题**；代价是 4 GB 以上的
*             物理地址本移植无法表达，与内核现状一致。
*
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_RTSTAT == 1)
#include "arkrtos_rt.h"
#endif

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG
#endif

/* ============================================================
 * CSR 编号（LoongArch 手册固定值，不随平台变化）
 * ============================================================ */
#define LA_CSR_CRMD         0x0u
#define LA_CSR_PRMD         0x1u
#define LA_CSR_ECFG         0x4u
#define LA_CSR_ESTAT        0x5u
#define LA_CSR_ERA          0x6u
#define LA_CSR_EENTRY       0xCu
#define LA_CSR_TCFG         0x41u
#define LA_CSR_TVAL         0x42u
#define LA_CSR_TICLR        0x44u

/* CRMD：PLV[1:0]、IE[2]、DA[3] */
#define LA_CRMD_IE          (1u << 2)

/* PRMD：PPLV[1:0]、PIE[2]、PWE[3]（位序与 CRMD 低 4 位相同，容易记反）。
 * `ERTN` 按 PRMD.PPLV 写 CRMD.PLV、按 PRMD.PIE 写 CRMD.IE，
 * 因此「回到 PLV0 且开中断」= PPLV=0 + PIE=1 = **0x4**，不是 0x1。 */
#define LA_PRMD_INIT        (0x4u)

/* ESTAT：IS[12:0]、Ecode[21:16]、EsubCode[30:22] */
#define LA_ESTAT_IS_TIMER   (1u << 11)      /* TI：定时器中断 */
#define LA_ESTAT_ECODE_SYS  (0xBu)          /* SYS：syscall    */
#define LA_ESTAT_ECODE_INT  (0x0u)          /* 中断（Ecode=0，靠 IS 分源） */

/* ECFG：LIE[12:0]、VS[15:13]。定时器 = LIE[11] */
#define LA_ECFG_LIE_TIMER   (1u << 11)

/* TCFG：En[0]、Periodic[1]、InitVal[31:2] */
#define LA_TCFG_EN          (1u << 0)
#define LA_TCFG_PERIODIC    (1u << 1)

/* syscall 指令编码：`syscall code` = 0x002B0000 | code(15 位) */
#define LA_INSN_SYSCALL_OP  (0x002B0000u)
#define LA_INSN_SYSCALL_NUM (0x7FFFu)

/* 定时器计数时钟：沿用 la64 在 QEMU virt 上实测的 25 MHz（InitVal=2000000
 * 时 10 s 内 125 次中断，误差 <1%）。**LA32 没有可运行的载体，这个值在本
 * 目标上未验证**；换真实芯片由板级头覆盖。 */
#ifndef ARKRTOS_PORT_TIMER_HZ
#define ARKRTOS_PORT_TIMER_HZ   25000000u
#endif

/* rdtime 计数时钟：同样是 la64 在 QEMU virt 上实测的 100 MHz（1e9 ticks
 * ≈ 10.0 s，与主机时间差 1.9%）。本目标未验证，真实芯片按手册覆盖。
 * 这个时钟给 delay_us 与 get_timer_counter 用，与定时器计数是两个独立源。 */
#ifndef ARKRTOS_PORT_RDTIME_HZ
#define ARKRTOS_PORT_RDTIME_HZ  100000000u
#endif

/* ============================================================
 * trap 帧索引（与 arkrtos_context.S 的 256 字节布局一一对应）
 * ============================================================ */
#define LA_FR_RA            (0u)
#define LA_FR_TP            (1u)
#define LA_FR_SP            (2u)
#define LA_FR_A0            (3u)
#define LA_FR_ERA           (30u)   /* 偏移 120 */
#define LA_FR_PRMD          (31u)   /* 偏移 124 */
#define LA_FR_WORDS         (32u)   /* 128 字节 = 32 槽 × 4 字节 */
#define LA_FR_SIZE          (128u)

/* 帧槽是 **4 字节**（LA32 的通用寄存器宽度），`unsigned long` 与 `uint32`
 * 在本目标上同宽，两者都能正确索引。仍沿用 la64 引入的专用类型名，好处是
 * 「帧槽宽度」在源码里是一个显式概念：把这份代码搬到 64 位目标时只需改这个
 * typedef 与 .S 里的 st.w/ld.w，不会漏掉索引宽度。
 * @note 反过来也成立：la64 那次「用 uint32* 索引 8 字节槽」的错位缺陷在本
 *       目标上不会出现，但这不代表可以把 la32 的帧当成 8 字节去用。 */
typedef unsigned long la_frame_word;

/* ============================================================
 * 内部状态
 * ============================================================ */

/* 是否已经在异常处理程序内部。LoongArch 进异常后没地方读「我在哪一层」，
 * 而内核需要这个判定决定「注册唤醒」还是「就地让出」。 */
static volatile uint32 s_in_trap = 0u;

/* 异常内部请求的切换：节拍处理与 SVC 处理都可能发起，由异常出口统一结算。
 * 单核 + trap 不嵌套 ⇒ 一个标志就够。 */
static volatile uint32 s_switch_pending = 0u;

/* 任务切换门闩：enter_idle 之前不切（与 M4 / RV32 移植同一语义）。 */
static uint8 s_switch_ready = 0u;

/* 外部符号：异常入口与汇编入口 */
extern void eentry(void);
extern void arkrtos_port_resume_task(uint32 stack_ptr);
extern void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);

/* 板级可覆盖的故障报告（默认什么都不打，见文件末尾） */
void arkrtos_port_fatal_report(uint32 code);

/* ============================================================
 * CSR 小工具
 * @note csrxchg 的 rd 既是「新值来源」也是「旧值去处」，所以每个小工具
 *       都单独写死 CSR 立即数：csr 必须是汇编期常量，无法用操作数传。
 * ============================================================ */
static uint32 la_crmd_read(void)
{
    uint32 v;

    __asm__ volatile("csrrd %0, 0x0" : "=r"(v) : : "memory");
    return v;
}

static uint32 la_estat_read(void)
{
    uint32 v;

    __asm__ volatile("csrrd %0, 0x5" : "=r"(v) : : "memory");
    return v;
}

static void la_estat_clear(void)
{
    uint32 v = 0u;

    __asm__ volatile("csrwr %0, 0x5" : "+r"(v) : : "memory");
}

/**
* @brief 读稳定计数器低 32 位
* @details LA32 的 `rdtime.d rd, rj` 是**寄存器对**形式：低 32 位写 rd、
*          高 32 位写 rj。这里只取低半部（HAL 的计数器就是 32 位），高半部
*          写进一个临时寄存器后丢弃。
* @note 不能用 `rdtime.w`：本工具链的 binutils 在 la32 上不认这条助记符
*       （实测 `rdtime.w` / `rdtimeh.w` 均报 "no match insn"），而 `.d` 形式
*       会被接受——这也是本文件里唯一一处看起来像 64 位、实际合法的指令。
*/
static uint32 la_rdtime(void)
{
    uint32 lo;
    uint32 hi;

    __asm__ volatile("rdtime.d %0, %1" : "=r"(lo), "=r"(hi) : : "memory");
    (void)hi;
    return lo;
}

/**
* @brief 微秒 -> 定时器计数
* @note 定时器时钟是 25 MHz 的整数倍关系，按「先约分再相乘」拆开避免
*       64 位乘除；与 RISC-V 版同一写法（那边是因为工具链没有对应 libgcc）。
*/
static uint32 la_us_to_counts(uint32 us)
{
    uint32 counts;

#if ((ARKRTOS_PORT_TIMER_HZ % 1000000u) == 0u)
    counts = us * (ARKRTOS_PORT_TIMER_HZ / 1000000u);
#elif ((ARKRTOS_PORT_TIMER_HZ % 1000u) == 0u)
    counts = ((us / 1000u) * (ARKRTOS_PORT_TIMER_HZ / 1000u))
           + (((us % 1000u) * (ARKRTOS_PORT_TIMER_HZ / 1000u)) / 1000u);
#else
    counts = (us / 1000u) * (ARKRTOS_PORT_TIMER_HZ / 1000u);
#endif

    if(counts == 0u)
    {
        counts = 1u;
    }

    return counts;
}

/* 写 TCFG：初始化值放 [31:2]，bit1 周期模式、bit0 使能。
 * @note 不能写 TVAL 来装周期：实测写 TVAL 会立刻触发 INE 异常（Ecode=0xD），
 *       硬件不允许用 TVAL 改 InitVal。 */
static void la_tcfg_write(uint32 init_val)
{
    uint32 v = ((init_val & 0x3FFFFFFFu) << 2) | LA_TCFG_PERIODIC | LA_TCFG_EN;

    __asm__ volatile("csrwr %0, 0x41" : "+r"(v) : : "memory");
}

static void la_ticlr_clear(void)
{
    uint32 v = 1u;

    __asm__ volatile("csrwr %0, 0x44" : "+r"(v) : : "memory");
}

/* ============================================================
 * CPU 控制
 * ============================================================ */
void arkrtos_port_wfi(void)  { __asm__ volatile("idle 0" ::: "memory"); }
void arkrtos_port_wfe(void)  { __asm__ volatile("idle 0" ::: "memory"); }   /* 无 WFE，等价于等中断 */
void arkrtos_port_nop(void)  { __asm__ volatile("nop"    ::: "memory"); }
void arkrtos_port_isb(void)  { __asm__ volatile("ibar 0" ::: "memory"); }
void arkrtos_port_dsb(void)  { __asm__ volatile("dbar 0" ::: "memory"); }
void arkrtos_port_dmb(void)  { __asm__ volatile("dbar 0" ::: "memory"); }

/* ============================================================
 * 原子操作与多核支持
 * ============================================================ */

/**
* @brief 比较并交换（LL/SC 实现）
* @details LoongArch 的 `ll.w` / `sc.w` 是真原子指令。单核下关中断也算
*          正确，但这套写法在 SMP 上同样成立，将来换多核不用重写。
* @note 用 `.w` 而不是 `.d` 与内核的 uint32 宽度严格对应：内核的原子量
*       是 32 位，用 64 位原子会读写到相邻字段。
*/
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    uint32 old;
    uint32 fail;

    do
    {
        __asm__ volatile("ll.w %0, %1, 0"
                         : "=&r"(old)
                         : "r"(p_addr)
                         : "memory");

        if(old != expect)
        {
            /* 撤销保留：写一次 sc.w 并丢弃结果即可。 */
            __asm__ volatile("sc.w $zero, %0, 0" : : "r"(p_addr) : "memory");
            return 0u;
        }

        __asm__ volatile("sc.w %0, %2, 0"
                         : "=&r"(fail)
                         : "r"(p_addr), "r"(new_value)
                         : "memory");
    } while(fail != 0u);

    return 1u;
}

uint32 arkrtos_port_cpu_id(void)
{
    return 0u;
}

void arkrtos_port_spin_hint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

#if (ARKRTOS_USE_SMP == 1)
/* 单核移植：没有第二核可启动，也没有跨核 IPI 通路（CSR.IPI 只发给其他核，
 * 单核上无对端可发）。 */
int32 arkrtos_port_cpu_start(uint32 cpu)
{
    (void)cpu;
    return -1;
}

int32 arkrtos_port_send_ipi(uint32 cpu)
{
    (void)cpu;
    return -1;
}
#endif /* ARKRTOS_USE_SMP == 1 */

/* ============================================================
 * 中断开关与异常上下文
 * ============================================================ */
uint8 arkrtos_port_in_isr(void)
{
    return (s_in_trap != 0u) ? 1u : 0u;
}

/**
* @brief 从异常帧里取系统调用号
* @details LoongArch 的 `syscall code` 把号码编在指令里（15 位），所以这里
*          从「指令字」解出来，而不是从某个寄存器槽读。
*
*          **注意**: la_trap_c() 在调 SVC_Server() 之前已经把帧里的 ERA 加了 4
*          （syscall 指令占 4 字节），所以此刻 ERA 指向下一条指令，被调用的
*          syscall 在 ERA - 4 处。两条路径（谁先谁后）在文件头注释里写死，
*          改动时两处必须同时改。
*
*          解出的指令若不是 syscall 形式，返回 0xFFFFFFFF：调用方会落到
*          default 分支拒绝这次服务，而不是拿一个看似合法的号去执行。
*/
uint32 arkrtos_port_syscall_num(void *p_exc_ctx)
{
    la_frame_word *frame = (la_frame_word *)p_exc_ctx;
    uint32 pc;
    uint32 insn;
    uint32 num;

    pc   = frame[LA_FR_ERA];
    insn = *(volatile uint32 *)(unsigned long)(pc - 4u);

    if((insn & 0xFFFF8000u) != LA_INSN_SYSCALL_OP)
    {
        return 0xFFFFFFFFu;
    }

    num = insn & LA_INSN_SYSCALL_NUM;

    return num;
}

uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
{
    if(idx >= ARKRTOS_ARCH_SVC_ARG_MAX)
    {
        return 0u;
    }

    return (uint32)((la_frame_word *)p_exc_ctx)[LA_FR_A0 + idx];
}

void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)
{
    ((la_frame_word *)p_exc_ctx)[LA_FR_A0] = (la_frame_word)value;
}

#if (ARKRTOS_USE_LINUX_ABI == 1)
/**
* @brief LA32 没有 Linux ABI 的载体
* @details Linux/LoongArch 只有 64 位；LA32 上不存在可供兼容的 Linux 用户
*          态 ABI，因此返回「不是 Linux 调用」，由 Ark 自己的号表继续处理。
*          这里不猜一个号，也不假装支持。
*/
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx)
{
    (void)p_exc_ctx;
    return 0xFFFFFFFFu;
}

void arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value)
{
    ((la_frame_word *)p_exc_ctx)[LA_FR_A0] = (la_frame_word)value;
}
#endif

void arkrtos_port_disable_irq(void)
{
    uint32 mask = LA_CRMD_IE;

    __asm__ volatile("csrxchg $zero, %0, 0x0" : : "r"(mask) : "memory");
}

void arkrtos_port_enable_irq(void)
{
    uint32 mask = LA_CRMD_IE;

    __asm__ volatile("csrxchg %0, %0, 0x0" : "+r"(mask) : : "memory");
}

/**
* @brief 进入临界区，返回进入前的 CRMD
* @details 与 RISC-V 版「一次读改写」同一口径：先 csrrd 读旧值，再
*          csrxchg 清 IE。csrxchg 的 rd 同时是新值来源，不能一次搞定两者，
*          所以这里是两条指令 —— 它们之间若来中断也不改变语义：临界区
*          的调用方随后会发现自己拿到的 state 里 IE=1，退出时会重新打开。
*/
uint32 arkrtos_port_enter_critical(void)
{
    uint32 old = la_crmd_read();
    uint32 mask = LA_CRMD_IE;

    __asm__ volatile("csrxchg $zero, %0, 0x0" : : "r"(mask) : "memory");

#if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_crit_enter();
#endif

    return old;
}

void arkrtos_port_exit_critical(uint32 state)
{
#if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_crit_exit();
#endif

    if((state & LA_CRMD_IE) != 0u)
    {
        arkrtos_port_enable_irq();
    }
}

/**
* @brief 请求一次任务切换
* @details 两种落点：
*          - 在异常处理程序里调用：只置 s_switch_pending，由 la_trap_c()
*            的出口结算（等价于 RISC-V 那次软中断，但不需要真的再进一次
*            trap）——绝不能在 trap 里就地 ertn 换栈，那会把异常处理程序
*            自己丢掉；
*          - 在任务上下文里调用：交给汇编的就地切换路径，构造同格式帧后
*            ertn（见 arkrtos_context.S）。
*          门闩未打开时两种落点都退化为「什么都不做」。
*/
void arkrtos_port_switch_task(void)
{
    if(s_switch_ready == 0u)
    {
        return;
    }

    if(s_in_trap != 0u)
    {
        s_switch_pending = 1u;
        return;
    }

    {
        /* 声明在 .S 里，避免 C 侧为它生成 prologue/epilogue。 */
        extern void arkrtos_port_switch_task_inline(void);

        arkrtos_port_switch_task_inline();
    }
}

void arkrtos_port_switch_enable(void)
{
    s_switch_ready = 1u;

    /* 立刻请求一次切换：第一个任务在这一步之后才开始运行。
     * 此刻仍在任务上下文（idle 栈上），走就地切换路径。 */
    arkrtos_port_switch_task();
}

/* ============================================================
 * 上下文层
 * ============================================================ */

/* LoongArch 移植没有 PSP/CONTROL 这组概念，也没有线程级特权切换
 * （不使用 PLV3）。这几个接口保留是为了满足 HAL 的完整声明。 */
void arkrtos_port_set_psp(uint32 val)
{
    (void)val;
}

uint32 arkrtos_port_get_control(void)
{
    return 0u;
}

void arkrtos_port_set_control(uint32 val)
{
    (void)val;
}

void arkrtos_port_set_thread_priv(uint32 is_priv)
{
    (void)is_priv;
}

/**
* @brief 构造任务初始 trap 帧
* @param stack_top 栈顶（高地址）
* @param entry     任务入口
* @return 帧指针（存入 task->stack_ptr）
* @details 帧格式与 arkrtos_context.S 完全一致。任务第一次被恢复时会走
*          「eentry 的恢复半段」：ERA←entry、PRMD←LA_PRMD_INIT（PIE=1、
*          PPLV=0，值为 0x4）、
*          sp←stack_top，然后 ertn 跳进任务，此时 CRMD.IE 已经打开。
*          ra 置 0：任务函数返回就等于跳到 0 取指，会立刻进致命异常，
*          与 Cortex-M 版（LR=0）同一处理口径。
*/
uint32 arkrtos_port_stack_init(uint32 stack_top, void (*entry)(void))
{
    la_frame_word *frame;
    uint32 i;
    uint32 top;

    top   = stack_top & ~0xFu;
    frame = (la_frame_word *)(unsigned long)(top - LA_FR_SIZE);

    for(i = 0u; i < LA_FR_WORDS; i++)
    {
        frame[i] = 0u;
    }

    frame[LA_FR_RA]   = 0u;
    frame[LA_FR_SP]   = top;
    frame[LA_FR_ERA]  = (uint32)(unsigned long)entry;
    frame[LA_FR_PRMD] = LA_PRMD_INIT;

    return (uint32)(unsigned long)frame;
}

/* ============================================================
 * 定时器层
 * ============================================================ */
uint32 arkrtos_port_get_system_clock(void)
{
    return ARKRTOS_PORT_RDTIME_HZ;
}

uint32 arkrtos_port_get_timer_counter(void)
{
    return la_rdtime();
}

uint32 arkrtos_port_get_timer_reload(void)
{
    return 0xFFFFFFFFu;
}

/* ============================================================
 * 周期级时间基准（rdcntvl.w）
 * ------------------------------------------------------------
 * 与 la64 同一套机制：TSC 按核频走，rdtime 按固定频率走，频率开机实测。
 * 本档没有执行载体（见 docs/验证载体分级.md），这段只在编译期成立，
 * 没有任何 QEMU / 真机读数 —— 不因为"写了"就当成可用。
 * ============================================================ */
#if (ARKRTOS_USE_RTSTAT == 1)

static uint32 s_cycle_hz;

static uint32 la_tsc_read(void)
{
    uint32 lo;

    /* 本工具链的 binutils 不认 rdcntvl.w 助记符（实测 "no match insn"），
     * 所以直接写它的编码形式 csrrd rd, 0x40（TID = 稳定计数器低 32 位）。 */
    __asm__ volatile("csrrd %0, 0x40" : "=r"(lo) : : "memory");
    return lo;
}

int32 arkrtos_port_cyccnt_init(void)
{
    /* LoongArch 只有一条稳定计数器：rdtime 读的就是它（CSR 0x40 = TID），
     * 频率是板级常量 ARKRTOS_PORT_RDTIME_HZ（QEMU virt 实测 100 MHz）。
     * 不存在「核频计数与已知时基不同源」的问题，所以不需要校准，也就
     * 不用在开机时空转 10 ms、不用 64 位除法。 */
    s_cycle_hz = ARKRTOS_PORT_RDTIME_HZ;
    return 0;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return la_tsc_read();
}

uint32 arkrtos_port_get_cycle_clock(void)
{
    return s_cycle_hz;
}

int32 arkrtos_port_cycle_clock_measured(void)
{
    /* 频率来自板级常量（rdtime 的固定频率），不是量出来的。 */
    return 0;
}

#else

int32 arkrtos_port_cyccnt_init(void)
{
    return -1;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_RTSTAT */

/**
* @brief 启动节拍定时器
* @param tick_period_us 节拍周期（微秒）
* @details TCFG 是自动重装的周期定时器（Periodic 位），硬件负这个责，
*          异常处理里只需要清 TICLR 并调内核节拍处理。
*          定时器中断的使能位（ECFG.LIE[11]）也在这里才放行 —— 与 RISC-V
*          版同一顺序：irq_init() 只开全局中断，节拍在调度数据结构就绪后
*          才允许进来。
*/
void arkrtos_port_start_timer(uint32 tick_period_us)
{
    la_tcfg_write(la_us_to_counts(tick_period_us));

    {
        uint32 mask = LA_ECFG_LIE_TIMER;

        __asm__ volatile("csrxchg %0, %0, 0x4" : "+r"(mask) : : "memory");
    }
}

/* tickless 用：直接给计数，不做微秒换算（避免 n * TICK_PERIOD_US 丢相位）。 */
void arkrtos_port_start_timer_counts(uint32 counts)
{
    if(counts == 0u)
    {
        counts = 1u;
    }

    la_tcfg_write(counts);

    {
        uint32 mask = LA_ECFG_LIE_TIMER;

        __asm__ volatile("csrxchg %0, %0, 0x4" : "+r"(mask) : : "memory");
    }
}

/* TCFG 没有「计到过 0」这样的粘滞标志：周期定时器只是按 InitVal 重装。
 * 返回 0 让上层按「提前唤醒」处理（结果仍正确，只是少省一次睡）。 */
uint32 arkrtos_port_timer_countflag_take(void)
{
    return 0u;
}

void arkrtos_port_delay_us(uint32 us)
{
    uint32 start = la_rdtime();
    uint32 wait  = (us > 42949u) ? 0xFFFFFFFFu : (us * (ARKRTOS_PORT_RDTIME_HZ / 1000000u));

    while((uint32)(la_rdtime() - start) < wait)
    {
        /* 忙等：这段时间刻意不睡，语义与 ARM 版一致 */
    }
}

/* ============================================================
 * 异常分发
 * ============================================================ */

/**
* @brief 就地切换的 C 半段（由 arkrtos_context.S 的立即切换路径调用）
* @param frame 刚构造好的当前帧
* @return 要恢复的帧；不需要切换时原样返回
*/
la_frame_word *la_switch_inline(la_frame_word *frame)
{
    int32 next;
    uint32 next_sp;

    next = arkrtos_sched_is_switching();
    if(next < 0)
    {
        return frame;
    }

    next_sp = arkrtos_sched_activate(next, (uint32)(unsigned long)frame);
    if(next_sp != 0u)
    {
        return (la_frame_word *)(unsigned long)next_sp;
    }

    return frame;
}

/**
* @brief 异常帧的统一 C 入口
* @param frame 当前上下文保存的帧指针（eentry 里由 s0 传入）
* @return 要恢复的帧指针：发生任务切换时换成另一个任务的帧，否则原样返回
* @details 三条路径：
*          - 中断（Ecode = 0）：本移植只用定时器（IS bit11），清 TICLR 后
*            交给内核节拍处理；
*          - syscall（Ecode = 0xB = SYS）：ERA += 4 后进 SVC_Server；
*          - 其余 Ecode 一律按致命异常处理（报 Ecode/ESTAT/ERA 后停机，
*            不做假恢复）。
*          出口处结算 s_switch_pending：这是「没有软中断」的补偿点。
*/
la_frame_word *la_trap_c(la_frame_word *frame)
{
    uint32 estat;
    uint32 ecode;

    s_in_trap++;

    estat = la_estat_read();
    ecode = (estat >> 16) & 0x3Fu;

    if(ecode == LA_ESTAT_ECODE_INT)
    {
        if((estat & LA_ESTAT_IS_TIMER) != 0u)
        {
            la_ticlr_clear();
            arkrtos_kernel_tick_handler();
        }
        else
        {
            /* 没有别的中断源被使能，来一个就是配置错误：把 IS 打出来停机。 */
            (void)la_estat_clear();
            arkrtos_port_fatal_report(estat);
            for(;;)
            {
                __asm__ volatile("idle 0" ::: "memory");
            }
        }
    }
    else if(ecode == LA_ESTAT_ECODE_SYS)
    {
        /* 硬件不会让 ERA 前进，syscall 指令占 4 字节。
         * 注意顺序：先 +4 再进 SVC_Server，所以 arkrtos_port_syscall_num()
         * 从 ERA - 4 取指令字（两处约定必须同步）。 */
        frame[LA_FR_ERA] += 4u;
        SVC_Server((void *)frame);
    }
    else
    {
        arkrtos_port_fatal_report(estat);
        for(;;)
        {
            __asm__ volatile("idle 0" ::: "memory");
        }
    }

    /* 出口结算：异常内部请求的切换在这里落地。与 RISC-V 版同一判定
     * （arkrtos_sched_is_switching() >= 0 才切，0 = 切到 idle）。 */
    if((s_switch_ready != 0u) && (s_switch_pending != 0u))
    {
        int32 next;

        s_switch_pending = 0u;

        next = arkrtos_sched_is_switching();
        if(next >= 0)
        {
            uint32 next_sp = arkrtos_sched_activate(next, (uint32)(unsigned long)frame);

            if(next_sp != 0u)
            {
                frame = (la_frame_word *)(unsigned long)next_sp;
            }
        }
    }

    s_in_trap--;

    return frame;
}

/* ============================================================
 * 板级初始化默认实现（board 层可覆盖）
 * ============================================================ */
ARKRTOS_WEAK void arkrtos_port_board_init(void)
{
    /* 无 FPU 需要使能，无时钟树需要配置：QEMU virt 起来就是这个状态。 */
}

/**
* @brief 安装异常入口并打开全局中断
* @details 三件事：
*          1) EENTRY = eentry（异常入口只有这一个，Ecode 全在 ESTAT 里）；
*          2) 清 TICLR，避免复位值留下挂起位；
*          3) CRMD.IE = 1 —— 相当于 Cortex-M 复位后 PRIMASK 为 0。
*          定时器中断使能（ECFG.LIE[11]）**不**在这里开：那时内核的调度
*          数据结构还没初始化，节拍在 trap 里改调度状态是不安全的。
*/
ARKRTOS_WEAK void arkrtos_port_irq_init(void)
{
    uint32 entry = (uint32)(unsigned long)&eentry;

    __asm__ volatile("csrwr %0, 0xC" : "+r"(entry) : : "memory");

    la_ticlr_clear();

    {
        uint32 mask = LA_CRMD_IE;

        __asm__ volatile("csrxchg %0, %0, 0x0" : "+r"(mask) : : "memory");
    }

    __asm__ volatile("ibar 0" ::: "memory");
}

ARKRTOS_WEAK void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
}

/* ============================================================
 * Flash 接口（QEMU 无片内 Flash，Load、App 装载均未启用）
 * ============================================================ */
ARKRTOS_WEAK int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    (void)addr;
    (void)size;
    return -1;
}

ARKRTOS_WEAK int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr;
    (void)data;
    (void)len;
    return -1;
}

ARKRTOS_WEAK uint32 arkrtos_port_flash_sector_size(uint32 addr)
{
    (void)addr;
    return 0u;
}

ARKRTOS_WEAK int32 arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out)
{
    (void)p_out;
    return -1;
}

/* ============================================================
 * 看门狗（QEMU virt 没有 IWDG，明确报「没有」而不是假装喂了）
 * ============================================================ */
ARKRTOS_WEAK int32 arkrtos_port_wdg_init(uint32 timeout_ms)
{
    (void)timeout_ms;
    return -1;
}

ARKRTOS_WEAK void arkrtos_port_wdg_feed(void)
{
}

ARKRTOS_WEAK uint32 arkrtos_port_wdg_timeout_ms(void)
{
    return 0u;
}

/* ============================================================
 * 致命异常报告：默认静默停机，板级覆盖后打到串口
 * ============================================================ */
ARKRTOS_WEAK void arkrtos_port_fatal_report(uint32 code)
{
    (void)code;
}
