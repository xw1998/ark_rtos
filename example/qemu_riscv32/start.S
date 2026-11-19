/**
* @file start.S
* @brief Ark RTOS 在 QEMU `virt`（RISC-V RV32）上的复位入口
* @details QEMU 的 `-kernel` 会把 ELF 的 PT_LOAD 段按 p_paddr 拷进内存，然后
*          直接跳到 ELF 的入口地址，因此这里不需要搬运 .data（它的 LMA 就是
*          VMA，两者都在 RAM 里），只需要清 .bss。
*
*          三件事，顺序固定：
*          1) 设 gp（小数据基址）与 sp（主栈顶）。gp 必须在任何可能用到
*             gp 相对寻址的代码之前设好，所以要在 .option norelax 保护区里写；
*          2) 清 .bss —— C 标准保证静态变量零初始化，运行期没人替我们做；
*          3) 进 main()。
*
*          main() 之后的死循环是兜底：正常路径不会回来（调度启动后 CPU
*          一直在 idle/task 之间跑）。
*
* @note 本文件不配置 mtvec / mie：那是 arkrtos_port_irq_init() 的职责，
*       让「中断什么时候打开」只有一个决定点。
* @author xw
*/

    .section .text.start, "ax"
    .globl  _start
    .align  2
_start:
#if (ARKRTOS_USE_SMP == 1)
    /* QEMU `virt` 的 -kernel 直启把**所有 hart 的复位 PC** 都设成 ELF 入口，
     * 于是次核也会跑到这里。第一件事就是按 mhartid 分叉：非 0 号 hart
     * 立刻进停靠循环（只用 CSR，不碰内存 —— 此刻 CPU0 可能还没清完 .bss，
     * 次核若先写自己的栈，等 CPU0 清 .bss 时会把正在用的帧清成 0）。 */
    csrr    t0, mhartid
    bnez    t0, rv_secondary_park
#endif

    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, _stack_top

    /* 清 .bss：4 字节一步，起止地址由链接脚本给出（都按 16 字节对齐） */
    la      t0, __bss_start
    la      t1, __bss_end
    j       2f
1:
    sw      zero, 0(t0)
    addi    t0, t0, 4
2:
    bltu    t0, t1, 1b

    /* 跑构造函数表（.ctors / .init_array）：utest 用例的自动登记靠它。
     * 必须在清完 .bss 之后（用例的静态缓冲都在 .bss 里）。
     * 游标放 s0/s1（被调用者保存）：构造函数会把 t0~t6 随便改掉。 */
    la      s0, __init_array_start
    la      s1, __init_array_end
    beq     s0, s1, 6f
4:
    lw      t2, 0(s0)
    beqz    t2, 5f
    li      t3, -1
    beq     t2, t3, 5f
    jalr    t2
5:
    addi    s0, s0, 4
    bltu    s0, s1, 4b
6:
    call    main

3:
    j       3b

#if (ARKRTOS_USE_SMP == 1)
/* 次核停靠（SMP 档）
 * @details 轮询 mip.MSIP 等 CPU0 敲门；CPU0 的 arkrtos_port_cpu_start()
 *          写 msip[hart]=1 就是全部的「启动协议」（见 port.c 的 SMP 注释）。
 *          醒来后：清自己的 msip → 换到自己的静态栈 → 装 mtvec/mie/MIE
 *          → 进 C 的 arkrtos_smp_secondary_main(mhartid)，永不返回。 */
rv_secondary_park:
    mv      s0, t0                  /* s0 = 本核 hart 号（跨调用保持不变） */
1:
    csrr    t1, mip
    andi    t1, t1, 8               /* MIP.MSIP = bit3 */
    beqz    t1, 1b

    /* 清自己的 msip：地址 = CLINT_BASE + 4 * hart */
    li      t1, 4
    mul     t1, s0, t1
    li      t2, 0x02000000
    add     t1, t1, t2
    sw      zero, 0(t1)

    /* 次核也要自己的 gp：C 代码里有 gp 相对寻址（小数据），
     * 而 CPU0 设的那个 gp 只是它自己的。 */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* sp = arkrtos_smp_stacks + (hart + 1) * ARKRTOS_SMP_SEC_STACK_SIZE
     * 0 号块不用（留给引导核的既有栈）；越界 hart 交给 C 侧判，
     * 那边会把非法核号一律空转。 */
    la      t1, arkrtos_smp_stacks
    li      t2, ARKRTOS_SMP_SEC_STACK_SIZE
    addi    t3, s0, 1
    mul     t3, t3, t2
    add     sp, t1, t3
    andi    sp, sp, -16             /* ABI 要求 16 字节对齐 */

    la      t1, rv_smp_trap_entry
    csrw    mtvec, t1               /* 次核异常只走这一条入口 */
    li      t1, 8                   /* mie.MSIE = bit3（IPI） */
    csrs    mie, t1
    csrsi   mstatus, 8              /* mstatus.MIE = 1 */

    mv      a0, s0
    call    arkrtos_smp_secondary_main
2:
    j       2b
#endif /* ARKRTOS_USE_SMP == 1 */
