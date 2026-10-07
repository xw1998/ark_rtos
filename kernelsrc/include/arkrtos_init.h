/**
* @file arkrtos_init.h
* @brief Ark RTOS 内核启动初始化接口
* @details 把"内核需要初始化哪些模块、以什么顺序"收敛到唯一一处实现
*          （arkrtos_init.c 的 arkrtos_kernel_module_init），避免出现两份启动流程
*          （内核默认入口与工程 main）各自演化、
*          最终导致某个模块在某个工程下漏初始化的问题。
*          内核不提供 main()：入口由工程 main() 提供并调用本函数。
*
*          工程侧 main() 的推荐启动顺序：
*          1) HAL / 时钟 / 外设初始化（CubeMX 生成部分）
*          2) arkrtos_port_board_init()
*          3) arkrtos_cfg_load()
*          4) arkrtos_register_tasks()（注册本工程的板级任务，可选）
*          5) arkrtos_port_irq_init()
*          6) arkrtos_kernel_module_init()   <-- 本文件提供
*          7) 分区表与外部映像：arkrtos_ptable_init() → arkrtos_loader_scan*()/start*()
*             → arkrtos_installer_init()
*          8) arkrtos_start_idle()（进入空闲任务，启动调度）
*/

#ifndef __ARKRTOS_INIT_H__
#define __ARKRTOS_INIT_H__

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化全部内核模块，并注册内置服务任务
* @details 顺序：事件 → 同步 → 消息队列 → 软定时器 → 故障记录 → 设备 → 板级设备，
*          然后注册软定时器服务任务，最后（需要时）初始化 MPU。
*          必须在 arkrtos_cfg_load() 之后、调度启动之前调用。
* @note 任何启动路径都应调用本函数，不要在工程侧重复逐个调用各模块 init。
*/
void arkrtos_kernel_module_init(void);

#if (defined(__cplusplus))
}
#endif

#endif /* __ARKRTOS_INIT_H__ */
