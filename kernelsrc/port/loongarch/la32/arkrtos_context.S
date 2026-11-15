/**
* @file arkrtos_context.S
* @brief Ark RTOS LoongArch32 异常入口与上下文切换（GNU as 语法）
* @details 与 la64 版承担同样的三件事，差异全部来自「寄存器 32 位」这一条：
*          帧槽从 8 字节变 4 字节、帧大小 256 → 128 字节、保存/恢复指令从
*          `st.d/ld.d/addi.d` 换成 `st.w/ld.w/addi.w`。
*
*          **注意**：本仓库的 la32 工具链（GCC 8.3.0 / binutils 2.37）在
*          `-march=loongarch32` 下**不拒绝** `.d` 形式的指令——`st.d`、`ld.d`、
*          `addi.d` 都能汇编通过。因此「汇编通过」不能证明代码是 32 位合法的，
*          本文件一律只用 `.w` 形式，改这里时也必须守住这一点（LA32 硬件上
*          `.d` 指令的行为与 LA64 不同，跑起来才知道错）。
*
*          1) **没有可用的软件中断**：CSR.IPI 只发给其他核，单核自触发无效
*             （la64 上实测过，同一套硬件的语义）。所以没有 PendSV 的对应物，
*             任务上下文里的切换**就地完成**：在调用者栈上构造一个与 trap 帧
*             同格式的帧，调 C 辅助函数决定切给谁，再按返回帧恢复并 ertn。
*             中断上下文里的切换请求由 la_trap_c() 在异常出口处结算。
*             两条路径共用同一套保存/恢复代码，帧格式只有一种。
*
*          2) **异常入口只有 EENTRY 一个**：Ecode 全在 ESTAT 里，由
*             la_trap_c() 分派。
*
*          栈帧布局（128 字节，16 字节对齐，与 arkrtos_port_stack_init() 一致）：
*
*              偏移  内容                  偏移  内容
*              0     ra  (r1)              72    t7  (r19)
*              4     tp  (r2)              76    t8  (r20)
*              8     sp  (r3) 原值         80    fp  (r22)
*              12    a0  (r4)              84    s0  (r23)
*              16    a1  (r5)              88    s1  (r24)
*              20    a2  (r6)              92    s2  (r25)
*              24    a3  (r7)              96    s3  (r26)
*              28    a4  (r8)              100   s4  (r27)
*              32    a5  (r9)              104   s5  (r28)
*              36    a6  (r10)             108   s6  (r29)
*              40    a7  (r11)             112   s7  (r30)
*              44    t0  (r12)             116   s8  (r31)
*              48    t1  (r13)             120   ERA
*              52    t2  (r14)             124   PRMD
*              56    t3  (r15)
*              60    t4  (r16)
*              64    t5  (r17)
*              68    t6  (r18)
*
*          r21 是 psABI 保留寄存器（TLS 用），编译器不分配，因此不进帧。
*
*          **s0 在帧内被当作帧指针**：它是 callee-saved，跨 C 调用不会被改掉，
*          所以「保存现场 → 调 C → 从返回的帧指针恢复」这条链不需要额外的
*          全局变量。C 返回的帧指针可能已经换成另一个任务的帧，恢复动作照它
*          执行即可。
*
* @note si12 偏移上限 +2047，本文件最大偏移 124，安全。
* @author xw
*/

/* ============================================================
 * 唯一异常入口
 * @details 硬件在进异常时已经把返回地址写进 ERA、把旧状态写进 PRMD，
 *          并清掉 CRMD.IE、把 PLV 置 0；这里只需要把通用寄存器搬进帧。
 *          sp 槽存「异常发生时的原始 sp」，恢复时先换栈、后取其余寄存器。
 * ============================================================ */

    .section .text.eentry, "ax"
    .globl  eentry
    .align  2
eentry:
    addi.w  $sp, $sp, -128
    st.w    $ra, $sp, 0
    st.w    $tp, $sp, 4
    addi.w  $t0, $sp, 128
    st.w    $t0, $sp, 8             /* 帧里的 sp 槽 = 异常发生时的原始 sp */
    st.w    $s0, $sp, 84            /* s0 即将被借用作帧指针，先存原值 */
    move    $s0, $sp                /* s0 = 帧指针 */

    st.w    $a0, $s0, 12
    st.w    $a1, $s0, 16
    st.w    $a2, $s0, 20
    st.w    $a3, $s0, 24
    st.w    $a4, $s0, 28
    st.w    $a5, $s0, 32
    st.w    $a6, $s0, 36
    st.w    $a7, $s0, 40
    st.w    $t0, $s0, 44
    st.w    $t1, $s0, 48
    st.w    $t2, $s0, 52
    st.w    $t3, $s0, 56
    st.w    $t4, $s0, 60
    st.w    $t5, $s0, 64
    st.w    $t6, $s0, 68
    st.w    $t7, $s0, 72
    st.w    $t8, $s0, 76
    st.w    $fp, $s0, 80
    st.w    $s1, $s0, 88
    st.w    $s2, $s0, 92
    st.w    $s3, $s0, 96
    st.w    $s4, $s0, 100
    st.w    $s5, $s0, 104
    st.w    $s6, $s0, 108
    st.w    $s7, $s0, 112
    st.w    $s8, $s0, 116

    csrrd   $t0, 0x6                /* ERA */
    st.w    $t0, $s0, 120
    csrrd   $t0, 0x1                /* PRMD */
    st.w    $t0, $s0, 124

    move    $a0, $s0
    bl      la_trap_c               /* 返回要恢复的帧（可能已换成别的任务） */
    move    $s0, $a0

    /* ERA / PRMD 与通用寄存器都从这里恢复；sp / tp / s0 最后取，
     * 因为在这一步之前 s0 还是帧指针。 */
    ld.w    $t0, $s0, 120
    csrwr   $t0, 0x6
    ld.w    $t0, $s0, 124
    csrwr   $t0, 0x1

    ld.w    $ra, $s0, 0
    ld.w    $a0, $s0, 12
    ld.w    $a1, $s0, 16
    ld.w    $a2, $s0, 20
    ld.w    $a3, $s0, 24
    ld.w    $a4, $s0, 28
    ld.w    $a5, $s0, 32
    ld.w    $a6, $s0, 36
    ld.w    $a7, $s0, 40
    ld.w    $t1, $s0, 48
    ld.w    $t2, $s0, 52
    ld.w    $t3, $s0, 56
    ld.w    $t4, $s0, 60
    ld.w    $t5, $s0, 64
    ld.w    $t6, $s0, 68
    ld.w    $t7, $s0, 72
    ld.w    $t8, $s0, 76
    ld.w    $fp, $s0, 80
    ld.w    $s1, $s0, 88
    ld.w    $s2, $s0, 92
    ld.w    $s3, $s0, 96
    ld.w    $s4, $s0, 100
    ld.w    $s5, $s0, 104
    ld.w    $s6, $s0, 108
    ld.w    $s7, $s0, 112
    ld.w    $s8, $s0, 116
    ld.w    $t0, $s0, 44           /* t0 在 csrwr 之后才恢复 */
    ld.w    $sp, $s0, 8            /* 先换栈：此刻 s0 仍指向帧，后面的取数还成立 */
    ld.w    $tp, $s0, 4
    ld.w    $s0, $s0, 84
    ertn

/* ============================================================
 * 任务上下文里的「立即切换」
 * @details 由 arkrtos_port_switch_task()（C 侧）调用，只在任务上下文里进入。
 *          构造一个与 trap 帧同格式的帧（ERA = 返回地址、PRMD 按当前 CRMD
 *          的 IE/PLV 折算），交给 C 辅助函数决定切给谁，再按返回帧恢复并
 *          ertn —— 于是「从这里返回」这件事对调用者完全透明：没切换时回到
 *          下一条指令，切换时直接回到另一个任务的断点。
 *
*          为什么可以就地切换而不是挂起：所有 ARKRTOS_SWITCH_TASK() 调用点
*          都在 ARKRTOS_ENABLE_IRQ() 之后、不在临界区里（逐处核查过），
*          且单核下此刻没有别的上下文会碰调度表。
 * ============================================================ */
    .section .text.arkrtos_port_switch_task_inline, "ax"
    .globl  arkrtos_port_switch_task_inline
    .align  2
arkrtos_port_switch_task_inline:
    addi.w  $sp, $sp, -128
    st.w    $ra, $sp, 0
    st.w    $tp, $sp, 4
    addi.w  $t0, $sp, 128
    st.w    $t0, $sp, 8
    st.w    $s0, $sp, 84
    move    $s0, $sp

    st.w    $a0, $s0, 12
    st.w    $a1, $s0, 16
    st.w    $a2, $s0, 20
    st.w    $a3, $s0, 24
    st.w    $a4, $s0, 28
    st.w    $a5, $s0, 32
    st.w    $a6, $s0, 36
    st.w    $a7, $s0, 40
    st.w    $t0, $s0, 44
    st.w    $t1, $s0, 48
    st.w    $t2, $s0, 52
    st.w    $t3, $s0, 56
    st.w    $t4, $s0, 60
    st.w    $t5, $s0, 64
    st.w    $t6, $s0, 68
    st.w    $t7, $s0, 72
    st.w    $t8, $s0, 76
    st.w    $fp, $s0, 80
    st.w    $s1, $s0, 88
    st.w    $s2, $s0, 92
    st.w    $s3, $s0, 96
    st.w    $s4, $s0, 100
    st.w    $s5, $s0, 104
    st.w    $s6, $s0, 108
    st.w    $s7, $s0, 112
    st.w    $s8, $s0, 116

    st.w    $ra, $s0, 120           /* ERA = 本函数的返回地址 */
    /* PRMD 与 CRMD 的低 4 位**位序相同**：PPLV[1:0]、PIE[2]、PWE[3]。
     * 所以「切回去」的 PRMD 就是 CRMD & 0x7（bit3 是 CRMD 的 DA，
     * 不能带进 PRMD 的 PWE）——这样切回去不会凭空打开或关掉中断。
     * la64 上早先按「PIE 在 bit0、PPLV 在 [3:2]」折算，结果每个任务都被
     * ertn 到 PLV1 且关中断，`idle 0` 再也等不到中断（见 docs）。 */
    csrrd   $t0, 0x0
    andi    $t0, $t0, 7
    st.w    $t0, $s0, 124

    move    $a0, $s0
    bl      la_switch_inline
    move    $s0, $a0

    ld.w    $t0, $s0, 120
    csrwr   $t0, 0x6
    ld.w    $t0, $s0, 124
    csrwr   $t0, 0x1

    ld.w    $ra, $s0, 0
    ld.w    $a0, $s0, 12
    ld.w    $a1, $s0, 16
    ld.w    $a2, $s0, 20
    ld.w    $a3, $s0, 24
    ld.w    $a4, $s0, 28
    ld.w    $a5, $s0, 32
    ld.w    $a6, $s0, 36
    ld.w    $a7, $s0, 40
    ld.w    $t1, $s0, 48
    ld.w    $t2, $s0, 52
    ld.w    $t3, $s0, 56
    ld.w    $t4, $s0, 60
    ld.w    $t5, $s0, 64
    ld.w    $t6, $s0, 68
    ld.w    $t7, $s0, 72
    ld.w    $t8, $s0, 76
    ld.w    $fp, $s0, 80
    ld.w    $s1, $s0, 88
    ld.w    $s2, $s0, 92
    ld.w    $s3, $s0, 96
    ld.w    $s4, $s0, 100
    ld.w    $s5, $s0, 104
    ld.w    $s6, $s0, 108
    ld.w    $s7, $s0, 112
    ld.w    $s8, $s0, 116
    ld.w    $t0, $s0, 44
    ld.w    $sp, $s0, 8
    ld.w    $tp, $s0, 4
    ld.w    $s0, $s0, 84
    ertn

/* ============================================================
 * 故障恢复：在异常处理程序内部直接切到另一个上下文
 * @param a0 目标任务的帧指针
 * ============================================================ */
    .section .text.arkrtos_port_resume_task, "ax"
    .globl  arkrtos_port_resume_task
    .align  2
arkrtos_port_resume_task:
    move    $s0, $a0
    ld.w    $t0, $s0, 120
    csrwr   $t0, 0x6
    ld.w    $t0, $s0, 124
    csrwr   $t0, 0x1

    ld.w    $ra, $s0, 0
    ld.w    $a0, $s0, 12
    ld.w    $a1, $s0, 16
    ld.w    $a2, $s0, 20
    ld.w    $a3, $s0, 24
    ld.w    $a4, $s0, 28
    ld.w    $a5, $s0, 32
    ld.w    $a6, $s0, 36
    ld.w    $a7, $s0, 40
    ld.w    $t1, $s0, 48
    ld.w    $t2, $s0, 52
    ld.w    $t3, $s0, 56
    ld.w    $t4, $s0, 60
    ld.w    $t5, $s0, 64
    ld.w    $t6, $s0, 68
    ld.w    $t7, $s0, 72
    ld.w    $t8, $s0, 76
    ld.w    $fp, $s0, 80
    ld.w    $s1, $s0, 88
    ld.w    $s2, $s0, 92
    ld.w    $s3, $s0, 96
    ld.w    $s4, $s0, 100
    ld.w    $s5, $s0, 104
    ld.w    $s6, $s0, 108
    ld.w    $s7, $s0, 112
    ld.w    $s8, $s0, 116
    ld.w    $t0, $s0, 44
    ld.w    $sp, $s0, 8
    ld.w    $tp, $s0, 4
    ld.w    $s0, $s0, 84
    ertn

/* ============================================================
 * 进入空闲上下文
 * @details 只把 sp 换成内核给的栈顶并返回。这里不能写成 C 函数：C 版本会在
 *          新栈上做 epilogue（从新 sp 处恢复 ra/s0），等于从一块未初始化的
 *          内存里弹栈。
 * @param a0 空闲任务栈顶原始地址（尚未构造栈帧）
 * @param a1 特权标志（本移植不使用，保留参数以对齐 HAL）
 * ============================================================ */
    .section .text.arkrtos_port_enter_idle, "ax"
    .globl  arkrtos_port_enter_idle
    .align  2
arkrtos_port_enter_idle:
    move    $sp, $a0
    jr      $ra
