/**
* @file  main.c
* @brief 小程序「正常退出 = 计划内交接」的上板判据实现。
* @details 判据原文（`docs/欠账盘点.md` §一）：
*
*              小程序正常退出后 `hwsafe` 报 handover=1 且设备未被中和
*
*          这条路径和"崩溃遗言"是**相反**的两个动作：
*
*            - 崩溃（fault / 被强杀）：内核必须把死者的硬件驱动到安全态，
*              `neutralized` 递增；
*            - 计划内交接（小程序 main 正常 return，`ARKRTOS_MINI_EXIT_IS_HANDOVER`）：
*              后继者已经接手，此时再去驱动硬件是**毛刺**不是安全 ——
*              内核只摘台账、`handover` 递增、`neutralized` **不动**。
*
*          所以要能看见它，程序必须做到两件事：
*            1. 退出前**仍然持有**一只设备（这里故意不 close）；
*            2. 从 `main` **正常返回**（不是 fault、不是被强杀）。
*
*          板上的取证顺序：
*
*              hwsafe                          # 记下 neutralized / handover 基线
*              mini run /mh.svcm               # 本程序
*              hwsafe                          # handover +1，neutralized 不变，
*                                              # led2 那一行 own 条目消失
*
*          LED 目视：led2 应当**还亮着**（没有被驱动到安全态）。
*/

#include <stdio.h>

#include "arkrtos.h"

int main(void)
{
    int32 led;
    int32 r;
    int   i;

    printf("MINI_HANDOVER: mini starts (planned hand-over probe)\n");

    /* led2 是板级注册的第三只 LED，平时没有所有者（led0/led1 被内核任务占着）。 */
    led = arkrtos_dev_open("led2", 0);
    if (led < 0) {
        printf("MINI_HANDOVER: open led2 failed (%d)\n", (int)led);
        return 1;
    }

    r = arkrtos_dev_ctrl(led, 1u, 0u);          /* ARKRTOS_LED_CTRL_ON = 1 */
    printf("MINI_HANDOVER: led2 handle=%d ctrl(ON)=%d\n", (int)led, (int)r);

    /* 先原地待 3 秒：这段时间里用调试器读 GPIOC_ODR，能看到"灯确实是点亮的"
     * （PC2 = 0）。没有这个窗口就分不清"ctrl 没生效"和"退出时被谁关了" ——
     * 探针在目标运行时读外设会返回假值，只有 halt 后读才是真的。 */
    for (i = 0; i < 3; i++) {
        arkrtos_task_delay(1000000u);           /* 参数是微秒 */
        printf("MINI_HANDOVER: holding, round %d\n", (int)(i + 1));
    }

    /* 故意不 close：台账在任务退出那一刻仍指向本任务，这正是要验的状态。
     * 如果这里先 close，退出时就无台账可摘，handover 不会动，用例也就白跑。 */
    printf("MINI_HANDOVER: returning from main, device still held\n");
    return 0;
}
