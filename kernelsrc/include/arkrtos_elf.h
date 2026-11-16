/**
* @file arkrtos_elf.h
* @brief 从 VFS 里的 ELF64 可执行文件直接起一个隔离任务（M1：静态映射档）
* @details 这一层回答的是「文件即程序」：给定一个路径，校验它是本架构的
*          ELF64、把 PT_LOAD 段读进 App 窗口、按 Linux 的约定铺好用户栈
*          （sp 指向 argc），再以降到非特权的任务把它跑起来。
*
*          **与 Linux 的差别必须写在标题上**：M1 用的是一个静态窗口，
*          同一时刻只有一个程序在跑，且所有程序共用同一个地址空间。
*          「每进程一份页表 / 同 VA 多实例 / 缺页」是 M2 的欠账，见
*          docs/MMU进程模型与ELF装载.md。不要把这个接口读成「Ark 有进程了」。
*
*          调用约定（用户态）按 Linux 走，不按 C ABI 传参：
*            sp -> argc, argv[0..argc-1], NULL, envp NULL
*          因此程序的第一条指令不需要内核替它传 a0/a1，任何按 Linux
*          crt 写的静态程序都能直接跑。
*
* @note 开关：ARKRTOS_USE_ELF（默认 0）。关闭时本文件只剩宏，整块零变化。
*       打开时还要求板级给出可执行窗口（ARKRTOS_ELF_WINDOW_BASE/SIZE），
*       缺一个是编译期 #error，不是运行期猜一个地址。
* @author xw
*/

#ifndef __ARKRTOS_ELF_H__
#define __ARKRTOS_ELF_H__

#include "arkrtos_types.h"

#if (ARKRTOS_USE_ELF == 1)

/* 返回码：0 成功，负值失败。名字经 arkrtos_elf_error_name() 换成人话。 */
#define ARKRTOS_ELF_OK              (0)
#define ARKRTOS_ELF_ERR_PARAM       (-1)    /* 空路径 / 空 argv 元素 / argc 越界 */
#define ARKRTOS_ELF_ERR_FS          (-2)    /* VFS 打不开或读不全（这不是格式错误） */
#define ARKRTOS_ELF_ERR_FORMAT      (-3)    /* 不是 ELF64 / 小端 / ET_EXEC / 静态 */
#define ARKRTOS_ELF_ERR_ARCH        (-4)    /* e_machine 不是 LoongArch */
#define ARKRTOS_ELF_ERR_LAYOUT      (-5)    /* 段或入口落在窗口外、覆盖栈、filesz>memsz */
#define ARKRTOS_ELF_ERR_NOMEM       (-6)    /* 任务表满 / 栈参数不合法 */
#define ARKRTOS_ELF_ERR_BUSY        (-7)    /* M1：窗口是单实例，上一个还在跑 */

/** @brief 一次装载的可读结果（全部是实测值，不是推算值） */
typedef struct {
    uint32 entry;           /* e_entry（按窗口内绝对地址） */
    uint32 bytes;           /* 从文件读进窗口的字节数（不含 BSS 清零） */
    uint32 segs;            /* PT_LOAD 个数 */
    uint32 argc;            /* 铺进用户栈的参数个数 */
    uint32 sp;              /* 用户栈初始 sp（指向 argc） */
    uint32 stack_size;      /* 分给该任务的栈字节数（argv 区不在其中） */
    int32  task;            /* 任务号；<=0 表示这次调用失败了 */
} arkrtos_elf_info_t;

/**
* @brief 装载并起跑一个 ELF64 可执行文件
* @param path  VFS 路径（如 "/bin/hello"）
* @param argc  参数个数（含 argv[0]）；0 表示自动补一个 argv[0] = path
* @param argv  参数数组；传 0 时按 argc=0 处理
* @return 0 = 已起跑；负值 = ARKRTOS_ELF_ERR_*
* @note 起跑后立刻返回（不等待程序退出）。要等退出由调用者自己按任务号看。
*/
int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[]);

/** @brief 上一次成功装载的读数；没有过成功装载时返回 ARKRTOS_ELF_ERR_PARAM */
int32 arkrtos_elf_info(arkrtos_elf_info_t *p_out);

/** @brief 1 = 有一个 ELF 任务在跑（M1 单实例），0 = 窗口空着 */
int32 arkrtos_elf_running(void);

/** @brief 返回码的短名（只用于打印，不参与判定） */
const char *arkrtos_elf_error_name(int32 rc);

#else  /* ARKRTOS_USE_ELF == 0 */

/* 关闭档：调用点不必再套 #if，且每个入口都回答「没有这件事」。 */
#define arkrtos_elf_exec(path, argc, argv)      ((int32)(-1))
#define arkrtos_elf_running()                   ((int32)0)
#define arkrtos_elf_info(out)                   ((int32)(-1))
#define arkrtos_elf_error_name(rc)              ("elf support not compiled in")

#endif /* ARKRTOS_USE_ELF */

#endif /* __ARKRTOS_ELF_H__ */
