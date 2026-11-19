/* Ark RTOS - QEMU `mps2-an386`（Cortex-M4）复位入口与向量表
 * -----------------------------------------------------------------------------
 * M-profile 的复位语义与 RISC-V 不同：CPU 不解码 ELF 入口，而是从地址 0x0
 * 取两个字 —— 第一个是初始 SP（MSP），第二个是复位 PC。所以向量表必须物理
 * 落在镜像最前面（链接脚本用 KEEP(*(.text.start)) 把它钉在 CHIP_FLASH_BASE）。
 *
 * QEMU 的 armv7m_load_kernel() 按 p_paddr 把 PT_LOAD 段拷进内存，随后复位从
 * 0x0 读表，因此 .data 的 LMA 与 VMA 同为 0x20000000（同一片 SSRAM 的两段），
 * 启动代码不需要搬运 .data，只清 .bss。
 *
 * 本文件由 gcc 预处理（-x assembler-with-cpp），所以能用 #include 与 #if。
 * ------------------------------------------------------------------------- */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* ============================ 向量表 ============================ */
    .section .text.start, "ax", %progbits
    .global  _vectors
    .type    _vectors, %object
_vectors:
    .word   _stack_top              /* 0  MSP                  */
    .word   Reset_Handler           /* 1  Reset                */
    .word   NMI_Handler             /* 2  NMI                  */
    .word   HardFault_Handler       /* 3  HardFault            */
    .word   MemManage_Handler       /* 4  MemManage            */
    .word   BusFault_Handler        /* 5  BusFault             */
    .word   UsageFault_Handler      /* 6  UsageFault           */
    .word   0                       /* 7  reserved             */
    .word   0                       /* 8  reserved             */
    .word   0                       /* 9  reserved             */
    .word   0                       /* 10 reserved             */
    .word   SVC_Handler             /* 11 SVCall               */
    .word   DebugMon_Handler        /* 12 DebugMon             */
    .word   0                       /* 13 reserved             */
    .word   PendSV_Handler          /* 14 PendSV（任务切换）   */
    .word   SysTick_Handler         /* 15 SysTick（内核节拍）  */
    .size   _vectors, . - _vectors

/* ============================ 复位入口 ============================ */
    .section .text.start, "ax", %progbits
    .global  Reset_Handler
    .type    Reset_Handler, %function
    .thumb_func
Reset_Handler:
    /* 硬件已经按向量表装了 MSP，这里再显式装一次：
     * 让「栈顶来自链接脚本」这件事在代码里可见，不依赖上位机行为。 */
    ldr     r0, =_stack_top
    mov     sp, r0

    /* 清 .bss（都按 16 字节对齐，按字清零即可） */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
1:
    cmp     r0, r1
    itt     lo
    strlo   r2, [r0], #4
    blo     1b

    /* 跑构造函数表：utest 用例等靠 __attribute__((constructor)) 自注册。
     * 本档 ARKRTOS_USE_UTEST = 0，但保留这段是链接脚本契约的一部分
     * （__init_array_start/end 一直在），关档时这段循环走 0 次。
     *
     * 游标放 r4/r5：blx 会改掉 r0-r3 与 r12，而 r4-r11 由被调方保存，
     * 循环跨调用仍然有效。 */
    ldr     r4, =__init_array_start
    ldr     r5, =__init_array_end
2:
    cmp     r4, r5
    bhs     3f
    ldr     r6, [r4], #4
    cmp     r6, #0
    beq     2b
    blx     r6
    b       2b
3:
    bl      main

4:
    b       4b
    .size   Reset_Handler, . - Reset_Handler

/* ============================ 默认处理 ============================ */
    .section .text, "ax", %progbits
    .global  NMI_Handler
    .global  DebugMon_Handler
    .global  Default_Handler
    .thumb_func
NMI_Handler:
DebugMon_Handler:
Default_Handler:
    b       Default_Handler
    .size   Default_Handler, . - Default_Handler
