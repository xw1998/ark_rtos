/**
* @file arkrtos_linux_abi.h
* @brief Linux 系统调用 ABI 兼容层（带 MMU / MPU 目标上的 ELF 程序）
* @details 这里只回答一个问题：一个按 Linux 约定发起系统调用的**静态链接**
*          可执行文件，能不能在 Ark 的 ELF 用户任务里跑起来。答案是"能跑
*          一个最小集合"，不是"支持 Linux"：
*
*            - 支持（asm-generic 号）：openat 56 / close 57 / pipe2 59 /
*              read 63 / write 64 / pread64 67 / exit 93 / exit_group 94 /
*              set_tid_address 96 / clock_gettime 113 / uname 160 /
*              getpid 172 / getuid 174 / geteuid 175 / brk 214 / munmap 215 /
*              execve 221 / mmap 222 / mprotect 226；信号：kill 129 /
*              tkill 130 / tgkill 131 / rt_sigaction 134 /
*              rt_sigprocmask 135 / rt_sigreturn 139。
*            - 不支持：fork / clone、进程组、fd 继承（execve 保留 fd 但不复制
*              地址空间，没有父子）、socket、真 mmap 语义（页级映射、文件映射、
*              权限位）、跨任务信号投递（信号只投给当前任务）、futex。未知号
*              一律返回 -ENOSYS 并计数，绝不静默当成功。
*            - execve 只做「同任务换镜像」：装载失败（格式/架构/布局/签名）
*              旧镜像照常跑并返回负 errno；装载成功则不返回，帧换成新入口。
*            - mprotect 只做区间校验并返回 0，**不强制**：整窗是一块 PS=20 的
*              RWX 映射。它单独计数（arkrtos_linux_abi_stat(4)），读数时能
*              把"接受"与"强制"分开。
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
*              3 = 其它失败，4 = mprotect 接受但未强制
* @return 计数值
*/
uint32 arkrtos_linux_abi_stat(uint32 which);

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
/* 按号直方图的读数口（只在 ARKRTOS_LINUX_ABI_SYSSTAT=1 时存在）。
 *
 * 它回答的是「这个二进制真正调了什么」，与「我们支持什么」是两个问题：
 * 真实发行版二进制卡住的地方，常常是一个我们没实现的号，要先把号拿到，
 * 才轮得上谈实现。
 *
 * 为什么整块放进 #if 而不是「关档时返回 0 的空壳」：空壳也是体积，
 * 默认档因此会多出上百字节目标码，与「默认关 = 零变化」的契约冲突。 */

/**
* @brief 已用槽数（前缀 [0, 返回值) 有效，按首次出现顺序）
*/
uint32 arkrtos_linux_abi_nr_slots(void);

/**
* @brief 读第 idx 个槽
* @param idx   槽下标
* @param p_nr  出参：系统调用号（可为 NULL）
* @return 该号的调用次数；越界返回 0
*/
uint32 arkrtos_linux_abi_nr_slot(uint32 idx, uint32 *p_nr);

/**
* @brief 槽用满之后未记名的调用次数（不静默丢）
*/
uint32 arkrtos_linux_abi_nr_overflow(void);
#endif /* ARKRTOS_LINUX_ABI_SYSSTAT */

/**
* @brief 清空每次 exec 都必须重置的状态（fd 表与堆游标）
* @details 由 ELF 装载器在起跑前调用：fd 表是内核全局的（一次只跑一个
*          程序），上一次留下的句柄不该被下一次继承。
*/
void arkrtos_linux_abi_reset(void);

#else

#define arkrtos_linux_abi_try(p_ctx)     (0)
#define arkrtos_linux_abi_stat(which)    (0u)
#define arkrtos_linux_abi_nr_slots()     (0u)
#define arkrtos_linux_abi_nr_slot(i, p)  (0u)
#define arkrtos_linux_abi_nr_overflow()  (0u)
#define arkrtos_linux_abi_reset()        ((void)0)

#endif /* ARKRTOS_USE_LINUX_ABI */

#endif /* __ARKRTOS_LINUX_ABI_H__ */
