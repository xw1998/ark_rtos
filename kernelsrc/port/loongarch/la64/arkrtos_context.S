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
*          **帧长不是常量**：上面 256 字节是 GPR/ERA/PRMD 部分，一项不动；
*          打开 ARKRTOS_USE_FPU 后在其上追加 32 个 FPR（256 + 8*i -> $f{i}），
*          总长 528。所有「开帧 / 撤帧」的相对计算都走 LA_FRAME_BYTES，
*          恢复路径靠 ld.d $sp,$s0,16 取栈指针，不依赖帧长常量。
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
 * 帧大小与 FPU 存取（编译期；与 arkrtos_port.c 的 LA_FR_SIZE 必须一致）
 * @details 前 256 字节是 GPR/ERA/PRMD（布局见文件头），一项不动；
 *          ARKRTOS_USE_FPU=1 时在其上追加 32 个 FPR 的槽：
 *              256 + 8*i  ->  $f{i}     (i = 0..31)
 *              512        ->  FCSR（舍入模式 / 异常使能；三个调用点都
 *                              先把 t0 存进 88 槽，借用它搬运）
 *          总长 528（16 字节对齐）。
 *          关档时两个宏为空、帧仍是 256 —— 与改动前逐条指令相同。
 * @note 基址用宏参数而不是固定寄存器：eentry 用 $s0，SMP 与 resume 用 $sp。
 * ============================================================ */
#if (ARKRTOS_USE_FPU == 1)
#if (ARKRTOS_USE_LSX == 1)
#define LA_FRAME_BYTES          1040
#else
#define LA_FRAME_BYTES          528
#endif

.macro LA_FP_SAVE base
    fst.d   $f0,  \base, 256
    fst.d   $f1,  \base, 264
    fst.d   $f2,  \base, 272
    fst.d   $f3,  \base, 280
    fst.d   $f4,  \base, 288
    fst.d   $f5,  \base, 296
    fst.d   $f6,  \base, 304
    fst.d   $f7,  \base, 312
    fst.d   $f8,  \base, 320
    fst.d   $f9,  \base, 328
    fst.d   $f10, \base, 336
    fst.d   $f11, \base, 344
    fst.d   $f12, \base, 352
    fst.d   $f13, \base, 360
    fst.d   $f14, \base, 368
    fst.d   $f15, \base, 376
    fst.d   $f16, \base, 384
    fst.d   $f17, \base, 392
    fst.d   $f18, \base, 400
    fst.d   $f19, \base, 408
    fst.d   $f20, \base, 416
    fst.d   $f21, \base, 424
    fst.d   $f22, \base, 432
    fst.d   $f23, \base, 440
    fst.d   $f24, \base, 448
    fst.d   $f25, \base, 456
    fst.d   $f26, \base, 464
    fst.d   $f27, \base, 472
    fst.d   $f28, \base, 480
    fst.d   $f29, \base, 488
    fst.d   $f30, \base, 496
    fst.d   $f31, \base, 504
    /* LSX：$vr0 占 16 字节，落在 528 + 16*i（528 = 33*16，16 字节对齐；
     * 帧基址与 wi 都保持 16 字节对齐，vst 不会落 ALE）。关档时整块不产生指令。 */
#if (ARKRTOS_USE_LSX == 1)
    vst     $vr0, \base, 528
    vst     $vr1, \base, 544
    vst     $vr2, \base, 560
    vst     $vr3, \base, 576
    vst     $vr4, \base, 592
    vst     $vr5, \base, 608
    vst     $vr6, \base, 624
    vst     $vr7, \base, 640
    vst     $vr8, \base, 656
    vst     $vr9, \base, 672
    vst     $vr10, \base, 688
    vst     $vr11, \base, 704
    vst     $vr12, \base, 720
    vst     $vr13, \base, 736
    vst     $vr14, \base, 752
    vst     $vr15, \base, 768
    vst     $vr16, \base, 784
    vst     $vr17, \base, 800
    vst     $vr18, \base, 816
    vst     $vr19, \base, 832
    vst     $vr20, \base, 848
    vst     $vr21, \base, 864
    vst     $vr22, \base, 880
    vst     $vr23, \base, 896
    vst     $vr24, \base, 912
    vst     $vr25, \base, 928
    vst     $vr26, \base, 944
    vst     $vr27, \base, 960
    vst     $vr28, \base, 976
    vst     $vr29, \base, 992
    vst     $vr30, \base, 1008
    vst     $vr31, \base, 1024
#endif
    /* FCSR 也是线程上下文：舍入模式（RM，bit[9:8]）决定同一条 FP 指令
     * 算出的结果，异常使能位决定 FP 异常是否陷进来。槽位一直预留在
     * 512，所以打开 FPU 后帧长仍是 528 —— 存它不改变任何栈预算。
     * $t0 在三个调用点（eentry / 次核入口 / 立即切换）都是「原值已存
     * 进 88 槽、之后才从帧里重新装回」，所以这里借用它不伤任何活值。 */
    /* 本工具链 binutils 2.31 的 as 不认识 movfcsr2gr/movgr2fcsr 这两个
     * 助记符（实测 no match insn，寄存器类没接线），所以这里写字面编码。
     * 编码出处：真 glibc 静态二进制 fe.elf（fesetround/fegetround）里
     * 生产使用的指令，用同一套 objdump 反汇编确认；再用 .word 往返
     * assembler/objdump 验证解回同一条指令。指令形状：
     *     movfcsr2gr rd, fcsr  = 0x0114C800 | (fcsr << 5) | rd
     *     movgr2fcsr fcsr, rj  = 0x0114C000 | (rj   << 5) | fcsr
     * 行尾注释保留助记符写法，免得后来者看不出这是哪条指令。 */
    .word   0x0114C80C                  /* movfcsr2gr $t0, $fcsr0 */
    st.d    $t0, \base, 512
.endm

.macro LA_FP_RESTORE base
    fld.d   $f0,  \base, 256
    fld.d   $f1,  \base, 264
    fld.d   $f2,  \base, 272
    fld.d   $f3,  \base, 280
    fld.d   $f4,  \base, 288
    fld.d   $f5,  \base, 296
    fld.d   $f6,  \base, 304
    fld.d   $f7,  \base, 312
    fld.d   $f8,  \base, 320
    fld.d   $f9,  \base, 328
    fld.d   $f10, \base, 336
    fld.d   $f11, \base, 344
    fld.d   $f12, \base, 352
    fld.d   $f13, \base, 360
    fld.d   $f14, \base, 368
    fld.d   $f15, \base, 376
    fld.d   $f16, \base, 384
    fld.d   $f17, \base, 392
    fld.d   $f18, \base, 400
    fld.d   $f19, \base, 408
    fld.d   $f20, \base, 416
    fld.d   $f21, \base, 424
    fld.d   $f22, \base, 432
    fld.d   $f23, \base, 440
    fld.d   $f24, \base, 448
    fld.d   $f25, \base, 456
    fld.d   $f26, \base, 464
    fld.d   $f27, \base, 472
    fld.d   $f28, \base, 480
    fld.d   $f29, \base, 488
    fld.d   $f30, \base, 496
    fld.d   $f31, \base, 504
#if (ARKRTOS_USE_LSX == 1)
    vld     $vr0, \base, 528
    vld     $vr1, \base, 544
    vld     $vr2, \base, 560
    vld     $vr3, \base, 576
    vld     $vr4, \base, 592
    vld     $vr5, \base, 608
    vld     $vr6, \base, 624
    vld     $vr7, \base, 640
    vld     $vr8, \base, 656
    vld     $vr9, \base, 672
    vld     $vr10, \base, 688
    vld     $vr11, \base, 704
    vld     $vr12, \base, 720
    vld     $vr13, \base, 736
    vld     $vr14, \base, 752
    vld     $vr15, \base, 768
    vld     $vr16, \base, 784
    vld     $vr17, \base, 800
    vld     $vr18, \base, 816
    vld     $vr19, \base, 832
    vld     $vr20, \base, 848
    vld     $vr21, \base, 864
    vld     $vr22, \base, 880
    vld     $vr23, \base, 896
    vld     $vr24, \base, 912
    vld     $vr25, \base, 928
    vld     $vr26, \base, 944
    vld     $vr27, \base, 960
    vld     $vr28, \base, 976
    vld     $vr29, \base, 992
    vld     $vr30, \base, 1008
    vld     $vr31, \base, 1024
#endif
    /* FCSR 最后恢复：先让 32 个 FPR（与 32 个向量寄存器）落位，再切
     * 舍入/异常使能，免得「异常使能已打开、FPR 还没装完」这种中间态。 */
    ld.d    $t0, \base, 512
    .word   0x0114C180                  /* movgr2fcsr $fcsr0, $t0 */
.endm
#else
#define LA_FRAME_BYTES          256

.macro LA_FP_SAVE base
.endm

.macro LA_FP_RESTORE base
.endm
#endif

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
#if (ARKRTOS_USE_SMP == 1)
    /* 次核分流。硬约束：**在确认核号之前不能破坏任何活寄存器**（被中断的
     * 线程可能正拿 t0 当循环指针）。所以先按最终帧大小开 256 字节、把 t0
     * 存进它自己的槽（88），再借 t0 读 CSR.TID；确认是核 0 就把帧撤掉、
     * 原样落回下面的单核路径（指令与关档逐条相同）。 */
    addi.d  $sp, $sp, -LA_FRAME_BYTES
    st.d    $t0, $sp, 88
    csrrd   $t0, 0x40
    bnez    $t0, la_smp_trap_entry
    ld.d    $t0, $sp, 88
    addi.d  $sp, $sp, LA_FRAME_BYTES
#endif
    addi.d  $sp, $sp, -LA_FRAME_BYTES
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    /* t0 必须**先于**它被借用作临时寄存器存进帧：下面要用 t0 算「异常
     * 发生时的原始 sp」（sp+256）。若等到那时才存，帧里的 t0 槽装的是
     * sp+256 而不是调用者的 t0，ertn 之后 t0 就被改成 sp+256 —— 任何把
     * 活值放在 t0 里的循环（例如 blk 驱动整卷填 0xFF）会拿着错指针继续
     * 写，一路写到任务栈上。实测就是这样把栈整片写成 0xFF 后停机的。 */
    st.d    $t0, $sp, 88            /* 原始 t0，先存 */
    addi.d  $t0, $sp, LA_FRAME_BYTES
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

    /* FPU：必须在调用任何 C 代码之前存完（C 可能在 fp 寄存器里放活值）。
     * 关档时是空宏，这一行不产生任何指令。 */
    LA_FP_SAVE $s0

    csrrd   $t0, 0x6                /* ERA */
    st.d    $t0, $s0, 240
    csrrd   $t0, 0x1                /* PRMD */
    st.d    $t0, $s0, 248

    move    $a0, $s0
    bl      la_trap_c               /* 返回要恢复的帧（可能已换成别的任务） */
    move    $s1, $s0                /* 被换下任务的帧：用来判「到底切没切」 */
    move    $s0, $a0

    /* 换窗口 (U-2)：目标帧可能落在另一个 App 的页里，而此刻 sp 还指向被打断
     * 任务的栈 —— 那个栈本身就在 App 窗口里。所以先换到一块内核静态栈再进
     * C（la_switch_prepare 自己要用栈），换完之后不再碰任何栈。 */
    la.abs  $sp, la_switch_stack_top
    move    $a0, $s0
    move    $a1, $s1
    bl      la_switch_prepare

    /* 恢复窗口重新关中断（第 381 行起那条理由在这里更致命）：
     * la_switch_prepare 的临界区退出会把 CRMD.IE 恢复成进入时的值，
     * 于是上面那句关中断只覆盖了「决定切给谁」这一段，恢复序列本身又
     * 变成可被 tick 打断的窗口——实测（QEMU -d int）中断就落在下面的 GPR 恢复
     * 中段，硬件把 ERA 改写成恢复序列里的地址；帧里没有这条地址，ertn
     * 于是反复跳回自己，逐轮把 sp 读成 0，最后在 eentry+4 上溢出致命。
     * 这里再关一次：ertn 会按 PRMD.PIE 恢复 IE，语义不变。 */
    ori     $t1, $zero, 0x4         /* CRMD.IE 是 bit2 */
    csrxchg $zero, $t1, 0x0

    /* ERA / PRMD 与通用寄存器都从这里恢复；sp / tp / s0 最后取，
     * 因为在这一步之前 s0 还是帧指针。 */
    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    /* FPU 必须在 ertn 之前、且 s0 仍当帧指针时恢复。 */
    LA_FP_RESTORE $s0

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

#if (ARKRTOS_USE_SMP == 1)
/* ============================================================
 * 次核异常入口（SMP）
 * @details 从 eentry 的分流处进来时：sp 已经开好 256 字节帧，t0 槽(88)
 *          里存着被打断的原值，t0 当前是本核号。
 *
 *          次核**不能**走 la_trap_c()：全局调度表、全局切换栈
 *          (la_switch_stack_top)、s_in_trap 都是单核假设，两核同时进
 *          la_trap_c 会互相踩。这里只做两件事 —— 清 IPI 挂起、交给默认
 *          处理函数 —— 然后原样 ertn。帧格式与主路径一致（便于对照），
 *          打开 USE_SMP_SCHED 后整段交给 la_smp_trap_c：由它分派定时器/IPI/致命异常，
 *          并可能返回另一个任务的帧（次核自己的运行队列在这里落地）。
 * ============================================================ */
la_smp_trap_entry:
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    st.d    $t1, $sp, 96
    addi.d  $t1, $sp, LA_FRAME_BYTES
    st.d    $t1, $sp, 16           /* 帧里的 sp 槽 = 被打断时的原始 sp */
    st.d    $a0, $sp, 24
    st.d    $a1, $sp, 32
    st.d    $a2, $sp, 40
    st.d    $a3, $sp, 48
    st.d    $a4, $sp, 56
    st.d    $a5, $sp, 64
    st.d    $a6, $sp, 72
    st.d    $a7, $sp, 80
    /* t0 槽已在分流处存过；t0 现在还是本核号，最后才恢复 */
    st.d    $t2, $sp, 104
    st.d    $t3, $sp, 112
    st.d    $t4, $sp, 120
    st.d    $t5, $sp, 128
    st.d    $t6, $sp, 136
    st.d    $t7, $sp, 144
    st.d    $t8, $sp, 152
    st.d    $fp, $sp, 160
    st.d    $s0, $sp, 168
    st.d    $s1, $sp, 176
    st.d    $s2, $sp, 184
    st.d    $s3, $sp, 192
    st.d    $s4, $sp, 200
    st.d    $s5, $sp, 208
    st.d    $s6, $sp, 216
    st.d    $s7, $sp, 224
    st.d    $s8, $sp, 232

    LA_FP_SAVE $sp

    csrrd   $t1, 0x6                /* ERA  */
    st.d    $t1, $sp, 240
    csrrd   $t1, 0x1                /* PRMD */
    st.d    $t1, $sp, 248

#if (ARKRTOS_USE_SMP_SCHED == 1)
    /* 调度分核：定时器中断、IPI、以及不该出现的 SYS 全交给 C 侧分派
     * （la_smp_trap_c）。它返回的帧**可能已经是另一个任务的帧**——次核
     * 在定时器里换任务就是靠这一手，所以先把 sp 换成返回的帧，下面的
     * 恢复序列才会从正确的帧里取寄存器。 */
    move    $a0, $sp
    csrrd   $a1, 0x40
    andi    $a1, $a1, 0x3ff
    bl      la_smp_trap_c
    move    $sp, $a0
#else
    /* 清 IPI 挂起：读 STATUS(0x1000) → 写 CLEAR(0x100C)。
     * 写 1 的位被清；不清的话 ertn 回去立刻再进一次，变成中断风暴。 */
    lu12i.w $t2, 1
    iocsrrd.w $t1, $t2
    ori     $t2, $t2, 0x0c
    iocsrwr.w $t1, $t2

    bl      arkrtos_smp_ipi_handler  /* 默认实现：本核 ipi_count++ */
#endif
    ld.d    $t1, $sp, 240
    csrwr   $t1, 0x6
    ld.d    $t1, $sp, 248
    csrwr   $t1, 0x1

    LA_FP_RESTORE $sp

    ld.d    $ra, $sp, 0
    ld.d    $a0, $sp, 24
    ld.d    $a1, $sp, 32
    ld.d    $a2, $sp, 40
    ld.d    $a3, $sp, 48
    ld.d    $a4, $sp, 56
    ld.d    $a5, $sp, 64
    ld.d    $a6, $sp, 72
    ld.d    $a7, $sp, 80
    ld.d    $t1, $sp, 96
    ld.d    $t2, $sp, 104
    ld.d    $t3, $sp, 112
    ld.d    $t4, $sp, 120
    ld.d    $t5, $sp, 128
    ld.d    $t6, $sp, 136
    ld.d    $t7, $sp, 144
    ld.d    $t8, $sp, 152
    ld.d    $fp, $sp, 160
    ld.d    $s0, $sp, 168
    ld.d    $s1, $sp, 176
    ld.d    $s2, $sp, 184
    ld.d    $s3, $sp, 192
    ld.d    $s4, $sp, 200
    ld.d    $s5, $sp, 208
    ld.d    $s6, $sp, 216
    ld.d    $s7, $sp, 224
    ld.d    $s8, $sp, 232
    ld.d    $tp, $sp, 8
    ld.d    $t0, $sp, 88
    ld.d    $sp, $sp, 16
    ertn

/* ============================================================
 * 次核启动入口（SMP）
 * @details 由 pflash 里的 slave_boot_code 跳进来：CRMD.DA=1、CRMD.IE=0，
 *          sp 是固件留下的值（不可用），EENTRY 被固件清成 0。这里做四件事：
 *            1) 换到自己的静态栈（arkrtos_smp_stacks 的第 tid 块）；
 *            2) 装 EENTRY = eentry；
 *            3) 打开 ECFG 的 IPI 位（bit12）与本核 CORE_EN（固件已开过，
 *               重复一次无副作用 —— 不依赖固件实现细节）；
 *            4) 开 CRMD.IE，进 C 的 arkrtos_smp_secondary_main()。
 *          这里**不**调 arkrtos_port_irq_init()：那会建页表、清定时器状态，
 *          而且它的开中断要过全局 s_in_trap（主核正在 trap 时会被静默跳过，
 *          次核就永远收不到 IPI）。
 * ============================================================ */
    .section .text.arkrtos_smp_secondary_entry, "ax"
    .globl  arkrtos_smp_secondary_entry
    .align  2
arkrtos_smp_secondary_entry:
    /* sp = arkrtos_smp_stacks + (tid + 1) * ARKRTOS_SMP_SEC_STACK_SIZE
     * 栈向下长，所以取第 tid 块的顶端。核号越界不在这里判：C 侧
     * arkrtos_smp_secondary_main() 会把非法核号一律空转，不碰状态表。 */
    csrrd   $t0, 0x40
    andi    $t0, $t0, 0x3ff
    la.abs  $sp, arkrtos_smp_stacks
    li.d    $t2, ARKRTOS_SMP_SEC_STACK_SIZE
    mul.d   $t1, $t0, $t2
    add.d   $sp, $sp, $t1
    add.d   $sp, $sp, $t2           /* 指向本块顶端（栈向下生长） */

#if (ARKRTOS_USE_FPU == 1)
    /* EUEN.FPE（bit 0）= 1：与主核 start.S 同一条设置，必须在装 EENTRY 与
     * 开中断之前。主核在 start.S 里设这一位，而次核走的是硬件复位后自己
     * 的 CSR 状态，那一位不会“继承”过来。不设的后果是静默的：FPU 档的
     * eentry 保存 FPR（fst.d），次核一进陷阱就落 Ecode=15 FPD，异常入口
     * 自己再故障 —— 表现是 IPI 永远收不到、次核在异常入口里原地打转
     * （2026-11-21 实测复现，见 docs/QEMU模拟器移植与验证.md 9.6.11）。 */
    csrrd   $t1, 0x2
    ori     $t2, $zero, 0x1
    or      $t1, $t1, $t2
    csrwr   $t1, 0x2
#endif
#if (ARKRTOS_USE_LSX == 1)
    /* EUEN.SXE（bit 1）= 1：与 start.S 同一理由（只开 SXE、不开 ASXE）。 */
    csrrd   $t1, 0x2
    ori     $t2, $zero, 0x2
    or      $t1, $t1, $t2
    csrwr   $t1, 0x2
#endif
    la.abs  $t1, eentry
    csrwr   $t1, 0xC                /* EENTRY */

    lu12i.w $t1, 1                  /* 0x1000 = ECFG.LIE[12]：核间中断。
                                     * 不能用 ori：它的立即数是 12 位无符号，
                                     * 0x1000 会报 Immediate overflow。 */
    move    $t2, $t1
    csrxchg $t2, $t1, 0x4

    lu12i.w $t1, 1                  /* 0x1000 */
    ori     $t1, $t1, 0x4           /* CORE_EN */
    ori     $t2, $zero, 0xfff
    iocsrwr.w $t2, $t1

    ori     $t1, $zero, 0x4         /* CRMD.IE */
    csrxchg $t1, $t1, 0x0

    move    $a0, $t0
    bl      arkrtos_smp_secondary_main
1:
    b       1b
#endif /* ARKRTOS_USE_SMP == 1 */

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
    addi.d  $sp, $sp, -LA_FRAME_BYTES
    st.d    $ra, $sp, 0
    st.d    $tp, $sp, 8
    st.d    $t0, $sp, 88            /* 原始 t0，理由同 eentry */
    addi.d  $t0, $sp, LA_FRAME_BYTES
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

    LA_FP_SAVE $s0

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
    ori     $t1, $zero, 0x4         /* CRMD.IE 是 bit2 */
    csrxchg $zero, $t1, 0x0

    move    $s1, $s0                /* 本任务的帧：用来判「到底切没切」 */
    move    $a0, $s0
    bl      la_switch_inline
    move    $s0, $a0

    /* 同 eentry：这段代码可能就跑在被换下 App 自己的栈上，换窗口只能推迟到
     * 这里、并且换到内核静态栈上做。 */
    la.abs  $sp, la_switch_stack_top
    move    $a0, $s0
    move    $a1, $s1
    bl      la_switch_prepare

    /* 同 eentry：上面那段关中断只覆盖到 la_switch_prepare 为止，它的临界区
     * 退出会把 IE 恢复回来。恢复序列（写 ERA/PRMD + GPR + ertn）必须不可打断：
     * 一旦被 tick 分走，ERA 会被改写成恢复序列里的地址，ertn 反复跳回自己。
     * 关中断不影响语义：ertn 按 PRMD.PIE 恢复 IE。 */
    ori     $t1, $zero, 0x4         /* CRMD.IE 是 bit2 */
    csrxchg $zero, $t1, 0x0

    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    LA_FP_RESTORE $s0

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
    /* 同前：恢复序列必须在关中断状态下执行。异常现场进来时 IE
     * 本就是关的，但这里显式再关一次不依赖调用方；ertn 按 PRMD.PIE
     * 恢复 IE，语义不变。 */
    ori     $t1, $zero, 0x4         /* CRMD.IE 是 bit2 */
    csrxchg $zero, $t1, 0x0
    ld.d    $t0, $s0, 240
    csrwr   $t0, 0x6
    ld.d    $t0, $s0, 248
    csrwr   $t0, 0x1

    LA_FP_RESTORE $s0

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

/* ============================================================
 * TLB 重填入口（TLBRENTRY）
 * @details 用户态（PLV3）访问未映射地址、或内核页对用户不可见时，硬件跳到
 *          CSR.TLBRENTRY，此时 CRMD.DA=1 / PG=0 / PLV=0 / IE=0。触发者可能是
 *          “sp 本身就指向非法页”的任务，所以第一件事必须换到静态故障栈，
 *          否则压栈就会再次 refill，形成无限重填。
 *          动作全在 la_refill_c() 里；它不返回（恢复上下文或停机）。
 * ============================================================ */
    .section .text.tlb_refill, "ax"
    .globl  tlb_refill_entry
    .align  2
tlb_refill_entry:
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需缺页档：装好条目之后要 ertn 回到**出错的那条指令**重试，所以被
     * 打断的全部通用寄存器必须原样还回去。故障路径（下面的 #else）从来不
     * 回原指令 —— 它要么换到另一个任务的帧、要么停机 —— 所以那边不需要
     * 保存，这一档必须保存。
     *
     * 帧基址用 $r21：LoongArch psABI 把它定为保留寄存器（内核与按 psABI
     * 编译的 App 都不会用），所以可以先拿它当指针、后面不必还原。
     *
     * 这里曾经用 $t0 当帧基址，写下去就是**先毁 $t0 再保存**：被打断的
     * $t0 原值在 `la.abs $t0, <帧>` 那一步就丢了，ertn 回来时它是帧地址。
     * 实测形态：ELF 装载的 elf_memcpy 用 $t0 当源指针，第一次缺页之后源指针
     * 变成帧地址、目的指针(v13=$t0-$t0_src+$t0)整段偏掉，2 MB 窗口被逐 8 KB
     * 越写越远，最后停在窗口顶端之外的 0x02200000。用 r21 之后这个问题消失。
     *
     * 「CSR.SAVE0（0x30）」那条路也**实测在 QEMU 9.2.3 上不成立**（写进去
     * 读回 0，硬件规范里它是可选实现），所以既不用 CSR 也不动任何活寄存器。
     * 静态帧没有重入问题：异常入口硬件已关中断，而这段处理只碰内核恒等
     * 映射区与 TLBR 镜像 CSR，自己不会再次触发重填。
     * 帧 256 字节 / 16 字节对齐，只放 GPR（ERA/PRMD 仍在 CSR 里）。 */
    la.abs  $r21, la_refill_stack_top
    addi.d  $r21, $r21, -256
    st.d    $ra, $r21, 0
    st.d    $tp, $r21, 8
    st.d    $sp, $r21, 16
    st.d    $a0, $r21, 24
    st.d    $a1, $r21, 32
    st.d    $a2, $r21, 40
    st.d    $a3, $r21, 48
    st.d    $a4, $r21, 56
    st.d    $a5, $r21, 64
    st.d    $a6, $r21, 72
    st.d    $a7, $r21, 80
    st.d    $t0, $r21, 88
    st.d    $t1, $r21, 96
    st.d    $t2, $r21, 104
    st.d    $t3, $r21, 112
    st.d    $t4, $r21, 120
    st.d    $t5, $r21, 128
    st.d    $t6, $r21, 136
    st.d    $t7, $r21, 144
    st.d    $t8, $r21, 152
    st.d    $fp, $r21, 160
    st.d    $s0, $r21, 168
    st.d    $s1, $r21, 176
    st.d    $s2, $r21, 184
    st.d    $s3, $r21, 192
    st.d    $s4, $r21, 200
    st.d    $s5, $r21, 208
    st.d    $s6, $r21, 216
    st.d    $s7, $r21, 224
    st.d    $s8, $r21, 232
    addi.d  $sp, $r21, 0
    bl      la_refill_demand_c
    bnez    $a0, 1f
    /* 不是这一档能处理的：落回原路径（它自己会再换一次静态栈，不返回） */
    la.abs  $sp, la_refill_stack_top
    bl      la_refill_c
1:
    ld.d    $ra, $sp, 0
    ld.d    $tp, $sp, 8
    ld.d    $a0, $sp, 24
    ld.d    $a1, $sp, 32
    ld.d    $a2, $sp, 40
    ld.d    $a3, $sp, 48
    ld.d    $a4, $sp, 56
    ld.d    $a5, $sp, 64
    ld.d    $a6, $sp, 72
    ld.d    $a7, $sp, 80
    ld.d    $t0, $sp, 88
    ld.d    $t1, $sp, 96
    ld.d    $t2, $sp, 104
    ld.d    $t3, $sp, 112
    ld.d    $t4, $sp, 120
    ld.d    $t5, $sp, 128
    ld.d    $t6, $sp, 136
    ld.d    $t7, $sp, 144
    ld.d    $t8, $sp, 152
    ld.d    $fp, $sp, 160
    ld.d    $s0, $sp, 168
    ld.d    $s1, $sp, 176
    ld.d    $s2, $sp, 184
    ld.d    $s3, $sp, 192
    ld.d    $s4, $sp, 200
    ld.d    $s5, $sp, 208
    ld.d    $s6, $sp, 216
    ld.d    $s7, $sp, 224
    ld.d    $s8, $sp, 232
    ld.d    $sp, $sp, 16         /* 原 sp 最后一个恢复：地址先算、再写 */
    ertn
#else
    la.abs  $sp, la_refill_stack_top
    bl      la_refill_c
1:
    b       1b
#endif

/* ============================================================
 * TLB 重填专用的静态故障栈
 * @details 只在 refill 入口用一次，8 KB（QEMU 版任务栈默认 4 KB，这里留足余量
 *          以便 la_refill_c 里调用打印与调度接口）。
 * ============================================================ */
    .section .bss, "aw", @nobits
    .align  4
    .globl  la_refill_stack_bottom
la_refill_stack_bottom:
    .space  8192
    .globl  la_refill_stack_top
la_refill_stack_top:

/* ============================================================
 * 换窗口专用的静态栈 (U-2)
 * @details 切换路径要重写 App 窗口条目，而调用它的代码可能就跑在被换下
 *          App 自己的栈上（那个栈就在窗口里）。重写的一瞬间，栈上的返回
 *          地址、s0 槽、局部量全部换了物理帧 —— 所以换窗口这一步必须搬
 *          到一块**恒等映射**里的栈上做，这块就是它。2 KB：里面只跑
 *          la_switch_prepare()（一次 tlbwr + 两次 tlbsrch/tlbrd）。
 * ============================================================ */
    .align  4
    .globl  la_switch_stack_bottom
la_switch_stack_bottom:
    .space  2048
    .globl  la_switch_stack_top
la_switch_stack_top:

/* ============================================================
 * App（PLV3）故障的派发入口
 * @details eentry 把帧压在 App 自己的栈上，而故障处理最终会走到
 *          arkrtos_task_recover_pending() -> arkrtos_task_stack_init()，
 *          那一步把**整个任务栈**重填一遍。处理函数若还跑在 App 栈上，
 *          它自己的 C 帧就被抹掉，返回时 ra = 0xcdcdcdcd、PC 跳到
 *          0xcdcdcdcc（RV32 移植踩过同一个坑，见 c05f1a6）。
 *          所以这里先把 sp 换到独立的故障栈再进 C。
 *          不返回：la_app_fault_c() 要么用新帧恢复，要么停机。
 * @param a0 eentry 压好的帧指针（App 栈上，此刻仍然完好）
 * @param a1 ESTAT.Ecode
 * ============================================================ */
    .section .text.la_app_fault, "ax"
    .globl  la_app_fault_switch
    .align  2
la_app_fault_switch:
    la.abs  $sp, la_fault_stack_top
    bl      la_app_fault_c
1:
    b       1b

/* 故障派发专用的静态栈（4 KB：里面有调度器与恢复路径的调用） */
    .section .bss, "aw", @nobits
    .align  4
    .globl  la_fault_stack_bottom
la_fault_stack_bottom:
    .space  4096
    .globl  la_fault_stack_top
la_fault_stack_top:


/* ============================================================
 * FCSR 存取原语（给 C 侧调用）
 * @details 为什么单独放两个函数、而不让 C 写内联汇编：
 *          这两条指令在本工具链的 as 里没有可用助记符，只能写字面编码，
 *          而字面编码里 rd/rj 的位段是固定的 —— 交给 .S 写死 $a0，
 *          C 侧只做一次函数调用，就不存在「编译器分配了别的寄存器、
 *          编码却按 $a0 算」这种错。
 *          get: movfcsr2gr $a0, $fcsr0  -> 0x0114C804
 *          set: movgr2fcsr $fcsr0, $a0  -> 0x0114C080
 *          仅在 ARKRTOS_USE_FPU=1 时提供（关档时没有帧内 FCSR 槽，
 *          这两个原语也不该被任何人调用）。
 * ============================================================ */
#if (ARKRTOS_USE_FPU == 1)
    .section .text.arkrtos_port_fcsr, "ax"
    .align  2
    .globl  arkrtos_port_fcsr_get
arkrtos_port_fcsr_get:
    .word   0x0114C804                  /* movfcsr2gr $a0, $fcsr0 */
    jr      $ra

    .align  2
    .globl  arkrtos_port_fcsr_set
arkrtos_port_fcsr_set:
    .word   0x0114C080                  /* movgr2fcsr $fcsr0, $a0 */
    jr      $ra
#endif
