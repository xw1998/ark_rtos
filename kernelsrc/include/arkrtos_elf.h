/**
* @file arkrtos_elf.h
* @brief 从 VFS 里的 ELF64 可执行文件直接起一个隔离任务（M1：静态映射档）
* @details 这一层回答的是「文件即程序」：给定一个路径，校验它是本架构的
*          ELF64、把 PT_LOAD 段读进 App 窗口、按 Linux 的约定铺好用户栈
*          （sp 指向 argc），再以降到非特权的任务把它跑起来。
*
*          动态链接（PT_INTERP）由 ARKRTOS_USE_DYNLOAD 单独控制：打开后
*          按 Linux 的约定先装解释器、把 AT_PHDR/AT_BASE/AT_ENTRY 等 auxv
*          铺在用户栈上、入口跳解释器；关闭时 PT_INTERP 仍按格式错误拒绝。
*          解释器本身（ld.so）是仓库外/示例里的用户态程序，不是内核的一部分。
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
#define ARKRTOS_ELF_ERR_SIGN        (-8)    /* 缺尾部签名 tag / tag 不匹配（EXEC_SIGN 档） */
#define ARKRTOS_ELF_ERR_INTERP      (-9)    /* PT_INTERP 解释器缺失/不可用（DYNLOAD 档） */
#define ARKRTOS_ELF_ERR_QUOTA       (-10)   /* SEC-G07：这个 App 的活任务数已到上限 */

/** @brief 一次装载的可读结果（全部是实测值，不是推算值） */
typedef struct {
    unsigned long entry;    /* e_entry（按窗口内绝对地址） */
    uint32 bytes;           /* 从文件读进窗口的字节数（不含 BSS 清零） */
    uint32 segs;            /* PT_LOAD 个数 */
    uint32 argc;            /* 铺进用户栈的参数个数 */
    unsigned long sp;       /* 用户栈初始 sp（指向 argc） */
    uint32 stack_size;      /* 分给该任务的栈字节数（argv 区不在其中） */
    int32  task;            /* 任务号；<=0 表示这次调用失败了 */
    unsigned long interp_base;  /* 解释器装载基址；0 = 没有解释器（静态档） */
    unsigned long interp_entry; /* 解释器入口（实际起跑的任务入口）；0 = 静态档 */
    uint8  dynamic;         /* 1 = 这次装载走的是 PT_INTERP 动态路径 */
    /* upcall 区（SEC-G08 隔离执行）：内核向本窗口借的一小块，放「让本 App 的
     * shell 回调以 PLV3 在自己空间里跑」的桩与桩栈。三个值都是 0 表示没有这块
     * 区（ARKRTOS_ELF_UPCALL_SIZE=0 的回退档）——不是「地址 0 可用」。 */
    unsigned long upcall_base;  /* 区起点（4 KB 对齐）；0 = 没有 upcall 区 */
    unsigned long stub_va;      /* 桩页入口（= upcall_base）；0 = 没有 */
    unsigned long upcall_sp;    /* 桩栈初始 sp（区顶往下留 16 字节）；0 = 没有 */
#if (ARKRTOS_USE_EXIT_CODE == 1)
    int32  exit_code;       /* U-8：任务上报过的退出码（exit_code_set=0 时不作数） */
    uint8  exit_code_set;   /* 1 = 该实例用 EXIT_CODE 子命令结束过，exit_code 有效 */
#endif
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

/**
* @brief 装进指定地址空间之外，多给一个"落到哪个空间"的出口
* @param p_space 可为 0；成功时写回空间号
* @return 0 = 已起跑；ARKRTOS_ELF_ERR_BUSY = 所有空间都有活实例
* @note 挑空间是装载器的决定，调用方只读结果。U-2（2026-11-17）起，
*       "同一时间只能跑一个程序"变成"每个地址空间一个"。
*/
int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[],
                             uint32 *p_space);

/** @brief 上一次成功装载的读数（最近一次的那个空间）；没有过成功装载时返回 ARKRTOS_ELF_ERR_PARAM */
/**
* @brief 把**当前任务自己**的镜像原地换掉（Linux execve 的内核落地）
* @param path   VFS 路径（内核侧命名空间路径）
* @param argc   参数个数（含 argv[0]）；0 表示补成 path
* @param argv   已由调用方从**旧镜像**拷出的内核侧字符串数组
* @param p_info 成功时填装载读数（task 字段是调用者的任务号）
* @param p_tp   成功时写回新镜像的 TLS 块地址（0 = 这份镜像没有预置 TLS）
* @return 0 / 负错误码
* @details 语义与 Linux 的 execve 对齐到「同一个任务号继续跑新镜像」这一层：
*          - 不起新任务、不换任务号（前后 getpid 相同）；
*          - 旧镜像的段被新镜像覆盖，旧代码不再可达；
*          - fd 表**不清空**：execve 不是「起一条新命令」，文件描述符属于
*            任务而不是属于镜像；
*          - 入口 / 栈指针 / TLS 由调用方写回当前任务的 trap 帧（port 层）。
*          调用者必须**先把 argv 字符串拷进内核**：它们原本躺在旧镜像的
*          参数区，装载一开始那块内存就归新镜像了。
*          成功时不返回旧镜像（Linux 约定），失败返回负错误码、旧镜像继续跑。
*          fork / 地址空间复制**不在**这条接口里。
*/
int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *argv[],
                               arkrtos_elf_info_t *p_info,
                               unsigned long *p_tp);

#if (ARKRTOS_USE_LINUX_FORK == 1)
/**
* @brief 复制**当前任务**，返回子任务的 id 与它落到的空间（U-19）
* @param p_svc_ctx 当前任务这次系统调用的 trap 帧（它同时是子空间里的同一 VA）
* @param p_child_task  可为 0；成功时写回子任务号
* @param p_child_space 可为 0；成功时写回子任务绑定的空间号
* @return ARKRTOS_ELF_OK / ARKRTOS_ELF_ERR_BUSY（没有空空间）/
*         ARKRTOS_ELF_ERR_NOMEM（帧池不够或任务表满）/ ARKRTOS_ELF_ERR_PARAM
* @details 子进程拿到父进程地址空间的**逐字副本**（8 KB 对级拷贝，父空间里
*          已经驻留的对才复制），子任务恢复的现场就是父这次系统调用的 trap
*          帧，只有 a0 被改成 0 —— 于是父返回子 pid、子返回 0。
*          不复制的东西（与 Linux 不同，文档写死）：fd 表、管道、信号表、
*          堆游标都是**内核全局单份**，子看到的是同一份，不是副本。
*          调用者必须是降权（is_priv == 0）用户任务；内核任务没有窗口。
* @note 只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */
int32 arkrtos_elf_fork(void *p_svc_ctx, int32 *p_child_task,
                       uint32 *p_child_space);
#endif

#if (ARKRTOS_USE_LINUX_THREAD == 1)
/**
* @brief 在当前任务的**同一份地址空间**里再起一个任务（U-25：CLONE_THREAD）
* @param p_svc_ctx    当前线程这次系统调用的 trap 帧（模板）
* @param child_stack  子线程的用户栈顶（非 0、16 字节对齐；只给栈顶，不给大小）
* @param child_tls    CLONE_SETTLS 时子线程的 $tp，否则传 0 表示照抄父线程
* @param p_child_task 可为 0；成功时写回子任务号
* @param p_hook       可为 0；子任务**在离开临界区之前**被回调一次，
*                     参数是（父任务号, 子任务号）。Linux ABI 层用它把子线程
*                     绑到父的进程状态槽上 —— 晚一步绑定，子线程第一次进
*                     ABI 会给自己惰性分配一份新槽，于是它有自己的 fd 表，
*                     而这正是线程语义的反面。回调里只许做纯记账（不许
*                     睡眠、不许拷贝用户内存）。
* @return ARKRTOS_ELF_OK / ARKRTOS_ELF_ERR_PARAM / ARKRTOS_ELF_ERR_NOMEM
* @details 与 arkrtos_elf_fork 的差别只有一条，但它是全部差别的来源：子任务
*          绑到**父任务那个空间**，不复制任何帧。于是父子看到的是同一份内存
*          （那正是线程要的），trap 帧落在 child_stack 顶上下一个帧长处。
*          初始返回值 A0 = 0，其余寄存器继承父线程，ERTN 回到父线程系统调用
*          的下一条指令。
*          不管的东西（文档写死）：栈大小不记账（clone 只给栈顶），因此
*          stack_size = 0、栈底保护字不布防；线程栈靠按需缺页长出来，
*          帧所在的那一对由 port 当场要帧并清零。
* @note 只在 ARKRTOS_USE_LINUX_THREAD == 1 时存在。 */
int32 arkrtos_elf_thread(void *p_svc_ctx, unsigned long child_stack,
                         unsigned long child_tls, int32 *p_child_task,
                         void (*p_hook)(int32 parent, int32 child));
#endif

/** @brief 上一次成功装载的读数（最近一次的那个空间）；没有过成功装载时返回 ARKRTOS_ELF_ERR_PARAM */
int32 arkrtos_elf_info(arkrtos_elf_info_t *p_out);

/** @brief 指定空间的那次装载读数 */
int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out);

/** @brief 1 = 指定空间里有活的 ELF 任务，0 = 该空间空着（或没有这个空间） */
int32 arkrtos_elf_running_in(uint32 space);

/** @brief 1 = 至少有一个 ELF 任务在跑，0 = 所有空间都空着 */
int32 arkrtos_elf_running(void);

/** @brief 可用的地址空间数（不分页的构建是 1） */
int32 arkrtos_elf_space_count(void);

/** @brief 最近一次成功装载落到哪个空间；-1 = 还没有过 */
int32 arkrtos_elf_last_space(void);

/** @brief 返回码的短名（只用于打印，不参与判定） */
const char *arkrtos_elf_error_name(int32 rc);

#else  /* ARKRTOS_USE_ELF == 0 */

/* 关闭档：调用点不必再套 #if，且每个入口都回答「没有这件事」。 */
#define arkrtos_elf_exec(path, argc, argv)      ((int32)(-1))
#define arkrtos_elf_exec_space(p, c, v, sp)     ((int32)(-1))
#define arkrtos_elf_running()                   ((int32)0)
#define arkrtos_elf_running_in(space)           ((int32)0)
#define arkrtos_elf_space_count()               ((int32)1)
#define arkrtos_elf_last_space()                ((int32)(-1))
#define arkrtos_elf_exec_replace(p, c, v, i, t)  ((int32)(-1))
#define arkrtos_elf_fork(ctx, t, sp)            ((int32)(-1))
#define arkrtos_elf_thread(ctx, cs, ct, t)      ((int32)(-1))
#define arkrtos_elf_info(out)                   ((int32)(-1))
#define arkrtos_elf_info_space(space, out)      ((int32)(-1))
#define arkrtos_elf_error_name(rc)              ("elf support not compiled in")

#endif /* ARKRTOS_USE_ELF */

#endif /* __ARKRTOS_ELF_H__ */
