/**
 * Ark RTOS 小程序（Mini） SDK — GCC 版启动入口（与 Keil 的 arkrtos_mini_start.s 对应）
 * ============================================================
 * Keil(armasm) 侧这一小段只做一件事：跳到 ARMCC 运行期入口 __main，由它完成
 * 「.data 初值拷贝 + .bss 清零（scatter loading）」再调 main()。
 * GCC 没有 __main，所以这里自己把同样三件事做掉，再调 main()：
 *   1) 把 .data 初值从镜像加载地址搬到 RAM（_sidata -> _sdata.._edata）
 *   2) 清零 .bss（_sbss.._ebss）
 *   3) 走 __libc_init_array（静态构造）后进入 main()
 *
 * 上述符号由 GNU ld 脚本（tools/gen_ld.py 生成）提供。
 * 镜像由内核装载并已设好栈，故这里不碰 SP。
 *
 * 维护：本文件只给 GCC 工程用，不要登记进 Keil 工程；
 *       两边的入口符号名必须一致（MINISTART）。
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .global MINISTART
    .thumb_func
    .type MINISTART, %function
MINISTART:
    /* 1) .data 初值搬运 */
    ldr  r0, =_sdata
    ldr  r1, =_edata
    ldr  r2, =_sidata
    movs r3, #0
    b    2f
1:  ldr  r4, [r2, r3]
    str  r4, [r0, r3]
    adds r3, r3, #4
2:  adds r4, r0, r3
    cmp  r4, r1
    bcc  1b

    /* 2) 清零 .bss */
    ldr  r2, =_sbss
    ldr  r4, =_ebss
    movs r3, #0
    b    4f
3:  str  r3, [r2]
    adds r2, r2, #4
4:  cmp  r2, r4
    bcc  3b

    /* 3) 静态构造 + 入口 */
    bl   __libc_init_array
    bl   main
5:  b    5b
    .size MINISTART, .-MINISTART
