/**
* @file arkrtos_linux_abi.c
* @brief Linux 系统调用 ABI 兼容层（静态 ELF 用户任务）
* @details 目标只有一个：让按 Linux 约定写成的**静态链接**程序在 Ark 的
*          PLV3 / U 模式任务里跑起来，而不是让 Ark 变成 Linux。
*
*          实现原则（与全仓其它模块一致）：
*            1) 宁可返回 -ENOSYS / -EFAULT，也不把没做的事说成成功；
*            2) 所有用户指针都先过 arkrtos_copy_from_user / _to_user，
*               绝不把非法指针变成内核里的越权访问或内核自己的 fault；
*            3) 错误以负 errno 返回，并在 64 位端口上做符号扩展
*               （见 arkrtos_port_svc_set_ret_signed）。
*
*          做了一半的事：execve（221）已支持**同任务换镜像**（不起新任务、
*          任务号不变、fd 表保留、堆游标归零）；fork / clone（220）+ wait4（260）
*          由 ARKRTOS_USE_LINUX_FORK 单独提供：子进程拿到父地址空间的逐字
*          副本、父返回 pid、子返回 0。**不复制**的是 fd 表 / 管道 / 信号表 /
*          堆游标（内核全局单份，子看到同一份），wait4 **不能阻塞**（活着就
*          回 -EAGAIN），子进程必须在任务槽位被复用前 reap（TCB 带注册代次，
*          晚到的 reap 如实回 -ECHILD）。线程（CLONE_VM 一族）一律 -EINVAL。
*          管道（pipe2）已做：一条环形缓冲 + 两端引用计数，两个 fd 指向同一条
*          管道。**阻塞语义（U-26）**：读空而写端还在、写满而读端还在时，任务
*          登记在管道上睡一个时间片再重放，对侧一动作就被主动唤醒（读走数据
*          唤醒写端、写入数据唤醒读端、关掉一端唤醒对侧）；O_NONBLOCK 档仍是
*          能读/写多少算多少 + -EAGAIN。读端全关时写回 -EPIPE 并置 SIGPIPE
*          待投递位（Linux 语义：默认动作结束任务，忽略/捕获时只回 -EPIPE）；
*          写端全关读回 EOF。
*          信号（signal）已做**同一任务自我投递**的最小一档：rt_sigaction /
*          rt_sigprocmask / kill / tkill / tgkill / rt_sigreturn，处理函数在
*          用户栈上拿一份**整份 trap 帧快照**（32 B 头 + 256/528/1040 B，
*          帧长由端口报），返回地址指向程序自己给的 restorer；
*          不做跨任务投递、不做实时信号、不做作业控制（SIGSTOP 一类按忽略
*          处理并单独计数）、没有 SIGPIPE 的默认动作。
*          明确不做的事（文档里逐条列出）：fork/clone、跨任务信号、父子 fd
*          继承（没有 fork 就没有第二个进程可继承）、socket、futex，以及真
*          mmap 语义。动态链接的**内核侧**
*          （PT_INTERP + auxv）由 ARKRTOS_USE_DYNLOAD 单独提供，解释器
*          本身是用户态程序；mprotect 接受但不强制（整窗 RWX，单独计数）。
*          mmap/brk 只是
*          在 ELF 窗口内部单调分配（窗口本身是整块静态映射），munmap 只
*          记账不还页——这不是 Linux 的 mmap，程序若依赖页级语义会失败，
*          而不是"看起来成功"。
*
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_LINUX_ABI == 1)

#include "arkrtos_linux_abi.h"
#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif
#include "arkrtos_def.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"   /* fork/wait4 记账要读 TCB 的注册代次 */
#include "arkrtos_mpu.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "arkrtos_board.h"
#include "arkrtos_log.h"   /* write(1/2) 要独占控制台，别让内核日志插进半行 */
#include "arkrtos_elf.h"   /* execve：装载器接口与 ARKRTOS_ELF_ERR_* */
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
#include "arkrtos_net.h"    /* U-23：内核内的网络调用口 arkrtos_net_call */
#endif

/* ---------------------------------------------------------------- 常量 */

/* asm-generic 号表：RISC-V 与 LoongArch 的 Linux UAPI 都用这一套。 */
#define LX_SYS_DUP                  (23u)   /* dup：复制一个 fd 到最小的空号 */
#define LX_SYS_DUP3                 (24u)   /* dup3：复制到一个指定号（可选 O_CLOEXEC） */
#define LX_SYS_FCNTL                (25u)   /* fcntl：F_DUPFD / F_GETFD / F_SETFD / F_GETFL / F_SETFL */
#define LX_SYS_OPENAT               (56u)
#define LX_SYS_CLOSE                (57u)
#define LX_SYS_PIPE2                (59u)
#define LX_SYS_READ                 (63u)
#define LX_SYS_WRITE                (64u)
#define LX_SYS_PREAD64              (67u)
#define LX_SYS_PSELECT6             (72u)
#define LX_SYS_EXIT                 (93u)
#define LX_SYS_EXIT_GROUP           (94u)
#define LX_SYS_SET_TID_ADDRESS      (96u)
#define LX_SYS_SET_ROBUST_LIST      (99u)   /* asm-generic：robust futex 链头 */
#define LX_SYS_GET_ROBUST_LIST      (100u)
#define LX_SYS_CLOCK_GETTIME        (113u)
#define LX_SYS_UNAME                (160u)
/* rseq / getcpu：都是 asm-generic 号（RISC-V 与 LoongArch 共用）。rseq 是可
 * 重启序列的注册面，getcpu 是它的慢路径；两者给出的 CPU 号必须一致。 */
#define LX_SYS_GETCPU               (168u)
#define LX_SYS_RSEQ                 (293u)
#define LX_SYS_GETPID               (172u)
#define LX_SYS_GETUID               (174u)
#define LX_SYS_GETEUID              (175u)
#define LX_SYS_GETTID               (178u)  /* U-25：asm-generic 表（LA64 用这张）；
                                             * 186 是 x86_64 的，别抄错 */
#define LX_SYS_BRK                  (214u)
#define LX_SYS_MUNMAP               (215u)
#define LX_SYS_EXECVE               (221u)
#define LX_SYS_MMAP                 (222u)
#define LX_SYS_MPROTECT             (226u)
#define LX_SYS_MSYNC                (227u)  /* U-21：只在 ARKRTOS_USE_PAGING_FILEMAP */
#define LX_SYS_CLONE                (220u)  /* U-19：只在 ARKRTOS_USE_LINUX_FORK */
#define LX_SYS_WAIT4                (260u)  /* U-19：同上 */

/* 信号相关的号：rt_sigaction / rt_sigprocmask / kill / tkill / tgkill /
 * rt_sigreturn。asm-generic 编号，LoongArch 与 RISC-V 共用。 */
#define LX_SYS_KILL                 (129u)
#define LX_SYS_TKILL                (130u)
#define LX_SYS_TGKILL               (131u)
#define LX_SYS_RT_SIGACTION         (134u)
#define LX_SYS_RT_SIGPROCMASK       (135u)
#define LX_SYS_RT_SIGRETURN         (139u)

/* futex：asm-generic 98。落 WAIT / WAKE / REQUEUE / CMP_REQUEUE /
 * WAIT_BITSET / WAKE_BITSET 六档；PI 与 FD 类 op 如实 -ENOSYS。 */
#define LX_SYS_FUTEX                (98u)

/* socket 族（U-23）。asm-generic 号表，LoongArch 与 RISC-V 共用；只在
 * ARKRTOS_USE_LINUX_SOCKET 档参与编译。send/recv 没有独立的号：glibc 把它们
 * 编译成 sendto/recvfrom（地址指针传 NULL），所以这里也不单列，由 read/write
 * 与 sendto/recvfrom 两路口承接。 */
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
#define LX_SYS_SOCKET               (198u)
#define LX_SYS_BIND                 (200u)
#define LX_SYS_LISTEN               (201u)
#define LX_SYS_ACCEPT               (202u)
#define LX_SYS_CONNECT              (203u)
#define LX_SYS_GETSOCKNAME          (204u)
#define LX_SYS_GETPEERNAME          (205u)
#define LX_SYS_SENDTO               (206u)
#define LX_SYS_RECVFROM             (207u)
#define LX_SYS_SETSOCKOPT           (208u)
#define LX_SYS_GETSOCKOPT           (209u)
#define LX_SYS_SHUTDOWN             (210u)
#define LX_SYS_ACCEPT4              (242u)
#endif


/* U-28 调度策略：asm-generic 号表，LoongArch 与 RISC-V 共用。
 * 只在 ARKRTOS_USE_LINUX_SCHED 档参与编译；关档时这些号落到 default 的
 * -ENOSYS，与打开本档之前逐位相同。 */
#if (ARKRTOS_USE_LINUX_SCHED == 1)
#define LX_SYS_SCHED_SETPARAM       (118u)
#define LX_SYS_SCHED_SETSCHEDULER   (119u)
#define LX_SYS_SCHED_GETSCHEDULER   (120u)
#define LX_SYS_SCHED_GETPARAM       (121u)
#define LX_SYS_SCHED_SETAFFINITY    (122u)
#define LX_SYS_SCHED_GETAFFINITY    (123u)
#define LX_SYS_SCHED_YIELD          (124u)
#define LX_SYS_SCHED_GET_PRIORITY_MAX (125u)
#define LX_SYS_SCHED_GET_PRIORITY_MIN (126u)
#define LX_SYS_SCHED_RR_GET_INTERVAL (127u)
#define LX_SYS_SCHED_SETATTR        (274u)
#define LX_SYS_SCHED_GETATTR        (275u)
#endif

/* errno（asm-generic 编号，程序侧读到的是 -errno） */
#define LX_EPERM                    (1)
#define LX_ESRCH                    (3)
#define LX_EINTR                    (4)
#define LX_ENOENT                   (2)
#define LX_EIO                      (5)
#define LX_E2BIG                    (7)
#define LX_ENOEXEC                  (8)
#define LX_EBADF                    (9)
#define LX_ECHILD                   (10)
#define LX_EAGAIN                   (11)
#define LX_EACCES                   (13)
#define LX_ENOMEM                   (12)
#define LX_EFAULT                   (14)
#define LX_EBUSY                    (16)
#define LX_EISDIR                   (21)
#define LX_EINVAL                   (22)
#define LX_ENFILE                   (23)
#define LX_EPIPE                    (32)
#define LX_EROFS                    (30)
#define LX_ENAMETOOLONG             (36)
#define LX_ENOLCK                   (37)
#define LX_ENOSYS                   (38)
#define LX_ETIMEDOUT                (110)
#define LX_EDEADLK                  (35)
/* socket 族专有的几个 errno（asm-generic 编号）。 */
#define LX_EMFILE                   (24)
#define LX_ENOTSOCK                 (88)
#define LX_EMSGSIZE                 (90)
#define LX_EPROTOTYPE               (91)
#define LX_ENOPROTOOPT              (92)
#define LX_EPROTONOSUPPORT          (93)
#define LX_ESOCKTNOSUPPORT          (94)
#define LX_EOPNOTSUPP               (95)
#define LX_EAFNOSUPPORT             (97)
#define LX_ECONNABORTED             (103)
#define LX_ECONNRESET               (104)
#define LX_ENOBUFS                  (105)
#define LX_EISCONN                  (106)
#define LX_ENOTCONN                 (107)
#define LX_ECONNREFUSED             (111)
#define LX_EHOSTUNREACH             (113)
#define LX_EINPROGRESS              (115)
#define LX_EALREADY                 (114)

/* 每个任务可见的 fd 上限；0/1/2 是标准流，不占表项。 */
#define LX_MAX_FD                   (8u)

/* 阻塞等待的一个时间片（ms）。两处用：
 *   1) wait4：子进程正常 exit 时由退出钩子立即唤醒，这个上限只在「子进程
 *      已经不在了但没人来唤醒」时兜底（例如故障路径把子任务禁用掉）；
 *   2) futex WAIT：本内核没有无限睡眠，等待按时间片重放，醒来后值没变就
 *      继续睡（不编 -ETIMEDOUT，与 Linux 不传 timeout 一致）。
 * 有界而不是无限：无限等待在那些情形下就是死锁。 */
#define LX_WAIT_SLICE_MS            (100u)

#if (ARKRTOS_USE_LINUX_FORK == 1)
/* fork 的记账上限：同时「生出来了但还没被 reap」的子进程个数。满了就
 * -EAGAIN（在真正 fork 之前判），不是生完再丢记录。 */
#define LX_MAX_CHILD                (4u)
#define LX_SIGCHLD_FLAG             (17u)

/* clone(2) 的旗标位（asm-generic 的 linux/sched.h）。只有 U-25 那一档认其中
 * 一部分；其余位一旦出现就 -EINVAL，绝不当成「差不多的旗标」放过去 ——
 * 一个被忽略的旗标会让程序以为拿到了它要的语义（典型是 CLONE_VFORK）。 */
#define LX_CLONE_VM                 (0x00000100u)
#define LX_CLONE_FS                 (0x00000200u)
#define LX_CLONE_FILES              (0x00000400u)
#define LX_CLONE_SIGHAND            (0x00000800u)
#define LX_CLONE_VFORK              (0x00004000u)
#define LX_CLONE_THREAD             (0x00010000u)
#define LX_CLONE_NEWNS              (0x00020000u)
#define LX_CLONE_SETTLS             (0x00080000u)
#define LX_CLONE_PARENT_SETTID      (0x00100000u)
#define LX_CLONE_CHILD_CLEARTID     (0x00200000u)
#define LX_CLONE_CHILD_SETTID       (0x01000000u)
/* 线程档接受的位（其余一律 -EINVAL）：VM / THREAD 是必需的，FS / FILES /
 * SIGHAND 在本内核里本来就是共享的（进程状态槽只有一份），所以接受；
 * SETTLS / 两个 SETTID / CLEARTID 真的实现了。 */
#define LX_CLONE_THREAD_OK  (LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | \
                             LX_CLONE_SIGHAND | LX_CLONE_THREAD | \
                             LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID | \
                             LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID)
#define LX_WNOHANG                  (1u)
#endif
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
/* futex 落的六档 op。FUTEX_PRIVATE_FLAG 接受但本实现不区分（键已经落在
 * 物理帧上，进程私有与共享走同一条查表路径）；FUTEX_CLOCK_REALTIME 只对
 * WAIT_BITSET 有意义，且本机没有墙钟，带 timeout 时如实 -ENOSYS。 */
#define LX_FUTEX_WAIT               (0u)
#define LX_FUTEX_WAKE               (1u)
#define LX_FUTEX_REQUEUE            (3u)
#define LX_FUTEX_CMP_REQUEUE        (4u)
#define LX_FUTEX_WAIT_BITSET        (9u)
#define LX_FUTEX_WAKE_BITSET        (10u)
#define LX_FUTEX_PRIVATE_FLAG       (128u)
#define LX_FUTEX_CLOCK_REALTIME     (256u)
/* 「不按位筛选」：普通 WAIT 登记的就是这档，普通 WAKE 也等于全选。 */
#define LX_FUTEX_BITSET_ALL         (0xFFFFFFFFu)
/* PI 类 op（6/7/8）。字布局与 Linux 逐位相同：低 30 位是持有者 tid，
 * bit31 = 队列里有人等，bit30 = 原持有者已死（用户态据此报 EOWNERDEAD）。 */
#define LX_FUTEX_LOCK_PI            (6u)
#define LX_FUTEX_UNLOCK_PI          (7u)
#define LX_FUTEX_TRYLOCK_PI         (8u)
/* PI 条件变量那一对（11/12）：先按普通 futex 等待（uaddr 上是「信号字」），
 * 收到 CMP_REQUEUE_PI 时整队搬到一个**已持有的** PI 锁（uaddr2）上，排头
 * 那个直接接手锁。号与 Linux 一致；语义偏差见文档 §8.4b。 */
#define LX_FUTEX_WAIT_REQUEUE_PI    (11u)
#define LX_FUTEX_CMP_REQUEUE_PI     (12u)
#define LX_FUTEX_TID_MASK           (0x3FFFFFFFu)
#define LX_FUTEX_WAITERS            (0x80000000u)
#define LX_FUTEX_OWNER_DIED         (0x40000000u)
/* robust list 的条数上限：与 Linux 的 ROBUST_LIST_LIMIT 同口径。链表被用户
 * 写坏（自环）时退出路径照样要能走完，不能在里面转不出来。 */
#define LX_ROBUST_LIST_LIMIT        (2048)
/* struct robust_list_head 是三个机器字（list / futex_offset /
 * list_op_pending）。长度按 sizeof(void *) 算，32 位与 64 位端口各自正确。 */
#define LX_ROBUST_HEAD_MIN          ((uint32)(sizeof(void *) * 3u))
#endif

#if (ARKRTOS_USE_LINUX_SELECT == 1)
/* pselect6 落点的最大描述符号数：0=stdin / 1=stdout / 2=stderr，
 * 3..(3+LX_MAX_FD-1) 走本进程 fd 表。任何 >= 本值的描述符被选中都回
 * -EBADF（本层根本没有那个对象）。FD_SETSIZE 按 Linux 默认 1024 位核，
 * 一张 fd_set 最多 128 字节；这里按 32 位字读取，不把整张搬上栈。 */
#define LX_SEL_NFD                  (LX_MAX_FD + 3u)
#define LX_SEL_SETSIZE              (1024u)
#endif

#if (ARKRTOS_USE_LINUX_SOCKET == 1)
/* socket 族常量（U-23）。家族只认 AF_INET；type 的低位是 STREAM/DGRAM，高位
 * 只认 Linux 的两个附加位，其余位如实 -EINVAL（Linux 在 __sys_socket 里就是
 * 这么判的，不是悄悄忽略）。
 *
 * SOL_SOCKET 的选项号是 **Linux 的编号**，与 net 服务自己的编号不同，必须在
 * 这里逐条翻译：Linux 的 2/7/8 分别是 net 的 1/3/2，而 SO_ERROR 在 net 那边
 * 有独立的 4 号。翻译漏一条就是一个「设置成功、行为没变」的假成功。 */
#define LX_AF_INET                  (2)
#define LX_SOCK_STREAM              (1)
#define LX_SOCK_DGRAM               (2)
#define LX_SOCK_NONBLOCK            (0x800u)
#define LX_SOCK_CLOEXEC             (0x80000u)
/* Linux 的 type 上界（net/socket.c 的 SOCK_MAX）：type >= 它回 -EINVAL，
 * 在范围内但本协议族不支持的 type 才回 -ESOCKTNOSUPPORT。 */
#define LX_SOCK_MAX                 (15)
#define LX_SOCKF_NONBLOCK           (1u)
#define LX_SOCKF_CLOEXEC            (2u)
#define LX_SOL_SOCKET               (1)
#define LX_SO_REUSEADDR             (2)
#define LX_SO_TYPE                  (3)
#define LX_SO_ERROR                 (4)
#define LX_SO_SNDBUF                (7)
#define LX_SO_RCVBUF                (8)
#define LX_SO_DOMAIN                (39)
#define LX_MSG_DONTWAIT             (0x40u)
#endif


#define LX_PATH_MAX                 (64u)
#define LX_IO_CHUNK                 (128u)

/* fd 的两类用途。文件类 fd 的 path/size/off 有效；管道类 fd 只用一个
 * 管道下标，读写都落到那块环形缓冲上。 */
#define LX_FD_FILE                  (0u)
#define LX_FD_PIPE_R                (1u)
#define LX_FD_PIPE_W                (2u)
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
#define LX_FD_SOCKET                (3u)    /* U-23：net 服务里的一格 socket */
#endif
/* dup(1)/dup(2) 保存下来的「控制台引用」。0/1/2 本身不占表项（没有真句柄），
 * 只有被 dup 到一个实 fd 号上时才需要一格占位：读回 EOF、写回控制台、关掉
 * 什么也不做。kind 值不放进 SOCKET 的 #if 里：dup 在任何档都要能编。 */
#define LX_FD_CONSOLE               (4u)

/* 管道容量与同时存在的条数。256 字节够一次 read/write 的常见块，也不至于
 * 让 .bss 多出一大块；条数按「一个程序同时用两条管道」给。
 * 容量比 Linux 的 64 KB 小得多：**超过 256 字节的写一定分段**——阻塞写分段
 * 等空间、非阻塞写返回短写。这一条在文档里写死，不假装一次原子写完。 */
#define LX_PIPE_MAX                 (2u)
#define LX_PIPE_BUF                 (256u)

/* ---------------------------------------------------------------- 信号 */

/* 支持 1..31 号（bit n-1 对应信号 n）。Linux 的实时信号 32..64 不做。 */
#define LX_NSIG                     (32u)
#define LX_SIGKILL                  (9u)
#define LX_SIGSTOP                  (19u)
#define LX_SIG_DFL                  (0UL)
#define LX_SIG_IGN                  (1UL)
#define LX_SIG_BLOCK                (0u)
#define LX_SIG_UNBLOCK              (1u)
#define LX_SIG_SETMASK              (2u)
#define LX_SIGSET_BYTES             (8u)

/* 信号帧铺在用户栈上：4 个 8 字节字的头 + **整份 trap 帧**的快照，16 字节
 * 对齐。magic 是第一道校验：rt_sigreturn 只认自己铺的帧，栈被改坏就回
 * -EFAULT。头里第三个字是帧长，第四个是快照字节数——帧长随 port 变
 * （带 FPU 528、LSX 1040），所以不能在两头各写一个常量。
 *
 * 为什么快照是整帧而不只是 era/sp/ra/tp：处理函数是普通 C 函数，会把
 * $t0 一类的临时寄存器改成自己的东西；只恢复四个寄存器的话，被打断的
 * 调用约定就断了（实测过：恢复后一条 ld.w 用了一个已经变成 2 的基址）。 */
#define LX_SIGFRAME_MAGIC           ((unsigned long)0x53494746524D4532UL)
/* 显式转换成 unsigned long：32 位端口上这个常量放不进一个字，隐式截断会让
 * 编译器对 `s_sigframe[0] != LX_SIGFRAME_MAGIC` 做出错误的常量折叠（宿
 * 主 32 位 gcc 实测：整个比较被折成恒真，magic 校验与后面的 import 一起
 * 被删掉）。magic 只在铺帧与收回之间自证，截断与否两侧一致，值本身不是
 * ABI（帧长与字段语义才是）；写在这里，不留给编译器猜。 */
#define LX_SIGFRAME_HDR_WORDS       (4u)
#define LX_SIGFRAME_HDR_BYTES       ((uint32)LX_SIGFRAME_HDR_WORDS * 8u)
/* 快照缓冲的上限（字）：LSX 档 130 字 = 1040 字节是当前最大的一档。
 * 端口报出比它还大的帧时**不投递**并把 pending 位留着，绝不截断快照。 */
#define LX_SIGFRAME_MAX_WORDS       (130u)

/* port 原语里语义编号：0 = era、1 = sp、2 = ra、3 = tp。 */
#define LX_UREG_ERA                 (0u)
#define LX_UREG_SP                  (1u)
#define LX_UREG_RA                  (2u)
#define LX_UREG_TP                  (3u)

/* execve 的暂存区：旧镜像一旦被覆盖，用户地址上的 argv 就没了，所以
 * 这些字符串必须在装载**之前**落进内核。16 个参数 × 256 字节与装载器
 * 自己的上限（ARKRTOS_ELF_MAX_ARGS / MAX_ARG_LEN）同档，超了回 -E2BIG。 */
#define LX_EXEC_ARGV_MAX            (16u)
#define LX_EXEC_ARG_LEN             (256u)
#define LX_EXEC_ARGV_BYTES          ((uint32)LX_EXEC_ARGV_MAX * (uint32)LX_EXEC_ARG_LEN)

/* openat 标志位（asm-generic） */
#define LX_O_ACCMODE                (0x3u)
#define LX_O_CREAT                  (0x40u)
#define LX_O_TRUNC                  (0x200u)
#define LX_O_APPEND                 (0x400u)
#define LX_O_NONBLOCK               (0x800u)
#define LX_O_ASYNC                  (0x2000u)   /* U-29：SIGIO 通知开关位（F_SETFL 认它） */
#define LX_O_CLOEXEC                (0x80000u)   /* openat / dup3 / pipe2 / socket 都记，exec 时据此还槽 */
/* U-21 唯一放开的访问模式：打开**已存在**的文件时 O_RDWR 合法（可写共享映射
 * 的前提）。O_WRONLY 不放开：只写映射的语义要整文件回填，本档没有。 */
#define LX_O_RDWR                   (0x2u)
#define LX_O_WRONLY                 (0x1u)
#define LX_O_RDONLY                 (0x0u)

/* fcntl(25) 认的 cmd 与 FD_CLOEXEC 位（asm-generic）。只做这五个：F_DUPFD /
 * F_DUPFD_CLOEXEC / F_GETFD / F_SETFD / F_GETFL / F_SETFL。其余（记录锁、租约、
 * F_GETOWN 一类）本层没有对应实体，一律 -EINVAL —— Linux 对不认识的 cmd 也是
 * 这么答的，不给一个"看似的成功"。 */
#define LX_F_DUPFD                  (0u)
#define LX_F_GETFD                  (1u)
#define LX_F_SETFD                  (2u)
#define LX_F_GETFL                  (3u)
#define LX_F_SETFL                  (4u)
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
/* U-29 两条（asm-generic）：登记 / 读回 SIGIO 的主人。开关关着时不定义，
 * 于是它们落到 default 分支回 -EINVAL —— 与本开关出现之前的行为一致。 */
#define LX_F_SETOWN                 (8u)
#define LX_F_GETOWN                 (9u)
#endif
#define LX_F_DUPFD_CLOEXEC          (1030u)

/* 记录锁（POSIX fcntl 的第 5/6/7 档）与锁类型（asm-generic）。 */
#define LX_F_GETLK                  (5u)
#define LX_F_SETLK                  (6u)
#define LX_F_SETLKW                 (7u)
#define LX_RDLCK                    (0u)
#define LX_WRLCK                    (1u)
#define LX_UNLCK                    (2u)
#define LX_FD_CLOEXEC               (1u)

/* mmap 标志位（asm-generic） */
#define LX_MAP_SHARED               (0x01u)
#define LX_MAP_PRIVATE              (0x02u)
#define LX_MAP_FIXED                (0x10u)
#define LX_MAP_ANONYMOUS            (0x20u)

/* mmap prot（asm-generic）。本档只认 READ 与 WRITE：PROT_EXEC 在 LA64 的
 * 这一套条目里没有 NR/NX 位可设（见文档「执行位无法在硬件上强制」），给了
 * 也不假装强制，因此**接受但不记账**，与 mprotect 那条路同一口径。 */
#define LX_PROT_READ                (0x1u)
#define LX_PROT_WRITE               (0x2u)

/* msync flags（asm-generic） */
#define LX_MS_ASYNC                 (1u)
#define LX_MS_INVALIDATE            (2u)
#define LX_MS_SYNC                  (4u)

/* 文件映射记账与写回上限（U-21）。
 *   - 每个进程最多 LX_FILEMAP_MAX 条「可写共享映射」的写回记账：真正需要
 *     写回的就是这一种，私有映射与被写保护的不必记账；
 *   - 写回要整文件进内核静态缓冲，所以文件大于 LX_FILEMAP_WB_MAX 时 mmap
 *     当场 -ENOMEM —— 不给一个「后面只写得回一半」的假承诺。 */
#define LX_FILEMAP_MAX              (2u)
#define LX_FILEMAP_WB_MAX           (8192u)

/* clockid */
#define LX_CLOCK_REALTIME           (0u)
#define LX_CLOCK_MONOTONIC          (1u)
#define LX_CLOCK_MONOTONIC_RAW      (4u)

/* 计数器下标 */
#define LX_STAT_HANDLED             (0u)
#define LX_STAT_ENOSYS              (1u)
#define LX_STAT_EFAULT              (2u)
#define LX_STAT_OTHER               (3u)
#define LX_STAT_MPROTECT            (4u)    /* mprotect 调用次数（关档接受不强制，开档真改页） */
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
#define LX_STAT_FILEMAP             (5u)    /* 真的写回过几次（U-21） */
#define LX_STAT_MAX                 (6u)
#else
#define LX_STAT_MAX                 (5u)
#endif

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
/* 按号直方图的槽数。16 个不同号已经足够描出一个 hello 世界级别的
 * 程序要什么（glibc 的 hello 会用到 10 个上下，动态装载器多几个）；
 * 超出只计溢出，不静默丢。 */
#define LX_NR_SLOTS                 (16u)
#endif

/* ---------------------------------------------------------------- 状态 */

typedef struct
{
    uint8  used;
    uint8  kind;        /* LX_FD_FILE / LX_FD_PIPE_R / LX_FD_PIPE_W /
                         * LX_FD_SOCKET / LX_FD_CONSOLE */
    uint8  pipe;        /* kind 是管道时，s_pipe 的下标 */
    uint8  wr;          /* 1 = 以 O_RDWR 打开（U-21 可写共享映射的前提） */
    uint8  cloexec;     /* FD_CLOEXEC：execve 换镜像之后这一槽要还回去 */
    uint16 fl;          /* 文件状态标志：O_ACCMODE + O_APPEND + O_NONBLOCK。
                         * F_GETFL 回的就是它，F_SETFL 只改后两位 */
    uint32 size;
    uint32 off;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    /* U-29：O_ASYNC 通知的主人（任务号，0 = 没有主人）。存任务号而不是
     * 进程槽号：槽会被 fork/exec 复用，任务号不会。 */
    int32  owner;
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* U-23：socket 类 fd 用到的四格。net 是 net 服务里那一格的句柄（1 起）；
     * stype 是建的时候给的 SOCK_STREAM/SOCK_DGRAM（getsockopt(SO_TYPE) 与
     * accept 的继承都要它）；sflags 是本层的 NONBLOCK/CLOEXEC 位；
     * pok/pbits 是 pselect6 一轮内的就绪答复缓存（见 lx_sock_net_ready）。 */
    uint8  net;
    uint8  stype;
    uint8  sflags;
    uint8  pok;
    uint8  pbits;
#endif
    char   path[LX_PATH_MAX];
} lx_fd_t;

typedef struct
{
    uint8  used;
    uint8  readers;     /* 打开的读端个数；到 0 时写端得到 -EPIPE */
    uint8  writers;     /* 打开的写端个数；到 0 时读端读到 EOF */
    uint16 head;
    uint16 tail;
    uint16 count;
    uint8  buf[LX_PIPE_BUF];
} lx_pipe_t;

/* 阻塞等待登记（每个任务最多一处，U-26）。**不放任务自己的进程槽里**：
 * 等待者的身份是任务，不是进程——fork 出来的子进程有自己的槽，但一次
 * 阻塞的是它自己这条执行流。
 *
 * 为什么必须留 done：阻塞写超过 256 字节时要分段等空间，重放那一轮不能
 * 从用户缓冲的第一字节重来（那会把已经写进管道的前半段再写一遍）。 */
typedef struct
{
    uint8  used;        /* 1 = 这个任务正挂在一处管道等待上 */
    uint8  dir;         /* 0 = 读端在等，1 = 写端在等 */
    uint8  pi;          /* s_pipe 的下标 */
    uint8  pad;
    uint32 done;        /* 写：已经写进去的字节数；读：恒 0 */
} lx_pipe_wait_t;

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
/* 一条「可写共享文件映射」的写回记账（U-21）。只记需要写回的那一种：
 * 私有映射改的是自己那份帧，没有可写回的东西；写保护的也不可能有脏页。
 * addr/len/off/size 都是 mmap 那一刻的值，写回时按 off 之前的文件内容 + 这段
 * 映射里的内容重新拼出整个文件（VFS 没有 write_at）。 */
typedef struct
{
    uint8  used;
    uint32 addr;
    uint32 len;
    uint32 off;
    uint32 size;
    char   path[LX_PATH_MAX];
} lx_filemap_t;
#endif

/* fd 表是内核全局的：本版本的 ELF 装载器一次只跑一个程序，装载器在
 * 每次起跑前调用 arkrtos_linux_abi_reset() 把它清空。 */
#if (ARKRTOS_USE_LINUX_FORK == 1)
/* 子进程记账（U-19）：pid 就是任务号；generation 是 TCB 的注册代次，
 * 用来把「同一个任务号上的新旧两个实例」分开 —— 槽位会被复用，只比 pid
 * 会认错人。这里同样是**内核全局单份**（与 fd 表同一条取舍）。 */
typedef struct
{
    uint8  used;
    int32  pid;
    uint32 generation;
    int32  parent;      /* 发起 fork 的父进程任务号（wait4 只认自己的儿子） */
} lx_child_t;

static lx_child_t s_child[LX_MAX_CHILD];
#endif
/* 管道对象独立于 fd 表：一条管道被两个 fd 引用，fd 关掉一半时管道还要在。 */
static lx_pipe_t s_pipe[LX_PIPE_MAX];
/* 每个任务那一处管道等待（按任务号索引）。任务号会被复用：退出路径上要
 * 清掉，否则新任务第一次碰管道就走了「重放」分支。 */
static lx_pipe_wait_t s_pipe_wait[ARKRTOS_TASK_MAX_NUM];

/* 定义在信号段，管道写端无人读时要置 SIGPIPE 待投递位（见 lx_sig_raise）。 */
static uint32 lx_sig_bit(uint32 sig);
static uint32 lx_sig_default_ignored(uint32 sig);
static void   lx_sig_raise(uint32 sig);
static uint32  s_stat[LX_STAT_MAX];

#if (ARKRTOS_USE_LINUX_FUTEX == 1)
/* 每个任务最多一次 futex WAIT：按任务号索引，不按进程槽（槽会随 fork 复制，
 * 等待者身份必须是任务本身）。key 是「物理帧 + 页内偏移」，所以父与子把同一
 * 张共享页上的同一个字拿到同一个键上。无效键记 ~0UL，不用 0 当哨兵
 * （0 是一个合法的物理地址）。 */
static unsigned long s_futex_key[ARKRTOS_TASK_MAX_NUM];
static uint8   s_futex_used[ARKRTOS_TASK_MAX_NUM];
static uint8   s_futex_woke[ARKRTOS_TASK_MAX_NUM];
/* 用户地址与期望值也必须登记：阻塞返回时 a0（返回值寄存器就是第一个参数
 * 寄存器）被清成 0，重放的那一次读到 uaddr == 0、val 仍是原值。没有这两格
 * 就只能拿 0 去读字（必然 -EFAULT），或者信任一个不该信任的参数。存下来，
 * 重放时用「上一次真正睡下时的地址和值」继续判。 */
static unsigned long s_futex_uaddr[ARKRTOS_TASK_MAX_NUM];
static uint32  s_futex_val[ARKRTOS_TASK_MAX_NUM];
/* WAIT_BITSET 的位集与截止时刻。截止时刻是内核毫秒坐标上的绝对值（0 表示
 * 没给 timeout）；timed 单独一格而不是拿 0 当哨兵，因为开机瞬间的绝对
 * 截止时刻真的可能是 0。 */
static uint32  s_futex_bitset[ARKRTOS_TASK_MAX_NUM];
static uint32  s_futex_deadline[ARKRTOS_TASK_MAX_NUM];
static uint8   s_futex_timed[ARKRTOS_TASK_MAX_NUM];
/* PI 等待（LOCK_PI 拿不到时登记的那一槽）。pi_owner 是「登记时字里的持有者
 * tid」：抬优先级与交接都要按持有者算，而重算时不能去读别人的地址空间
 * （跨进程的等待者根本不在同一张页表里，读到的字不可信），所以抄下来。 */
static uint8   s_futex_pi[ARKRTOS_TASK_MAX_NUM];
static int32   s_futex_pi_owner[ARKRTOS_TASK_MAX_NUM];
/* 这一槽是「WAIT_REQUEUE_PI 登记的」而不是普通 LOCK_PI。两类都用 s_futex_pi
 * 标着（普通 WAKE / REQUEUE 不能碰它们，判据一致），但只有 rqpi 那些能被
 * CMP_REQUEUE_PI 整队搬走；反过来，CMP_REQUEUE_PI 也不许搬 LOCK_PI 的人
 * ——那会把一个正等互斥锁的任务塞进另一个锁的队列里。 */
static uint8   s_futex_rqpi[ARKRTOS_TASK_MAX_NUM];
/* robust list：每个任务自己那一份（链头 + 调用方报的长度）。任务号会被
 * 复用，退出时必须清掉，否则新任务会拿别人的链去改别人的字。 */
static unsigned long s_robust_head[ARKRTOS_TASK_MAX_NUM];
static uint32  s_robust_len[ARKRTOS_TASK_MAX_NUM];
#endif

#if (ARKRTOS_USE_LINUX_SELECT == 1)
/* 每个任务最多一次 pselect6 阻塞：登记截止时刻（内核毫秒；0 也合法，所以用
 * 独立的 armed 标志而不是拿 0 当哨兵）。一条任务同一时刻只可能在一条
 * pselect6 里，不需要像 futex 那样按 key 分辨是不是重放。 */
static uint8  s_sel_armed[ARKRTOS_TASK_MAX_NUM];
static uint32 s_sel_deadline[ARKRTOS_TASK_MAX_NUM];
#endif

#if (ARKRTOS_USE_LINUX_SOCKET == 1)
/* 每个任务最多一次 socket 调用在飞（U-23）。登记的是**原始系统调用参数**
 * （a0..a5）：重放时 a0 已经被 lx_finish 写成本次返回值（0），只有 a1..a5
 * 还在寄存器里，所以不能靠参数寄存器。第一次进来把六个参数抄进本任务的槽，
 * 重放时反过来从槽里取 —— 两种情况下 r[] 都是权威值。
 *
 * step/res/aux 只给 accept 用：accept 成功之后若调用方要地址，还要再发一次
 * getpeername。第二次请求同样会重放，而重放会把整段处理函数再走一遍 ——
 * 如果那时不留记号，就会再造一个 socket。所以主请求一旦成功就把「已经接过
 * 的句柄 + 分配到的 fd」记下来，重放时跳过 accept 直奔地址查询。 */
typedef struct
{
    uint8  armed;
    uint8  step;
    uint32 arg[6];
    int32  res;
    int32  aux;
} lx_sockctx_t;

static lx_sockctx_t s_sockctx[ARKRTOS_TASK_MAX_NUM];
#endif

/* ------------------------------------------------------------- 信号状态 */

typedef struct
{
    unsigned long handler;      /* SIG_DFL(0) / SIG_IGN(1) / 用户函数地址 */
    unsigned long flags;        /* 原样保存，本实现只做「处理期间屏蔽」 */
    unsigned long restorer;     /* 必须非 0：没有 restorer 就没有返回路径 */
    unsigned long mask;         /* 处理期间附加的屏蔽位 */
} lx_sigaction_t;

/* ---------------------------- 进程级状态（fd 表 / 信号动作 / 屏蔽 / pending）
 * 历史版本把这两张表做成内核全局单份，并写死「一次只跑一个 ELF 程序」。
 * fork 落地之后这条前提不再成立：父与子必须各自持有自己的 fd 表与信号表
 * （Linux 语义），否则子进程 close 一个 fd 会把父的也关掉、子进程装的处理
 * 函数会被父执行。
 *
 * 关档（ARKRTOS_USE_LINUX_FORK=0）时 LX_PROC_MAX 就是 1：只有 s_proc[0]
 * 一份，映射与分配逻辑整段不参与编译，所有调用点读到的仍是那唯一的全局表，
 * 行为与历史版本逐位相同（体积只多出几个恒返回 0 的内联判断）。
 *
 * 槽位在任务第一次进 Linux ABI 时惰性分配（lx_proc_cur）；fork 时由
 * lx_clone 预先分配并把父的那一份复制过去。
 * 槽用满时 arkrtos_linux_abi_try 直接回 -ENOMEM —— 绝不把两个进程塞进
 * 同一份 fd/信号状态里，那会是一个查不出来的错答案。 */
#if (ARKRTOS_USE_LINUX_FORK == 1)
#define LX_PROC_MAX   (LX_MAX_CHILD + 3u)
#else
#define LX_PROC_MAX   (1u)
#endif

typedef struct
{
    lx_fd_t        fd[LX_MAX_FD];
    /* 标准流别名（dup/dup3）：std_slot[0..2] 存「槽 + 1」，0 = 没有别名。
     * 为什么要别名：0/1/2 在本层没有真槽位（控制台是直写、stdin 读到 EOF），
     * 而 dup2(fd, 1) 是真实程序最常用的重定向手法。别名**占一个真槽**而不是
     * 直接指向源槽：close(源 fd) 之后别名必须仍然有效（Linux 里这是两个独立
     * 描述符），共享槽做不到这一点。 */
    uint8          std_slot[3];
    lx_sigaction_t sig[LX_NSIG];
    uint32         sig_pending;
    uint32         sig_blocked;
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* U-25：线程组号（= 组长的任务号）与组长注册时的代次。同一组的线程
     * 共用一个槽，所以这两个字段对组里每个成员都是同一份 —— getpid 读它，
     * exit_group 靠它认出「我这一组是谁」。tgid == 0 表示还没定（惰性分配
     * 的槽在第一次进 ABI 时补上）。 */
    int32          tgid;
    uint32         tgid_gen;
#endif
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    lx_filemap_t   fmap[LX_FILEMAP_MAX];
#endif
} lx_proc_t;

static lx_proc_t s_proc[LX_PROC_MAX];
#if (ARKRTOS_USE_LINUX_FORK == 1)
static uint8 s_proc_used[LX_PROC_MAX];               /* 1 = 该槽已有主 */
static uint8 s_proc_of_task[ARKRTOS_TASK_MAX_NUM];   /* 0 = 未绑定；n = 槽 n-1 */
/* 1 = 该任务正卡在 wait4 的阻塞里：子进程退出 / 收到信号时只唤醒这种任务，
 * 不去打扰因为别的原因（sleep、信号量）在等的任务。 */
static uint8 s_lx_wait[ARKRTOS_TASK_MAX_NUM];
#endif
#if (ARKRTOS_USE_LINUX_THREAD == 1)
/* 每个线程的 CLONE_CHILD_CLEARTID 地址（0 = 没给）。退出时把那个字清零并
 * FUTEX_WAKE 一次 —— pthread_join 就卡在这里等。放在 ABI 层而不是 TCB 里：
 * 这是 Linux 语义的东西，内核核心不需要知道它。 */
static unsigned long s_lx_cleartid[ARKRTOS_TASK_MAX_NUM];
#endif

/* 历史名字改成宏：函数体里 s_fd[i] / s_sig[n] / s_sig_pending / s_sig_blocked
 * 原样可用，读写的都是**当前进程**那一份。 */
static lx_proc_t *lx_proc_curp(void);
#define s_fd            (lx_proc_curp()->fd)
#define s_sig           (lx_proc_curp()->sig)
#define s_sig_pending   (lx_proc_curp()->sig_pending)
#define s_sig_blocked   (lx_proc_curp()->sig_blocked)
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
#define s_fmap          (lx_proc_curp()->fmap)
#endif

/* 当前进程有没有「未屏蔽、且真会打断系统调用」的待投递信号。阻塞等待
 * 遇到这种情形按 Linux 回 -EINTR，而不是继续睡（管道阻塞读写与 futex
 * WAIT 共用这一条判据）。
 *
 * 判据必须排掉**默认动作是「忽略」**的那些位：SIGCHLD(17) / SIGCONT(18) /
 * SIGURG(23) / SIGWINCH(28) 在 SIG_DFL 下，以及任何显式 SIG_IGN 的信号。
 * Linux 的 get_signal() 对这类 pending 直接销掉返回 0，既不铺帧、也不
 * 打断系统调用。把它们算进来会让「子进程退出了」变成「父进程下一次阻塞
 * read 回 -EINTR」——实测抓到过：wait4 回收之后 SIGCHLD 的 pending 位还
 * 在（本层不清它，Linux 也不清），不排掉的话每个 fork 夹具的阻塞调用都
 * 会白白多回一次 -EINTR。 */
static uint8 lx_sig_pending_unblocked(void)
{
    uint32 sig;
    uint32 ready = s_sig_pending & ~s_sig_blocked;

    for(sig = 1u; sig < (uint32)LX_NSIG; sig++)
    {
        if((ready & lx_sig_bit(sig)) == 0u)
        {
            continue;
        }
        if(s_sig[sig].handler == LX_SIG_IGN)
        {
            continue;           /* 显式忽略：不打断 */
        }
        if((s_sig[sig].handler == LX_SIG_DFL) &&
           (lx_sig_default_ignored(sig) != 0u))
        {
            continue;           /* 默认忽略：不打断 */
        }
        return 1u;
    }
    return 0u;
}

/* 记录锁的回收钩子（定义在 fcntl 那一节）：进程槽被重置、或这个文件的任意
 * 一个 fd 被关掉，锁都必须跟着消失 —— 留着就是「谁也拿不到、谁也没见过」的
 * 幽灵锁。放在这里是因为两个调用点都在下面的结构体清理里。 */
static void lx_rslk_release_owner(uint32 slot);
static void lx_rslk_release_path(uint32 slot, const char *path);

static void lx_proc_zero(uint32 slot)
{
    uint32 i;

    /* 槽位被重置（退出、复用、全量 reset）：这个槽名下的记录锁必须跟着
     * 消失，否则下一次复用会让新进程"继承"一堆谁也解不开的锁。 */
    lx_rslk_release_owner(slot);
    for(i = 0u; i < LX_MAX_FD; i++)
    {
        s_proc[slot].fd[i].used = 0u;
        s_proc[slot].fd[i].kind = LX_FD_FILE;
        s_proc[slot].fd[i].pipe = 0u;
        s_proc[slot].fd[i].size = 0u;
        s_proc[slot].fd[i].off  = 0u;
        s_proc[slot].fd[i].path[0] = 0;
        s_proc[slot].fd[i].wr   = 0u;
        s_proc[slot].fd[i].cloexec = 0u;
        s_proc[slot].fd[i].fl   = 0u;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
        s_proc[slot].fd[i].owner = 0;
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
        s_proc[slot].fd[i].net    = 0u;
        s_proc[slot].fd[i].stype  = 0u;
        s_proc[slot].fd[i].sflags = 0u;
        s_proc[slot].fd[i].pok    = 0u;
        s_proc[slot].fd[i].pbits  = 0u;
#endif
    }
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* 写回记账**不跨 fork 继承**（见开关文档第 5 条）：子进程要同步自己的映射
     * 就读自己那张表，父的那条不会替它写回。 */
    for(i = 0u; i < (uint32)LX_FILEMAP_MAX; i++)
    {
        s_proc[slot].fmap[i].used = 0u;
        s_proc[slot].fmap[i].addr = 0u;
        s_proc[slot].fmap[i].len  = 0u;
        s_proc[slot].fmap[i].off  = 0u;
        s_proc[slot].fmap[i].size = 0u;
        s_proc[slot].fmap[i].path[0] = 0;
    }
#endif
    for(i = 0u; i < (uint32)LX_NSIG; i++)
    {
        s_proc[slot].sig[i].handler  = LX_SIG_DFL;
        s_proc[slot].sig[i].flags    = 0UL;
        s_proc[slot].sig[i].restorer = 0UL;
        s_proc[slot].sig[i].mask     = 0UL;
    }
    for(i = 0u; i < 3u; i++)
    {
        s_proc[slot].std_slot[i] = 0u;
    }
    s_proc[slot].sig_pending = 0u;
    s_proc[slot].sig_blocked = 0u;
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    s_proc[slot].tgid     = 0;
    s_proc[slot].tgid_gen = 0u;
#endif
}

#if (ARKRTOS_USE_LINUX_FORK == 1)
/* 任务 -> 槽（只查已绑定的；-1 = 这个任务还没有进程状态）。 */
static int32 lx_proc_find(int32 task_id)
{
    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    if(s_proc_of_task[task_id - 1] == 0u)
    {
        return -1;
    }
    return (int32)(s_proc_of_task[task_id - 1] - 1u);
}

/* 占一个空槽（不清内容也不绑定；调用方决定零化还是复制父的）。-1 = 用满。 */
static int32 lx_proc_alloc(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)LX_PROC_MAX; i++)
    {
        if(s_proc_used[i] == 0u)
        {
            s_proc_used[i] = 1u;
            return (int32)i;
        }
    }
    return -1;
}

static void lx_proc_free(uint32 slot)
{
    if(slot < (uint32)LX_PROC_MAX)
    {
        s_proc_used[slot] = 0u;
    }
}

static void lx_proc_bind(uint32 slot, int32 task_id)
{
    if((task_id > 0) && (task_id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        s_proc_of_task[task_id - 1] = (uint8)(slot + 1u);
    }
}

static void lx_proc_unbind(int32 task_id)
{
    if((task_id > 0) && (task_id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        s_proc_of_task[task_id - 1] = 0u;
        s_lx_wait[task_id - 1]      = 0u;
    }
}
#endif /* ARKRTOS_USE_LINUX_FORK == 1 */

/* 当前任务的槽号：第一次进 ABI 时惰性分配。返回 LX_PROC_MAX 表示槽用满
 * （调用方 arkrtos_linux_abi_try 会如实回 -ENOMEM）。 */
static uint32 lx_proc_cur(void)
{
#if (ARKRTOS_USE_LINUX_FORK == 1)
    int32 slot = lx_proc_find((int32)arkrtos_current_task_id);

    if(slot >= 0)
    {
        return (uint32)slot;
    }
    slot = lx_proc_alloc();
    if(slot < 0)
    {
        return (uint32)LX_PROC_MAX;
    }
    lx_proc_zero((uint32)slot);
    lx_proc_bind((uint32)slot, (int32)arkrtos_current_task_id);
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* 这个槽的第一位主人就是组长。代次一起记下：任务槽会被复用，单看任务号
     * 认不出「同一个号上的新旧两个实例」。 */
    s_proc[(uint32)slot].tgid     = (int32)arkrtos_current_task_id;
    s_proc[(uint32)slot].tgid_gen =
        arkrtos_task_table[arkrtos_current_task_id - 1].generation;
#endif
    return (uint32)slot;
#else
    return 0u;
#endif
}

static lx_proc_t *lx_proc_curp(void)
{
    uint32 slot = lx_proc_cur();

    /* 越界只可能出现在 try() 已经拒绝之后：钳到 0 只为不踩内存，
     * 那条路径上一个字都不会被返回给程序。 */
    if(slot >= (uint32)LX_PROC_MAX)
    {
        slot = 0u;
    }
    return &s_proc[slot];
}


/* 32 项动作表：1..31 有效，0 号不用。一个任务一份（fd 表也是这个口径：
 * 本版本一次只跑一个 ELF 程序）。 */
static uint32 s_sig_delivered;      /* 真投递进处理函数的次数 */
static uint32 s_sig_ignored;        /* SIG_IGN 或默认忽略 */
static uint32 s_sig_refused;        /* kill 的目标不是自己 */
static uint32 s_sig_no_restorer;    /* 装处理函数时没给 restorer */
static uint32 s_sig_stop_ignored;   /* 默认动作是「挂起」但本内核没有作业控制 */
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
static uint32 s_sigio_sent;         /* 管道变可读时真投出去的 SIGIO 次数 */
static uint32 s_sigio_pgrp_refused; /* F_SETOWN 给负参数（进程组）被拒的次数 */
#endif
/* 信号帧的暂存（头 + 整帧快照）。投递路径在内核里不嵌套（一次只有一条
 * 系统调用在返回），所以一块静态缓冲就够，不在内核栈上开 1 KB。 */
static unsigned long s_sigframe[LX_SIGFRAME_HDR_WORDS + LX_SIGFRAME_MAX_WORDS];

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
typedef struct
{
    uint32 nr;
    uint32 hits;
} lx_nr_slot_t;

/* 稀疏表，不是 512 项数组：本内核的可执行文件最多用到十几个号，一张
 * 512×4 的表对 RAM 池是白付的体积。前缀 [0, s_nr_used) 有效。 */
static lx_nr_slot_t s_nr[LX_NR_SLOTS];
static uint32       s_nr_used;
static uint32       s_nr_overflow;
#endif

/* 堆游标（brk / mmap 共用）。窗口是整块静态映射，所以这里的"分配"只是
 * 在窗口内划出一段不重叠的地址，不建立任何新映射。 */
static unsigned long s_heap_cur;

/* ---------------------------------------------------------------- 小工具 */

static void lx_ret(void *p_ctx, int32 value)
{
    arkrtos_port_svc_set_ret_signed(p_ctx, value);
}

/* 地址类返回值（brk / mmap）必须是整字宽：走 int32 的那个出口在 64 位
 * port 上会把 0x120000000 这类地址截成负数（"失败的假象"）。这两个
 * 处理函数自己写回返回值，再返回 LX_RET_PTR_DONE 让分发器不覆盖。 */
#define LX_RET_PTR_DONE     ((int32)0x7FFFFFFF)

/* execve 成功：帧已经被改成新镜像的入口，这一路没有任何返回值要写。
 * 用另一个哨兵而不用 0，是为了让「成功但不返回」与「返回 0」在分发器里
 * 看得出区别。 */
#define LX_RET_EXEC_DONE    ((int32)0x7FFFFF00)
/* 「这次调用还没结束，参数寄存器原样留给重放的那一次」。分发器见到它只计数：
 * 不写 a0（写了就把 a0 里的第一个参数冲掉，重放时读到一个假参数）、不投递
 * 信号（调用还没返回，信号帧里不该有这个半截的现场）。socket 族与 pselect6
 * 的阻塞路径都用它。 */
#define LX_RET_RETRY        ((int32)0x7FFFFE00)

static void lx_ret_ptr(void *p_ctx, unsigned long value)
{
    arkrtos_port_svc_set_ret(p_ctx, value);
}

/**
* @brief 从用户地址拷一个以 NUL 结尾的路径
* @return 0 成功；-EFAULT 指针非法；-ENAMETOOLONG 没有 NUL
* @details 逐字节过 copy_from_user：它会对每一个字节核对是否落在调用者的
*          窗口内，既不会越界读，也不会把内核地址当用户地址用。
*/
/**
* @brief 把用户地址上的 NUL 结尾串拷进内核缓冲
* @return 0 成功；-EFAULT 指针非法；-ENAMETOOLONG 在 cap 内没有 NUL
*/
static int32 lx_copy_user_str(unsigned long uaddr, char *out, uint32 cap)
{
    uint32 i;

    if((uaddr == 0u) || (out == 0) || (cap == 0u))
    {
        return -LX_EFAULT;
    }

    for(i = 0u; i < cap; i++)
    {
        uint8 c = 0u;

        if(arkrtos_copy_from_user(&c, (const void *)(unsigned long)(uaddr + i),
                                  1u) != 0)
        {
            return -LX_EFAULT;
        }
        out[i] = (char)c;
        if(c == 0u)
        {
            return 0;
        }
    }

    return -LX_ENAMETOOLONG;
}

static int32 lx_copy_path(void *p_ctx, uint32 idx, char *out, uint32 out_size)
{
    return lx_copy_user_str(ARKRTOS_SVC_ARG(p_ctx, idx), out, out_size);
}

static void lx_copy_str(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (cap == 0u))
    {
        return;
    }
    while((i + 1u < cap) && (src[i] != 0))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static unsigned long lx_heap_base(void)
{
#if (ARKRTOS_USE_DYNLOAD == 1)
    /* 动态档：窗口背面留给解释器，堆从解释器区之上开始，不与它重叠。 */
    return (unsigned long)ARKRTOS_ELF_INTERP_BASE +
           (unsigned long)ARKRTOS_ELF_INTERP_SIZE;
#else
    return (unsigned long)ARKRTOS_ELF_WINDOW_BASE +
           ((unsigned long)ARKRTOS_ELF_WINDOW_SIZE / 2u);
#endif
}

static unsigned long lx_heap_end(void)
{
    /* 窗口顶端是装载器切的栈与参数区（默认 32K + 16K），这里再留 128K，
     * 不与之重叠。 */
    return (unsigned long)ARKRTOS_ELF_WINDOW_BASE +
           ((unsigned long)ARKRTOS_ELF_WINDOW_SIZE - (128u * 1024u));
}

static unsigned long lx_align_up(unsigned long v)
{
    unsigned long mask = 4095u;

    if(v > (~0UL - mask))
    {
        return 0u;      /* 溢出：调用方据此回 -ENOMEM */
    }
    return (v + mask) & ~mask;
}

/* LA64 端口上共享页的物理粒度是 8 KB 一对（见
 * arkrtos_port_paging_map_shared）：一段 4 KB 的共享映射也会独占整对。
 * 所以共享映射的起址必须落在对边界上、堆游标也必须整对前进 —— 否则第二个
 * 共享映射会叠进第一对的另一半，被端口按「这一段已经有内容」拒绝，形式上
 * 是 -ENOMEM，读起来像内存不够。私有映射不这样要求：按需缺页的一对里可
 * 以住两个不同进程的不同页。 */
#if (ARKRTOS_USE_PAGING_SHM == 1)
static unsigned long lx_pair_up(unsigned long v)
{
    unsigned long mask = 0x1FFFul;

    if(v > (~0UL - mask))
    {
        return 0u;
    }
    return (v + mask) & ~mask;
}
#endif

static int32 lx_fd_find(int32 fd)
{
    int32 idx;

    if(fd < 3)
    {
        return -1;
    }
    idx = fd - 3;
    if(idx >= (int32)LX_MAX_FD)
    {
        return -1;
    }
    if(s_fd[idx].used == 0u)
    {
        return -1;
    }
    return idx;
}

/* 找第一个空 fd 槽位；返回的是表下标（不是 fd 号）。 */
static int32 lx_fd_free_idx(void)
{
    uint32 i;

    for(i = 0u; i < LX_MAX_FD; i++)
    {
        if(s_fd[i].used == 0u)
        {
            return (int32)i;
        }
    }
    return -1;
}

/* 定义在管道那一节后面（dup 要先用它把一个槽还回去）。 */
static uint32 lx_fd_release(uint32 slot, uint32 idx);
static int32 lx_sig_send(int32 pid, uint32 sig);
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
static void lx_async_notify_pipe(int32 pi);
#endif

/* 标准流别名 → 槽下标；-1 = 没有别名。used 一起判：槽被别的路径放掉之后
 * 这里只会看到悬空，如实当「没有别名」处理，绝不去索引一个空槽。 */
static int32 lx_std_slot(int32 fd)
{
    uint8 v;

    if((fd < 0) || (fd > 2))
    {
        return -1;
    }
    v = lx_proc_curp()->std_slot[fd];
    if((v == 0u) || ((uint32)(v - 1u) >= LX_MAX_FD))
    {
        return -1;
    }
    if(s_fd[v - 1u].used == 0u)
    {
        return -1;
    }
    return (int32)(v - 1u);
}

static void lx_std_set(int32 fd, int32 idx)
{
    if((fd < 0) || (fd > 2))
    {
        return;
    }
    lx_proc_curp()->std_slot[fd] = (idx < 0) ? 0u : (uint8)(idx + 1);
}

/* 一个控制台引用占的空槽：读回 EOF、写回控制台、关掉什么也不做。 */
static void lx_fd_console_fill(uint32 idx)
{
    s_fd[idx].kind    = LX_FD_CONSOLE;
    s_fd[idx].pipe    = 0u;
    s_fd[idx].wr      = 0u;
    s_fd[idx].cloexec = 0u;
    /* 控制台读回 EOF、写不等锁，行为上就是一台不阻塞的设备：F_GETFL 如实
     * 这么报，而不是报 0（阻塞）让程序去等一个永远不来的事件。 */
    s_fd[idx].fl      = (uint16)(LX_O_RDWR | LX_O_NONBLOCK);
    s_fd[idx].size    = 0u;
    s_fd[idx].off     = 0u;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    s_fd[idx].owner   = 0;
#endif
    s_fd[idx].path[0] = 0;
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    s_fd[idx].net     = 0u;
    s_fd[idx].stype   = 0u;
    s_fd[idx].sflags  = 0u;
    s_fd[idx].pok     = 0u;
    s_fd[idx].pbits   = 0u;
#endif
    s_fd[idx].used    = 1u;
}

/* 把一个有效槽复制到另一个空槽（dup 的实体）。
 *
 * 复制的是**槽**，不是 Linux 的「打开文件表项（struct file）」：两者唯一
 * 可见的差别是文件游标 off 各走各的（Linux 的 dup 共享游标）。管道与控制台
 * 没有游标，语义与 Linux 完全一致；这条偏差写在
 * docs/Linux可执行文件兼容-系统调用ABI.md 的 dup 一节里，并且有用例钉住。
 *
 * 管道要按复制出来的那一份**再加一次**读写端计数：两端计数是「有几个 fd
 * 都指向这条管道」，dup 之后确实多了一个。
 *
 * socket 不在这里复制：net 句柄是「一格一份」的，两个槽指向同一个 handle 会
 * 出现「谁先 close 谁把对方打哑」。调用方已对 socket 明确 -EINVAL，这里把
 * 该清的清掉，绝不把句柄抄过去。 */
static void lx_fd_clone(uint32 dst, uint32 src)
{
    uint8 kind = s_fd[src].kind;
    int32 pi;

    s_fd[dst].kind    = kind;
    s_fd[dst].pipe    = s_fd[src].pipe;
    s_fd[dst].wr      = s_fd[src].wr;
    s_fd[dst].cloexec = 0u;     /* Linux：dup 出来的副本不带 FD_CLOEXEC */
    s_fd[dst].fl      = s_fd[src].fl;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    /* Linux 里 dup 共享同一个 file description，F_SETOWN 的登记当然共享。 */
    s_fd[dst].owner   = s_fd[src].owner;
#endif
    s_fd[dst].size    = s_fd[src].size;
    s_fd[dst].off     = s_fd[src].off;
    lx_copy_str(s_fd[dst].path, (uint32)sizeof(s_fd[dst].path), s_fd[src].path);
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    s_fd[dst].net     = 0u;
    s_fd[dst].stype   = 0u;
    s_fd[dst].sflags  = 0u;
    s_fd[dst].pok     = 0u;
    s_fd[dst].pbits   = 0u;
#endif
    if((kind == LX_FD_PIPE_R) || (kind == LX_FD_PIPE_W))
    {
        pi = (int32)s_fd[dst].pipe;
        if((pi >= 0) && (pi < (int32)LX_PIPE_MAX) && (s_pipe[pi].used != 0u))
        {
            if(kind == LX_FD_PIPE_R)
            {
                s_pipe[pi].readers++;
            }
            else
            {
                s_pipe[pi].writers++;
            }
        }
    }
    s_fd[dst].used = 1u;
}

static int32 lx_pipe_alloc(void)
{
    uint32 i;

    for(i = 0u; i < LX_PIPE_MAX; i++)
    {
        if(s_pipe[i].used == 0u)
        {
            return (int32)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ 阻塞等待（U-26） */

/* 叫醒挂在某条管道某个方向上的全部等待者。写侧写入数据后叫醒读侧，读侧
 * 腾出空间后叫醒写侧；关闭路径在「某方向的引用计数归零」时叫醒对侧
 * （读端归零 -> 写端醒来看到 -EPIPE，写端归零 -> 读端醒来读到 EOF）。
 *
 * 唤醒只负责「叫醒」，判定留在重放那一轮：条件可能在一瞬间又被对侧改回去
 * （刚写入就被别的读者读走），在唤醒点替对方下结论只会给出假答案。
 * 与 lx_futex_wake_key / lx_sock_ctx 同一套做法。 */
static void lx_pipe_wake(int32 pi, uint8 dir)
{
    int32  i;
    uint32 crit = ARKRTOS_ENTER_CRITICAL();

    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if((s_pipe_wait[i - 1].used != 0u) &&
           (s_pipe_wait[i - 1].dir == dir) &&
           ((int32)s_pipe_wait[i - 1].pi == pi))
        {
            (void)arkrtos_task_wakeup_internal(i);
        }
    }
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    /* dir==0 = 读方向可读（有新数据，或者写端全关的 EOF），正是 SIGIO 的
     * 触发条件；可写方向不在本版本的范围，不发。临界区可嵌套（enter 返回
     * PRIMASK、exit 原样恢复），这里在退出前调用是安全的。 */
    if(dir == 0u)
    {
        lx_async_notify_pipe(pi);
    }
#endif
    ARKRTOS_EXIT_CRITICAL(crit);
}

#if (ARKRTOS_USE_LINUX_SIGIO == 1)
/* U-29：某条管道刚刚变成「有数据可读」，把 SIGIO 发给登记了主人的读端。
 *
 * 认的方式是**扫描**每进程的 fd 表，不另立一张注册表：注册表要与 close /
 * dup / execve / 进程退出四处同步，漏一处就会给一个已经不存在的 fd 发信号
 * （或者反过来明明还开着却不发）。扫描的是唯一真源，代价是每次管道写最多
 * LX_PROC_MAX * LX_MAX_FD 次整数比较 —— 两个数都是 16，256 次比较，比一次
 * 系统调用本身便宜。
 *
 * 发给谁：owner 是任务号，主人不在时 lx_sig_send 自会回 -ESRCH（丢弃），不会
 * 把信号投给别的任务。这里不进临界区：扫描之间被抢占，最坏是给一个刚退出的
 * 任务发一次、被丢弃；这与「主人没了就没人收」的语义一致。
 */
static void lx_async_notify_pipe(int32 pi)
{
    uint32 slot;
    uint32 i;

    for(slot = 0u; slot < (uint32)LX_PROC_MAX; slot++)
    {
#if (ARKRTOS_USE_LINUX_FORK == 1)
        if(s_proc_used[slot] == 0u)
        {
            continue;               /* 没有主人的槽：整段跳过 */
        }
#else
        if(slot != 0u)
        {
            continue;               /* 单进程档只有 0 号槽 */
        }
#endif
        for(i = 0u; i < LX_MAX_FD; i++)
        {
            if(s_proc[slot].fd[i].used == 0u)
            {
                continue;
            }
            if(s_proc[slot].fd[i].kind != LX_FD_PIPE_R)
            {
                continue;
            }
            if((int32)s_proc[slot].fd[i].pipe != pi)
            {
                continue;
            }
            if((s_proc[slot].fd[i].fl & (uint16)LX_O_ASYNC) == 0u)
            {
                continue;
            }
            if(s_proc[slot].fd[i].owner == 0)
            {
                continue;
            }
            s_sigio_sent++;
            (void)lx_sig_send((int32)s_proc[slot].fd[i].owner, 29u);
        }
    }
}
#endif

/* 撤掉当前任务的等待登记。每一次「这次调用真的结束了」（拿到数据、EOF、
 * 错误、-EINTR）都要走一遍：登记留着会让下一次系统调用被误认成重放。 */
static void lx_pipe_forget(int32 me)
{
    uint32 crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    s_pipe_wait[me - 1].used = 0u;
    ARKRTOS_EXIT_CRITICAL(crit);
}

/* 这是不是同一次调用的下一轮？是则取回已经写进去的字节数（读侧没有进度，
 * 一律 0）。判定用「同一任务 + 同一条管道 + 同一个方向」，不看地址：同一
 * 个任务睡着时不可能再发起第二个管道调用，所以这个匹配不会认错人。 */
static uint32 lx_pipe_resume(int32 me, int32 pi, uint8 dir)
{
    uint32 done = 0u;
    uint32 crit;

    crit = ARKRTOS_ENTER_CRITICAL();
    if((me > 0) && (me <= (int32)ARKRTOS_TASK_MAX_NUM) &&
       (s_pipe_wait[me - 1].used != 0u) &&
       (s_pipe_wait[me - 1].dir == dir) &&
       ((int32)s_pipe_wait[me - 1].pi == pi))
    {
        done = s_pipe_wait[me - 1].done;
    }
    ARKRTOS_EXIT_CRITICAL(crit);
    return done;
}

/* 挂起一个时间片，返回 LX_RET_RETRY。
 *
 * 为什么用 LX_RET_RETRY 而不是像 futex 那样回 0：fd / 用户地址 / 长度就在
 * a0..a2 里，回 0 会把 a0 冲成 0（fd 0 = 控制台），重放那一轮读到的全是假
 * 参数。LX_RET_RETRY 是 socket 与 pselect6 阻塞路径用的同一条约定——参数
 * 寄存器原样留给重放的那一次，分发器既不写 a0、也不投信号（这次调用还没
 * 返回，信号帧里不该有这个半截的现场）。
 *
 * 时间片是「丢唤醒」的兜底：对侧的动作会主动叫醒我们，但如果不巧在登记
 * 与睡下之间被抢占，那一次唤醒就丢了；100 ms 后重放一轮重新判条件，不会
 * 因为一次没叫醒就永久卡住。 */
static int32 lx_pipe_sleep(void *p_ctx, int32 me, int32 pi, uint8 dir, uint32 done)
{
    uint32 crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EAGAIN;          /* 没有任务身份：不假装睡过 */
    }
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        return -LX_ENOSYS;          /* 端口不能重放：如实拒绝，不含糊地回 0 */
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    s_pipe_wait[me - 1].used = 1u;
    s_pipe_wait[me - 1].dir  = dir;
    s_pipe_wait[me - 1].pi   = (uint8)pi;
    s_pipe_wait[me - 1].done = done;
    ARKRTOS_EXIT_CRITICAL(crit);
    (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
    return LX_RET_RETRY;
}

/* ---------------------------------------------------------------- 管道读写 */

/* 从管道读。语义按 Linux：
 *   * 有数据就回（短读正常，不等到 want 满）；
 *   * 空且写端全关 -> 0（EOF）；
 *   * 空而写端还在 -> O_NONBLOCK 回 -EAGAIN，否则挂起等下一位写者；
 *   * 有未屏蔽信号待投递 -> -EINTR（Linux 的阻塞读同样被信号打断）。
 * 返回值可能是 LX_RET_RETRY：那表示「这次调用还没结束」。 */
static int32 lx_pipe_read(void *p_ctx, int32 idx, unsigned long ua, uint32 want)
{
    int32  me = (int32)arkrtos_current_task_id;
    int32  pi = (int32)s_fd[idx].pipe;
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;

    if((pi < 0) || (pi >= (int32)LX_PIPE_MAX) || (s_pipe[pi].used == 0u))
    {
        lx_pipe_forget(me);
        return -LX_EBADF;
    }
    (void)lx_pipe_resume(me, pi, 0u);      /* 读侧没有进度要接着算 */
    if(want == 0u)
    {
        lx_pipe_forget(me);
        return 0;
    }
    if(ua == 0u)
    {
        lx_pipe_forget(me);
        return -LX_EFAULT;
    }

    while((done < want) && (s_pipe[pi].count > 0u))
    {
        uint32 n = want - done;
        uint32 k;

        if(n > (uint32)LX_IO_CHUNK)
        {
            n = (uint32)LX_IO_CHUNK;
        }
        if(n > (uint32)s_pipe[pi].count)
        {
            n = (uint32)s_pipe[pi].count;
        }
        /* 先按 tail 把这一块抄进内核缓冲，**拷贝成功之后**才推进 tail：
         * 用户指针在半路失效时，环形缓冲里不能少掉任何字节（否则下一次
         * read 会从后面接着读，数据静默丢失）。 */
        {
            uint16 tail = s_pipe[pi].tail;

            for(k = 0u; k < n; k++)
            {
                buf[k] = s_pipe[pi].buf[tail];
                tail = (uint16)(((uint32)tail + 1u) % (uint32)LX_PIPE_BUF);
            }
            if(arkrtos_copy_to_user((void *)(unsigned long)(ua + done), buf, n) != 0)
            {
                lx_pipe_forget(me);
                if(done > 0u)
                {
                    lx_pipe_wake(pi, 1u);   /* 已经取走的部分照样腾出了空间 */
                    return (int32)done;
                }
                return -LX_EFAULT;
            }
            s_pipe[pi].tail  = tail;
            s_pipe[pi].count = (uint16)((uint32)s_pipe[pi].count - n);
        }
        done += n;
    }

    if(done > 0u)
    {
        lx_pipe_forget(me);
        lx_pipe_wake(pi, 1u);       /* 腾出了空间：叫醒可能等着的写端 */
        return (int32)done;
    }
    if(s_pipe[pi].writers == 0u)
    {
        lx_pipe_forget(me);
        return 0;                   /* EOF */
    }
    if((s_fd[idx].fl & (uint16)LX_O_NONBLOCK) != 0u)
    {
        lx_pipe_forget(me);
        return -LX_EAGAIN;
    }
    if(lx_sig_pending_unblocked() != 0u)
    {
        lx_pipe_forget(me);
        return -LX_EINTR;
    }
    return lx_pipe_sleep(p_ctx, me, pi, 0u, 0u);
}

/* 往管道写。语义按 Linux：
 *   * 读端全关 -> 先置 SIGPIPE 待投递位，再回 -EPIPE；已经写进去的那部分
 *     照实回短写（Linux 也是这样：短写之后的下一次才吃到 -EPIPE）；
 *   * 缓冲满 -> O_NONBLOCK 回 -EAGAIN（或已写部分的短写），否则挂起等读端
 *     腾空间；
 *   * 阻塞写是**写全**的：超过 256 字节的块分段等空间，直到 want 都进去；
 *   * 有未屏蔽信号 -> 已写部分优先回短写，一字节没写才回 -EINTR。
 * 返回值可能是 LX_RET_RETRY（见 lx_pipe_sleep）。 */
static int32 lx_pipe_write(void *p_ctx, int32 idx, unsigned long ua, uint32 want)
{
    int32  me = (int32)arkrtos_current_task_id;
    int32  pi = (int32)s_fd[idx].pipe;
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;

    if((pi < 0) || (pi >= (int32)LX_PIPE_MAX) || (s_pipe[pi].used == 0u))
    {
        lx_pipe_forget(me);
        return -LX_EBADF;
    }
    done = lx_pipe_resume(me, pi, 1u);     /* done 跨重放累计 */
    if(want == 0u)
    {
        lx_pipe_forget(me);
        return 0;
    }
    if(ua == 0u)
    {
        lx_pipe_forget(me);
        return -LX_EFAULT;
    }
    if(done >= want)
    {
        /* 上一轮已经把整块都写进去了，只剩最后一次返回（不会发生，但把
         * 它写成显式的，免得以后改循环时越界） */
        lx_pipe_forget(me);
        return (int32)done;
    }

    for(;;)
    {
        /* 读端全关：**先判再拷**。Linux 的 write 在 readers 归零时不管环形
         * 缓冲还剩多少空间都回 -EPIPE（SIGPIPE 也照样投），所以这一判必须
         * 在写循环之前；把它放在循环之后会让「关掉读端再写一个字节」变成
         * 成功写进缓冲，等到下次写才报错。 */
        if(s_pipe[pi].readers == 0u)
        {
            lx_pipe_forget(me);
            if(done > 0u)
            {
                return (int32)done; /* 已提交的照常交付，下一轮再 -EPIPE */
            }
            lx_sig_raise(13u);      /* SIGPIPE：默认动作结束任务 */
            return -LX_EPIPE;
        }
        while((done < want) && (s_pipe[pi].count < (uint16)LX_PIPE_BUF))
        {
            uint32 space = (uint32)LX_PIPE_BUF - (uint32)s_pipe[pi].count;
            uint32 n = want - done;
            uint32 k;

            if(n > (uint32)LX_IO_CHUNK)
            {
                n = (uint32)LX_IO_CHUNK;
            }
            if(n > space)
            {
                n = space;
            }
            if(arkrtos_copy_from_user(buf, (const void *)(unsigned long)(ua + done), n) != 0)
            {
                lx_pipe_forget(me);
                if(done > 0u)
                {
                    lx_pipe_wake(pi, 0u);
                    return (int32)done;
                }
                return -LX_EFAULT;
            }
            for(k = 0u; k < n; k++)
            {
                s_pipe[pi].buf[s_pipe[pi].head] = buf[k];
                s_pipe[pi].head = (uint16)(((uint32)s_pipe[pi].head + 1u) % (uint32)LX_PIPE_BUF);
                s_pipe[pi].count++;
            }
            done += n;
        }

        if(done == want)
        {
            lx_pipe_forget(me);
            lx_pipe_wake(pi, 0u);   /* 有新数据：叫醒可能等着的读端 */
            return (int32)done;
        }
        /* 循环末尾**不再**重复判 `readers == 0`：`for(;;)` 顶部那一处就是
         * 同一个判据，下一轮（睡醒重放后）第一件事就是它。单线程内核里读端
         * 不可能在一次拷贝中间消失；抢占式内核里最坏多睡一个时间片，醒来
         * 拿到的仍是 -EPIPE，不会把已写字节说成没写。留着两份判据只会多出
         * 一段在宿主上永远走不到的死代码。 */
        if((s_fd[idx].fl & (uint16)LX_O_NONBLOCK) != 0u)
        {
            lx_pipe_forget(me);
            return (done > 0u) ? (int32)done : -LX_EAGAIN;
        }
        if(lx_sig_pending_unblocked() != 0u)
        {
            lx_pipe_forget(me);
            return (done > 0u) ? (int32)done : -LX_EINTR;
        }
        {
            int32 rc = lx_pipe_sleep(p_ctx, me, pi, 1u, done);

            if((rc < 0) && (done > 0u))
            {
                /* 端口不能重放（-ENOSYS）：已经拷进去的那部分照常交付，
                 * 回已写字节数——与「拷贝半路失败」同一条口径，绝不把
                 * 写进去的数据说成没写。 */
                return (int32)done;
            }
            return rc;
        }
    }
}

/* pipe2(59)：成功时把两个 fd 号（读端在先）写回用户数组。flags 只认
 * O_NONBLOCK，其余位明确 -EINVAL：本内核没有 exec 时的关闭语义，接受
 * O_CLOEXEC 就是给一个不成立的承诺。 */
static int32 lx_pipe2(void *p_ctx)
{
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 1u);
    int32  fds[2];
    int32  pi;
    int32  i0;
    int32  i1;

    if(ua == 0u)
    {
        return -LX_EFAULT;
    }
    if((flags & ~(uint32)(LX_O_NONBLOCK | LX_O_CLOEXEC)) != 0u)
    {
        return -LX_EINVAL;
    }

    pi = lx_pipe_alloc();
    if(pi < 0)
    {
        return -LX_ENFILE;
    }
    i0 = lx_fd_free_idx();
    if(i0 < 0)
    {
        return -LX_ENFILE;
    }
    s_fd[i0].used = 1u;         /* 先占住，第二次找空位才不会拿回同一个 */
    i1 = lx_fd_free_idx();
    if(i1 < 0)
    {
        s_fd[i0].used = 0u;
        return -LX_ENFILE;
    }

    s_pipe[pi].used    = 1u;
    s_pipe[pi].readers = 1u;
    s_pipe[pi].writers = 1u;
    s_pipe[pi].head    = 0u;
    s_pipe[pi].tail    = 0u;
    s_pipe[pi].count   = 0u;

    s_fd[i0].kind     = LX_FD_PIPE_R;
    s_fd[i0].pipe     = (uint8)pi;
    s_fd[i0].size     = 0u;
    s_fd[i0].off      = 0u;
    s_fd[i0].path[0]  = 0;
    s_fd[i0].wr       = 0u;
    s_fd[i1].used     = 1u;
    s_fd[i1].kind     = LX_FD_PIPE_W;
    s_fd[i1].pipe     = (uint8)pi;
    s_fd[i1].size     = 0u;
    s_fd[i1].off      = 0u;
    s_fd[i1].path[0]  = 0;
    s_fd[i1].wr       = 0u;
    /* 状态标志如实报：不带 O_NONBLOCK 就是**阻塞**管道（Linux 的默认），
     * 读写空/满时挂起等对侧；带 O_NONBLOCK 才是能读/写多少算多少 + -EAGAIN。
     * F_SETFL 之后可以来回改这一位，语义跟着变。 */
    s_fd[i0].fl = (uint16)LX_O_RDONLY;
    s_fd[i1].fl = (uint16)LX_O_WRONLY;
    if((flags & (uint32)LX_O_NONBLOCK) != 0u)
    {
        s_fd[i0].fl |= (uint16)LX_O_NONBLOCK;
        s_fd[i1].fl |= (uint16)LX_O_NONBLOCK;
    }
    s_fd[i0].cloexec = ((flags & (uint32)LX_O_CLOEXEC) != 0u) ? 1u : 0u;
    s_fd[i1].cloexec = s_fd[i0].cloexec;

    fds[0] = (int32)(3 + i0);
    fds[1] = (int32)(3 + i1);
    if(arkrtos_copy_to_user((void *)(unsigned long)ua, fds,
                            (uint32)sizeof(fds)) != 0)
    {
        /* 用户地址写不进去：把刚建的两端整条撤回，不留半条管道。 */
        s_pipe[pi].used    = 0u;
        s_pipe[pi].count   = 0u;
        s_pipe[pi].readers = 0u;
        s_pipe[pi].writers = 0u;
        s_fd[i0].used = 0u;
        s_fd[i1].used = 0u;
        return -LX_EFAULT;
    }

    return 0;
}

/* ---------------------------------------------------------------- dup */

/* dup / dup3 的实体：把 oldfd 复制到 newfd。
 *
 *   oldfd 0..2 且没有别名  -> 复制的是「控制台」这个对象（Linux 的 dup(1)
 *                             是保存 stdout 的标准手法，程序常用）；
 *   newfd < 0              -> dup：取最小的空号；
 *   newfd 0..2             -> 标准流别名：占一个**新槽**，别名记在 std_slot；
 *   newfd >= 3             -> 覆盖那个 fd（Linux 语义：先关掉再指过去）。
 *
 * 明确的边界（文档与用例都钉住）：
 *   * socket 的 fd 不能 dup（-EINVAL）——net 句柄一格一份，共享要一层打开
 *     文件表项，本层没有；给出 -EINVAL 而不是一个会互相打哑的假句柄；
 *   * dup3 的 O_CLOEXEC 明确 -EINVAL——本内核没有 exec 时的关闭语义，与
 *     pipe2 同一立场；
 *   * 覆盖一个 socket 槽也是 -EINVAL：那一关必须过 net 服务（重放语义），
 *     本层没有那条路，硬关会在系统里漏一个 net socket。 */
static int32 lx_fd_dup_to(int32 oldfd, int32 newfd)
{
    int32 idx     = -1;
    int32 dst;
    uint8 console = 0u;

    if((oldfd >= 0) && (oldfd <= 2))
    {
        idx = lx_std_slot(oldfd);
        if(idx < 0)
        {
            console = 1u;       /* 没有别名的标准流 = 控制台对象 */
        }
    }
    else
    {
        idx = lx_fd_find(oldfd);
        if(idx < 0)
        {
            return -LX_EBADF;
        }
    }
    if(console == 0u)
    {
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
        if(s_fd[idx].kind == LX_FD_SOCKET)
        {
            return -LX_EINVAL;
        }
#endif
    }

    if(newfd < 0)
    {
        dst = lx_fd_free_idx();
        if(dst < 0)
        {
            return -LX_ENFILE;
        }
        if(console != 0u)
        {
            lx_fd_console_fill((uint32)dst);
        }
        else
        {
            lx_fd_clone((uint32)dst, (uint32)idx);
        }
        return (int32)(3 + dst);
    }

    if(newfd <= 2)
    {
        int32 old_alias = lx_std_slot(newfd);

        if(old_alias >= 0)
        {
            /* 别名原来那一格就是本次的目标格：先摘别名再还槽，不会多占一格，
             * 也不会在表满时假装失败。 */
            lx_std_set(newfd, -1);
            (void)lx_fd_release(lx_proc_cur(), (uint32)old_alias);
            dst = old_alias;
        }
        else
        {
            dst = lx_fd_free_idx();
            if(dst < 0)
            {
                return -LX_ENFILE;
            }
        }
        if(console != 0u)
        {
            lx_fd_console_fill((uint32)dst);
        }
        else
        {
            lx_fd_clone((uint32)dst, (uint32)idx);
        }
        lx_std_set(newfd, dst);
        return newfd;
    }

    if((uint32)(newfd - 3) >= LX_MAX_FD)
    {
        return -LX_EBADF;
    }
    dst = newfd - 3;
    if(s_fd[dst].used != 0u)
    {
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
        if(s_fd[dst].kind == LX_FD_SOCKET)
        {
            return -LX_EINVAL;
        }
#endif
        (void)lx_fd_release(lx_proc_cur(), (uint32)dst);
    }
    if(console != 0u)
    {
        lx_fd_console_fill((uint32)dst);
    }
    else
    {
        lx_fd_clone((uint32)dst, (uint32)idx);
    }
    return newfd;
}

/* dup(23)：复制到最小的空号。失败只可能是 -EBADF 或 -ENFILE。
 * 表满为什么是 ENFILE 而不是 Linux 的 EMFILE：本层只有一张固定大小的
 * fd 表（不是内核文件表 + 进程 fd 表两层），pipe2 / openat 在同一条件下
 * 都是 -ENFILE，dup 跟着这条既定口径走，不单独造第二种回答。 */
static int32 lx_dup(void *p_ctx)
{
    return lx_fd_dup_to((int32)ARKRTOS_SVC_ARG(p_ctx, 0u), -1);
}

/* dup3(24)：复制到指定号。参数校验逐条对齐 Linux：
 *   flags 里有 O_CLOEXEC     -> 接受：新号带 FD_CLOEXEC（exec 时会被还掉）
 *   flags 里有其它位         -> -EINVAL
 *   oldfd == newfd           -> -EINVAL（这是 dup3 与 dup2 的唯一区别）
 *   newfd < 0                -> -EINVAL
 *   oldfd 无效               -> -EBADF
 *   newfd >= LX_MAX_FD+3     -> -EBADF
 *
 * O_CLOEXEC 过去是明确拒绝的（"本内核没有 exec 时的关闭语义"）。那条理由
 * 现在不成立了：execve 成功换帧之后 lx_fd_close_cloexec() 会把这个槽还回去，
 * 于是这里可以如实接受 —— 接受一个标志的前提是真的会照它做。 */
static int32 lx_dup3(void *p_ctx)
{
    int32  oldfd = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    int32  newfd = (int32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32  rc;

    if((flags & ~(uint32)LX_O_CLOEXEC) != 0u)
    {
        return -LX_EINVAL;
    }
    if(oldfd == newfd)
    {
        return -LX_EINVAL;
    }
    if(newfd < 0)
    {
        return -LX_EINVAL;
    }
    rc = lx_fd_dup_to(oldfd, newfd);
    if((rc >= 0) && ((flags & (uint32)LX_O_CLOEXEC) != 0u))
    {
        /* newfd 可能是 0..2：那时副本占的是**别名槽**，lx_fd_find 按 fd>=3
         * 查表会找不到它。别名有槽就必须一起标 CLOEXEC，否则 exec 之后
         * fd 1 还指着管道，程序以为重定向已经撤掉了。 */
        int32 idx = (rc <= 2) ? lx_std_slot(rc) : lx_fd_find(rc);

        if(idx >= 0)
        {
            s_fd[idx].cloexec = 1u;
        }
    }
    return rc;
}

/* F_DUPFD：取 >= arg 的最小的空号。0..2 只有别名才占真槽，所以搜索从
 * max(arg, 3) 起。arg 越过表上限回 -EINVAL（Linux 的 fcntl 也是 EINVAL，
 * 而 dup3 对越界 newfd 回 -EBADF —— 两条路的口径本来就不同，不并成一条）；
 * 表满回 -ENFILE，与 dup / pipe2 / openat 的既定口径一致。 */
static int32 lx_fd_dup_least(int32 oldfd, int32 arg)
{
    int32 fd;

    if(arg < 0)
    {
        return -LX_EINVAL;
    }
    for(fd = (arg < 3) ? 3 : arg; (uint32)(fd - 3) < LX_MAX_FD; fd++)
    {
        if(s_fd[fd - 3].used == 0u)
        {
            return lx_fd_dup_to(oldfd, fd);
        }
    }
    return ((uint32)(((arg < 3) ? 3 : arg) - 3) >= LX_MAX_FD) ? -LX_EINVAL : -LX_ENFILE;
}

/* F_SETFL：只改 O_APPEND 与 O_NONBLOCK（Linux 也只认这两位，其余位静默
 * 忽略，不报错）。socket 的 NONBLOCK 还同时写回 sflags，socket 层读的是那一格。 */
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
/* U-29：dup 出来的是同一份 file description —— Linux 上 F_SETFL / F_SETOWN
 * 改的就是那个对象，所有副本立刻看得见。本内核没有独立的 struct file，fd
 * 槽里存的是登记快照（dup 只是复制），所以这两个 cmd 必须把改动同步到同一
 * 进程内指向同一个管道对象的其它副本（同 kind + 同 pipe）。
 *
 * 边界：fork 之后父子各自的槽是两份拷贝，F_SETFL / F_SETOWN 不会跨进程同步
 * （要正确必须引入真正的 file 对象，见 docs/已知限制.md）。管道是本层唯一
 * 「同一对象被多个 fd 引用」的 kind；其它 kind 维持既有行为（只改本格）。 */
static void lx_fd_sync_peer(int32 idx)
{
    uint32 k;

    if((s_fd[idx].kind != LX_FD_PIPE_R) && (s_fd[idx].kind != LX_FD_PIPE_W))
    {
        return;
    }
    for(k = 0u; k < (uint32)LX_MAX_FD; k++)
    {
        if((int32)k == idx)
        {
            continue;
        }
        if(s_fd[k].used == 0u)
        {
            continue;
        }
        if((s_fd[k].kind != s_fd[idx].kind) || (s_fd[k].pipe != s_fd[idx].pipe))
        {
            continue;
        }
        s_fd[k].fl    = s_fd[idx].fl;
        s_fd[k].owner = s_fd[idx].owner;
    }
}
#endif

static void lx_fd_set_fl(int32 idx, uint32 arg)
{
    uint16 fl = s_fd[idx].fl;

    if((arg & (uint32)LX_O_APPEND) != 0u)
    {
        fl |= (uint16)LX_O_APPEND;
    }
    else
    {
        fl = (uint16)((uint32)fl & ~(uint32)LX_O_APPEND);
    }
    if((arg & (uint32)LX_O_NONBLOCK) != 0u)
    {
        fl |= (uint16)LX_O_NONBLOCK;
    }
    else
    {
        fl = (uint16)((uint32)fl & ~(uint32)LX_O_NONBLOCK);
    }
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    /* U-29：O_ASYNC 只记在这一格里；F_GETFL 读得回原样拼出来的位。 */
    if((arg & (uint32)LX_O_ASYNC) != 0u)
    {
        fl |= (uint16)LX_O_ASYNC;
    }
    else
    {
        fl = (uint16)((uint32)fl & ~(uint32)LX_O_ASYNC);
    }
#endif
    s_fd[idx].fl = fl;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    lx_fd_sync_peer(idx);       /* dup 副本是同一份 file description */
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        if((arg & (uint32)LX_O_NONBLOCK) != 0u)
        {
            s_fd[idx].sflags |= (uint8)LX_SOCKF_NONBLOCK;
        }
        else
        {
            s_fd[idx].sflags = (uint8)((uint32)s_fd[idx].sflags & ~(uint32)LX_SOCKF_NONBLOCK);
        }
    }
#endif
}

/* ------------------------------------------------------------ 记录锁（fcntl 5/6/7）
 *
 * POSIX 记录锁：按「文件 + 字节区间」加锁，所有者是**进程**，不是 fd，
 * 也不是任务。三条由此推出的语义，本层照办：
 *   * 同一进程重复加锁不会和自己冲突，只把自己那一段的旧锁换成新锁；
 *   * close 掉这个文件的**任意**一个 fd，本进程在这个文件上的锁全部释放
 *     （这是 Linux 的语义：锁属于进程，而 close 无法知道还有没有别的 fd
 *     指着同一个文件）；
 *   * 锁不随 fork 继承（子进程的槽是新的，见 lx_proc_zero），但随 execve
 *     保留（execve 不动进程槽）。
 *
 * 文件身份只有路径：本层没有 inode，同一路径就是同一个文件。硬链接、
 * bind mount 这类「不同路径、同一个文件」分不出来，写进已知限制。
 *
 * 布局按 asm-generic 的**64 位** struct flock 解析（l_type@0、l_whence@2、
 * l_start@8、l_len@16、l_pid@24，共 32 字节）——本内核的 Linux ABI 载体
 * 只有 LA64 这一种 64 位字宽；32 位 asm-generic 的 16 字节布局没有载体。
 */
#define LX_RSLK_BYTES               (32u)
#define LX_RSLK_O_TYPE              (0u)
#define LX_RSLK_O_WHENCE            (2u)
#define LX_RSLK_O_START             (8u)
#define LX_RSLK_O_LEN               (16u)
#define LX_RSLK_O_PID               (24u)
#define LX_SEEK_SET                 (0)
#define LX_SEEK_CUR                 (1)
#define LX_SEEK_END                 (2)
/* 锁表上限。满了回 -ENOLCK，不淘汰别人的锁：静默丢掉一把别人正靠着的锁，
 * 比如实拒绝糟得多。 */
#define LX_RSLK_MAX                 (8u)

typedef struct
{
    uint8  used;
    uint8  type;        /* LX_RDLCK / LX_WRLCK */
    uint16 owner;       /* 进程槽 + 1；0 = 空条目 */
    int32  pid;         /* 加锁者（F_GETLK 回填给询问方） */
    uint32 start;
    uint32 len;         /* 0 = 到 EOF */
    char   path[LX_PATH_MAX];
} lx_rslk_t;

/* 阻塞等待登记：只记「谁在等哪个 fd」，路径在唤醒那一刻从 fd 表现取。
 * 这样每条登记只有 4 字节，也省不掉正确性——fd 被关掉时等待者会被唤醒，
 * 重放那一轮看到的是 -EBADF，不会真的等丢。 */
typedef struct
{
    uint8  used;
    uint8  idx;         /* fd 表下标 */
    uint16 slot;        /* 进程槽（0 基） */
} lx_rslk_wait_t;

static lx_rslk_t      s_rslk[LX_RSLK_MAX];
static lx_rslk_wait_t s_rslk_wait[ARKRTOS_TASK_MAX_NUM];

/* 小端读写。struct flock 的四个字段都是小端（LA64 与 asm-generic 一致），
 * 而宿主与目标字宽不同，所以一律按字节取，不用 memcpy 到结构体上。 */
static uint16 lx_rslk_rd16(const uint8 *p)
{
    return (uint16)((uint16)p[0] | (uint16)((uint16)p[1] << 8));
}

static uint32 lx_rslk_rd32(const uint8 *p)
{
    return ((uint32)p[0]) | ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static void lx_rslk_wr16(uint8 *p, uint16 v)
{
    p[0] = (uint8)((uint32)v & 0xFFu);
    p[1] = (uint8)(((uint32)v >> 8) & 0xFFu);
}

static void lx_rslk_wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

/* 读一个 64 位 off_t：只有低 32 位是有效范围的整数值（本层文件大小本来
 * 就是 32 位），高 32 位必须是低 32 位的符号扩展。高 32 位给了别的值就
 * -EINVAL，绝不截断成一个「看起来合法」的假区间。 */
static int32 lx_rslk_rd_off(const uint8 *p, int32 *out)
{
    uint32 lo = lx_rslk_rd32(p);
    uint32 hi = lx_rslk_rd32(p + 4u);
    uint32 want = ((lo & 0x80000000u) != 0u) ? 0xFFFFFFFFu : 0u;

    if(hi != want)
    {
        return -LX_EINVAL;
    }
    *out = (int32)lo;
    return 0;
}

static void lx_rslk_wr_off(uint8 *p, uint32 v)
{
    lx_rslk_wr32(p, v);
    lx_rslk_wr32(p + 4u, ((v & 0x80000000u) != 0u) ? 0xFFFFFFFFu : 0u);
}

static uint8 lx_rslk_same_path(const char *a, const char *b)
{
    uint32 i = 0u;

    while(i < LX_PATH_MAX)
    {
        if(a[i] != b[i])
        {
            return 0u;
        }
        if(a[i] == 0)
        {
            return 1u;
        }
        i++;
    }
    return 0u;
}

static void lx_rslk_copy_path(char *dst, const char *src)
{
    uint32 i = 0u;

    while(((i + 1u) < LX_PATH_MAX) && (src[i] != 0))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* 区间的开区间上界（不含）。len == 0 是「到 EOF」，等价于上界 0xFFFFFFFF；
 * 加出溢出的也按到 EOF 处理——把一个真实的大区间截断成小区间，会让本该
 * 冲突的两把锁静默共存。 */
static uint32 lx_rslk_end(uint32 start, uint32 len)
{
    if(len == 0u)
    {
        return 0xFFFFFFFFu;
    }
    if(len > (0xFFFFFFFFu - start))
    {
        return 0xFFFFFFFFu;
    }
    return start + len;
}

static uint8 lx_rslk_overlap(uint32 as, uint32 ae, uint32 bs, uint32 be)
{
    return (uint8)((as < be) && (bs < ae));
}

static int32 lx_rslk_alloc(void)
{
    uint32 i;

    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        if(s_rslk[i].used == 0u)
        {
            return (int32)i;
        }
    }
    return -1;
}

/* 找一把与 [start,end) 冲突的别人的锁：只看同一个 path、有交集、且「我要
 * 写或者它要写」（两把读锁不冲突）。多把里报**起始偏移最小**的那把——
 * Linux 也是这么挑的（f_pos 最小者）。-1 = 没有冲突。 */
static int32 lx_rslk_conflict(uint16 owner, const char *path,
                              uint32 start, uint32 end, uint8 type)
{
    uint32 i;
    int32  best = -1;

    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        uint32 bs;
        uint32 be;

        if((s_rslk[i].used == 0u) || (s_rslk[i].owner == owner))
        {
            continue;
        }
        if(lx_rslk_same_path(s_rslk[i].path, path) == 0u)
        {
            continue;
        }
        bs = s_rslk[i].start;
        be = lx_rslk_end(s_rslk[i].start, s_rslk[i].len);
        if(lx_rslk_overlap(start, end, bs, be) == 0u)
        {
            continue;
        }
        if((type != (uint8)LX_WRLCK) && (s_rslk[i].type != (uint8)LX_WRLCK))
        {
            continue;
        }
        if((best < 0) || (bs < s_rslk[(uint32)best].start))
        {
            best = (int32)i;
        }
    }
    return best;
}

/* 把 owner 在 path 上与 [start,end) 相交的那部分切掉：整段覆盖就整条删，
 * 砍头/砍尾就缩短，中间开洞就劈成两条。劈裂要空位，所以**先数清楚**，
 * 不够就回 -ENOLCK 且一个字节都不改（不能留下半截状态）。 */
static int32 lx_rslk_cut(uint16 owner, const char *path, uint32 start, uint32 end)
{
    uint32 i;
    uint32 free_n = 0u;
    uint32 need   = 0u;

    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        uint32 bs;
        uint32 be;

        if(s_rslk[i].used == 0u)
        {
            free_n++;
            continue;
        }
        if((s_rslk[i].owner != owner) ||
           (lx_rslk_same_path(s_rslk[i].path, path) == 0u))
        {
            continue;
        }
        bs = s_rslk[i].start;
        be = lx_rslk_end(s_rslk[i].start, s_rslk[i].len);
        if(lx_rslk_overlap(start, end, bs, be) == 0u)
        {
            continue;
        }
        if((bs < start) && (end < be))
        {
            need++;
        }
    }
    if(need > free_n)
    {
        return -LX_ENOLCK;
    }
    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        uint32 bs;
        uint32 be;

        if((s_rslk[i].used == 0u) || (s_rslk[i].owner != owner) ||
           (lx_rslk_same_path(s_rslk[i].path, path) == 0u))
        {
            continue;
        }
        bs = s_rslk[i].start;
        be = lx_rslk_end(s_rslk[i].start, s_rslk[i].len);
        if(lx_rslk_overlap(start, end, bs, be) == 0u)
        {
            continue;
        }
        if((bs < start) && (end < be))
        {
            int32 f = lx_rslk_alloc();

            if(f < 0)
            {
                return -LX_ENOLCK;      /* 上面已数过，这里不该发生 */
            }
            s_rslk[(uint32)f] = s_rslk[i];
            s_rslk[(uint32)f].start = end;
            s_rslk[(uint32)f].len   = (be == 0xFFFFFFFFu) ? 0u : (be - end);
            s_rslk[i].len = start - bs;
        }
        else if(bs < start)
        {
            s_rslk[i].len = start - bs;             /* 砍尾 */
        }
        else if(end < be)
        {
            s_rslk[i].start = end;                  /* 砍头 */
            s_rslk[i].len   = (be == 0xFFFFFFFFu) ? 0u : (be - end);
        }
        else
        {
            s_rslk[i].used = 0u;                    /* 整条删 */
        }
    }
    return 0;
}

/* 加一把锁。语义按 POSIX：先把自己在相交区间里的旧锁切掉，再放进新锁。
 * 空位要够「自己那把 + 劈裂需要的」，不够就 -ENOLCK（先数后改）。 */
static int32 lx_rslk_install(uint16 owner, int32 pid, const char *path,
                             uint32 start, uint32 len, uint8 type)
{
    uint32 i;
    uint32 free_n = 0u;
    uint32 need   = 0u;
    uint32 end    = lx_rslk_end(start, len);
    int32  f;
    int32  rc;

    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        uint32 bs;
        uint32 be;

        if(s_rslk[i].used == 0u)
        {
            free_n++;
            continue;
        }
        if((s_rslk[i].owner != owner) ||
           (lx_rslk_same_path(s_rslk[i].path, path) == 0u))
        {
            continue;
        }
        bs = s_rslk[i].start;
        be = lx_rslk_end(s_rslk[i].start, s_rslk[i].len);
        if(lx_rslk_overlap(start, end, bs, be) == 0u)
        {
            continue;
        }
        if((bs < start) && (end < be))
        {
            need++;                     /* 劈裂：多占一格 */
        }
        else if((bs >= start) && (be <= end))
        {
            free_n++;                   /* 整条被替换：腾出一格 */
        }
    }
    if(free_n < (need + 1u))
    {
        return -LX_ENOLCK;
    }
    rc = lx_rslk_cut(owner, path, start, end);
    if(rc != 0)
    {
        return rc;
    }
    f = lx_rslk_alloc();
    if(f < 0)
    {
        return -LX_ENOLCK;              /* 数过的账，不该发生 */
    }
    s_rslk[(uint32)f].used  = 1u;
    s_rslk[(uint32)f].type  = type;
    s_rslk[(uint32)f].owner = owner;
    s_rslk[(uint32)f].pid   = pid;
    s_rslk[(uint32)f].start = start;
    s_rslk[(uint32)f].len   = len;
    lx_rslk_copy_path(s_rslk[(uint32)f].path, path);
    return 0;
}

/* 叫醒在 path 上等着的全部任务。与管道同一套约定：唤醒只负责叫人，判定
 * 留在重放那一轮——条件可能在唤醒与重放之间又被别人改掉。 */
static void lx_rslk_wake(const char *path)
{
    int32  i;
    uint32 crit;

    crit = ARKRTOS_ENTER_CRITICAL();
    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        lx_rslk_wait_t *p_w = &s_rslk_wait[i - 1];
        uint32 slot = (uint32)p_w->slot;

        if((p_w->used == 0u) || (slot >= (uint32)LX_PROC_MAX) ||
           ((uint32)p_w->idx >= LX_MAX_FD))
        {
            continue;
        }
        if(s_proc[slot].fd[p_w->idx].used == 0u)
        {
            continue;
        }
        if(lx_rslk_same_path(s_proc[slot].fd[p_w->idx].path, path) == 0u)
        {
            continue;
        }
        (void)arkrtos_task_wakeup_internal(i);
    }
    ARKRTOS_EXIT_CRITICAL(crit);
}

/* 无差别的全唤醒：进程槽被重置时可能一次放掉好几个文件的锁，逐个按路径
 * 叫醒要做多轮扫描，收益不值得。伪唤醒无害——重放那一轮会自己判条件。 */
static void lx_rslk_wake_all(void)
{
    int32  i;
    uint32 crit = ARKRTOS_ENTER_CRITICAL();

    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if(s_rslk_wait[i - 1].used != 0u)
        {
            (void)arkrtos_task_wakeup_internal(i);
        }
    }
    ARKRTOS_EXIT_CRITICAL(crit);
}

/* 本进程关掉某个文件的任意一个 fd：它在这个文件上的锁全部释放（POSIX）。 */
static void lx_rslk_release_path(uint32 slot, const char *path)
{
    uint32 i;
    uint32 n = 0u;
    uint16 owner;
    uint32 crit;

    if((slot >= (uint32)LX_PROC_MAX) || (path == 0) || (path[0] == 0))
    {
        return;
    }
    owner = (uint16)(slot + 1u);
    crit = ARKRTOS_ENTER_CRITICAL();
    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        if((s_rslk[i].used != 0u) && (s_rslk[i].owner == owner) &&
           (lx_rslk_same_path(s_rslk[i].path, path) != 0u))
        {
            s_rslk[i].used = 0u;
            n++;
        }
    }
    ARKRTOS_EXIT_CRITICAL(crit);
    if(n != 0u)
    {
        lx_rslk_wake(path);
    }
}

/* 进程槽被重置：这个槽名下的锁全部消失。不清的话下一次槽位复用会让新
 * 进程莫名其妙地"继承"一堆谁也解不开的锁。 */
static void lx_rslk_release_owner(uint32 slot)
{
    uint32 i;
    uint32 n = 0u;
    uint16 owner;
    uint32 crit;

    if(slot >= (uint32)LX_PROC_MAX)
    {
        return;
    }
    owner = (uint16)(slot + 1u);
    crit = ARKRTOS_ENTER_CRITICAL();
    for(i = 0u; i < LX_RSLK_MAX; i++)
    {
        if((s_rslk[i].used != 0u) && (s_rslk[i].owner == owner))
        {
            s_rslk[i].used = 0u;
            n++;
        }
    }
    ARKRTOS_EXIT_CRITICAL(crit);
    if(n != 0u)
    {
        lx_rslk_wake_all();
    }
}

/* 撤掉当前任务的等待登记。每一次「这次调用真的结束了」都要走一遍，
 * 否则下一次系统调用会被误认成重放。 */
static void lx_rslk_forget(int32 me)
{
    uint32 crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    s_rslk_wait[me - 1].used = 0u;
    ARKRTOS_EXIT_CRITICAL(crit);
}

/* 挂起一个时间片，返回 LX_RET_RETRY（与 lx_pipe_sleep 同一条约定：参数
 * 寄存器原样留给重放的那一次）。 */
static int32 lx_rslk_sleep(void *p_ctx, int32 me, int32 slot, int32 idx)
{
    uint32 crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EAGAIN;          /* 没有任务身份：不假装睡过 */
    }
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        return -LX_ENOSYS;          /* 端口不能重放：如实拒绝 */
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    s_rslk_wait[me - 1].used = 1u;
    s_rslk_wait[me - 1].slot = (uint16)slot;
    s_rslk_wait[me - 1].idx  = (uint8)idx;
    ARKRTOS_EXIT_CRITICAL(crit);
    (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
    return LX_RET_RETRY;
}

/* 把 whence 相对的偏移换成绝对偏移。负的结果、加过 32 位的结果都回 -EINVAL
 * （Linux 对 l_start + l_len < 0 也是 EINVAL），不截断成假区间。 */
static int32 lx_rslk_abs(uint32 base, int32 rel, uint32 *out)
{
    if(rel < 0)
    {
        uint32 back = (uint32)0u - (uint32)rel;

        if(back > base)
        {
            return -LX_EINVAL;
        }
        *out = base - back;
        return 0;
    }
    if((uint32)rel > (0xFFFFFFFFu - base))
    {
        return -LX_EINVAL;
    }
    *out = base + (uint32)rel;
    return 0;
}

/* fcntl 的记录锁三档。idx 已经是本进程 fd 表的下标（-1 = 控制台本体，
 * 不能加锁）。返回值可能是 LX_RET_RETRY（F_SETLKW 挂起）。 */
static int32 lx_fcntl_lock(void *p_ctx, int32 slot, int32 idx, uint32 cmd,
                           unsigned long uaddr)
{
    uint8    lbuf[LX_RSLK_BYTES];
    lx_fd_t *p_fd;
    int32    me = (int32)arkrtos_current_task_id;
    int32    start32;
    int32    len32;
    int32    rc;
    int32    hit;
    uint32   type;
    uint32   whence;
    uint32   base;
    uint32   start;
    uint32   len;
    uint32   end;
    uint16   owner;

    if((slot < 0) || ((uint32)slot >= (uint32)LX_PROC_MAX))
    {
        return -LX_ENOMEM;          /* 进程槽用满：不拿别人的槽当自己 */
    }
    if((idx < 0) || ((uint32)idx >= LX_MAX_FD))
    {
        return -LX_EINVAL;          /* 控制台本体没有文件可锁 */
    }
    p_fd = &s_proc[(uint32)slot].fd[(uint32)idx];
    if((p_fd->used == 0u) || (p_fd->kind != LX_FD_FILE) || (p_fd->path[0] == 0))
    {
        lx_rslk_forget(me);
        return -LX_EINVAL;          /* 管道 / 套接字 / 控制台：如实拒绝 */
    }
    if(uaddr == 0ul)
    {
        lx_rslk_forget(me);
        return -LX_EFAULT;
    }
    if(arkrtos_copy_from_user(lbuf, (const void *)uaddr, LX_RSLK_BYTES) != 0)
    {
        lx_rslk_forget(me);
        return -LX_EFAULT;
    }
    type   = (uint32)lx_rslk_rd16(&lbuf[LX_RSLK_O_TYPE]);
    whence = (uint32)lx_rslk_rd16(&lbuf[LX_RSLK_O_WHENCE]);
    if(type > (uint32)LX_UNLCK)
    {
        lx_rslk_forget(me);
        return -LX_EINVAL;
    }
    if((cmd == LX_F_GETLK) && (type == (uint32)LX_UNLCK))
    {
        /* Linux：问「有一把解锁的锁吗」没有意义，直接 EINVAL，
         * 不回一把假锁。 */
        lx_rslk_forget(me);
        return -LX_EINVAL;
    }
    if(whence > (uint32)LX_SEEK_END)
    {
        lx_rslk_forget(me);
        return -LX_EINVAL;
    }
    rc = lx_rslk_rd_off(&lbuf[LX_RSLK_O_START], &start32);
    if(rc != 0)
    {
        lx_rslk_forget(me);
        return rc;
    }
    rc = lx_rslk_rd_off(&lbuf[LX_RSLK_O_LEN], &len32);
    if(rc != 0)
    {
        lx_rslk_forget(me);
        return rc;
    }
    if(len32 < 0)
    {
        lx_rslk_forget(me);
        return -LX_EINVAL;          /* 负长度没有意义 */
    }
    base = 0u;
    if(whence == (uint32)LX_SEEK_CUR)
    {
        base = p_fd->off;
    }
    else if(whence == (uint32)LX_SEEK_END)
    {
        base = p_fd->size;
    }
    rc = lx_rslk_abs(base, start32, &start);
    if(rc != 0)
    {
        lx_rslk_forget(me);
        return rc;
    }
    len   = (uint32)len32;
    end   = lx_rslk_end(start, len);
    owner = (uint16)((uint32)slot + 1u);

    if(cmd == LX_F_GETLK)
    {
        hit = lx_rslk_conflict(owner, p_fd->path, start, end, (uint8)type);
        if(hit < 0)
        {
            /* 无冲突：只把 l_type 改成 F_UNLCK，其余四格不动（Linux 也不动）。 */
            lx_rslk_wr16(&lbuf[LX_RSLK_O_TYPE], (uint16)LX_UNLCK);
        }
        else
        {
            lx_rslk_wr16(&lbuf[LX_RSLK_O_TYPE], (uint16)s_rslk[(uint32)hit].type);
            lx_rslk_wr16(&lbuf[LX_RSLK_O_WHENCE], (uint16)LX_SEEK_SET);
            lx_rslk_wr_off(&lbuf[LX_RSLK_O_START], s_rslk[(uint32)hit].start);
            lx_rslk_wr_off(&lbuf[LX_RSLK_O_LEN], s_rslk[(uint32)hit].len);
            lx_rslk_wr32(&lbuf[LX_RSLK_O_PID], (uint32)s_rslk[(uint32)hit].pid);
        }
        lx_rslk_forget(me);
        if(arkrtos_copy_to_user((void *)uaddr, lbuf, LX_RSLK_BYTES) != 0)
        {
            return -LX_EFAULT;
        }
        return 0;
    }
    if(type == (uint32)LX_UNLCK)
    {
        rc = lx_rslk_cut(owner, p_fd->path, start, end);
        lx_rslk_forget(me);
        if(rc != 0)
        {
            return rc;
        }
        /* 解锁可能放开一个正在等的写者：定向叫醒这个文件上的等待者。 */
        lx_rslk_wake(p_fd->path);
        return 0;
    }
    hit = lx_rslk_conflict(owner, p_fd->path, start, end, (uint8)type);
    if(hit >= 0)
    {
        if(cmd == LX_F_SETLK)
        {
            lx_rslk_forget(me);
            return -LX_EAGAIN;
        }
        if(lx_sig_pending_unblocked() != 0u)
        {
            lx_rslk_forget(me);
            return -LX_EINTR;
        }
        return lx_rslk_sleep(p_ctx, me, slot, idx);
    }
    rc = lx_rslk_install(owner, me, p_fd->path, start, len, (uint8)type);
    lx_rslk_forget(me);
    return rc;
}

#if (ARKRTOS_USE_LINUX_SIGIO == 1)
/* U-29 的两条 cmd。语义写死在这里，不靠读者从行为里猜：
 *   F_SETOWN(fd, 0)     主人是调用者自己（Linux 同义）；
 *   F_SETOWN(fd, pid>0) 主人是这个任务号。**不校验存活**：Linux 也是在投递
 *                       那一刻才看得出「主人没了」，提前退 -ESRCH 会让
 *                       「先设主人、稍后才 fork 出主人」这种顺序直接失败；
 *   F_SETOWN(fd, pid<0) 进程组。内核里没有进程组，回 -EINVAL 并单独计数 ——
 *                       当成自己是最坏的一种「看起来成功了」：信号会发错人；
 *   F_GETOWN            回主人，没有主人回 0。
 * 0..2 没有别名时是控制台本体、没有槽可记：F_SETOWN 回 -EINVAL（不假装记下了
 * 一个永远不会生效的主人），F_GETOWN 回 0。
 */
static int32 lx_fd_owner_cmd(int32 idx, uint32 cmd, int32 who)
{
    if(cmd == (uint32)LX_F_GETOWN)
    {
        return (idx < 0) ? 0 : (int32)s_fd[idx].owner;
    }
    if(idx < 0)
    {
        return -LX_EINVAL;
    }
    if(who < 0)
    {
        s_sigio_pgrp_refused++;
        return -LX_EINVAL;
    }
    if(who == 0)
    {
        who = (int32)arkrtos_current_task_id;
    }
    s_fd[idx].owner = who;
    lx_fd_sync_peer(idx);       /* dup 副本是同一份 file description */
    return 0;
}
#endif

/* fcntl(25)：本层认两组 cmd —— fd 标志那五个（F_DUPFD / F_DUPFD_CLOEXEC /
 * F_GETFD / F_SETFD / F_GETFL / F_SETFL）与记录锁那三个（F_GETLK / F_SETLK /
 * F_SETLKW），其余 -EINVAL（Linux 对不认识的 cmd 也是 EINVAL，不回 ENOSYS）。
 *
 * 四处必须说清楚的边界：
 *   * 0..2 没有别名时它们是「控制台对象」而不是槽：F_GETFD 回 0、F_SETFD
 *     回 0 但不记 FD_CLOEXEC（没有槽可还，exec 之后控制台照旧在）、
 *     F_GETFL 回 O_RDWR|O_NONBLOCK。
 *   * 管道两端的 O_NONBLOCK 可以自由置/清（U-26）：置位是能读/写多少算
 *     多少 + -EAGAIN，清零是阻塞（读空、写满时挂起等对侧）。Linux 里
 *     「先把两端设成非阻塞、发送前再清掉」是常见的惯用法，两边都要认。
 *   * 文件 fd 的 O_APPEND 只被记录下来（F_GETFL 能读回）：本层文件写仍
 *     -ENOSYS，不会因为它变成"能追加"，所以也不假装写成功了。
 *   * 记录锁只落在**普通文件**上（管道/套接字/控制台 -EINVAL），所有者是
 *     进程槽，文件身份是路径。F_SETLKW 挂起按 LX_WAIT_SLICE_MS 时间片
 *     重放，没有死锁检测（Linux 会回 -EDEADLK 的那种情形本层会一直等），
 *     也没有 OFD 锁与租约（F_OFD_* / F_SETLEASE 回 -EINVAL）。 */
static int32 lx_fcntl(void *p_ctx)
{
    int32  fd  = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32 cmd = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long arg = ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32  idx = -1;
    int32  rc;

    if((fd >= 0) && (fd <= 2))
    {
        idx = lx_std_slot(fd);      /* -1 = 没有别名的标准流（控制台本体） */
    }
    else
    {
        idx = lx_fd_find(fd);
        if(idx < 0)
        {
            return -LX_EBADF;
        }
    }

    switch(cmd)
    {
    case LX_F_DUPFD:
    case LX_F_DUPFD_CLOEXEC:
        rc = lx_fd_dup_least(fd, (int32)arg);
        if((rc >= 0) && (cmd == (uint32)LX_F_DUPFD_CLOEXEC))
        {
            int32 d = lx_fd_find(rc);

            if(d >= 0)
            {
                s_fd[d].cloexec = 1u;
            }
        }
        return rc;
    case LX_F_GETFD:
        return ((idx >= 0) && (s_fd[idx].cloexec != 0u)) ? (int32)LX_FD_CLOEXEC : 0;
    case LX_F_SETFD:
        if(idx >= 0)
        {
            s_fd[idx].cloexec = ((arg & (unsigned long)LX_FD_CLOEXEC) != 0ul) ? 1u : 0u;
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
            if(s_fd[idx].kind == LX_FD_SOCKET)
            {
                if(s_fd[idx].cloexec != 0u)
                {
                    s_fd[idx].sflags |= (uint8)LX_SOCKF_CLOEXEC;
                }
                else
                {
                    s_fd[idx].sflags = (uint8)((uint32)s_fd[idx].sflags & ~(uint32)LX_SOCKF_CLOEXEC);
                }
            }
#endif
        }
        return 0;
    case LX_F_GETFL:
        if(idx < 0)
        {
            return (int32)((uint32)LX_O_RDWR | (uint32)LX_O_NONBLOCK);
        }
        return (int32)s_fd[idx].fl;
    case LX_F_SETFL:
        if(idx < 0)
        {
            /* 控制台对象本来就不阻塞：只接受"要求不阻塞"这一种写法。 */
            return ((arg & (unsigned long)LX_O_NONBLOCK) != 0ul) ? 0 : -LX_EINVAL;
        }
        lx_fd_set_fl(idx, (uint32)arg);
        return 0;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    case LX_F_SETOWN:
    case LX_F_GETOWN:
        return lx_fd_owner_cmd(idx, cmd, (int32)arg);
#endif
    case LX_F_GETLK:
    case LX_F_SETLK:
    case LX_F_SETLKW:
        {
            int32 slot = (int32)lx_proc_cur();

            if(slot >= (int32)LX_PROC_MAX)
            {
                return -LX_ENOMEM;
            }
            return lx_fcntl_lock(p_ctx, slot, idx, cmd, arg);
        }
    default:
        return -LX_EINVAL;
    }
}

/* ---------------------------------------------------------------- 信号 */

/* 这一档只做**同一任务自我投递**：kill / tkill / tgkill 指向别人一律
 * -ESRCH（没有第二个进程可投），投递点只在系统调用返回用户态之前——所以
 * 「被打断的代码」必然停在一条 syscall 之后，寄存器里活着的就是 trap 帧
 * 保存的那一份，不需要在任意时刻抢占用户态。
 *
 * 信号帧铺在用户栈上，返回地址指向程序自己给的 restorer；restorer 发
 * rt_sigreturn，内核把帧里的 era/sp/ra/tp/a0 与屏蔽字原样装回去。没有
 * restorer 就不投递（装处理函数时直接 -EINVAL）——不假装能回到被打断处。
 *
 * 明确不做：实时信号（32..64）、跨任务投递、信号处理期间的自动重入保护
 * 之外的语义（SA_NODEFER 只按字面记 flags，不实现）、作业控制（SIGSTOP
 * 一类默认动作是挂起，这里按忽略处理并单独计数）、SIGPIPE 的默认动作
 * （管道写已经直接回 -EPIPE，不发信号）。
 */

static uint32 lx_sig_bit(uint32 sig)
{
    return (uint32)1u << (sig - 1u);
}

/* 置一个待投递信号位。投递点仍然是「系统调用返回用户态之前」，所以这里
 * 只登记：默认动作结束任务（退出码 128 + signo）、装了处理器就进处理函数、
 * 忽略则只记一次。管道写端无人读时的 SIGPIPE（13）走的就是这一条 ——
 * 写回去的 -EPIPE 与 Linux 一致，先置位再返回，没有处理函数时任务就结束。 */
static void lx_sig_raise(uint32 sig)
{
    if((sig == 0u) || (sig >= (uint32)LX_NSIG))
    {
        return;
    }
    (void)lx_proc_cur();            /* 保证当前任务的进程槽存在 */
    s_sig_pending |= lx_sig_bit(sig);
}

/* Linux 里默认动作是「忽略」的几个。 */
static uint32 lx_sig_default_ignored(uint32 sig)
{
    if((sig == 17u) || (sig == 18u) || (sig == 23u) || (sig == 28u))
    {
        return 1u;
    }
    return 0u;
}

/* 默认动作是「挂起」的几个：本内核没有作业控制，停不了任务，按忽略处理
 * 并单独计数——不把「没法实现」说成「已经处理」。 */
static uint32 lx_sig_default_stops(uint32 sig)
{
    if((sig == 19u) || (sig == 20u) || (sig == 21u) || (sig == 22u))
    {
        return 1u;
    }
    return 0u;
}

/* 装/查动作。用户结构按 UAPI loongarch64 的 32 字节布局读：
 * { handler, flags, restorer, mask(8 B) }，sigsetsize 必须是 8。 */
static int32 lx_rt_sigaction(void *p_ctx)
{
    uint32 sig   = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long u_act = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long u_old = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32 setsz = ARKRTOS_SVC_ARG(p_ctx, 3u);
    unsigned long act[4];
    unsigned long old[4];

    if(setsz != (uint32)LX_SIGSET_BYTES)
    {
        return -LX_EINVAL;
    }
    if((sig == 0u) || (sig >= (uint32)LX_NSIG) ||
       (sig == (uint32)LX_SIGKILL) || (sig == (uint32)LX_SIGSTOP))
    {
        return -LX_EINVAL;      /* SIGKILL / SIGSTOP 不可捕获，与 Linux 一致 */
    }

    /* 端口能力探测：没有「读写用户 trap 帧」的 port 直接回 -ENOSYS，
     * 而不是让程序装上一个永远投递不到的处理函数。写回的是读到的原值，
     * 这一步不改任何寄存器。 */
    if(arkrtos_port_svc_set_user_reg(p_ctx, LX_UREG_ERA,
            arkrtos_port_svc_user_reg(p_ctx, LX_UREG_ERA)) != 0)
    {
        return -LX_ENOSYS;
    }

    if(u_act != 0UL)
    {
        if(arkrtos_copy_from_user(act, (const void *)(unsigned long)u_act,
                                  (uint32)sizeof(act)) != 0)
        {
            return -LX_EFAULT;
        }
        if((act[0] != LX_SIG_DFL) && (act[0] != LX_SIG_IGN) && (act[2] == 0UL))
        {
            /* 没有 restorer 就没有「处理完回到哪」：拒绝，不猜一个地址。 */
            s_sig_no_restorer++;
            return -LX_EINVAL;
        }
    }

    if(u_old != 0UL)
    {
        old[0] = s_sig[sig].handler;
        old[1] = s_sig[sig].flags;
        old[2] = s_sig[sig].restorer;
        old[3] = s_sig[sig].mask;
        if(arkrtos_copy_to_user((void *)(unsigned long)u_old, old,
                                (uint32)sizeof(old)) != 0)
        {
            return -LX_EFAULT;
        }
    }

    if(u_act != 0UL)
    {
        s_sig[sig].handler  = act[0];
        s_sig[sig].flags    = act[1];
        s_sig[sig].restorer = act[2];
        s_sig[sig].mask     = act[3];
    }
    return 0;
}

/* 屏蔽字的查询与修改。SIGKILL / SIGSTOP 永远屏蔽不了（Linux 同样如此）。 */
static int32 lx_rt_sigprocmask(void *p_ctx)
{
    uint32 how       = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long u_set = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long u_old = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32 setsz     = ARKRTOS_SVC_ARG(p_ctx, 3u);
    uint32 old = s_sig_blocked;
    unsigned long v;
    uint32 m;

    if(setsz != (uint32)LX_SIGSET_BYTES)
    {
        return -LX_EINVAL;
    }
    if(how > (uint32)LX_SIG_SETMASK)
    {
        return -LX_EINVAL;
    }

    if(u_old != 0UL)
    {
        v = (unsigned long)old;
        if(arkrtos_copy_to_user((void *)(unsigned long)u_old, &v,
                                (uint32)sizeof(v)) != 0)
        {
            return -LX_EFAULT;
        }
    }

    if(u_set != 0UL)
    {
        if(arkrtos_copy_from_user(&v, (const void *)(unsigned long)u_set,
                                  (uint32)sizeof(v)) != 0)
        {
            return -LX_EFAULT;
        }
        m = (uint32)(v) & 0x7FFFFFFFu;
        if(how == (uint32)LX_SIG_BLOCK)
        {
            s_sig_blocked |= m;
        }
        else if(how == (uint32)LX_SIG_UNBLOCK)
        {
            s_sig_blocked &= ~m;
        }
        else
        {
            s_sig_blocked = m;
        }
        s_sig_blocked &= ~(lx_sig_bit((uint32)LX_SIGKILL) |
                           lx_sig_bit((uint32)LX_SIGSTOP));
    }
    return 0;
}

/* kill / tkill / tgkill 共用一个实体：本内核只有一个可投递的对象——自己。 */
static int32 lx_sig_send(int32 pid, uint32 sig)
{
    if(pid != (int32)arkrtos_current_task_id)
    {
#if (ARKRTOS_USE_LINUX_FORK == 1)
        int32  slot;

        /* 跨进程投递：目标必须是「本内核认识的、还活着的」那个任务，而且
         * 它自己已经有一份进程状态（fork 出来的子一定绑定了槽）。只置
         * pending，不动目标的寄存器与栈；真正的铺帧仍在目标自己下一次
         * 系统调用返回时做（屏蔽字与 handler 都是它那一份）。
         * 目标正卡在 wait4 的阻塞里时顺手唤醒它，让它在这次调用的返回
         * 路径上就拿到信号；目标在别处等待/正在跑时不打扰。 */
        if((pid > 0) && (pid <= (int32)ARKRTOS_TASK_MAX_NUM))
        {
            slot = lx_proc_find(pid);
            if((slot >= 0) &&
               (arkrtos_task_table[pid - 1].status != ARKRTOS_TASK_INVALID))
            {
                uint32 crit;

                if(sig == 0u)
                {
                    return 0;       /* 只做存在性探测，与 Linux 一致 */
                }
                if(sig >= (uint32)LX_NSIG)
                {
                    return -LX_EINVAL;
                }
                crit = ARKRTOS_ENTER_CRITICAL();
                s_proc[(uint32)slot].sig_pending |= lx_sig_bit(sig);
                if(s_lx_wait[pid - 1] != 0u)
                {
                    (void)arkrtos_task_wakeup_internal(pid);
                }
                ARKRTOS_EXIT_CRITICAL(crit);
                return 0;
            }
        }
#endif
        s_sig_refused++;
        return -LX_ESRCH;
    }
    if(sig == 0u)
    {
        return 0;       /* 只做存在性探测，与 Linux 一致 */
    }
    if(sig >= (uint32)LX_NSIG)
    {
        return -LX_EINVAL;
    }
    s_sig_pending |= lx_sig_bit(sig);
    return 0;
}

static int32 lx_kill(void *p_ctx)
{
    return lx_sig_send((int32)ARKRTOS_SVC_ARG(p_ctx, 0u),
                       (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u));
}

static int32 lx_tkill(void *p_ctx)
{
    return lx_sig_send((int32)ARKRTOS_SVC_ARG(p_ctx, 0u),
                       (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u));
}

/* tgkill(tgid, tid, sig)：两个号都必须是自己；只有一个任务，两者相等。 */
static int32 lx_tgkill(void *p_ctx)
{
    int32 tgid = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    int32 tid  = (int32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 sig = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);

    if(tgid != tid)
    {
        s_sig_refused++;
        return -LX_ESRCH;
    }
    return lx_sig_send(tid, sig);
}

/* 从 pending 里挑一个可投递信号，并按 action 处理「忽略 / 默认忽略 / 默认停止 /
 * 默认结束」这几种不需要铺帧的情形。
 * 返回：0 = 无需投递（或被忽略、或没有可投递信号），调用方直接返回 0；
 *       1 = 任务已被默认动作结束，调用方返回 1；
 *     >1 = 选中信号号（pending 位已清，sa 经出参带出）。 */
static uint32 lx_sig_pick(lx_sigaction_t **p_sa)
{
    uint32 sig;
    uint32 ready;
    lx_sigaction_t *sa;

    ready = s_sig_pending & ~s_sig_blocked;
    if(ready == 0u)
    {
        return 0u;
    }
    for(sig = 1u; sig < (uint32)LX_NSIG; sig++)
    {
        if((ready & lx_sig_bit(sig)) != 0u)
        {
            break;
        }
    }
    if(sig >= (uint32)LX_NSIG)
    {
        return 0u;
    }
    sa = &s_sig[sig];
    s_sig_pending &= ~lx_sig_bit(sig);

    if(sa->handler == LX_SIG_IGN)
    {
        s_sig_ignored++;
        return 0u;
    }
    if(sa->handler == LX_SIG_DFL)
    {
        if(lx_sig_default_ignored(sig) != 0u)
        {
            s_sig_ignored++;
            return 0u;
        }
        if(lx_sig_default_stops(sig) != 0u)
        {
            s_sig_stop_ignored++;
            return 0u;
        }
        /* 其余默认动作是「结束任务」。内核里唯一能表达这件事的是退出码
         * 128 + signo（Linux 把它写进 wait 状态，这里没有 wait 状态）。 */
        #if (ARKRTOS_USE_EXIT_CODE == 1)
        arkrtos_thread_exit_code_internal((int32)(128u + sig));
        #else
        arkrtos_thread_exit_internal();
        #endif
        return 1u;
    }

    *p_sa = sa;
    return sig;
}

/* 把整帧铺到用户栈上（头 + 被打断点快照）。
 * 返回 0 = 铺好（帧址经出参带出）；1 = 本次不投递（pending 位已放回，调用方返回 0）。 */
static uint32 lx_sig_build_frame(void *p_ctx, uint32 sig, unsigned long *p_fsp)
{
    uint32 fbytes;
    uint32 total;
    uint32 saved_blocked;
    unsigned long fsp;
    unsigned long insn;
    lx_sigaction_t *sa = &s_sig[sig];

    /* 整帧快照：端口报不出帧长（或报得比缓冲还大）就不投递，pending 位
     * 留着，下一次系统调用返回时再试——绝不截断快照。 */
    fbytes = arkrtos_port_svc_frame_bytes();
    if((fbytes == 0u) || (fbytes > (uint32)(LX_SIGFRAME_MAX_WORDS * 8u)))
    {
        s_sig_refused++;
        s_sig_pending |= lx_sig_bit(sig);
        return 1u;
    }
    total = LX_SIGFRAME_HDR_BYTES + fbytes;

    if(((sa->handler & 3UL) != 0UL) ||
       (arkrtos_copy_from_user(&insn, (const void *)(unsigned long)sa->handler,
                               4u) != 0))
    {
        /* handler 取不到指令：不投递，pending 位放回去，
         * 绝不把 era 指向一个读不出来的地址。 */
        s_sig_pending |= lx_sig_bit(sig);
        return 1u;
    }

    /* 帧址由端口给：本 port 没有独立的异常处理栈，系统调用处理链就跑在
     * 用户栈下面，位置算错会把内核自己的返回地址盖掉（实测症状是复制完
     * 之后 PC 变 0）。具体怎么让开由端口负责，这一层不自己算地址。 */
    fsp = arkrtos_port_svc_frame_reserve(p_ctx, total);
    if(fsp == 0UL)
    {
        s_sig_pending |= lx_sig_bit(sig);   /* 栈不够：留着，下次再试 */
        return 1u;
    }
    saved_blocked = s_sig_blocked;

    /* 先把**被打断的整帧**存进内核缓冲，再把它和头一起铺到用户栈上。
     * 顺序不能反过来：写 era/sp/ra 之后再导出，存下来的就是处理函数的
     * 现场而不是被打断点的现场。 */
    s_sigframe[0] = LX_SIGFRAME_MAGIC;
    s_sigframe[1] = (unsigned long)sig;
    s_sigframe[2] = (unsigned long)saved_blocked;
    s_sigframe[3] = (unsigned long)fbytes;
    arkrtos_port_svc_frame_export(p_ctx, &s_sigframe[LX_SIGFRAME_HDR_WORDS]);

    if(arkrtos_copy_to_user((void *)(unsigned long)fsp, s_sigframe,
                            total) != 0)
    {
        s_sig_pending |= lx_sig_bit(sig);
        return 1u;
    }

    *p_fsp = fsp;
    return 0u;
}

/* 改上下文进 handler：屏蔽字加上本信号与 sa->mask（永远不含 KILL/STOP）。
 * 任一处 set_user_reg 失败就回滚屏蔽字并还回 pending 位。
 * 返回 0 = 已进入 handler，1 = 没进去（调用方返回 0）。 */
static uint32 lx_sig_install(void *p_ctx, uint32 sig, lx_sigaction_t *sa,
                             unsigned long fsp)
{
    uint32 saved_blocked = s_sig_blocked;

    s_sig_blocked |= lx_sig_bit(sig) | (uint32)(sa->mask & 0x7FFFFFFFUL);
    s_sig_blocked &= ~(lx_sig_bit((uint32)LX_SIGKILL) |
                       lx_sig_bit((uint32)LX_SIGSTOP));

    /* 顺序有讲究：先把 ra/sp 写好，最后才写 era。era 一改，下一次返回用户态
     * 就跳进 handler；任何一步失败都不能留下「era 已指向 handler、sp 还没指
     * 向帧」的半截现场，所以唯一的跳转点排在最后（此前 era 先写，SP 失败时
     * 会留下半个现场，宿主用例抓到过）。 */
    if((arkrtos_port_svc_set_user_reg(p_ctx, LX_UREG_RA, sa->restorer) != 0) ||
       (arkrtos_port_svc_set_user_reg(p_ctx, LX_UREG_SP, fsp) != 0) ||
       (arkrtos_port_svc_set_user_reg(p_ctx, LX_UREG_ERA, sa->handler) != 0))
    {
        s_sig_blocked = saved_blocked;
        s_sig_pending |= lx_sig_bit(sig);
        return 1u;
    }

    ARKRTOS_SVC_RET(p_ctx, (uint32)sig);    /* handler 的第一个参数是信号号 */
    s_sig_delivered++;
    return 0u;
}

/* ------------------------------------------------ rseq(293) / getcpu(168)
 *
 * rseq 是可重启序列的注册面：glibc 起运行时问一次这个号，成功就从
 * __rseq_abi.cpu_id 读 CPU 号（快路径），失败就退回 getcpu 系统调用。
 * 本层做三件事，缺一件这个号就只是纸面成功：
 *
 *   1) 注册契约：flags == 0、rseq_len == 32、rseq 32 字节对齐；rseq == NULL
 *      是注销。三条与 Linux 逐条相等（Linux 也只认 32 这个长度）。
 *   2) cpu_id_start / cpu_id 的写回：本内核单核调度，两个字段恒 0。这不是
 *      编造——这台机器没有第二个 CPU 可迁移，0 就是真值。
 *   3) rseq_cs 的中止：在**真把控制权交给别人**的点上看用户 PC 是否落在
 *      [start_ip, start_ip + post_commit_offset) 里，落在里面就把 PC 改成
 *      abort_ip 并把 rseq_cs 清零。Linux 的 rseq_ip_fixup 做的就是这件事。
 *      不做它，临界区被抢占后会带着「没被打断」的假设继续跑——那是这类
 *      机制里最难查的一种错。两个中止点：信号投递（本文件）、tick 抢占
 *      （port 在真正切换之前调 arkrtos_linux_abi_rseq_preempt()）。
 *
 * 边界（docs/已知限制.md 里逐条写着）：
 *   * 单核：没有 migration，cpu_id 恒 0；
 *   * rseq_cs 的 version / flags 必须是 0；否则按「这块内存不是本层认得的
 *     rseq_cs」处理——清掉 rseq_cs、不改 PC。Linux 这里是 SIGSEGV 杀任务，
 *     而切换点里没有投递信号的位置，宁可不动 PC 也不跳一个读不懂的地址；
 *   * 注册按任务记、不跨 fork 继承（Linux 也是每条线程各自注册）；
 *   * 中止只覆盖「用户态被打断」这一种现场（内核态被打断时用户 PC 不在
 *     临界区里，本来就不该动它）。
 */
#define LX_RSEQ_BYTES               (32u)
#define LX_RSEQ_ALIGN_MASK          (0x1FUL)
#define LX_RSEQ_OFF_CPU_ID_START    (0u)
#define LX_RSEQ_OFF_CPU_ID          (4u)
#define LX_RSEQ_OFF_RSEQ_CS         (8u)
#define LX_RSEQ_CS_BYTES            (32u)
#define LX_RSEQ_CS_OFF_START_IP     (8u)
#define LX_RSEQ_CS_OFF_POST_COMMIT  (16u)
#define LX_RSEQ_CS_OFF_ABORT_IP     (24u)

/* 每个任务一格：注册的 struct rseq 用户地址（0 = 没注册）。Linux 按线程记，
 * 本层按任务号记——一条任务就是一个线程（U-25 之后同组线程号也各不相同）。 */
static unsigned long s_rseq_area[ARKRTOS_TASK_MAX_NUM];

static uint32 lx_rseq_rd32(const uint8 *p)
{
    return ((uint32)p[0]) | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
}

static unsigned long long lx_rseq_rd64(const uint8 *p)
{
    unsigned long long v = 0ull;
    int32 i;

    for(i = 7; i >= 0; i--)
    {
        v = (v << 8) | (unsigned long long)p[i];
    }
    return v;
}

/* 当前任务在 rseq 表里的下标；拿不到任务号回 -1（本层所有系统调用都跑在
 * 任务里，取不到只可能是端口在没有任务上下文时调进来）。 */
static int32 lx_rseq_slot(void)
{
    int32 tid = (int32)arkrtos_current_task_id;

    if((tid <= 0) || (tid > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    return tid - 1;
}

/* rseq(293)：rseq / rseq_len / flags / sig。第 4 个参数是中止指令流里的签名，
 * 本层不在内核里执行用户的中止指令，因此不看它——签名只被 abort 路径解释，
 * 不看它不影响语义。 */
static int32 lx_rseq(void *p_ctx)
{
    unsigned long area  = ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32        len   = (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32        flags = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32        zero  = 0u;
    int32         slot  = lx_rseq_slot();

    if((flags != 0u) || (len != (uint32)LX_RSEQ_BYTES))
    {
        return -LX_EINVAL;
    }
    if(slot < 0)
    {
        return -LX_ESRCH;
    }
    if(area == 0UL)
    {
        s_rseq_area[(uint32)slot] = 0UL;        /* 注销 */
        return 0;
    }
    if((area & LX_RSEQ_ALIGN_MASK) != 0UL)
    {
        return -LX_EINVAL;
    }
    /* cpu_id_start 与 cpu_id 一起写成 0。写不进去就明确 -EFAULT：注册成功
     * 却让程序读到一个假 CPU 号，比当场拒绝坏得多。 */
    if((arkrtos_copy_to_user((void *)area, &zero, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(area + (unsigned long)LX_RSEQ_OFF_CPU_ID),
                             &zero, 4u) != 0))
    {
        return -LX_EFAULT;
    }
    s_rseq_area[(uint32)slot] = area;
    return 0;
}

/* getcpu(168)：两个 4 字节出参（任一可为 NULL）加一个 cache 参数（本层不用）。
 * 单核：CPU 号与 NUMA 节点号都是 0，与 rseq 写回的值必须一致——两处给出不同
 * 的号，程序就会在两套快/慢路径上看到两个世界。 */
static int32 lx_getcpu(void *p_ctx)
{
    unsigned long pcpu  = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long pnode = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32        zero  = 0u;

    if((pcpu != 0UL) && (arkrtos_copy_to_user((void *)pcpu, &zero, 4u) != 0))
    {
        return -LX_EFAULT;
    }
    if((pnode != 0UL) && (arkrtos_copy_to_user((void *)pnode, &zero, 4u) != 0))
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* rseq_cs 的中止：p_frame 是「即将把控制权交回用户态、但打断了用户执行」的
 * 那份 trap 帧。返回 1 = 真把 PC 改到了 abort_ip。
 *
 * 判据逐条与 Linux 对齐：rseq 已注册 / rseq_cs 非 0 / rseq_cs 的 version 与
 * flags 都是 0 / 被打断的 PC 落在 [start_ip, start_ip + post_commit) 里。
 * 任何一条不成立都**不动 PC**：改错一个不在这块数据上的现场，比什么都不做
 * 危险得多。 */
static uint32 lx_rseq_fixup(void *p_frame)
{
    int32              slot = lx_rseq_slot();
    unsigned long      area;
    unsigned long long cs;
    unsigned long long zero64 = 0ull;
    uint8              buf[LX_RSEQ_CS_BYTES];
    unsigned long      pc;
    unsigned long long start;
    unsigned long long post;
    unsigned long long abort_ip;

    if(slot < 0)
    {
        return 0u;
    }
    area = s_rseq_area[(uint32)slot];
    if(area == 0UL)
    {
        return 0u;
    }
    if(arkrtos_copy_from_user(&buf[0],
                              (const void *)(area +
                                  (unsigned long)LX_RSEQ_OFF_RSEQ_CS), 8u) != 0)
    {
        return 0u;
    }
    cs = lx_rseq_rd64(&buf[0]);
    if(cs == 0ull)
    {
        return 0u;                              /* 不在临界区里 */
    }
    if(arkrtos_copy_from_user(&buf[0], (const void *)(unsigned long)cs,
                              (uint32)LX_RSEQ_CS_BYTES) != 0)
    {
        return 0u;
    }
    if((lx_rseq_rd32(&buf[0]) != 0u) || (lx_rseq_rd32(&buf[4]) != 0u))
    {
        /* 内容不是本层认得的 rseq_cs：清掉指针，不跳。 */
        (void)arkrtos_copy_to_user((void *)(area +
                                   (unsigned long)LX_RSEQ_OFF_RSEQ_CS),
                                   &zero64, 8u);
        return 0u;
    }

    start    = lx_rseq_rd64(&buf[LX_RSEQ_CS_OFF_START_IP]);
    post     = lx_rseq_rd64(&buf[LX_RSEQ_CS_OFF_POST_COMMIT]);
    abort_ip = lx_rseq_rd64(&buf[LX_RSEQ_CS_OFF_ABORT_IP]);

    pc = arkrtos_port_svc_user_reg(p_frame, LX_UREG_ERA);
    if(((unsigned long long)pc < start) ||
       ((unsigned long long)pc >= (start + post)) || (abort_ip == 0ull))
    {
        return 0u;
    }

    /* 顺序是硬的：先清 rseq_cs，再改 PC。反过来的话，PC 一改而清指针失败，
     * 程序回到 abort 路径后会看到「cs 还在」而再跑一遍临界区。 */
    if(arkrtos_copy_to_user((void *)(area +
                              (unsigned long)LX_RSEQ_OFF_RSEQ_CS),
                            &zero64, 8u) != 0)
    {
        return 0u;
    }
    if(arkrtos_port_svc_set_user_reg(p_frame, LX_UREG_ERA,
                                     (unsigned long)abort_ip) != 0)
    {
        return 0u;
    }
    return 1u;
}

/* port 在真正切换任务之前调用（LA64 是 la_trap_c 的出口结算处）。只在被打断
 * 的是用户态时才该调用——内核态被打断时用户 PC 不在临界区里。 */
uint32 arkrtos_linux_abi_rseq_preempt(void *p_frame)
{
    return lx_rseq_fixup(p_frame);
}

/* 信号帧：头（magic / signo / blocked / 快照字节数）+ 整份 trap 帧。
 * 投递成功返回 0；返回 1 表示任务已被默认动作结束（调用方不要再动上下文）。 */
static uint32 lx_signal_deliver(void *p_ctx)
{
    uint32 sig;
    unsigned long fsp;
    lx_sigaction_t *sa;

    /* 0 = 无事可做，1 = 已被默认动作结束；这两种都要原样交回调用方
     * （返回 1 时调用方不能再去动上下文）。 */
    sig = lx_sig_pick(&sa);
    if(sig <= 1u)
    {
        return sig;
    }

    /* rseq_cs 的中止点之一：信号要投进 handler 了，而被中断的 PC 可能正落在
     * 可重启序列的临界区里。必须在铺帧**之前**做——Linux 的 rseq_ip_fixup 也
     * 在信号处理之前跑，于是存进信号帧的是 abort_ip，sigreturn 之后从 abort
     * 路径继续，而不是带着「没被打断」的假设返回临界区中间。 */
    (void)lx_rseq_fixup(p_ctx);

    if(lx_sig_build_frame(p_ctx, sig, &fsp) != 0u)
    {
        return 0u;
    }

    (void)lx_sig_install(p_ctx, sig, sa, fsp);
    return 0u;
}

/* rt_sigreturn：把整帧快照原样装回去。帧地址就是进入本次系统调用时的
 * 用户 sp（restorer 里没有任何压栈），所以不需要额外的寄存器传参。 */
static int32 lx_rt_sigreturn(void *p_ctx)
{
    uint32 fbytes = arkrtos_port_svc_frame_bytes();
    uint32 total;
    unsigned long sp = arkrtos_port_svc_user_reg(p_ctx, LX_UREG_SP);


    if((fbytes == 0u) || (fbytes > (uint32)(LX_SIGFRAME_MAX_WORDS * 8u)))
    {
        return -LX_ENOSYS;
    }
    total = LX_SIGFRAME_HDR_BYTES + fbytes;
    if((sp == 0UL) ||
       (arkrtos_copy_from_user(s_sigframe, (const void *)(unsigned long)sp,
                               total) != 0))
    {
        return -LX_EFAULT;
    }
    if((s_sigframe[0] != LX_SIGFRAME_MAGIC) ||
       (s_sigframe[3] != (unsigned long)fbytes))
    {
        return -LX_EFAULT;      /* 不是我们铺的帧：不按它恢复现场 */
    }
    if(arkrtos_port_svc_frame_import(p_ctx,
            &s_sigframe[LX_SIGFRAME_HDR_WORDS]) != 0)
    {
        return -LX_ENOSYS;
    }
    s_sig_blocked = (uint32)s_sigframe[2];
    return LX_RET_PTR_DONE;     /* 现场（含 a0 的返回值）已由快照装回 */
}

/* ---------------------------------------------------------------- 调用 */

static int32 lx_openat(void *p_ctx)
{
    char   path[LX_PATH_MAX];
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32 size  = 0u;
    uint32 wr    = 0u;      /* 1 = 本次以 O_RDWR 打开（只有 FILEMAP 档会给 1） */
    int32  rc;
    uint32 i;

    rc = lx_copy_path(p_ctx, 1u, path, (uint32)sizeof(path));
    if(rc != 0)
    {
        return rc;
    }

    /* 这台板上的用户态文件系统是只读的（文件由内核侧写进去）。想写就
     * 明确回 -EROFS，而不是给一个"能打开、写不进去"的句柄。
     *
     * U-21 只放开一个例外：FILEMAP 档下打开一个**已存在**的文件时 O_RDWR
     * 合法 —— 可写共享映射要求 fd 以读写打开（Linux 语义），没有它就验不了
     * 写回。O_CREAT / O_TRUNC / O_WRONLY 仍然 -EROFS：建文件与截断不在这一档
     * 的语义里，写内容也只走映射写回这一条路。 */
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    if(((flags & LX_O_ACCMODE) == LX_O_RDWR) &&
       ((flags & (LX_O_CREAT | LX_O_TRUNC)) == 0u))
    {
        wr = 1u;
    }
    else
#endif
    if(((flags & LX_O_ACCMODE) != 0u) || ((flags & LX_O_CREAT) != 0u) ||
       ((flags & LX_O_TRUNC) != 0u))
    {
        return -LX_EROFS;
    }

    /* 路径是**命名空间**路径（/mnt/ram/xxx 是挂载点），必须走 VFS 门面；
     * arkrtos_fs_stat_path() 解的是卷内路径，拿错一层问就会把一个存在
     * 的文件报成 -ENOENT（2026-11-19 在 QEMU 上撞到的就是这个）。 */
    {
        uint32 mode = 0u;

        if(arkrtos_vfs_app_stat(path, &size, &mode) < 0)
        {
            return -LX_ENOENT;
        }
        if((mode & ARKRTOS_VFS_S_IFDIR) != 0u)
        {
            return -LX_EISDIR;
        }
    }

    for(i = 0u; i < LX_MAX_FD; i++)
    {
        if(s_fd[i].used == 0u)
        {
            break;
        }
    }
    if(i == LX_MAX_FD)
    {
        return -LX_ENFILE;
    }

    lx_copy_str(s_fd[i].path, (uint32)sizeof(s_fd[i].path), path);
    s_fd[i].kind = LX_FD_FILE;
    s_fd[i].pipe = 0u;
    s_fd[i].size = size;
    s_fd[i].off  = 0u;
    s_fd[i].wr   = (uint8)wr;
    s_fd[i].cloexec = ((flags & (uint32)LX_O_CLOEXEC) != 0u) ? 1u : 0u;
    s_fd[i].fl = (uint16)((wr != 0u) ? LX_O_RDWR : LX_O_RDONLY);
    if((flags & (uint32)LX_O_APPEND) != 0u)
    {
        s_fd[i].fl |= (uint16)LX_O_APPEND;
    }
    if((flags & (uint32)LX_O_NONBLOCK) != 0u)
    {
        s_fd[i].fl |= (uint16)LX_O_NONBLOCK;
    }
    s_fd[i].used = 1u;

    return (int32)(3u + i);
}

/* 关掉一个 fd 槽（含管道引用计数）。参数是**显式槽位**：任务退出时收尸
 * 要清的是那个进程的槽，而当前上下文可能已经不是它了。
 * 返回 1 = 这个槽原本是有效 fd。 */
static uint32 lx_fd_release(uint32 slot, uint32 idx)
{
    uint32 i;

    if((slot >= (uint32)LX_PROC_MAX) || (idx >= LX_MAX_FD))
    {
        return 0u;
    }
    if(s_proc[slot].fd[idx].used == 0u)
    {
        return 0u;
    }
    /* 管道按「有几个 fd 指着它」记账；dup 出来的那一份在复制时加过一次，
     * 所以在**每一次**释放里减一次，与引用计数不是一回事。
     * 套接字不进这张账：net 那一格由 close 或退出时的 best-effort 关闭负责
     * （见 lx_sock_close / lx_sock_close_all），这里只退 fd 槽。
     * 控制台引用没有外力可还，直接退槽。 */
    if((s_proc[slot].fd[idx].kind == LX_FD_PIPE_R) ||
       (s_proc[slot].fd[idx].kind == LX_FD_PIPE_W))
    {
        int32 pi = (int32)s_proc[slot].fd[idx].pipe;

        if((pi >= 0) && (pi < (int32)LX_PIPE_MAX) && (s_pipe[pi].used != 0u))
        {
            if(s_proc[slot].fd[idx].kind == LX_FD_PIPE_R)
            {
                if(s_pipe[pi].readers != 0u)
                {
                    s_pipe[pi].readers--;
                }
                /* 读者没了：等着的写端醒来拿 -EPIPE（并发 SIGPIPE）。 */
                if(s_pipe[pi].readers == 0u)
                {
                    lx_pipe_wake(pi, 1u);
                }
            }
            else
            {
                if(s_pipe[pi].writers != 0u)
                {
                    s_pipe[pi].writers--;
                }
                /* 写者没了：等着的读端醒来读到 EOF。 */
                if(s_pipe[pi].writers == 0u)
                {
                    lx_pipe_wake(pi, 0u);
                }
            }
            if((s_pipe[pi].readers == 0u) && (s_pipe[pi].writers == 0u))
            {
                s_pipe[pi].used  = 0u;
                s_pipe[pi].count = 0u;
                s_pipe[pi].head  = 0u;
                s_pipe[pi].tail  = 0u;
            }
        }
    }
    s_proc[slot].fd[idx].used = 0u;
    s_proc[slot].fd[idx].kind = LX_FD_FILE;
    s_proc[slot].fd[idx].pipe = 0u;
    s_proc[slot].fd[idx].off  = 0u;
    s_proc[slot].fd[idx].size = 0u;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    s_proc[slot].fd[idx].owner = 0;     /* 槽退掉，主人登记也跟着没了 */
#endif
    /* 指向这一格的标准流别名同时摘掉：别名与槽是同一份状态，留一个指向
     * 空槽的别名等于给程序一个假句柄（lx_std_slot 也会当它不存在，但让它
     * 一直挂着会让后面的 dup3 误判「这一格被别名占着」）。 */
    for(i = 0u; i < 3u; i++)
    {
        if(s_proc[slot].std_slot[i] == (uint8)(idx + 1u))
        {
            s_proc[slot].std_slot[i] = 0u;
        }
    }
    /* close 掉这个文件的任意一个 fd：本进程在这个文件上的记录锁全部
     * 释放（POSIX 语义，锁属于进程而不属于 fd）。必须赶在清 path 之前，
     * 因为文件身份就是路径。 */
    if(s_proc[slot].fd[idx].path[0] != 0)
    {
        lx_rslk_release_path(slot, s_proc[slot].fd[idx].path);
    }
    s_proc[slot].fd[idx].path[0] = 0;
    s_proc[slot].fd[idx].wr = 0u;
    s_proc[slot].fd[idx].cloexec = 0u;
    s_proc[slot].fd[idx].fl = 0u;
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    s_proc[slot].fd[idx].net    = 0u;
    s_proc[slot].fd[idx].stype  = 0u;
    s_proc[slot].fd[idx].sflags = 0u;
    s_proc[slot].fd[idx].pok    = 0u;
    s_proc[slot].fd[idx].pbits  = 0u;
#endif
    return 1u;
}

/* execve 换镜像成功之后的收尾：还掉所有带 FD_CLOEXEC 的槽（它们占的标准流
 * 别名由 lx_fd_release 一并摘掉）。这是 openat / dup3 / pipe2 / socket 敢接受
 * O_CLOEXEC 的唯一依据；不做这一步，那些标志就只是一个记下来没人执行的字。 */
static void lx_fd_close_cloexec(void)
{
    uint32 slot = lx_proc_cur();
    uint32 i;

    for(i = 0u; i < LX_MAX_FD; i++)
    {
        if((s_proc[slot].fd[i].used != 0u) &&
           (s_proc[slot].fd[i].cloexec != 0u))
        {
            (void)lx_fd_release(slot, i);
        }
    }
}

#if (ARKRTOS_USE_LINUX_SOCKET == 1)
/* ------------------------------------------------------ socket（U-23） */

/* 一次 net 请求的负错误号 → 本层 errno。ENOTSUP 是「这套内核根本没编进
 * 协议栈」，对应 -ENOSYS；其余逐条对齐 Linux 的同名 errno。对不上号的只可能
 * 是实现漏了一档，如实回 -EIO —— 编一个看似合理的号会让程序走错分支。 */
static int32 lx_sock_errno(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_NET_EINVAL:        return -LX_EINVAL;
    case ARKRTOS_NET_EWOULDBLOCK:   return -LX_EAGAIN;
    case ARKRTOS_NET_EINPROGRESS:   return -LX_EINPROGRESS;
    case ARKRTOS_NET_EALREADY:      return -LX_EALREADY;
    case ARKRTOS_NET_EISCONN:       return -LX_EISCONN;
    case ARKRTOS_NET_ENOTCONN:      return -LX_ENOTCONN;
    case ARKRTOS_NET_ECONNREFUSED:  return -LX_ECONNREFUSED;
    case ARKRTOS_NET_ECONNRESET:    return -LX_ECONNRESET;
    case ARKRTOS_NET_ECONNABORTED:  return -LX_ECONNABORTED;
    case ARKRTOS_NET_EHOSTUNREACH:  return -LX_EHOSTUNREACH;
    case ARKRTOS_NET_EMSGSIZE:      return -LX_EMSGSIZE;
    case ARKRTOS_NET_ENOBUFS:       return -LX_ENOBUFS;
    case ARKRTOS_NET_ENOTSUP:       return -LX_ENOSYS;
    default:                        return -LX_EIO;
    }
}

/* 本任务的重启槽归零。任务退出/换程序时调用：任务号会被复用，不清的话新
 * 任务会继承一条「已经在等」的旧登记，第一次 socket 调用就会走重放分支。 */
static void lx_sock_ctx_clear(int32 task_id)
{
    uint32 i;

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    s_sockctx[task_id - 1].armed = 0u;
    s_sockctx[task_id - 1].step  = 0u;
    s_sockctx[task_id - 1].res   = 0;
    s_sockctx[task_id - 1].aux   = 0;
    for(i = 0u; i < 6u; i++)
    {
        s_sockctx[task_id - 1].arg[i] = 0u;
    }
}

/* 重放时参数已经不在寄存器里（a0 被 lx_finish 清成 0），所以每个 socket 处理
 * 函数都必须先经过这里：armed=0 时把这次调用的 6 个参数抄进本任务的槽，
 * armed=1 时反过来从槽里取。两种情况 r[] 都是权威值。 */
static void lx_sock_load(int32 me, void *p_ctx, uint32 *r)
{
    uint32 i;

    if(s_sockctx[me - 1].armed != 0u)
    {
        for(i = 0u; i < 6u; i++)
        {
            r[i] = s_sockctx[me - 1].arg[i];
        }
        return;
    }
    for(i = 0u; i < 6u; i++)
    {
        r[i] = (uint32)ARKRTOS_SVC_ARG(p_ctx, i);
        s_sockctx[me - 1].arg[i] = r[i];
    }
}

/* connect 那种「协议栈还在推进」的两段式等待用的重放节拍。整个时间片
 * （LX_WAIT_SLICE_MS = 100 ms）留给「等一个还没落地的回复」；那类等待现在
 * 由服务任务在回复落地时直接叫醒，睡满一个时间片只是兜底。而「回复已经
 * 说了 EINPROGRESS」这种状态变化通常几毫秒内就发生，用 100 ms 去等会把
 * 一次本机回环 connect 变成 100 ms 级操作（实测 121 ms → 现在 20 ms 内）。 */
#define LX_SOCK_POLL_MS   (10u)

/* 「这一次还没结束」：登记重放、睡一个时间片，返回 LX_RET_RETRY。没有可重放
 * 端口时如实回 -EAGAIN（不假装睡过）并把登记撤掉。 */
static int32 lx_sock_replay_ms(void *p_ctx, int32 me, uint32 slice_ms)
{
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        lx_sock_ctx_clear(me);
        return -LX_EAGAIN;
    }
    s_sockctx[me - 1].armed = 1u;
    /* 服务任务在回复落地时会主动叫醒我们（见 arkrtos_net_handle_msg），所以
     * 下面这一觉通常远短于一个时间片。先问一句「回复是不是已经到了」：
     * 服务任务可能在上面的 arkrtos_net_call 返回之前就写好了回复，那时唤醒
     * 发不出去，这一步把那条竞态也收掉。时间片仍然保留，它是丢唤醒时的
     * 兜底 —— 不会因为一次没叫醒就永久卡住。 */
    if(arkrtos_net_reply_ready(me) != 0)
    {
        return LX_RET_RETRY;
    }
    (void)arkrtos_task_block_in_critical(slice_ms);
    return LX_RET_RETRY;
}

static int32 lx_sock_replay(void *p_ctx, int32 me)
{
    return lx_sock_replay_ms(p_ctx, me, (uint32)LX_WAIT_SLICE_MS);
}

/* 发一次 net 请求并推进到结束。成功时把非负结果写进 *p_res 并返回 0；需要等
 * （EPENDING/EBUSY）时走重放，返回 LX_RET_RETRY。EBUSY 也等：请求槽全系统
 * 只有一个，别人（同一进程的另一个 socket 调用、或 App 自己的网络 SDK）在飞
 * 时，唯一正确的做法是稍后再问，而不是把「忙」当成这个 socket 的错误交给程序。
 *
 * 一次 net 请求至少要一个往返（提交 → 服务任务执行 → 取回结果），所以任何一个
 * socket 系统调用都必然经历一次重放 —— 这也是为什么参数必须落在本任务的槽里。 */
static int32 lx_sock_submit(void *p_ctx, int32 me, uint32 op,
                            uint32 a1, uint32 a2, uint32 a3,
                            uint32 a4, uint32 a5, int32 *p_res)
{
    int32 rc;

    rc = arkrtos_net_call(op, a1, a2, a3, a4, a5);
    if((rc == ARKRTOS_NET_EPENDING) || (rc == ARKRTOS_NET_EBUSY))
    {
        return lx_sock_replay(p_ctx, me);
    }
    s_sockctx[me - 1].step = 0u;
    if(rc < 0)
    {
        return lx_sock_errno(rc);
    }
    *p_res = rc;
    return 0;
}

/* 找一个空 fd 槽，返回 fd 号（3 起）；满了 -EMFILE。**在真正建 socket 之前
 * 调用**：先建再回滚会把一个 lwIP socket 漏在系统里，而这里没有「关不掉」的
 * 补救路径（服务任务也挂掉时谁也关不了）。 */
static int32 lx_sock_alloc_fd(void)
{
    uint32 i;

    for(i = 0u; i < LX_MAX_FD; i++)
    {
        if(s_fd[i].used == 0u)
        {
            return (int32)(3u + i);
        }
    }
    return -LX_EMFILE;
}

/* fd 号 → 表下标，且必须是 socket 类。fd 不存在回 -EBADF，种类不对回
 * -ENOTSOCK（Linux 的 socket 族调用对非 socket 描述符给的就是它）。 */
static int32 lx_sock_idx(int32 fd)
{
    int32 idx = lx_fd_find(fd);

    if(idx < 0)
    {
        return -LX_EBADF;
    }
    if(s_fd[idx].kind != LX_FD_SOCKET)
    {
        return -LX_ENOTSOCK;
    }
    return idx;
}

/* 把一个刚拿到的 net 句柄落进 fd 槽。 */
static void lx_sock_fill(uint32 idx, int32 handle, uint32 stype, uint32 sflags)
{
    s_fd[idx].kind    = LX_FD_SOCKET;
    s_fd[idx].pipe    = 0u;
    s_fd[idx].wr      = 0u;
    s_fd[idx].size    = 0u;
    s_fd[idx].off     = 0u;
    s_fd[idx].path[0] = 0;
    s_fd[idx].net     = (uint8)handle;
    s_fd[idx].stype   = (uint8)stype;
    s_fd[idx].sflags  = (uint8)sflags;
    s_fd[idx].cloexec = ((sflags & (uint32)LX_SOCKF_CLOEXEC) != 0u) ? 1u : 0u;
    s_fd[idx].fl      = (uint16)(LX_O_RDWR |
        (((sflags & (uint32)LX_SOCKF_NONBLOCK) != 0u) ? LX_O_NONBLOCK : 0u));
    s_fd[idx].pok     = 0u;
    s_fd[idx].pbits   = 0u;
    s_fd[idx].used    = 1u;
}

/* Linux sockaddr_in 的布局（32/64 位同布局，共 16 字节）：u16 family /
 * u16 port(网络序) / u32 addr(网络序) / 8 字节零。前 8 字节就装下了全部信息，
 * 所以 addrlen 只要 >= 8 就能读（Linux 也是这么判的）。 */
static int32 lx_sock_addr_in(unsigned long ua, uint32 alen,
                             uint32 *p_ip, uint32 *p_port)
{
    uint8  sa[8];
    uint32 family;

    if(ua == 0ul)
    {
        return -LX_EFAULT;
    }
    if(alen < 8u)
    {
        return -LX_EINVAL;
    }
    if(arkrtos_copy_from_user(sa, (const void *)ua, 8u) != 0)
    {
        return -LX_EFAULT;
    }
    family = (uint32)sa[0] | ((uint32)sa[1] << 8);
    if(family != (uint32)LX_AF_INET)
    {
        return -LX_EAFNOSUPPORT;
    }
    *p_port = ((uint32)sa[2] << 8) | (uint32)sa[3];
    *p_ip   = ((uint32)sa[4] << 24) | ((uint32)sa[5] << 16) |
              ((uint32)sa[6] << 8)  | (uint32)sa[7];
    return 0;
}

/* net 的输出是「两个 4 字节 host order 写进用户地址」。我们没有第二个干净的
 * 用户缓冲，于是借调用方自己的 sockaddr 当临时区：**端口写偏移 0、地址写偏移
 * 4**，两块互不重叠（正好对上 net 的先后顺序），拿回来之后再按 Linux 布局重写
 * 整个 16 字节。这样既不多占内存，也不会把半个地址留在用户表里。 */
static int32 lx_sock_addr_fetch(unsigned long ua, uint32 *p_ip, uint32 *p_port)
{
    uint32 w;

    if(arkrtos_copy_from_user(&w, (const void *)ua, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    *p_port = w;
    if(arkrtos_copy_from_user(&w, (const void *)(ua + 4ul), 4u) != 0)
    {
        return -LX_EFAULT;
    }
    *p_ip = w;
    return 0;
}

/* 把地址与端口按 Linux 的 sockaddr_in 写回用户表，并把 addrlen 写成 16。
 * ua==0 是「调用方不要地址」：此时 addrlen 按 Linux 的习惯写 0。 */
static int32 lx_sock_addr_out(unsigned long ua, unsigned long ulenp,
                             uint32 ip, uint32 port)
{
    uint8  sa[16];
    uint32 len;
    uint32 i;

    if(ua != 0ul)
    {
        for(i = 0u; i < 16u; i++)
        {
            sa[i] = 0u;
        }
        sa[0] = (uint8)((uint32)LX_AF_INET & 0xFFu);
        sa[1] = (uint8)(((uint32)LX_AF_INET >> 8) & 0xFFu);
        sa[2] = (uint8)((port >> 8) & 0xFFu);
        sa[3] = (uint8)(port & 0xFFu);
        sa[4] = (uint8)((ip >> 24) & 0xFFu);
        sa[5] = (uint8)((ip >> 16) & 0xFFu);
        sa[6] = (uint8)((ip >> 8) & 0xFFu);
        sa[7] = (uint8)(ip & 0xFFu);
        if(arkrtos_copy_to_user((void *)ua, sa, 16u) != 0)
        {
            return -LX_EFAULT;
        }
        len = 16u;
    }
    else
    {
        len = 0u;
    }
    if(ulenp != 0ul)
    {
        if(arkrtos_copy_to_user((void *)ulenp, &len, 4u) != 0)
        {
            return -LX_EFAULT;
        }
    }
    return 0;
}

/* Linux 的 SOL_SOCKET 选项号 → net 服务的选项号。返回 -1 表示**这一层**不认，
 * 调用方据此回 -ENOPROTOOPT，而不是把一个陌生的号发给 net 让它猜。 */
static int32 lx_sock_opt_net(int32 opt)
{
    switch(opt)
    {
    case LX_SO_REUSEADDR: return ARKRTOS_NET_SO_REUSEADDR;
    case LX_SO_RCVBUF:    return ARKRTOS_NET_SO_RCVBUF;
    case LX_SO_SNDBUF:    return ARKRTOS_NET_SO_SNDBUF;
    case LX_SO_ERROR:     return ARKRTOS_NET_SO_ERROR;
    default:              return -1;
    }
}

/* 参数是「这次调用的 6 个原始参数」能否推进到结束。所有处理函数共用的前置：
 * 内核上下文（没有可登记的身份）一律 -ENOSYS。 */
static int32 lx_sock_me_ok(void)
{
    int32 me = (int32)arkrtos_current_task_id;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }
    return me;
}

/* ------------------------------ socket(2) */
static int32 lx_socket(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 type;
    uint32 sf = 0u;
    int32  res = 0;
    int32  rc;
    int32  fd;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    if(r[0] != (uint32)LX_AF_INET)
    {
        return -LX_EAFNOSUPPORT;
    }
    /* type 与两个标志位是**同一个字**里的不同段：先摘掉标志位再判 type。
     * Linux 的判法是 type >= SOCK_MAX 回 -EINVAL，范围内但不支持的 type 回
     * -ESOCKTNOSUPPORT —— 两种错法必须分开，程序据此决定是「调用写错了」
     * 还是「这套内核不支持这种 socket」。 */
    type = r[1] & ~(uint32)(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC);
    if(type >= (uint32)LX_SOCK_MAX)
    {
        return -LX_EINVAL;
    }
    if((type != (uint32)LX_SOCK_STREAM) && (type != (uint32)LX_SOCK_DGRAM))
    {
        return -LX_ESOCKTNOSUPPORT;
    }
    if(r[2] != 0u)
    {
        return -LX_EPROTONOSUPPORT;
    }

    fd = lx_sock_alloc_fd();
    if(fd < 0)
    {
        return fd;
    }

    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SOCKET, type,
                        0u, 0u, 0u, 0u, &res);
    if(rc != 0)
    {
        return rc;
    }
    if((res <= 0) || (res > 255))
    {
        /* 服务回的句柄不在约定范围里：这不是成功，也不能拿它去后续调用。 */
        return -LX_EIO;
    }
    if((r[1] & (uint32)LX_SOCK_NONBLOCK) != 0u)
    {
        sf |= LX_SOCKF_NONBLOCK;
    }
    if((r[1] & (uint32)LX_SOCK_CLOEXEC) != 0u)
    {
        sf |= LX_SOCKF_CLOEXEC;
    }
    lx_sock_fill((uint32)(fd - 3), res, type, sf);
    return fd;
}

/* ------------------------------ bind(2) */
static int32 lx_bind(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    rc = lx_sock_addr_in(r[1], r[2], &ip, &port);
    if(rc < 0)
    {
        return rc;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_BIND, (uint32)s_fd[idx].net,
                        ip, port, 0u, 0u, &res);
    return (rc == 0) ? 0 : rc;
}

/* ------------------------------ listen(2) */
static int32 lx_listen(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 backlog;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    /* 负 backlog：Linux 静默当 0；上界 255 是 net 协议里的宽度，Linux 也是
     * 静默截断（不是报错）。 */
    backlog = r[1];
    if(backlog > 0x7FFFFFFFu)
    {
        backlog = 0u;
    }
    if(backlog > 255u)
    {
        backlog = 255u;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_LISTEN, (uint32)s_fd[idx].net,
                        backlog, 0u, 0u, 0u, &res);
    return (rc == 0) ? 0 : rc;
}

/* ------------------------------ accept(2) / accept4(2)
 * accept 要做两次 net 请求（接一个 + 取对端地址），两次都会重放。所以第一次
 * 成功之后必须把「接到的句柄 + 分配到的 fd」记进本任务的槽：重放时跳过 accept，
 * 否则每重放一次就多出一个没人认领的 socket。 */
static int32 lx_accept(void *p_ctx, uint8 with_flags)
{
    int32  me;
    uint32 r[6];
    uint32 extra = 0u;
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  idx;
    int32  res = 0;
    int32  rc;
    int32  newfd;
    int32  newh;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    if((s_sockctx[me - 1].armed != 0u) && (s_sockctx[me - 1].step == 1u))
    {
        /* 上一轮已经把 accept 做完了：这一步只补地址查询。 */
        newh  = s_sockctx[me - 1].res;
        newfd = s_sockctx[me - 1].aux;
    }
    else
    {
        idx = lx_sock_idx((int32)r[0]);
        if(idx < 0)
        {
            return idx;
        }
        if(with_flags != 0u)
        {
            if((r[3] & ~(uint32)(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC)) != 0u)
            {
                return -LX_EINVAL;
            }
            extra = r[3];
        }
        newfd = lx_sock_alloc_fd();
        if(newfd < 0)
        {
            return newfd;
        }
        rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_ACCEPT,
                            (uint32)s_fd[idx].net, 0u, 0u, 0u, 0u, &res);
        if(rc != 0)
        {
            return rc;
        }
        if((res <= 0) || (res > 255))
        {
            return -LX_EIO;
        }
        newh = res;
        {
            uint32 sf = 0u;

            if((extra & (uint32)LX_SOCK_NONBLOCK) != 0u)
            {
                sf |= LX_SOCKF_NONBLOCK;
            }
            if((extra & (uint32)LX_SOCK_CLOEXEC) != 0u)
            {
                sf |= LX_SOCKF_CLOEXEC;
            }
            /* 新连接的 socket 类型跟着监听的那个走。 */
            lx_sock_fill((uint32)(newfd - 3), newh, (uint32)s_fd[idx].stype, sf);
        }
        if(r[1] == 0u)
        {
            return newfd;
        }
        s_sockctx[me - 1].res  = newh;
        s_sockctx[me - 1].aux  = newfd;
        s_sockctx[me - 1].step = 1u;
    }

    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_GETPEERNAME, (uint32)newh,
                        (uint32)(r[1] + 4ul), r[1], 0u, 0u, &res);
    if(rc != 0)
    {
        return rc;
    }
    rc = lx_sock_addr_fetch(r[1], &ip, &port);
    if(rc < 0)
    {
        /* 地址写不回去：fd 已经建好了，只能把它退掉，不让调用方拿到一个
         * 「一半失败」的句柄。net 侧的那一格由任务退出时的收尸关闭。 */
        (void)lx_fd_release(lx_proc_cur(), (uint32)(newfd - 3));
        return rc;
    }
    rc = lx_sock_addr_out(r[1], r[2], ip, port);
    if(rc < 0)
    {
        (void)lx_fd_release(lx_proc_cur(), (uint32)(newfd - 3));
        return rc;
    }
    return newfd;
}

/* ------------------------------ connect(2) */
static int32 lx_connect(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    rc = lx_sock_addr_in(r[1], r[2], &ip, &port);
    if(rc < 0)
    {
        return rc;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_CONNECT, (uint32)s_fd[idx].net,
                        ip, port, 0u, 0u, &res);
    if((rc == -LX_EINPROGRESS) || (rc == -LX_EALREADY))
    {
        /* 非阻塞 socket 的 connect 是两段式：把「还在进行」如实交回去，程序
         * 自己再问（Linux 就是这个语义）。阻塞 socket 就在时间片上重放，直到
         * 连上或给出真正的错误。 */
        if((s_fd[idx].sflags & LX_SOCKF_NONBLOCK) != 0u)
        {
            return rc;
        }
        return lx_sock_replay_ms(p_ctx, me, (uint32)LX_SOCK_POLL_MS);
    }
    return (rc == 0) ? 0 : rc;
}

/* sendto(2)（也承接 glibc 的 send/recv 与 write/read 的 socket 分支）。
 * flags 只认 MSG_DONTWAIT：本层不做 SIGPIPE，也不往 net 转发 lwIP 的 flags
 * 编号（协议层语义不同，转发过去就是一个翻译错误）。 */
static int32 lx_sendto(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if(r[2] > 0xFFFFu)
    {
        return -LX_EMSGSIZE;        /* net 协议里的长度宽度 */
    }
    if(r[4] != 0u)
    {
        rc = lx_sock_addr_in(r[4], r[5], &ip, &port);
        if(rc < 0)
        {
            return rc;
        }
        rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SENDTO,
                            (uint32)s_fd[idx].net, r[1], r[2], ip, port, &res);
    }
    else
    {
        rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SEND,
                            (uint32)s_fd[idx].net, r[1], r[2], 0u, 0u, &res);
    }
    if(rc == -LX_EAGAIN)
    {
        if(((s_fd[idx].sflags & LX_SOCKF_NONBLOCK) != 0u) ||
           ((r[3] & LX_MSG_DONTWAIT) != 0u))
        {
            return rc;
        }
        return lx_sock_replay(p_ctx, me);
    }
    return (rc == 0) ? res : rc;
}

/* recvfrom(2)。len==0 时 Linux 直接回 0 且不发请求；地址的输出同样借用户自己
 * 的 sockaddr 当临时区（见 lx_sock_addr_fetch）。 */
static int32 lx_recvfrom(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 ip = 0u;
    uint32 port = 0u;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if(r[2] == 0u)
    {
        return 0;
    }
    if(r[2] > 0xFFFFu)
    {
        return -LX_EMSGSIZE;
    }
    if(r[4] != 0u)
    {
        rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_RECVFROM,
                            (uint32)s_fd[idx].net, r[1], r[2],
                            (uint32)(r[4] + 4ul), r[4], &res);
    }
    else
    {
        rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_RECV,
                            (uint32)s_fd[idx].net, r[1], r[2], 0u, 0u, &res);
    }
    if(rc == -LX_EAGAIN)
    {
        if(((s_fd[idx].sflags & LX_SOCKF_NONBLOCK) != 0u) ||
           ((r[3] & LX_MSG_DONTWAIT) != 0u))
        {
            return rc;
        }
        return lx_sock_replay(p_ctx, me);
    }
    if(rc != 0)
    {
        return rc;
    }
    if(r[4] != 0u)
    {
        rc = lx_sock_addr_fetch(r[4], &ip, &port);
        if(rc < 0)
        {
            return rc;
        }
        rc = lx_sock_addr_out(r[4], r[5], ip, port);
        if(rc < 0)
        {
            return rc;
        }
    }
    return res;
}

/* ------------------------------ setsockopt(2) */
static int32 lx_setsockopt(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 val = 0u;
    int32  idx;
    int32  nopt;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if((int32)r[1] != LX_SOL_SOCKET)
    {
        return -LX_ENOPROTOOPT;
    }
    nopt = lx_sock_opt_net((int32)r[2]);
    if(nopt < 0)
    {
        return -LX_ENOPROTOOPT;
    }
    if((int32)r[2] == LX_SO_ERROR)
    {
        return -LX_EINVAL;          /* 只读选项 */
    }
    if(r[3] == 0u)
    {
        return -LX_EFAULT;
    }
    if(r[4] < 4u)
    {
        return -LX_EINVAL;
    }
    if(arkrtos_copy_from_user(&val, (const void *)(arkrtos_uptr_t)r[3], 4u) != 0)
    {
        return -LX_EFAULT;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SETSOCKOPT,
                        (uint32)s_fd[idx].net, (uint32)nopt, val, 0u, 0u, &res);
    return (rc == 0) ? 0 : rc;
}

/* ------------------------------ getsockopt(2)
 * SO_TYPE / SO_DOMAIN / SO_ERROR 三个本地就能答：
 *   - TYPE/DOMAIN 是建 socket 时就知道的事实；
 *   - ERROR 恒 0：本层没有异步错误队列，socket 的错误都在调用返回值里给出。
 * 与其让 net 去猜（它没有这三格），不如在这里如实回答。 */
static int32 lx_getsockopt(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    uint32 val = 0u;
    uint32 len;
    uint32 four = 4u;
    int32  idx;
    int32  opt;
    int32  nopt;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if((int32)r[1] != LX_SOL_SOCKET)
    {
        return -LX_ENOPROTOOPT;
    }
    if(r[3] == 0u)
    {
        return -LX_EFAULT;
    }
    if(r[4] != 0u)
    {
        if(arkrtos_copy_from_user(&len, (const void *)(arkrtos_uptr_t)r[4], 4u) != 0)
        {
            return -LX_EFAULT;
        }
        if(len < 4u)
        {
            return -LX_EINVAL;
        }
    }
    opt = (int32)r[2];
    if((opt == LX_SO_TYPE) || (opt == LX_SO_DOMAIN) || (opt == LX_SO_ERROR))
    {
        if(opt == LX_SO_TYPE)
        {
            val = (uint32)s_fd[idx].stype;
        }
        else if(opt == LX_SO_DOMAIN)
        {
            val = (uint32)LX_AF_INET;
        }
        else
        {
            val = 0u;
        }
        if(arkrtos_copy_to_user((void *)(arkrtos_uptr_t)r[3], &val, 4u) != 0)
        {
            return -LX_EFAULT;
        }
        if((r[4] != 0u) &&
           (arkrtos_copy_to_user((void *)(arkrtos_uptr_t)r[4], &four, 4u) != 0))
        {
            return -LX_EFAULT;
        }
        return 0;
    }
    nopt = lx_sock_opt_net(opt);
    if(nopt < 0)
    {
        return -LX_ENOPROTOOPT;
    }
    /* 其余交给 net：它把 4 字节结果直接写进用户地址（这就是为什么这里必须
     * 传用户指针，而不是内核栈上的临时变量）。 */
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_GETSOCKOPT,
                        (uint32)s_fd[idx].net, (uint32)nopt, r[3], 0u, 0u, &res);
    if(rc != 0)
    {
        return rc;
    }
    if((r[4] != 0u) && (arkrtos_copy_to_user((void *)(arkrtos_uptr_t)r[4], &four, 4u) != 0))
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* ------------------------------ getsockname(2) / getpeername(2) */
static int32 lx_sock_name(void *p_ctx, uint8 peer)
{
    int32  me;
    uint32 r[6];
    uint32 ip = 0u;
    uint32 port = 0u;
    uint32 op;
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    op = (peer != 0u) ? (uint32)ARKRTOS_NET_SUB_GETPEERNAME
                      : (uint32)ARKRTOS_NET_SUB_GETSOCKNAME;
    if(r[1] == 0u)
    {
        /* 不要地址：仍然问一次，让「socket 已经不在了」这类错误照常报出来。 */
        rc = lx_sock_submit(p_ctx, me, op, (uint32)s_fd[idx].net,
                            0u, 0u, 0u, 0u, &res);
        return (rc == 0) ? 0 : rc;
    }
    rc = lx_sock_submit(p_ctx, me, op, (uint32)s_fd[idx].net,
                        (uint32)(r[1] + 4ul), r[1], 0u, 0u, &res);
    if(rc != 0)
    {
        return rc;
    }
    rc = lx_sock_addr_fetch(r[1], &ip, &port);
    if(rc < 0)
    {
        return rc;
    }
    return lx_sock_addr_out(r[1], r[2], ip, port);
}

/* ------------------------------ shutdown(2) */
static int32 lx_shutdown(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if(r[1] > (uint32)ARKRTOS_NET_SHUT_RDWR)
    {
        return -LX_EINVAL;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SHUTDOWN,
                        (uint32)s_fd[idx].net, r[1], 0u, 0u, 0u, &res);
    return (rc == 0) ? 0 : rc;
}

/* ------------------------------ read(2) / write(2) 落在 socket 上
 * glibc 的 read/write 就是 63/64 号系统调用，落在 socket 上时等价于
 * recv(fd,buf,len,0) / send(fd,buf,len,0)。这两条是**阻塞**语义：非阻塞
 * socket 立刻回 -EAGAIN，阻塞 socket 在时间片上重放。 */
static int32 lx_sock_read(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if(r[2] == 0u)
    {
        return 0;
    }
    if(r[2] > 0xFFFFu)
    {
        return -LX_EMSGSIZE;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_RECV,
                        (uint32)s_fd[idx].net, r[1], r[2], 0u, 0u, &res);
    if((rc == -LX_EAGAIN) && ((s_fd[idx].sflags & LX_SOCKF_NONBLOCK) == 0u))
    {
        return lx_sock_replay(p_ctx, me);
    }
    return (rc == 0) ? res : rc;
}

static int32 lx_sock_write(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    if(r[2] > 0xFFFFu)
    {
        return -LX_EMSGSIZE;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_SEND,
                        (uint32)s_fd[idx].net, r[1], r[2], 0u, 0u, &res);
    if((rc == -LX_EAGAIN) && ((s_fd[idx].sflags & LX_SOCKF_NONBLOCK) == 0u))
    {
        return lx_sock_replay(p_ctx, me);
    }
    return (rc == 0) ? res : rc;
}

/* ------------------------------ close(2) 落在 socket 上
 * 把 net 那一格关掉，再把 fd 槽退掉。close 的错误本层不报（Linux 的 close
 * 也是一样：错误在写回时已经错过了报告时机），但**一定要退槽**，否则程序
 * 反复 close 会把 8 个槽耗干。 */
static int32 lx_sock_close(void *p_ctx)
{
    int32  me;
    uint32 r[6];
    int32  idx;
    int32  res = 0;
    int32  rc;

    me = lx_sock_me_ok();
    if(me < 0)
    {
        return -LX_ENOSYS;
    }
    lx_sock_load(me, p_ctx, r);

    idx = lx_sock_idx((int32)r[0]);
    if(idx < 0)
    {
        return idx;
    }
    rc = lx_sock_submit(p_ctx, me, ARKRTOS_NET_SUB_CLOSE,
                        (uint32)s_fd[idx].net, 0u, 0u, 0u, 0u, &res);
    if(rc == LX_RET_RETRY)
    {
        return rc;
    }
    (void)lx_fd_release(lx_proc_cur(), (uint32)idx);
    return 0;
}

/* ------------------------------ pselect6 的就绪探测
 * 谓词不能阻塞，所以这里只发一次请求：服务还没答复就按「未就绪」处理，由外面
 * 那一轮的时间片重放再来问。一次 net poll 至少要一个往返，所以第一轮一定报
 * 未就绪 —— 与「真的没准备好」在读数上不可区分，但两者都只让调用方多等一个
 * 时间片，不会给出错的就绪位。
 *
 * 缓存只在**一次 pselect6 调用内**有效：lx_pselect6 每轮先把所有 pok 清掉，
 * 第一遍扫描填、第二遍（写回那遍）复用。留到下一轮就会拿一个过期的就绪位把
 * 调用方骗起来。 */
#if (ARKRTOS_USE_LINUX_SELECT == 1)
static uint8 lx_sock_net_ready(uint32 idx, uint32 mask)
{
    int32 rc;

    if((uint32)s_fd[idx].net == 0u)
    {
        return 0u;
    }
    if(s_fd[idx].pok == 0u)
    {
        rc = arkrtos_net_call(ARKRTOS_NET_SUB_POLL, (uint32)s_fd[idx].net,
                              (uint32)(ARKRTOS_NET_POLL_IN | ARKRTOS_NET_POLL_OUT),
                              0u, 0u, 0u);
        if(rc > 0)
        {
            s_fd[idx].pbits = (uint8)rc;
            s_fd[idx].pok   = 1u;
        }
    }
    return ((s_fd[idx].pbits & mask) != 0u) ? 1u : 0u;
}
#endif /* ARKRTOS_USE_LINUX_SELECT == 1 */
/* 本次调用是不是「重放的那一次」。只有 socket 的处理函数会登记重放，所以
 * close/read/write 在前面看到 armed 就能确定要转给 socket 那条路 —— 那时
 * a0 已经被 lx_finish 清成 0，不能再用它当 fd。 */
static uint8 lx_sock_armed(void)
{
    int32 me = (int32)arkrtos_current_task_id;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return 0u;
    }
    return s_sockctx[me - 1].armed;
}

/* 退出收尸：把该槽上所有套接字的 net 句柄 best-effort 关掉。不等答复 ——
 * 退出路径上没有可以交回错误的调用者，服务任务会自己做完。必须在临界区
 * **之外**调用：net 请求是投消息，不能在关中断时做。 */
#if (ARKRTOS_USE_LINUX_FORK == 1)
static void lx_sock_close_all(uint32 slot)
{
    uint32 i;

    if(slot >= (uint32)LX_PROC_MAX)
    {
        return;
    }
    for(i = 0u; i < LX_MAX_FD; i++)
    {
        if((s_proc[slot].fd[i].used != 0u) &&
           (s_proc[slot].fd[i].kind == LX_FD_SOCKET) &&
           ((uint32)s_proc[slot].fd[i].net != 0u))
        {
            (void)arkrtos_net_call(ARKRTOS_NET_SUB_CLOSE,
                                   (uint32)s_proc[slot].fd[i].net,
                                   0u, 0u, 0u, 0u);
        }
    }
}
#endif /* ARKRTOS_USE_LINUX_FORK == 1 */
#endif /* ARKRTOS_USE_LINUX_SOCKET == 1 */
#if (ARKRTOS_USE_LINUX_FORK == 1)
/* 把父进程的状态复制进刚 fork 出来的子槽：fd 表逐项照抄（管道对象的读写端
 * 引用计数各加一 —— fd 被复制了一份，管道还是同一条，与 Linux 的 dup 语义
 * 一致），信号动作照抄；pending 与屏蔽字清零（新进程从 clone 的返回点开始，
 * 不该继承父进程还没投递的信号）。 */
static void lx_proc_fork_child(uint32 slot, int32 child, int32 parent)
{
    int32  pslot = lx_proc_find(parent);
    uint32 i;

    lx_proc_zero(slot);
    if(pslot >= 0)
    {
        for(i = 0u; i < LX_MAX_FD; i++)
        {
            s_proc[slot].fd[i] = s_proc[(uint32)pslot].fd[i];
            if(s_proc[slot].fd[i].used == 0u)
            {
                continue;
            }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
            if(s_proc[slot].fd[i].kind == LX_FD_SOCKET)
            {
                /* 净层没有 dup：套接字不跨 fork 继承。子进程看到一个空槽，
                 * 之后用它回 -EBADF —— 不给一个属于父进程的假句柄。 */
                s_proc[slot].fd[i].used   = 0u;
                s_proc[slot].fd[i].kind   = LX_FD_FILE;
                s_proc[slot].fd[i].net    = 0u;
                s_proc[slot].fd[i].stype  = 0u;
                s_proc[slot].fd[i].sflags = 0u;
                s_proc[slot].fd[i].pok    = 0u;
                s_proc[slot].fd[i].pbits  = 0u;
                continue;
            }
#endif
            if(s_proc[slot].fd[i].kind == LX_FD_FILE)
            {
                continue;
            }
            {
                int32 pi = (int32)s_proc[slot].fd[i].pipe;

                if((pi < 0) || (pi >= (int32)LX_PIPE_MAX) || (s_pipe[pi].used == 0u))
                {
                    continue;
                }
                if(s_proc[slot].fd[i].kind == LX_FD_PIPE_R)
                {
                    s_pipe[pi].readers++;
                }
                else
                {
                    s_pipe[pi].writers++;
                }
            }
        }
        for(i = 0u; i < (uint32)LX_NSIG; i++)
        {
            s_proc[slot].sig[i] = s_proc[(uint32)pslot].sig[i];
        }
        s_proc[slot].sig_blocked = s_proc[(uint32)pslot].sig_blocked;
        s_proc[slot].sig_pending = 0u;
        /* 标准流别名按槽下标 1:1 照抄：fd 表是逐项复制的，子进程里同一格
         * 装的是同一个对象的新引用，别名指过去仍然是对的。 */
        for(i = 0u; i < 3u; i++)
        {
            s_proc[slot].std_slot[i] = s_proc[(uint32)pslot].std_slot[i];
        }
    }
    lx_proc_bind(slot, child);
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* fork 出来的子进程是**自己那一组**的组长：它之后 clone 出来的线程挂在
     * 这个槽上，getpid 也回它。 */
    s_proc[slot].tgid     = child;
    s_proc[slot].tgid_gen = ((child > 0) && (child <= (int32)ARKRTOS_TASK_MAX_NUM)) ?
                            arkrtos_task_table[child - 1].generation : 0u;
#endif
}
#endif /* ARKRTOS_USE_LINUX_FORK == 1 */

static int32 lx_close(void *p_ctx)
{
    int32 fd = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 重放的那一次 a0 已经被 lx_finish 清成 0，参数只剩在本任务的槽里；
     * 这里必须先看 armed，否则会把 fd=0 当成「关标准流」直接回 0。 */
    if(lx_sock_armed() != 0u)
    {
        return lx_sock_close(p_ctx);
    }
#endif
    int32 idx;

    if((fd >= 0) && (fd <= 2))
    {
        /* 标准流别名：关掉它等于把重定向撤掉，fd 号本身仍在（与 Linux 一致：
         * 之后 write(1, ...) 回到控制台）。没有别名就是真标准流，关掉是空操作，
         * 不制造假句柄。 */
        idx = lx_std_slot(fd);
        if(idx < 0)
        {
            return 0;
        }
        lx_std_set(fd, -1);
        (void)lx_fd_release(lx_proc_cur(), (uint32)idx);
        return 0;
    }

    idx = lx_fd_find(fd);
    if(idx < 0)
    {
        return -LX_EBADF;
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 套接字要交给 net 去关：只摘掉 fd 项而不告诉协议栈，会在系统里
     * 漏一个 net socket（槽位数有上限，漏满之后所有 socket 调用都失败）。
     * 这一段属于正常路径，不能写在 if(idx < 0) 的块里。 */
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        return lx_sock_close(p_ctx);
    }
#endif

    (void)lx_fd_release(lx_proc_cur(), (uint32)idx);
    return 0;
}

static int32 lx_read(void *p_ctx)
{
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(lx_sock_armed() != 0u)
    {
        return lx_sock_read(p_ctx);
    }
#endif
    int32  fd   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 want = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;
    int32  idx;

    if((fd >= 0) && (fd <= 2))
    {
        /* 标准流：有别名就落到别名那一格（dup2(file, 0) 之后 read(0) 读的是
         * 那个文件或管道），没有别名就是「没有 stdin，读到 EOF」。 */
        idx = lx_std_slot(fd);
        if(idx < 0)
        {
            return 0;
        }
    }
    else
    {
        idx = lx_fd_find(fd);
        if(idx < 0)
        {
            return -LX_EBADF;
        }
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        return lx_sock_read(p_ctx);
    }
#endif
    if(s_fd[idx].kind == LX_FD_CONSOLE)
    {
        return 0;           /* 控制台引用：读到 EOF（与 fd 0 同一条语义） */
    }
    if(s_fd[idx].kind != LX_FD_FILE)
    {
        if(s_fd[idx].kind != LX_FD_PIPE_R)
        {
            return -LX_EBADF;       /* 管道的写端不能读 */
        }
        return lx_pipe_read(p_ctx, idx, ua, want);
    }
    if(want == 0u)
    {
        return 0;
    }
    if(ua == 0u)
    {
        return -LX_EFAULT;
    }

    /* 分块读写：内核只碰自己的 buf，用户地址每次都单独校验。 */
    while(done < want)
    {
        uint32 n = want - done;
        uint32 got = 0u;

        if(n > LX_IO_CHUNK)
        {
            n = LX_IO_CHUNK;
        }

        if(arkrtos_vfs_read_at(s_fd[idx].path, buf, n,
                               s_fd[idx].off + done, &got) < 0)
        {
            /* 分块失败：已交付的 done 字节必须记进游标。不记的话下一次
             * read 会从原处重放同一段，程序看到的是重复数据而不是短读。 */
            s_fd[idx].off += done;
            return (done > 0u) ? (int32)done : -LX_EIO;
        }
        if(got == 0u)
        {
            break;
        }
        if(arkrtos_copy_to_user((void *)(unsigned long)(ua + done), buf, got) != 0)
        {
            s_fd[idx].off += done;
            return (done > 0u) ? (int32)done : -LX_EFAULT;
        }

        done += got;
        if(got < n)
        {
            break;
        }
    }

    s_fd[idx].off += done;
    return (int32)done;
}

/* fd 1/2（以及 dup 出来的控制台引用）的写：整次 write 独占控制台。
 * 内核日志（[LA64U] ...）拿的是同一把锁，于是用户的一行不会被切成两截。
 * 之前逐字节直写 board_putc、不拿锁，子进程的 COW 日志能插在
 * "[fork] ... -> 1" 与 " ok" 之间，门禁看起来像少了一条读数——读数没少，
 * 是行被劈开了。 */
static int32 lx_console_out(unsigned long ua, uint32 want)
{
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;
    int32  held;

    if((ua == 0u) && (want != 0u))
    {
        return -LX_EFAULT;
    }
    held = arkrtos_console_lock();
    while(done < want)
    {
        uint32 n = want - done;
        uint32 i;

        if(n > LX_IO_CHUNK)
        {
            n = LX_IO_CHUNK;
        }
        if(arkrtos_copy_from_user(buf, (const void *)(unsigned long)(ua + done),
                                  n) != 0)
        {
            if(held != 0)
            {
                arkrtos_console_unlock();
            }
            return (done > 0u) ? (int32)done : -LX_EFAULT;
        }
        for(i = 0u; i < n; i++)
        {
            arkrtos_board_putc((char)buf[i]);
        }
        done += n;
    }
    if(held != 0)
    {
        arkrtos_console_unlock();
    }
    return (int32)want;
}

static int32 lx_write(void *p_ctx)
{
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(lx_sock_armed() != 0u)
    {
        return lx_sock_write(p_ctx);
    }
#endif
    int32  fd   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 want = ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32  idx;

    if((fd >= 0) && (fd <= 2))
    {
        /* 标准流：dup2(x, 1) 之后 write(1, ...) 要落到那条 fd 上（重定向的
         * 常见写法），没有别名才是真控制台。 */
        idx = lx_std_slot(fd);
        if(idx < 0)
        {
            return lx_console_out(ua, want);
        }
    }
    else
    {
        idx = lx_fd_find(fd);
        if(idx < 0)
        {
            return -LX_EBADF;
        }
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        return lx_sock_write(p_ctx);
    }
#endif
    /* 文件写仍是 -EROFS（openat 就不让可写打开）；这条路只认管道写端。
     * U-21 的 O_RDWR fd 也不走这里：文件内容的写只有「映射 + 写回」一条
     * 路，这里如实回 -ENOSYS，而不是给一个看似成功的空写。 */
    if(s_fd[idx].kind == LX_FD_FILE)
    {
        return -LX_ENOSYS;
    }
    if(s_fd[idx].kind == LX_FD_CONSOLE)
    {
        return lx_console_out(ua, want);
    }
    if(s_fd[idx].kind != LX_FD_PIPE_W)
    {
        return -LX_EBADF;
    }
    return lx_pipe_write(p_ctx, idx, ua, want);
}

static int32 lx_clock_gettime(void *p_ctx)
{
    uint32 clk = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 ms  = 0u;

    if(ua == 0u)
    {
        return -LX_EFAULT;
    }

    /* 这台机器没有墙钟：CLOCK_REALTIME 明确回 -ENOSYS，不拿开机时长
     * 冒充"现在几点"。 */
    if(clk == LX_CLOCK_REALTIME)
    {
        return -LX_ENOSYS;
    }
    if((clk != LX_CLOCK_MONOTONIC) && (clk != LX_CLOCK_MONOTONIC_RAW))
    {
        return -LX_EINVAL;
    }

    ms = arkrtos_kernel_get_time();     /* 内核毫秒计数 */

    {
        /* Linux ABI 的 timespec 用 long：LA64 上是两个 8 字节，RV32 上是
         * 两个 4 字节。这里用 long 而不是固定宽度类型，才和 UAPI 对齐。 */
        long ts[2];

        ts[0] = (long)(ms / 1000u);
        ts[1] = (long)((ms % 1000u) * 1000000u);
        if(arkrtos_copy_to_user((void *)(unsigned long)ua, ts,
                                (uint32)sizeof(ts)) != 0)
        {
            return -LX_EFAULT;
        }
    }

    return 0;
}

static int32 lx_uname(void *p_ctx)
{
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 0u);
    struct
    {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    } u;
    uint32 i;

    if(ua == 0u)
    {
        return -LX_EFAULT;
    }

    for(i = 0u; i < (uint32)sizeof(u); i++)
    {
        ((char *)&u)[i] = 0;
    }

    /* 不谎报发行版版本号：release/version 留空，只报"这是什么"。 */
    lx_copy_str(u.sysname,  (uint32)sizeof(u.sysname),  "ArkRTOS");
    lx_copy_str(u.nodename, (uint32)sizeof(u.nodename), "arkrtos");
#if defined(__loongarch64)
    lx_copy_str(u.machine,  (uint32)sizeof(u.machine),  "loongarch64");
#elif defined(__riscv)
    lx_copy_str(u.machine,  (uint32)sizeof(u.machine),  "riscv32");
#else
    lx_copy_str(u.machine,  (uint32)sizeof(u.machine),  "unknown");
#endif

    if(arkrtos_copy_to_user((void *)(unsigned long)ua, &u,
                            (uint32)sizeof(u)) != 0)
    {
        return -LX_EFAULT;
    }

    return 0;
}

/* pread64(fd, buf, count, pos)：从指定偏移读，不改 fd 的读游标。
 * 这正是 ld.so 铺共享库段要用的：它按段表随机访问同一个文件。 */
static int32 lx_pread64(void *p_ctx)
{
    int32  fd   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 want = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32 pos  = ARKRTOS_SVC_ARG(p_ctx, 3u);
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;
    int32  idx;

    idx = lx_fd_find(fd);
    if(idx < 0)
    {
        return -LX_EBADF;
    }
    if(want == 0u)
    {
        return 0;
    }
    if(ua == 0u)
    {
        return -LX_EFAULT;
    }

    while(done < want)
    {
        uint32 n = want - done;
        uint32 got = 0u;

        if(n > LX_IO_CHUNK)
        {
            n = LX_IO_CHUNK;
        }
        if(pos > (0xFFFFFFFFu - done))
        {
            return (done > 0u) ? (int32)done : -LX_EINVAL;
        }
        if(arkrtos_vfs_read_at(s_fd[idx].path, buf, n, pos + done, &got) < 0)
        {
            return (done > 0u) ? (int32)done : -LX_EIO;
        }
        if(got == 0u)
        {
            break;
        }
        if(arkrtos_copy_to_user((void *)(unsigned long)(ua + done), buf, got) != 0)
        {
            return (done > 0u) ? (int32)done : -LX_EFAULT;
        }
        done += got;
        if(got < n)
        {
            break;
        }
    }

    return (int32)done;
}

/* mprotect(addr, len, prot)
 *
 * 两种档的语义必须分开读，不许混着讲：
 *   - 关档（默认）：整窗是一块 PS=20 的 RWX 映射，硬件上拆不出只读页，这里
 *     **接受并记账**（返回 0）但保护不生效；
 *   - 开 ARKRTOS_USE_PAGING_MPROTECT 后：按 4 KB 页改硬件条目的 WE 位，写保护
 *     页上的写真的会触发故障。PROT_EXEC / PROT_NONE 仍只记账不强制（LA64 这
 *     一级页表没有 NR/NX 位，PROT_NONE 在本档等于只读），这一点写在 features.h
 *     与 §8.6 里，不在代码里假装。
 * 记账单独计数，就是为了让「接受」与「强制」在读数时能分开。 */
static int32 lx_mprotect(void *p_ctx)
{
    unsigned long addr = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long len  = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long prot = ARKRTOS_SVC_ARG(p_ctx, 2u);
    unsigned long win_base = (unsigned long)ARKRTOS_ELF_WINDOW_BASE;
    unsigned long win_end  = win_base + (uint32)ARKRTOS_ELF_WINDOW_SIZE;
    unsigned long span;

    if((addr & 0xFFFu) != 0u)
    {
        return -LX_EINVAL;
    }
    if(len == 0u)
    {
        return 0;
    }
    /* 减法写法：len 很大时加法会回绕。 */
    if((addr < win_base) || (len > (win_end - addr)))
    {
        return -LX_ENOMEM;
    }

#if (ARKRTOS_USE_PAGING_MPROTECT == 1)
    /* 端口层只认 4 KB 对齐的长度：Linux 允许任意 len（尾部按整页算），这里
     * 按 Linux 的语义向上取整到页，取整后仍越界就回 -ENOMEM。 */
    span = (len + 0xFFFul) & ~0xFFFul;
    if((span < len) || (addr > (win_end - 1ul)) || (span > (win_end - addr)))
    {
        return -LX_ENOMEM;
    }
    if(arkrtos_port_paging_protect(addr, span,
                                   ((prot & LX_PROT_WRITE) != 0u) ? 1u : 0u) != 0)
    {
        return -LX_ENOMEM;
    }
#else
    (void)prot;
    (void)span;
#endif

    s_stat[LX_STAT_MPROTECT]++;
    return 0;
}

static int32 lx_brk(void *p_ctx)
{
    unsigned long want  = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long base  = lx_heap_base();
    unsigned long limit = lx_heap_end();

    if(s_heap_cur == 0u)
    {
        s_heap_cur = base;
    }

    if(want == 0u)
    {
        lx_ret_ptr(p_ctx, s_heap_cur);
        return LX_RET_PTR_DONE;
    }
    if((want < base) || (want > limit))
    {
        /* Linux 的约定：失败时返回当前 break，不返回负值。 */
        lx_ret_ptr(p_ctx, s_heap_cur);
        return LX_RET_PTR_DONE;
    }

    s_heap_cur = want;
    lx_ret_ptr(p_ctx, s_heap_cur);
    return LX_RET_PTR_DONE;
}

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
/* ------------------------------------------------ 文件映射与写回（U-21） */

/* 写回缓冲是内核单份：VFS 的口径本来就是「同一时刻只能有一个读者拿着文件
 * 缓存」，所以两笔写回不能并行。忙标志把第二笔如实挡成 -EBUSY，而不是让两份
 * 数据在同一个缓冲里互相覆盖 —— 那会写回一个查不出来的错文件。 */
static uint8 s_fm_wb[LX_FILEMAP_WB_MAX];
static uint8 s_fm_wb_busy;

static int32 lx_fm_wb_begin(void)
{
    uint32 crit = ARKRTOS_ENTER_CRITICAL();
    int32  rc   = 0;

    if(s_fm_wb_busy != 0u)
    {
        rc = -LX_EBUSY;
    }
    else
    {
        s_fm_wb_busy = 1u;
    }
    ARKRTOS_EXIT_CRITICAL(crit);
    return rc;
}

static void lx_fm_wb_end(void)
{
    uint32 crit = ARKRTOS_ENTER_CRITICAL();

    s_fm_wb_busy = 0u;
    ARKRTOS_EXIT_CRITICAL(crit);
}

/* 把一条记账写回文件。返回 0 = 写回完成（或这条记账本来就不需要写回）。
 * 做法是「读整个文件 → 覆盖映射段 → 整文件写回」：VFS 没有 write_at，而这里
 * 不能糊（拼错一位就是一个静默的错文件）。 */
static int32 lx_filemap_sync(lx_filemap_t *p_m)
{
    uint32 size = 0u;
    uint32 mode = 0u;
    uint32 got  = 0u;
    uint32 n;
    int32  rc;

    if((p_m == 0) || (p_m->used == 0u))
    {
        return 0;
    }
    rc = lx_fm_wb_begin();
    if(rc != 0)
    {
        return rc;
    }
    rc = -LX_EIO;
    if(arkrtos_vfs_read_at(p_m->path, s_fm_wb, (uint32)sizeof(s_fm_wb),
                           0u, &got) < 0)
    {
        goto out;
    }
    if(arkrtos_vfs_app_stat(p_m->path, &size, &mode) < 0)
    {
        goto out;
    }
    if(size != got)
    {
        /* 文件在映射期间被别人改了长度：整文件写回会把它截断成 got 字节，
         * 那就是丢数据。如实 -EIO，让调用方决定。 */
        goto out;
    }
    n = p_m->len;
    if(p_m->off >= size)
    {
        n = 0u;     /* 整段都在 EOF 之后：写回没有意义，文件也不因此变长 */
    }
    else if(n > (size - p_m->off))
    {
        n = size - p_m->off;
    }
    if(n != 0u)
    {
        if(arkrtos_copy_from_user(&s_fm_wb[p_m->off],
                                  (const void *)(unsigned long)p_m->addr,
                                  n) != 0)
        {
            rc = -LX_EFAULT;
            goto out;
        }
        if(arkrtos_vfs_write_file(p_m->path, s_fm_wb, got) < 0)
        {
            goto out;
        }
    }
    rc = 0;
out:
    lx_fm_wb_end();
    return rc;
}

/* msync(addr, len, flags)：Linux 的 227。只写回本进程记账里的那段可写共享
 * 映射；堆窗口里的其他地址回 0（私有/匿名映射本来就没有可写回的东西，Linux
 * 也回 0）；窗口之外才 -ENOMEM。 */
static int32 lx_msync(void *p_ctx)
{
    unsigned long addr  = ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32 len   = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32 i;

    if(((addr & 0xFFFu) != 0u) || (len == 0u))
    {
        return -LX_EINVAL;
    }
    if((flags & ~(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC)) != 0u)
    {
        return -LX_EINVAL;
    }
    for(i = 0u; i < (uint32)LX_FILEMAP_MAX; i++)
    {
        lx_filemap_t *p_m = &s_fmap[i];
        int32 rc;

        if(p_m->used == 0u)
        {
            continue;
        }
        if((addr < (unsigned long)p_m->addr) ||
           (addr > ((unsigned long)p_m->addr + (unsigned long)p_m->len)))
        {
            continue;
        }
        if((unsigned long)len >
           (((unsigned long)p_m->addr + (unsigned long)p_m->len) - addr))
        {
            continue;
        }
        rc = lx_filemap_sync(p_m);
        if(rc != 0)
        {
            return rc;
        }
        s_stat[LX_STAT_FILEMAP]++;
        return 0;
    }
    if((addr >= lx_heap_base()) && (addr < lx_heap_end()))
    {
        return 0;       /* 堆窗口内、但没有写回账：没有可写回的东西 */
    }
    return -LX_ENOMEM;
}

/* mmap(fd, offset)：把文件内容装进堆窗口的一段（U-21）。
 * len / MAP_FIXED / PRIVATE|SHARED 已由 lx_mmap 校验过。 */
static int32 lx_mmap_file(void *p_ctx, uint32 len, uint32 prot, uint32 flags)
{
    unsigned long base  = lx_heap_base();
    unsigned long limit = lx_heap_end();
    unsigned long start;
    unsigned long end;
    uint32 uoff = ARKRTOS_SVC_ARG(p_ctx, 5u);
    uint32 size = 0u;
    uint32 i;
    uint32 n;
    uint32 done = 0u;
    uint8  buf[LX_IO_CHUNK];
    int32  idx  = lx_fd_find((int32)ARKRTOS_SVC_ARG(p_ctx, 4u));
    int32  rc   = 0;
    lx_filemap_t *p_m = 0;

    if(idx < 0)
    {
        return -LX_EBADF;
    }
    if(s_fd[idx].kind != LX_FD_FILE)
    {
        return -LX_EBADF;       /* 管道与标准流不能映射 */
    }
    if(((uoff & 0xFFFu) != 0u) || ((prot & LX_PROT_READ) == 0u))
    {
        return -LX_EINVAL;
    }
    if(((flags & LX_MAP_SHARED) != 0u) && ((prot & LX_PROT_WRITE) != 0u))
    {
        /* 可写共享映射：必须 O_RDWR 打开（Linux 用 -EACCES 表达），且文件
         * 必须装得进写回缓冲（现在就 -ENOMEM，而不是以后只写得回一半）。 */
        if(s_fd[idx].wr == 0u)
        {
            return -LX_EACCES;
        }
        if(s_fd[idx].size > (uint32)LX_FILEMAP_WB_MAX)
        {
            return -LX_ENOMEM;
        }
        for(i = 0u; i < (uint32)LX_FILEMAP_MAX; i++)
        {
            if(s_fmap[i].used == 0u)
            {
                p_m = &s_fmap[i];
                break;
            }
        }
        if(p_m == 0)
        {
            return -LX_ENOMEM;  /* 写回记账槽用满 */
        }
    }
    size = s_fd[idx].size;
    if(s_heap_cur == 0u)
    {
        s_heap_cur = base;
    }
    start = lx_align_up(s_heap_cur);
    if(start == 0u)
    {
        return -LX_ENOMEM;
    }
    if((start > limit) || (len > (limit - start)) ||
       ((start + (unsigned long)len) < start))
    {
        return -LX_ENOMEM;
    }
    end = lx_align_up(start + len);
    if((end == 0u) || (end > limit))
    {
        return -LX_ENOMEM;
    }
    s_heap_cur = end;

    /* 文件内容：EOF 之前按 LX_IO_CHUNK 分块拷进映射区，EOF 之后清零。
     * 越界读不伪造 SIGBUS（Linux 对超出文件末尾的整页也是零）。 */
    n = len;
    if(uoff >= size)
    {
        n = 0u;
    }
    else if(n > (size - uoff))
    {
        n = size - uoff;
    }
    while(done < n)
    {
        uint32 want = n - done;
        uint32 got  = 0u;

        if(want > LX_IO_CHUNK)
        {
            want = LX_IO_CHUNK;
        }
        if(arkrtos_vfs_read_at(s_fd[idx].path, buf, want, uoff + done, &got) < 0)
        {
            rc = -LX_EIO;
            goto fail;
        }
        if(got == 0u)
        {
            break;
        }
        if(arkrtos_copy_to_user((void *)(unsigned long)(start + done),
                                buf, got) != 0)
        {
            rc = -LX_EFAULT;
            goto fail;
        }
        done += got;
        if(got < want)
        {
            break;
        }
    }
    if(done < len)
    {
        for(i = 0u; i < (uint32)sizeof(buf); i++)
        {
            buf[i] = 0u;
        }
        while(done < len)
        {
            uint32 want = len - done;

            if(want > LX_IO_CHUNK)
            {
                want = LX_IO_CHUNK;
            }
            if(arkrtos_copy_to_user((void *)(unsigned long)(start + done),
                                    buf, want) != 0)
            {
                rc = -LX_EFAULT;
                goto fail;
            }
            done += want;
        }
    }

    if(p_m != 0)
    {
        p_m->used = 1u;
        p_m->addr = (uint32)start;
        p_m->len  = len;
        p_m->off  = uoff;
        p_m->size = size;
        lx_copy_str(p_m->path, (uint32)sizeof(p_m->path), s_fd[idx].path);
    }
    lx_ret_ptr(p_ctx, start);
    return LX_RET_PTR_DONE;

fail:
    /* 堆游标不回滚：这一段可能已经有页装进去了，把它再发给下一个映射就是
     * 两段映射共用一页（静默错答案）。记账清掉，这段地址就地作废。 */
    if(p_m != 0)
    {
        p_m->used = 0u;
    }
    return rc;
}
#endif /* ARKRTOS_USE_PAGING_FILEMAP == 1 */

static int32 lx_mmap(void *p_ctx)
{
    uint32 len   = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 3u);
    unsigned long base  = lx_heap_base();
    unsigned long limit = lx_heap_end();
    unsigned long start;
    unsigned long end;

    if(len == 0u)
    {
        return -LX_EINVAL;
    }
    /* MAP_PRIVATE 与 MAP_SHARED 必须且只能有一个：两个都给是调用方写错，
     * 一个都不给是没有语义的请求。MAP_SHARED 只在 ARKRTOS_USE_PAGING_SHM 档
     * 里真做（否则同样 -ENOSYS，不偷偷降级成私有：降级会让两个进程看起来
     * 共享、其实各改各的）。
     * MAP_FIXED 只在按需缺页档（ARKRTOS_USE_PAGING_DEMAND）真做：那一档才有
     * 按对装条目的能力，也才谈得上「把这一段原来的东西丢掉」。别的档回
     * -ENOSYS，不改成「在别处分配还给同一个地址」—— 那是骗调用方。 */
    if((((flags & (LX_MAP_PRIVATE | LX_MAP_SHARED)) == 0u)) ||
       (((flags & LX_MAP_PRIVATE) != 0u) &&
        ((flags & LX_MAP_SHARED) != 0u)))
    {
        return -LX_ENOSYS;
    }
#if (ARKRTOS_USE_PAGING_DEMAND != 1)
    if((flags & LX_MAP_FIXED) != 0u)
    {
        return -LX_ENOSYS;
    }
#endif
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    if((flags & LX_MAP_FIXED) != 0u)
    {
        unsigned long want = (unsigned long)ARKRTOS_SVC_ARG(p_ctx, 0u);
        int32 drc;

        /* MAP_FIXED 的意思就是「就在这个地址」：不原样接受就如实拒绝，绝不在
         * 别处分配一个还给同一个地址 —— 那样程序会以为自己手里是那块内存。
         *   1) 只支持匿名私有（文件映射 + MAP_FIXED 与共享 + MAP_FIXED 都回
         *      -ENOSYS，不半做）；
         *   2) 必须是 4 KB 对齐，且整段落在堆窗口里（窗口外是程序自己的映像、
         *      解释器区与栈区，把它们「丢掉」只会制造一场谁都不该负责的停机）；
         *   3) 丢掉这一段：驻留的私有对就地清零，写时复制共享的对先复制
         *      （对面保持原样），换出过的对连槽一起作废。
         * 成功后游标推到这一段之后，下一次「在哪儿都行」的 mmap 不会又发
         * 一次同一段地址。 */
        if((((flags & LX_MAP_ANONYMOUS) == 0u)) ||
           ((flags & LX_MAP_SHARED) != 0u))
        {
            return -LX_ENOSYS;
        }
        if(((want & 0xFFFul) != 0ul) || (want < base))
        {
            return -LX_EINVAL;
        }
        if((len > (limit - want)) || ((want + (unsigned long)len) < want))
        {
            return -LX_ENOMEM;
        }
        /* want 与堆窗口顶端 limit 都是页对齐的，上一句已经判过
         * len <= limit - want，故 want + len 不会越界也不会回绕，
         * align_up 最多补到 limit。这里只算长度交给端口。 */
        end = lx_align_up(want + len);
        drc = arkrtos_port_paging_discard(want, end - want);
        if(drc != 0)
        {
            return (drc == -2) ? -LX_ENOMEM : -LX_EINVAL;
        }
        if(end > s_heap_cur)
        {
            s_heap_cur = end;
        }
        lx_ret_ptr(p_ctx, want);
        return LX_RET_PTR_DONE;
    }
#endif

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* 没有 MAP_ANONYMOUS 就是文件映射（U-21）。arg2 = prot，arg4 = fd，
     * arg5 = offset，由这里往下传。 */
    if((flags & LX_MAP_ANONYMOUS) == 0u)
    {
        return lx_mmap_file(p_ctx, len, ARKRTOS_SVC_ARG(p_ctx, 2u), flags);
    }
#else
    /* 只认匿名映射；文件映射一律不假装支持。 */
    if((flags & LX_MAP_ANONYMOUS) == 0u)
    {
        return -LX_ENOSYS;
    }
#endif
#if (ARKRTOS_USE_PAGING_SHM != 1)
    if((flags & LX_MAP_SHARED) != 0u)
    {
        return -LX_ENOSYS;
    }
#endif

    if(s_heap_cur == 0u)
    {
        s_heap_cur = base;
    }

    start = lx_align_up(s_heap_cur);
    if(start == 0u)
    {
        return -LX_ENOMEM;
    }
#if (ARKRTOS_USE_PAGING_SHM == 1)
    if((flags & LX_MAP_SHARED) != 0u)
    {
        start = lx_pair_up(start);
        if(start == 0u)
        {
            return -LX_ENOMEM;
        }
    }
#endif
    /* 32 位端口上 start + len 会回绕：先按减法核对余量，再看加法本身
     * 有没有溢出。只靠 end > limit 是漏的——回绕之后 end 变成一个小
     * 地址，于是「要了 4 GB」会被当成成功，堆游标反而被写回低地址。 */
    if((start > limit) || (len > (limit - start)) ||
       ((start + (unsigned long)len) < start))
    {
        return -LX_ENOMEM;
    }
    end = lx_align_up(start + len);
    if((end == 0u) || (end > limit))
    {
        return -LX_ENOMEM;
    }

#if (ARKRTOS_USE_PAGING_SHM == 1)
    if((flags & LX_MAP_SHARED) != 0u)
    {
        /* 共享映射必须在 mmap 这一刻就真的有帧：fork 时父子共享的是那几对
         * 实体，而不是「还没付过费」的状态。建不出来就**什么都不改**
         * （游标不推进、记账不落），调用方拿到 -ENOMEM 可以原样重试。 */
        if(arkrtos_port_paging_map_shared(start, end - start) != 0)
        {
            return -LX_ENOMEM;
        }
    }
#endif
#if (ARKRTOS_USE_PAGING_SHM == 1)
    /* 共享映射按整对推进游标：留下的半个 4 KB 洞不还给分配器（这一段本来就
     * 是那一对的，给出去就会与共享帧重叠）。 */
    s_heap_cur = ((flags & LX_MAP_SHARED) != 0u) ? lx_pair_up(end) : end;
    if(s_heap_cur == 0u)
    {
        s_heap_cur = end;
    }
#else
    s_heap_cur = end;
#endif
    lx_ret_ptr(p_ctx, start);
    return LX_RET_PTR_DONE;
}

static int32 lx_munmap(void *p_ctx)
{
    unsigned long addr = ARKRTOS_SVC_ARG(p_ctx, 0u);

    if(addr == 0u)
    {
        return -LX_EINVAL;
    }

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* U-21：munmap 是写回的一个时机。命中记账就先写回；写回失败时**不**忘掉
     * 记账（munmap 是最后一次写回机会之一，吞掉错误就等于丢数据），把错误
     * 交回去让调用方重试。 */
    {
        uint32 i;
        uint32 len = ARKRTOS_SVC_ARG(p_ctx, 1u);

        if(((addr & 0xFFFu) != 0u) || (len == 0u))
        {
            return -LX_EINVAL;
        }
        for(i = 0u; i < (uint32)LX_FILEMAP_MAX; i++)
        {
            int32 rc;

            if(s_fmap[i].used == 0u)
            {
                continue;
            }
            if((unsigned long)s_fmap[i].addr != addr)
            {
                continue;
            }
            rc = lx_filemap_sync(&s_fmap[i]);
            if(rc != 0)
            {
                return rc;
            }
            s_fmap[i].used = 0u;
            break;
        }
    }
#endif

    /* 窗口是整块静态映射，没有页级取消映射这件事。这里只记账成功，
     * 绝不把地址还给分配器——否则下一次 mmap 会在程序还认为有效的地址
     * 上返回一段"新内存"（静默错答案）。 */
    return 0;
}

#if (ARKRTOS_USE_LINUX_FORK == 1)
/* ------------------------------------------------ clone / wait4（U-19） */

#if (ARKRTOS_USE_APP_QUOTA == 1)
/* SEC-G07：配额拒绝的唯一外部痕迹。没有这一行，调用者只看到 EAGAIN，
 * 分不出「空间用完」与「这个 App 的额度用完」——而这两件事的处置完全不同。 */
static void lx_appq_log(const char *what)
{
    int32 id = arkrtos_current_task_id;

#if (ARKRTOS_USE_AUDIT_LOG == 1)
    /* Both refusal sites (thread and fork) come through this function, so the
     * trail cannot end up knowing about only one of them. code carries the
     * owner: the record layout has no second spare field, and QUOTA_REFUSE's
     * code field is defined as the owner the limit was enforced on. */
    {
        uint32 owner = ((id > 0) && (id <= (int32)ARKRTOS_TASK_MAX_NUM))
                       ? (uint32)arkrtos_task_table[id - 1].app_owner : 0u;

        (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_QUOTA_REFUSE,
                                       (uint32)id, owner);
    }
#endif

    arkrtos_board_puts("[appq] refused ");
    arkrtos_board_puts(what);
    arkrtos_board_puts(": owner ");
    if((id > 0) && (id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        arkrtos_board_putu(arkrtos_task_table[id - 1].app_owner);
    }
    else
    {
        arkrtos_board_puts("?");
    }
    arkrtos_board_puts(" quota ");
    arkrtos_board_putu((uint32)ARKRTOS_APP_MAX_TASKS);
    arkrtos_board_puts(" tasks reached\n");
}
#endif

static int32 lx_fork_errno(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_ELF_ERR_QUOTA:
        /* 每 App 任务配额用完：Linux 在 RLIMIT_NPROC 命中时回的也是 EAGAIN。 */
        return -LX_EAGAIN;
    case ARKRTOS_ELF_ERR_BUSY:
        /* 地址空间全被活实例占着：这是并发上限，不是内存不够。 */
        return -LX_EAGAIN;
    case ARKRTOS_ELF_ERR_NOMEM:
        return -LX_ENOMEM;
    case ARKRTOS_ELF_ERR_PARAM:
        return -LX_EINVAL;
    default:
        return -LX_EINVAL;
    }
}

/* 在真正 fork 之前先确认还有记账位：反过来的顺序（先生后记账）会出现
 * 「子进程已经在跑、父却拿不到它」这种谁都不该负责的形态。 */
static int32 lx_child_slot(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)LX_MAX_CHILD; i++)
    {
        if(s_child[i].used == 0u)
        {
            return (int32)i;
        }
    }

    return -1;
}

#if (ARKRTOS_USE_LINUX_THREAD == 1)
/* ---------------------------------------------- 线程组（U-25）

 * 三个判据都刻意「现场数一遍」而不是维护计数器：
 *   - 一个组里有几个任务活着：数 s_proc_of_task（O(任务表)，只在退出与
 *     wait4 这些冷路径上发生）；
 *   - 计数器要押「每条退出路径都会回调」，而这个内核里退出路径不止一条
 *     （正常 exit、mprotect 故障摘任务、exit_group 强杀），漏一条就是
 *     槽位永远不释放 —— 那种缺陷只在跑满一轮循环之后才现形。
 */

/* 这个槽上除了 task_id 之外还有几个活着的任务。0 = 我是最后一个。 */
static uint32 lx_proc_others_live(uint32 slot, int32 task_id)
{
    uint32 i;
    uint32 n = 0u;

    if(slot >= (uint32)LX_PROC_MAX)
    {
        return 0u;
    }
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if(((int32)(i + 1u)) == task_id)
        {
            continue;
        }
        if(s_proc_of_task[i] != (uint8)(slot + 1u))
        {
            continue;
        }
        if(arkrtos_task_table[i].status == ARKRTOS_TASK_INVALID)
        {
            continue;
        }
        n++;
    }
    return n;
}

/* 当前任务的线程组号。没有进程状态（还没进过 ABI）就回自己的任务号 ——
 * 那正是「一个任务自成一组」的情形。 */
static int32 lx_tgid_of_current(void)
{
    int32 slot = lx_proc_find((int32)arkrtos_current_task_id);

    if((slot >= 0) && (s_proc[(uint32)slot].tgid > 0))
    {
        return s_proc[(uint32)slot].tgid;
    }
    return (int32)arkrtos_current_task_id;
}

/* 线程记账回调：在装载器的临界区里被调用，只写一格 uint8（见 elf.h）。 */
static void lx_thread_bind_hook(int32 parent, int32 child)
{
    int32 slot = lx_proc_find(parent);

    if(slot >= 0)
    {
        lx_proc_bind((uint32)slot, child);
    }
}

/**
* @brief clone(2) 的线程分支：共享地址空间 + 共享进程状态槽
* @details 与 fork 分支的差别有三条，每条都必须做对：
*            1) 地址空间**不复制**（arkrtos_elf_thread 直接绑父的那个 space），
*               于是父子的全局变量是同一份 —— 这正是线程要的；
*            2) 进程状态槽**共用**：子线程用 lx_thread_bind_hook 在装载器的
*               临界区里就被绑到父的槽上。晚一步，子线程第一次进 ABI 会给自己
*               惰性分配一份新槽，于是它有自己的 fd 表 —— 而这种错不报错，
*               只表现为「线程里 close 不掉主线程的 fd」；
*            3) 不登记 s_child[]：Linux 的 wait4 看不见线程（它们不是子进程），
*               只有整组退出才由组长那一份记账收尾。
*          flags 判据：认 LX_CLONE_THREAD_OK 里的位，多一位就 -EINVAL；
*          CLONE_VM 与 CLONE_THREAD 必须同时给（只给 VM 是「共享内存但独立
*          进程状态」，本内核没有这一档）；child_stack 必须非 0 且 16 字节
*          对齐（LA64 的 ABI 要求 sp 16 对齐，帧就压在它下面）。
*/
static int32 lx_clone_thread(void *p_ctx, uint32 flags, unsigned long child_stack)
{
    unsigned long ptid = ARKRTOS_SVC_ARG(p_ctx, 2u);   /* parent_tid */
    unsigned long ctid = ARKRTOS_SVC_ARG(p_ctx, 3u);   /* child_tid  */
    unsigned long tls  = ARKRTOS_SVC_ARG(p_ctx, 4u);   /* tls        */
    uint32 zero = 0u;
    int32  me   = (int32)arkrtos_current_task_id;
    int32  slot;
    int32  child = 0;
    int32  rc;

    if((flags & ~(uint32)LX_CLONE_THREAD_OK) != 0u)
    {
        return -LX_EINVAL;      /* CLONE_VFORK / CLONE_NEW* / 其它没实现的位 */
    }
    if((flags & (uint32)LX_CLONE_VM) == 0u)
    {
        return -LX_EINVAL;      /* 线程必须让子任务看见同一份内存 */
    }
    if((child_stack == 0ul) || ((child_stack & 15ul) != 0ul))
    {
        return -LX_EINVAL;
    }
    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EINVAL;
    }

    /* 槽位先确认在手上：等到子任务已经在跑才发现拿不到槽，就只能二选一
     * （让它和父共用一份是错的，当场收掉一个正在跑的任务是更大的错）。 */
    slot = (int32)lx_proc_cur();
    if(slot >= (int32)LX_PROC_MAX)
    {
        return -LX_EAGAIN;
    }

    /* 两个 SETTID 都是「先探一次可写再创建」：创建之后才发现的坏指针，
     * 只能留下一个已经跑起来的线程和一个说不清的返回码。 */
    if(((flags & (uint32)LX_CLONE_PARENT_SETTID) != 0u) && (ptid != 0ul))
    {
        if(arkrtos_copy_to_user((void *)ptid, &zero, 4u) != 0)
        {
            return -LX_EFAULT;
        }
    }
    if(((flags & (uint32)LX_CLONE_CHILD_SETTID) != 0u) && (ctid != 0ul))
    {
        if(arkrtos_copy_to_user((void *)ctid, &zero, 4u) != 0)
        {
            return -LX_EFAULT;
        }
    }

    rc = arkrtos_elf_thread(p_ctx, child_stack,
                            ((flags & (uint32)LX_CLONE_SETTLS) != 0u) ? tls : 0ul,
                            &child, lx_thread_bind_hook);
    if(rc != ARKRTOS_ELF_OK)
    {
#if (ARKRTOS_USE_APP_QUOTA == 1)
        if(rc == ARKRTOS_ELF_ERR_QUOTA)
        {
            lx_appq_log("thread");
        }
#endif
        return lx_fork_errno(rc);
    }
    if((child <= 0) || (child > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EINVAL;
    }

    if((flags & (uint32)LX_CLONE_CHILD_CLEARTID) != 0u)
    {
        s_lx_cleartid[child - 1] = ctid;    /* 0 = 没给，退出时什么也不做 */
    }

    {
        int32 tid = child;

        if(((flags & (uint32)LX_CLONE_PARENT_SETTID) != 0u) && (ptid != 0ul))
        {
            (void)arkrtos_copy_to_user((void *)ptid, &tid, 4u);
        }
        if(((flags & (uint32)LX_CLONE_CHILD_SETTID) != 0u) && (ctid != 0ul))
        {
            (void)arkrtos_copy_to_user((void *)ctid, &tid, 4u);
        }
    }

    return child;
}

/**
* @brief exit_group(2)：把同组全部任务按「正常退出」收掉，退出码记在每人身上
* @details 为什么不只退自己：Linux 的 exit_group 是「结束整个线程组」，
*          组长那一份 TCB 上的退出码才是 wait4 要读的东西 —— 一个线程自己
*          退掉、把组留给别人，会让父进程永远等不到那一组结束。
*
*          做法与 arkrtos_task_kill_internal / arkrtos_cpu_fault_kill 同一套：
*          摘出就绪链与延时链、置 INVALID、调 ABI 退出钩子（futex/select/
*          socket 的等待登记 + 本线程的 CLEARTID 清零）、再回收资源。
*          任务号先收集再动手：收集那一遍在临界区里，动手那一遍不可能在
*          临界区里（ABI 钩子要调 VFS / 拷贝用户内存）。
*
*          **SMP 边界**：这里强杀的是「可能正在另一核上跑」的任务。本内核的
*          线程档只在单核 QEMU 配置下被验证过；多核要真正的核间停核协议，
*          这一版没有，文档里写死。 */
static void lx_thread_group_exit(int32 code)
{
    int32  me   = (int32)arkrtos_current_task_id;
    int32  slot = lx_proc_find(me);
    int32  ids[ARKRTOS_TASK_MAX_NUM];
    uint32 n = 0u;
    uint32 i;
    uint32 k;

    if(slot < 0)
    {
        return;
    }

    {
        uint32 crit = ARKRTOS_ENTER_CRITICAL();

        for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
        {
            if(((int32)(i + 1u)) == me)
            {
                continue;
            }
            if(s_proc_of_task[i] != (uint8)((uint32)slot + 1u))
            {
                continue;
            }
            if(arkrtos_task_table[i].status == ARKRTOS_TASK_INVALID)
            {
                continue;
            }
            ids[n] = (int32)(i + 1u);
            n++;
        }
        ARKRTOS_EXIT_CRITICAL(crit);
    }

    for(k = 0u; k < n; k++)
    {
        int32  t   = ids[k];
        arkrtos_task_t *p_t = &arkrtos_task_table[t - 1];

        if(p_t->status == ARKRTOS_TASK_INVALID)
        {
            continue;           /* 期间已经被别处收掉了 */
        }

        {
            uint32 crit = ARKRTOS_ENTER_CRITICAL();

            arkrtos_ready_del(t - 1);
            arkrtos_delay_disarm(t - 1);
            p_t->status = ARKRTOS_TASK_INVALID;
#if (ARKRTOS_USE_EXIT_CODE == 1)
            /* 全组同一个退出码：Linux 只把「导致整组退出的那个码」报给父进程，
             * 而父进程等的是组长那份 TCB。 */
            p_t->exit_code      = code;
            p_t->exit_by_signal = 0u;
            p_t->exit_code_set  = 1u;
#endif
            ARKRTOS_EXIT_CRITICAL(crit);
        }

        /* 先让 ABI 记账收尾（futex/select/socket 的登记要放掉，CLEARTID 要清），
         * 再回收互斥锁与 VFS 句柄。顺序反了会让后者看到一份还在等的登记。 */
        arkrtos_linux_abi_on_task_exit(t);
        arkrtos_task_release_resources(t);
    }
}
#endif /* ARKRTOS_USE_LINUX_THREAD == 1 */

/**
* @brief clone(2)：只做 fork，不做线程
* @details 只接受 flags == 0 或 SIGCHLD、child_stack == 0。CLONE_VM /
*          CLONE_THREAD / CLONE_VFORK / CLONE_SETTLS / CLONE_FILES 等一律
*          -EINVAL —— 本内核没有共享地址空间的线程，也没有线程组。
*          成功时父返回子 pid（任务号），子返回 0（那一格已经在子帧里改好）。
*/
static int32 lx_clone(void *p_ctx)
{
    uint32 flags = (uint32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long child_stack = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 space = 0u;
    int32  child = 0;
    int32  slot;
#if (ARKRTOS_USE_LINUX_FORK == 1)
    int32  pslot = -1;
#endif
    int32  rc;

#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* 线程分支先分流：它要 CLONE_VM|CLONE_THREAD 与一条自己的栈，
     * 与 fork 的判据正好相反（fork 要求 child_stack == 0）。 */
    if((flags & (uint32)LX_CLONE_THREAD) != 0u)
    {
        return lx_clone_thread(p_ctx, flags, child_stack);
    }
#endif
    if((child_stack != 0ul) ||
       ((flags != 0u) && (flags != (uint32)LX_SIGCHLD_FLAG)))
    {
        return -LX_EINVAL;
    }

    slot = lx_child_slot();
    if(slot < 0)
    {
        return -LX_EAGAIN;
    }

#if (ARKRTOS_USE_LINUX_FORK == 1)
    /* 进程状态槽在 fork **之前**占好：占不到就不 fork（-EAGAIN）。反过来的
     * 顺序不行 —— 子进程已经跑起来了却没有 fd/信号表，只能二选一：让它和
     * 父共用一份（静默错答案），或者当场收掉一个正在跑的任务（更大的错）。 */
    pslot = lx_proc_alloc();
    if(pslot < 0)
    {
        return -LX_EAGAIN;
    }
#endif

    rc = arkrtos_elf_fork(p_ctx, &child, &space);
    if(rc != ARKRTOS_ELF_OK)
    {
#if (ARKRTOS_USE_LINUX_FORK == 1)
        lx_proc_free((uint32)pslot);
#endif
#if (ARKRTOS_USE_APP_QUOTA == 1)
        if(rc == ARKRTOS_ELF_ERR_QUOTA)
        {
            lx_appq_log("fork");
        }
#endif
        return lx_fork_errno(rc);
    }

    if((child <= 0) || (child > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        /* 装载器不会给出这种值；真给了就如实报错，不登记一条假记账。 */
#if (ARKRTOS_USE_LINUX_FORK == 1)
        lx_proc_free((uint32)pslot);
#endif
        return -LX_EINVAL;
    }

#if (ARKRTOS_USE_LINUX_FORK == 1)
    lx_proc_fork_child((uint32)pslot, child, (int32)arkrtos_current_task_id);
#endif

    s_child[slot].used       = 1u;
    s_child[slot].pid        = child;
    s_child[slot].generation = arkrtos_task_table[child - 1].generation;
    s_child[slot].parent     = (int32)arkrtos_current_task_id;

    return child;
}

/**
* @brief wait4(2)：只支持 options == 0 / WNOHANG
* @details 三条与 Linux 不同的边界（文档里写死）：
*            1) **不能阻塞**：子进程还活着且没给 WNOHANG 时回 -EAGAIN，
*               因为本内核没有「在系统调用里睡眠等事件」这条通路；
*            2) 状态只有「上报过退出码」这一种：exit_code_set == 0（故障 /
*               在别处被杀）时回 -EIO 并放掉记账 —— 编一个 0 会看起来像成功；
*            3) 槽位被复用后回 -ECHILD（靠注册代次认出来），不把新任务当儿子。
*          rusage 指针忽略、一个字节都不写。
*/
static int32 lx_wait4(void *p_ctx)
{
    int32  pid     = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ustatus = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 options = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32  me      = (int32)arkrtos_current_task_id;

    if((options & ~(uint32)LX_WNOHANG) != 0u)
    {
        return -LX_EINVAL;
    }
    if(me <= 0)
    {
        return -LX_ECHILD;      /* 内核上下文不可能有儿子 */
    }

    #if (ARKRTOS_USE_LINUX_FORK == 1)
    {
        /* 上次睡下时登记的标记到此作废：它只在「正在睡」的时候有意义。
         * 重放这条系统调用时先清掉，避免上一个时间片留下的标记让退出钩子
         * 对着一个没在等的任务发唤醒。 */
        uint32 crit0 = ARKRTOS_ENTER_CRITICAL();

        s_lx_wait[me - 1] = 0u;
        ARKRTOS_EXIT_CRITICAL(crit0);
    }
    #endif

    /* 阻塞是循环重扫，不是「睡下就信唤醒」：子进程可能在父进程睡下之前
     * 就已经 exit（退出钩子的唤醒投给了一个还没睡的父进程），也可能死在
     * 故障路径上（那条路不经过退出钩子）。两种情况都由「醒来再扫一遍」
     * 兜住，最多多睡一个 LX_WAIT_SLICE_MS 的时间片。 */
    for(;;)
    {
        uint32 crit;
        uint32 i;
        int32  alive = 0;

        crit = ARKRTOS_ENTER_CRITICAL();
        for(i = 0u; i < (uint32)LX_MAX_CHILD; i++)
        {
            arkrtos_task_t *p_task;
            int32 st;

            if(s_child[i].used == 0u)
            {
                continue;
            }
            if(s_child[i].parent != me)
            {
                continue;       /* 别人的儿子：不认 */
            }
            if((pid > 0) && (pid != s_child[i].pid))
            {
                continue;
            }

            if((s_child[i].pid <= 0) || (s_child[i].pid > (int32)ARKRTOS_TASK_MAX_NUM))
            {
                s_child[i].used = 0u;
                continue;
            }

            p_task = &arkrtos_task_table[s_child[i].pid - 1];

            if(p_task->generation != s_child[i].generation)
            {
                /* 槽位已被别人复用：那个实例早就没了，退出码也无从取回。 */
                s_child[i].used = 0u;
                continue;
            }

            if(p_task->status != ARKRTOS_TASK_INVALID)
            {
                if((options & (uint32)LX_WNOHANG) != 0u)
                {
                    ARKRTOS_EXIT_CRITICAL(crit);
                    return 0;
                }
                alive = 1;      /* 还有活着的儿子：可以睡一觉再扫 */
                continue;
            }

            #if (ARKRTOS_USE_EXIT_CODE == 1)
            if(p_task->exit_code_set == 0u)
            {
                s_child[i].used = 0u;
                ARKRTOS_EXIT_CRITICAL(crit);
                return -LX_EIO;
            }
            #endif

            st = s_child[i].pid;
            /* 拷状态在临界区外做（copy_to_user 可能触发缺页，不能在关中断
             * 的窗口里走那条路）。记账先不放：写不进去时调用者改好指针再
             * 来一次，这一次的退出码还在。 */
            ARKRTOS_EXIT_CRITICAL(crit);
            if(ustatus != 0ul)
            {
                uint32 status = (uint32)0u;

                #if (ARKRTOS_USE_EXIT_CODE == 1)
                /* 两种终态在 Linux 里编码不同：
                 *   exit(code)  -> status = code << 8（WIFEXITED=1）
                 *   被信号杀死  -> status = 信号号（WIFSIGNALED=1）
                 * 混着写会让 WIFEXITED 对一次 SIGSEGV 也为真。 */
                if(p_task->exit_by_signal != 0u)
                {
                    status = (uint32)p_task->exit_code & 0x7Fu;
                }
                else
                {
                    status = (((uint32)p_task->exit_code & 0xFFu) << 8);
                }
                #endif
                if(arkrtos_copy_to_user((void *)(unsigned long)ustatus,
                                        &status, 4u) != 0)
                {
                    return -LX_EFAULT;
                }
            }
            crit = ARKRTOS_ENTER_CRITICAL();
            s_child[i].used = 0u;
            ARKRTOS_EXIT_CRITICAL(crit);
            return st;
        }

        if(alive == 0)
        {
            ARKRTOS_EXIT_CRITICAL(crit);
            return -LX_ECHILD;
        }

        #if (ARKRTOS_USE_LINUX_FORK == 1)
        /* 登记「我卡在 wait4」：子退出 / 跨进程信号只在登记过的时候唤醒，
         * 不打扰因为别的原因在等的任务。 */
        s_lx_wait[me - 1] = 1u;
        #endif
        /* 本内核在 SVC 处理程序内部**没有真正睡眠**的通路：阻塞只是
         * 「置 WAIT + 请求切换」，切换在异常出口结算，函数本身立刻返回。
         * 所以这条系统调用必须能被重放：把 ERA 退回 syscall 指令，任务被
         * 唤醒（子进程退出钩子 / 超时时间片）后从用户态重新发起这次 wait4，
         * 届时再扫一遍记账。参数寄存器在帧里没有被改写，重放读到同一组值。 */
        if(arkrtos_port_svc_restart(p_ctx) != 0)
        {
            /* 端口不给重放能力：如实回 -EAGAIN，不假装阻塞过。 */
            #if (ARKRTOS_USE_LINUX_FORK == 1)
            s_lx_wait[me - 1] = 0u;
            #endif
            ARKRTOS_EXIT_CRITICAL(crit);
            return -LX_EAGAIN;
        }
        /* 置 WAIT + 武装 LX_WAIT_SLICE_MS 超时 + 请求切换；不在这里等它返回。 */
        (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
        ARKRTOS_EXIT_CRITICAL(crit);
        /* 这次调用的返回值会被重放的那一次覆盖，写什么都无所谓。 */
        return 0;
    }
}
#endif /* ARKRTOS_USE_LINUX_FORK == 1 */

#if (ARKRTOS_USE_LINUX_FUTEX == 1)
/* ------------------------------- futex（WAIT/WAKE/REQUEUE/BITSET 六档） */

/* 按「基准优先级 + 本任务身上还挂着的 PI 等待者」重算有效优先级。
 * base_priority 是唯一真源（与内核互斥锁 arkrtos_mtx_recalc_task_priority
 * 同一口径）：拿被抬过以后的值当原始值，一次嵌套继承就会把优先级永久抬着，
 * 那是泄漏不是继承。
 *
 * 只扫本表、不读用户内存：跨进程的 PI 等待者不在同一张页表里，字读出来
 * 没有意义；登记时抄下的 pi_owner 才是可靠依据。 */
static void lx_pi_recalc_owner(int32 owner_tid)
{
    arkrtos_task_t *p_owner;
    uint8 pri;
    int32 i;

    if((owner_tid <= 0) || ((uint32)owner_tid > (uint32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    p_owner = &arkrtos_task_table[(uint32)owner_tid - 1u];
    pri = p_owner->base_priority;
    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        uint8 wpri;

        if((s_futex_used[i - 1] == 0u) || (s_futex_pi[i - 1] == 0u) ||
           (s_futex_pi_owner[i - 1] != owner_tid))
        {
            continue;
        }
        wpri = arkrtos_task_table[(uint32)i - 1u].base_priority;
        if(wpri < pri)
        {
            pri = wpri;
        }
    }
    if(p_owner->priority != pri)
    {
        uint8 old_pri = p_owner->priority;

        p_owner->priority = pri;
        arkrtos_ready_reprio((int32)((uint32)owner_tid - 1u), old_pri);
    }
}

/* 本次退出的任务如果还挂着 futex 等待，把它那一槽清掉。否则任务号被复用后
 * 新任务会继承一条「已经在等」的旧记录。PI 等待者走的时候还要把原持有者的
 * 优先级算回去：被杀 / 超时以后继承不该继续挂着。 */
static void lx_futex_forget(int32 task_id)
{
    if((task_id > 0) && (task_id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        int32 owner = s_futex_pi_owner[task_id - 1];

        s_futex_used[task_id - 1]     = 0u;
        s_futex_woke[task_id - 1]     = 0u;
        s_futex_key[task_id - 1]      = ~0UL;
        s_futex_uaddr[task_id - 1]    = 0ul;
        s_futex_val[task_id - 1]      = 0u;
        s_futex_bitset[task_id - 1]   = 0u;
        s_futex_deadline[task_id - 1] = 0u;
        s_futex_timed[task_id - 1]    = 0u;
        s_futex_pi[task_id - 1]       = 0u;
        s_futex_rqpi[task_id - 1]     = 0u;
        s_futex_pi_owner[task_id - 1] = 0;
        if(owner != 0)
        {
            lx_pi_recalc_owner(owner);
        }
    }
}

/* 把用户地址换成 futex 键。
 * 开了分页窗口时键是「物理帧 + 页内偏移」：每个进程有自己的 8 KB 对窗口，
 * 同一个 VA 在父与子身上指向不同帧，而 futex 要的是「同一个字」，所以键必须
 * 落在物理帧上。窗口之外（栈、内核直映射区）没有可查的帧，回 -1，调用方回
 * ~0UL（无效）—— 不拿 VA 假装它是键，那会让两个无关进程在同一个 VA 上互相
 * 叫醒。没有窗口时（MPU 后端）整个地址空间只有一份，VA 本身就是稳定的键。 */
static unsigned long lx_futex_key_task(int32 task_id, unsigned long uaddr)
{
#if (ARKRTOS_USE_PAGING == 1)
    arkrtos_task_t *p_task;
    unsigned long   win;
    unsigned long   off;
    uint32          space;
    uint32          pa;
#endif

    /* 上面先判了 > 0，所以这里再转成无符号只为了不触发 -Wsign-compare；
     * 转换不会改变任何一个合法任务号的判定。 */
    if((task_id <= 0) ||
       ((uint32)task_id > (uint32)ARKRTOS_TASK_MAX_NUM))
    {
        return ~0UL;
    }
#if (ARKRTOS_USE_PAGING == 1)
    win = arkrtos_paging_window_va();
    if(win == 0ul)
    {
        /* 没有分页窗口（MPU 后端 / 未开分页）：整个地址空间只有一份，
         * 虚拟地址本身就是稳定的键。 */
        return (unsigned long)uaddr;
    }
    if(uaddr < win)
    {
        return ~0UL;
    }
    off = uaddr - win;
    if(off >= ((unsigned long)arkrtos_paging_pair_total() * 0x2000ul))
    {
        return ~0UL;
    }
    p_task = &arkrtos_task_table[task_id - 1];
    space = arkrtos_paging_space_of(p_task);
    pa = arkrtos_paging_pair_pa(space, (uint32)(off / 0x2000ul));
    if(pa == 0u)
    {
        return ~0UL;
    }
    return (unsigned long)pa + (off % 0x2000ul);
#else
    /* 没有分页后端：整个地址空间只有一份，虚拟地址本身就是稳定的键。 */
    return (unsigned long)uaddr;
#endif
}

/* 当前任务自己的键（绝大多数调用者的用法）。 */
static unsigned long lx_futex_key(unsigned long uaddr)
{
    return lx_futex_key_task((int32)arkrtos_current_task_id, uaddr);
}

/* 当前进程有没有「未屏蔽的待投递信号」。futex WAIT 在 Linux 上遇到这种情形
 * 回 -EINTR 而不是继续睡。 */
static uint8 lx_futex_sig_pending(void)
{
    return lx_sig_pending_unblocked();
}

/* 读用户给的 timespec，换成内核毫秒坐标上的截止时刻。
 *
 * @param abs_flag 1 = 绝对时刻（WAIT_BITSET 的语义），0 = 相对时长（WAIT）
 * @param p_dl     出参：截止时刻（内核毫秒）
 * @return 0 = 没给 timeout（无限等）；1 = 已算出截止；负数 = errno
 *
 * 为什么先换成「截止时刻」而不是每次重放都算 now + 时长：相对超时只该从
 * 第一次进入算起，每次时间片醒来重新加一遍的话永远也到不了期。
 * 绝对时刻用 32 位毫秒表示时只保留「从现在起 2^31-1 ms 以内」的部分，
 * 更远的按上限处理；这样到期判定可以用 int32 差值，不受计数回绕影响。 */
static int32 lx_futex_timeout(void *p_ctx, uint32 abs_flag, uint32 *p_dl)
{
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 3u);
    long          ts[2];
    uint32        ms;
    uint32        now;

    if(ua == 0ul)
    {
        return 0;
    }
    if(arkrtos_copy_from_user(ts, (const void *)ua, (uint32)sizeof(ts)) != 0)
    {
        return -LX_EFAULT;
    }
    if((ts[0] < 0) || (ts[1] < 0) || (ts[1] >= 1000000000L))
    {
        return -LX_EINVAL;
    }
    if(ts[0] > 2147483L)
    {
        /* 2^31-1 ms 约 24.8 天；再长的时长按上限算，避免 32 位乘加溢出。 */
        ms = 0x7FFFFFFFu;
    }
    else
    {
        ms = ((uint32)ts[0] * 1000u) + (uint32)(ts[1] / 1000000L);
    }
    now = arkrtos_kernel_get_time();
    if(abs_flag != 0u)
    {
        if((int32)(ms - now) <= 0)
        {
            *p_dl = now;                        /* 已经过去：立刻到期 */
            return 1;
        }
        ms -= now;
    }
    if(ms > 0x7FFFFFFFu)
    {
        ms = 0x7FFFFFFFu;                       /* 上限约 24.8 天 */
    }
    *p_dl = now + ms;
    return 1;
}

/**
* @brief futex(2)：WAIT / WAKE / REQUEUE / CMP_REQUEUE / WAIT_BITSET /
*        WAKE_BITSET 六档
* @details 与 Linux 一致的语义：
*            1) WAIT 在入口处读到的字 != val 时回 -EAGAIN（不睡）；
*            2) 有未屏蔽信号时回 -EINTR（不睡）；
*            3) WAKE 唤醒同一键上最多 val 个等待者，返回实际唤醒数；
*            4) WAIT 带 timeout（相对）到期回 -ETIMEDOUT；WAIT_BITSET 的
*               timeout 与 Linux 一样是**绝对**时刻（本内核时钟是单调毫秒）；
*            5) WAIT_BITSET / WAKE_BITSET 用 val3 当位集，只唤醒位集相交的
*               等待者；普通 WAIT 登记的是全 1，所以普通 WAKE 能叫醒它，
*               WAKE_BITSET 的任意非零位集也能；
*            6) REQUEUE 把同一键上的等待者（先唤醒 nr_wake 个）改挂到
*               uaddr2；CMP_REQUEUE 在动手前先核对 uaddr 上的字等于 val3，
*               不等回 -EAGAIN。Linux 把 nr_requeue 塞在第四个参数
*               （timeout 那一格）里，这里照抄这个复用。返回值是**唤醒数
*               加改挂数**（man futex 的措辞是 "woken up or requeued"）：
*               只回唤醒数时，nr_wake=0 的 CMP_REQUEUE 永远回 0，调用方
*               没法从返回值知道「挂过去了没有」——glibc 的
*               pthread_cond_broadcast 正是按这个数判断的。
*          与 Linux 不同的边界（文档里写死）：
*            - 键取自「物理帧 + 页内偏移」，只有落在 App 分页窗口里的字才
*              有键；栈上的字（不在窗口里）如实回 -ENOSYS；
*            - FD 类 op（2）以及 13/14 一律 -ENOSYS；PI 类 5..8 与 PI 条件
 *              变量的 11/12 有实现（偏差见 §8.4 / §8.4b）；
*            - 本机没有墙钟，FUTEX_CLOCK_REALTIME 带 timeout 时如实
*              -ENOSYS（不拿开机时长冒充绝对时刻）；
*            - 本内核没有无限睡眠：等待按 LX_WAIT_SLICE_MS 时间片重放，没
*              给 timeout 时重放醒来只要值没变就继续睡（Linux 语义一致）；
*            - 被 requeue 的等待者把「期望值」更新为 uaddr2 上的当前字：
*              requeue 本身不唤醒它（与 Linux 一致），但它下次时间片醒来
*              不会因为两个地址的字不同而被误判成「已唤醒」；
*            - 重放识别按「任务自己那一槽还在用」判定，不按地址比对：键在
*              requeue 时已经搬到 uaddr2，按地址会比不出自己。
*/
/* 按键唤醒至多 want 个等待者（位掩码过滤）。抽出来是因为 CLONE_CHILD_CLEARTID
 * 的退出路径也要走同一套判据：两处各写一遍，迟早在「唤醒谁」上分叉。 */
static int32 lx_futex_wake_key(unsigned long key, int32 want, uint32 mask)
{
    int32  woke = 0;
    int32  i;
    uint32 crit;

    if((want < 0) || (mask == 0u))
    {
        return -LX_EINVAL;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    for(i = 1; (i <= (int32)ARKRTOS_TASK_MAX_NUM) && (woke < want); i++)
    {
        if((s_futex_used[i - 1] != 0u) && (s_futex_pi[i - 1] == 0u) &&
           (s_futex_key[i - 1] == key) &&
           ((s_futex_bitset[i - 1] & mask) != 0u))
        {
            s_futex_woke[i - 1] = 1u;
            (void)arkrtos_task_wakeup_internal(i);
            woke++;
        }
    }
    ARKRTOS_EXIT_CRITICAL(crit);
    return woke;
}

/* ------------------------------- PI 类 op（LOCK_PI / UNLOCK_PI / TRYLOCK_PI） */

/* 读 / 写 futex 字（4 字节）。PI 的持有者与等待者一定在同一张页表里（键就是
 * 同一个物理字），所以调用方给的地址就是那个字的地址；键只用来确认「是同一
 * 个字」，不用它反查地址。 */
static int32 lx_futex_get_word(unsigned long uaddr, uint32 *p_word)
{
    if(arkrtos_copy_from_user(p_word, (const void *)uaddr, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    return 0;
}

static int32 lx_futex_put_word(unsigned long uaddr, uint32 word)
{
    if(arkrtos_copy_to_user((void *)uaddr, &word, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* 同一键上基准优先级最高的 PI 等待者（数值越小优先级越高，同级按任务号）。
 * 交接按这个顺序，否则「优先级继承」只是名义上的：低优先级的等待者先拿走
 * 锁，高优先级那个还得再排一轮。 */
static int32 lx_pi_top_waiter(unsigned long key)
{
    int32 best = -1;
    int32 i;

    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if((s_futex_used[i - 1] == 0u) || (s_futex_pi[i - 1] == 0u) ||
           (s_futex_key[i - 1] != key))
        {
            continue;
        }
        if(best < 0)
        {
            best = i;
            continue;
        }
        if(arkrtos_task_table[(uint32)i - 1u].base_priority <
           arkrtos_task_table[(uint32)best - 1u].base_priority)
        {
            best = i;
        }
    }
    return best;
}

static uint32 lx_pi_waiter_count(unsigned long key)
{
    uint32 n = 0u;
    int32  i;

    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if((s_futex_used[i - 1] != 0u) && (s_futex_pi[i - 1] != 0u) &&
           (s_futex_key[i - 1] == key))
        {
            n++;
        }
    }
    return n;
}

/* UNLOCK_PI：不是持有者 ⇒ -EPERM；有人等 ⇒ 把所有权**直接交接**给基准优先级
 * 最高的那个（写字 + 唤醒），没人等 ⇒ 清 0。交接而不是「叫醒后大家再抢」，
 * 是因为本内核不能让所有等待者同时跑起来抢同一个字（那正是优先级反转）。 */
static int32 lx_futex_pi_unlock(unsigned long key, unsigned long uaddr,
                                uint32 word, int32 me)
{
    uint32 crit;
    int32  top;
    uint32 nxt;
    int32  i;

    if((word & (uint32)LX_FUTEX_TID_MASK) != (uint32)me)
    {
        return -LX_EPERM;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    top  = ((word & (uint32)LX_FUTEX_WAITERS) != 0u) ?
           lx_pi_top_waiter(key) : -1;
    if(top > 0)
    {
        /* 交接后队列里还有人就留着 WAITERS 位，让新持有者的 UNLOCK_PI 知道
         * 还要再交一次；OWNER_DIED 原样带过去（上一个持有者死掉留下的记号，
         * 后来者必须看得见）。剩下那些等待者的「持有者」也一并改成新持有者，
         * 否则优先级会一直挂在旧持有者头上。 */
        nxt = (uint32)top | (word & (uint32)LX_FUTEX_OWNER_DIED);
        if(lx_pi_waiter_count(key) > 1u)
        {
            nxt |= (uint32)LX_FUTEX_WAITERS;
        }
        for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
        {
            if((i == top) || (s_futex_used[i - 1] == 0u) ||
               (s_futex_pi[i - 1] == 0u) || (s_futex_key[i - 1] != key))
            {
                continue;
            }
            s_futex_pi_owner[i - 1] = top;
        }
        s_futex_pi_owner[top - 1] = top;
        s_futex_woke[top - 1]     = 1u;
    }
    else
    {
        nxt = 0u;
    }
    ARKRTOS_EXIT_CRITICAL(crit);

    if(lx_futex_put_word(uaddr, nxt) != 0)
    {
        return -LX_EFAULT;
    }
    if(top > 0)
    {
        (void)arkrtos_task_wakeup_internal(top);
    }
    lx_pi_recalc_owner(me);
    if(top > 0)
    {
        lx_pi_recalc_owner(top);
    }
    return 0;
}

/* ------------------- PI 条件变量：WAIT_REQUEUE_PI / CMP_REQUEUE_PI ------- */

/* WAIT_REQUEUE_PI(uaddr, val, uaddr2[, timeout])：等 uaddr 上的信号字，等到
 * CMP_REQUEUE_PI 把整队搬到 uaddr2（已持有的 PI 锁）上，排头那个直接接手。
 * 与 Linux 逐条对齐的部分：值不符 ⇒ -EAGAIN；uaddr2 没人持有 ⇒ -EINVAL
 * （Linux 也这么判：requeue 的目标必须是一把有人攥着的 PI 锁，否则「接手」
 * 没有对象）；被搬走的排头由 CMP_REQUEUE_PI 把字写成它的 tid，这里重放时
 * 只确认所有权，不再读字（用户可能已经改了它）。
 * 与 Linux 不同的边界：timeout 按相对时长解释（与 WAIT/LOCK_PI 同口径），
 * 不是绝对时刻；本内核没有无限睡眠，等待按 LX_WAIT_SLICE_MS 时间片重放。 */
static int32 lx_futex_wait_requeue_pi(void *p_ctx, unsigned long uaddr,
                                      uint32 val, int32 me)
{
    unsigned long key  = 0ul;
    unsigned long key2 = 0ul;
    unsigned long uaddr2;
    uint32 word  = 0u;
    uint32 word2 = 0u;
    uint32 tid2;
    uint8  replay;
    uint8  woke  = 0u;
    uint8  timed = 0u;
    uint32 dl    = 0u;
    uint32 crit;
    int32  rc;

    /* 重放识别只看自己那一槽：一个任务睡着时不可能再发起一次 WAIT。 */
    crit = ARKRTOS_ENTER_CRITICAL();
    replay = ((s_futex_used[me - 1] != 0u) &&
              (s_futex_rqpi[me - 1] != 0u)) ? 1u : 0u;
    if(replay != 0u)
    {
        key   = s_futex_key[me - 1];
        uaddr = s_futex_uaddr[me - 1];
        woke  = s_futex_woke[me - 1];
        timed = s_futex_timed[me - 1];
        dl    = s_futex_deadline[me - 1];
    }
    ARKRTOS_EXIT_CRITICAL(crit);

    if(replay == 0u)
    {
        uaddr2 = ARKRTOS_SVC_ARG(p_ctx, 4u);
        if(uaddr2 == 0ul)
        {
            return -LX_EINVAL;
        }
        key = lx_futex_key(uaddr);
        if(key == ~0UL)
        {
            return -LX_ENOSYS;
        }
        rc = lx_futex_get_word(uaddr, &word);
        if(rc != 0)
        {
            return rc;
        }
        if(word != val)
        {
            return -LX_EAGAIN;      /* 值已经变了：与 WAIT 同口径，别睡 */
        }
        key2 = lx_futex_key(uaddr2);
        if(key2 == ~0UL)
        {
            return -LX_ENOSYS;
        }
        rc = lx_futex_get_word(uaddr2, &word2);
        if(rc != 0)
        {
            return rc;
        }
        tid2 = word2 & (uint32)LX_FUTEX_TID_MASK;
        if(tid2 == 0u)
        {
            /* 空锁：Linux 回 -EINVAL（requeue 的目标必须有人持有）。 */
            return -LX_EINVAL;
        }
        /* 置 WAITERS：持有者的 UNLOCK_PI 靠它知道要交接。与 LOCK_PI 同一
         * 口径，区别只在「谁接手」由 CMP_REQUEUE_PI 决定。 */
        if((word2 & (uint32)LX_FUTEX_WAITERS) == 0u)
        {
            rc = lx_futex_put_word(uaddr2, word2 | (uint32)LX_FUTEX_WAITERS);
            if(rc != 0)
            {
                return rc;
            }
        }
        rc = lx_futex_timeout(p_ctx, 0u, &dl);
        if(rc < 0)
        {
            return rc;
        }
        timed = (rc > 0) ? 1u : 0u;
        if((timed != 0u) && ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
        {
            return -LX_ETIMEDOUT;
        }
        if(lx_futex_sig_pending() != 0u)
        {
            return -LX_EINTR;
        }
        if(arkrtos_port_svc_restart(p_ctx) != 0)
        {
            return -LX_ENOSYS;
        }
        crit = ARKRTOS_ENTER_CRITICAL();
        s_futex_key[me - 1]      = key;
        s_futex_used[me - 1]     = 1u;
        s_futex_woke[me - 1]     = 0u;
        s_futex_uaddr[me - 1]    = uaddr;
        s_futex_val[me - 1]      = val;
        s_futex_bitset[me - 1]   = (uint32)LX_FUTEX_BITSET_ALL;
        s_futex_deadline[me - 1] = dl;
        s_futex_timed[me - 1]    = timed;
        s_futex_pi[me - 1]       = 1u;
        s_futex_rqpi[me - 1]     = 1u;
        s_futex_pi_owner[me - 1] = (int32)tid2;
        ARKRTOS_EXIT_CRITICAL(crit);
        lx_pi_recalc_owner((int32)tid2);
        (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
        return 0;
    }

    /* ---- 重放 ---- */
    if(woke != 0u)
    {
        /* CMP_REQUEUE_PI 已经把 uaddr2 的字写成我的 tid：所有权到手，
         * 不再读字（调用方可能已经改成 0 或改了别的位）。 */
        lx_futex_forget(me);
        return 0;
    }
    if((timed != 0u) && ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
    {
        lx_futex_forget(me);
        return -LX_ETIMEDOUT;
    }
    if(lx_futex_sig_pending() != 0u)
    {
        lx_futex_forget(me);
        return -LX_EINTR;
    }
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        lx_futex_forget(me);
        return -LX_ENOSYS;
    }
    /* 没被搬走也没超时：继续睡一个时间片（值有没有变不该在这里判 ——
     * Linux 的 requeue 等待者只在被搬走时才醒来）。 */
    (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
    return 0;
}

/* CMP_REQUEUE_PI(uaddr, nr_wake, uaddr2, nr_requeue, val3)：
 *   1) *uaddr != val3 ⇒ -EAGAIN（Linux 同一判据）；
 *   2) uaddr2 没人持有 ⇒ -EINVAL；
 *   3) 先按 nr_wake 叫醒 uaddr 上的普通等待者（与 Linux 顺序一致：先 wake
 *      再搬，不然被叫醒的那个会抢先把 uaddr2 改掉）；
 *   4) 剩下 WAIT_REQUEUE_PI 的等待者里挑基准优先级最高的当排头：把 uaddr2
 *      的字写成它的 tid（队里还有人等就保留 WAITERS，OWNER_DIED 原样带过
 *      去），叫醒它；其余改挂到 uaddr2 上，持有者记成新排头。
 * 返回值是**唤醒数加搬运数**（man futex 的 "woken up or requeued"）：
 * nr_wake=0 时也能从返回值看出有没有搬成功。 */
static int32 lx_futex_cmp_requeue_pi(void *p_ctx, int32 me)
{
    unsigned long uaddr   = ARKRTOS_SVC_ARG(p_ctx, 0u);
    int32  want  = (int32)(uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);   /* nr_wake */
    int32  nreq  = (int32)(uint32)ARKRTOS_SVC_ARG(p_ctx, 3u);   /* nr_requeue */
    unsigned long uaddr2  = ARKRTOS_SVC_ARG(p_ctx, 4u);
    uint32 expected = (uint32)ARKRTOS_SVC_ARG(p_ctx, 5u);
    unsigned long key;
    unsigned long key2;
    uint32 word  = 0u;
    uint32 word2 = 0u;
    uint32 nxt;
    uint32 tid2;
    uint32 crit;
    int32  woke = 0;
    int32  req  = 0;
    int32  top  = -1;
    int32  i;

    (void)me;
    if((want < 0) || (nreq < 0) || (uaddr2 == 0ul))
    {
        return -LX_EINVAL;
    }
    key = lx_futex_key(uaddr);
    if(key == ~0UL)
    {
        return -LX_ENOSYS;
    }
    key2 = lx_futex_key(uaddr2);
    if(key2 == ~0UL)
    {
        return -LX_ENOSYS;
    }
    if(lx_futex_get_word(uaddr, &word) != 0)
    {
        return -LX_EFAULT;
    }
    if(word != expected)
    {
        return -LX_EAGAIN;
    }
    if(lx_futex_get_word(uaddr2, &word2) != 0)
    {
        return -LX_EFAULT;
    }
    tid2 = word2 & (uint32)LX_FUTEX_TID_MASK;
    if(tid2 == 0u)
    {
        return -LX_EINVAL;      /* 目标锁没人持有，没有可接手的位置 */
    }

    /* 1) 普通等待者：只叫醒 nr_wake 个，一个都不搬。
     * want >= 0 与 mask != 0 在上面已经判过，这里不会回 -EINVAL。 */
    woke = lx_futex_wake_key(key, want, (uint32)LX_FUTEX_BITSET_ALL);

    /* 2) 排头：基准优先级最高的 rqpi 等待者（同级取任务号小的）。 */
    crit = ARKRTOS_ENTER_CRITICAL();
    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if((s_futex_used[i - 1] == 0u) || (s_futex_rqpi[i - 1] == 0u) ||
           (s_futex_key[i - 1] != key))
        {
            continue;
        }
        if((top < 0) ||
           (arkrtos_task_table[(uint32)i - 1u].base_priority <
            arkrtos_task_table[(uint32)top - 1u].base_priority))
        {
            top = i;
        }
    }
    if(top < 0)
    {
        ARKRTOS_EXIT_CRITICAL(crit);
        return woke;            /* 没有 PI 条件变量等待者：只叫醒了普通人 */
    }
    nxt = (uint32)top | (word2 & (uint32)LX_FUTEX_OWNER_DIED);
    /* 后面还有人（且本次允许搬）就留 WAITERS 位：新持有者的 UNLOCK_PI 靠它
     * 知道还要再交一次。没被搬走的那些仍在 uaddr 上等，不该让锁字替它们
     * 记账 —— 否则下一次 UNLOCK_PI 会去交一把空队列。 */
    if(nreq > 0)
    {
        for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
        {
            if((s_futex_used[i - 1] != 0u) && (s_futex_rqpi[i - 1] != 0u) &&
               (s_futex_key[i - 1] == key) && (i != top))
            {
                nxt |= (uint32)LX_FUTEX_WAITERS;
                break;
            }
        }
    }
    s_futex_woke[top - 1] = 1u;
    /* 其余改挂：键与地址搬到 uaddr2，期望值记成搬运后 uaddr2 上的字，持有
     * 者记成新排头（优先级继承要算在真正攥锁的那个人头上）。 */
    for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        if((i == top) || (s_futex_used[i - 1] == 0u) ||
           (s_futex_rqpi[i - 1] == 0u) || (s_futex_key[i - 1] != key))
        {
            continue;
        }
        if(req >= nreq)
        {
            break;
        }
        s_futex_key[i - 1]       = key2;
        s_futex_uaddr[i - 1]     = uaddr2;
        s_futex_val[i - 1]       = nxt;
        s_futex_pi_owner[i - 1]  = top;
        req++;
    }
    ARKRTOS_EXIT_CRITICAL(crit);

    if(lx_futex_put_word(uaddr2, nxt) != 0)
    {
        /* 字写不进去（用户把地址写坏了）：把叫醒撤销，别让排头拿着一个
         * 没落到内存里的「所有权」跑起来。搬过的那几个也已经按 nxt 记了
         * 期望值，一并如实报错。 */
        crit = ARKRTOS_ENTER_CRITICAL();
        s_futex_woke[top - 1] = 0u;
        ARKRTOS_EXIT_CRITICAL(crit);
        return -LX_EFAULT;
    }
    (void)arkrtos_task_wakeup_internal(top);
    lx_pi_recalc_owner((int32)tid2);    /* 原持有者头上的继承撤销 */
    lx_pi_recalc_owner(top);            /* 新持有者按队列重算 */
    return woke + req + 1;              /* 唤醒数 + 搬运数（含排头） */
}

/* PI 类 op 的总入口。与 Linux 的 rtmutex 一致的部分：
 *   - 字布局：低 30 位持有者 tid、bit31 有等待者、bit30 原持有者已死；
 *   - 自己已经持有再加锁 ⇒ -EDEADLK（真死锁，不假装成功）；
 *   - 拿不到时置 bit31 再登记等待：持有者的 UNLOCK_PI 靠它知道要交接；
 *   - 优先级继承：持有者的有效优先级抬到等待者里最高的那个，交接 / 超时 /
 *     被杀之后按 base_priority 算回去（与内核互斥锁同一口径）；
 *   - 原持有者已死时接手者拿到的字里保留 bit30，用户态据此报 EOWNERDEAD。
 * 与 Linux 不同的边界（文档 §8.4 逐条写死）：
 *   - LOCK_PI 的 timeout 按**相对**时长解释（与 WAIT 同口径），不是 Linux 的
 *     绝对时刻；到期回 -ETIMEDOUT 并摘掉登记、把持有者优先级算回去；
 *   - WAIT_REQUEUE_PI(11) / CMP_REQUEUE_PI(12) 有独立实现（见上）：这里只管
 *     LOCK_PI / UNLOCK_PI / TRYLOCK_PI 三档。
 */
static int32 lx_futex_pi(void *p_ctx, uint32 op, unsigned long uaddr,
                         uint32 val, int32 me)
{
    unsigned long key = 0ul;
    uint32 word = 0u;
    uint32 tid;
    uint8  replay;
    uint8  woke  = 0u;
    uint8  timed = 0u;
    uint32 dl    = 0u;
    int32  owner0 = 0;
    uint32 crit;
    int32  rc;

    (void)val;      /* LOCK_PI / TRYLOCK_PI 不看期望值（Linux 也不看） */

    /* 重放识别与 WAIT 同一套判据：自己那一槽还在用，就是上一次的续；此时
     * a0 已被返回值清成 0，只有登记下来的地址与键可信。 */
    crit = ARKRTOS_ENTER_CRITICAL();
    replay = ((s_futex_used[me - 1] != 0u) && (s_futex_pi[me - 1] != 0u)) ? 1u : 0u;
    if(replay != 0u)
    {
        key    = s_futex_key[me - 1];
        uaddr  = s_futex_uaddr[me - 1];
        woke   = s_futex_woke[me - 1];
        timed  = s_futex_timed[me - 1];
        dl     = s_futex_deadline[me - 1];
        owner0 = s_futex_pi_owner[me - 1];
    }
    ARKRTOS_EXIT_CRITICAL(crit);

    if(replay == 0u)
    {
        key = lx_futex_key(uaddr);
        if(key == ~0UL)
        {
            return -LX_ENOSYS;
        }
        rc = lx_futex_get_word(uaddr, &word);
        if(rc != 0)
        {
            return rc;
        }
        tid = word & (uint32)LX_FUTEX_TID_MASK;

        if(op == (uint32)LX_FUTEX_UNLOCK_PI)
        {
            return lx_futex_pi_unlock(key, uaddr, word, me);
        }
        if(tid == (uint32)me)
        {
            /* 自己已经持有（TRYLOCK 在 Linux 里也回 -EDEADLK，不是 -EAGAIN）。 */
            return -LX_EDEADLK;
        }
        if((tid == 0u) || ((word & (uint32)LX_FUTEX_OWNER_DIED) != 0u))
        {
            /* 空锁、或者持有者已死（bit30 只会在持有者死掉时被置上，活着的
             * 持有者不可能带着它）：直接接手，OWNER_DIED 原样保留 —— 用户态
             * 据此报 EOWNERDEAD，再调 pthread_mutex_consistent 把它清掉。 */
            return lx_futex_put_word(uaddr,
                                     (word & (uint32)LX_FUTEX_OWNER_DIED) |
                                     (uint32)me);
        }
        if(op == (uint32)LX_FUTEX_TRYLOCK_PI)
        {
            /* 拿不到。Linux 也顺手把 WAITERS 置上：确实有人试过。 */
            if((word & (uint32)LX_FUTEX_WAITERS) == 0u)
            {
                (void)lx_futex_put_word(uaddr, word | (uint32)LX_FUTEX_WAITERS);
            }
            return -LX_EAGAIN;
        }

        /* LOCK_PI：置 WAITERS → 登记 → 抬持有者优先级 → 睡 */
        if((word & (uint32)LX_FUTEX_WAITERS) == 0u)
        {
            rc = lx_futex_put_word(uaddr, word | (uint32)LX_FUTEX_WAITERS);
            if(rc != 0)
            {
                return rc;
            }
        }
        rc = lx_futex_timeout(p_ctx, 0u, &dl);
        if(rc < 0)
        {
            return rc;
        }
        timed = (rc > 0) ? 1u : 0u;
        if((timed != 0u) && ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
        {
            return -LX_ETIMEDOUT;       /* 已经到期：不白睡一个时间片 */
        }
        if(lx_futex_sig_pending() != 0u)
        {
            return -LX_EINTR;
        }
        if(arkrtos_port_svc_restart(p_ctx) != 0)
        {
            return -LX_ENOSYS;
        }
        crit = ARKRTOS_ENTER_CRITICAL();
        s_futex_key[me - 1]      = key;
        s_futex_used[me - 1]     = 1u;
        s_futex_woke[me - 1]     = 0u;
        s_futex_uaddr[me - 1]    = uaddr;
        s_futex_val[me - 1]      = word;
        s_futex_bitset[me - 1]   = (uint32)LX_FUTEX_BITSET_ALL;
        s_futex_deadline[me - 1] = dl;
        s_futex_timed[me - 1]    = timed;
        s_futex_pi[me - 1]       = 1u;
        s_futex_pi_owner[me - 1] = (int32)tid;
        ARKRTOS_EXIT_CRITICAL(crit);
        lx_pi_recalc_owner((int32)tid);
        (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
        return 0;
    }

    /* ---- 重放 ---- */
    if(woke != 0u)
    {
        /* 交接：持有者在 UNLOCK_PI（或 robust 清理）里已经把字写成我的 tid，
         * 这里只是确认所有权。不再读字 —— 用户可能已经改成 0 或改了别的位。 */
        lx_futex_forget(me);
        return 0;
    }
    if((timed != 0u) && ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
    {
        lx_futex_forget(me);
        return -LX_ETIMEDOUT;
    }
    rc = lx_futex_get_word(uaddr, &word);
    if(rc != 0)
    {
        lx_futex_forget(me);
        return rc;
    }
    tid = word & (uint32)LX_FUTEX_TID_MASK;
    if((tid == 0u) || (tid == (uint32)me) ||
       (((word & (uint32)LX_FUTEX_OWNER_DIED) != 0u) &&
        (tid == (uint32)owner0)))
    {
        /* 拿得到（或原持有者已死，接手）：写 tid，OWNER_DIED 保留。 */
        if(tid != (uint32)me)
        {
            rc = lx_futex_put_word(uaddr,
                                   (word & (uint32)LX_FUTEX_OWNER_DIED) |
                                   (uint32)me);
            if(rc != 0)
            {
                lx_futex_forget(me);
                return rc;
            }
        }
        lx_futex_forget(me);
        return 0;
    }
    if(lx_futex_sig_pending() != 0u)
    {
        lx_futex_forget(me);
        return -LX_EINTR;
    }
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        lx_futex_forget(me);
        return -LX_ENOSYS;
    }
    /* 继续睡。持有者可能换了人（交接后的重排 / 上一个持有者死掉），把登记里
     * 的持有者改成当前字里的那一个，两边的优先级都要重算。 */
    if((int32)tid != owner0)
    {
        crit = ARKRTOS_ENTER_CRITICAL();
        s_futex_pi_owner[me - 1] = (int32)tid;
        ARKRTOS_EXIT_CRITICAL(crit);
        lx_pi_recalc_owner(owner0);
        lx_pi_recalc_owner((int32)tid);
    }
    (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
    return 0;
}

static int32 lx_futex(void *p_ctx)
{
    unsigned long uaddr = ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32 op_full = (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 val     = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32  me      = (int32)arkrtos_current_task_id;
    uint32 op      = op_full & ~(uint32)(LX_FUTEX_PRIVATE_FLAG |
                                         LX_FUTEX_CLOCK_REALTIME);
    uint32 rt      = (op_full & (uint32)LX_FUTEX_CLOCK_REALTIME) != 0u ? 1u : 0u;
    unsigned long key;
    uint32 word = 0u;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EFAULT;
    }

    /* CLOCK_REALTIME 只有 WAIT_BITSET 用得到；本机没有墙钟，带了 timeout
     * 就如实 -ENOSYS。没带 timeout 时这面旗没有作用对象，直接忽略。 */
    if(rt != 0u)
    {
        if(op != (uint32)LX_FUTEX_WAIT_BITSET)
        {
            return -LX_ENOSYS;
        }
        if(ARKRTOS_SVC_ARG(p_ctx, 3u) != 0ul)
        {
            return -LX_ENOSYS;
        }
    }

    /* PI 条件变量的两档：先于通用路径，因为它们要看两个地址、而且
     * WAIT_REQUEUE_PI 重放时 a0 已被返回值清成 0（不能拿它算键）。 */
    if(op == (uint32)LX_FUTEX_WAIT_REQUEUE_PI)
    {
        return lx_futex_wait_requeue_pi(p_ctx, uaddr, val, me);
    }
    if(op == (uint32)LX_FUTEX_CMP_REQUEUE_PI)
    {
        return lx_futex_cmp_requeue_pi(p_ctx, me);
    }

    /* PI 类 op 走自己的路径：它们先看字里的持有者，而不是先算键。 */
    if((op == (uint32)LX_FUTEX_LOCK_PI) || (op == (uint32)LX_FUTEX_TRYLOCK_PI) ||
       (op == (uint32)LX_FUTEX_UNLOCK_PI))
    {
        return lx_futex_pi(p_ctx, op, uaddr, val, me);
    }

    key = lx_futex_key(uaddr);
    if((key == ~0UL) && (uaddr == 0ul) &&
       ((op == (uint32)LX_FUTEX_WAIT) || (op == (uint32)LX_FUTEX_WAIT_BITSET)))
    {
        /* 重放的那一次读到的 uaddr 是 0 —— 不是调用方写了 0，而是返回值
         * 寄存器就是第一个参数寄存器：阻塞路径返回 0 时，分派器把 0 写进
         * a0。这里不能靠地址认重放，只能靠「我这一槽还登记着」。一个任务
         * 同时只可能有一次在飞的 WAIT（它睡着的时候不可能再发起一次），
         * 所以 uaddr==0 且本槽在用就只能是它自己的重放。
         * 这不是理论洁癖：wait4 的重放同样把 pid 读成 0，只是 0 对 wait4
         * 本来就是合法输入（等任意儿子），所以此前没有暴露。 */
        uint32 crit0 = ARKRTOS_ENTER_CRITICAL();
        uint8  used0 = s_futex_used[me - 1];

        if(used0 != 0u)
        {
            key   = s_futex_key[me - 1];
            uaddr = s_futex_uaddr[me - 1];
            val   = s_futex_val[me - 1];
        }
        ARKRTOS_EXIT_CRITICAL(crit0);
    }
    if(key == ~0UL)
    {
        return -LX_ENOSYS;
    }

    /* ---- WAKE / WAKE_BITSET ---- */
    if((op == (uint32)LX_FUTEX_WAKE) || (op == (uint32)LX_FUTEX_WAKE_BITSET))
    {
        int32  want = (int32)val;
        uint32 mask = (op == (uint32)LX_FUTEX_WAKE_BITSET) ?
                      (uint32)ARKRTOS_SVC_ARG(p_ctx, 5u) :
                      (uint32)LX_FUTEX_BITSET_ALL;

        if((op == (uint32)LX_FUTEX_WAKE_BITSET) && (mask == 0u))
        {
            return -LX_EINVAL;
        }
        return lx_futex_wake_key(key, want, mask);
    }

    /* ---- REQUEUE / CMP_REQUEUE ---- */
    if((op == (uint32)LX_FUTEX_REQUEUE) || (op == (uint32)LX_FUTEX_CMP_REQUEUE))
    {
        unsigned long uaddr2 = ARKRTOS_SVC_ARG(p_ctx, 4u);
        uint32 val2 = (uint32)ARKRTOS_SVC_ARG(p_ctx, 3u);   /* nr_requeue 复用这一格 */
        int32  want = (int32)val;
        int32  nreq = (int32)val2;
        unsigned long key2;
        uint32 word2 = 0u;
        int32  woke = 0;
        int32  req  = 0;
        int32  i;
        uint32 crit;

        if((want < 0) || (nreq < 0) || (uaddr2 == 0ul))
        {
            return -LX_EINVAL;
        }
        key2 = lx_futex_key(uaddr2);
        if(key2 == ~0UL)
        {
            return -LX_ENOSYS;
        }
        if(op == (uint32)LX_FUTEX_CMP_REQUEUE)
        {
            uint32 expected = (uint32)ARKRTOS_SVC_ARG(p_ctx, 5u);

            if(arkrtos_copy_from_user(&word, (const void *)uaddr, 4u) != 0)
            {
                return -LX_EFAULT;
            }
            if(word != expected)
            {
                return -LX_EAGAIN;
            }
        }
        /* 被 requeue 的等待者把期望值改记成新地址上的当前字：它们本来就
         * 不该被这次调用唤醒（Linux 里 requeue 不叫醒人）。 */
        if(arkrtos_copy_from_user(&word2, (const void *)uaddr2, 4u) != 0)
        {
            return -LX_EFAULT;
        }

        crit = ARKRTOS_ENTER_CRITICAL();
        for(i = 1; (i <= (int32)ARKRTOS_TASK_MAX_NUM) &&
                   ((woke < want) || (req < nreq)); i++)
        {
            if((s_futex_used[i - 1] != 0u) && (s_futex_pi[i - 1] == 0u) &&
               (s_futex_key[i - 1] == key))
            {
                if(woke < want)
                {
                    s_futex_woke[i - 1] = 1u;
                    (void)arkrtos_task_wakeup_internal(i);
                    woke++;
                }
                else
                {
                    s_futex_key[i - 1]   = key2;
                    s_futex_uaddr[i - 1] = uaddr2;
                    s_futex_val[i - 1]   = word2;
                    req++;
                }
            }
        }
        ARKRTOS_EXIT_CRITICAL(crit);

        /* 唤醒数 + 改挂数（不是只回唤醒数）：nr_wake=0 的 CMP_REQUEUE
         * 也必须能从返回值看出「有没有挂过去」。 */
        return (int32)(woke + req);
    }

    if((op != (uint32)LX_FUTEX_WAIT) && (op != (uint32)LX_FUTEX_WAIT_BITSET))
    {
        return -LX_ENOSYS;
    }

    /* ---- WAIT / WAIT_BITSET：先判这是不是上一次睡下那一次的重放 ---- */
    {
        uint32        crit    = ARKRTOS_ENTER_CRITICAL();
        uint8         resumed = (s_futex_used[me - 1] != 0u) ? 1u : 0u;
        uint8         woke    = s_futex_woke[me - 1];
        uint8         timed   = s_futex_timed[me - 1];
        uint32        dl      = s_futex_deadline[me - 1];
        unsigned long saddr   = s_futex_uaddr[me - 1];
        uint32        sval    = s_futex_val[me - 1];

        ARKRTOS_EXIT_CRITICAL(crit);

        /* 「我是不是那次重放」只认自己那一槽，不认地址：REQUEUE 会把这一
         * 槽的键改到 uaddr2 上，而重放的那一次读到的 uaddr 还是用户原样
         * 写下的旧地址——早先按「槽里的键 == 这次算出的键」认重放，被改挂
         * 过的等待者醒来后认不出自己，会在旧地址上重新睡下，等不到新地址
         * 上的 WAKE（fork 夹具 futex2 requeue 一档就是这么挂死的）。一个
         * 任务睡着的时候不可能再发起一次 WAIT，所以「槽在用」就等于「这次
         * 是重放」；要比对的值也取槽里那一份——改挂时它已经更新成 uaddr2
         * 上的当前字，正是期望值该有的口径。 */
        if(resumed != 0u)
        {
            if(woke != 0u)
            {
                lx_futex_forget(me);
                return 0;           /* 真的被 WAKE 叫醒 */
            }
            if((timed != 0u) &&
               ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
            {
                lx_futex_forget(me);
                return -LX_ETIMEDOUT;
            }
            /* 时间片到了（本内核的兜底唤醒）：值变了就当被叫醒，交给调用方
             * 重新判断；值没变就继续睡一个时间片。 */
            if(arkrtos_copy_from_user(&word, (const void *)saddr, 4u) != 0)
            {
                lx_futex_forget(me);
                return -LX_EFAULT;
            }
            if(word != sval)
            {
                lx_futex_forget(me);
                return 0;
            }
            if(lx_futex_sig_pending() != 0u)
            {
                lx_futex_forget(me);
                return -LX_EINTR;
            }
            if(arkrtos_port_svc_restart(p_ctx) != 0)
            {
                lx_futex_forget(me);
                return -LX_ENOSYS;
            }
            (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
            return 0;
        }
    }

    /* ---- WAIT / WAIT_BITSET：第一次进入 ---- */
    {
        uint32 bits = (op == (uint32)LX_FUTEX_WAIT_BITSET) ?
                      (uint32)ARKRTOS_SVC_ARG(p_ctx, 5u) :
                      (uint32)LX_FUTEX_BITSET_ALL;
        uint32 dl   = 0u;
        uint8  timed = 0u;
        int32  trc;

        if((op == (uint32)LX_FUTEX_WAIT_BITSET) && (bits == 0u))
        {
            return -LX_EINVAL;
        }
        trc = lx_futex_timeout(p_ctx,
                               (op == (uint32)LX_FUTEX_WAIT_BITSET) ? 1u : 0u,
                               &dl);
        if(trc < 0)
        {
            return trc;
        }
        if(trc > 0)
        {
            timed = 1u;
        }

        if(arkrtos_copy_from_user(&word, (const void *)uaddr, 4u) != 0)
        {
            return -LX_EFAULT;
        }
        if(word != val)
        {
            return -LX_EAGAIN;
        }
        /* 值相等但已经过了截止时刻：直接 -ETIMEDOUT，不白睡一个时间片。 */
        if((timed != 0u) &&
           ((int32)(arkrtos_kernel_get_time() - dl) >= 0))
        {
            return -LX_ETIMEDOUT;
        }
        if(lx_futex_sig_pending() != 0u)
        {
            return -LX_EINTR;
        }
        if(arkrtos_port_svc_restart(p_ctx) != 0)
        {
            return -LX_ENOSYS;
        }
        {
            uint32 crit = ARKRTOS_ENTER_CRITICAL();

            s_futex_key[me - 1]      = key;
            s_futex_used[me - 1]     = 1u;
            s_futex_woke[me - 1]     = 0u;
            s_futex_uaddr[me - 1]    = uaddr;
            s_futex_val[me - 1]      = val;
            s_futex_bitset[me - 1]   = bits;
            s_futex_deadline[me - 1] = dl;
            s_futex_timed[me - 1]    = timed;
            ARKRTOS_EXIT_CRITICAL(crit);
        }
        (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
        return 0;
    }
}

/* --------------------------- robust list（99 / 100 + 退出清理） */

/* 把一条 robust 链上的字标成「持有者已死」，有人等时唤醒一个：
 *   - 非 PI：tid 清掉、置 bit30（与 Linux 同口径），bit31 原样留着；
 *   - PI：tid **保留**（等待者要靠它认出「上一位持有者死了」），只置 bit30，
 *     再唤醒队首；交接动作由被唤醒者的重放路径完成 —— 它在那里能看到
 *     tid == 自己的 pi_owner 且 bit30 置位，于是接手。
 * 字里的 tid 已经不是它、或地址读不出来就跳过：退出路径没有返回值可交，
 * 硬改别人的字比漏一条更糟。 */
static int32 lx_robust_mark(unsigned long uaddr, int32 task_id, uint8 pi)
{
    uint32        word = 0u;
    unsigned long key;
    int32         rc;

    rc = lx_futex_get_word(uaddr, &word);
    if(rc != 0)
    {
        return rc;
    }
    if((word & (uint32)LX_FUTEX_TID_MASK) != (uint32)task_id)
    {
        return 0;       /* 用户态自己解过锁了，不动 */
    }
    key = lx_futex_key_task(task_id, uaddr);
    if(key == ~0UL)
    {
        return -LX_ENOSYS;
    }
    if(pi != 0u)
    {
        uint32 crit;
        int32  top;
        int32  i;
        uint32 nxt;

        /* 交接与 UNLOCK_PI 同一套：字里直接写成新持有者的 tid，被唤醒者的
         * 重放路径拿到的就是「已经交接」的确认。这里的交接比 UNLOCK_PI 更
         * 要紧 —— 死掉的持有者不会再有机会替我们写字。 */
        crit = ARKRTOS_ENTER_CRITICAL();
        top  = lx_pi_top_waiter(key);
        if(top > 0)
        {
            for(i = 1; i <= (int32)ARKRTOS_TASK_MAX_NUM; i++)
            {
                if((i == top) || (s_futex_used[i - 1] == 0u) ||
                   (s_futex_pi[i - 1] == 0u) || (s_futex_key[i - 1] != key))
                {
                    continue;
                }
                s_futex_pi_owner[i - 1] = top;
            }
            s_futex_pi_owner[top - 1] = top;
            s_futex_woke[top - 1]     = 1u;
        }
        ARKRTOS_EXIT_CRITICAL(crit);

        nxt = ((top > 0) ? (uint32)top : (word & (uint32)LX_FUTEX_TID_MASK)) |
              (uint32)LX_FUTEX_OWNER_DIED;
        rc = lx_futex_put_word(uaddr, nxt);
        if(rc != 0)
        {
            return rc;
        }
        if(top > 0)
        {
            (void)arkrtos_task_wakeup_internal(top);
            lx_pi_recalc_owner((int32)(word & (uint32)LX_FUTEX_TID_MASK));
        }
        return 0;
    }
    rc = lx_futex_put_word(uaddr, (word & (uint32)LX_FUTEX_WAITERS) |
                                  (uint32)LX_FUTEX_OWNER_DIED);
    if(rc != 0)
    {
        return rc;
    }
    if((word & (uint32)LX_FUTEX_WAITERS) != 0u)
    {
        (void)lx_futex_wake_key(key, 1, (uint32)LX_FUTEX_BITSET_ALL);
    }
    return 0;
}

/* 任务退出：走一遍它登记过的 robust 链。
 * 布局与 Linux 逐字相同：robust_list_head { next, futex_offset, pending }，
 * 每个 next 指针的最低位是「这条是 PI futex」的记号（FUTEX_PI_LIST）。
 * 只处理「字里的 tid 还是它」的条目。环按 LX_ROBUST_LIST_LIMIT 截断：
 * 链表被用户写坏（自环）时，退出路径也得能走完 —— 死在退出钩子里就是整个
 * 系统停摆，比漏清一条严重得多。 */
static void lx_robust_exit(int32 task_id)
{
    unsigned long head;
    uint32        len;
    unsigned long hdr[3];
    unsigned long entry;
    unsigned long pending;
    long          offset;
    int32         n;
    uint32        crit;

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    head = s_robust_head[task_id - 1];
    len  = s_robust_len[task_id - 1];
    s_robust_head[task_id - 1] = 0ul;
    s_robust_len[task_id - 1]  = 0u;
    ARKRTOS_EXIT_CRITICAL(crit);

    if((head == 0ul) || (len < LX_ROBUST_HEAD_MIN))
    {
        return;         /* 没登记过：没有可清的东西 */
    }
    if(arkrtos_copy_from_user(hdr, (const void *)head, (uint32)sizeof(hdr)) != 0)
    {
        return;
    }
    entry   = hdr[0];
    offset  = (long)hdr[1];
    pending = hdr[2];

    for(n = 0; (n < LX_ROBUST_LIST_LIMIT) && (entry != 0ul) && (entry != head); n++)
    {
        unsigned long next = 0ul;
        unsigned long node = entry & ~1ul;
        uint8         pi   = (uint8)(entry & 1ul);

        if(arkrtos_copy_from_user(&next, (const void *)node,
                                  (uint32)sizeof(next)) != 0)
        {
            break;
        }
        (void)lx_robust_mark(node + (unsigned long)offset, task_id, pi);
        entry = next;
    }
    if(pending != 0ul)
    {
        /* list_op_pending：glibc 在「已经把自己从链上摘下、字还没写完」的
         * 窗口里登记这一格。漏它就是一次真的漏清理（Linux 也处理它）。 */
        (void)lx_robust_mark((pending & ~1ul) + (unsigned long)offset, task_id,
                             (uint8)(pending & 1ul));
    }
}

/* set_robust_list(99)：登记链头。长度装不下结构、指针没对齐、指针为空一律
 * -EINVAL（Linux 同）—— 不猜调用方想干什么。 */
static int32 lx_robust_set(unsigned long head, uint32 len)
{
    int32  me = (int32)arkrtos_current_task_id;
    uint32 crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EFAULT;
    }
    if((head == 0ul) || (len < LX_ROBUST_HEAD_MIN) ||
       ((head & (unsigned long)(sizeof(void *) - 1u)) != 0ul))
    {
        return -LX_EINVAL;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    s_robust_head[me - 1] = head;
    s_robust_len[me - 1]  = len;
    ARKRTOS_EXIT_CRITICAL(crit);
    return 0;
}

/* get_robust_list(100)：只认自己。本内核没有可查询的第二个进程身份，别的
 * pid 如实 -ESRCH，不编造一份「大概是这样」的数据。 */
static int32 lx_robust_get(void *p_ctx)
{
    int32         pid = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long pp  = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long pl  = ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32         me  = (int32)arkrtos_current_task_id;
    unsigned long head;
    uint32        len;
    uint32        crit;

    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -LX_EFAULT;
    }
    if((pid != 0) && (pid != me))
    {
        return -LX_ESRCH;
    }
    if((pp == 0ul) || (pl == 0ul))
    {
        return -LX_EFAULT;
    }
    crit = ARKRTOS_ENTER_CRITICAL();
    head = s_robust_head[me - 1];
    len  = s_robust_len[me - 1];
    ARKRTOS_EXIT_CRITICAL(crit);
    if(arkrtos_copy_to_user((void *)pp, &head, (uint32)sizeof(head)) != 0)
    {
        return -LX_EFAULT;
    }
    if(arkrtos_copy_to_user((void *)pl, &len, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    return 0;
}
#endif /* ARKRTOS_USE_LINUX_FUTEX == 1 */

#if (ARKRTOS_USE_LINUX_THREAD == 1)
/* 线程退出：把 CLONE_CHILD_CLEARTID 给的字清零，再唤醒一个等待者。
 * 顺序是「先清字、再唤醒」：pthread_join 醒来后要读到 0，倒过来会让它读到
 * 旧 tid 又睡回去 —— 那种错误只在真线程上出现，而且表现成偶发挂死。
 *
 * 清零不依赖 futex 档：没有 futex 也要把字清掉（程序可能只是轮询）。
 * 唤醒那半段才在 futex 档里。 */
static void lx_thread_clear_tid(int32 task_id)
{
    unsigned long addr;
    uint32 zero = 0u;

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    addr = s_lx_cleartid[task_id - 1];
    s_lx_cleartid[task_id - 1] = 0ul;
    if(addr == 0ul)
    {
        return;
    }

    /* 写的还是**这个任务自己的**地址空间：它已经不在就绪集里，但空间要到
     * 装载器下次复用时才被撤，所以这次写是安全的。写不进去也没有返回值
     * 可以交 —— 退出路径没有调用者。 */
    (void)arkrtos_copy_to_user_task(&arkrtos_task_table[task_id - 1], (void *)addr,
                                    &zero, 4u);
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    {
        unsigned long key = lx_futex_key_task(task_id, addr);

        if(key != ~0UL)
        {
            (void)lx_futex_wake_key(key, 1, (uint32)LX_FUTEX_BITSET_ALL);
        }
    }
#endif
}
#endif /* ARKRTOS_USE_LINUX_THREAD == 1 */

#if (ARKRTOS_USE_LINUX_SELECT == 1)
/* ------------------------------- pselect6（U-22，就绪等待） */

/* fd 有没有落地对象。0/1/2 当作打开（stdin 读到 EOF、stdout/stderr 可写），
 * 3..10 查本进程 fd 表；其余一律没有。 */
static uint8 lx_sel_open(int32 fd)
{
    /* fd 由 32 位 nfds 逐位推出，不会为负，不在这里留一条走不到的拒绝。 */
    if(fd <= 2)
    {
        return 1u;
    }
    return (lx_fd_find(fd) >= 0) ? 1u : 0u;
}

/* 读就绪。fd 0 永远可读（读到 EOF），普通文件永远就绪（同步读写），管道
 * 读端有数据或写端全关才就绪。写端 fd / 未打开 fd 不可读。 */
static uint8 lx_sel_rd_ready(int32 fd)
{
    int32 idx;

    if(fd <= 2)
    {
        /* 标准流别名要按它落地的那一格判；没有别名时只有 stdin 可读
         * （读到 EOF），stdout/stderr 不可读。 */
        idx = lx_std_slot(fd);
        if(idx < 0)
        {
            return (fd == 0) ? 1u : 0u;
        }
    }
    else
    {
        idx = lx_fd_find(fd);
        if(idx < 0)
        {
            return 0u;
        }
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        /* 套接字的就绪由 net 服务答（一次 POLL）。服务还没答复就按
         * 未就绪处理：谓词不能阻塞，外圈那一轮的时间片会再来问。 */
        return lx_sock_net_ready((uint32)idx, (uint32)ARKRTOS_NET_POLL_IN);
    }
#endif
    if(s_fd[idx].kind == LX_FD_FILE)
    {
        return 1u;
    }
    if(s_fd[idx].kind == LX_FD_CONSOLE)
    {
        return 1u;          /* 读回 EOF：按「可读」报，select 的语义如此 */
    }
    if(s_fd[idx].kind == LX_FD_PIPE_R)
    {
        int32 pi = (int32)s_fd[idx].pipe;

        if((pi >= 0) && (pi < (int32)LX_PIPE_MAX) && (s_pipe[pi].used != 0u))
        {
            if((s_pipe[pi].count > 0u) || (s_pipe[pi].writers == 0u))
            {
                return 1u;
            }
        }
    }
    return 0u;
}

/* 写就绪。fd 1/2 永远可写（控制台），普通文件永远就绪，管道写端有空间或
 * 读端全关（写回 -EPIPE）才就绪。读端 fd / 未打开 fd 不可写。 */
static uint8 lx_sel_wr_ready(int32 fd)
{
    int32 idx;

    if(fd <= 2)
    {
        idx = lx_std_slot(fd);
        if(idx < 0)
        {
            return ((fd == 1) || (fd == 2)) ? 1u : 0u;
        }
    }
    else
    {
        idx = lx_fd_find(fd);
        if(idx < 0)
        {
            return 0u;
        }
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    if(s_fd[idx].kind == LX_FD_SOCKET)
    {
        return lx_sock_net_ready((uint32)idx, (uint32)ARKRTOS_NET_POLL_OUT);
    }
#endif
    if(s_fd[idx].kind == LX_FD_FILE)
    {
        return 1u;
    }
    if(s_fd[idx].kind == LX_FD_CONSOLE)
    {
        return 1u;          /* 控制台永远可写 */
    }
    if(s_fd[idx].kind == LX_FD_PIPE_W)
    {
        int32 pi = (int32)s_fd[idx].pipe;

        if((pi >= 0) && (pi < (int32)LX_PIPE_MAX) && (s_pipe[pi].used != 0u))
        {
            if((s_pipe[pi].readers == 0u) ||
               (s_pipe[pi].count < (uint16)LX_PIPE_BUF))
            {
                return 1u;
            }
        }
    }
    return 0u;
}

/* 读用户 timespec，算出「从现在起多少毫秒」。pselect6 的 timeout 是相对时长
 * 且不被写回（这是 pselect 与 select 的区别）。返回负数 errno。 */
static int32 lx_sel_rel_ms(unsigned long ua, uint32 *p_ms)
{
    long ts[2];

    if(arkrtos_copy_from_user(ts, (const void *)ua, (uint32)sizeof(ts)) != 0)
    {
        return -LX_EFAULT;
    }
    if((ts[0] < 0) || (ts[1] < 0) || (ts[1] >= 1000000000L))
    {
        return -LX_EINVAL;
    }
    if(ts[0] > 2147483L)
    {
        *p_ms = 0x7FFFFFFFu;                    /* 约 24.8 天，够用且不溢出 */
    }
    else
    {
        *p_ms = ((uint32)ts[0] * 1000u) + (uint32)(ts[1] / 1000000L);
    }
    return 0;
}

/* 扫一遍 fd_set，统计就绪数；write_back != 0 时把用户那三张表改写成「只剩
 * 就绪位」。按 32 位字读写，不把整张 128 字节 fd_set 搬上栈。返回 0 或
 * -EBADF/-EFAULT；就绪计数写进 *p_cnt。except 集永不上报。 */
static int32 lx_sel_scan(uint32 nfds, unsigned long rptr, unsigned long wptr,
                         unsigned long eptr, uint32 *p_cnt, uint8 write_back)
{
    uint32 words = (nfds + 31u) / 32u;
    uint32 cnt = 0u;
    uint32 w;

    for(w = 0u; w < words; w++)
    {
        uint32 in_r = 0u;
        uint32 in_w = 0u;
        uint32 in_e = 0u;
        uint32 out_r = 0u;
        uint32 out_w = 0u;
        uint32 base = w * 32u;
        uint32 bit;
        unsigned long wr = (unsigned long)(w * 4u);

        if((rptr != 0ul) &&
           (arkrtos_copy_from_user(&in_r, (const void *)(rptr + wr), 4u) != 0))
        {
            return -LX_EFAULT;
        }
        if((wptr != 0ul) &&
           (arkrtos_copy_from_user(&in_w, (const void *)(wptr + wr), 4u) != 0))
        {
            return -LX_EFAULT;
        }
        if((eptr != 0ul) &&
           (arkrtos_copy_from_user(&in_e, (const void *)(eptr + wr), 4u) != 0))
        {
            return -LX_EFAULT;
        }

        for(bit = 0u; bit < 32u; bit++)
        {
            uint32 mask = (uint32)1u << bit;
            uint32 fd = base + bit;

            if(fd >= nfds)
            {
                break;
            }
            if(((in_r | in_w | in_e) & mask) == 0u)
            {
                continue;
            }
            if(lx_sel_open((int32)fd) == 0u)
            {
                return -LX_EBADF;
            }
            if((in_r & mask) != 0u)
            {
                if(lx_sel_rd_ready((int32)fd) != 0u)
                {
                    out_r |= mask;
                    cnt++;
                }
            }
            if((in_w & mask) != 0u)
            {
                if(lx_sel_wr_ready((int32)fd) != 0u)
                {
                    out_w |= mask;
                    cnt++;
                }
            }
            /* exceptfds：本层没有带外数据，永不置位。 */
        }

        /* 写回：这几个 4 字节刚刚被上面同一组指针 copy_from_user 读成功，
         * 中间没有让出 CPU，本层又没有别的执行流能改本任务的映射，所以写回
         * 不可能失败。返回值只看「写了几字节」，不编一条走不到的 EFAULT。 */
        if(write_back != 0u)
        {
            if(rptr != 0ul)
            {
                (void)arkrtos_copy_to_user((void *)(rptr + wr), &out_r, 4u);
            }
            if(wptr != 0ul)
            {
                (void)arkrtos_copy_to_user((void *)(wptr + wr), &out_w, 4u);
            }
            if(eptr != 0ul)
            {
                uint32 zero = 0u;

                (void)arkrtos_copy_to_user((void *)(eptr + wr), &zero, 4u);
            }
        }
    }

    *p_cnt = cnt;
    return 0;
}

/* 本任务若还挂着一次 pselect6 阻塞，把它的截止时刻清掉。任务号会被复用，
 * 不清的话新任务会继承一条「已经在等」的旧记录。 */
static void lx_sel_forget(int32 task_id)
{
    if((task_id > 0) && (task_id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        s_sel_armed[task_id - 1]    = 0u;
        s_sel_deadline[task_id - 1] = 0u;
    }
}

/**
* @brief pselect6(2)：对文件/管道描述符做就绪等待
* @details 与 Linux 一致的部分：readfds/writefds 轮询一次，就绪位写回用户
*          表、非就绪位清除，返回值是就绪描述符个数；timeout=0 是纯轮询，
*          timeout=NULL 是无限等待（本内核按时间片重放）；timeout 是相对时长
*          且不被写回。
*          与 Linux 不同的边界（都在文档里写死）：
*            - exceptfds 接受但永不上报（没有带外数据/异步错误队列）；
*            - 第六个参数（每次调用临时信号掩码）回 -ENOSYS 而不是忽略；
*            - 就绪判据按本层对象定义：管道读端有数据或写端全关、管道写端
*              有空间或读端全关、普通文件永远就绪、stdin 可读、stdout/stderr
*              可写；没有任何对象支撑的描述符被选中回 -EBADF；
*            - 没有可重放端口时阻塞路径回 -EAGAIN，不假装睡过。
*/
static int32 lx_pselect6(void *p_ctx)
{
    uint32 nfds = (uint32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long rptr = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long wptr = ARKRTOS_SVC_ARG(p_ctx, 2u);
    unsigned long eptr = ARKRTOS_SVC_ARG(p_ctx, 3u);
    unsigned long tptr = ARKRTOS_SVC_ARG(p_ctx, 4u);
    unsigned long sptr = ARKRTOS_SVC_ARG(p_ctx, 5u);
    int32  me = (int32)arkrtos_current_task_id;
    uint32 cnt = 0u;
    int32  rc;

    /* 第六个参数是「每次调用临时信号掩码」的指针；本层不支持，明确拒绝，
     * 不装作忽略。 */
    if(sptr != 0ul)
    {
        return -LX_ENOSYS;
    }
    if(nfds > (uint32)LX_SEL_SETSIZE)
    {
        return -LX_EINVAL;
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 套接字就绪是每轮现问的：跨轮留着上一轮的就绪位会把调用方骗起来。
     * 第一遍扫描填、第二遍（写回那遍）复用。 */
    {
        uint32 k;

        for(k = 0u; k < (uint32)LX_MAX_FD; k++)
        {
            s_fd[k].pok   = 0u;
            s_fd[k].pbits = 0u;
        }
    }
#endif

    rc = lx_sel_scan(nfds, rptr, wptr, eptr, &cnt, 0u);
    if(rc < 0)
    {
        lx_sel_forget(me);
        return rc;
    }

    if(cnt > 0u)
    {
        rc = lx_sel_scan(nfds, rptr, wptr, eptr, &cnt, 1u);
        lx_sel_forget(me);
        return (rc < 0) ? rc : (int32)cnt;
    }

    /* 没有就绪的。以下要么立刻超时返回 0，要么阻塞重放。 */
    if((me <= 0) || (me > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        /* 内核上下文没有可阻塞的身份：按纯轮询处理，回 0（并清空用户表）。 */
        (void)lx_sel_scan(nfds, rptr, wptr, eptr, &cnt, 1u);
        return 0;
    }

    if(tptr != 0ul)
    {
        if(s_sel_armed[me - 1] == 0u)
        {
            uint32 ms = 0u;

            rc = lx_sel_rel_ms(tptr, &ms);
            if(rc < 0)
            {
                lx_sel_forget(me);
                return rc;
            }
            if(ms == 0u)
            {
                /* select 的 timeout=0 是「纯轮询」：立即回 0，不睡。 */
                (void)lx_sel_scan(nfds, rptr, wptr, eptr, &cnt, 1u);
                lx_sel_forget(me);
                return 0;
            }
            s_sel_deadline[me - 1] = arkrtos_kernel_get_time() + ms;
            s_sel_armed[me - 1]    = 1u;
        }
        else if((int32)(arkrtos_kernel_get_time() - s_sel_deadline[me - 1]) >= 0)
        {
            /* 截止时刻已过：Linux 回 0 并清空就绪集。 */
            (void)lx_sel_scan(nfds, rptr, wptr, eptr, &cnt, 1u);
            lx_sel_forget(me);
            return 0;
        }
    }

    /* 需要睡：本内核没有在 SVC 内真睡的通路，和 futex/wait4 一样靠重放。
     * 没有可重放端口就如实回 -EAGAIN，不假装睡过。 */
    if(arkrtos_port_svc_restart(p_ctx) != 0)
    {
        lx_sel_forget(me);
        return -LX_EAGAIN;
    }
    (void)arkrtos_task_block_in_critical((uint32)LX_WAIT_SLICE_MS);
    /* 不写 a0：nfds 是这一轮的输入，被清成 0 会让下一次重放少扫
     * 一圈描述符。和 socket 一样走 RETRY 哨兵。 */
    return LX_RET_RETRY;
}
#endif /* ARKRTOS_USE_LINUX_SELECT == 1 */
/* ------------------------------------------------ execve（同任务换镜像） */

/* MinGW 把 __attribute__((weak)) 编成「同一目标内的弱别名」（符号表里
 * 是 w 未定义，COMDAT 组名带调用者），跨目标引用既不解析也不计数。
 * 所以覆盖档（coverage_gate 的 linuxabi 用例）用 -DARKRTOS_HOST_WEAK_PORT=0
 * 把 svc_restart 的缺省实现编成普通强符号：行为一字不差（回 -1），只是宿主
 * 看得见才数得到。exec_frame 那条不套这个宏：它的成功路径由用例的 --wrap
 * 强定义接管，改成强符号反而会顶掉 wrap。产物构建永远不带这个宏。 */
#if defined(ARKRTOS_HOST_WEAK_PORT) && (ARKRTOS_HOST_WEAK_PORT == 0)
#define ARKRTOS_PORT_FALLBACK_LINKAGE
#else
#define ARKRTOS_PORT_FALLBACK_LINKAGE __attribute__((weak))
#endif

/* 没有「同任务换镜像」落地能力的 port（RISC-V / Cortex-M / LA32）不提供
 * 这条符号：弱定义让那些构建照样链接，运行期 execve 明确回 -ENOSYS，
 * 而不是把一个可信的 0 交给程序。 */
__attribute__((weak)) int32
arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entry,
                            unsigned long sp, unsigned long tp)
{
    (void)p_exc_ctx;
    (void)entry;
    (void)sp;
    (void)tp;
    return -1;
}

/* 同上：没有「重放系统调用」能力的 port 明确回 -1，阻塞等待由 wait4 自己
 * 降级成 -EAGAIN。绝不返回 0 假装睡过 —— 那会让父进程在子进程还活着的时候
 * 拿着一个编出来的退出码继续跑。
 *
 * 宿主用例要逐段改这条端口能力（futex WAIT 的重放分支），而 --wrap 只对
 * 未定义引用生效、管不到同一个目标里的定义，所以覆盖档用
 * -DARKRTOS_HOST_NO_PORT_RESTART=1 把缺省实现整段去掉，由用例自己给。 */
#if !defined(ARKRTOS_HOST_NO_PORT_RESTART)
ARKRTOS_PORT_FALLBACK_LINKAGE int32 arkrtos_port_svc_restart(void *p_exc_ctx)
{
    (void)p_exc_ctx;
    return -1;
}
#endif

static int32 lx_exec_errno(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_ELF_ERR_FS:
        return -LX_ENOENT;
    case ARKRTOS_ELF_ERR_FORMAT:
    case ARKRTOS_ELF_ERR_ARCH:
    case ARKRTOS_ELF_ERR_LAYOUT:
        return -LX_ENOEXEC;
    case ARKRTOS_ELF_ERR_NOMEM:
        return -LX_ENOMEM;
    case ARKRTOS_ELF_ERR_BUSY:
        return -LX_EBUSY;
    case ARKRTOS_ELF_ERR_SIGN:
        return -LX_EACCES;
    case ARKRTOS_ELF_ERR_INTERP:
        return -LX_ENOENT;
    default:
        return -LX_EINVAL;
    }
}

/**
* @brief execve(path, argv, envp)：把**当前任务自己**的镜像换掉
* @details 与 Linux 一致的部分：任务号不变、fd 保留、成功不返回、失败
*          返回负 errno 且旧镜像继续跑。
*          与 Linux 不一致的部分（都在文档里写死）：
*            - envp 不消费：这台机器没有环境变量这一层，新镜像拿到的是
*              和刚起跑时同一张空表；
*            - 地址空间不复制（没有 fork），所以没有「父子」这回事；
*            - 旧镜像的堆游标作废（新镜像的 brk/mmap 从零开始），但**已分配
*              的地址不做任何回收声明**——旧镜像已经没了，没有谁还在用它们。
*          参数纪律：path 与 argv 都在用户内存里，且马上要被新镜像覆盖，
*          所以全部先拷进内核再装载。
*/
static int32 lx_execve(void *p_ctx)
{
    static char argv_buf[LX_EXEC_ARGV_BYTES];
    char  *argv[LX_EXEC_ARGV_MAX];
    char   path[LX_PATH_MAX];
    arkrtos_elf_info_t info;
    unsigned long tp = 0ul;
    unsigned long uargv;
    uint32 used = 0u;
    int32  argc = 0;
    int32  rc;

    rc = lx_copy_path(p_ctx, 0u, path, (uint32)sizeof(path));
    if(rc != 0)
    {
        return rc;
    }

    uargv = ARKRTOS_SVC_ARG(p_ctx, 1u);
    if(uargv != 0u)
    {
        for(;;)
        {
            unsigned long up = 0ul;

            if(argc >= (int32)LX_EXEC_ARGV_MAX)
            {
                return -LX_E2BIG;
            }
            if(arkrtos_copy_from_user(&up,
                    (const void *)(unsigned long)(uargv +
                        (unsigned long)(8u * (uint32)argc)), 8u) != 0)
            {
                return -LX_EFAULT;
            }
            if(up == 0ul)
            {
                break;
            }
            if((used + LX_EXEC_ARG_LEN) > LX_EXEC_ARGV_BYTES)
            {
                return -LX_E2BIG;
            }
            argv[argc] = &argv_buf[used];
            rc = lx_copy_user_str(up, &argv_buf[used], LX_EXEC_ARG_LEN);
            if(rc != 0)
            {
                return rc;
            }
            used += LX_EXEC_ARG_LEN;
            argc++;
        }
    }
    rc = arkrtos_elf_exec_replace(path, argc, argv, &info, &tp);
    if(rc != ARKRTOS_ELF_OK)
    {
        /* 校验阶段就拒绝（格式/架构/布局/签名）：窗口一个字节没动，
         * 旧镜像照常跑。这条路径上的失败是**可返回**的。 */
        return lx_exec_errno(rc);
    }

    /* 到这里旧镜像已经被覆盖：帧一改就再也回不去了。所以下面这一步
     * 不允许失败——port 只在「帧指针为空」或「入口不在窗口内」时拒绝，
     * 而入口已经在装载器里校验过（elf_check_entry / 解释器区间）。 */
    if(arkrtos_port_svc_exec_frame(p_ctx, info.entry, info.sp, tp) != 0)
    {
        return -LX_ENOSYS;
    }

    /* 新镜像的 brk/mmap 从窗口内那块堆的起点重新开始；旧游标属于旧镜像。 */
    s_heap_cur = 0ul;

    /* Linux 语义：execve 之后带 FD_CLOEXEC 的 fd 全部消失。放在换帧之后，
     * 因为这是整条路径上唯一不可能失败的收尾 —— 装载已经成功、旧镜像回不去
     * 了，这里也就不会出现"关了一半又返回"的中间态。 */
    lx_fd_close_cloexec();

    return LX_RET_EXEC_DONE;
}

#if (ARKRTOS_USE_LINUX_SCHED == 1)
/* ============================================================
 * U-28 调度策略翻译层（sched_* 十二个号）
 * ------------------------------------------------------------
 * 内核侧只认「固定优先级 + deadline 类」两件事；本层负责把 Linux 的
 * policy / priority / nice / deadline 三元组翻译成内核能执行的形态：
 *
 *   策略              Linux 参数              内核形态
 *   SCHED_OTHER       nice -20..19            优先级 128+nice（108..147）
 *   SCHED_BATCH       同 OTHER                同上（不做延迟唤醒，见文档）
 *   SCHED_IDLE        priority == 0           优先级 147（普通档最末格）
 *   SCHED_FIFO        priority 1..99          优先级 100-priority（1..99）
 *   SCHED_RR          同 FIFO                 同上（时间片由 tick 轮转提供）
 *   SCHED_DEADLINE    sched_setattr 三元组    arkrtos_task_dl_set()
 *
 * 这不是 CFS 权重：本内核是固定优先级抢占式调度器，nice 只是把任务
 * 放到普通档的某一格。SCHED_IDLE 因此不能真正做到「只在空闲时跑」，
 * 只能落在普通档最末一格 —— 如实记录，不假装有 CFS。
 *
 * policy 镜像存进 TCB（sched_policy），随槽位注册清零，不在 ABI 层另开
 * 一张会与任务槽复用脱节的表。
 * ============================================================ */
#define LX_SCHED_OTHER            (0)
#define LX_SCHED_FIFO             (1)
#define LX_SCHED_RR               (2)
#define LX_SCHED_BATCH            (3)
#define LX_SCHED_IDLE             (5)
#define LX_SCHED_DEADLINE         (6)

#define LX_SCHED_ATTR_SIZE_VER0   (48u)
#define LX_SCHED_ATTR_SIZE_VER1   (56u)

/* pid -> 任务号。pid == 0 表示调用者自己（Linux 语义）；负数非法。
 * 目标必须是「本内核认识的、还活着的」任务；否则 -ESRCH（与 kill 一致）。 */
static int32 lx_sched_target(int32 pid, int32 *p_out)
{
    int32 id = (pid == 0) ? (int32)arkrtos_current_task_id : pid;

    if(id <= 0)
    {
        return -LX_EINVAL;
    }
    if((id > (int32)ARKRTOS_TASK_MAX_NUM) ||
       (arkrtos_task_table[id - 1].status == ARKRTOS_TASK_INVALID))
    {
        return -LX_ESRCH;
    }
    *p_out = id;
    return 0;
}

/* nice -> 内核优先级。nice 0 落在普通档正中的 128。 */
static int32 lx_sched_prio_of(uint32 policy, int32 prio, int32 nice, uint8 *p_out)
{
    switch(policy)
    {
    case LX_SCHED_FIFO:
    case LX_SCHED_RR:
        if((prio < 1) || (prio > 99))
        {
            return -LX_EINVAL;
        }
        *p_out = (uint8)(100 - prio);
        return 0;
    case LX_SCHED_OTHER:
    case LX_SCHED_BATCH:
        if((nice < -20) || (nice > 19))
        {
            return -LX_EINVAL;
        }
        *p_out = (uint8)(128 + nice);
        return 0;
    case LX_SCHED_IDLE:
        if(prio != 0)
        {
            return -LX_EINVAL;
        }
        *p_out = (uint8)147;
        return 0;
    default:
        return -LX_EINVAL;
    }
}

/* 改内核优先级：priority 与 base_priority 一起改（不留一个会被继承撤销
 * 又回不去的中间值），并让就绪桶跟着搬。运行中的任务不在就绪链上，
 * reprio 自己会跳过。 */
static void lx_sched_set_prio(int32 id, uint8 new_prio)
{
    arkrtos_task_t *p = &arkrtos_task_table[id - 1];
    uint32 crit;
    uint8  old;

    crit = ARKRTOS_ENTER_CRITICAL();
    old  = p->priority;
    if(old != new_prio)
    {
        p->priority      = new_prio;
        p->base_priority = new_prio;
        arkrtos_ready_reprio(id - 1, old);
    }
    ARKRTOS_EXIT_CRITICAL(crit);
}

static void lx_sched_drop_dl(int32 id)
{
#if (ARKRTOS_USE_SCHED_DEADLINE == 1)
    if(arkrtos_task_table[id - 1].dl_active != 0u)
    {
        arkrtos_task_dl_clear(id);
    }
#else
    (void)id;
#endif
}

/* sched_setscheduler(118 是 setparam，119 是这个)。三个参数：
 * pid / policy / struct sched_param*。 */
static int32 lx_sched_setscheduler(void *p_ctx)
{
    int32         pid    = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32        policy = (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long u_par  = ARKRTOS_SVC_ARG(p_ctx, 2u);
    int32         prio   = 0;
    int32         id;
    int32         rc;
    uint8         kp;

    /* Linux：deadline 的三个参数只能经 sched_setattr 传入，这个号没有
     * 承载它们的字段，只能回 EINVAL（不是 ENOSYS —— 号本身认识）。 */
    if(policy == (uint32)LX_SCHED_DEADLINE)
    {
        return -LX_EINVAL;
    }
    if(u_par == 0ul)
    {
        return -LX_EFAULT;
    }
    if(arkrtos_copy_from_user(&prio, (const void *)u_par, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    if(rc != 0)
    {
        return rc;
    }
    rc = lx_sched_prio_of(policy, prio, 0, &kp);
    if(rc != 0)
    {
        return rc;
    }
    lx_sched_drop_dl(id);                    /* 从 deadline 类转出：释放带宽 */
    arkrtos_task_table[id - 1].sched_policy = (uint8)policy;
    lx_sched_set_prio(id, kp);
    return 0;
}

/* sched_setparam(118)：只改优先级，策略保持不变。OTHER/BATCH/IDLE 的
 * priority 必须是 0，且 nice 不由这个号改（Linux 同型）。 */
static int32 lx_sched_setparam(void *p_ctx)
{
    int32         pid   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long u_par = ARKRTOS_SVC_ARG(p_ctx, 1u);
    int32         prio  = 0;
    int32         id;
    int32         rc;
    uint32        policy;
    uint8         kp;

    if(u_par == 0ul)
    {
        return -LX_EFAULT;
    }
    if(arkrtos_copy_from_user(&prio, (const void *)u_par, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    if(rc != 0)
    {
        return rc;
    }
    policy = (uint32)arkrtos_task_table[id - 1].sched_policy;
    if(policy == (uint32)LX_SCHED_DEADLINE)
    {
        return -LX_EINVAL;                   /* 同 Linux：改用 sched_setattr */
    }
    if((policy == (uint32)LX_SCHED_OTHER) ||
       (policy == (uint32)LX_SCHED_BATCH) ||
       (policy == (uint32)LX_SCHED_IDLE))
    {
        return (prio == 0) ? 0 : -LX_EINVAL;
    }
    rc = lx_sched_prio_of(policy, prio, 0, &kp);
    if(rc != 0)
    {
        return rc;
    }
    lx_sched_set_prio(id, kp);
    return 0;
}

/* sched_getparam(121)：priority 由内核优先级反推。非 RT 策略一律回 0。 */
static int32 lx_sched_getparam(void *p_ctx)
{
    int32         pid   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long u_par = ARKRTOS_SVC_ARG(p_ctx, 1u);
    int32         id;
    int32         rc;
    int32         prio;
    uint32        policy;

    if(u_par == 0ul)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    if(rc != 0)
    {
        return rc;
    }
    policy = (uint32)arkrtos_task_table[id - 1].sched_policy;
    prio = 0;
    if((policy == (uint32)LX_SCHED_FIFO) || (policy == (uint32)LX_SCHED_RR))
    {
        prio = 100 - (int32)arkrtos_task_table[id - 1].priority;
    }
    if(arkrtos_copy_to_user((void *)u_par, &prio, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* sched_getscheduler(120)。 */
static int32 lx_sched_getscheduler(void *p_ctx)
{
    int32 pid = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    int32 id;
    int32 rc = lx_sched_target(pid, &id);

    if(rc != 0)
    {
        return rc;
    }
    return (int32)arkrtos_task_table[id - 1].sched_policy;
}

/* sched_setattr(274)。struct sched_attr 的布局（VER0 48 字节）：
 *   +0 size(u32)  +4 sched_policy(u32)  +8 sched_flags(u64)
 *   +16 sched_nice(s32)  +20 sched_priority(u32)
 *   +24 sched_runtime(u64)  +32 sched_deadline(u64)  +40 sched_period(u64)
 * 逐字段按字节读，避免对用户指针做一次结构体强转（对齐与大小端都不假设）。 */
static int32 lx_sched_setattr(void *p_ctx)
{
    int32              pid      = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long      ua       = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32             sysflags = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32             size     = 0u;
    uint32             policy   = 0u;
    unsigned long long aflags   = 0ull;
    int32              nice     = 0;
    uint32             prio     = 0u;
    unsigned long long runtime  = 0ull;
    unsigned long long deadline = 0ull;
    unsigned long long period   = 0ull;
    int32              id;
    int32              rc;
    uint8              kp;

    if(ua == 0ul)
    {
        return -LX_EFAULT;
    }
    if(arkrtos_copy_from_user(&size, (const void *)ua, 4u) != 0)
    {
        return -LX_EFAULT;
    }
    if(size < LX_SCHED_ATTR_SIZE_VER0)
    {
        return -LX_EINVAL;
    }
    if((arkrtos_copy_from_user(&policy, (const void *)(ua + 4ul), 4u) != 0) ||
       (arkrtos_copy_from_user(&aflags, (const void *)(ua + 8ul), 8u) != 0) ||
       (arkrtos_copy_from_user(&nice, (const void *)(ua + 16ul), 4u) != 0) ||
       (arkrtos_copy_from_user(&prio, (const void *)(ua + 20ul), 4u) != 0) ||
       (arkrtos_copy_from_user(&runtime, (const void *)(ua + 24ul), 8u) != 0) ||
       (arkrtos_copy_from_user(&deadline, (const void *)(ua + 32ul), 8u) != 0) ||
       (arkrtos_copy_from_user(&period, (const void *)(ua + 40ul), 8u) != 0))
    {
        return -LX_EFAULT;
    }
    /* 系统调用本身的 flags 与结构里的 sched_flags：一个未知位都不认。
     * RESET_ON_FORK 也没有实现 —— 认了却不做，就是把调用方的预期变成谎言。 */
    if((sysflags != 0u) || (aflags != 0ull))
    {
        return -LX_EINVAL;
    }
    rc = lx_sched_target(pid, &id);
    if(rc != 0)
    {
        return rc;
    }

    if(policy == (uint32)LX_SCHED_DEADLINE)
    {
#if (ARKRTOS_USE_SCHED_DEADLINE == 1)
        if(prio != 0u)
        {
            return -LX_EINVAL;
        }
        if((runtime == 0ull) || (deadline == 0ull) || (period == 0ull))
        {
            return -LX_EINVAL;
        }
        if((runtime > 0xFFFFFFFFull) || (deadline > 0xFFFFFFFFull) ||
           (period > 0xFFFFFFFFull))
        {
            return -LX_EINVAL;       /* 内核按 uint32 ns 存参数 */
        }
        rc = arkrtos_task_dl_set(id, (uint32)runtime, (uint32)deadline,
                                 (uint32)period);
        if(rc == ARKRTOS_TASK_ERR_BUSY)
        {
            return -LX_EBUSY;        /* 准入失败：带宽不足 */
        }
        if(rc != 0)
        {
            return -LX_EINVAL;
        }
        arkrtos_task_table[id - 1].sched_policy = (uint8)LX_SCHED_DEADLINE;
        return 0;
#else
        /* 内核没有 deadline 类：不假装成功。 */
        return -LX_EINVAL;
#endif
    }

    /* 非 deadline：三个时间参数必须全 0（Linux 同型），否则是调用方把
     * deadline 参数塞进了别的策略。 */
    if((runtime != 0ull) || (deadline != 0ull) || (period != 0ull))
    {
        return -LX_EINVAL;
    }
    rc = lx_sched_prio_of(policy, (int32)prio, nice, &kp);
    if(rc != 0)
    {
        return rc;
    }
    lx_sched_drop_dl(id);
    arkrtos_task_table[id - 1].sched_policy = (uint8)policy;
    lx_sched_set_prio(id, kp);
    return 0;
}

/* sched_getattr(275)。回写同一布局；size >= VER1 时把 util 两个字段写成 0
 * （本内核不做利用率跟踪，但字段存在，必须给定值而不是留垃圾）。 */
static int32 lx_sched_getattr(void *p_ctx)
{
    int32              pid      = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long      ua       = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32             size     = (uint32)ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint32             sysflags = (uint32)ARKRTOS_SVC_ARG(p_ctx, 3u);
    uint32             policy;
    int32              nice = 0;
    uint32             prio = 0u;
    uint32             runtime = 0u;
    uint32             deadline = 0u;
    uint32             period = 0u;
    unsigned long long zero64 = 0ull;
    uint32             zero32 = 0u;
    uint32             wsize;
    int32              id;
    int32              rc;

    if(sysflags != 0u)
    {
        return -LX_EINVAL;
    }
    if(size < LX_SCHED_ATTR_SIZE_VER0)
    {
        return -LX_EINVAL;
    }
    if(ua == 0ul)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    if(rc != 0)
    {
        return rc;
    }
    policy = (uint32)arkrtos_task_table[id - 1].sched_policy;
    if((policy == (uint32)LX_SCHED_OTHER) || (policy == (uint32)LX_SCHED_BATCH))
    {
        nice = (int32)arkrtos_task_table[id - 1].priority - 128;
    }
    if((policy == (uint32)LX_SCHED_FIFO) || (policy == (uint32)LX_SCHED_RR))
    {
        prio = (uint32)(100 - (int32)arkrtos_task_table[id - 1].priority);
    }
#if (ARKRTOS_USE_SCHED_DEADLINE == 1)
    if(policy == (uint32)LX_SCHED_DEADLINE)
    {
        (void)arkrtos_task_dl_get(id, &runtime, &deadline, &period);
    }
#endif
    wsize = (size >= LX_SCHED_ATTR_SIZE_VER1) ? LX_SCHED_ATTR_SIZE_VER1
                                              : LX_SCHED_ATTR_SIZE_VER0;
    if((arkrtos_copy_to_user((void *)ua, &wsize, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 4ul), &policy, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 8ul), &zero64, 8u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 16ul), &nice, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 20ul), &prio, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 24ul), &runtime, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 28ul), &zero32, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 32ul), &deadline, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 36ul), &zero32, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 40ul), &period, 4u) != 0) ||
       (arkrtos_copy_to_user((void *)(ua + 44ul), &zero32, 4u) != 0))
    {
        return -LX_EFAULT;
    }
    if(wsize >= LX_SCHED_ATTR_SIZE_VER1)
    {
        if((arkrtos_copy_to_user((void *)(ua + 48ul), &zero32, 4u) != 0) ||
           (arkrtos_copy_to_user((void *)(ua + 52ul), &zero32, 4u) != 0))
        {
            return -LX_EFAULT;
        }
    }
    return 0;
}

/* sched_get_priority_max(125) / min(126)。FIFO/RR 是 99/1；其余策略 0/0
 * （Linux 对 OTHER/BATCH/IDLE/DEADLINE 都给 0）。 */
static int32 lx_sched_prio_bound(void *p_ctx, uint8 want_max)
{
    uint32 policy = (uint32)ARKRTOS_SVC_ARG(p_ctx, 0u);

    switch(policy)
    {
    case LX_SCHED_FIFO:
    case LX_SCHED_RR:
        return want_max ? 99 : 1;
    case LX_SCHED_OTHER:
    case LX_SCHED_BATCH:
    case LX_SCHED_IDLE:
    case LX_SCHED_DEADLINE:
        return 0;
    default:
        return -LX_EINVAL;
    }
}

/* sched_rr_get_interval(127)。RR 的轮转量就是内核时间片，按 ns 回给用户。
 * struct timespec 在 LP64 上是两个 8 字节 long。 */
static int32 lx_sched_rr_get_interval(void *p_ctx)
{
    int32         pid = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long up  = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long long ns;
    unsigned long sec;
    unsigned long nsec;
    int32         id;
    int32         rc = lx_sched_target(pid, &id);
    (void)id;

    if(rc != 0)
    {
        return rc;
    }
    if(up == 0ul)
    {
        return -LX_EFAULT;
    }
    ns   = ((unsigned long long)ARKRTOS_TIME_SLICE_TICKS) *
           ((unsigned long long)ARKRTOS_TICK_PERIOD_US) * 1000ull;
    sec  = (unsigned long)(ns / 1000000000ull);
    nsec = (unsigned long)(ns % 1000000000ull);
    if((arkrtos_copy_to_user((void *)up, &sec, (uint32)sizeof(unsigned long)) != 0) ||
       (arkrtos_copy_to_user((void *)(up + (unsigned long)sizeof(unsigned long)),
                             &nsec, (uint32)sizeof(unsigned long)) != 0))
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* sched_setaffinity(122) / getaffinity(123)。本内核只有一个 CPU：掩码里
 * 只允许 bit0，且必须给出 bit0。len 按 Linux 规则至少一个 unsigned long。 */
static int32 lx_sched_setaffinity(void *p_ctx)
{
    int32         pid  = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32        len  = (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long um   = ARKRTOS_SVC_ARG(p_ctx, 2u);
    unsigned long mask = 0ul;
    int32         id;
    int32         rc;

    if(len < (uint32)sizeof(unsigned long))
    {
        return -LX_EINVAL;
    }
    if(um == 0ul)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    (void)id;
    if(rc != 0)
    {
        return rc;
    }
    if(arkrtos_copy_from_user(&mask, (const void *)um, (uint32)sizeof(mask)) != 0)
    {
        return -LX_EFAULT;
    }
    if(((mask & 1ul) == 0ul) || ((mask & ~1ul) != 0ul))
    {
        return -LX_EINVAL;       /* 只有 CPU0；空掩码或别核位都不收 */
    }
    return 0;
}

static int32 lx_sched_getaffinity(void *p_ctx)
{
    int32         pid  = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32        len  = (uint32)ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long um   = ARKRTOS_SVC_ARG(p_ctx, 2u);
    unsigned long mask = 1ul;
    int32         id;
    int32         rc;

    if(len < (uint32)sizeof(unsigned long))
    {
        return -LX_EINVAL;
    }
    if(um == 0ul)
    {
        return -LX_EFAULT;
    }
    rc = lx_sched_target(pid, &id);
    (void)id;
    if(rc != 0)
    {
        return rc;
    }
    if(arkrtos_copy_to_user((void *)um, &mask, (uint32)sizeof(mask)) != 0)
    {
        return -LX_EFAULT;
    }
    return 0;
}

/* sched_yield(124)：把自己挪到同优先级队尾，再请求一次调度。 */
static int32 lx_sched_yield(void)
{
    arkrtos_task_yield_internal();
    return 0;
}


#endif /* ARKRTOS_USE_LINUX_SCHED */

/* ---------------------------------------------------------------- 入口 */

/* 号表直方图：在分发之前记，走到 default 的 -ENOSYS 与 exit 提前返回的调用
 * 也要进直方图，否则「程序卡在哪一号」恰好在最关键的那几号上看不见。 */
#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
static void lx_note_nr(uint32 num)
{
    uint32 i;

    for(i = 0u; i < s_nr_used; i++)
    {
        if(s_nr[i].nr == num)
        {
            s_nr[i].hits++;
            break;
        }
    }
    if(i == s_nr_used)
    {
        if(s_nr_used < (uint32)LX_NR_SLOTS)
        {
            s_nr[s_nr_used].nr   = num;
            s_nr[s_nr_used].hits = 1u;
            s_nr_used++;
        }
        else
        {
            s_nr_overflow++;
        }
    }
}
#endif

/* 号表分派。返回非 0 表示「这次调用已经结束、调用方直接 return 1」——只有
 * exit 族（帧已交还）与 default（回了 -ENOSYS）这两条路会这样；其余都写
 * *p_rc 交给调用方做返回值分类。 */
static int32 lx_dispatch(void *p_svc_ctx, uint32 num, int32 *p_rc)
{
    int32 rc = 0;

    switch(num)
    {
    case LX_SYS_DUP:
        rc = lx_dup(p_svc_ctx);
        break;
    case LX_SYS_DUP3:
        rc = lx_dup3(p_svc_ctx);
        break;
    case LX_SYS_FCNTL:
        rc = lx_fcntl(p_svc_ctx);
        break;
    case LX_SYS_RSEQ:
        rc = lx_rseq(p_svc_ctx);
        break;
    case LX_SYS_GETCPU:
        rc = lx_getcpu(p_svc_ctx);
        break;
    case LX_SYS_OPENAT:
        rc = lx_openat(p_svc_ctx);
        break;
    case LX_SYS_CLOSE:
        rc = lx_close(p_svc_ctx);
        break;
    case LX_SYS_READ:
        rc = lx_read(p_svc_ctx);
        break;
    case LX_SYS_WRITE:
        rc = lx_write(p_svc_ctx);
        break;
    case LX_SYS_PREAD64:
        rc = lx_pread64(p_svc_ctx);
        break;
    case LX_SYS_PIPE2:
        rc = lx_pipe2(p_svc_ctx);
        break;
    case LX_SYS_KILL:
        rc = lx_kill(p_svc_ctx);
        break;
    case LX_SYS_TKILL:
        rc = lx_tkill(p_svc_ctx);
        break;
    case LX_SYS_TGKILL:
        rc = lx_tgkill(p_svc_ctx);
        break;
    case LX_SYS_RT_SIGACTION:
        rc = lx_rt_sigaction(p_svc_ctx);
        break;
    case LX_SYS_RT_SIGPROCMASK:
        rc = lx_rt_sigprocmask(p_svc_ctx);
        break;
    case LX_SYS_RT_SIGRETURN:
        rc = lx_rt_sigreturn(p_svc_ctx);
        break;
    case LX_SYS_MPROTECT:
        rc = lx_mprotect(p_svc_ctx);
        break;
    case LX_SYS_CLOCK_GETTIME:
        rc = lx_clock_gettime(p_svc_ctx);
        break;
    case LX_SYS_UNAME:
        rc = lx_uname(p_svc_ctx);
        break;
    case LX_SYS_BRK:
        rc = lx_brk(p_svc_ctx);
        break;
    case LX_SYS_EXECVE:
        rc = lx_execve(p_svc_ctx);
        break;
    case LX_SYS_MMAP:
        rc = lx_mmap(p_svc_ctx);
        break;
    case LX_SYS_MUNMAP:
        rc = lx_munmap(p_svc_ctx);
        break;
    #if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    case LX_SYS_MSYNC:
        rc = lx_msync(p_svc_ctx);
        break;
    #endif
    #if (ARKRTOS_USE_LINUX_FUTEX == 1)
    case LX_SYS_FUTEX:
        rc = lx_futex(p_svc_ctx);
        break;
    #endif
    #if (ARKRTOS_USE_LINUX_SELECT == 1)
    case LX_SYS_PSELECT6:
        rc = lx_pselect6(p_svc_ctx);
        break;
    #endif
    #if (ARKRTOS_USE_LINUX_SOCKET == 1)
    case LX_SYS_SOCKET:
        rc = lx_socket(p_svc_ctx);
        break;
    case LX_SYS_BIND:
        rc = lx_bind(p_svc_ctx);
        break;
    case LX_SYS_LISTEN:
        rc = lx_listen(p_svc_ctx);
        break;
    case LX_SYS_ACCEPT:
        rc = lx_accept(p_svc_ctx, 0u);
        break;
    case LX_SYS_ACCEPT4:
        rc = lx_accept(p_svc_ctx, 1u);
        break;
    case LX_SYS_CONNECT:
        rc = lx_connect(p_svc_ctx);
        break;
    case LX_SYS_GETSOCKNAME:
        rc = lx_sock_name(p_svc_ctx, 0u);
        break;
    case LX_SYS_GETPEERNAME:
        rc = lx_sock_name(p_svc_ctx, 1u);
        break;
    case LX_SYS_SENDTO:
        rc = lx_sendto(p_svc_ctx);
        break;
    case LX_SYS_RECVFROM:
        rc = lx_recvfrom(p_svc_ctx);
        break;
    case LX_SYS_SETSOCKOPT:
        rc = lx_setsockopt(p_svc_ctx);
        break;
    case LX_SYS_GETSOCKOPT:
        rc = lx_getsockopt(p_svc_ctx);
        break;
    case LX_SYS_SHUTDOWN:
        rc = lx_shutdown(p_svc_ctx);
        break;
    #endif
    #if (ARKRTOS_USE_LINUX_SCHED == 1)
    case LX_SYS_SCHED_SETPARAM:
        rc = lx_sched_setparam(p_svc_ctx);
        break;
    case LX_SYS_SCHED_SETSCHEDULER:
        rc = lx_sched_setscheduler(p_svc_ctx);
        break;
    case LX_SYS_SCHED_GETSCHEDULER:
        rc = lx_sched_getscheduler(p_svc_ctx);
        break;
    case LX_SYS_SCHED_GETPARAM:
        rc = lx_sched_getparam(p_svc_ctx);
        break;
    case LX_SYS_SCHED_SETAFFINITY:
        rc = lx_sched_setaffinity(p_svc_ctx);
        break;
    case LX_SYS_SCHED_GETAFFINITY:
        rc = lx_sched_getaffinity(p_svc_ctx);
        break;
    case LX_SYS_SCHED_YIELD:
        rc = lx_sched_yield();
        break;
    case LX_SYS_SCHED_GET_PRIORITY_MAX:
        rc = lx_sched_prio_bound(p_svc_ctx, 1u);
        break;
    case LX_SYS_SCHED_GET_PRIORITY_MIN:
        rc = lx_sched_prio_bound(p_svc_ctx, 0u);
        break;
    case LX_SYS_SCHED_RR_GET_INTERVAL:
        rc = lx_sched_rr_get_interval(p_svc_ctx);
        break;
    case LX_SYS_SCHED_SETATTR:
        rc = lx_sched_setattr(p_svc_ctx);
        break;
    case LX_SYS_SCHED_GETATTR:
        rc = lx_sched_getattr(p_svc_ctx);
        break;
#endif
#if (ARKRTOS_USE_LINUX_FORK == 1)
    case LX_SYS_CLONE:
        rc = lx_clone(p_svc_ctx);
        break;
    case LX_SYS_WAIT4:
        rc = lx_wait4(p_svc_ctx);
        break;
    #endif
    case LX_SYS_GETPID:
#if (ARKRTOS_USE_LINUX_THREAD == 1)
        /* 线程组语义：同一组里每个线程的 getpid 都回组长的号（Linux 就是这么
         * 定义的）。没有线程时它就是自己的任务号，与打开本档之前逐位相同。 */
        rc = lx_tgid_of_current();
#else
        rc = (int32)arkrtos_current_task_id;
#endif
        break;
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    case LX_SYS_GETTID:
        /* 本线程自己的号。关闭线程档时不认这个号（仍回 -ENOSYS）—— 没有线程
         * 的世界里 gettid == getpid，多认一个号只会让默认镜像的行为变样。 */
        rc = (int32)arkrtos_current_task_id;
        break;
#endif
    case LX_SYS_GETUID:
    case LX_SYS_GETEUID:
        rc = 0;         /* 没有用户模型：唯一身份是 0，不编造 uid */
        break;
    case LX_SYS_SET_TID_ADDRESS:
        /* Linux 把这个指针登记给 robust futex 清理用。本内核的 futex 只有
         * WAIT/WAKE，没有 robust 表，因此只回报自己的 tid，不保存、也不往
         * 用户内存里写一个字（glibc 传进来的就是它自己的 tid 变量，替它清零
         * 是副作用）。开关关掉时同样如此。 */
        rc = (int32)arkrtos_current_task_id;
        break;
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    case LX_SYS_SET_ROBUST_LIST:
        /* 退出时按这条链把还挂着的字标成「持有者已死」。没有 futex 档时这条
         * 链没有语义（连 FUTEX_OWNER_DIED 都无法表达），如实 -ENOSYS。 */
        rc = lx_robust_set(ARKRTOS_SVC_ARG(p_svc_ctx, 0u),
                           (uint32)ARKRTOS_SVC_ARG(p_svc_ctx, 1u));
        break;
    case LX_SYS_GET_ROBUST_LIST:
        rc = lx_robust_get(p_svc_ctx);
        break;
#endif
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    case LX_SYS_EXIT_GROUP:
        /* exit_group(94)：整组一起走。分派器在两个号之间分得清，因为号就是
         * 这里的 case 标签 —— 不需要从帧里反推是哪一条指令。 */
        s_stat[LX_STAT_HANDLED]++;
        {
            int32 code0 = (int32)ARKRTOS_SVC_ARG(p_svc_ctx, 0u);

            lx_thread_group_exit(code0);
            #if (ARKRTOS_USE_EXIT_CODE == 1)
            arkrtos_thread_exit_code_internal(code0);
            #else
            arkrtos_thread_exit_internal();
            #endif
        }
        lx_ret(p_svc_ctx, 0);
        return 1;
#else
    /* 线程档关掉时没有线程组，exit_group 与 exit 同实体（单线程程序里
     * 两者本来就等价）—— Linux 的 glibc 在任何进程里都可能发 94，
     * 回 -ENOSYS 会让「正常退出」变成一条错误路径。 */
    case LX_SYS_EXIT_GROUP:
#endif
    case LX_SYS_EXIT:
        /* exit(93) 只退调用它的那个线程（关闭线程档时就是退整个进程）。
         * 组里其它线程继续跑；整组退出走上面的 exit_group(94)。 */
        s_stat[LX_STAT_HANDLED]++;
        #if (ARKRTOS_USE_EXIT_CODE == 1)
        arkrtos_thread_exit_code_internal((int32)ARKRTOS_SVC_ARG(p_svc_ctx, 0u));
        #else
        arkrtos_thread_exit_internal();
        #endif
        lx_ret(p_svc_ctx, 0);
        return 1;
    default:
        s_stat[LX_STAT_ENOSYS]++;
        lx_ret(p_svc_ctx, -LX_ENOSYS);
        return 1;
    }

    *p_rc = rc;
    return 0;
}

/* 返回值分类 + 写回 + 信号投递。地址类与 exec 已经在处理函数里按整字宽写回
 * 或换好了帧，这里只补计数、不许再覆盖。 */
static void lx_finish(void *p_svc_ctx, int32 rc)
{
    if(rc == LX_RET_PTR_DONE)
    {
        /* 地址类返回值已由处理函数按整字宽写回，这里不许再覆盖。 */
        s_stat[LX_STAT_HANDLED]++;
        return;
    }
    if(rc == LX_RET_EXEC_DONE)
    {
        /* execve 成功：帧已经是新镜像的入口，没有返回值可写（Linux 语义）。 */
        s_stat[LX_STAT_HANDLED]++;
        return;
    }
    if(rc == LX_RET_RETRY)
    {
        /* 还在等（socket / pselect6 / 阻塞管道）：这一次不算完成，既不写 a0
         * （那是本次调用的输入，重放的那一次要重新读），也不投信号（调用
         * 还没返回，信号帧里不该有这个半截的现场）。 */
        s_stat[LX_STAT_HANDLED]++;
        return;
    }
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 这一次 socket 系统调用真的结束了（不是重放）：把登记撤掉。不撤的话
     * 下一次调用进来 armed 还立着，参数会从上一次的槽里取 —— 表现是「参数
     * 被悄悄换成上一次的」，只有真跑一串调用才看得出来。重放那一路在上面
     * 已经 return 掉了，accept 的两步式（同一逻辑调用里两次 net 请求）也不
     * 受影响。 */
    lx_sock_ctx_clear((int32)arkrtos_current_task_id);
#endif
    if(rc == -LX_EFAULT)
    {
        s_stat[LX_STAT_EFAULT]++;
    }
    else if(rc < 0)
    {
        s_stat[LX_STAT_OTHER]++;
    }
    else
    {
        s_stat[LX_STAT_HANDLED]++;
    }

    lx_ret(p_svc_ctx, rc);
    /* 投递点：系统调用返回用户态之前。此时 a0 已经是本次调用的返回值，
     * 所以信号帧把它一并存下来，rt_sigreturn 再装回去。 */
    (void)lx_signal_deliver(p_svc_ctx);
}

int32 arkrtos_linux_abi_try(void *p_svc_ctx)
{
    uint32 num = arkrtos_port_linux_syscall_num(p_svc_ctx);
    int32  rc  = 0;

    if(num == 0xFFFFFFFFu)
    {
        return 0;       /* 不是 Linux ABI 调用，交回 Ark 号表 */
    }

#if (ARKRTOS_USE_LINUX_FORK == 1)
    if(lx_proc_cur() >= (uint32)LX_PROC_MAX)
    {
        /* 进程状态槽用满：如实回 -ENOMEM。两条进程共用一份 fd/信号表会产出
         * 一个查不出来的错答案（子进程 close 掉父的 fd），比拒绝更糟。 */
        s_stat[LX_STAT_OTHER]++;
        ARKRTOS_SVC_RET(p_svc_ctx, (uint32)(-LX_ENOMEM));
        return 1;
    }
#endif


    #if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    lx_note_nr(num);
    #endif

    if(lx_dispatch(p_svc_ctx, num, &rc) != 0)
    {
        return 1;       /* exit 族 / default：返回码已写、调用已结束 */
    }

    lx_finish(p_svc_ctx, rc);
    return 1;
}

uint32 arkrtos_linux_abi_stat(uint32 which)
{
    if(which >= (uint32)LX_STAT_MAX)
    {
        return 0u;
    }
    return s_stat[which];
}

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
/* 三个读数函数只在开关打开时才有实体：默认档（SYSSTAT 0）连这三个
 * 恒返回 0 的空壳都不该存在——「默认关 = 体积与行为零变化」是实测过的
 * 契约，空壳会让默认档多出上百字节代码。调用点本来就全在
 * `#if SYSSTAT == 1` 里，头文件在关档时也不声明它们。 */

uint32 arkrtos_linux_abi_nr_slots(void)
{
    return s_nr_used;
}

uint32 arkrtos_linux_abi_nr_slot(uint32 idx, uint32 *p_nr)
{
    if(idx >= s_nr_used)
    {
        if(p_nr != 0)
        {
            *p_nr = 0u;
        }
        return 0u;
    }
    if(p_nr != 0)
    {
        *p_nr = s_nr[idx].nr;
    }
    return s_nr[idx].hits;
}

uint32 arkrtos_linux_abi_nr_overflow(void)
{
    return s_nr_overflow;
}
#endif /* ARKRTOS_LINUX_ABI_SYSSTAT */

#if (ARKRTOS_USE_LINUX_FORK == 1)
/**
* @brief 进程退出收尾（fork 记账 + 父进程通知 + 状态还池）
* @details 调用点在 arkrtos_task_kill_internal()：status 已是 INVALID、
*          退出码已写好。这里做三件事：
*            1) 对每一条「pid == 刚死的任务、代次也对得上」的记账，唤醒
*               正在 wait4 里等它的父进程，并置 SIGCHLD 待投递位；
*            2) 把该进程的 fd 全部关掉（管道读写端引用计数递减，与 close
*               同一条路径）；
*            3) 信号动作表与槽位还池，并解除任务号绑定。
*          第 1 步只唤醒「登记过自己在 wait4」的父进程（s_lx_wait），
*          不打扰因为别的原因在等的任务；父进程没在等就只是 pending。
*/
void arkrtos_linux_abi_on_task_exit(int32 task_id)
{
    uint32 i;

    if((task_id <= 0) || (task_id > (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }
    /* 任务号会被复用：不清的话下一个任务在切换点上会去读上一个程序留下的
     * 地址（那时那张页表已经不在，读到的每一格都不可信）。 */
    s_rseq_area[(uint32)task_id - 1u] = 0UL;
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* U-21：退出是写回的一个时机（Linux 在 close/exit 时把脏页写回）。这一步
     * 必须在下面的临界区之前做完 —— 写回要调 VFS，不能在关中断里干，也不能
     * 拿着 fd 表的锁去等存储。写回失败只能记在日志外的读数里：进程已经在
     * 退出，没有返回值可以交（这条代价写在开关文档里）。 */
    {
        int32 fslot = lx_proc_find(task_id);
        uint32 f;

        if(fslot >= 0)
        {
            for(f = 0u; f < (uint32)LX_FILEMAP_MAX; f++)
            {
                (void)lx_filemap_sync(&s_proc[(uint32)fslot].fmap[f]);
            }
        }
    }
#endif
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    /* 顺序不能换：robust 清理要按「它还持有这个字」改写用户内存，而
     * lx_futex_forget 只清内核这边的登记，两者互不影响，但先清链更直观地
     * 表达「先交代它欠下的锁，再撤掉它的等待槽」。 */
    lx_robust_exit(task_id);
    lx_futex_forget(task_id);
#endif
#if (ARKRTOS_USE_LINUX_SELECT == 1)
    lx_sel_forget(task_id);
#endif
    /* 任务号会被复用：不清的话新任务第一次碰管道就走了「重放」分支，
     * 参数从上一个已死任务的登记里取。 */
    lx_pipe_forget(task_id);
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 任务号会被复用：不清的话新任务第一次 socket 调用就走了重放分支。 */
    lx_sock_ctx_clear(task_id);
#endif

    for(i = 0u; i < (uint32)LX_MAX_CHILD; i++)
    {
        int32  parent;
        int32  pslot;
        uint32 crit;

        if((s_child[i].used == 0u) || (s_child[i].pid != task_id))
        {
            continue;
        }
        if(arkrtos_task_table[task_id - 1].generation != s_child[i].generation)
        {
            continue;
        }
        parent = s_child[i].parent;
        if((parent <= 0) || (parent > (int32)ARKRTOS_TASK_MAX_NUM))
        {
            continue;
        }
        pslot = lx_proc_find(parent);
        if(pslot < 0)
        {
            continue;       /* 父进程没有进程状态：不伪造通知 */
        }
        crit = ARKRTOS_ENTER_CRITICAL();
        s_proc[(uint32)pslot].sig_pending |= lx_sig_bit((uint32)LX_SIGCHLD_FLAG);
        if(s_lx_wait[parent - 1] != 0u)
        {
            (void)arkrtos_task_wakeup_internal(parent);
        }
        ARKRTOS_EXIT_CRITICAL(crit);
    }

#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* U-25：同一个线程组的槽是被几个任务共用的。只要组里还有别人活着，这一次
     * 退出就只是「一个线程退了」——关 fd、清槽、放开 socket 全都不能做，
     * 否则退掉的这一个会把整组的 fd 表与套接字一起带走。
     * 位置必须在 sock_close_all 与 fd 释放之前（它们在下面）。 */
    {
        int32 tslot = lx_proc_find(task_id);

        if((tslot >= 0) && (lx_proc_others_live((uint32)tslot, task_id) != 0u))
        {
            lx_thread_clear_tid(task_id);
            return;
        }
    }
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 套接字的 net 句柄不受 fd 表的临界区保护，也必须在关中断之前投出去。 */
    {
        int32 sslot = lx_proc_find(task_id);

        if(sslot >= 0)
        {
            lx_sock_close_all((uint32)sslot);
        }
    }
#endif
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    /* 组里最后一个人走了：CLEARTID 也在这里清（前面的提前返回已经处理过
     * 「还有兄弟活着」的情形）。 */
    lx_thread_clear_tid(task_id);
#endif
    {
        int32 slot = lx_proc_find(task_id);

        if(slot >= 0)
        {
            uint32 crit = ARKRTOS_ENTER_CRITICAL();
            uint32 f;

            for(f = 0u; f < LX_MAX_FD; f++)
            {
                (void)lx_fd_release((uint32)slot, f);
            }
            lx_proc_zero((uint32)slot);
            lx_proc_free((uint32)slot);
            lx_proc_unbind(task_id);
            ARKRTOS_EXIT_CRITICAL(crit);
        }
    }
}
#else
/* fork 关档：内核只在 ARKRTOS_USE_LINUX_ABI=1 时调用本函数，这里如实回答
 * 「没有可收尾的进程状态」，而不是假装做完了什么。 */
void arkrtos_linux_abi_on_task_exit(int32 task_id)
{
    if((task_id > 0) && (task_id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        s_rseq_area[(uint32)task_id - 1u] = 0UL;
    }
    (void)task_id;
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    /* 顺序不能换：robust 清理要按「它还持有这个字」改写用户内存，而
     * lx_futex_forget 只清内核这边的登记，两者互不影响，但先清链更直观地
     * 表达「先交代它欠下的锁，再撤掉它的等待槽」。 */
    lx_robust_exit(task_id);
    lx_futex_forget(task_id);
#endif
#if (ARKRTOS_USE_LINUX_SELECT == 1)
    lx_sel_forget(task_id);
#endif
    /* 任务号会被复用：不清的话新任务第一次碰管道就走了「重放」分支，
     * 参数从上一个已死任务的登记里取。 */
    lx_pipe_forget(task_id);
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    /* 任务号会被复用：不清的话新任务第一次 socket 调用就走了重放分支。 */
    lx_sock_ctx_clear(task_id);
#endif
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* 关 fork 档只有槽 0：退出即写回，与开档那条路同一口径。 */
    {
        uint32 f;

        for(f = 0u; f < (uint32)LX_FILEMAP_MAX; f++)
        {
            (void)lx_filemap_sync(&s_proc[0].fmap[f]);
        }
    }
#endif
}
#endif /* ARKRTOS_USE_LINUX_FORK == 1 */


void arkrtos_linux_abi_reset(void)
{
    uint32 i;
    uint32 slot;

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    for(i = 0u; i < (uint32)LX_NR_SLOTS; i++)
    {
        s_nr[i].nr   = 0u;
        s_nr[i].hits = 0u;
    }
    s_nr_used     = 0u;
    s_nr_overflow = 0u;
#endif
    /* 进程状态（fd 表 + 信号动作/屏蔽/pending）逐槽清空。fork 关档时只有
     * 槽 0，等价于历史版本的「清那张全局表」；fork 开档时连槽位与任务号的
     * 映射一起清，下一个进 ABI 的任务重新惰性分配。 */
    for(slot = 0u; slot < (uint32)LX_PROC_MAX; slot++)
    {
        lx_proc_zero(slot);
    }
#if (ARKRTOS_USE_LINUX_FORK == 1)
    for(i = 0u; i < (uint32)LX_PROC_MAX; i++)
    {
        s_proc_used[i] = 0u;
    }
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_proc_of_task[i] = 0u;
        s_lx_wait[i]      = 0u;
    }
#endif
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_futex_key[i]      = ~0UL;
        s_futex_used[i]     = 0u;
        s_futex_woke[i]     = 0u;
        s_futex_uaddr[i]    = 0ul;
        s_futex_val[i]      = 0u;
        s_futex_bitset[i]   = 0u;
        s_futex_deadline[i] = 0u;
        s_futex_timed[i]    = 0u;
        s_futex_pi[i]       = 0u;
        s_futex_pi_owner[i] = 0;
        s_robust_head[i]    = 0ul;
        s_robust_len[i]     = 0u;
    }
#endif
#if (ARKRTOS_USE_LINUX_SELECT == 1)
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_sel_armed[i]    = 0u;
        s_sel_deadline[i] = 0u;
    }
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        lx_sock_ctx_clear((int32)(i + 1u));
    }
#endif

    for(i = 0u; i < LX_PIPE_MAX; i++)
    {
        s_pipe[i].used    = 0u;
        s_pipe[i].readers = 0u;
        s_pipe[i].writers = 0u;
        s_pipe[i].head    = 0u;
        s_pipe[i].tail    = 0u;
        s_pipe[i].count   = 0u;
    }
    /* 管道等待登记是任务自己的：换程序就该视为没在等（上一个程序的等待
     * 在重放时会拿一个已经不存在的 fd 去判）。 */
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_pipe_wait[i].used = 0u;
        s_pipe_wait[i].done = 0u;
    }

    /* 信号：动作表、pending、屏蔽字已在上面逐槽清过，这里只归零计数器。
     * 新程序不该继承上一个程序的信号处理函数（那是一个已经不存在的代码地址）。 */
    s_sig_delivered = 0u;
    s_sig_ignored = 0u;
    s_sig_refused = 0u;
    s_sig_no_restorer = 0u;
    s_sig_stop_ignored = 0u;
#if (ARKRTOS_USE_LINUX_SIGIO == 1)
    s_sigio_sent = 0u;
    s_sigio_pgrp_refused = 0u;
#endif

    s_heap_cur = 0u;

    /* rseq 注册是任务自己的：换程序就该视为没注册。上一个程序留下的地址在
     * 新镜像里可能已经不可读，切换点上拿它去做 rseq_cs 判定只会读到垃圾。 */
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        s_rseq_area[i] = 0UL;
    }

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* 写回缓冲的忙标志：换程序时归零。上一个程序的写回若是被强杀的，标志
     * 会留在 1，下一个程序的所有写回都会莫名其妙 -EBUSY。 */
    s_fm_wb_busy = 0u;
#endif

#if (ARKRTOS_USE_LINUX_FORK == 1)
    /* 子进程记账：换程序时一并清掉（装载器的语义仍是「起一个新程序」，
     * 上一次留下的父子关系不该被它继承）。 */
    for(i = 0u; i < (uint32)LX_MAX_CHILD; i++)
    {
        s_child[i].used       = 0u;
        s_child[i].pid        = 0;
        s_child[i].generation = 0u;
        s_child[i].parent     = 0;
    }
#endif
}

#endif /* ARKRTOS_USE_LINUX_ABI */
