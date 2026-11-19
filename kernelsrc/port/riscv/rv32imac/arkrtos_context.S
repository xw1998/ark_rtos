/**
* @file arkrtos_context.S
* @brief Ark RTOS RISC-V RV32 异常入口与上下文切换（GNU as 语法）
* @details 与 Cortex-M 的 arkrtos_context.S 承担同样的三件事，但因为 RISC-V
*          没有硬件自动压栈、也没有独立的 SVC/PendSV 异常，实现方式不同：
*
*          1) **唯一异常入口** trap_entry：ecall（系统调用）、机器定时器中断、
*             机器软件中断（用来触发任务切换）全部走这里，由 riscv_trap_c()
*             按 mcause 分派。RISC-V 不区分「异常」与「中断」的入口。
*          2) **arkrtos_port_resume_task**：故障恢复路径专用，在异常处理程序
*             内部直接把 CPU 切到另一个已保存的上下文并 mret。
*          3) **arkrtos_port_enter_idle**：进入空闲上下文。Cortex-M 只改 PSP
*             就返回，RISC-V 必须真的把 sp 换掉，因此这里必须是汇编且不建栈帧。
*
*          栈帧布局（144 字节，16 字节对齐，与 arkrtos_port_stack_init() 一致）：
*
*              偏移  内容                偏移  内容
*              0     ra (x1)             68    s2  (x18)
*              4     sp (x2) 原值       72    s3
*              8     gp (x3)             76    s4
*              12    tp (x4)             80    s5
*              16    t0 (x5)             84    s6
*              20    t1 (x6)             88    s7
*              24    t2 (x7)             92    s8
*              28    s0 (x8)             96    s9
*              32    s1 (x9)            100    s10
*              36    a0 (x10)           104    s11
*              40    a1                  108    t3 (x28)
*              44    a2                  112    t4
*              48    a3                  116    t5
*              52    a4                  120    t6
*              56    a5                  124    mepc
*              60    a6                  128    mstatus
*              64    a7                  132..140 填充（对齐到 144）
*
*          特权级（U 模式）由帧内 mstatus.MPP 承载，但**恢复时以全局
*          g_rv_next_mpp 为准**：一次不切换的 trap 要保持来源特权，切换时由
*          arkrtos_port_set_thread_priv() 改成新任务的值。所以两条恢复路径
*          （trap_entry 与 arkrtos_port_resume_task）都必须做这次覆盖，
*          否则「切到 App 任务」会跑成 M 模式、「App 的 tick 中断」会把任务
*          抬回 M 模式 —— 两种都不会报错，只会静默地没有隔离。
*
*          故障路径额外用一张**独立栈**（rv_fault_stack）：内核恢复故障任务时
*          会重建它的栈，处理程序不能还坐在那段区间里（详见 rv_fault_enter）。
*
*          tp（x4）在帧内被当作帧指针使用：它是 callee-saved，跨 C 调用不会被
*          改掉，所以「保存现场 → 调 C → 从返回的帧指针恢复」这条链不需要
*          额外的全局变量。返回给 trap_entry 的帧指针可能已经换成了另一个任务
*          的帧（发生任务切换时），恢复动作照它执行即可。
*
* @author xw
*/

/* ============================================================
 * 唯一异常入口
 * ============================================================ */
    .section .text.trap_entry, "ax"
    .globl  trap_entry
    .align  2
trap_entry:
    addi    sp, sp, -144
    sw      ra,   0(sp)
    sw      t0,  16(sp)             /* t0 先存：下面要拿它算原 sp */
    sw      gp,   8(sp)
    sw      tp,  12(sp)
    addi    t0, sp, 144
    sw      t0,   4(sp)             /* 帧里的 sp 槽 = 陷阱发生时的原始 sp */
    mv      tp, sp                  /* tp = 帧指针（callee-saved，跨 C 调用有效） */
    sw      t1,  20(tp)
    sw      t2,  24(tp)
    sw      s0,  28(tp)
    sw      s1,  32(tp)
    sw      a0,  36(tp)
    sw      a1,  40(tp)
    sw      a2,  44(tp)
    sw      a3,  48(tp)
    sw      a4,  52(tp)
    sw      a5,  56(tp)
    sw      a6,  60(tp)
    sw      a7,  64(tp)
    sw      s2,  68(tp)
    sw      s3,  72(tp)
    sw      s4,  76(tp)
    sw      s5,  80(tp)
    sw      s6,  84(tp)
    sw      s7,  88(tp)
    sw      s8,  92(tp)
    sw      s9,  96(tp)
    sw      s10, 100(tp)
    sw      s11, 104(tp)
    sw      t3, 108(tp)
    sw      t4, 112(tp)
    sw      t5, 116(tp)
    sw      t6, 120(tp)
    csrr    t0, mepc
    sw      t0, 124(tp)
    csrr    t0, mstatus
    sw      t0, 128(tp)

    mv      a0, tp
    call    riscv_trap_c            /* 返回要恢复的帧（可能已换成别的任务） */
    mv      tp, a0

    /* 恢复：mepc/mstatus 与通用寄存器都从这里开始，sp/tp/t0 最后取，
     * 因为在这一步之前 tp 还是帧指针。 */
    lw      t0, 124(tp)
    csrw    mepc, t0
    /* mstatus 的 MPP 不由帧决定，而由 g_rv_next_mpp 决定：
     * 帧里那份 MPP 是「进 trap 时的来源特权」，切换后它就过期了。
     * 其余位（MIE 的旧值、MPIE、MPP 之外的一切）取帧里保存的值。 */
    lw      t0, 128(tp)
    la      t1, g_rv_next_mpp
    lw      t1, 0(t1)
    li      t2, 0xFFFFE7FF          /* 清 bit12:11（MPP） */
    and     t0, t0, t2
    slli    t1, t1, 11
    or      t0, t0, t1
    csrw    mstatus, t0
    lw      ra,   0(tp)
    lw      gp,   8(tp)
    lw      t1,  20(tp)
    lw      t2,  24(tp)
    lw      s0,  28(tp)
    lw      s1,  32(tp)
    lw      a0,  36(tp)
    lw      a1,  40(tp)
    lw      a2,  44(tp)
    lw      a3,  48(tp)
    lw      a4,  52(tp)
    lw      a5,  56(tp)
    lw      a6,  60(tp)
    lw      a7,  64(tp)
    lw      s2,  68(tp)
    lw      s3,  72(tp)
    lw      s4,  76(tp)
    lw      s5,  80(tp)
    lw      s6,  84(tp)
    lw      s7,  88(tp)
    lw      s8,  92(tp)
    lw      s9,  96(tp)
    lw      s10, 100(tp)
    lw      s11, 104(tp)
    lw      t3, 108(tp)
    lw      t4, 112(tp)
    lw      t5, 116(tp)
    lw      t6, 120(tp)
    lw      sp,   4(tp)             /* 先换栈：此刻 tp 仍指向帧，后面的取数还成立 */
    lw      t0,  16(tp)
    lw      tp,  12(tp)
    mret

/* ============================================================
 * 故障恢复：在异常处理程序内部直接切到另一个上下文
 * ============================================================ */
    .section .text.arkrtos_port_resume_task, "ax"
    .globl  arkrtos_port_resume_task
    .align  2
arkrtos_port_resume_task:
    mv      tp, a0                  /* a0 = 目标任务的帧指针 */
    lw      t0, 124(tp)
    csrw    mepc, t0
    /* mstatus 的 MPP 不由帧决定，而由 g_rv_next_mpp 决定：
     * 帧里那份 MPP 是「进 trap 时的来源特权」，切换后它就过期了。
     * 其余位（MIE 的旧值、MPIE、MPP 之外的一切）取帧里保存的值。 */
    lw      t0, 128(tp)
    la      t1, g_rv_next_mpp
    lw      t1, 0(t1)
    li      t2, 0xFFFFE7FF          /* 清 bit12:11（MPP） */
    and     t0, t0, t2
    slli    t1, t1, 11
    or      t0, t0, t1
    csrw    mstatus, t0
    lw      ra,   0(tp)
    lw      gp,   8(tp)
    lw      t1,  20(tp)
    lw      t2,  24(tp)
    lw      s0,  28(tp)
    lw      s1,  32(tp)
    lw      a0,  36(tp)
    lw      a1,  40(tp)
    lw      a2,  44(tp)
    lw      a3,  48(tp)
    lw      a4,  52(tp)
    lw      a5,  56(tp)
    lw      a6,  60(tp)
    lw      a7,  64(tp)
    lw      s2,  68(tp)
    lw      s3,  72(tp)
    lw      s4,  76(tp)
    lw      s5,  80(tp)
    lw      s6,  84(tp)
    lw      s7,  88(tp)
    lw      s8,  92(tp)
    lw      s9,  96(tp)
    lw      s10, 100(tp)
    lw      s11, 104(tp)
    lw      t3, 108(tp)
    lw      t4, 112(tp)
    lw      t5, 116(tp)
    lw      t6, 120(tp)
    lw      sp,   4(tp)
    lw      t0,  16(tp)
    lw      tp,  12(tp)
    mret

/* ============================================================
 * 故障路径：切到独立故障栈后交给 C
 * @param a0 mcause，a1 from_priv（U/M）
 * @details 本移植没有独立 handler 栈（普通 trap 就是在被中断任务的栈上
 *          建帧），但**故障路径不能这样**：内核恢复故障任务时会重建那段
 *          栈（先整段填 0xcdcdcdcd），正在用的活动栈帧会被一起填掉。
 *          所以进入 C 之前先把 sp 搬到 rv_fault_stack 的栈顶。
 *          栈顶地址由 C 侧的 rv_fault_stack_top_addr 给，两边不会漂移。
 *          不返回：目标函数要么 resume_task 要么停机。
 * ============================================================ */
    .section .text.rv_fault_enter, "ax"
    .globl  rv_fault_enter
    .align  2
rv_fault_enter:
    la      t0, rv_fault_stack_top_addr
    lw      sp, 0(t0)
    andi    sp, sp, -16             /* 栈顶 16 字节对齐（ABI 硬要求） */
    tail    arkrtos_port_fault_dispatch

/* ============================================================
 * 进入空闲上下文
 * @details 只把 sp 换成内核给的栈顶并返回。这里不能写成 C 函数：C 版本会在
 *          新栈上做 epilogue（从新 sp 处恢复 ra/s0），等于从一块未初始化的
 *          内存里弹栈。Cortex-M 版只改 PSP 所以可以留在 C 里，RISC-V 不行。
 * @param a0 空闲任务栈顶原始地址（尚未构造栈帧）
 * ============================================================ */
    .section .text.arkrtos_port_enter_idle, "ax"
    .globl  arkrtos_port_enter_idle
    .align  2
arkrtos_port_enter_idle:
    mv      sp, a0
    ret

#if (ARKRTOS_USE_SMP == 1)
/* ============================================================
 * 次核异常入口（SMP 档）
 * @details 次核**不走** trap_entry：那条路会进 riscv_trap_c，而
 *          s_in_trap / g_rv_next_mpp / 内核分派全是主核一份的全局状态。
 *          这里只做三件事：存下被中断处的 caller-saved 寄存器、
 *          按 mcause 调 rv_smp_trap_c（清 msip + 计数，或致命停机）、
 *          恢复后 mret。
 *          次核目前只跑 idle 循环，所以帧里不必保存 callee-saved：
 *          被中断的只有 arkrtos_smp_idle_loop 的循环变量，而那是
 *          caller-saved 之外的东西……为稳妥起见把 s0/s1 也存了。
 *          48 字节帧，16 字节对齐（ABI 要求）。
 * ============================================================ */
    .section .text.rv_smp_trap_entry, "ax"
    .globl  rv_smp_trap_entry
    .align  2
rv_smp_trap_entry:
    addi    sp, sp, -48
    sw      ra,   0(sp)
    sw      t0,   4(sp)
    sw      t1,   8(sp)
    sw      t2,  12(sp)
    sw      a0,  16(sp)
    sw      a1,  20(sp)
    sw      a2,  24(sp)
    sw      a3,  28(sp)
    sw      a4,  32(sp)
    sw      a5,  36(sp)
    sw      s0,  40(sp)
    sw      s1,  44(sp)

    csrr    a0, mcause
    call    rv_smp_trap_c            /* 清 msip 并计数，或致命停机（不返回） */

    lw      ra,   0(sp)
    lw      t0,   4(sp)
    lw      t1,   8(sp)
    lw      t2,  12(sp)
    lw      a0,  16(sp)
    lw      a1,  20(sp)
    lw      a2,  24(sp)
    lw      a3,  28(sp)
    lw      a4,  32(sp)
    lw      a5,  36(sp)
    lw      s0,  40(sp)
    lw      s1,  44(sp)
    addi    sp, sp, 48
    mret
#endif /* ARKRTOS_USE_SMP == 1 */
