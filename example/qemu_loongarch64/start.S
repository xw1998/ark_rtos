/**
* @file start.S
* @brief Ark RTOS 在 QEMU `virt`（LoongArch64）上的复位入口
* @details QEMU 的 `-kernel` 会把 ELF 的 PT_LOAD 段按 p_paddr 拷进内存，然后
*          直接跳到 ELF 的入口地址，因此这里不需要搬运 .data（它的 LMA 就是
*          VMA，两者都在 RAM 里），只需要清 .bss。
*
*          两件事，顺序固定：
*          1) 设 sp（主栈顶）；
*          2) 清 .bss —— C 标准保证静态变量零初始化，运行期没人替我们做；
*          3) 进 main()。
*
*          **不设 gp**：LoongArch psABI 没有小数据区这一说（$r21 是保留寄存器，
*          用于 TLS / 放松），所以链接脚本也不需要 __global_pointer$。
*
*          main() 之后的死循环是兜底：正常路径不会回来（调度启动后 CPU
*          一直在 idle/task 之间跑）。
*
* @note 本文件不配置 EENTRY / CRMD：那是 arkrtos_port_irq_init() 的职责，
*       让「中断什么时候打开」只有一个决定点。
* @author xw
*/

    .section .text.start, "ax"
    .globl  _start
    .align  2
_start:
#if (ARKRTOS_USE_FPU == 1)
    /* EUEN.FPE（bit1）= 1，必须在任何 C 代码之前：内核按 lp64d（硬浮点 ABI）
     * 编译，理论上任何 C 函数都可能被 gcc 塞进浮点指令，那时 FPE=0 就是
     * ESTAT.Ecode=15 FPD。t0/t1 在这里还没用过（下一个用 t0 的是 SMP 分流）。
     * 不开这一位，连 eentry 里存 FPR 的那条 fst.d 自己都会故障（实测：
     * 第一次任务切换就地落进 INE/FPD 死循环）。 */
    csrrd   $t0, 0x2
    ori     $t1, $zero, 0x1
    or      $t0, $t0, $t1
    csrwr   $t0, 0x2
#endif

#if (ARKRTOS_USE_LSX == 1)
    /* EUEN.SXE（bit1）= 1：向量指令开关。必须在任何用户代码之前打开，
     * 同时**只开 SXE、不开 ASXE（bit2）** —— 帧里只保存 128 位的 $vr，
     * 打开 LASX 却不保存 $xr 会变成跨任务静默算错；不开这一位，
     * 用到 LASX 的代码落 Ecode=13 INE，是显式故障。
     * 依赖 ARKRTOS_USE_FPU（FPE 同一条 CSR，上面那段已先行设置）。 */
    csrrd   $t0, 0x2
    ori     $t1, $zero, 0x2
    or      $t0, $t0, $t1
    csrwr   $t0, 0x2
#endif

#if (ARKRTOS_USE_SMP == 1)
    /* 次核停靠：CSR.TID（0x40）是本核硬件号，QEMU 在 cpu reset 里直接填
     * env->CSR_TID = cpu_index（target/loongarch/cpu.c），所以读出来就是
     * 核号，不依赖任何板级寄存器。
     *
     * 非 0 核不设 sp、不清 .bss、不跑构造函数表、不进 main —— 这三件事只能
     * 由主核做一次，做两遍就会互相踩。
     *
     * **本机（QEMU）这条分支不可达**：hw/loongarch/boot.c 的
     * loongarch_direct_kernel_boot() 给次核设的 PC 是 VIRT_FLASH0_BASE 里的
     * slave_boot_code（idle 等 mailbox），不是本 ELF 的 entry；它要等 IPI
     * 与 mailbox 才跳进来，那属于 IPI 通路（本轮不做）。所以这个分支服务于
     * 真实硬件 / 其它启动路径，本机没有能触发它的路径。 */
    csrrd   $t0, 0x40
    beqz    $t0, 7f
8:
    idle    0
    b       8b
7:
#endif
    la.abs  $sp, _stack_top

    /* 清 .bss：8 字节一步，起止地址由链接脚本给出（都按 16 字节对齐） */
    la.abs  $t0, __bss_start
    la.abs  $t1, __bss_end
    beq     $t0, $t1, 2f
1:
    st.d    $zero, $t0, 0
    addi.d  $t0, $t0, 8
    blt     $t0, $t1, 1b
2:
    /* 跑构造函数表（.ctors / .init_array）：utest 用例的自动登记、以及
     * 将来任何 __attribute__((constructor)) 的初始化都靠它。
     * 必须在清零 .bss 之后 —— 用例的静态缓冲都在 .bss 里。
     * 循环游标放 $s0/$s1（被调用者保存寄存器）：构造函数是普通 C 函数，
     * 会把 $t0~$t8 随便改掉，用临时寄存器当游标会跑飞。 */
    la.abs  $s0, __init_array_start
    la.abs  $s1, __init_array_end
    beq     $s0, $s1, 6f
4:
    ld.d    $t2, $s0, 0
    beq     $t2, $zero, 5f          /* 空槽（部分工具链留 0 作结束符） */
    addi.d  $t3, $zero, -1
    beq     $t2, $t3, 5f            /* 老式 .ctors 用 -1 作结束标记 */
    jirl    $ra, $t2, 0
5:
    addi.d  $s0, $s0, 8
    blt     $s0, $s1, 4b
6:
    bl      main

3:
    idle    0
    b       3b
