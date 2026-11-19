/**
* @file kernel_stubs.c
* @brief MPS2-AN386 最小档：本档未启用的内核/端口符号的显式失败桩
* @details 本档（board/qemu_mps2_an386/arkrtos_board_config.h）把 WDG /
*          FLASHAPP / SHELL / FS 等全部开关关了，但内核源码里仍有几条无条件
*          引用了这些端口与子系统符号的调用点（arkrtos_loader.c 的 flash
*          擦写、task.c 的 fsmount 与 shell 释放、wdg 喂狗）。不提供符号就
*          链接不过，随便糊一个「返回 0」又会在真被调用时给出错答案。
*
*          所以这里给的是**明确失败**的桩：写 flash 一律返回 -1，挂载返回
*          -1，注册/释放返回 -1，喂狗是真空函数（本档没有看门狗硬件用例）。
*          调用方拿到负值会走错误分支并报错，不会静默当成功。
*
*          这一份不是「漏实现的占位」，而是「本档明确不支持」的声明——将来
*          打开 FS / SHELL / APPRUN 时必须删掉对应桩并写真实现，否则会被本
*          文件开头的编译期断言挡住。文件系统这条不写在这里：FS == 0 时
*          kernelsrc/src/arkrtos_fs.c 自带整套「没有卷」的 facade，属于同一
*          语义的唯一来源，本文件不重复实现。
*
* @author xw
*/

#include "arkrtos_types.h"
#include "arkrtos_config.h"
#include "arkrtos_hal.h"
#include "arkrtos_shell.h"
#include "arkrtos_fs.h"

/* 编译期护栏：一旦将来在这些板级档位里打开了对应功能，本文件必须改写，
 * 不允许「开关开了但实现还是桩」这种假绿状态存在。 */
#if (ARKRTOS_USE_FS != 0) || (ARKRTOS_USE_VFS != 0)
#error "kernel_stubs.c: FS/VFS 已启用，请改用 kernelsrc/src/arkrtos_fs.c 的真实现"
#endif
#if (ARKRTOS_USE_SHELL != 0)
#error "kernel_stubs.c: SHELL 已启用，请删除 arkrtos_shell_* 桩并写实现"
#endif
#if (ARKRTOS_USE_FLASHAPP != 0)
#error "kernel_stubs.c: FLASHAPP 已启用，请删除 arkrtos_port_flash_* 桩并写实现"
#endif

/**
 * @brief flash 擦除桩
 * @return 恒返回 -1（本档没有可写的 flash 驱动）
 */
int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    (void)addr;
    (void)size;
    return -1;
}

/**
 * @brief flash 写入桩
 * @return 恒返回 -1（本档没有可写的 flash 驱动）
 */
int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr;
    (void)data;
    (void)len;
    return -1;
}

/**
 * @brief 喂狗桩
 * @details 本档 WDG 开关为 0，内核即便调用也不该有副作用；空实现即正确行为。
 */
void arkrtos_port_wdg_feed(void)
{
}

/**
 * @brief 用户 shell 扩展命令注册桩
 * @return 恒返回 -1（本档没有 shell 任务）
 */
int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd)
{
    (void)cmd;
    return -1;
}

/**
 * @brief shell 释放任务桩
 * @return 恒返回 -1（本档没有 shell 任务）
 */
int32 arkrtos_shell_release_task(uint32 task_id)
{
    (void)task_id;
    return -1;
}

/**
 * @brief 用户 shell 扩展命令注销桩
 * @return 恒返回 -1（本档没有 shell 任务）
 */
int32 arkrtos_shell_ext_unregister(const char *name)
{
    (void)name;
    return -1;
}

/**
 * @brief shell 输出桩
 * @details 本档没有 shell 任务，任何输出请求都当作失败；不打到串口是为避免
 *          与任务自检输出混在一起造成误读。
 * @return 恒返回 -1
 */
int32 arkrtos_shell_print_n(const char *msg, uint32 len)
{
    (void)msg;
    (void)len;
    return -1;
}
