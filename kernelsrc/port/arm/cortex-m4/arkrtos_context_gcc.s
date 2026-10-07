/* Ark RTOS Cortex-M 上下文切换 —— GNU as (arm-none-eabi-gcc) 语法版本
 * -----------------------------------------------------------------------------
 * 与 kernelsrc/port/arm/cortex-m4/arkrtos_context.S（Keil armasm 版）逐条等价：
 * 同一套栈布局、同一套插桩点、同一套 EXC_RETURN 语义，只是换成 GNU 汇编语法，
 * 使内核可以用 arm-none-eabi-gcc 工具链构建。两边若有一处改动，必须同步另一处。
 *
 * FPU：由 ARKRTOS_USE_FPU 控制（本文件经 gcc 预处理，可用 #if）。开启时任务可能
 * 拥有含 S16-S31 的扩展帧，EXC_RETURN 的 bit4 说明被中断代码用的是哪种帧：
 *   bit4 = 0 → 扩展帧（还要存/取 S16-S31）
 *   bit4 = 1 → 基本帧
 * -----------------------------------------------------------------------------*/

    .syntax unified
    .thumb

/* ============================ PendSV_Handler ============================ */
    .section .text.PendSV_Handler, "ax", %progbits
    .global  PendSV_Handler
    .type    PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    push    {r0, lr}

    /* 插桩：上下文切换现场。调用 C 前先自己保护 R0-R3/R12/LR（AAPCS 允许被调方
     * 改它们），这里 R0 与 R12 本函数后面还要用。 */
    push    {r0-r3, r12, lr}
    movs    r0, #14
    movs    r1, #0
    ldr     r2, =arkrtos_trace_isr
    blx     r2
    pop     {r0-r3, r12, lr}

    ldr     r2, =arkrtos_sched_is_switching
    blx     r2
    cmn     r0, #0
    bmi     endpend

    mrs     r1, psp

    /* 上面的 BLX 可能改掉 R0-R3/R12，所以从入口的 PUSH {R0, LR} 重新取回被中断
     * 上下文的 EXC_RETURN（LR 在 SP+4）。 */
    ldr     r12, [sp, #4]

#if (ARKRTOS_USE_FPU == 1)
    /* 扩展帧在用（EXC_RETURN bit4=0）：连 S16-S31 一起存 */
    tst     r12, #0x10
    it      eq
    vstmdbeq r1!, {s16-s31}
#endif

    /* 存 R4-R11 与本上下文的 EXC_RETURN。R12 > R11，EXC_RETURN 落在块顶，
     * 正好紧贴硬件帧下面——正是 arkrtos_port_stack_init() 搭出的布局。 */
    stmdb   r1!, {r4-r11, r12}

    ldr     r2, =arkrtos_sched_activate
    blx     r2

    /* R0 = 目标任务栈，R3 = 目标任务自己的 EXC_RETURN。 */
    ldmia   r0!, {r4-r11, r12}

#if (ARKRTOS_USE_FPU == 1)
    /* 目标任务的 EXC_RETURN 说明它是否拥有 S16-S31。 */
    tst     r12, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif

    msr     psp, r0

    /* 插桩：切换完成。R12 此时是目标任务自己的 EXC_RETURN，马上要拿它异常返回，
     * 所以要保护到 BLX 之后再恢复。 */
    push    {r0-r3, r12, lr}
    movs    r0, #14
    movs    r1, #1
    ldr     r2, =arkrtos_trace_isr
    blx     r2
    pop     {r0-r3, r12, lr}

    /* 用目标任务的 EXC_RETURN 异常返回，而不是入口压在栈上的那个：只有它才让
     * 硬件在 PSP 上恢复 Thread 模式——真正完成切换的是这一步。 */
    pop     {r0, lr}
    bx      r12

endpend:
    /* 插桩：本次 PendSV 没真正切换（调度器判定不需要），进出仍成对记，
     * 否则时间线上会出现「只进不出」的中断。 */
    push    {r0-r3, r12, lr}
    movs    r0, #14
    movs    r1, #1
    ldr     r2, =arkrtos_trace_isr
    blx     r2
    pop     {r0-r3, r12, lr}

    pop     {r0, pc}
    .size   PendSV_Handler, . - PendSV_Handler

/* =================== arkrtos_port_resume_task =================== */
/* 在异常处理程序内部直接恢复目标任务并异常返回。
 * 入参: R0 = 目标任务栈指针（指向其保存的 R4-R11 + EXC_RETURN）
 * 说明: 故障恢复路径专用。HardFault 优先级(-1)高于 PendSV(0xFF)，故障处理程序
 *       活动期间 PendSV 不会被服务，因此不能靠 PendSV 触发切换，必须在本函数内
 *       直接完成「恢复寄存器 + 异常返回」。函数不返回。
 *       栈布局与 PendSV_Handler / arkrtos_port_stack_init 保持一致。 */
    .section .text.arkrtos_port_resume_task, "ax", %progbits
    .global  arkrtos_port_resume_task
    .type    arkrtos_port_resume_task, %function
    .thumb_func
arkrtos_port_resume_task:
    ldmia   r0!, {r4-r11, lr}

#if (ARKRTOS_USE_FPU == 1)
    /* 目标任务保存的 EXC_RETURN bit4=0 表示其使用 FPU 扩展帧，还需恢复 S16-S31 */
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif

    msr     psp, r0
    bx      lr
    .size   arkrtos_port_resume_task, . - arkrtos_port_resume_task

/* ============================ SVC_Handler ============================ */
    .section .text.SVC_Handler, "ax", %progbits
    .global  SVC_Handler
    .type    SVC_Handler, %function
    .thumb_func
SVC_Handler:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    b       SVC_Server
    .size   SVC_Handler, . - SVC_Handler
