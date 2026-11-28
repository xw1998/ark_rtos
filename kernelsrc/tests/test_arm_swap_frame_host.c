/* test_arm_swap_frame_host.c —— Cortex-M 热切换帧形状判据的宿主用例
 *
 * 被测对象是 kernelsrc/port/arm/arkrtos_arm_swap_frame.h 里的纯函数：
 *   arkrtos_arm_swap_frame_bytes(帧指针, 是否 FPU 扩展帧, 还剩多少字节)
 *
 * 它决定两件事，错一件都足以毁掉一个正在跑的任务：
 *   1) 内核按它返回的字节数做「帧是否整个落在旧块栈区里」的边界检查；
 *   2) 端口按它返回的字节数决定 PC / xPSR 在第几个字。
 *
 * 所以这里不只要「正常帧返回 32」，还要钉住四条危险形态：
 *   * FPCA 说扩展帧但内存放不下 104 字节（中断里发的 SVC 走 MSP）——必须
 *     返回 0，绝不越过块尾去读 [25]；
 *   * FPCA 说扩展帧、[25] 却不像 xPSR——返回 0，绝不退回去按基本帧改；
 *   * [7] 像 xPSR 而 [25] 也像 xPSR——形状由 FPCA 定，不是「哪个像用哪个」；
 *   * [7] 是 S4 的位模式（1/512 概率碰巧像 xPSR）——只要 FPCA 说扩展帧，
 *     就不许按基本帧理解。
 *
 * 不证明：真硬件压帧的字节布局（那要靠上板或 QEMU 的 M 核模型）、S16-S31
 * 的软件压栈、异常返回时硬件真的按扩展帧弹栈。
 */

#include <stdio.h>

#include "../port/arm/arkrtos_arm_swap_frame.h"

static int g_fail = 0;
static int g_ck = 0;

static void ck(unsigned long got, unsigned long want, const char *what)
{
    g_ck++;
    if(got != want)
    {
        printf("FAIL %s: got %lu want %lu\n", what, got, want);
        g_fail++;
    }
}

/* 一帧假内存：26 个字够装扩展帧，尾部留几个字让越界读「有内容」而不是崩溃，
 * 这样「必须返回 0 而不是读到块外」的断言才不会因为访问违例变成崩溃。 */
static unsigned long g_frame[32];

static void reset_frame(unsigned long basic_psr, unsigned long ext_psr)
{
    unsigned int i;

    for(i = 0u; i < 32u; i++)
    {
        g_frame[i] = 0xDEADBEEFul;
    }
    g_frame[7] = basic_psr;
    g_frame[ARKRTOS_ARM_FRAME_FPU_PSR_WORD] = ext_psr;
}

int main(void)
{
    const unsigned long GOOD_PSR  = 0x01000000ul;      /* Thumb=1, IPSR=0 */
    const unsigned long NO_THUMB  = 0x00000000ul;
    const unsigned long IN_HANDLER= 0x01000005ul;      /* IPSR=5：不在 Thread 模式 */
    unsigned long n;

    /* 常量自洽：扩展帧的偏移是从 26 个字推出来的，不是另抄一份 */
    ck((unsigned long)ARKRTOS_ARM_FRAME_BASIC_BYTES, 32ul, "basic frame is 8 words");
    ck((unsigned long)ARKRTOS_ARM_FRAME_FPU_WORDS * 4ul,
       (unsigned long)ARKRTOS_ARM_FRAME_FPU_BYTES, "26 words == 104 bytes");
    ck((unsigned long)ARKRTOS_ARM_FRAME_FPU_R0_WORD + 6ul,
       (unsigned long)ARKRTOS_ARM_FRAME_FPU_PC_WORD, "PC sits 6 words after R0");
    ck((unsigned long)ARKRTOS_ARM_FRAME_FPU_R0_WORD + 7ul,
       (unsigned long)ARKRTOS_ARM_FRAME_FPU_PSR_WORD, "xPSR sits after PC");

    /* 空指针：无论怎么问都是「不认识」 */
    ck(arkrtos_arm_swap_frame_bytes(0, 0, 4096ul), 0ul, "null frame basic");
    ck(arkrtos_arm_swap_frame_bytes(0, 1, 4096ul), 0ul, "null frame fpu");

    /* ---- 基本帧 ---- */
    reset_frame(GOOD_PSR, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 32ul), 32ul,
       "basic frame with a legal xPSR");
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 4096ul), 32ul,
       "basic frame with plenty of room still reports 32");

    /* 放不下：块尾只剩 31 字节时连基本帧都装不进 */
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 31ul), 0ul,
       "basic frame refused when only 31 bytes are left");

    reset_frame(NO_THUMB, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 4096ul), 0ul,
       "basic frame without the Thumb bit is refused");

    reset_frame(IN_HANDLER, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 4096ul), 0ul,
       "basic frame with a non-zero IPSR is refused");

    /* ---- FPU 扩展帧 ---- */
    reset_frame(GOOD_PSR, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 104ul), 104ul,
       "fpu frame with a legal xPSR at [25]");
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 4096ul), 104ul,
       "fpu frame with plenty of room still reports 104");

    /* FPCA 说扩展帧、内存却放不下：绝不越过块尾去读 [25] */
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 103ul), 0ul,
       "fpu frame refused when only 103 bytes are left");

    reset_frame(GOOD_PSR, NO_THUMB);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 4096ul), 0ul,
       "fpu frame whose [25] is not an xPSR is refused");

    reset_frame(GOOD_PSR, IN_HANDLER);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 4096ul), 0ul,
       "fpu frame whose [25] is an exception xPSR is refused");

    /* 最危险的一条：两个位置都「像」xPSR，形状必须由 FPCA 定。
     * FPCA=0 → 32（[7] 是 xPSR，[25] 只是栈上的普通数据）；
     * FPCA=1 → 104（[7] 其实是 S4，只是碰巧长得像）。 */
    reset_frame(GOOD_PSR, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 4096ul), 32ul,
       "shape follows FPCA (basic when FPCA is clear)");
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 1, 4096ul), 104ul,
       "shape follows FPCA (extended when FPCA is set)");

    /* FPCA 说基本帧时，即使 [25] 长得像 xPSR 也不许「升级」成扩展帧：
     * 那会把一段普通栈数据当 FPU 现场清零。 */
    reset_frame(NO_THUMB, GOOD_PSR);
    ck(arkrtos_arm_swap_frame_bytes(g_frame, 0, 4096ul), 0ul,
       "a bad [7] is never rescued by a good [25]");

    /* 边界：正好 32 / 正好 104 是放得下的下界 */
    reset_frame(GOOD_PSR, GOOD_PSR);
    n = arkrtos_arm_swap_frame_bytes(g_frame, 0, 32ul);
    ck(n, 32ul, "exactly 32 bytes left is enough for a basic frame");
    n = arkrtos_arm_swap_frame_bytes(g_frame, 1, 104ul);
    ck(n, 104ul, "exactly 104 bytes left is enough for an fpu frame");

    if(g_fail != 0)
    {
        printf("ARMFRAME-HOST FAIL: %d of %d checks failed\n", g_fail, g_ck);
        return 1;
    }
    printf("ARMFRAME-HOST OK (%d checks)\n", g_ck);
    return 0;
}
