/**
* @file arkrtos_context.S
* @brief Ark RTOS LoongArch64 异常入口与上下文切换（GNU as 语法）
* @details 与 Cortex-M / RISC-V 的 arkrtos_context.S 承担同样的三件事，但
*          LoongArch 的两条硬约束决定了实现方式不同：
*
*          1) **没有软件中断**。LoongArch 的 IPI（CSR.IPI）只发给「其他核」，
*             单核上 `csrwr` 自触发实测没有任何中断产生（见 docs 里的探针
*             记录）——所以不存在 RISC-V 那种「置 MSIP 挂起、异常返回后再
*             进一次 trap 完成切换」的 PendSV 对应物。任务上下文里的切换
*             只能**就地完成**：在调用者栈上构造一个与 trap 帧同格式的帧，
*             调 C 辅助函数决定「切给谁」，再按返回帧统一恢复并 ertn。
*             中断上下文里的切换请求由 la_trap_c() 在异常出口处结算。
*             两条路径共用同一套保存/恢复代码，帧格式只有一种。
*
*          2) **异常入口只有 EENTRY 一个**：Ecode 全在 ESTAT 里，由
*             la_trap_c() 分派。
*
*          栈帧布局（256 字节，16 字节对齐，与 arkrtos_port_stack_init() 一致）：
*
*              偏移  内容                  偏移  内容
*              0     ra  (r1)              136   t6  (r18)
*              8     tp  (r2)              144   t7  (r19)
*              16    sp  (r3) 原值         152   t8  (r20)
*              24    a0  (r4)              160   fp  (r22)
*              32    a1  (r5)              168   s0  (r23)
*              40    a2  (r6)              176   s1  (r24)
*              48    a3  (r7)              184   s2  (r25)
*              56    a4  (r8)              192   s3  (r26)
*              64    a5  (r9)              200   s4  (r27)
*              72    a6  (r10)             208   s5  (r28)
*              80    a7  (r11)             216   s6  (r29)
*              88    t0  (r12)             224   s7  (r30)
*              96    t1  (r13)             232   s8  (r31)
*              104   t2  (r14)             240   ERA
*              112   t3  (r15)             248   PRMD
*              120   t4  (r16)
*              128   t5  (r17)
*
*          r21 是 psABI 保留寄存器（TLS 用），编译器不分配，因此不进帧。
*
*          **s0 在帧内被当作帧指针**：它是 callee-saved，跨 C 调用不会被改掉，
*          所以「保存现场 → 调 C → 从返回的帧指针恢复」这条链不需要额外的
*          全局变量（RISC-V 版用 tp，理由相同）。C 返回的帧指针可能已经换成
*          另一个任务的帧，恢复动作照它执行即可。
*
* @note 寄存器名用 LoongArch psABI 的别名（$ra/$tp/$sp/$a0~$a7/$t0~$t8/
*       $fp/$s0~$s8）。si12 偏移上限 +2047，本文件最大偏移 248，安全。
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
    addi.d  $sp, $sp, -256
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    /* t0 必须**先于**它被借用作临时寄存器存进帧：下面要用 t0 算「异常
     * 发生时的原始 sp」（sp+256）。若等到那时才存，帧里的 t0 槽装的是
     * sp+256 而不是调用者的 t0，ertn 之后 t0 就被改成 sp+256 —— 任何把
     * 活值放在 t0 里的循环（例如 blk 驱动整卷填 0xFF）会拿着错指针继续
     * 写，一路写到任务栈上。实测就是这样把栈整片写成 0xFF 后停机的。 */
    st.d    $t0, $sp, 88            /* 原始 t0，先存 */
    addi.d  $t0, $sp, 256
    st.d    $t0, $sp, 16            /* 帧里的 sp 槽 = 异常发生时的原始 sp */
    st.d    $s0, $sp, 168           /* s0 即将被借用作帧指针，先存原值 */
    move    $s0, $sp                /* s0 = 帧指针 */

    st.d    $a0, $s0, 24
    st.d    $a1, $s0, 32
    st.d    $a2, $s0, 40
    st.d    $a3, $s0, 48
    st.d    $a4, $s0, 56
    st.d    $a5, $s0, 64
    st.d    $a6, $s0, 72
    st.d    $a7, $s0, 80
    /* t0 已在入口处存过，这里不能再写（那时 t0 是 sp+256） */
    st.d    $t1, $s0, 96
    st.d    $t2, $s0, 104
    st.d    $t3, $s0, 112
    st.d    $t4, $s0, 120
    st.d    $t5, $s0, 128
    st.d    $t6, $s0, 136
    st.d    $t7, $s0, 144
    st.d    $t8, $s0, 152
    st.d    $fp, $s0, 160
    st.d    $s1, $s0, 176
    st.d    $s2, $s0, 184
    st.d    $s3, $s0, 192
    st.d    $s4, $s0, 200
    st.d    $s5, $s0, 208
    st.d    $s6, $s0, 216
    st.d    $s7, $s0, 224
    st.d    $s8, $s0, 232

    csrrd   $t0, 0x6                /* ERA */
    st.d    $t0, $s0, 240
    csrrd   $t0, 0x1                /* PRMD */
    st.d    $t0, $s0, 248

    move    $a0, $s0
    bl      la_trap_c               /* 返回要恢复的帧（可能已换成别的任务） */
    move    $s0, $a0

    /* ERA / PRMD 与通用寄存器都从这里恢复；sp / tp / s0 最后取，
     * 因为在这一步之前 s0 还是帧指针。 */
    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    ld.d    $ra, $s0, 0
    ld.d    $a0, $s0, 24
    ld.d    $a1, $s0, 32
    ld.d    $a2, $s0, 40
    ld.d    $a3, $s0, 48
    ld.d    $a4, $s0, 56
    ld.d    $a5, $s0, 64
    ld.d    $a6, $s0, 72
    ld.d    $a7, $s0, 80
    ld.d    $t1, $s0, 96
    ld.d    $t2, $s0, 104
    ld.d    $t3, $s0, 112
    ld.d    $t4, $s0, 120
    ld.d    $t5, $s0, 128
    ld.d    $t6, $s0, 136
    ld.d    $t7, $s0, 144
    ld.d    $t8, $s0, 152
    ld.d    $fp, $s0, 160
    ld.d    $s1, $s0, 176
    ld.d    $s2, $s0, 184
    ld.d    $s3, $s0, 192
    ld.d    $s4, $s0, 200
    ld.d    $s5, $s0, 208
    ld.d    $s6, $s0, 216
    ld.d    $s7, $s0, 224
    ld.d    $s8, $s0, 232
    ld.d    $t0, $s0, 88           /* t0 在 csrwr 之后才恢复 */
    ld.d    $sp, $s0, 16           /* 先换栈：此刻 s0 仍指向帧，后面的取数还成立 */
    ld.d    $tp, $s0, 8
    ld.d    $s0, $s0, 168
    ertn

/* ============================================================
 * 任务上下文里的「立即切换」
 * @details 由 arkrtos_port_switch_task()（C 侧）调用，只在任务上下文里进入。构造一个与 trap 帧同格式的
 *          帧（ERA = 返回地址、PRMD 按当前 CRMD 的 IE/PLV 折算），交给
 *          C 辅助函数决定切给谁，再按返回帧恢复并 ertn —— 于是「从这里
 *          返回」这件事对调用者完全透明：没切换时回到下一条指令，切换时
 *          直接回到另一个任务的断点。
 *
 *          为什么可以就地切换而不是挂起：所有 ARKRTOS_SWITCH_TASK() 调用点
 *          都在 ARKRTOS_ENABLE_IRQ() 之后、不在临界区里（逐处核查过），
 *          且单核下此刻没有别的上下文会碰调度表。
 * ============================================================ */
    .section .text.arkrtos_port_switch_task_inline, "ax"
    .globl  arkrtos_port_switch_task_inline
    .align  2
arkrtos_port_switch_task_inline:
    addi.d  $sp, $sp, -256
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    st.d    $t0, $sp, 88            /* 原始 t0，理由同 eentry */
    addi.d  $t0, $sp, 256
    st.d    $t0, $sp, 16
    st.d    $s0, $sp, 168
    move    $s0, $sp

    st.d    $a0, $s0, 24
    st.d    $a1, $s0, 32
    st.d    $a2, $s0, 40
    st.d    $a3, $s0, 48
    st.d    $a4, $s0, 56
    st.d    $a5, $s0, 64
    st.d    $a6, $s0, 72
    st.d    $a7, $s0, 80
    /* t0 已在入口处存过，这里不能再写（那时 t0 是 sp+256） */
    st.d    $t1, $s0, 96
    st.d    $t2, $s0, 104
    st.d    $t3, $s0, 112
    st.d    $t4, $s0, 120
    st.d    $t5, $s0, 128
    st.d    $t6, $s0, 136
    st.d    $t7, $s0, 144
    st.d    $t8, $s0, 152
    st.d    $fp, $s0, 160
    st.d    $s1, $s0, 176
    st.d    $s2, $s0, 184
    st.d    $s3, $s0, 192
    st.d    $s4, $s0, 200
    st.d    $s5, $s0, 208
    st.d    $s6, $s0, 216
    st.d    $s7, $s0, 224
    st.d    $s8, $s0, 232

    st.d    $ra, $s0, 240           /* ERA = 本函数的返回地址 */
    /* PRMD 与 CRMD 的低 4 位**位序相同**：PPLV[1:0]、PIE[2]、PWE[3]。
     * 所以「切回去」的 PRMD 就是 CRMD & 0x7（bit3 是 CRMD 的 DA，
     * 不能带进 PRMD 的 PWE）——这样切回去不会凭空打开或关掉中断。
     * 早先按「PIE 在 bit0、PPLV 在 [3:2]」折算，结果每个任务都被
     * ertn 到 PLV1 且关中断，`idle 0` 再也等不到中断（见 docs）。 */
    csrrd   $t0, 0x0
    andi    $t0, $t0, 7
    st.d    $t0, $s0, 248
    /* 存档之后立刻关中断：下面这段（bl 到恢复现场并 ertn）绝不能被 tick
     * 打断。恢复序列开头就靠 csrwr 写 ERA/PRMD 来确定返回到哪里，而一次
     * 中断会由硬件把这两个 CSR 改写成「被打断的那条恢复指令」——中断
     * 返回后 ertn 于是落进恢复序列中间，带着半套寄存器继续跑，最终跳到
     * 非法地址（实测表现为 ld.d 拿一个代码地址当帧指针、撞 ADEM 停机）。
     * 关中断不影响语义：ertn 会按 PRMD.PIE（上面已从 CRMD 存好）恢复 IE。 */
    ori     $t1, $zero, 1
    csrxchg $zero, $t1, 0x0

    move    $a0, $s0
    bl      la_switch_inline
    move    $s0, $a0

    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    ld.d    $ra, $s0, 0
    ld.d    $a0, $s0, 24
    ld.d    $a1, $s0, 32
    ld.d    $a2, $s0, 40
    ld.d    $a3, $s0, 48
    ld.d    $a4, $s0, 56
    ld.d    $a5, $s0, 64
    ld.d    $a6, $s0, 72
    ld.d    $a7, $s0, 80
    ld.d    $t1, $s0, 96
    ld.d    $t2, $s0, 104
    ld.d    $t3, $s0, 112
    ld.d    $t4, $s0, 120
    ld.d    $t5, $s0, 128
    ld.d    $t6, $s0, 136
    ld.d    $t7, $s0, 144
    ld.d    $t8, $s0, 152
    ld.d    $fp, $s0, 160
    ld.d    $s1, $s0, 176
    ld.d    $s2, $s0, 184
    ld.d    $s3, $s0, 192
    ld.d    $s4, $s0, 200
    ld.d    $s5, $s0, 208
    ld.d    $s6, $s0, 216
    ld.d    $s7, $s0, 224
    ld.d    $s8, $s0, 232
    ld.d    $t0, $s0, 88
    ld.d    $sp, $s0, 16
    ld.d    $tp, $s0, 8
    ld.d    $s0, $s0, 168
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
    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    ld.d    $ra, $s0, 0
    ld.d    $a0, $s0, 24
    ld.d    $a1, $s0, 32
    ld.d    $a2, $s0, 40
    ld.d    $a3, $s0, 48
    ld.d    $a4, $s0, 56
    ld.d    $a5, $s0, 64
    ld.d    $a6, $s0, 72
    ld.d    $a7, $s0, 80
    ld.d    $t1, $s0, 96
    ld.d    $t2, $s0, 104
    ld.d    $t3, $s0, 112
    ld.d    $t4, $s0, 120
    ld.d    $t5, $s0, 128
    ld.d    $t6, $s0, 136
    ld.d    $t7, $s0, 144
    ld.d    $t8, $s0, 152
    ld.d    $fp, $s0, 160
    ld.d    $s1, $s0, 176
    ld.d    $s2, $s0, 184
    ld.d    $s3, $s0, 192
    ld.d    $s4, $s0, 200
    ld.d    $s5, $s0, 208
    ld.d    $s6, $s0, 216
    ld.d    $s7, $s0, 224
    ld.d    $s8, $s0, 232
    ld.d    $t0, $s0, 88
    ld.d    $sp, $s0, 16
    ld.d    $tp, $s0, 8
    ld.d    $s0, $s0, 168
    ertn

/* ============================================================
 * 进入空闲上下文
 * @details 只把 sp 换成内核给的栈顶并返回。这里不能写成 C 函数：C 版本会在
 *          新栈上做 epilogue（从新 sp 处恢复 ra/s0），等于从一块未初始化的
 *          内存里弹栈。Cortex-M 版只改 PSP 所以可以留在 C 里，LoongArch 不行。
 * @param a0 空闲任务栈顶原始地址（尚未构造栈帧）
 * @param a1 特权标志（本移植不使用，保留参数以对齐 HAL）
 * ============================================================ */
    .section .text.arkrtos_port_enter_idle, "ax"
    .globl  arkrtos_port_enter_idle
    .align  2
arkrtos_port_enter_idle:
    move    $sp, $a0
    jr      $ra
