/**
* @file arkrtos_port.c
* @brief Ark RTOS LoongArch64 移植层实现
* @details 实现 arkrtos_hal.h 声明的全部硬件抽象接口。目标平台是 QEMU 的
*          `virt` 机器（单核、PLV0、无页表），但本文件里只有 CSR 编号、
*          定时器与 UART 的常量是平台相关的，其余都是 LoongArch 架构本身
*          的约定，换到真实的 LA64 芯片时只需要改板级头。
*
*          ## 与 Cortex-M / RISC-V 移植的对应关系
*
*          | Cortex-M              | RISC-V                  | 本移植                       |
*          |-----------------------|-------------------------|------------------------------|
*          | SVC 异常              | `ecall`（号经 a7）      | `syscall code`（号在指令里） |
*          | PendSV 切换           | CLINT 软件中断          | **就地切换**（无软中断可用） |
*          | SysTick 节拍          | CLINT 机器定时器        | TCFG/TICLR 定时器            |
*          | 硬件自动压栈异常帧    | trap_entry 手写 144 字节| eentry 手写 256 字节帧       |
*          | PRIMASK               | mstatus.MIE             | CRMD.IE                      |
*          | IPSR != 0 判在中断里  | 软件计数 s_in_trap      | 软件计数 s_in_trap           |
*
*          ## 为什么没有「挂起切换」
*
*          LoongArch 的 CSR.IPI 只向**其他核**发中断：单核上 `csrwr` 自触发
*          实测不产生任何中断（docs 里有探针记录），所以没有 RISC-V 中
*          MSIP/PendSV 那样的「置挂起位、异常返回后再进一次 trap」的机制。
*          于是切换分成两种落点，共用同一种 256 字节帧：
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
*          1) **PLV3 用户态隔离已打开**（ARKRTOS_USE_PRIV=1）：内核低 16 MB、
*             内核 UART 都是 PLV0 页，App 只拿到一页 2 MB（PLV3）加一页 UART
*             别名。App 碰内核页 = PPI(7)、碰未映射地址 = 重填异常(1/2/3)、
*             执行 CSR = IPE(14)，三条都走内核的故障恢复而不是停机。
*             **没有做的**：没有软件页表（TLB 条目是启动时写死的静态映射，
*             没有按需换页、没有 swap）、App 页是整页 RWX（不做 W^X）、
*             App 的 sp 由 App 自己决定 —— 它若指向内核页，trap 帧就会按它
*             给的位置写；
*          2) 普通异常（eentry）与被中断的任务共用同一个栈（没有独立 handler
*             栈）；只有 TLB 重填这条入口用静态故障栈（la_refill_stack），
*             因为重填可能是「被打断的 sp 本身就是坏的」引起的；
*          3) trap 不可嵌套：进异常时 CRMD.IE 已被硬件清零，处理程序全程关中断；
*          4) `syscall` 之后 ERA 由本层加 4（硬件不会自己前进，不加上就是死循环）；
*          5) **64 位指针、32 位内核**：内核的地址字段是 uint32，本 port 在两处
*             （sched_activate 的帧指针、任务栈指针）显式截断到 32 位再回填。
*             QEMU virt 的 RAM 全在低 4 GB 内，所以截断不丢信息；换到高地址
*             布局的机器上必须先改内核的地址宽度，不能靠这个 port。
*
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_fault.h"

/* 板级输出。port 层不包含 board/<板>/arkrtos_board.h（换板时这个头会换），
 * 这几个函数是唯一需要解析的板级符号，就地声明即可。 */
extern void arkrtos_board_puts(const char *s);
extern void arkrtos_board_putu(uint32 val);
extern void arkrtos_board_puthex(uint32 val);
#include "arkrtos_config.h"
#if (ARKRTOS_USE_SMP == 1)
#include "arkrtos_smp.h"
#endif
#if (ARKRTOS_USE_SMP_SCHED == 1)
#include "arkrtos_smp_sched.h"
#endif

#if (ARKRTOS_USE_RTSTAT == 1)
#include "arkrtos_rt.h"
#endif

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG
#endif

/* ============================================================
 * CSR 编号（LoongArch 手册固定值，不随平台变化）
 * ============================================================ */
#define LA_CSR_CRMD         0x0
#define LA_CSR_PRMD         0x1
#define LA_CSR_ECFG         0x4
#define LA_CSR_ESTAT        0x5
#define LA_CSR_ERA          0x6
#define LA_CSR_EENTRY       0xC
#define LA_CSR_TCFG         0x41
#define LA_CSR_TVAL         0x42
#define LA_CSR_TICLR        0x44
#define LA_CSR_TID          0x40        /* 只读：本核硬件号 */

/* 分页 / TLB 相关（PRIV=1 时才用到）。TLBEHI 的低 13 位是 VPPN 的起点，
 * 所以「页基址」直接写进 TLBEHI 就是对的，不需要移位。 */
#define LA_CSR_BADV         0x7
#define LA_CSR_TLBIDX       0x10
#define LA_CSR_TLBEHI       0x11
#define LA_CSR_TLBELO0      0x12
#define LA_CSR_TLBELO1      0x13
#define LA_CSR_ASID         0x18
#define LA_CSR_STLBPS       0x1e        /* STLB 页大小（PS 在 [4:0]） */
#define LA_CSR_TLBRENTRY    0x88
#define LA_CSR_TLBRBADV     0x89
#define LA_CSR_TLBRERA      0x8A
#define LA_CSR_TLBRELO0     0x8C
#define LA_CSR_TLBRELO1     0x8D
#define LA_CSR_TLBREHI      0x8E        /* 重填入口用的 entryhi（PS 在 [5:0]，VPPN 从 bit13 起） */
#define LA_CSR_TLBRPRMD     0x8F

/* CRMD：PLV[1:0]、IE[2]、DA[3]、PG[4] */
#define LA_CRMD_IE          (1u << 2)
#define LA_CRMD_DA          (1u << 3)   /* 直接地址模式：不翻译、也不做任何 PLV 检查 */
#define LA_CRMD_PG          (1u << 4)   /* 分页使能：访存经 TLB 翻译并做权限检查 */

/* PRMD：PPLV[1:0]、PIE[2]、PWE[3]（位序与 CRMD 低 4 位相同，容易记反）。
 * `ERTN` 按 PRMD.PPLV 写 CRMD.PLV、按 PRMD.PIE 写 CRMD.IE，
 * 因此「回到 PLV0 且开中断」= PPLV=0 + PIE=1 = **0x4**，不是 0x1。 */
#define LA_PRMD_INIT        (0x4u)

/* ESTAT：IS[12:0]、Ecode[21:16]、EsubCode[30:22] */
#define LA_ESTAT_IS_TIMER   (1u << 11)      /* TI：定时器中断 */
#define LA_ESTAT_ECODE_SYS  (0xBu)          /* SYS：syscall    */
#define LA_ESTAT_ECODE_INT  (0x0u)          /* 中断（Ecode=0，靠 IS 分源） */
#define LA_ESTAT_ECODE_PPI  (0x7u)          /* PPI：页存在但 PLV 不够（App 摸内核页） */
#define LA_ESTAT_ECODE_IPE  (0xEu)          /* IPE：特权指令（App 执行 CSR / idle） */

/* ECFG：LIE[12:0]、VS[15:13]。定时器 = LIE[11] */
#define LA_ECFG_LIE_TIMER   (1u << 11)

/* TCFG：En[0]、Periodic[1]、InitVal[31:2] */
#define LA_TCFG_EN          (1u << 0)
#define LA_TCFG_PERIODIC    (1u << 1)

/* syscall 指令编码：`syscall code` = 0x002B0000 | code(15 位) */
#define LA_INSN_SYSCALL_OP  (0x002B0000u)
#define LA_INSN_SYSCALL_NUM (0x7FFFu)

/* 定时器计数时钟：QEMU virt 实测 TCFG 计数 25 MHz（InitVal=2000000 时
 * 10 s 内 125 次中断，误差 <1%）。换平台由板级头覆盖。 */
#ifndef ARKRTOS_PORT_TIMER_HZ
#define ARKRTOS_PORT_TIMER_HZ   25000000u
#endif

/* rdtime 计数时钟：QEMU virt 实测 100 MHz（rdtime 1e9 ticks ≈ 10.0 s，
 * 与主机时间差 1.9%，含启动开销）。这个时钟给 delay_us 与
 * get_timer_counter 用，与上面的定时器时钟是两个独立的计数源。 */
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
#define LA_FR_ERA           (30u)   /* 偏移 240 */
#define LA_FR_PRMD          (31u)   /* 偏移 248 */
#define LA_FR_WORDS         (32u)   /* 256 字节 */
#define LA_FR_SIZE          (256u)

/* 帧槽是 **8 字节**（LA64 的通用寄存器宽度），所以 C 侧绝不能用 uint32*
 * 去索引：uint32 在 LP64 下是 4 字节，frame[i] 只会走到真实槽位的低半部，
 * 32 个索引只覆盖 128 字节。用这个类型取代原来的 uint32*。 */
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

#if (ARKRTOS_USE_PRIV == 1)
/* 下一个要恢复的任务的 PLV（0 = 内核任务，3 = App）。
 * arkrtos_port_set_thread_priv() 只写这里：它被调用时（在
 * arkrtos_sched_activate() 内部）手上没有帧指针，而 ertn 只认帧里的
 * PRMD.PPLV —— 帧由拿到的返回值补写，见 LA_PRIV_APPLY。 */
static uint32 s_next_pplv = 0u;
#endif

/* 异常码短名（定义在异常分发一节，故障打印要用，这里先声明） */
static const char *la_ecode_name(uint32 ecode);

/* App 侧故障计数：只增不减，给「真的拒了几次」一个可核对的数。 */
volatile uint32 la_cnt_ppi = 0u;
volatile uint32 la_cnt_ipe = 0u;
volatile uint32 la_cnt_refill = 0u;
volatile uint32 la_cnt_other = 0u;

/* 外部符号：异常入口与汇编入口 */
extern void eentry(void);
extern void arkrtos_port_resume_task(uint32 stack_ptr);
extern void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);

#if (ARKRTOS_USE_PRIV == 1)
/* TLB 重填入口（arkrtos_context.S）。重填时硬件不保存任何通用寄存器，
 * 入口第一件事就是换到静态故障栈，见 la_refill_c()。 */
extern void tlb_refill_entry(void);
/* App 故障的派发入口（同上，换栈后再进 C，见 arkrtos_context.S） */
extern void la_app_fault_switch(la_frame_word *frame, uint32 ecode);
#endif

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

static uint32 la_rdtime(void)
{
    uint32 lo;

    __asm__ volatile("rdtime.d %0, $zero" : "=r"(lo) : : "memory");
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
            /* 值已被别人改过：直接放弃，不写回。
             * 不要用 "sc.w $zero, addr, 0" 撤销保留——sc.w 若成功会把 0
             * 真的写进 *p_addr（本仓库踩过：在线掩码被静默清零，表现为
             * 次核明明上线却一直读到 mask=0）。ll 的保留由本核下一次
             * ll/sc 或 ertn 自然失效。 */
            return 0u;
        }

        /* sc.w rd, rj, si14：rd 既是"要写的值"也是"成功标志"（0=成功）。
         * 所以必须先用 or 把 new_value 放进 rd，再用 p_addr 当地址。
         * 写成 "sc.w %0, %2, 0"（rd 是输出、rj 是 new_value）会把
         * new_value 当地址、把 rd 里的垃圾当数据写出去——不报错，静默
         * 错答案：CAS 返回成功但目标内存一个字都没改。 */
        __asm__ volatile("or   %0, %2, $zero\n\t"
                         "sc.w %0, %1, 0"
                         : "=&r"(fail)
                         : "r"(p_addr), "r"(new_value)
                         : "memory");
        if(fail == 0u)
        {
            return 1u;                    /* 手册语义：rd=0 表示成功 */
        }
        /* QEMU 的 LoongArch 后端把 SC 的「成功」编码成 rd=1（与手册相反）：
         * 实测 sc.w 明明把 0x123 写进了内存，rd 却读到 1。只看 rd 就会把
         * 「已经写成功」判成失败，重试时 ll 读到自己刚写的值又走
         * old != expect 分支返回 0 —— 锁字被置 1 却没有持有者，单核上直接
         * 死等（表现为整机停摆：lock=1 / owner=0xffffffff / nest=0）。
         * 所以加第二判据：回读内存，值已经是 new_value 就认定成功。
         * 锁字 0->1 这种一次性迁移下该判据不会误判；理论上的 ABA（别人把
         * 同一个 new_value 写回去）要求对手已经赢下这次 CAS，且只影响多核
         * 同时抢同一把锁的场景。 */
        if(*(volatile uint32 *)p_addr == new_value)
        {
            return 1u;
        }
    } while(1);
}

uint32 arkrtos_port_cpu_id(void)
{
#if (ARKRTOS_USE_SMP == 1)
    /* CSR.TID 是只读的"定时器 ID"，其值就是硬件核号：QEMU 在 cpu reset 里
     * 写 env->CSR_TID = cs->cpu_index（target/loongarch/cpu.c:551）。读 CSR
     * 不需要 TLB，也不受 CRMD.DA 影响，所以次核一进 C 就能用。
     *
     * 关档（默认）下仍然返回常量 0：默认镜像的字节不许因为这一条变动。 */
    unsigned long tid;

    /* 不用 LA_CSRR 宏：它定义在本文件后半段（分页那一节），这个函数在它
     * 之前，宏还不存在（症状是链接期报 undefined reference to LA_CSRR）。 */
    __asm__ volatile("csrrd %0, 0x40" : "=r"(tid) : : "memory");
    return (uint32)(tid & 0x3FFu);        /* 规范里 TID 的核号在低 10 位 */
#else
    return 0u;
#endif
}

void arkrtos_port_spin_hint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

#if (ARKRTOS_USE_SMP == 1)
/* ------------------------------------------------------------
 * SMP：次核启动与核间中断（QEMU virt / Loongson IPI）
 *
 * @details 2026-11-19 从 QEMU 9.2.3 源码 + 实跑确认的协议：
 *          -kernel 直启模式下，CPU0 的复位 PC 是内核入口，**其余核**的复位
 *          PC 是 VIRT_FLASH0_BASE，那里烧着 hw/loongarch/boot.c 的
 *          slave_boot_code。这段固件做四件事：把 EENTRY 清零、清 CRMD.IE、
 *          打开 ECFG 的 IPI 位并把本核 CORE_EN 写 0xfff，然后在
 *          `idle 0` 里轮询自己的 IOCSR mailbox(0x1020)；mailbox 非 0 就
 *          读它当入口地址跳过去。所以「启动次核」= 写 mailbox + 发一个 IPI。
 *          **次核根本不经过我们 ELF 的 _start**，它的第一段我方代码是
 *          arkrtos_context.S 里的 arkrtos_smp_secondary_entry。
 *
 *          IOCSR 窗口（include/hw/intc/loongson_ipi_common.h，
 *          SMP_IPI_MAILBOX = 0x1000，不是 0x10000）：
 *            0x1000 STATUS    读：本核挂起的 IPI 向量位图
 *            0x1004 EN        读写：本核使能的向量位图
 *            0x100C CLEAR     写：清挂起（写 1 的位被清）
 *            0x1020 BUF0      次核入口地址（mailbox 槽 0）
 *            0x1040 IPI_SEND  写 32 位：(目标核号 << 16) | 向量号(0..31)
 *            0x1048 MAIL_SEND 写 64 位：(地址 << 32) | (核号 << 16)，只认 8 字节访问
 *
 *          MAIL_SEND 的 bit27..30 是**保留原值的字节掩码**（QEMU 里 mask=0
 *          表示四个字节全用新值；写 0xF 反而一个字节也写不进去），所以这里
 *          只给核号、掩码位保持 0。
 *
 *          入口地址必须是**物理地址**：次核从 pflash 跳过来时 CRMD.DA=1，
 *          不做地址翻译。本镜像链接在 0x00200000 且恒等映射，直接取符号地址。
 *
 *          2026-11-19 范围：② 次核启动 ③ IPI 通路 已跑通；④ 调度分核由
 *          独立模块 arkrtos_smp_sched.c 承担（ARKRTOS_USE_SMP_SCHED，默认
 *          关）。打开后次核有自己的运行队列与节拍，任务由调用方显式迁移；
 *          **全局调度表/就绪位图/延时链仍然只有 CPU0 一份**，所以这不是
 *          对称多核，两件事不能混着说。
 * ------------------------------------------------------------ */
#define LA_IOCSR_IPI_STATUS     0x1000u
#define LA_IOCSR_IPI_CLEAR      0x100Cu
#define LA_IOCSR_IPI_SEND       0x1040u
#define LA_IOCSR_MAIL_SEND      0x1048u

/* 次核入口（arkrtos_context.S）：读 CSR.TID 选自己的栈、装 EENTRY/ECFG/
 * CORE_EN、开 IE，然后进 arkrtos_smp_secondary_main()。 */
extern void arkrtos_smp_secondary_entry(void);

static void la_iocsr_write_w(uint32 addr, uint32 val)
{
    /* GNU as 语法：iocsrwr.w rd, rj —— 把 rd 写进 rj 指向的 IOCSR。 */
    __asm__ volatile("iocsrwr.w %0, %1" : : "r"(val), "r"(addr) : "memory");
}

static void la_iocsr_write_d(uint32 addr, unsigned long val)
{
    __asm__ volatile("iocsrwr.d %0, %1" : : "r"(val), "r"(addr) : "memory");
}

int32 arkrtos_port_cpu_start(uint32 cpu)
{
    unsigned long entry = (unsigned long)&arkrtos_smp_secondary_entry;

    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        return -1;
    }

    /* 先放入口地址、再发 IPI：反过来的话次核可能从 mailbox 读到 0 直接跳 0。 */
    la_iocsr_write_d(LA_IOCSR_MAIL_SEND,
                     (entry << 32) | ((unsigned long)cpu << 16));

    la_iocsr_write_w(LA_IOCSR_IPI_SEND, (cpu << 16) | 1u);
    return 0;
}

int32 arkrtos_port_send_ipi(uint32 cpu)
{
    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        /* 核 0 是引导核：本移植没有给主核装 IPI 处理路径（它一进异常就落到
         * la_trap_c 的通用链路），所以拒绝自投，明确报 -1，而不是让主核
         * 掉进一个没处理过的中断里。 */
        return -1;
    }

    la_iocsr_write_w(LA_IOCSR_IPI_SEND, (cpu << 16) | 1u);
    return 0;
}
#endif /* ARKRTOS_USE_SMP == 1 */

/* ============================================================
 * 分页与 TLB（PLV3 用户态隔离）
 * @details LoongArch 上「保护」只有一条路：分页 + TLB 条目里的 PLV。
 *          CRMD.DA=1 时地址直接当物理地址用，**连 PLV 检查都不做** ——
 *          这就是旧版本「只跑 PLV0、没有隔离」的真相。打开 PG 之后每次
 *          访存都要先过条目的 V / PLV / WE / NX 判定，越权变成异常。
 *
 *          这里不用软件页表：QEMU 9.2.3 的 loongarch 目标 cpu->ptw = OFF，
 *          既没有硬件遍历器，也没有 PWCL/PWCH 的配置，所以走「启动时
 *          一次性写死 TLB 条目」的静态映射（四条）：
 *
 *          | 区域                       | VA            | PS | PLV | 权限 |
 *          |----------------------------|---------------|----|-----|------|
 *          | 内核低 16 MB（镜像/数据/栈）| 0x00000000   | 22 |  0  | RWX  |
 *          | 内核 UART                  | 0x1FE00000    | 12 |  0  | RW   |
 *          | App 页（代码/数据/栈）     | APP_BASE      | 20 |  3  | RWX  |
 *          | App 的 UART 别名           | APP_UART 页   | 12 |  3  | RW   |
 *
 *          一条条目管「偶页 + 奇页」两页（LO1 的 PA 恒等于 LO0 + 2^PS），
 *          所以 PS=22 的一条覆盖 8 MB，低 16 MB 两条就够。
 *
 *          PLV3 的页对 PLV0 **也**可访问（RPLV=0 时判 PLV <= 条目 PLV），
 *          于是内核读写 App 页（trap 帧、栈帧重建）不用额外映射，
 *          任务切换也不必重编程 TLB —— 切换只改帧里的 PRMD.PPLV。
 * ============================================================ */

/* App 页几何：板级头可覆盖。默认 2 MB @ 0x02000000（QEMU DRAM 内、
 * 内核镜像与内核 RAM 之外的空地，见 board/qemu_loongarch64 的几何注释）。 */
#ifndef ARKRTOS_QEMU_APP_BASE
#define ARKRTOS_QEMU_APP_BASE       0x02000000u
#endif
#ifndef ARKRTOS_QEMU_APP_SIZE
#define ARKRTOS_QEMU_APP_SIZE       (2u * 1024u * 1024u)
#endif
/* App 能直接写的 UART 别名地址（含页内偏移）。
 * PS=12 的页映射保留页内偏移，所以 0x02200000 页里的 0x1E0 就对应
 * PA 0x1FE001E0 —— App 用的就是这个完整地址，不是页基址。 */
#ifndef ARKRTOS_QEMU_APP_UART
#define ARKRTOS_QEMU_APP_UART       0x022001E0u
#endif

/* 页级窗口（U-3 第二阶）的几何。一条 PS=12 的条目管**相邻两页**
 * （TLBELO0 = vaddr、TLBELO1 = vaddr + 4 KB），所以 2 MB 窗口 = 512 页 =
 * 256 条条目，正好铺满 STLB 的一个路（256 组）：
 *     组号 = (VA >> 13) & 0xFF，窗口基址 0x02000000 的组号是 0
 *     ⇒ 条目号 = way * 256 + i
 * 窗口占 way 0；两条 4 KB 的 UART 页在打开这一档时改走 way 7 / 6，避免与
 * 窗口第一条（同组 0）抢同一路 —— tlbfill 的路号是随机的，撞上一次就
 * 顶掉窗口的映射，症状是 App 第一次取指就 refill。 */
#define LA_PG_WIN_PAGES      ((uint32)(ARKRTOS_QEMU_APP_SIZE / 0x1000u))
#define LA_PG_WIN_ENTRIES    ((uint32)(LA_PG_WIN_PAGES / 2u))
#define LA_PG_WIN_WAY        0u
#define LA_PG_KUART_WAY      7u
#define LA_PG_AUART_WAY      6u

/* 页级窗口的组合合法性：它只能长在「分页 + 帧池」之上，缺一个就直接
 * 编不过 —— 那种档跑起来的表现是「窗口一半映射、一半 refill」，最难查。 */
#if (ARKRTOS_USE_PAGING_PAGES == 1)
#if (ARKRTOS_USE_PAGING != 1)
#error "ARKRTOS_USE_PAGING_PAGES=1 requires ARKRTOS_USE_PAGING=1"
#endif
#if (ARKRTOS_USE_PAGING_FRAMES != 1)
#error "ARKRTOS_USE_PAGING_PAGES=1 requires ARKRTOS_USE_PAGING_FRAMES=1"
#endif
#endif

/* 按需缺页得长在页级窗口上（它改的是「什么时候装那 256 条条目」），
 * 而且它要在 refill 里跑，所以还得有特权级那一档。 */
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
#if (ARKRTOS_USE_PAGING_PAGES != 1)
#error "ARKRTOS_USE_PAGING_DEMAND=1 requires ARKRTOS_USE_PAGING_PAGES=1"
#endif
#if (ARKRTOS_USE_PRIV != 1)
#error "ARKRTOS_USE_PAGING_DEMAND=1 requires ARKRTOS_USE_PRIV=1"
#endif
#endif

/* 页替换长在按需缺页上：只有按需档才有「某一对还没驻留」这个状态可换。
 * 关档时下面整段（账本、交换区、中继缓冲）一个字节都不编进镜像。 */
#if (ARKRTOS_USE_PAGING_SWAP == 1)
#if (ARKRTOS_USE_PAGING_DEMAND != 1)
#error "ARKRTOS_USE_PAGING_SWAP=1 requires ARKRTOS_USE_PAGING_DEMAND=1"
#endif
#if (ARKRTOS_USE_PAGING_FRAMES != 1)
#error "ARKRTOS_USE_PAGING_SWAP=1 requires ARKRTOS_USE_PAGING_FRAMES=1"
#endif
/* 交换槽数：一个槽就是一整对 8 KB。8 个槽 = 64 KB .bss，够「少留几对常驻、
 * 多留几对在盘上」这种实验，又不至于把 QEMU 的镜像撑大。（这里说的「盘上」
 * 就是这个静态区，理由见 arkrtos_features.h 的开关说明。） */
#ifndef ARKRTOS_PAGING_SWAP_SLOTS
#define ARKRTOS_PAGING_SWAP_SLOTS   8u
#endif
#if (ARKRTOS_PAGING_SWAP_SLOTS == 0u) || (ARKRTOS_PAGING_SWAP_SLOTS > 64u)
#error "ARKRTOS_PAGING_SWAP_SLOTS must be 1..64 (one slot holds one 8 KB pair)"
#endif
#endif

/* App 帧合法窗：App 任务的 trap 帧落在 App 页里（PLV3 页，内核可读写）。 */
#define LA_FRAME_IN_APP(sp) \
    ((((uint32)(sp)) >= ARKRTOS_QEMU_APP_BASE) && \
     (((uint32)(sp)) < (ARKRTOS_QEMU_APP_BASE + ARKRTOS_QEMU_APP_SIZE)))

#if (ARKRTOS_USE_PRIV == 1)

/* TLBELO 位域（64 位形式）：V/WE/PLV/CCA/G + PPN[12+] + NR/NX/RPLV。
 * PPN 只保留 bit12 以上：QEMU 会把条目里位于 [12, PS-1] 的软件位清掉，
 * 留着只是自找「地址对不上」。 */
#define LA_PTE_V            (1UL << 0)
#define LA_PTE_WE           (1UL << 1)
#define LA_PTE_PLV(n)       ((unsigned long)(n) << 2)
#define LA_PTE_CCA(n)       ((unsigned long)(n) << 4)
#define LA_PTE_G            (1UL << 6)
#define LA_PTE_PPN(pa)      ((unsigned long)(pa) & ~0xFFFUL)

/* 内核 RAM：恒等映射、可缓存（CCA=1）、全局、PLV0 */
#define LA_PTE_KERNEL(pa)   (LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(1u) | LA_PTE_G | \
                             LA_PTE_PLV(0u) | LA_PTE_PPN(pa))
/* MMIO：强序不缓存（CCA=0） */
#define LA_PTE_MMIO(pa)     (LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(0u) | LA_PTE_G | \
                             LA_PTE_PLV(0u) | LA_PTE_PPN(pa))
/* App 普通页：PLV3 ⇒ PLV0..3 都可访问 */
#define LA_PTE_APP(pa)      (LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(1u) | \
                             LA_PTE_PLV(3u) | LA_PTE_PPN(pa))
/* App 的 MMIO 别名：同样 PLV3，但不缓存 */
#define LA_PTE_APP_MMIO(pa) (LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(0u) | \
                             LA_PTE_PLV(3u) | LA_PTE_PPN(pa))

/* 表索引：STLB 是 8 路 × 256 组（组号 = (VA >> 13) & 0xFF，路号自己挑），
 * MTLB 是 2048 起的 64 条绝对条目。 */
#define LA_STLB_WAYS        8u
#define LA_MTLB_INDEX       2048u
#define LA_STLB_INDEX(va, way) \
    ((((uint32)(way)) % LA_STLB_WAYS) * 256u + ((((uint32)(va)) >> 13) & 0xFFu))

#define LA_STR_(x)          #x
#define LA_STR(x)           LA_STR_(x)

/* CSR 读写。`csrwr` 的 CSR 号必须是汇编期立即数，这里用「先展开再字符串化」
 * 绕开：LA_CSRW(LA_CSR_TLBIDX, v) → csrwr %0, 0x10。 */
#define LA_CSRW(n, v)       do { unsigned long _la_cv = (unsigned long)(v); \
                                 __asm__ volatile("csrwr %0, " LA_STR(n) \
                                                  : "+r"(_la_cv) : : "memory"); \
                            } while(0)
#define LA_CSRR(n, out)     do { unsigned long _la_cv; \
                                 __asm__ volatile("csrrd %0, " LA_STR(n) \
                                                  : "=r"(_la_cv) : : "memory"); \
                                 (out) = _la_cv; \
                            } while(0)

/**
* @brief 填一条 TLB 条目
* @param vaddr 虚拟地址基址（必须按 2^(ps+1) 对齐）
* @param ps    页大小指数（12 = 4 KB，22 = 4 MB）
* @param lo0   偶页 TLBELO（映射 vaddr）
* @param lo1   奇页 TLBELO（映射 vaddr + 2^ps）
* @details 走 `tlbfill` 而不是自己算索引：QEMU 的 tlbfill 按 PS 自动选表 ——
*          PS == STLBPS(12) 进 STLB（8 路 × 256 组），PS != 12 进 MTLB
*          （绝对索引 2048..2111）。而**查找**时 STLB 的每一路都按
*          stlb_ps(12) 去比 VPPN，所以大页条目一旦被塞进 STLB 就永远匹配
*          不上（症状是「地址明明有映射却一直报重填」）。tlbfill 替你
*          选了正确的表，这是用它而不是手工写索引的唯一理由。
*/
static void la_tlb_fill(uint32 vaddr, uint32 ps, uint32 index,
                        unsigned long lo0, unsigned long lo1)
{
    /* TLBIDX = INDEX[23:0] | PS[29:24] | NE[31]（NE=0 ⇒ 写 INDEX 指定的条目） */
    LA_CSRW(LA_CSR_TLBIDX, ((unsigned long)ps << 24) | (unsigned long)(index & 0xFFFFFFu));
    LA_CSRW(LA_CSR_TLBEHI, (unsigned long)(vaddr & ~0x1FFFu));
    LA_CSRW(LA_CSR_TLBELO0, lo0);
    LA_CSRW(LA_CSR_TLBELO1, lo1);

    if(ps == 12u)
    {
        /* 4 KB 页进 STLB：它的组号由 VA 决定、路号随机挑 8 路之一。
         * 本移植的两条 4 KB 条目（内核 UART / App 的 UART 别名）分在不同的
         * 组里，所以随机路号不会互相覆盖，tlbfill 是够用的。 */
        __asm__ volatile("tlbfill" ::: "memory");
    }
    else
    {
        /* 大页进 MTLB：**必须用 tlbwr**。QEMU 9.2.3 的 helper_tlbfill 在
         * PS != STLBPS 时执行 `index = get_random_tlb(LOONGARCH_STLB,
         * LOONGARCH_TLB_MAX - 1)` —— 随机挑一条 MTLB 条目**覆盖掉**。
         * 只写三四条时撞上的概率约 5%，症状是「启动时读得到 App 页、
         * 跑一会儿就报 KERNEL refill」这种飘忽的失败（实测踩到过一次）。
         * tlbwr 认 TLBIDX.INDEX，是确定的。 */
        __asm__ volatile("tlbwr" ::: "memory");
    }
}

/**
* @brief 确定性写一条 TLB 条目（不经过 tlbfill 的随机选路）
* @param vaddr 虚拟地址基址
* @param ps    页大小指数
* @param index 绝对条目号（STLB 用 way*256+组，MTLB 用 2048 起）
* @details `tlbfill` 在 PS == STLBPS 时会替调用方随机挑一路，好处是不用
*          知道 STLB 的索引布局，坏处是运行期反复调用会**随机顶掉同组里
*          已有的条目**（内核 UART 与动态页恰好同组时就踩到过：登记完一条
*          动态映射，紧接着控制台第一次打印就 KERNEL refill）。
*          运行期登记不能用那种「碰运气」的写法，所以这里直接 `tlbwr`，
*          索引由调用方算好。
*
*          4 KB 页刻意写进 **MTLB**（2048 起的绝对条目）而不是 STLB：
*          MTLB 是确定索引、查找时按条目自己的 PS 比 VPPN，不会和内核那几条
*          静态 4 KB 条目抢同一组的 8 路，也不需要知道 STLB 的 way 布局。
*          代价是 MTLB 只有 64 条，所以动态表的上限由它决定（见 LA_PG_DYN_PAIRS）。
*/
static void la_tlb_write(uint32 vaddr, uint32 ps, uint32 index,
                         unsigned long lo0, unsigned long lo1)
{
    LA_CSRW(LA_CSR_TLBIDX, ((unsigned long)ps << 24) | (unsigned long)(index & 0xFFFFFFu));
    LA_CSRW(LA_CSR_TLBEHI, (unsigned long)(vaddr & ~0x1FFFu));
    LA_CSRW(LA_CSR_TLBELO0, lo0);
    LA_CSRW(LA_CSR_TLBELO1, lo1);
    __asm__ volatile("tlbwr" ::: "memory");
}

#if (ARKRTOS_USE_PAGING_ASID == 1)
/**
* @brief 读 CSR.ASID（10 位，本 port 只用到 1..ARKRTOS_PAGING_APPS_MAX）
*/
static unsigned long la_asid_get(void)
{
    unsigned long v = 0UL;

    LA_CSRR(LA_CSR_ASID, v);
    return v & 0x3FFUL;
}

/**
* @brief 换 CSR.ASID，返回换之前的值
* @param asid 目标 ASID（只取低 10 位）
* @details `csrwr` 把旧值写回同一个寄存器，所以一次指令就够 —— 切换路径上
*          每一次多余的 CSR 访问都是白花钱。
*/
static unsigned long la_asid_swap(unsigned long asid)
{
    unsigned long v = (unsigned long)(asid & 0x3FFUL);

    __asm__ volatile("csrwr %0, " LA_STR(LA_CSR_ASID) : "+r"(v) : : "memory");
    return v & 0x3FFUL;
}
#endif

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/**
* @brief 在 TLB 重填上下文里装一条 4 KB 对（只能在 refill 入口里用）
* @param vaddr 页对基址（8 KB 对齐）
* @param lo0   偶页 TLBELO、lo1 奇页 TLBELO
* @details **为什么不能在这里用 la_tlb_write()**：重填异常里 TLBRERA.ISTLBR=1，
*          此刻 `tlbwr` 与 `tlbfill` 取的都不是 TLBEHI/TLBELO0/TLBELO1，而是
*          TLBR* 那一组镜像 CSR。实测（QEMU 9.2.3）：重填处理里写 TLBEHI 再
*          `tlbwr`，条目按硬件填的 TLBREHI（= 出错地址的 VPPN）落进去、
*          TLBELO 取 TLBRELO0/1 的旧值 ⇒ V=0，于是**下一次访问变成
*          TLBRET_INVALID 走 eentry 的 PIS**（不是重填，因为 tlbfill 只对
*          "查不到"置 ISTLBR），直接撞上「PLV0 致命异常」停机。
*          正确形态就是硬件重填处理惯用的那一套：写 TLBREHI/TLBRELO0/
*          TLBRELO1 再 `tlbfill`。TLBREHI 的 PS 字段在 [5:0]，VPPN 从 bit13
*          起 ⇒ 页基址直接或上 12 就是完整值。
*          索引由 tlbfill 按 VA 自己算（PS==STLBPS 时进 STLB，way 0..7 由
*          硬件随机挑）：窗口里每个 8 KB 对占一个 STLB 组、一组 8 路只用 1
*          路，所以随便落哪一路都不会与别的对撞。
*/
static void la_tlb_fill_refill(uint32 vaddr, unsigned long lo0, unsigned long lo1)
{
    LA_CSRW(LA_CSR_TLBREHI, (unsigned long)(vaddr & ~0x1FFFu) | 12UL);
    LA_CSRW(LA_CSR_TLBRELO0, lo0);
    LA_CSRW(LA_CSR_TLBRELO1, lo1);
    __asm__ volatile("tlbfill" ::: "memory");
}
#endif

/**
* @brief 建立静态映射并打开分页
* @details 必须在开中断之前调用（节拍一进来就要过 TLB），也必须在
*          所有任务注册之后调用不需要 —— 帧内容与映射方式无关，
*          帧所在的地址在 DA 模式下写和分页模式下写是同一块内存。
*/
#if ((ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_PRIV == 1) && \
     (ARKRTOS_USE_PAGING == 1))
/* 定义在下面的分页后端一节（要用帧池与 la_tlb_write），这里先声明：
 * la_mmu_init() 打开分页之前必须把窗口铺好，否则第一条 App 取指就 refill。 */
static void la_pg_boot_map(void);
#endif

static void la_mmu_init(void)
{
    /* ASID 全 0：内核与 App 共用一份映射，切换任务不换页 */
    LA_CSRW(LA_CSR_ASID, 0UL);

    /* STLB 的页大小必须显式写成 4 KB（0xC）。这条不写就有一个只在
     * QEMU 上才现形的坑：STLBPS 实测复位值是 0，而 helper_tlbfill()
     * 是拿 `pagesize == STLBPS` 来判断「写 STLB 还是写 MTLB」的——
     * 我们按 4 KB 填 UART 页时 pagesize=12 与 stlb_ps=0 不等，QEMU 就
     * 当成大页去随机覆盖一条 MTLB 条目（get_random_tlb(2048, 2111)），
     * 于是内核那两条 PS=22 的大页会被陆续打掉，表现为「启动能跑，
     * 跑一会儿 KERNEL refill (plv0) -> fatal」，且 badv/era 每次不同。
     * 真实硬件上 STLBPS 复位即为 0xC，所以这是「QEMU 上必须写、
     * 硬件上写了也无害」的一条。 */
    LA_CSRW(LA_CSR_STLBPS, 12UL);

    /* 复位后 TLB 本来是空的，但「空」不写下来就只能靠记忆：显式清一次 */
    __asm__ volatile("invtlb 0, $zero, $zero" ::: "memory");

    /* 内核低 16 MB 恒等映射。两条 PS=22：
     *   0x00000000 → 覆盖 0x000000~0x7FFFFF（镜像 0x200000~、静态区 0x302000~）
     *   0x00800000 → 覆盖 0x800000~0xFFFFFF（主栈顶 0xC00000 在这里） */
    /* 大页必须落在 MTLB（绝对索引 2048 起，共 64 条）：STLB 的条目没有自己
     * 的 PS 字段，查找一律按 STLBPS(12) 比，塞进去的大页永远匹配不上。
     * 4 KB 页落 STLB，组号由 tlbfill 按 VA 自己算，index 参数对它们无效。 */
    la_tlb_fill(0x00000000u, 22u, LA_MTLB_INDEX + 0u,
                LA_PTE_KERNEL(0x00000000u), LA_PTE_KERNEL(0x00400000u));
    la_tlb_fill(0x00800000u, 22u, LA_MTLB_INDEX + 1u,
                LA_PTE_KERNEL(0x00800000u), LA_PTE_KERNEL(0x00C00000u));

    /* 内核 UART（PLV0、RW）：控制台 */
#if ((ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_PAGING_DEMAND == 1))
    /* 按需档：窗口条目是在重填入口里用 tlbfill 装的，way 由硬件在那一个
     * 组里**随机挑**（QEMU: get_random_tlb(0,7)），所以「给 UART 指定
     * way 7 让开」这条在按需档不成立 —— 实测 pmap u2 跑到一半控制台那一
     * 页被窗口第 0 对（同在第 0 组）随机顶掉，badv=0x1fe001e5 停机。
     * 窗口永远是 PS=12 ⇒ 永远落 STLB（0..2047），把两条 MMIO 页放进
     * MTLB 就再也不可能与窗口撞。MTLB 槽位分工写死在这里：
     *   +0/+1 内核两条 PS=22 大页、+2 App 整块（非页级档）、
     *   +3..+10 动态 alias（LA_MTLB_DYN_FIRST，8 对）、
     *   +11 页帧池 PS=22，+12/+13 就是这两条 MMIO 页 —— 必须避开
     *   +3..+10，否则 pmap test 一登记动态映射就把控制台页顶掉
     *   （实测：badv=0x1fe001e5，同一形态第二次踩到）。 */
    la_tlb_write(ARKRTOS_QEMU_UART_BASE & ~0xFFFu, 12u,
                 LA_MTLB_INDEX + 12u,
                 LA_PTE_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                 LA_PTE_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#elif (ARKRTOS_USE_PAGING_PAGES == 1)
    /* 页级窗口占了 way 0 的整套组，UART 页必须让开：显式写 way 7。 */
    la_tlb_write(ARKRTOS_QEMU_UART_BASE & ~0xFFFu, 12u,
                 LA_STLB_INDEX(ARKRTOS_QEMU_UART_BASE, LA_PG_KUART_WAY),
                 LA_PTE_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                 LA_PTE_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#else
    la_tlb_fill(ARKRTOS_QEMU_UART_BASE & ~0xFFFu, 12u,
                LA_STLB_INDEX(ARKRTOS_QEMU_UART_BASE, 0u),
                LA_PTE_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                LA_PTE_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#endif

    /* App 页（PLV3、RWX）：App 的代码、数据、栈全在这 2 MB 里。
     * **必须用 tlbwr 写固定索引**：这是唯一一条会在运行期被换掉的条目
     * （每 App 地址空间切换，见 arkrtos_port_paging_activate），索引必须
     * 已知。走 tlbfill 会被随机塞进某条 MTLB 槽，切换时往 2050 写一条新的，
     * 而旧条目仍留在别的槽里 —— 查找命中旧的那条，切换静默失效（实测：
     * space 1 写入的数据在 space 0 里被读到）。 */
#if (ARKRTOS_USE_PAGING_PAGES == 1)
    /* 页级窗口：从帧池给 space 0 拿一整块 2 MB，再铺 256 条 4 KB 条目。 */
    la_pg_boot_map();
#else
    la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u,
                 LA_PTE_APP(ARKRTOS_QEMU_APP_BASE),
                 LA_PTE_APP(ARKRTOS_QEMU_APP_BASE + 0x100000u));
#endif

    /* App 的 UART 别名（PLV3、RW）：同一台 16550，换个 VA。
     * 内核那页是 PLV0，App 碰不到，所以必须单独给一页 —— 这一页同时也是
     * 正对照：App 在自己的页里读写是允许的，被拒的只有内核页与未映射地址。
     * **它必须与内核 UART 落在不同的 STLB 组**：组号是 (VA >> 13) & 0xFF，
     * 内核 UART 在组 0x00，所以这里取 0x02202000（组 0x01）。同组时两条
     * 4 KB 条目会在同一组的 8 路里抢位置，而 tlbfill 的路号是随机的 ⇒
     * 控制台那一页会被顶掉。传进去的 index 对 4 KB 页无效（tlbfill 自己
     * 按 VA 选组），保留它只为让调用点与别的条目读起来对称。 */
#if ((ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_PAGING_DEMAND == 1))
    /* 同上：按需档的窗口 way 是随机的，App UART 别名也在组 1，干脆也放
     * MTLB +13（与上面 +12 一起避开动态映射的 +3..+10）。
     * （这一页同时是「App 能读自己的页」的正对照，不能丢。） */
    la_tlb_write(ARKRTOS_QEMU_APP_UART & ~0xFFFu, 12u,
                 LA_MTLB_INDEX + 13u,
                 LA_PTE_APP_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                 LA_PTE_APP_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#elif (ARKRTOS_USE_PAGING_PAGES == 1)
    la_tlb_write(ARKRTOS_QEMU_APP_UART & ~0xFFFu, 12u,
                 LA_STLB_INDEX(ARKRTOS_QEMU_APP_UART, LA_PG_AUART_WAY),
                 LA_PTE_APP_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                 LA_PTE_APP_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#else
    la_tlb_fill(ARKRTOS_QEMU_APP_UART & ~0xFFFu, 12u,
                LA_STLB_INDEX(ARKRTOS_QEMU_APP_UART, 1u),
                LA_PTE_APP_MMIO(ARKRTOS_QEMU_UART_BASE & ~0xFFFu),
                LA_PTE_APP_MMIO((ARKRTOS_QEMU_UART_BASE & ~0xFFFu) + 0x1000u));
#endif

#if (ARKRTOS_USE_PAGING_FRAMES == 1)
    /* 4 KB 页帧池（U-3）：一条 PS=22 恒等映射，让内核在分页模式下按物理
     * 地址直接读写帧本身。没有这一条，帧池就是一块在 PLV0 下也触不到的
     * 黑洞 —— 分配出去的帧只能用、不能被自检写图案再读回。
     * 落在 MTLB 的 2059（= LA_MTLB_INDEX + 11），避开动态映射那 8 个槽位
     * （2051..2058）。基址 8 MB 对齐、长度不超过 8 MB，两条编译期 #error
     * 在下面的帧池一节里拦。 */
    la_tlb_fill(ARKRTOS_QEMU_FRAME_BASE, 22u, LA_MTLB_INDEX + 11u,
                LA_PTE_KERNEL(ARKRTOS_QEMU_FRAME_BASE),
                LA_PTE_KERNEL(ARKRTOS_QEMU_FRAME_BASE + 0x400000u));
#endif

    /* 未映射地址的入口。硬件从这里进来时 CRMD.DA=1、PG=0、PLV=0、IE=0。 */
    LA_CSRW(LA_CSR_TLBRENTRY, (unsigned long)&tlb_refill_entry);

    /* 打开分页：先关 DA 再开 PG。下一条取指就走第一条条目了。 */
    {
        unsigned long crmd;

        LA_CSRR(LA_CSR_CRMD, crmd);
        crmd &= ~(unsigned long)LA_CRMD_DA;
        crmd |= (unsigned long)LA_CRMD_PG;
        LA_CSRW(LA_CSR_CRMD, crmd);
    }

    __asm__ volatile("ibar 0" ::: "memory");
}

/**
* @brief 把「下一个任务的特权级」写进要恢复的帧
* @param frame 即将被 ertn 恢复的帧
* @details 只改 PRMD.PPLV（低 2 位），PIE/PWE 原样保留：ertn 按
*          PRMD.PPLV 写 CRMD.PLV、按 PRMD.PIE 写 CRMD.IE，所以「回到
*          PLV3 且开中断」= PPLV=3 + PIE=1 = 0x7。
*/
static void la_frame_apply_priv(la_frame_word *frame)
{
    frame[LA_FR_PRMD] = (frame[LA_FR_PRMD] & ~3UL) | (unsigned long)s_next_pplv;
}

/* 关掉时退化为空操作：默认档的代码与行为一个字节都不变。 */
#define LA_PRIV_APPLY(f)    la_frame_apply_priv(f)

#else  /* ARKRTOS_USE_PRIV == 0 */

#define LA_PRIV_APPLY(f)    do { (void)(f); } while(0)

#endif /* ARKRTOS_USE_PRIV == 1 */

/* ============================================================
 * 分页隔离后端（ARKRTOS_USE_PAGING=1）
 * @brief 给 arkrtos_paging.c 提供四个 port 接口。
 * @details 本 port 的映射是**静态**的：la_mmu_init() 在开中断之前把
 *          内核低 16 MB、内核 UART、App 2 MB（PLV3）、App UART 别名
 *          四条填进 TLB，此后不再变动。任务切换只换 PLV（帧里的
 *          PRMD.PPLV），不换页表、不改 ASID。所以这里的 check() 是
 *          **审计**：查一段跨度是否落在那几条条目里、权限够不够。
 *          它不能撤销任何东西 —— 撤销需要动态地址空间，登记为欠账。
 * ============================================================ */
#if (ARKRTOS_USE_PAGING == 1)

/* 跨度表：与 la_mmu_init() 里的 la_tlb_fill() 调用一一对应。
 * mem 是允许的属性位掩码（位号 = arkrtos_mpu_mem_t 枚举值）。 */
#define LA_PG_BIT(m)            (1u << (uint32)(m))

typedef struct {
    uint32 base;
    uint32 size;
    uint32 mem;                 /* allowed arkrtos_mpu_mem_t kinds */
} la_pg_span_t;

static const la_pg_span_t s_pg_spans[] = {
    /* 内核恒等映射 0x00000000~0x00FFFFFF：PLV0、可读写、可执行 */
    { 0x00000000u, 16u * 1024u * 1024u,
      LA_PG_BIT(ARKRTOS_MPU_MEM_ROM) | LA_PG_BIT(ARKRTOS_MPU_MEM_RAM) },

    /* App 页 0x02000000 + 2 MB：PLV3、RWX，App 的代码/数据/栈都在里面 */
    { ARKRTOS_QEMU_APP_BASE, ARKRTOS_QEMU_APP_SIZE,
      LA_PG_BIT(ARKRTOS_MPU_MEM_ROM) | LA_PG_BIT(ARKRTOS_MPU_MEM_RAM) |
      LA_PG_BIT(ARKRTOS_MPU_MEM_RAMX) },

    /* 内核 UART（PLV0、不缓存）：只有内核任务用得到它 */
    { (ARKRTOS_QEMU_UART_BASE & ~0xFFFu), 0x1000u,
      LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RO) | LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RW) },

    /* App 的 UART 别名（PLV3、不缓存）：App 唯一的 MMIO 页 */
    { (ARKRTOS_QEMU_APP_UART & ~0xFFFu), 0x1000u,
      LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RO) | LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RW) },
};

#define LA_PG_SPAN_COUNT        (sizeof(s_pg_spans) / sizeof(s_pg_spans[0]))

/* 板级给了几个 App 地址空间。缺省 1 = 只有 space 0（PA 0x02000000），
 * 也就是这条机制出现之前的行为；多个空间要板级显式给数。 */
#ifndef ARKRTOS_PAGING_APPS_MAX
#define ARKRTOS_PAGING_APPS_MAX     1u
#endif

static uint32 s_pg_app_space;       /* 当前装进 App 窗口的空间号 */

/* ------------------------------------------------------------
 * ASID（U-7）：空间标签，切换只剩一次 CSR 写
 * @details 关档时 ASID 恒 0：切空间必须把窗口那 256 条条目整段失效再重铺，
 *          因为所有空间的条目在 TLB 里长得一模一样（同一个 VA、同一个
 *          ASID），留着就一定是别人的帧。
 *          开档时空间 s 对应 ASID s+1（0 留给内核自己的默认视图 = space 0），
 *          每个空间独占一路 STLB（way = s % 8）：
 *            - 条目按 ASID 区分 ⇒ 别的空间那 256 条**留着也无害**，
 *              它们只是「不匹配」；
 *            - 切换就是一次 `csrwr CSR.ASID`，不再 512 次 invtlb +
 *              256 次 tlbwr（关档每切一次都要付这一笔）。
 *          三处配套，缺一条就不成立：
 *            1. 内核自有映射（恒等映射、两条 UART 页、动态 MMIO 别名）
 *               必须是全局条目（TLBELO.G=1），否则换个 ASID 就看不见；
 *            2. 铺条目之前必须先切到该空间的 ASID —— 条目记的是写的那一刻
 *               的 CSR.ASID，不是空间的号码；
 *            3. `invtlb 3` 是**按 VA** 失效、不看 ASID ⇒ 它会连带清掉别的
 *               空间同 VA 的条目，所以撤映射之后要把当前空间补铺回来。
 * ------------------------------------------------------------ */
#if (ARKRTOS_USE_PAGING_ASID == 1)
#if (ARKRTOS_USE_PAGING_PAGES != 1)
#error "ARKRTOS_USE_PAGING_ASID=1 requires ARKRTOS_USE_PAGING_PAGES=1"
#endif
#if (ARKRTOS_USE_PRIV != 1)
#error "ARKRTOS_USE_PAGING_ASID=1 requires ARKRTOS_USE_PRIV=1"
#endif
#if (ARKRTOS_PAGING_APPS_MAX > 8u)
#error "ARKRTOS_USE_PAGING_ASID=1 gives every space one STLB way: ARKRTOS_PAGING_APPS_MAX must be <= 8"
#endif
#define LA_PG_WIN_WAY_OF(space)   ((uint32)(space) % LA_STLB_WAYS)
#define LA_PG_ASID_OF(space)      ((unsigned long)(space) + 1UL)
#define LA_PG_DYN_G               (LA_PTE_G)
#else
#define LA_PG_WIN_WAY_OF(space)   LA_PG_WIN_WAY
#define LA_PG_ASID_OF(space)      0UL
#define LA_PG_DYN_G               (0UL)
#endif

#if (ARKRTOS_USE_PAGING_PAGES == 1)
/* 页级窗口下每个空间的帧基址：0 = 还没从帧池分配（池基址至少 0x04000000，
 * 所以 0 不会与任何合法帧冲突）。空间一旦拿到帧就不还 —— 空间回收仍欠，
 * 与第一阶作为取舍登记在案的「没有使用方」变成「有使用方、但只借不还」。 */
static uint32 s_pg_space_base[ARKRTOS_PAGING_APPS_MAX];
#endif

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/* 按需档的账本：每个空间 256 个 8 KB 对（一条 PS=12 条目管相邻两页），
 * 0 = 这一对还没被摸过、没有帧。s_pg_space_base 在这档恒为 0 —— 空间不再
 * 有一个「整块 2 MB 的基址」，物理页是按对零散拿的。 */
static uint32 s_pg_pair_pa[ARKRTOS_PAGING_APPS_MAX][LA_PG_WIN_ENTRIES];
static uint32 s_pg_demand_faults;   /* 装过条目的次数（诊断用） */
#endif

/* ------------------------------------------------------------
 * 每 App 地址空间（U-1）：同一 VA，不同物理帧
 * @details 本 port 没有硬件页表遍历（QEMU 9.2.3 的 cpu->ptw = OFF），所以
 *          「每进程页表」在这里的落地形态是：**每个 App 一组条目，切换时
 *          用 tlbwr 把 App 窗口那一条换成该 App 的物理帧**。地址空间号就是
 *          帧号，帧从 ARKRTOS_QEMU_APP_BASE 起按 2 MB 步进：
 *
 *            space 0 -> PA 0x02000000（QEMU 把 .appprobe 装到这里，也是
 *                       ELF 装载器一直以来用的那一块，行为不变）
 *            space 1 -> PA 0x04000000
 *            ...
 *
 *          为什么帧不放在内核恒等映射（低 16 MB）里：App 的 2 MB 窗口
 *          本身就在 16 MB 以上，内核是通过**同一个 VA**（PLV0 可以访问
 *          PLV3 条目，LoongArch 的判据是 current PLV <= 条目的 PLV）读写
 *          App 内存的，因此帧只要落在 DRAM 里、由 App 窗口条目指过去就行。
 *          这也顺带把「一个 App 的物理页对另一个 App 不可达」变成结构性
 *          事实：别的 App 的帧没有任何 VA 映射到它。
 *
 *          切换只需要重写一条 MTLB 条目（PS=20，索引 LA_MTLB_INDEX+2），
 *          不需要 invtlb：同一条条目被 tlbwr 覆盖后旧翻译就不存在了。
 *          内核那两条 PS=22 恒等映射与两条 4 KB UART 条目一根手指都不碰。
 *
 *          帧数由板级 ARKRTOS_PAGING_APPS_MAX 决定，**默认 1**（只有 space 0，
 *          与改动前逐字节同义）。这个宏缺省时取 1，别的 LA 板不需要改。
 * ------------------------------------------------------------ */
/** 空间号 -> 物理帧基址（2 MB 对齐，满足 PS=20 条目要求） */
static uint32 la_pg_app_frame(uint32 space)
{
#if (ARKRTOS_USE_PAGING_PAGES == 1)
    /* 页级窗口：帧由帧池按空间整块给出，不再由「空间号 × 2 MB」推出来。 */
    return s_pg_space_base[space];
#else
    return ARKRTOS_QEMU_APP_BASE + (space * ARKRTOS_QEMU_APP_SIZE);
#endif
}

#if (ARKRTOS_USE_PRIV == 1)

/**
* @brief 让某个 VA 的翻译失效（含 QEMU 的地址翻译缓存）
* @details `invtlb` op=3：按 VA 清掉命中的 TLB 条目，并整表 flush 掉 QEMU
*          的 softmmu 缓存。（旧注释写「按 (ASID, VA)」是错的：pmap
*          asidinval 在 QEMU 上量出来的是「不看 ASID」，详见下面的 op 表。）
*          为什么必须显式做：QEMU 的 tlbwr 只清它覆盖到的地址范围，而它算
*          **奇页**地址的那句写错成 `addr & pagesize`（应为 `| pagesize`），
*          于是 PS=20 条目的上半 1 MB 永远留在 softmmu 缓存里 —— 重写条目
*          后经那半页读写仍是旧物理帧，症状正是「切换静默失效」。
*          顺序要紧：先 invtlb 再写新条目（invtlb 会按旧条目的 PS 匹配）。
*
*          U-7b 实测（QEMU 9.2.3，pmap invop 逐 op 取证，别再试一遍）：
 *   op 0 / 1 / 2 / 6 ：一次调用把**整张表**打掉（含内核恒等映射），
 *                      紧接着取指就 KERNEL refill fatal。op 6 不是
 *                      「按 (ASID, VA) 清一条」，别指望它。
 *   op 4 / 5          ：活下来，窗口条目一个都没掉 —— 在本载体上
 *                      等价于空操作。
 *   op 7              ：非法指令（estat ecode=0xd）。
 *   op 3              ：按 VA 失效，只清这个 VA，内核不受影响 ——
 *                      这条是本 port 唯一敢用的失效手段。
 * 于是「只清某个 ASID 的条目」在本载体上**做不到**：想只撤一个空间的
 * 窗口，只能先切到它的 ASID 再按 VA 撤（撤完把当前空间补铺回来），
 * 这就是 activate / space_release 现在走的那条路。 */
static void la_tlb_inval_page(uint32 va)
{
    __asm__ volatile("invtlb 3, $zero, %0"
                     :: "r"((unsigned long)va) : "memory");
}

#if (ARKRTOS_USE_PAGING_PAGES == 1)
/* ------------------------------------------------------------
 * 页级窗口（U-3 第二阶）：帧池的第一个真实使用方
 * @details 每个地址空间从帧池里整块拿 LA_PG_WIN_PAGES（512）帧 = 2 MB，
 *          然后把窗口铺成 LA_PG_WIN_ENTRIES（256）条 PS=12 条目，全部落在
 *          STLB 的 way 0。切换空间 = 先把整段窗口 VA 逐页失效（QEMU 的
 *          softmmu 缓存按 VA 记翻译，tlbwr 只清自己那 8 KB 且奇页地址
 *          算错，所以必须显式 invtlb），再重写这 256 条。
 *
 *          三条不猜的规矩：
 *            1. 帧不够就**不装**：ensure 失败时不留下半张窗口（半张窗口
 *               会让 App 一半代码读得到、一半 refill，那种失败最难查）；
 *            2. 装完必须抽查首尾两页的 PPN 与 ps，对不上报 -2 且不改
 *               s_pg_app_space（报错留在原地，比「切换了」这句谎话好）；
 *            3. 拿过的帧不还：空间回收是欠账，说清楚比偷偷还了更安全。
 * ------------------------------------------------------------ */
#if (ARKRTOS_USE_PAGING_FRAMES != 1)
#error "ARKRTOS_USE_PAGING_PAGES=1 requires ARKRTOS_USE_PAGING_FRAMES=1"
#endif
#if ((ARKRTOS_QEMU_APP_SIZE % 0x2000u) != 0u)
#error "page-granular window needs ARKRTOS_QEMU_APP_SIZE to be a multiple of 8 KB"
#endif

/** 给空间从帧池里拿一整块 2 MB；已经拿过就什么都不做 */
static int32 la_pg_space_ensure(uint32 space)
{
    uint32 base = 0u;

    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需档不在这里整块取帧：每一对 8 KB 在第一次被访问时才要两帧
     * （见 la_pg_demand_fault），所以「空间帧基址」这个概念在这档不存在。 */
    (void)base;
    return 0;
#endif
    if(s_pg_space_base[space] != 0u)
    {
        return 0;
    }
    if(arkrtos_port_paging_frame_alloc(LA_PG_WIN_PAGES, &base) != 0)
    {
        return -3;
    }

    s_pg_space_base[space] = base;
    return 0;
}

/** 把该空间的 512 页铺进窗口（256 条 PS=12 条目，占该空间自己那一路） */
static void la_pg_install_space(uint32 space)
{
    uint32 i;
#if (ARKRTOS_USE_PAGING_ASID == 1)
    /* 条目记的是**写的那一刻**的 CSR.ASID（TLBWR/TLBFILL 把 CSR.ASID 抄进
     * 表项），不是「空间的号码」。所以铺之前先切过去，铺完换回调用者的值：
     * 切不切到该空间由调用者决定（activate 会切，回收路径只是借用一下）。 */
    unsigned long saved_asid = la_asid_swap(LA_PG_ASID_OF(space));
#endif
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需档：只重铺**已经付过费**的那几对（invtlb 之后要补回来，见
     * la_pg_assert_window）；没摸过的对留空，等它自己 fault。 */
    for(i = 0u; i < LA_PG_WIN_ENTRIES; i++)
    {
        uint32 pa = s_pg_pair_pa[space][i];

        if(pa != 0u)
        {
            uint32 va = ARKRTOS_QEMU_APP_BASE + (i * 0x2000u);

            la_tlb_write(va, 12u, LA_STLB_INDEX(va, LA_PG_WIN_WAY_OF(space)),
                         LA_PTE_APP(pa), LA_PTE_APP(pa + 0x1000u));
        }
    }
#else
    uint32 base = s_pg_space_base[space];

    for(i = 0u; i < LA_PG_WIN_ENTRIES; i++)
    {
        uint32 va = ARKRTOS_QEMU_APP_BASE + (i * 0x2000u);

        la_tlb_write(va, 12u, LA_STLB_INDEX(va, LA_PG_WIN_WAY_OF(space)),
                     LA_PTE_APP(base + (i * 0x2000u)),
                     LA_PTE_APP(base + (i * 0x2000u) + 0x1000u));
    }
#endif
#if (ARKRTOS_USE_PAGING_ASID == 1)
    (void)la_asid_swap(saved_asid);
#endif
}

/** 把整段窗口 VA 的旧翻译清掉（逐 4 KB：QEMU 按 (ASID, VA) 匹配条目） */
static void la_pg_drop_window(void)
{
    uint32 i;

    for(i = 0u; i < LA_PG_WIN_PAGES; i++)
    {
        la_tlb_inval_page(ARKRTOS_QEMU_APP_BASE + (i * 0x1000u));
    }
}

#if (ARKRTOS_USE_PAGING_SWAP == 1)
/* ------------------------------------------------------------
 * 页替换（U-16）：帧池满了就换出一对
 * @details 按需档原来的形态是「摸到就付两帧、永不归还」，于是帧池一满就
 *          只能拒绝。这一档把「拒绝」换成「换出别的某一对」：
 *
 *            缺页 -> 要两帧 -> 池空 -> 挑一对换出（内容抄进交换槽、两帧还池）
 *                 -> 再要一次 -> 装上 -> ertn 回原指令
 *
 *          账本用「槽号 + 1」而不是「0xFF = 没有」：静态对象初值只能是 0，
 *          用 0 表示「没有」就不需要任何运行期初始化，也就不存在「忘了初始
 *          化 ⇒ 一堆对指向 0 号槽」这种只在压力下显形的形态。
 *
 *          三条不猜的规矩：
 *            1. 帧一帧都没拿到就返回 0（交回原来的故障路径），绝不装一条
 *               半成品条目 —— 那会让同一条指令无限重填；
 *            2. 换出**先失效 TLB 条目再还帧**。反序会出现「帧已归池、App 仍
 *               能经旧条目读到它」，两个持有者共享一帧而且一声不响；
 *            3. 换入先把内容抄到中继缓冲、放掉自己的槽，再去要帧。否则
 *               死锁：要帧要先换出别人，换出要一个空槽，而唯一可能空着的
 *               槽正是这一对自己占的。
 * ------------------------------------------------------------ */
#define LA_PG_PAIR_WORDS        2048u   /* 8 KB / 4 */

static uint8  s_pg_pair_slot[ARKRTOS_PAGING_APPS_MAX][LA_PG_WIN_ENTRIES];
static uint32 s_pg_swap_area[ARKRTOS_PAGING_SWAP_SLOTS][LA_PG_PAIR_WORDS];
static uint32 s_pg_bounce[LA_PG_PAIR_WORDS];
static uint32 s_pg_swap_bits;       /* 槽占用位图：bit s = 槽 s 有人占 */
static uint32 s_pg_swap_hand;       /* 换出轮转指针（确定性，不取随机数） */
static uint32 s_pg_swap_out;        /* 换出次数 */
static uint32 s_pg_swap_in;         /* 换回次数 */
static uint32 s_pg_swap_fail;       /* 「池空且换不出东西」的次数 */

/** 8 KB 拷贝：重填上下文里没有 libc，也不能取锁/阻塞，只能是裸循环 */
static void la_pg_copy_pair(uint32 *dst, const uint32 *src)
{
    uint32 i;

    for(i = 0u; i < LA_PG_PAIR_WORDS; i++)
    {
        dst[i] = src[i];
    }
}

/** 找一个空交换槽并占住；没有返回 -1 */
static int32 la_pg_swap_slot_alloc(uint32 *p_slot)
{
    uint32 s;

    for(s = 0u; s < (uint32)ARKRTOS_PAGING_SWAP_SLOTS; s++)
    {
        if((s_pg_swap_bits & (1u << s)) == 0u)
        {
            s_pg_swap_bits |= (1u << s);
            *p_slot = s;
            return 0;
        }
    }

    return -1;
}

/** 放掉一个交换槽 */
static void la_pg_swap_slot_free(uint32 slot)
{
    if(slot < (uint32)ARKRTOS_PAGING_SWAP_SLOTS)
    {
        s_pg_swap_bits &= ~(1u << slot);
    }
}

/** 占用的槽数（读数用） */
static uint32 la_pg_swap_slots_used(void)
{
    uint32 s;
    uint32 n = 0u;

    for(s = 0u; s < (uint32)ARKRTOS_PAGING_SWAP_SLOTS; s++)
    {
        if((s_pg_swap_bits & (1u << s)) != 0u)
        {
            n++;
        }
    }

    return n;
}

/** 驻留（有帧）的对数（读数用） */
static uint32 la_pg_pair_resident_count(void)
{
    uint32 space;
    uint32 i;
    uint32 n = 0u;

    for(space = 0u; space < (uint32)ARKRTOS_PAGING_APPS_MAX; space++)
    {
        for(i = 0u; i < LA_PG_WIN_ENTRIES; i++)
        {
            if(s_pg_pair_pa[space][i] != 0u)
            {
                n++;
            }
        }
    }

    return n;
}

/**
* @brief 换出一对：内容抄进交换槽、两帧还回池子
* @param skip_space 正在缺页的空间（第一轮避开它）
* @param skip_idx   该空间里绝不换出的对号（正在换入的那一对）
* @return 0 = 换出了；-1 = 没有驻留的对可换；-2 = 交换槽满；-3 = 帧池拒绝
* @details 两轮挑选：第一轮只挑**别的空间**的对 —— 它们此刻一条条目都没装着，
*          换掉不产生额外缺页；本空间的对可能正被这条取指路径用着，第二轮
*          才轮到它们（仍然避开 skip_idx）。轮转指针跨调用保持，免得每次都
*          盯着同一对换（那会退化成「只有一对在挪」）。
*/
static int32 la_pg_swap_out_one(uint32 skip_space, uint32 skip_idx)
{
    uint32 total = (uint32)ARKRTOS_PAGING_APPS_MAX * LA_PG_WIN_ENTRIES;
    uint32 slot = 0u;
    uint32 pass;
    uint32 n;

    if(la_pg_swap_slot_alloc(&slot) != 0)
    {
        return -2;
    }

    for(pass = 0u; pass < 2u; pass++)
    {
        for(n = 0u; n < total; n++)
        {
            uint32 k = (s_pg_swap_hand + n) % total;
            uint32 space = k / LA_PG_WIN_ENTRIES;
            uint32 idx = k % LA_PG_WIN_ENTRIES;
            uint32 pa;

            if((pass == 0u) && (space == skip_space))
            {
                continue;
            }
            if((pass == 1u) && (space != skip_space))
            {
                continue;
            }
            if((space == skip_space) && (idx == skip_idx))
            {
                continue;
            }

            pa = s_pg_pair_pa[space][idx];
            if(pa == 0u)
            {
                continue;
            }

            la_pg_copy_pair(s_pg_swap_area[slot], (const uint32 *)pa);

            if(space == s_pg_app_space)
            {
                uint32 va = ARKRTOS_QEMU_APP_BASE + (idx * 0x2000u);

                la_tlb_inval_page(va);
                la_tlb_inval_page(va + 0x1000u);
            }

            if(arkrtos_port_paging_frame_free(pa, 2u) != 0)
            {
                la_pg_swap_slot_free(slot);
                return -3;
            }

            s_pg_pair_pa[space][idx] = 0u;
            s_pg_pair_slot[space][idx] = (uint8)(slot + 1u);
            s_pg_swap_hand = (k + 1u) % total;
            s_pg_swap_out++;
            return 0;
        }
    }

    la_pg_swap_slot_free(slot);
    return -1;
}

/**
* @brief 给一对 8 KB 要到两帧：池里有就直接拿，没有就先换出一对
* @param space 正在缺页的空间
* @param idx   正在缺页的对号（绝不换出它自己）
* @param p_pa  成功时写回首帧物理基址
* @return 0 = 拿到；-1 = 池空且换不出东西
* @details 换出一对还回的是两帧连续帧，所以正常一次就够；留 8 次循环是给
*          「换出成功但那两帧拼不出一次两帧分配」的碎片情形当余量。
*/
static int32 la_pg_take_pair(uint32 space, uint32 idx, uint32 *p_pa)
{
    uint32 pa = 0u;
    uint32 tries;

    if(arkrtos_port_paging_frame_alloc(2u, &pa) == 0)
    {
        *p_pa = pa;
        return 0;
    }

    for(tries = 0u; tries < 8u; tries++)
    {
        if(la_pg_swap_out_one(space, idx) != 0)
        {
            return -1;
        }

        if(arkrtos_port_paging_frame_alloc(2u, &pa) == 0)
        {
            *p_pa = pa;
            return 0;
        }
    }

    return -1;
}

/**
* @brief 页替换读数（U-16）
* @param which 0 = 换出次数、1 = 换回次数、2 = 占用槽数、3 = 失败次数、
*              4 = 驻留对数、5 = 槽总数
* @return 对应的读数；which 越界返回 0
*/
uint32 arkrtos_port_paging_swap_stat(uint32 which)
{
    switch(which)
    {
    case 0u:
        return s_pg_swap_out;
    case 1u:
        return s_pg_swap_in;
    case 2u:
        return la_pg_swap_slots_used();
    case 3u:
        return s_pg_swap_fail;
    case 4u:
        return la_pg_pair_resident_count();
    case 5u:
        return (uint32)ARKRTOS_PAGING_SWAP_SLOTS;
    default:
        return 0u;
    }
}
#endif /* ARKRTOS_USE_PAGING_SWAP == 1 */

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/**
* @brief 按需缺页：给 badv 所在的那一对 8 KB 拿帧并装条目
* @param badv 触发 refill 的地址（CSR.TLBRBADV）
* @return 1 = 条目已装好，调用方应 ertn 回到出错的那条指令重试；
*         0 = 不归这一档管（不在窗口里 / 帧池空了），交给原来的故障路径
* @details 粒度是**一对**而不是一页：一条 PS=12 条目同时给出两页，所以
*          「装半条」根本表达不出来 —— 要么两帧一起拿，要么一条都不装。
*          这一档刻意不区分 PLV：内核经窗口拷贝（ELF 装载、pmap 自测）
*          与 App 自己访问走的是同一条路，否则装载器第一笔写就死在
*          「KERNEL refill -> fatal」上。
*
*          没有帧就返回 0，让原来的路径去报错/杀任务 —— 这里绝不假装
*          装好了（装一条 V=0 的条目会让 ertn 回到同一条指令上无限重填）。
*/
static int32 la_pg_demand_fault(uint32 badv)
{
    uint32 space = s_pg_app_space;
    uint32 idx;
    uint32 pa;

    if((badv < ARKRTOS_QEMU_APP_BASE) ||
       (badv >= (ARKRTOS_QEMU_APP_BASE + ARKRTOS_QEMU_APP_SIZE)))
    {
        arkrtos_board_puts("[DBG] out-of-window badv=");
        arkrtos_board_puthex(badv);
        arkrtos_board_puts(" faults=");
        arkrtos_board_putu(s_pg_demand_faults);
        arkrtos_board_puts("\n");
        return 0;
    }

    idx = (badv - ARKRTOS_QEMU_APP_BASE) / 0x2000u;
    if(idx >= LA_PG_WIN_ENTRIES)
    {
        return 0;
    }

    pa = s_pg_pair_pa[space][idx];
    if(pa == 0u)
    {
#if (ARKRTOS_USE_PAGING_SWAP == 1)
        uint32 slot = (uint32)s_pg_pair_slot[space][idx];

        if(slot != 0u)
        {
            /* 换回：先把内容抄到中继缓冲并放掉自己的槽（理由见上面第 3 条
             * 规矩：不放掉会死锁），再去要帧。 */
            la_pg_copy_pair(s_pg_bounce, s_pg_swap_area[slot - 1u]);
            s_pg_pair_slot[space][idx] = 0u;
            la_pg_swap_slot_free(slot - 1u);
        }

        if(la_pg_take_pair(space, idx, &pa) != 0)
        {
            s_pg_swap_fail++;

            if(slot != 0u)
            {
                /* 一帧都没拿到：内容还在中继缓冲里，放回一个槽。把内容吃掉
                 * 比失败更糟 —— 那等于「换回失败顺便把数据丢了」。 */
                uint32 back = 0u;

                if(la_pg_swap_slot_alloc(&back) == 0)
                {
                    la_pg_copy_pair(s_pg_swap_area[back], s_pg_bounce);
                    s_pg_pair_slot[space][idx] = (uint8)(back + 1u);
                }
            }

            return 0;
        }

        s_pg_pair_pa[space][idx] = pa;

        if(slot != 0u)
        {
            la_pg_copy_pair((uint32 *)pa, s_pg_bounce);
            s_pg_swap_in++;
        }
        else
        {
            /* 第一次摸到这一对，而帧可能刚从别人手里换出来（里面是旧内容）。
             * 必须显式清零：换出/换入让「帧池刚起来是干净的」这个前提不再
             * 成立。关这一档时不做这件事 —— 那时帧确实来自刚清零的 RAM。 */
            uint32 w;

            for(w = 0u; w < LA_PG_PAIR_WORDS; w++)
            {
                ((uint32 *)pa)[w] = 0u;
            }
        }
#else
        if(arkrtos_port_paging_frame_alloc(2u, &pa) != 0)
        {
            return 0;
        }

        s_pg_pair_pa[space][idx] = pa;
#endif
    }

    {
        uint32 va = ARKRTOS_QEMU_APP_BASE + (idx * 0x2000u);

        /* 这里处在重填上下文里：必须走 TLBREHI/TLBRELO + tlbfill
         * （tlbwr/tlbfill 在 ISTLBR=1 时只认 TLBR* 那一组镜像 CSR）。 */
        la_tlb_fill_refill(va, LA_PTE_APP(pa), LA_PTE_APP(pa + 0x1000u));
    }

    s_pg_demand_faults++;
    return 1;
}

#endif /* ARKRTOS_USE_PAGING_DEMAND == 1 */

/** 开分页之前的首次铺窗口：只铺 space 0（其它空间等 activate 时再要帧） */
static void la_pg_boot_map(void)
{
    if(la_pg_space_ensure(0u) != 0)
    {
        /* 帧池空 ⇒ 窗口没有任何映射。不假装铺好了：第一次 App 访问会走
         * refill 并被当成保护错误，比「映射到 PA 0」这种错答案安全得多。 */
        return;
    }

    la_pg_install_space(0u);

#if (ARKRTOS_USE_PAGING_ASID == 1)
    /* 内核的默认视图就是 space 0：内核经窗口读写 space 0 的帧时 CSR.ASID
     * 必须是这个空间的 ASID，否则那条非全局的窗口条目匹配不上（内核自己的
     * 恒等映射、UART 页都是全局条目，不受影响）。 */
    (void)la_asid_swap(LA_PG_ASID_OF(0u));
#endif
}
#endif /* ARKRTOS_USE_PAGING_PAGES == 1 */

/**
* @brief 把一个地址空间装进 App 窗口
* @param space 空间号（0 .. 板级 ARKRTOS_PAGING_APPS_MAX-1）
* @return 0 = 已是该空间或切换成功；-1 = 空间号越界
* @details 只换 App 窗口那一条 PS=20 条目。调用方（调度器切到某个 App）
*          保证此时不会有别的任务正在这个窗口里取指 —— 调用点在切换路径上，
*          当前任务已经不在 App 页里跑。
*/
/**
* @brief 把 App 窗口那一条 PS=20 条目按当前空间重写一遍
* @details 映射内容不变，只是重新落一条有效条目。存在的理由是实测出来的：
*          QEMU 的 `invtlb 3`（按 VA 失效）**会连带打掉这条 PS=20 条目**
*          —— pmap test 末尾撤销动态映射之后再 pmap look 0x02000000 报
*          no entry，紧接着任何一次窗口访问都是 KERNEL refill (plv0)。
*          真机上按 VA 失效不该波及其他页，但这条修复在真机上也只是一次
*          tlbwr，代价可忽略，换来的是「invtlb 之后窗口一定还在」。
*/
static void la_pg_assert_window(void)
{
#if (ARKRTOS_USE_PAGING_ASID == 1)
    /* ASID 档：条目按 ASID 分开，补铺自己这一路就够，不必先失效（tlbwr 覆盖
     * 同一条目）。别的空间若被一次 invtlb 3 连带清掉，下一次 activate 的读回
     * 校验会补回来 —— 这一条只保证「当前空间的窗口一定在」。 */
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    la_pg_install_space(s_pg_app_space);
#else
    if(s_pg_space_base[s_pg_app_space] != 0u)
    {
        la_pg_install_space(s_pg_app_space);
    }
#endif
#elif (ARKRTOS_USE_PAGING_PAGES == 1)
    /* 页级窗口：一次 invtlb 只会打掉命中的那几页，所以这里整段重铺。
     * 代价（512 次 invtlb + 256 次 tlbwr）只在这一条路径上付，换来的是
     * 「撤销动态映射之后窗口一定还在」。 */
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需档没有「空间帧基址」可判（恒 0），判据换成「有没有装过任何一对」；
     * 重铺只补已经付过费的那几对，没摸过的继续留空。 */
    la_pg_drop_window();
    la_pg_install_space(s_pg_app_space);
#else
    if(s_pg_space_base[s_pg_app_space] == 0u)
    {
        return;                     /* 还没铺过：没有可补的 */
    }

    la_pg_drop_window();
    la_pg_install_space(s_pg_app_space);
#endif
#else
    uint32 pa = la_pg_app_frame(s_pg_app_space);

    la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u,
                 LA_PTE_APP(pa), LA_PTE_APP(pa + 0x100000u));
#endif
}

#if (ARKRTOS_USE_PAGING_ASID == 1)
/** 这一对 8 KB 在这个空间里「应该」翻译到哪；0 = 这一档说不出该是什么 */
static uint32 la_pg_pair_expect(uint32 space, uint32 idx)
{
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    return s_pg_pair_pa[space][idx];
#else
    return s_pg_space_base[space] + (idx * 0x2000u);
#endif
}

/** 读回校验一对 8 KB：期望值为 0（这一档还没付费）时视为通过 */
static int32 la_pg_pair_check(uint32 space, uint32 idx)
{
    uint32 va = ARKRTOS_QEMU_APP_BASE + (idx * 0x2000u);
    uint32 expect = la_pg_pair_expect(space, idx);
    uint32 ppn = 0u;
    uint32 ps = 0u;

    if(expect == 0u)
    {
        return 0;
    }

    if((arkrtos_port_paging_lookup(va, 0, &ppn, &ps) == 0u) ||
       (ppn != expect) || (ps != 12u))
    {
        return -1;
    }

    return 0;
}
#endif

int32 arkrtos_port_paging_activate(uint32 space)
{
#if (ARKRTOS_USE_PAGING_ASID == 1)
    /* ASID 档：切换 = 一次 CSR 写。别的空间那 256 条一条都不用动 —— 它们的
     * ASID 不匹配，只是未命中，翻译不到别人的帧。
     *
     * 但「不动」不等于「一定还在」：invtlb 3 按 VA 失效、不看 ASID，会连带
     * 清掉别的空间同 VA 的条目（撤销动态映射、空间回收都走它）。所以每次
     * 切换都读回首末两对确认，缺了就补铺这一路。补完还对不上就返回 -2，
     * 且 s_pg_app_space 照样更新 —— CSR.ASID 已经切过去了，说「没切」是
     * 假话；这一档失败的形态是「条目缺了会 refill」，不是「读到别人的帧」。 */
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

#if (ARKRTOS_USE_PAGING_DEMAND != 1)
    if(la_pg_space_ensure(space) != 0)
    {
        return -3;
    }
#endif

    (void)la_asid_swap(LA_PG_ASID_OF(space));

    {
        int32 bad = 0;

        if((la_pg_pair_check(space, 0u) != 0) ||
           (la_pg_pair_check(space, LA_PG_WIN_ENTRIES - 1u) != 0))
        {
            la_pg_install_space(space);
            bad = 1;
        }

        s_pg_app_space = space;

        if((bad != 0) &&
           ((la_pg_pair_check(space, 0u) != 0) ||
            (la_pg_pair_check(space, LA_PG_WIN_ENTRIES - 1u) != 0)))
        {
            return -2;
        }
    }

    return 0;
#else
#if (ARKRTOS_USE_PAGING_PAGES == 1)
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需档：切空间 = 清掉旧翻译 + 记账，条目一条都不铺（等第一次访问
     * 走 refill）。也不做「已是该空间」短路 —— 这一档的真相是
     * s_pg_pair_pa，短路省不下什么却多一条会骗人的路。 */
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

    la_pg_drop_window();
    s_pg_app_space = space;
    return 0;
#else
    /* 页级窗口：先要帧（不够就 -3，窗口一条都不动），再失效旧翻译、重铺
     * 256 条，最后抽查首尾两页。 */
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

    if(la_pg_space_ensure(space) != 0)
    {
        return -3;
    }

    if(space == s_pg_app_space)
    {
        uint32 probed = 0u;
        uint32 probed_ps = 0u;

        if((arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0,
                                       &probed, &probed_ps) != 0u) &&
           (probed == s_pg_space_base[space]) && (probed_ps == 12u))
        {
            return 0;
        }
    }

    la_pg_drop_window();
    la_pg_install_space(space);

    {
        uint32 last_va = ARKRTOS_QEMU_APP_BASE + ARKRTOS_QEMU_APP_SIZE - 0x1000u;
        uint32 probed = 0u;
        uint32 probed_ps = 0u;

        if((arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0, &probed, &probed_ps) == 0u) ||
           (probed != s_pg_space_base[space]) || (probed_ps != 12u))
        {
            return -2;
        }
        /* 末页是某条条目的 TLBELO1（奇页），而 lookup 读回的永远是 TLBELO0
         * （偶页）的 PPN —— 期望值是这一段 8 KB 的偶页基址，不是末页本身的
         * 基址。写成末页基址会得到一次假失败（实测踩到过）。 */
        if((arkrtos_port_paging_lookup(last_va, 0, &probed, &probed_ps) == 0u) ||
           (probed != (s_pg_space_base[space] + ARKRTOS_QEMU_APP_SIZE - 0x2000u)) ||
           (probed_ps != 12u))
        {
            return -2;
        }
    }

    s_pg_app_space = space;
    return 0;
#endif /* ARKRTOS_USE_PAGING_DEMAND */
#else
    uint32 pa;

    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

    if(space == s_pg_app_space)
    {
        /* 「同一个空间」不等于「条目还在」：别的路径的一次 invtlb 就可能
         * 把它打掉（见 la_pg_assert_window）。读回来确认，对不上就重写。 */
        uint32 probed = 0u;

        if((arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0, &probed, 0) != 0u) &&
           (probed == la_pg_app_frame(space)))
        {
            return 0;
        }
    }

    pa = la_pg_app_frame(space);

    /* VA 与 PA 都必须 2 MB 对齐（PS=20 的条目要求），帧号与 VA 基址都是
     * 2 MB 的整数倍，这条由构造保证。一条 MTLB 条目覆盖 2 MB（LO0/LO1
     * 各 1 MB）。 */
    la_tlb_inval_page(ARKRTOS_QEMU_APP_BASE);
    la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u,
                 LA_PTE_APP(pa), LA_PTE_APP(pa + 0x100000u));

    /* 写进去不等于生效：tlbwr 认不认这条索引、查找会不会命中另一条陈旧
     * 条目，只有读回来才知道。读回来对不上就报 -2 并**不改** s_pg_app_space
     * —— 报错留在原地，比「切换了」这句谎话好。 */
    {
        uint32 probed_ppn = 0u;

        if((arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0, &probed_ppn, 0) == 0u) ||
           (probed_ppn != pa))
        {
            return -2;
        }
    }

    s_pg_app_space = space;
    return 0;
#endif /* ARKRTOS_USE_PAGING_PAGES == 1 */
#endif /* ARKRTOS_USE_PAGING_ASID == 1 */
}

/* ------------------------------------------------------------ 延迟换窗口 (U-2) */

/* 待装的空间号。0xFFFFFFFF = 没有待装项：0 是合法空间号，不能拿它当空值。 */
#define LA_PG_PENDING_NONE      0xFFFFFFFFu

static volatile uint32 s_pg_pending_space = LA_PG_PENDING_NONE;

/** 调用者此刻的 sp 是否落在 App 窗口里 */
static uint32 la_sp_in_app_window(void)
{
    unsigned long sp;

    __asm__ volatile("move %0, $sp" : "=r"(sp) : : "memory");

    return (((uint32)sp >= (uint32)ARKRTOS_QEMU_APP_BASE) &&
            ((uint32)sp <  ((uint32)ARKRTOS_QEMU_APP_BASE +
                            (uint32)ARKRTOS_QEMU_APP_SIZE))) ? 1u : 0u;
}

/**
* @brief 调度器换窗口：调用者自己站在窗口里时必须推迟（U-2）
* @details 详见 arkrtos_hal.h 的声明。判据就是「我此刻的 sp 在不在窗口里」，
*          它精确对应危险条件：恢复现场要用的那一帧（以及本函数的返回地址）
*          正压在同一个窗口 VA 上。推迟不等于不换 —— 换的时机交给
*          arkrtos_context.S：那里已经不再碰被换下任务的栈了。
*/
int32 arkrtos_port_paging_switch_to(uint32 space)
{
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

    if(la_sp_in_app_window() != 0u)
    {
        s_pg_pending_space = space;
        return 0;
    }

    return arkrtos_port_paging_activate(space);
}

/**
* @brief 恢复现场之前补做被推迟的窗口切换（由 arkrtos_context.S 调用）
* @param new_frame 即将恢复的帧
* @param old_frame 被换下任务的帧（同一个 = 调度器最后没切走）
* @details 本函数由汇编入口在**一块内核静态栈**上调用：进入时 sp 可能还指向
*          App 窗口里的栈，而它自己一落地就要开始写栈 —— 所以汇编侧先把 sp
*          换到 la_switch_stack，再进来。返回之后汇编侧不再碰任何栈，只按
*          $s0 指向的目标帧恢复寄存器。
*
*          PRMD 也要在这里补：C 侧的 LA_PRIV_APPLY 在「窗口还没换」的时候
*          执行，它写进去的是旧窗口里的那块内存，不是目标帧。
*/
void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_frame)
{
    uint32 space = s_pg_pending_space;

    if(space == LA_PG_PENDING_NONE)
    {
        return;
    }

    s_pg_pending_space = LA_PG_PENDING_NONE;

    /* 调度器把帧原样交回来了：窗口必须留在原处，否则这一帧的恢复会从
     * 另一个 App 的帧里取值。 */
    if(new_frame == old_frame)
    {
        return;
    }

    (void)arkrtos_port_paging_activate(space);

    new_frame[LA_FR_PRMD] = (new_frame[LA_FR_PRMD] & ~3UL) | (unsigned long)s_next_pplv;
}

/* ------------------------------------------------------------ 空间回收 (U-3) */

/**
* @brief 把一个地址空间的物理帧还给帧池
* @param space 空间号
* @return 0 = 已归还（本来就没拿过也算 0，幂等）；-1 = 空间号越界；
*         -2 = 这一档没有帧池可还（PS=20 静态窗口）；-3 = 调用者正站在
*         这个窗口里（调用者自己的栈就在这块窗口里），此时一个字节都不动；-4 = 帧池拒绝
*         （它的 -1/-3 原样透出，账本保持原样）
* @details 归还之前先把窗口里属于这个空间的东西撤掉，理由只有一条但足够：
*          帧还回池子之后会被别的空间拿走，而旧条目仍然指着它 —— 两个空间
*          共享同一块物理页，没有任何报错，只在很久以后显形。
*          撤法按档走：页级窗口整段逐页 invtlb（QEMU 的 softmmu 缓存按 VA
*          记翻译）；按需档没有「整块帧」，逐对 free 并清账本。
*          s_pg_app_space 保持原值：这个空间仍然存在，只是空着；下一次
*          activate() 会重新要帧（ensure 看到基址 0 就再要一块）。这与
*          「sid 不存在」是两件事，压成一个状态会让 activate 的短路判据骗人。
*/
int32 arkrtos_port_paging_space_release(uint32 space)
{
#if (ARKRTOS_USE_PAGING_PAGES != 1)
    /* 静态 PS=20 窗口：帧由板级地址决定，没有从池里拿过东西，也就没有
     * 可归还的。返回 -2 而不是 0 —— 「本来就没有」和「已经还了」分开报。 */
    (void)space;
    return -2;
#else
    uint32 i;
    int32  rc = 0;

    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return -1;
    }

    /* 自己就站在这个窗口里时不许动：撤掉条目之后，返回地址与栈都在一个
     * 已经不存在的翻译里，接下来的一次 ret 就是 refill 致命。判据不依赖
     * 调用方是谁（shell / 装载器都在核心里跑）。 */
    if((space == s_pg_app_space) && (la_sp_in_app_window() != 0u))
    {
        return -3;
    }

#if (ARKRTOS_USE_PAGING_ASID == 1)
    {
        /* ASID 档：要撤的是**那个空间**的条目，而 invtlb 3 按 VA 失效、不看
         * ASID ⇒ 先切到它的 ASID 再撤（切不切其实都撤得到，切换是为了让
         * 「撤的是谁」这件事在代码里写得出来），撤完切回原处并把当前空间的
         * 窗口补铺回来 —— 那 512 次失效连它一起打掉了。
         * 不撤的后果只有一个但足够：帧还回池子后被别的空间拿走，旧条目仍
         * 指着它，两个空间共享同一块物理页而且没有任何报错。 */
        unsigned long prev = la_asid_swap(LA_PG_ASID_OF(space));

        la_pg_drop_window();
        (void)la_asid_swap(prev);

        if(space != s_pg_app_space)
        {
            la_pg_install_space(s_pg_app_space);
        }
    }
#else
    if(space == s_pg_app_space)
    {
        /* 旧条目先撤。不撤 = 帧回池后被别人拿走、窗口仍指着它。 */
        la_pg_drop_window();
    }
#endif

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需档：truth 在 s_pg_pair_pa，逐对还（一条 PS=12 条目管两页）。 */
    for(i = 0u; i < LA_PG_WIN_ENTRIES; i++)
    {
        uint32 pa = s_pg_pair_pa[space][i];

        if(pa != 0u)
        {
            if(arkrtos_port_paging_frame_free(pa, 2u) != 0)
            {
                if(rc == 0)
                {
                    rc = -4;    /* 第一处拒绝如实上报，不继续假装 */
                }
            }
            else
            {
                s_pg_pair_pa[space][i] = 0u;
            }
        }
#if (ARKRTOS_USE_PAGING_SWAP == 1)
        else if(s_pg_pair_slot[space][i] != 0u)
        {
            /* 换出去的对没有帧可还，但槽是它占着的：一起放掉，否则「空间
             * 回收」之后槽永远泄漏，跑几轮就再也换不出东西。 */
            la_pg_swap_slot_free((uint32)s_pg_pair_slot[space][i] - 1u);
            s_pg_pair_slot[space][i] = 0u;
        }
#endif
    }

    (void)i;
    return rc;
#else
    /* 页级窗口：整块 512 帧一起还；池子那边是 all-or-nothing，拒绝时账本
     * 一个字不改（下次 activate 仍指向同一块帧，比「以为还了其实没还」安全）。 */
    if(s_pg_space_base[space] != 0u)
    {
        if(arkrtos_port_paging_frame_free(s_pg_space_base[space],
                                          LA_PG_WIN_PAGES) != 0)
        {
            return -4;
        }

        s_pg_space_base[space] = 0u;
    }

    (void)i;
    return 0;
#endif
#endif /* ARKRTOS_USE_PAGING_PAGES */
}

#else  /* ARKRTOS_USE_PRIV == 0：没有 TLB 通路 */

int32 arkrtos_port_paging_activate(uint32 space)
{
    (void)space;
    return -1;
}

/** 没有窗口可换：恢复现场前没有要补做的事 */
void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_frame)
{
    (void)new_frame;
    (void)old_frame;
}

int32 arkrtos_port_paging_space_release(uint32 space)
{
    (void)space;

    return -1;                           /* 没有 TLB 通路就没有窗口可撤 */
}

#endif /* ARKRTOS_USE_PRIV == 1 */

/** 当前装进 App 窗口的空间号 */
uint32 arkrtos_port_paging_space(void)
{
    return s_pg_app_space;
}

/** 空间号对应的物理帧基址；越界返回 0（不猜一个地址出去） */
uint32 arkrtos_port_paging_space_frame(uint32 space)
{
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return 0u;
    }

    return la_pg_app_frame(space);
}

/** 板级给了几个地址空间（>= 1） */
uint32 arkrtos_port_paging_space_count(void)
{
    return (uint32)ARKRTOS_PAGING_APPS_MAX;
}

/** App 窗口的 VA 基址：各空间共用同一个 VA，物理侧才按空间换 */
uint32 arkrtos_port_paging_window_va(void)
{
    return ARKRTOS_QEMU_APP_BASE;
}

#if (ARKRTOS_USE_PAGING_ASID == 1)
/**
* @brief 读当前硬件的 CSR.ASID（U-7）
* @return 0..1023
* @details 只读，不改状态。地址空间切换之后用它回报「现在跑在谁的视图里」，
*          这是 ASID 档下判断「切换到底做了没有」的唯一直接读数。
*/
unsigned long arkrtos_port_paging_asid_current(void)
{
    return la_asid_get();
}

/**
* @brief 空间号对应的 ASID（空间 s -> ASID s+1，0 留给内核默认视图）
* @param space 0 .. arkrtos_port_paging_space_count()-1
* @return ASID 值；空间号越界时返回 0（不猜一个别空间的号）
*/
uint32 arkrtos_port_paging_asid_of(uint32 space)
{
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return 0u;
    }

    return (uint32)LA_PG_ASID_OF(space);
}

/**
* @brief 空间的窗口条目落在哪一条 STLB way 上
* @param space 0 .. arkrtos_port_paging_space_count()-1
* @return way 号；越界返回 0
* @details ASID 档下每个空间占一条独立 way，所以「两个空间的窗口条目并存」
*          这件事在硬件上是看得见的（不同的 way）；关档时全部挤在 way 0，
*          切换必须重写条目。
*/
uint32 arkrtos_port_paging_window_way(uint32 space)
{
    if(space >= (uint32)ARKRTOS_PAGING_APPS_MAX)
    {
        return 0u;
    }

    return LA_PG_WIN_WAY_OF(space);
}

/**
* @brief 只写一次 CSR.ASID 并返回换之前的值（诊断用，U-7b）
* @param asid 目标 ASID（只取低 10 位）
* @return 换之前的 CSR.ASID
* @details 与 activate() 的差别只有一条但很要紧：activate 会读回校验、缺
*          什么补什么，所以拿它测「条目还在不在」测不出来 —— 它会当场把
*          缺的补上。这个函数只做一次 csrwr：不记账、不补铺、不改
*          s_pg_app_space，调用方负责换回去。生产路径不该用它。
*/
unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid)
{
    return la_asid_swap(asid);
}

/**
* @brief 按 VA 让翻译失效：`invtlb` op=3，不看 ASID、global 条目不动
* @param va 要失效的虚拟地址
* @details 这就是 U-7 里那个「会连带打掉别的空间同 VA 条目」的操作。留着
*          它不是给生产路径用的，而是给 pmap asidinval 做对照 —— 「op=6 是
*          定向的」这句话只有跟 op=3 的连带行为摆在一起才算证据。
*/
void arkrtos_port_paging_inval_va(uint32 va)
{
    la_tlb_inval_page(va);
}

#endif

/**
* @brief 读回一个 VA 当前由哪条 TLB 条目翻译（诊断用）
* @param va     要查的虚拟地址
* @param index  命中条目的绝对号（STLB 0..2047 / MTLB 2048..）
* @param ppn    命中条目的偶页物理基址（TLBELO0 的 PPN）
* @param ps     命中条目的页大小指数（12 = 4 KB，22 = 4 MB）
* @return 1 = 命中；0 = 未命中（此时三个输出参数不被写）
* @details `tlbsrch` 按 TLBEHI.VPPN 查找，命中时把命中条目的 INDEX/PS 写回
*          TLBIDX、把 TLBELO0/1 写回 CSR。未命中时 TLBIDX 的 EHINV 位
*          （bit 31）置 1。这条接口只读，不建映射、不改状态。
*          存在的理由：tlbwr 写完一条条目后，「查找到底命中谁」在 C 层
*          看不见；地址空间切换曾经**静默失效**（写进 space 1 的数据在
*          space 0 里读得到），就是靠它把「写进去了但没生效」和「生效了
*          但命中另一条」分开的。
*/
uint32 arkrtos_port_paging_lookup(uint32 va, uint32 *index, uint32 *ppn, uint32 *ps)
{
#if (ARKRTOS_USE_PRIV == 1)
    unsigned long idx = 0UL;
    unsigned long lo0 = 0UL;

    LA_CSRW(LA_CSR_TLBEHI, (unsigned long)va);
    __asm__ volatile("tlbsrch" ::: "memory");
    LA_CSRR(LA_CSR_TLBIDX, idx);

    /* TLBSRCH 只写 TLBIDX（INDEX/NE），**不**写 TLBELO；条目内容要用 TLBRD
     * 按 TLBIDX.INDEX 读回。少了这一步，读到的 PPN 会是上一次 tlbwr 留下的
     * 陈旧值 —— 看起来「对上了」，其实什么都没验证。 */
    if(((uint32)idx & 0x80000000u) != 0u)     /* NE：未命中 */
    {
        return 0u;
    }
    __asm__ volatile("tlbrd" ::: "memory");
    LA_CSRR(LA_CSR_TLBIDX, idx);
    LA_CSRR(LA_CSR_TLBELO0, lo0);
    if(((uint32)idx & 0x80000000u) != 0u)     /* 条目无效 */
    {
        return 0u;
    }
    if(((uint32)lo0 & LA_PTE_V) == 0u)
    {
        return 0u;
    }

    if(index != 0)
    {
        *index = (uint32)idx & 0xFFFu;
    }
    if(ppn != 0)
    {
        *ppn = (uint32)(lo0 & ~0xFFFUL);
    }
    if(ps != 0)
    {
        *ps = ((uint32)idx >> 24) & 0x3Fu;
    }
    return 1u;
#else
    (void)va;
    (void)index;
    (void)ppn;
    (void)ps;
    return 0u;
#endif
}

#if (ARKRTOS_USE_PRIV == 1)

/* ------------------------------------------------------------
 * 动态映射（运行期登记，给驱动登记 MMIO 用）
 * @details 静态那四条只顾内核镜像 / App 页 / 两个 UART 页。驱动在板级初始化
 *          时如果要用一个不在那四条里的设备窗口，就必须能往 STLB 里加一条，
 *          这里就是那条路。粒度固定 4 KB：大页要占 MTLB（64 条，靠 tlbwr
 *          确定性写入），那是「每进程页表」那条线的事，不在这条接口里混做。
 *
 *          四条硬约束，全部按「宁可报错」：
 *            1. va/pa 必须 4 KB 对齐，属性只接受 RAM / ROM / PERIPH_RO /
 *               PERIPH_RW。App 页要 ASID 与 PLV3 条目（属 U-1），这里一律
 *               拒绝，而不是拿 PLV0 条目凑一个「看着能跑」的映射。
 *            2. 落在静态跨度里的 VA 一律拒绝：同一 VA 两条条目谁先匹配
 *               取决于表，症状是随机的重填错误。
 *            3. 条目按「偶页 + 奇页」成对写（硬件一条条目管两页）。只登记
 *               一半时另一半保持 V=0 —— 绝不顺手把邻居页也放开。
 *            4. STLB 一组只有 8 路、而 tlbfill 的路号是随机的，所以登记前
 *               先数该组已用条目（静态 4 KB 条目 + 已登记的对），到 8 就
 *               拒绝，而不是等随机覆盖把内核 UART 顶掉。
 * ------------------------------------------------------------ */
/* 动态条目在 MTLB 里的起始偏移：静态三条大页占 2048..2050 */
#define LA_MTLB_DYN_FIRST   3u
#define LA_PG_DYN_PAIRS     8u

typedef struct {
    uint32        used;
    uint32        pair;          /* va 与 va+4 KB 所在的那一对，8 KB 对齐 */
    uint32        pa[2];         /* 每页登记到的物理页基址（冲突判定用） */
    unsigned long lo[2];         /* 0 = 该页不做映射（TLBELO 留 V=0） */
    uint32        mask[2];       /* 该页允许的 mem 位掩码 */
} la_pg_dyn_t;

static la_pg_dyn_t s_pg_dyn[LA_PG_DYN_PAIRS];

/** 这段 VA 是否落在静态跨度里（重叠即算） */
static uint32 la_pg_static_hit(uint32 base, uint32 size)
{
    uint32 i;

    for(i = 0u; i < LA_PG_SPAN_COUNT; i++)
    {
        const la_pg_span_t *p = &s_pg_spans[i];

        if((base < (p->base + p->size)) && (p->base < (base + size)))
        {
            return 1u;
        }
    }

    return 0u;
}

/** mem -> TLBELO；0 表示这条属性不提供动态映射
 *  @note 开 ASID 档时四条都带 G（全局）：动态别名是**内核**的 MMIO 通路
 *        （PLV0），它不属于任何一个 App 空间，绑死在某一个 ASID 上就会出现
 *        「换个空间就 VFS 读不动 / 控制台写不出」这种只在开档后显形的形态。 */
static unsigned long la_pg_dyn_lo(uint32 pa, arkrtos_mpu_mem_t mem)
{
    switch(mem)
    {
    case ARKRTOS_MPU_MEM_RAM:
        return LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(1u) | LA_PTE_PLV(0u) | LA_PG_DYN_G | LA_PTE_PPN(pa);

    case ARKRTOS_MPU_MEM_ROM:
        return LA_PTE_V | LA_PTE_CCA(1u) | LA_PTE_PLV(0u) | LA_PG_DYN_G | LA_PTE_PPN(pa);

    case ARKRTOS_MPU_MEM_PERIPH_RW:
        return LA_PTE_V | LA_PTE_WE | LA_PTE_CCA(0u) | LA_PTE_PLV(0u) | LA_PG_DYN_G | LA_PTE_PPN(pa);

    case ARKRTOS_MPU_MEM_PERIPH_RO:
        return LA_PTE_V | LA_PTE_CCA(0u) | LA_PTE_PLV(0u) | LA_PG_DYN_G | LA_PTE_PPN(pa);

    default:
        return 0UL;
    }
}

/** 属性掩码：RW 的页同时满足 RO 的审计，反过来不行 */
static uint32 la_pg_dyn_mask(arkrtos_mpu_mem_t mem)
{
    if(mem == ARKRTOS_MPU_MEM_PERIPH_RW)
    {
        return LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RO) | LA_PG_BIT(ARKRTOS_MPU_MEM_PERIPH_RW);
    }

    return LA_PG_BIT(mem);
}

/**
* @brief 追加一条动态映射（一页 4 KB）
* @return 0 = 已进 TLB 且可审计（同一页同一属性重复登记也是 0）；
*         -1 = 参数/属性不支持；-2 = 与静态映射重叠；-3 = 动态表满；
*         -5 = 同一 VA 已登记成别的物理页/属性
*/
int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem)
{
    uint32 pair;
    uint32 half;
    uint32 i;
    uint32 slot = LA_PG_DYN_PAIRS;
    unsigned long lo;

    if(((va & 0xFFFu) != 0u) || ((pa & 0xFFFu) != 0u))
    {
        return -1;                       /* 粒度是 4 KB，不对齐就不猜 */
    }

    lo = la_pg_dyn_lo(pa, mem);
    if(lo == 0UL)
    {
        return -1;                       /* 该属性动态映射不提供 */
    }

    pair = va & ~0x1FFFu;
    half = (va >> 12) & 1u;

    if(la_pg_static_hit(va, 0x1000u) != 0u)
    {
        return -2;
    }

    /* 这一对已经登记过就复用槽位（幂等），否则找一个空槽 */
    for(i = 0u; i < LA_PG_DYN_PAIRS; i++)
    {
        if((s_pg_dyn[i].used != 0u) && (s_pg_dyn[i].pair == pair))
        {
            slot = i;
            break;
        }
    }

    if(slot == LA_PG_DYN_PAIRS)
    {
        for(i = 0u; i < LA_PG_DYN_PAIRS; i++)
        {
            if(s_pg_dyn[i].used == 0u)
            {
                slot = i;
                break;
            }
        }

        if(slot == LA_PG_DYN_PAIRS)
        {
            return -3;
        }

        /* 条目落 MTLB，不与 STLB 抢组；上限就是那 8 个槽位本身 */

        s_pg_dyn[slot].used    = 1u;
        s_pg_dyn[slot].pair    = pair;
        s_pg_dyn[slot].pa[0]   = 0u;
        s_pg_dyn[slot].pa[1]   = 0u;
        s_pg_dyn[slot].lo[0]   = 0UL;
        s_pg_dyn[slot].lo[1]   = 0UL;
        s_pg_dyn[slot].mask[0] = 0u;
        s_pg_dyn[slot].mask[1] = 0u;
    }

    /* 同一 VA 已经登记成别的物理页（或别的属性）时拒绝：静默改绑会让
     * 「我登记过这条」变成一句没有内容的话，也会让已经拿着这个 VA 的
     * 调用方去访问另一块内存 —— 那是错答案，不是重映射。 */
    if(s_pg_dyn[slot].lo[half] != 0UL)
    {
        if((s_pg_dyn[slot].pa[half] != pa) || (s_pg_dyn[slot].lo[half] != lo))
        {
            return -5;
        }

        /* 一字不差的重复登记：幂等 */
        la_tlb_write(pair, 12u, LA_MTLB_INDEX + LA_MTLB_DYN_FIRST + slot,
                     s_pg_dyn[slot].lo[0], s_pg_dyn[slot].lo[1]);
        return 0;
    }

    s_pg_dyn[slot].pa[half]   = pa;
    s_pg_dyn[slot].lo[half]   = lo;
    s_pg_dyn[slot].mask[half] = la_pg_dyn_mask(mem);

    /* 一对页一起写：TLBELO1 的 PA 由硬件推成 LO0 + 2^PS，所以没登记的那
     * 一半必须是 0（V=0、不匹配），绝不能拿「另一页的 PA + 4 KB」凑一条
     * 有效条目把邻居页顺手放开。
     *
     * 用 la_tlb_write（tlbwr）而不是 tlbfill：动态登记是运行期的，走 tlbfill
     * 会随机挑路、有概率把同组的内核 UART 条目顶掉（实测踩到过一次，症状是
     * 登记成功后的下一次控制台输出直接 KERNEL refill）。条目落 MTLB 的
     * 绝对索引，就不会与静态 4 KB 条目抢 STLB 的组。 */
    la_tlb_write(pair, 12u, LA_MTLB_INDEX + LA_MTLB_DYN_FIRST + slot,
                 s_pg_dyn[slot].lo[0], s_pg_dyn[slot].lo[1]);

    return 0;
}

/**
* @brief 撤销一条动态映射（U-15）
* @return 0 = 已撤销；-1 = 未 4 KB 对齐；-2 = 落在静态跨度里；-4 = 没登记过
* @details 动态条目落在 MTLB 的绝对索引上（LA_MTLB_INDEX + LA_MTLB_DYN_FIRST
*          + slot），撤销时按同一个索引 tlbwr 写回一条 V=0 的条目。
*          **但只重写 MTLB 不够**：这个 VA 一旦被访问过，STLB 与 QEMU 的
*          softmmu 缓存里还留着旧翻译，tlbwr 只改 machine TLB，不会回头看
*          STLB（实测症状：look 报 no entry，可经这个 VA 读出来的仍是旧物理
*          页）。所以两半都先 invtlb 再写。静态映射一律拒绝（-2）：那是内核
*          自己的条目，撤掉它下一次取指就 KERNEL refill。
*
*          只清被撤的那一半：一对 8 KB 里另一半还登记着就不能一起丢。两半
*          都空了才把槽位标回空闲，让下一个调用方能用上它 —— 这正是 U-15 缺
*          的那一半能力（「动态映射只增不减」）。 */
int32 arkrtos_port_paging_unalias(uint32 va)
{
    uint32 pair;
    uint32 half;
    uint32 i;
    uint32 slot = LA_PG_DYN_PAIRS;

    if((va & 0xFFFu) != 0u)
    {
        return -1;
    }

    if(la_pg_static_hit(va, 0x1000u) != 0u)
    {
        return -2;
    }

    pair = va & ~0x1FFFu;
    half = (va >> 12) & 1u;

    for(i = 0u; i < LA_PG_DYN_PAIRS; i++)
    {
        if((s_pg_dyn[i].used != 0u) && (s_pg_dyn[i].pair == pair))
        {
            slot = i;
            break;
        }
    }

    if((slot == LA_PG_DYN_PAIRS) || (s_pg_dyn[slot].lo[half] == 0UL))
    {
        return -4;                       /* 本来就没登记过：不猜，报错 */
    }

    s_pg_dyn[slot].pa[half]   = 0u;
    s_pg_dyn[slot].lo[half]   = 0UL;
    s_pg_dyn[slot].mask[half] = 0u;

    if((s_pg_dyn[slot].lo[0] == 0UL) && (s_pg_dyn[slot].lo[1] == 0UL))
    {
        s_pg_dyn[slot].used = 0u;        /* 两半都空了，槽位回收 */
        s_pg_dyn[slot].pair = 0u;
    }

    /* 先让这个 8 KB 对的两半在 STLB / softmmu 里都失效，再写 machine TLB。
     * 顺序不能颠倒：invtlb 是按旧条目的 PS 匹配的，写在后面会查不到旧条目。 */
    la_tlb_inval_page(pair);
    la_tlb_inval_page(pair + 0x1000u);

    /* 剩下的那一半（或 _ ，两半都空时）照原样写回同一条 MTLB 条目：
     * 空的那一半 V=0，不会匹配，也就不会顺手把邻居页放开。 */
    la_tlb_write(pair, 12u, LA_MTLB_INDEX + LA_MTLB_DYN_FIRST + slot,
                 s_pg_dyn[slot].lo[0], s_pg_dyn[slot].lo[1]);

    /* invtlb 在 QEMU 上会连带打掉 App 窗口那条 PS=20 条目（实测：撤销
     * 之后再访问窗口 = KERNEL refill）。窗口内容本来就没变，重写一遍
     * 把它放回原位。 */
    la_pg_assert_window();

    return 0;
}

/**
* @brief 已占用的动态映射槽位数
* @details 只会数 used 标志，不去读 TLB：读回路径（tlbsrch/tlbrd）在
*          撤销之后不一定还能告诉你「槽位空着」，而槽位书是真的。 */
uint32 arkrtos_port_paging_dyn_used(void)
{
    uint32 i;
    uint32 n = 0u;

    for(i = 0u; i < LA_PG_DYN_PAIRS; i++)
    {
        if(s_pg_dyn[i].used != 0u)
        {
            n++;
        }
    }

    return n;
}

#else  /* ARKRTOS_USE_PRIV == 0：没有 TLB 通路，动态映射一律拒绝 */

int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem)
{
    (void)va;
    (void)pa;
    (void)mem;

    return -1;
}

int32 arkrtos_port_paging_unalias(uint32 va)
{
    (void)va;

    return -1;                           /* 没有 TLB 通路就没有东西可撤 */
}

uint32 arkrtos_port_paging_dyn_used(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_PRIV == 1 */

/**
* @brief 报告分页通路可用
* @return 0 = 本 port 有分页通路
* @details 本函数**不**建映射：映射由 la_mmu_init() 在 arkrtos_port_irq_init()
*          里建好，而 arkrtos_mpu_module_init() 可能比它更早被调用。在这里
*          读 CRMD.PG 判断「是否已生效」会得到一个与调用时机有关的答案，
*          那不是这条接口要回答的问题。真正建映射失败时 la_mmu_init 之后的
*          首条取指就会 refill 致命，不会静默。
*/
int32 arkrtos_port_paging_init(void)
{
    return (ARKRTOS_USE_PRIV == 1) ? 0 : -1;
}

/**
* @brief 审计一段地址跨度是否被静态映射覆盖
* @param base 起始虚拟地址
* @param size 字节数
* @param mem  期望属性
* @return 0 = 已覆盖且权限满足；-1 = 未覆盖 / 权限不足 / 参数非法
* @details 判定按「整段必须落在单条跨度之内」：跨两条条目的跨度不在这里
*          接受，因为调用方（TCB 窗口）本来就是单块连续内存，跨条目只可能
*          是记录坏了 —— 那种情况下报 -1 才是诚实的。
*/
int32 arkrtos_port_paging_check(uint32 base, uint32 size, arkrtos_mpu_mem_t mem)
{
    uint32 i;
    uint32 end;

    if((size == 0u) || (base > (0xFFFFFFFFu - size)))
    {
        return -1;
    }

    end = base + size;

    for(i = 0u; i < LA_PG_SPAN_COUNT; i++)
    {
        const la_pg_span_t *p = &s_pg_spans[i];

        if((base >= p->base) && (end <= (p->base + p->size)))
        {
            return ((p->mem & LA_PG_BIT(mem)) != 0u) ? 0 : -1;
        }
    }

#if (ARKRTOS_USE_PRIV == 1)
    /* 动态登记的页：同一条口径，整段必须落在一页之内 */
    for(i = 0u; i < LA_PG_DYN_PAIRS; i++)
    {
        uint32 h;

        if(s_pg_dyn[i].used == 0u)
        {
            continue;
        }

        for(h = 0u; h < 2u; h++)
        {
            uint32 page = s_pg_dyn[i].pair + (h * 0x1000u);

            if((s_pg_dyn[i].lo[h] != 0UL) && (base >= page) &&
               (end <= (page + 0x1000u)))
            {
                return ((s_pg_dyn[i].mask[h] & LA_PG_BIT(mem)) != 0u) ? 0 : -1;
            }
        }
    }
#endif /* ARKRTOS_USE_PRIV == 1 */

    return -1;
}

/**
* @brief 让一段地址的翻译失效
* @details 静态映射从不撤销：真去 invtlb 会把内核自己的条目打掉，下一次
*          取指就走 TLBRENTRY。所以这里是空操作，而不是「尽力而为」。
*/
void arkrtos_port_paging_flush(uint32 base, uint32 size)
{
    (void)base;
    (void)size;
}

/* ------------------------------------------------------------
 * 4 KB 页帧池（U-3）
 * @details 一块板级给定的物理区间（ARKRTOS_QEMU_FRAME_BASE 起
 *          ARKRTOS_QEMU_FRAME_COUNT 帧），用一张位图记录哪些帧已分配。
 *          这是「帧从哪来」这一层：分配器只管记账与连续区间搜索，
 *          **不**建映射、**不**碰 App 窗口。
 *
 *          三条不猜的规矩：
 *            1. 帧号与地址的换算是 base + i * 4 KB，不做别的解释；
 *            2. 分配必须给出一段**连续**的帧（App 的 2 MB 窗口要一整块），
 *               找不到就返回 -2，不做「只给一部分」；
 *            3. 释放一个没分配过的帧返回 -3 且**一个字节不动** —— 静默
 *               把位图清零会让两个持有者拿到同一帧。
 * ------------------------------------------------------------ */
#if (ARKRTOS_USE_PAGING_FRAMES == 1)

#ifndef ARKRTOS_QEMU_FRAME_BASE
#error "ARKRTOS_USE_PAGING_FRAMES=1 requires ARKRTOS_QEMU_FRAME_BASE from the board"
#endif
#ifndef ARKRTOS_QEMU_FRAME_COUNT
#error "ARKRTOS_USE_PAGING_FRAMES=1 requires ARKRTOS_QEMU_FRAME_COUNT from the board"
#endif
#if ((ARKRTOS_QEMU_FRAME_BASE & 0x7FFFFFu) != 0u)
#error "frame pool base must be 8 MB aligned (the PS=22 identity entry needs it)"
#endif
#if (ARKRTOS_QEMU_FRAME_COUNT == 0u) || (ARKRTOS_QEMU_FRAME_COUNT > 2048u)
#error "frame pool must hold 1..2048 frames (8 MB, one PS=22 pair)"
#endif

#define LA_FR_BIT_WORDS     ((ARKRTOS_QEMU_FRAME_COUNT + 31u) / 32u)

static uint32 s_fr_bits[LA_FR_BIT_WORDS];
static uint32 s_fr_used;

/** 该帧是否已分配 */
static uint32 la_fr_test(uint32 i)
{
    return (s_fr_bits[i >> 5] >> (i & 31u)) & 1u;
}

/** 标记已分配（调用方保证之前是空闲） */
static void la_fr_mark(uint32 i)
{
    s_fr_bits[i >> 5] |= (1u << (i & 31u));
    s_fr_used++;
}

/** 标记空闲（调用方保证之前是已分配） */
static void la_fr_clear(uint32 i)
{
    s_fr_bits[i >> 5] &= ~(1u << (i & 31u));
    s_fr_used--;
}

/**
* @brief 分配一段连续的 4 KB 页帧
* @param count 需要的帧数（> 0）
* @param p_base 成功时写回首帧的物理基址
* @return 0 = 成功；-1 = 参数非法；-2 = 没有这么长的连续空闲段
* @details 从低帧号往高扫，取第一段满足的 run（first-fit）。不做碎片整理：
*          整理会移动别人正在用的帧，那不是分配器该悄悄做的事。
*/
int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base)
{
    uint32 i;
    uint32 run;

    if((count == 0u) || (p_base == 0) || (count > (uint32)ARKRTOS_QEMU_FRAME_COUNT))
    {
        return -1;
    }

    run = 0u;
    for(i = 0u; i < (uint32)ARKRTOS_QEMU_FRAME_COUNT; i++)
    {
        if(la_fr_test(i) == 0u)
        {
            run++;
            if(run == count)
            {
                uint32 first = i + 1u - count;
                uint32 k;

                for(k = first; k <= i; k++)
                {
                    la_fr_mark(k);
                }

                *p_base = ARKRTOS_QEMU_FRAME_BASE + (first * 0x1000u);
                return 0;
            }
        }
        else
        {
            run = 0u;
        }
    }

    return -2;
}

/**
* @brief 归还一段连续 4 KB 页帧
* @param base 首帧物理基址（必须 4 KB 对齐、落在池内）
* @param count 帧数（> 0）
* @return 0 = 已归还；-1 = 不在池内 / 未对齐 / count 越界；
*         -3 = 这段里本来就有没分配过的帧（此时**一帧都不动**）
* @details 「先全部验证再改」：只要有一帧本来空闲，整段拒绝。否则同一个
*          帧会被两个持有者同时以为归自己，那个 bug 只在很久以后显形。
*/
int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count)
{
    uint32 first;
    uint32 k;

    if((count == 0u) || ((base & 0xFFFu) != 0u) ||
       (base < (uint32)ARKRTOS_QEMU_FRAME_BASE))
    {
        return -1;
    }

    first = (base - (uint32)ARKRTOS_QEMU_FRAME_BASE) >> 12;
    if((first >= (uint32)ARKRTOS_QEMU_FRAME_COUNT) ||
       (count > ((uint32)ARKRTOS_QEMU_FRAME_COUNT - first)))
    {
        return -1;
    }

    for(k = first; k < (first + count); k++)
    {
        if(la_fr_test(k) == 0u)
        {
            return -3;
        }
    }

    for(k = first; k < (first + count); k++)
    {
        la_fr_clear(k);
    }

    return 0;
}

/** 帧总数 */
uint32 arkrtos_port_paging_frame_total(void)
{
    return (uint32)ARKRTOS_QEMU_FRAME_COUNT;
}

/** 已分配帧数 */
uint32 arkrtos_port_paging_frame_used(void)
{
    return s_fr_used;
}

#else  /* ARKRTOS_USE_PAGING_FRAMES == 0：没有帧池 */

int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base)
{
    (void)count;
    (void)p_base;

    return -1;
}

int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count)
{
    (void)base;
    (void)count;

    return -1;
}

uint32 arkrtos_port_paging_frame_total(void)
{
    return 0u;
}

uint32 arkrtos_port_paging_frame_used(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_PAGING_FRAMES == 1 */

#endif /* ARKRTOS_USE_PAGING == 1 */

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
* @brief Linux/LoongArch ABI 的系统调用号（syscall 0 + $a7）
* @details Ark 自己的服务号写在 `syscall` 的 15 位立即数里，Linux 用
*          `syscall 0` 并把号放进 $a7。因此「立即数是否为 0」就是分界，
*          不需要按号段猜，两边也不会撞号。
*/
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx)
{
    la_frame_word *frame = (la_frame_word *)p_exc_ctx;
    uint32 pc;
    uint32 insn;

    pc   = frame[LA_FR_ERA];
    insn = *(volatile uint32 *)(unsigned long)(pc - 4u);

    if((insn & 0xFFFF8000u) != LA_INSN_SYSCALL_OP)
    {
        return 0xFFFFFFFFu;
    }
    if((insn & LA_INSN_SYSCALL_NUM) != 0u)
    {
        return 0xFFFFFFFFu;
    }

    return (uint32)frame[LA_FR_A0 + 7u];   /* $a7 = a0 + 7 */
}

void arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value)
{
    /* 负 errno 必须符号扩展到 64 位：只写低 32 位时程序会把
     * 0x00000000FFFFFFEA 读成一个正数（"成功的假象"）。 */
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

    /* **异常内部不允许开中断。** LoongArch 的 ERA/PRMD 只有一份、没有嵌套
     * 栈：在异常处理程序里把 CRMD.IE 打开，节拍连接进来就成了嵌套异常，
     * 而嵌套异常的出口一旦发生任务切换（s_switch_pending 可能是外层刚刚
     * 置上的），外层处理程序的 C 栈帧就再也回不去了 —— 它那句
     * `s_in_trap--` 永远执行不到，于是 s_in_trap 永久漏 1。之后**所有**
     * 任务上下文的切换请求都被当成「在中断里」而只置 pending：
     * `arkrtos_task_wait_internal()` 会把任务标成 WAIT、挂进延时链，然后
     * 立刻返回（0 拍），要等下一拍节拍在异常出口才真的切走。实测症状是
     * A 任务的等待 0 拍就返回、SVC 读到的毫秒比本地读数大 500 ms。
     *
     * 异常返回时 PRMD.PIE 会按被打断时的状态恢复，所以这里不做事才是对的：
     * 该开的中断由 ertn 负责开。 */
    if(s_in_trap != 0u)
    {
        return;
    }

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

/* LoongArch 没有 PSP/CONTROL 这组寄存器，这三个接口保留是为了满足 HAL 的
 * 完整声明。线程级特权走下面的 arkrtos_port_set_thread_priv()：它不是
 * CPU 寄存器，而是「下一个帧的 PRMD.PPLV」。 */
void arkrtos_port_set_psp(uint32 val)
{
    (void)val;
}

#if (ARKRTOS_USE_TASK_SWAP == 1)
/* ---- 任务热切换的端口层（LA64） -------------------------------------
 * 帧格式见 la64/arkrtos_context.S 的文件头（256 字节 / 32 个 64 位字），
 * ERA 在 [30]、PRMD 在 [31]，a0..a7 在 [3..10]。恢复路径是
 * 「la_trap_c 的返回值」——没有 PSP 可挪，所以 activate 把新帧记在
 * s_la_swap_frame 里，由 la_trap_c 在 SVC_Server 返回之后取走。
 *
 * 帧里必须改的四个位置：
 *   ERA  <- 新入口
 *   SP   <- 新块里的栈顶（不能留旧值：整块拷贝后 sp 槽里还是旧块地址）
 *   RA   <- 0（新 main 返回即跳 0，与新建任务同一口径）
 *   PRMD <- 保留 PPLV[1:0]（回到哪个特权级），置 PIE[2]（ertn 后 IE=1）
 * a0..a7 清零；tp/fp/s0-s8 保留原值。
 * ------------------------------------------------------------------- */
static la_frame_word *s_la_swap_frame = 0;

int32 arkrtos_port_swap_frame_ok(const void *p_exc_ctx)
{
    const la_frame_word *p_frame = (const la_frame_word *)p_exc_ctx;
    unsigned long era;
    unsigned long plv;

    if(p_frame == 0)
    {
        return 0;
    }
    era = p_frame[LA_FR_ERA];
    plv = p_frame[LA_FR_PRMD] & 3UL;
    if((era == 0UL) || ((era & 3UL) != 0UL))
    {
        return 0;
    }
    /* PRMD.PPLV 只能是 0（PLV0 内核）或 3（PLV3 App）：其余值说明这不是一份
     * ertn 能用的帧，拒绝而不是猜。 */
    if((plv != 0UL) && (plv != 3UL))
    {
        return 0;
    }
    return 1;
}

uint32 arkrtos_port_swap_frame_bytes(void)
{
    return LA_FR_SIZE;
}

void arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry)
{
    la_frame_word *p_frame = (la_frame_word *)p_exc_ctx;
    uint32 i;

    for(i = LA_FR_A0; i < (LA_FR_A0 + 8u); i++)
    {
        p_frame[i] = 0UL;
    }

    p_frame[LA_FR_RA]   = 0UL;
    p_frame[LA_FR_SP]   = (unsigned long)p_frame + (unsigned long)LA_FR_SIZE;
    p_frame[LA_FR_ERA]  = (unsigned long)entry;
    p_frame[LA_FR_PRMD] = (p_frame[LA_FR_PRMD] & 3UL) | (unsigned long)LA_PRMD_INIT;
}

void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx)
{
    (void)p_old_ctx;
    s_la_swap_frame = (la_frame_word *)p_new_ctx;
}
#endif /* ARKRTOS_USE_TASK_SWAP == 1 */

uint32 arkrtos_port_get_control(void)
{
    return 0u;
}

void arkrtos_port_set_control(uint32 val)
{
    (void)val;
}

/**
* @brief 记录下一个任务的特权级（由 arkrtos_sched_activate 每次切换时调用）
* @param is_priv 1 = 内核任务（PLV0），0 = App（PLV3）
* @details 这里**不能**直接写 CRMD.PLV：本函数在 sched_activate() 内部被
*          调用，此刻还不知道要恢复哪个帧；而 ertn 恢复特权级只认帧里的
*          PRMD.PPLV。所以只记下来，由拿帧的一方用 LA_PRIV_APPLY 补写。
*/
void arkrtos_port_set_thread_priv(uint32 is_priv)
{
#if (ARKRTOS_USE_PRIV == 1)
    s_next_pplv = (is_priv != 0u) ? 0u : 3u;
#else
    (void)is_priv;
#endif
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
 * 周期级时间基准（rdtime）
 * ------------------------------------------------------------
 * LoongArch 上「周期计数器」只有一个来源：rdtime（读的就是 CSR 0x40 =
 * TID，稳定计数器低 32 位）。所谓 rdcntvl.w 是它的助记符别名，**不是**另
 * 一条按核频走的 TSC —— 所以这里没有「两条不同源的计数」可校准，频率就是
 * 板级常量 ARKRTOS_PORT_RDTIME_HZ（QEMU virt 实测 100 MHz）。
 *
 * 两个实测踩到的坑，写在这里免得下次重踩：
 *   ① 本工具链的 binutils 不认 rdcntvl.w 助记符（"no match insn"），
 *      只能写 rdtime.d（或用同样编码的 csrrd rd, 0x40）；
 *   ② csrrd rd, 0x40 在 QEMU 上读回来**不动**（实测：两次读之间夹 200 次
 *      加法，值不变），于是 arkrtos_rt_counter_alive() 判「计数器不走」，
 *      rt show 只会报"cycle counter unavailable"。改成 rdtime.d 之后
 *      同一条判活立刻就过了 —— 内核其它地方本来就用 rdtime，保持同源。
 *
 * 所以 cyccnt_init() 不做校准（没有第二个源可对），只把频率置成常量；
 * cycle_clock_measured() 返回 0，读数页据此标 [board constant]。
 * RTSTAT 关闭时本段不产生任何代码。
 * ============================================================ */
#if (ARKRTOS_USE_RTSTAT == 1)

static uint32 s_cycle_hz;              /* 实测的 TSC 频率；0 = 未校准 */

static uint32 la_tsc_read(void)
{
    /* 直接复用内核那一条 rdtime 读数：同一条计数器，同一个 32 位截断口径。
     * 不走 csrrd 0x40 的原因见上面 ① ② 两条实测。 */
    return la_rdtime();
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

#else  /* RTSTAT == 0：保留原来的「不可用」桩，零体积 */

int32 arkrtos_port_cyccnt_init(void)
{
    return -1;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_RTSTAT */

#if (ARKRTOS_USE_TICKLESS == 1)
/* TCFG 是周期定时器，TVAL 会重载，单看它分不清"睡满"与"刚重载"。
 * 所以编程时留一个 rdtime 快照，按两个时钟的频率比把经过的时间折成
 * TCFG 计数。rdtime 与 TCFG 都是板级常量频率，比值是整数。 */
static uint32 s_sleep_time;

uint32 arkrtos_port_timer_sleep_max(void)
{
    /* TCFG.InitVal 只有 [31:2] 共 30 位可用。 */
    return 0x3FFFFFFFu;
}

uint32 arkrtos_port_timer_elapsed_counts(uint32 programmed)
{
    uint32 dt = (uint32)(la_rdtime() - s_sleep_time);
    unsigned long long c;

    if(dt == 0u)
    {
        return 1u;
    }

    c = ((unsigned long long)dt * (unsigned long long)ARKRTOS_PORT_TIMER_HZ) /
        (unsigned long long)ARKRTOS_PORT_RDTIME_HZ;

    (void)programmed;
    return (c == 0ull) ? 1u : (uint32)c;
}
#endif /* ARKRTOS_USE_TICKLESS */

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

#if (ARKRTOS_USE_TICKLESS == 1)
    /* 与 la_tcfg_write 紧挨着取快照：两者之间只差几条指令。 */
    s_sleep_time = la_rdtime();
#endif
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
    uint32 factor = ARKRTOS_PORT_RDTIME_HZ / 1000000u;
    uint32 wait;

    /* 溢出保护必须按「每微秒多少计数」算，不能写死一个常数：
     * 早先写死 42949（= 2^32/100000），在 RDTIME_HZ=100 MHz 下把
     * 阈值缩小了一千倍 —— 任何超过 42.9 ms 的延时都会被夹成
     * 0xFFFFFFFF 个计数，也就是 42.9 秒（实测 500 ms 的 App 延时
     * 变成 42.9 s，看起来像「任务卡死」）。 */
    if(factor == 0u)
    {
        factor = 1u;
    }

    wait = (us > (0xFFFFFFFFu / factor)) ? 0xFFFFFFFFu : (us * factor);

    while((uint32)(la_rdtime() - start) < wait)
    {
        /* 忙等：这段时间刻意不睡，语义与 ARM 版一致 */
    }
}

/* ============================================================
 * 异常分发
 * ============================================================ */

/* 切换路径环形日志：只在致命异常转储里读，正常路径上只是几条 store，
 * 不打印、不死等，因此不会像打印探针那样把时序搅乱（那会让 bug 躲起来）。 */
extern volatile uint32 arkrtos_ready_heal_cnt;
extern volatile uint32 arkrtos_ready_heal_g;
extern volatile uint32 arkrtos_ready_heal_word;
extern volatile uint32 arkrtos_ready_heal_bit;
extern volatile uint32 arkrtos_ready_heal_head;
extern volatile uint32 arkrtos_ready_heal_total;

/* 每条 8 个字：next / 旧帧 / 新帧 / 切换后 cur_id / 目标切换前 status /
 * 目标切换前 ready_next / 目标帧里的 ERA 槽 / 目标帧里的 SP 槽。
 * 前六个字段是「死任务被重新调度」这类脱钩现场的自证材料；后两个是给
 * 「resume 之后跳到一个非代码地址」这条致命异常准备的：只要目标帧的
 * ERA 槽不是一个代码地址（或者 SP 槽等于该任务的栈顶 —— 那是
 * arkrtos_port_stack_init 才会写的形状），就能当场认出「resume 用的不是
 * 一份真正的保存帧」。 */
#define LA_SWP_FIELDS   8u
uint32 s_swp_log[LA_SWP_FIELDS * 16u];
/* 任务表是内核全局对象（arkrtos_cfg.h），port 层就地声明，
 * 不为一条诊断日志给公共头文件拉依赖。 */
extern arkrtos_task_t arkrtos_task_table[];
extern int32 arkrtos_task_count;
extern volatile uint32 arkrtos_kill_ret_cnt;
uint32 s_swp_pos = 0u;
uint32 s_swp_enter = 0u;
uint32 s_swp_exit = 0u;
uint32 s_swp_bogus = 0u;         /* 被栈帧合法性闸门拦下的次数 */
uint32 s_swp_bogus_next = 0u;    /* 最近一次被拦下的栈指针 */
uint32 s_swp_bogus_cur = 0u;     /* 拦下时的当前任务号 */

/* 内核 RAM 窗（与 example 的链接脚本一致）：所有任务的栈都在这里，
 * 帧指针不可能落在窗外，落在窗外就只可能是越界读出来的垃圾。 */
#define LA_FRAME_MIN    0x00300000u
#define LA_FRAME_MAX    0x00B00000u

/**
* @brief 就地切换的 C 半段（由 arkrtos_context.S 的立即切换路径调用）
* @param frame 刚构造好的当前帧
* @return 要恢复的帧；不需要切换时原样返回
* @note 这里还有一道栈帧合法性闸门：next_sp 必须落在内核 RAM 窗内且按
*       16 字节对齐，否则拒绝切换并计数（不拿野指针去 ld.d）。
*/
la_frame_word *la_switch_inline(la_frame_word *frame)
{
    int32 next;
    uint32 next_sp;

#if (ARKRTOS_USE_PRIV == 1)
    /* 本任务的 PLV：activate() 会把 s_next_pplv 改成「目标任务」的值，
     * 而不切换（或切换被闸门拦下）时活在 ARM 上的仍然是本任务本身，
     * 它的帧必须按本任务的特权级回落，否则一个 App 任务会被降成 PLV0。 */
    uint32 self_pplv = s_next_pplv;
#endif

    s_swp_enter++;

    next = arkrtos_sched_is_switching();
    if(next < 0)
    {
        s_swp_exit++;
        return frame;
    }

    {
        /* 切换目标的「切换前」状态，必须在 activate 改它之前取 */
        uint32 i  = (s_swp_pos & 15u) * LA_SWP_FIELDS;
        uint32 st = 0u;
        uint32 rd = 0u;

        if((next > 0) && (next <= (int32)arkrtos_task_count))
        {
            st = (uint32)arkrtos_task_table[next - 1].status;
            rd = (uint32)arkrtos_task_table[next - 1].ready_next;
        }

        next_sp = arkrtos_sched_activate(next, (uint32)(unsigned long)frame);

        s_swp_log[i + 0u] = (uint32)next;
        s_swp_log[i + 1u] = (uint32)(unsigned long)frame;
        s_swp_log[i + 2u] = next_sp;
        s_swp_log[i + 3u] = (uint32)arkrtos_current_task_id;
        s_swp_log[i + 4u] = st;
        s_swp_log[i + 5u] = rd;
        s_swp_pos++;
    }
    s_swp_exit++;
    if(next_sp != 0u)
    {
        /* 栈帧合法性闸门：任务的栈全部落在内核 RAM 窗内、且按 16 字节对齐
         * （arkrtos_port_stack_init 就是按 16 对齐造的帧）。内核侧已经有
         * 任务号越界检查，这里是架构层的最后一道：宁可原地不切，
         * 也不要拿一个野指针去 ld.d —— 那会变成一条没有任何线索的访存异常。 */
        /* 合法窗有两个：内核 RAM 窗，以及 App 页窗（App 任务的帧落在
         * 它自己的 PLV3 页里 —— 内核对那页同样可读写）。 */
        if((((next_sp < LA_FRAME_MIN) || (next_sp >= LA_FRAME_MAX)) &&
            (LA_FRAME_IN_APP(next_sp) == 0)) ||
           ((next_sp & 0xFu) != 0u))
        {
            s_swp_bogus++;
            s_swp_bogus_next = next_sp;
            s_swp_bogus_cur  = (uint32)arkrtos_current_task_id;
#if (ARKRTOS_USE_PRIV == 1)
            s_next_pplv = self_pplv;       /* 没切走：恢复本任务的特权级 */
#endif
            return frame;
        }
        {
            la_frame_word *next_frame = (la_frame_word *)(unsigned long)next_sp;

            /* 诊断：记下目标帧的 ERA / SP 槽。这一帧马上要被 ertn 恢复，
             * 这两格是「恢复之后到底跳到哪、用谁的 sp」的直接来源。 */
            uint32 j = (uint32)((s_swp_pos - 1u) & 15u) * LA_SWP_FIELDS;
            s_swp_log[j + 6u] = (uint32)next_frame[LA_FR_ERA];
            s_swp_log[j + 7u] = (uint32)next_frame[LA_FR_SP];

            /* 真正的切换点：把目标任务的 PLV 写进它的帧，再 ertn 过去。
             * 不切换（next_sp == 0）时不动自己的帧：那时 s_next_pplv 是
             * 别处的残留值，套到自己头上会把自己的特权级改掉。 */
            LA_PRIV_APPLY(next_frame);
            return next_frame;
        }
    }

#if (ARKRTOS_USE_PRIV == 1)
    s_next_pplv = self_pplv;           /* next_sp == 0：原地继续本任务 */
#endif
    return frame;
}

/**
* @brief 致命异常时把出错上下文的寄存器打出来
* @details ERA/ESTAT/BADI 只说明「在哪条指令、什么类型的错」，要定位「为什么」
*          还得看出错那一刻的通用寄存器。帧指针由 eentry 传进 la_trap_c，
*          这里直接从帧里取，不必再读硬件寄存器（那几个早被本函数改掉了）。
*          本函数只在致命路径上调用，不影响正常路径的性能与时序。
* @param frame 出错上下文保存的帧
*/
static void la_fatal_dump(la_frame_word *frame)
{
    uint32 i;

    arkrtos_board_puts(" frame@");
    arkrtos_board_puthex((uint32)(unsigned long)frame);
    arkrtos_board_puts("\n");
    for(i = 0u; i < LA_FR_WORDS; i++)
    {
        if((i & 3u) == 0u)
        {
            arkrtos_board_puts("  [");
            arkrtos_board_puthex(i * 8u);
            arkrtos_board_puts("] ");
        }
        arkrtos_board_puthex((uint32)frame[i]);
        arkrtos_board_puts((i & 3u) == 3u ? "\n" : " ");
    }

    /* 故障帧归属判定：这一帧是不是某个任务的「初始帧槽」（栈顶 - 0x100）？
     * 只有 arkrtos_port_stack_init() 会造出 sp 槽 == 栈顶、ra 槽 == 0 的帧。
     * 命中这两个条件就说明「被 resume 的是一份初始帧 / 被当成初始帧用的
     * 旧内存」，而不是一次真正的现场保存。 */
    {
        int32 t;
        uint32 fa = (uint32)(unsigned long)frame;

        for(t = 0; t < arkrtos_task_count; t++)
        {
            uint32 bot = (uint32)(unsigned long)arkrtos_task_table[t].stack_bottom;
            uint32 top = arkrtos_task_table[t].stack_top;

            if((fa >= bot) && (fa < top))
            {
                arkrtos_board_puts(" frame owner=task ");
                arkrtos_board_putu((uint32)t + 1u);
                arkrtos_board_puts(" bottom=");
                arkrtos_board_puthex(bot);
                arkrtos_board_puts(" top=");
                arkrtos_board_puthex(top);
                arkrtos_board_puts(" is_init_slot=");
                arkrtos_board_puthex((fa == (top - 0x100u)) ? 1u : 0u);
                arkrtos_board_puts(" ra0=");
                arkrtos_board_puthex((frame[LA_FR_RA] == 0u) ? 1u : 0u);
                arkrtos_board_puts(" sp_is_top=");
                arkrtos_board_puthex((frame[LA_FR_SP] == top) ? 1u : 0u);
                arkrtos_board_puts("\n");
                break;
            }
        }
        if(t >= arkrtos_task_count)
        {
            arkrtos_board_puts(" frame owner=none (outside every task stack)\n");
        }
    }

    /* 最近若干次切换：next / 旧帧 / 返回的新帧 / 切换后的当前任务 id。
     * next_sp == 0xffffffff 表示调度器交回一个非法栈指针。 */
    arkrtos_board_puts(" enter=");
    arkrtos_board_puthex(s_swp_enter);
    arkrtos_board_puts(" exit=");
    arkrtos_board_puthex(s_swp_exit);
    arkrtos_board_puts(" pos=");
    arkrtos_board_puthex(s_swp_pos);
    arkrtos_board_puts("\n switch-log (next frame new_sp cur_id st rd target-era target-sp, newest last):\n");
    {
        uint32 k;

        uint32 total = (s_swp_pos < 16u) ? s_swp_pos : 16u;

        for(k = 0u; k < total; k++)
        {
            /* 从最旧的一条开始：写指针前方 total 条就是全部有效记录 */
            uint32 i = ((s_swp_pos - total + k) & 15u) * LA_SWP_FIELDS;

            arkrtos_board_puts("  ");
            arkrtos_board_puthex(s_swp_log[i + 0u]);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex(s_swp_log[i + 1u]);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex(s_swp_log[i + 2u]);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex(s_swp_log[i + 3u]);
            arkrtos_board_puts(" st=");
            arkrtos_board_puthex(s_swp_log[i + 4u]);
            arkrtos_board_puts(" rd=");
            arkrtos_board_puthex(s_swp_log[i + 5u]);
            arkrtos_board_puts(" era=");
            arkrtos_board_puthex(s_swp_log[i + 6u]);
            arkrtos_board_puts(" sp=");
            arkrtos_board_puthex(s_swp_log[i + 7u]);
            arkrtos_board_puts("\n");
        }
    }

    /* 内核就绪位图自愈现场：非 0 说明「位图有位、桶头为空」这类脱钩真的发生过 */
    arkrtos_board_puts(" heal=");
    arkrtos_board_puthex(arkrtos_ready_heal_cnt);
    arkrtos_board_puts(" heal_g=");
    arkrtos_board_puthex(arkrtos_ready_heal_g);
    arkrtos_board_puts(" heal_word=");
    arkrtos_board_puthex(arkrtos_ready_heal_word);
    arkrtos_board_puts(" heal_bit=");
    arkrtos_board_puthex(arkrtos_ready_heal_bit);
    arkrtos_board_puts(" heal_head=");
    arkrtos_board_puthex(arkrtos_ready_heal_head);
    arkrtos_board_puts(" heal_total=");
    arkrtos_board_puthex(arkrtos_ready_heal_total);
    /* 自杀死任务从 SWITCH_TASK 里返回的次数：非 0 说明有一条任务入口
     * 已经 return 到 ra=0（那正是本类致命异常的形状）。 */
    arkrtos_board_puts(" kill_ret=");
    arkrtos_board_puthex(arkrtos_kill_ret_cnt);
    /* 任务表快照：致命异常时真正需要的是「谁还在就绪集里、它的栈指针指向
     * 哪」。status 与 ready_next 分开打，是因为「INVALID 但仍挂在就绪链上」
     * 这种组合本身就是脱钩现场。只在致命路径执行，正常路径零成本。 */
    arkrtos_board_puts("\n task table (id: status prio/base ready_next entry stack_ptr):\n");
    {
        int32 t;

        for(t = 0; t < arkrtos_task_count; t++)
        {
            arkrtos_board_puts("  ");
            arkrtos_board_putu((uint32)t + 1u);
            arkrtos_board_puts(": ");
            arkrtos_board_puthex((uint32)arkrtos_task_table[t].status);
            arkrtos_board_puts(" ");
            arkrtos_board_putu((uint32)arkrtos_task_table[t].priority);
            arkrtos_board_puts("/");
            arkrtos_board_putu((uint32)arkrtos_task_table[t].base_priority);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex((uint32)arkrtos_task_table[t].ready_next);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex((uint32)(unsigned long)arkrtos_task_table[t].entry);
            arkrtos_board_puts(" ");
            arkrtos_board_puthex(arkrtos_task_table[t].stack_ptr);
            /* 栈底守卫字：不是 0xed01 就说明有东西冲到这块栈底以下（或它自己
             * 故意写坏了，比如 stack guard 演示任务）。相邻栈是 .bss 里紧挨着
             * 分配的，所以「谁的守卫坏了」直接指认越界者。 */
            arkrtos_board_puts(" guard=");
            arkrtos_board_puthex((uint32)(unsigned long)
                                 arkrtos_task_table[t].stack_bottom[0]);
            arkrtos_board_puts("\n");
        }
    }

    arkrtos_board_puts("\n bogus=");
    arkrtos_board_puthex(s_swp_bogus);
    arkrtos_board_puts(" bogus_next=");
    arkrtos_board_puthex(s_swp_bogus_next);
    arkrtos_board_puts(" bogus_cur=");
    arkrtos_board_puthex(s_swp_bogus_cur);
    arkrtos_board_puts("\n");
}

/**
* @brief App（PLV3）故障的统一收尾：记录 → 交内核恢复 → 不返回
* @details 与 RISC-V 的 arkrtos_port_fault_dispatch() 同一语义：
*          内核的 arkrtos_cpu_fault_handler() 负责计数、按崩溃策略
*          决定「重建栈帧重启」还是「禁用该 App」，并返回下一个要跑的
*          帧；本函数只负责把它恢复起来。
*          帧在这里已经是内核态产物（可能是重建后的新帧），不再回到
*          出错的那条指令。
*/
static void la_fault_recover(void)
{
    uint32 next = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_MEMFAULT);

    if(next != 0u)
    {
        la_frame_word *next_frame = (la_frame_word *)(unsigned long)next;

        LA_PRIV_APPLY(next_frame);
        /* 从这里开始不再算「在异常里」：resume_task 只做恢复 + ertn */
        s_in_trap = 0u;
        arkrtos_port_resume_task(next);
    }

    /* 连一个可恢复的任务都选不出来：明确停机，不假装恢复过。 */
    arkrtos_board_puts("[LA64U] no recoverable task -> halt\n");
    arkrtos_port_fatal_report(0xFEE1u);
    for(;;)
    {
        __asm__ volatile("idle 0" ::: "memory");
    }
}

/**
* @brief 从 eentry 抓到的 App 故障（PPI / IPE / 其它）
* @param frame eentry 压好的帧（在 App 自己的栈上）
* @param ecode ESTAT.Ecode
* @note 不返回。调用者（arkrtos_context.S 的 la_app_fault_switch）已经先把
*       sp 换到静态故障栈 la_fault_stack：本函数末尾会走
*       arkrtos_task_recover_pending() -> arkrtos_task_stack_init()，那一步
*       把**整个任务栈**重填一遍，若还跑在 App 栈上，自己的 C 帧就被抹掉
*       （RV32 移植踩过，见 c05f1a6）。帧本身（frame 参数）仍在 App 栈上，
*       所以下面要先把要用的字段取成局部量。
*/
void la_app_fault_c(la_frame_word *frame, uint32 ecode)
{
    unsigned long badv = 0ul;
    /* 先把要打的东西取成局部量：后面 arkrtos_cpu_fault_handler() 会重建
     * 本任务的整个栈（含这个帧所在的位置），事后再读 frame[...] 就是读垃圾。 */
    uint32 era_val  = (uint32)frame[LA_FR_ERA];
    uint32 pplv_val = (uint32)(frame[LA_FR_PRMD] & 3UL);

    LA_CSRR(LA_CSR_BADV, badv);

    if(ecode == LA_ESTAT_ECODE_PPI)
    {
        la_cnt_ppi++;
    }
    else if(ecode == LA_ESTAT_ECODE_IPE)
    {
        la_cnt_ipe++;
    }
    else
    {
        la_cnt_other++;
    }

    arkrtos_board_puts("[LA64U] app fault ecode=");
    arkrtos_board_putu(ecode);
    arkrtos_board_puts("(");
    arkrtos_board_puts(la_ecode_name(ecode));
    arkrtos_board_puts(") badv=");
    arkrtos_board_puthex((uint32)badv);
    arkrtos_board_puts(" era=");
    arkrtos_board_puthex(era_val);
    arkrtos_board_puts(" plv=");
    arkrtos_board_putu(pplv_val);
    arkrtos_board_puts("\n");

    la_fault_recover();
}

/**
* @brief TLB 重填的处理（由 arkrtos_context.S 的 tlb_refill_entry 调入）
* @details 硬件进这条入口时：不保存任何通用寄存器、CRMD.DA=1、PG=0、
*          PLV=0、IE=0，出错的信息只在 TLBRBADV / TLBRERA / TLBRPRMD 里。
*          所以入口第一件事是换到静态故障栈（被打断的 sp 本身就可能是坏的）。
*
*          两种来源必须分开：
*            - TLBRPRMD.PPLV != 0：App 摸了未映射地址 ⇒ 正常保护，记一条并恢复；
*            - PPLV == 0：内核自己摸了未映射地址 ⇒ 移植 bug，直接停机，
*              绝不能拿「重启任务」把它掩掉（那会让 bug 变成随机重启）。
*
*          **恢复时必须写 TLBRERA/TLBRPRMD**：ertn 看到 TLBRERA.ISTLBR=1 就
*          走重填的返回分支，只认这对镜像 CSR，并把 CRMD.DA 清 0、PG 置 1。
*          写错了的症状是「ertn 回到出错的那条指令」——无限重填。
*/
void la_refill_c(void)
{
    unsigned long tlbrera = 0ul;
    unsigned long tlbrprmd = 0ul;
    unsigned long tlbrbadv = 0ul;

    LA_CSRR(LA_CSR_TLBRERA, tlbrera);
    LA_CSRR(LA_CSR_TLBRPRMD, tlbrprmd);
    LA_CSRR(LA_CSR_TLBRBADV, tlbrbadv);

    if((tlbrprmd & 3UL) == 0UL)
    {
        arkrtos_board_puts("[LA64U] KERNEL refill (plv0) badv=");
        arkrtos_board_puthex((uint32)tlbrbadv);
        arkrtos_board_puts(" era=");
        arkrtos_board_puthex((uint32)(tlbrera & ~3UL));
        arkrtos_board_puts(" -> fatal\n");
        arkrtos_port_fatal_report((uint32)tlbrbadv);
        for(;;)
        {
            __asm__ volatile("idle 0" ::: "memory");
        }
    }

    la_cnt_refill++;
    arkrtos_board_puts("[LA64U] tlb refill badv=");
    arkrtos_board_puthex((uint32)tlbrbadv);
    arkrtos_board_puts(" era=");
    arkrtos_board_puthex((uint32)(tlbrera & ~3UL));
    arkrtos_board_puts(" plv=");
    arkrtos_board_putu((uint32)(tlbrprmd & 3UL));
    arkrtos_board_puts("\n");

    /* 重填也是「在异常里」：开着这个标志，恢复路径里的临界区退出不会真的
     * 把 CRMD.IE 打开（否则节拍会嵌进故障处理里）。 */
    s_in_trap = 1u;
    s_switch_pending = 0u;

    {
        uint32 next = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_MEMFAULT);

        if(next != 0u)
        {
            la_frame_word *next_frame = (la_frame_word *)(unsigned long)next;

            LA_PRIV_APPLY(next_frame);

            LA_CSRW(LA_CSR_TLBRERA, ((unsigned long)next_frame[LA_FR_ERA] & ~3UL) | 1UL);
            LA_CSRW(LA_CSR_TLBRPRMD, (unsigned long)next_frame[LA_FR_PRMD]);

            s_in_trap = 0u;
            arkrtos_port_resume_task(next);
        }
    }

    arkrtos_board_puts("[LA64U] refill: no recoverable task -> halt\n");
    arkrtos_port_fatal_report(0xFEE2u);
    for(;;)
    {
        __asm__ volatile("idle 0" ::: "memory");
    }
}

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/**
* @brief refill 入口在按需档下的 C 入口（由 arkrtos_context.S 调入）
* @return 1 = 条目已装好，汇编侧恢复被打断的全部寄存器后 ertn 回原指令；
*         0 = 不是这一档能处理的（调用方落回 la_refill_c，那条不返回）
* @details 与 la_refill_c() 的关键差别：**它要回到出错的那条指令**。
*          la_refill_c() 的出口只有「换到另一个任务」和「停机」两种，
*          所以它不需要、也从来没有保存过通用寄存器；按需缺页必须回到原
*          指令重试，因此保存/恢复放在汇编入口里（见 arkrtos_context.S
*          的 tlb_refill_entry），这里只负责装条目。
*
*          TLBRERA / TLBRPRMD 一个字都不碰：ertn 会按 TLBRERA.ISTLBR=1
*          回到 era 并把 CRMD.DA/PG 恢复成分页态。
*/
int32 la_refill_demand_c(void)
{
    unsigned long tlbrbadv = 0ul;

    LA_CSRR(LA_CSR_TLBRBADV, tlbrbadv);

    if(la_pg_demand_fault((uint32)tlbrbadv) != 0)
    {
        return 1;
    }

    la_refill_c();                  /* 不返回：原来的报错 / 杀任务路径 */
    return 0;
}
#endif /* ARKRTOS_USE_PAGING_DEMAND == 1 */

/**
* @brief 异常码的短名（只用于打印，不参与判定）
*/
static const char *la_ecode_name(uint32 ecode)
{
    switch(ecode)
    {
        case 1u:  return "PIL:page-invalid-load";
        case 2u:  return "PIS:page-invalid-store";
        case 3u:  return "PIF:page-invalid-fetch";
        case 7u:  return "PPI:privilege";
        case 8u:  return "ADEM:misalign";
        case 9u:  return "ALE:align";
        case 12u: return "BRK:breakpoint";
        case 14u: return "IPE:privileged-insn";
        default:  return "other";
    }
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

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 本 port 的 trap 帧直接压在被中断任务的栈上：帧写下去之后立刻查一次
     * 栈底保护字，把「帧把栈吃穿」变成一条带任务号的记录，而不是稍后
     * 某个随机指针崩掉。这里只记录，不改调度状态（还在异常里）。 */
    arkrtos_stack_guard_check_current();
    #endif

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
        #if (ARKRTOS_USE_TASK_SWAP == 1)
        /* 热切换：端口层把改写好的新帧记在 s_la_swap_frame 里。恢复帧是
         * 本函数的返回值，所以这里必须换掉，否则返回的还是旧块里那一帧。 */
        if(s_la_swap_frame != 0)
        {
            frame = s_la_swap_frame;
            s_la_swap_frame = 0;
        }
        #endif
    }
    else
    {
        #if (ARKRTOS_USE_PRIV == 1)
        /* 从 PLV3 进来的非 syscall 异常 = App 越权或非法指令。
         * 走故障恢复（重启该 App）而不是停机：一个 App 的 bug 打死整个
         * 系统，就不是「隔离」了。PLV0 自己的异常仍然按致命处理。 */
        if((frame[LA_FR_PRMD] & 3UL) != 0UL)
        {
            /* 走换栈入口：故障处理会把整个任务栈重填，处理函数不能跑在
             * 那个栈上（否则返回地址变成 0xcdcdcdcd）。不返回。 */
            la_app_fault_switch(frame, ecode);
        }
        #endif

        arkrtos_port_fatal_report(estat);
        la_fatal_dump(frame);
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
                /* 交换到别人（可能是 PLV3 的 App）时，把它的特权级写进帧 */
                LA_PRIV_APPLY(frame);
            }
        }
    }

    s_in_trap--;

    return frame;
}

#if (ARKRTOS_USE_SMP == 1) && (ARKRTOS_USE_SMP_SCHED == 1)
/**
* @brief 清本核的 IPI 挂起位（IOCSR 0x1000 状态 → 0x100C 清除）
* @note 这段序列原来是 arkrtos_context.S 里 la_smp_trap_entry 的内联拷贝，
*       打开调度分核后汇编不再自己清，两处只剩这一份。
*/
static void la_smp_ipi_clear(void)
{
    uint32 st = 0u;

    __asm__ volatile("iocsrrd.w %0, %1" : "=r"(st) : "r"(LA_IOCSR_IPI_STATUS));
    la_iocsr_write_w(LA_IOCSR_IPI_CLEAR, st);
}

/**
* @brief 次核异常的统一 C 入口（只在调度分核打开时存在）
* @param frame 次核入口在被打断上下文栈上压好的 256 字节帧
* @param cpu   本核号（汇编按 CSR.TID 传入）
* @return 要 ertn 回去的帧指针（定时器路径可能换成另一个任务的帧）
* @details 三条路径，全都只用"本核"状态：
*          - 定时器中断：清 TICLR → 本核计一拍 → 调度器决定下一个帧；
*          - IPI：清挂起 → 默认处理函数（计数，应用可覆盖强符号）；
*          - 其余一律致命：次核不许发 SVC（对象表/文件系统/同步对象都没有
*            跨核保护），也不做 App 故障恢复（那套页表与窗口逻辑属于 CPU0）。
*          绝不把次核的 SYS 静默放行——放过去就是数据竞争，报错停机是唯一
*          诚实的处理。
*/
la_frame_word *la_smp_trap_c(la_frame_word *frame, uint32 cpu)
{
    uint32 estat = la_estat_read();
    uint32 ecode = (estat >> 16) & 0x3Fu;

    if(ecode == LA_ESTAT_ECODE_INT)
    {
        if((estat & LA_ESTAT_IS_TIMER) != 0u)
        {
            la_ticlr_clear();
            arkrtos_smp_sched_tick(cpu);
            return (la_frame_word *)(unsigned long)
                   arkrtos_smp_sched_switch(cpu, (uint32)(unsigned long)frame);
        }

        la_smp_ipi_clear();
        arkrtos_smp_ipi_handler();
        return frame;
    }

    if(ecode == LA_ESTAT_ECODE_SYS)
    {
        /* 次核任务的 SYS = 配置错误（次核不许调内核 API）。 */
        arkrtos_port_fatal_report(estat);
        la_fatal_dump(frame);
        for(;;)
        {
            __asm__ volatile("idle 0" ::: "memory");
        }
    }

    arkrtos_port_fatal_report(estat);
    la_fatal_dump(frame);
    for(;;)
    {
        __asm__ volatile("idle 0" ::: "memory");
    }
}
#endif /* ARKRTOS_USE_SMP && ARKRTOS_USE_SMP_SCHED */

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

    #if (ARKRTOS_USE_PRIV == 1)
    /* 分页在开中断之前打开：节拍一进来就要经 TLB 取指/访存。
     * 此时任务已经注册完（帧写在各自的栈上），映射与帧内容无关。 */
    la_mmu_init();
    #endif

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
