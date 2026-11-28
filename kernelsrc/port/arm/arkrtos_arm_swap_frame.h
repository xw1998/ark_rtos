/*
 * arkrtos_arm_swap_frame.h —— Cortex-M 热切换帧形状判定（M3 / M4 / M7 共用）
 *
 * 为什么单独摘成一个纯函数
 * ------------------------
 * 「SVC 压出来的这一帧有多大、能不能改写」同时决定两件事：
 *   1) 内核做「帧是否整个落在旧块的栈区里」边界检查时用多大的数；
 *   2) 端口改写这一帧时，PC / xPSR 在第几个字。
 *
 * M4 / M7 打开 FPU 之后，**用过 FPU 的任务**在 SVC 入口拿到的是扩展帧：
 * 硬件在基本帧下面再压 S0-S15、一个保留字、FPSCR，共 0x68 = 104 字节，
 * 于是 R0 从第 0 个字后移到第 18 个字，PC 到第 24、xPSR 到第 25。M3 没有
 * FPU，永远只有 32 字节的基本帧。
 *
 * 这段判断只看「帧内存」和「异常入口那一刻 CONTROL.FPCA 的值」，不读硬件
 * 寄存器、不写内存，所以宿主用例可以拿假帧逐条验
 * （kernelsrc/tests/test_arm_swap_frame_host.c）。端口层只负责把 FPCA 位和
 * 「这个帧到旧块末尾还剩多少字节」两样喂进来。
 *
 * 为什么要吃 avail（还剩多少字节）
 * --------------------------------
 * 基本帧的 xPSR 在 [7]，扩展帧的 xPSR 在 [25]。如果 FPCA 说「扩展帧」而
 * 这块内存只剩 40 字节（典型情形：任务在中断上下文里发的 SVC，帧压在 MSP
 * 上、根本不在这个任务的块里），照 [25] 去读就越界。传进「还剩多少」之后，
 * 本函数只在放得下的前提下才去认形状——不猜，也不越界读。
 *
 * 为什么不用「看 [7] 像不像 xPSR」来判扩展帧
 * -------------------------------------------
 * 扩展帧的 [7] 是 S4 的位模式，它「碰巧」满足 Thumb 位为 1、IPSR 为 0 的
 * 概率是 1/512。按内容猜错一次就是改写错位置、把 FPU 现场写坏，而且是
 * 「跑一万次错二十次」那种。所以形状由硬件给的 FPCA 决定，内容只用来
 * **复核**（xPSR 不合法就拒绝，绝不静默按另一种形状改）。
 */

#ifndef ARKRTOS_ARM_SWAP_FRAME_H
#define ARKRTOS_ARM_SWAP_FRAME_H

/* 基本帧：R0 R1 R2 R3 R12 LR PC xPSR，8 个字。 */
#define ARKRTOS_ARM_FRAME_BASIC_BYTES   32u
/* 扩展帧：S0-S15(16) + 保留(1) + FPSCR(1) + 基本帧(8) = 26 个字。 */
#define ARKRTOS_ARM_FRAME_FPU_BYTES    104u
#define ARKRTOS_ARM_FRAME_FPU_WORDS     26u
/* 扩展帧里 R0 与 PC / xPSR 的字偏移，由上面 26 个字推出，不是抄来的。 */
#define ARKRTOS_ARM_FRAME_FPU_R0_WORD   18u
#define ARKRTOS_ARM_FRAME_FPU_PC_WORD   24u
#define ARKRTOS_ARM_FRAME_FPU_PSR_WORD  25u

/* xPSR 的合法性：Thumb 位必须为 1，IPSR 必须为 0（Thread 模式）。
 * 与 arkrtos_port_stack_init() 里造帧时写的 0x01000000 同一个口径。 */
static int arkrtos_arm_frame_psr_ok(unsigned long psr)
{
    if((psr & 0x01000000ul) == 0ul)
    {
        return 0;
    }
    if((psr & 0x000001FFul) != 0ul)
    {
        return 0;
    }
    return 1;
}

/* 这一帧占多少字节；0 = 这一帧本架构不认识（或根本放不下一帧）。
 *
 * p_frame   SVC 入口的帧指针（PSP）
 * fpu_frame 异常入口那一刻 CONTROL.FPCA 非 0（= 硬件压了扩展帧）
 * avail     p_frame 到旧块（栈区）末尾还剩多少字节；越界时由调用方给 0
 */
static unsigned long arkrtos_arm_swap_frame_bytes(const void *p_frame,
                                                  int fpu_frame,
                                                  unsigned long avail)
{
    const unsigned long *p_words = (const unsigned long *)p_frame;

    if(p_words == 0)
    {
        return 0ul;
    }

    if(fpu_frame != 0)
    {
        if(avail < (unsigned long)ARKRTOS_ARM_FRAME_FPU_BYTES)
        {
            return 0ul;
        }
        if(arkrtos_arm_frame_psr_ok(p_words[ARKRTOS_ARM_FRAME_FPU_PSR_WORD]) == 0)
        {
            return 0ul;
        }
        return (unsigned long)ARKRTOS_ARM_FRAME_FPU_BYTES;
    }

    if(avail < (unsigned long)ARKRTOS_ARM_FRAME_BASIC_BYTES)
    {
        return 0ul;
    }
    if(arkrtos_arm_frame_psr_ok(p_words[7]) == 0)
    {
        return 0ul;
    }
    return (unsigned long)ARKRTOS_ARM_FRAME_BASIC_BYTES;
}

#endif /* ARKRTOS_ARM_SWAP_FRAME_H */
