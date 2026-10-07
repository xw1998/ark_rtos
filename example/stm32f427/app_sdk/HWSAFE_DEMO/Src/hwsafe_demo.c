/**
* @brief Ark RTOS 硬件安全态（"遗言"）机制上板验证镜像（HWSAFE_DEMO）
* @details 这是一个**故意崩溃**的镜像，用来验证：App 合法控制过的硬件，在 App
*          崩溃、任务被故障恢复杀掉之后，由**内核**（而不是崩溃的 App）把它带回
*          安全态。
*
*          流程：
*            1. 打开 led0（PC0）并点亮；
*            2. 用 arkrtos_dev_set_will("led0", 0, 0) 留一句遗言：
*               "我死后请对 led0 执行 drv_ctrl(cmd=0)（关灯）"；
*            3. 故意越权写内核私有 RAM -> MemManage -> 任务被杀；
*            4. 内核在收尸路径执行该遗言 -> LED 被强制关掉。
*
*          验证口径（shell 侧，见 README）：
*            - `hwsafe` 显示 neutralized>=1、last_owner=<本槽任务号>；
*            - `fault` 有 MEMFAULT 记录；
*            - 目视 LED 已灭。
*
*          本镜像**故意不带心跳**，整机是否还活着要在 shell 侧看。
*          探针只跑一次就转长睡（连续 3 次故障会把整槽禁掉）。
*
*          地址是写死的：App 侧不许 include 分区头（布局只能经 SVC 0x18 取得），
*          而这是一个只用来打一下的探针，不是可移植代码。
*/

#include "arkrtos.h"

/* 内核私有 RAM（chip RAM 里 share(8K) 之后、内核自己用的一段）：
 * 它明确不在本 App 的 RAM 窗口里，也不在共享窗口里。 */
#define HWSAFE_DEMO_KERNEL_RAM  ((volatile uint32 *)0x20003000u)

static void hw_puts(int32 con, const char *s)
{
    int32 len = 0;

    while(s[len] != '\0')
    {
        len++;
    }
    if((con >= 0) && (len > 0))
    {
        (void)arkrtos_dev_write(con, (void *)s, len);
    }
}

void AppMain(void)
{
    int32 con;
    int32 led;
    int32 r;

    con = arkrtos_dev_open("ttyS0", 0);

    /* 每次被启动都跑一次探针：内核不会自动重启崩溃的 App，需要复验时由操作者
     * 再 `app start <slot>`（注意同一槽连续 3 次故障会被终局策略 held，用
     * `app start` 可再放行）。 */
    hw_puts(con, "\r\nHWSAFE_DEMO: App-crash hardware safe-state probe\r\n");

    /* 1) 打开 led0 并点亮（cmd 1 = on，见 drvled.h 的 ARKRTOS_LED_CTRL_ON）。 */
    led = arkrtos_dev_open("led0", 0);
    if(led >= 0)
    {
        (void)arkrtos_dev_ctrl(led, 1u, 0u);
        hw_puts(con, "HWSAFE_DEMO: led0 opened and turned ON\r\n");

        /* 2) 留遗言：我死后把 led0 关掉（cmd 0 = off）。内核据此在本任务死亡
         *    时执行 drv_ctrl(obj, 0, 0) —— 不需要本 App 的代码还能被调用。 */
        r = arkrtos_dev_set_will("led0", 0, 0);
        if(r == 0)
        {
            hw_puts(con, "HWSAFE_DEMO: will declared (led0 ctrl=0 off)\r\n");
        }
        else
        {
            hw_puts(con, "HWSAFE_DEMO: will REFUSED (kernel ARKRTOS_HW_SAFE=0?)\r\n");
        }
    }
    else
    {
        hw_puts(con, "HWSAFE_DEMO: led0 open failed\r\n");
    }

    hw_puts(con, "HWSAFE_DEMO: now violating on purpose (write kernel RAM, expect MemManage)\r\n");

    /* 3) 越权写内核 RAM。这一句不会返回。 */
    *HWSAFE_DEMO_KERNEL_RAM = 0xA5A5A5A5u;

    hw_puts(con, "HWSAFE_DEMO: violation NOT blocked - kernel RAM is writable\r\n");

    for(;;)
    {
        arkrtos_task_wait(1000u);
    }
}
