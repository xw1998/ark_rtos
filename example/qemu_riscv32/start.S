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
