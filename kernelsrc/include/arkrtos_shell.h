/**
* @file arkrtos_shell.h
* @brief Ark RTOS 内核 Shell 控制台（ark-shell 移植）对外接口
* @details 控制台任务独占 SHELL_DEV_NAME 串口，提供 App/驱动/任务的启停与
*          查询、故障读数、串口安装等人机命令。
*
*          串口归属约定：开启 shell 后不再注册常驻安装任务（见
*          arkrtos_installer.c 的说明），安装改由 `install` 命令触发一次性窗口，
*          在同一任务内串行执行，避免两个读者抢同一个串口 FIFO。
*
* @note 本文件属于内核内部接口。
*/

#ifndef __ARKRTOS_SHELL_H__
#define __ARKRTOS_SHELL_H__

#include "arkrtos_types.h"
#include "arkrtos_ushell.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化并注册内核 Shell 控制台任务
* @return 成功返回任务号（>0）；SHELL_ENABLE 为 0 时返回 0
* @details 需在调度启动之前调用（紧跟 arkrtos_loader_start_autostart*() 之后）。
*/
int32 arkrtos_shell_init(void);

/**
* @brief 取控制台已打开的串口句柄
* @return 设备句柄；尚未打开时返回 -1
* @details 安装窗口复用同一个句柄收镜像，语义上比再 open 一次更清楚。
*/
int32 arkrtos_shell_uart_handle(void);

/**
* @brief 打开控制台串口（已打开则直接返回既有句柄）
* @return 设备句柄；打开失败返回 -1
*/
int32 arkrtos_shell_uart_open(void);

/**
* @brief 往内核控制台注册一条用户态（App / 驱动）命令
* @param cmd 用户态描述符；name / help 会被复制进内核 RAM，
*            所以镜像卸载后不会留下指向已擦除 Flash 的悬垂指针
* @return ARKRTOS_USHELL_OK 或负错误码（见 arkrtos_ushell.h）
* @details 供内核分发层（SVC 0x1A 子命令 1）调用；用户态请用
*          arkrtos_shell_cmd_register()，不要直接调用本函数。
*/
int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd);

/**
* @brief 注销一条用户态命令（按名字，不区分大小写）
* @return ARKRTOS_USHELL_OK 或负错误码
*/
int32 arkrtos_shell_ext_unregister(const char *name);

/**
* @brief Drop every user command registered by one task
* @param task_id owner task id (0 = kernel, nothing to drop)
* @return number of entries released
* @details Hooked into task teardown so an App cannot leave its command name
*          reserved after it is stopped or uninstalled.
*/
int32 arkrtos_shell_release_task(uint32 task_id);

#if (ARKRTOS_SHELL_BOARD_CMDS == 1)
/**
* @brief 板级命令扩展点（板级实现，内核在命令表建好后调用一次）
* @param idx 命令表里第一个空闲槽位
* @return 追加完成后新的表长；没有追加就原样返回 idx
* @details 调用点在 register_kernel_commands() 末尾，也就是 ark_shell_init()
*          重建命令表之后。板级命令只能在这里登记：在那之前写 g_cmd_table
*          会被整表覆盖，现场表现是命令凭空消失（"sdram: command not found"）。
*          返回表长的约定与 arkvim_shell_register()/arkc_shell_register() 一致。
*/
int32 arkrtos_board_shell_register(int32 idx);
#endif

/**
* @brief 向控制台输出一段有界长度的文本（不追加换行）
* @param msg 文本首地址
* @param len 输出长度；由分发层先探测上限，避免在内核侧做无界 strlen
* @return ARKRTOS_USHELL_OK 或负错误码
* @details SVC 处理运行在用户任务的 PSP 上（App 栈 4K、驱动栈 1K），
*          因此这里用内核静态缓冲分段输出，不在调用者栈上开临时数组。
*/
int32 arkrtos_shell_print_n(const char *msg, uint32 len);

/**
* @brief 把一个用户输入的路径解析成命名空间绝对路径
* @param in  用户输入的路径（绝对、相对、~/ 都接受）
* @param out 输出缓冲；解析失败时内容不动
* @param cap 输出缓冲长度
* @return 0=成功；-1=参数非法或解析结果装不下
* @details 相对路径按 shell 当前目录解析，"." 与 ".." 按 Linux 语义消化。
*          存在的理由是「一个 shell 只该有一个 cwd 语义」：组件（如 vim
*          命令）要接受用户给的路径时，必须和提示符下敲同一个路径得到同
*          一个文件，而不是各写一份略有差别的拼接规则。
*          装不下时返回 -1 而不是悄悄截断——截断后的路径会指向另一个文件。
* @note 只在 ARKRTOS_USE_VFS=1 的构建里存在（分辨率依赖 VFS 路径语义）。
*/
int32 arkrtos_shell_resolve_path(const char *in, char *out, uint32 cap);


#if (defined(__cplusplus))
}
#endif

#endif /* __ARKRTOS_SHELL_H__ */
