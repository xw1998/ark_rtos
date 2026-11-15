/**
* @file start.S
* @brief Ark RTOS 在 LoongArch32 上的复位入口
* @details 与 la64 版同一套三件事（设 sp、清 .bss、进 main），差别只有寄存器
*          宽度：清 .bss 每步搬 4 字节而不是 8 字节，`la.abs` 展开成
*          `lu12i.w + ori` 而不是 `lu12i.w + ori + lu32i.d + lu52i.d`。
*
*          **本目标没有可运行的载体**（无 loongarch32 模拟器、无板子），这份
*          入口只验证到「汇编通过 + 链接出 ELF」。将来接真实芯片时，装载方式
*          （ELF 装载器 / 从 Flash 搬运）必须按那颗芯片的启动流程重写，
*          不能假设与本文件一致。
*
*          两件事，顺序固定：
*          1) 设 sp（主栈顶）；
*          2) 清 .bss —— C 标准保证静态变量零初始化，运行期没人替我们做；
*          3) 进 main()。
*
*          **不设 gp**：LoongArch psABI 没有小数据区这一说（$r21 是保留寄存器，
*          用于 TLS / 放松），所以链接脚本也不需要 __global_pointer$。
*
* @note 本文件不配置 EENTRY / CRMD：那是 arkrtos_port_irq_init() 的职责，
*       让「中断什么时候打开」只有一个决定点。
* @author xw
*/

    .section .text.start, "ax"
    .globl  _start
    .align  2
_start:
    la.abs  $sp, _stack_top

    /* 清 .bss：4 字节一步（LA32 的通用寄存器宽度）。
     * @warning 不要写 st.d：本工具链在 -march=loongarch32 下**不拒绝** .d 形式，
     *          汇编能过，但那是 LA64 语义，跑到 LA32 硬件上是错的。 */
    la.abs  $t0, __bss_start
    la.abs  $t1, __bss_end
    beq     $t0, $t1, 2f
1:
    st.w    $zero, $t0, 0
    addi.w  $t0, $t0, 4
    blt     $t0, $t1, 1b
2:
    bl      main

3:
    idle    0
    b       3b
