/**
* @brief Ark RTOS 内核启动与初始化（单一权威实现）
* @details 本文件只提供 arkrtos_kernel_module_init()：内核各模块的初始化顺序在此唯一确定。
*          内核【不提供 main() 入口】，入口一律由工程侧 main() 提供——
*          同一个映像里存在两个 main 时，AC6 会因辅助符号 __ARM_use_no_argv
*          重复定义而链接失败（L6200E: by arkrtos_init.o and main.o），
*          即“弱 main 可被工程 main 覆盖”在 AC6 下并不成立。
*
*          应用自定义 main() 时建议遵循以下启动顺序：
*          1) HAL 初始化 + 时钟配置 + 外设初始化（CubeMX 生成部分）
*          2) arkrtos_port_board_init()
*          3) arkrtos_cfg_load()
*          4) arkrtos_port_irq_init()
*          5) arkrtos_kernel_module_init()   各内核模块 + 板级设备 + 内置服务任务
*                                          （唯一权威顺序实现，见本文件下方）
*          6) 注册应用/驱动任务（arkrtos_task_register 等，必须在调度启动前完成）
*          7) arkrtos_start_idle()（进入空闲任务，启动调度）
*
*          需要“设备自安装”能力的工程，在第 5 步之后追加：
*          arkrtos_ptable_init() → arkrtos_loader_scan_driver()/arkrtos_loader_start_driver()
*          → arkrtos_loader_scan()/arkrtos_loader_start() → arkrtos_installer_init()
*          （参考实现：example/stm32f427/kernel/ARKRTOS_TEST/Core/Src/main.c 的 arkrtos_kernel_init）
*
* @author xw
* @date 2026.05.03
*/

#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_event.h"
#include "arkrtos_hal.h"
#include "arkrtos_dev.h"
#include "arkrtos_sync.h"
#include "arkrtos_mq.h"
#include "arkrtos_timer.h"
#include "arkrtos_fault.h"
#include "arkrtos_mpu.h"
#include "arkrtos_config.h"
#include "arkrtos_init.h"

/* 内核模块初始化：唯一权威顺序实现（任何工程的 main 都只调用它） */
void arkrtos_kernel_module_init(void)
{
    arkrtos_event_module_init();
    arkrtos_sync_module_init();
    arkrtos_mq_module_init();
    arkrtos_timer_module_init();
    arkrtos_fault_module_init();
    arkrtos_dev_module_init();
    arkrtos_dev_board_init();

    /* 内置定时器服务任务：须在 cfg_load 之后、调度启动之前注册 */
    arkrtos_timer_task_install();

    #if (ARKRTOS_USE_MPU == 1)
    arkrtos_mpu_module_init();
    #endif
}
