/**
* @file arkrtos_linux_abi.h
* @brief Linux 系统调用 ABI 兼容层（带 MMU / MPU 目标上的静态 ELF 程序）
* @details 这里只回答一个问题：一个按 Linux 约定发起系统调用的**静态链接**
*          可执行文件，能不能在 Ark 的 ELF 用户任务里跑起来。答案是"能跑
*          一个最小集合"，不是"支持 Linux"：
*
*            - 支持（asm-generic 号）：openat 56 / close 57 / read 63 /
*              write 64 / exit 93 / exit_group 94 / set_tid_address 96 /
*              clock_gettime 113 / uname 160 / getpid 172 / getuid 174 /
*              geteuid 175 / brk 214 / munmap 215 / mmap 222。
*            - 不支持：动态链接（装载器在 PT_INTERP 处就拒绝）、fork / execve、
*              信号、进程组、fd 继承与管道、socket、真 mmap 语义（页级映射、
*              文件映射、权限位）、futex。未知号一律返回 -ENOSYS 并计数，
*              绝不静默当成功。
*
*          布局差异只在 port 层：LoongArch64 用 `syscall 0` + $a7，RISC-V 用
*          `ecall` + a7；Cortex-M 与 LA32 没有这条 ABI 的载体，端口层直接
*          回答"不是 Linux 调用"。
*
* @note 开关是 ARKRTOS_USE_LINUX_ABI（默认 0）。关档时本文件的宏退化为
*       常量 0，调用点不需要 #if，也不会引入任何符号或体积。
* @author xw
*/

#ifndef __ARKRTOS_LINUX_ABI_H__
#define __ARKRTOS_LINUX_ABI_H__

#include "arkrtos_config.h"

#if (ARKRTOS_USE_LINUX_ABI == 1)

/**
* @brief 在 SVC 入口尝试按 Linux ABI 处理本次调用
* @param p_svc_ctx 系统调用上下文（SVC_Server 收到的那一个）
* @return 1 = 已处理（返回值已写回 a0）；0 = 不是 Linux 调用，交给 Ark 号表
* @details 必须在 Ark 自己的 switch 之前调用。判定完全由 port 层给出，
*          内核不感知寄存器布局；本函数也不会把 Ark 的 0x10..0x22 吃掉。
*/
int32 arkrtos_linux_abi_try(void *p_svc_ctx);

/**
* @brief 读计数器（内核 / shell 用）
* @param which 0 = 已处理，1 = 未知号（-ENOSYS），2 = 用户指针被拒，
*              3 = 其它失败
* @return 计数值
*/
uint32 arkrtos_linux_abi_stat(uint32 which);

/**
* @brief 清空每次 exec 都必须重置的状态（fd 表与堆游标）
* @details 由 ELF 装载器在起跑前调用：fd 表是内核全局的（一次只跑一个
*          程序），上一次留下的句柄不该被下一次继承。
*/
void arkrtos_linux_abi_reset(void);

#else

#define arkrtos_linux_abi_try(p_ctx)     (0)
#define arkrtos_linux_abi_stat(which)    (0u)
#define arkrtos_linux_abi_reset()        ((void)0)

#endif /* ARKRTOS_USE_LINUX_ABI */

#endif /* __ARKRTOS_LINUX_ABI_H__ */