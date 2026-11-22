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
*          任务号不变、fd 表保留、堆游标归零）；fork / clone 仍然没有——
*          那需要地址空间复制，与「一个任务一块窗口」的模型不是同一级工作量，
*          假装支持只会给出错答案。
*          管道（pipe2）已做：一条环形缓冲 + 两端引用计数，两个 fd 指向同一条
*          管道；读空回 -EAGAIN、写满回 -EAGAIN、读端全关写回 -EPIPE、写端
*          全关读回 EOF。**不实现阻塞**——没有 fork、fd 表全局单份，两端通常
*          在同一个任务里，阻塞就是自死锁。
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
#include "arkrtos_def.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "arkrtos_board.h"
#include "arkrtos_elf.h"   /* execve：装载器接口与 ARKRTOS_ELF_ERR_* */

/* ---------------------------------------------------------------- 常量 */

/* asm-generic 号表：RISC-V 与 LoongArch 的 Linux UAPI 都用这一套。 */
#define LX_SYS_OPENAT               (56u)
#define LX_SYS_CLOSE                (57u)
#define LX_SYS_PIPE2                (59u)
#define LX_SYS_READ                 (63u)
#define LX_SYS_WRITE                (64u)
#define LX_SYS_PREAD64              (67u)
#define LX_SYS_EXIT                 (93u)
#define LX_SYS_EXIT_GROUP           (94u)
#define LX_SYS_SET_TID_ADDRESS      (96u)
#define LX_SYS_CLOCK_GETTIME        (113u)
#define LX_SYS_UNAME                (160u)
#define LX_SYS_GETPID               (172u)
#define LX_SYS_GETUID               (174u)
#define LX_SYS_GETEUID              (175u)
#define LX_SYS_BRK                  (214u)
#define LX_SYS_MUNMAP               (215u)
#define LX_SYS_EXECVE               (221u)
#define LX_SYS_MMAP                 (222u)
#define LX_SYS_MPROTECT             (226u)

/* 信号相关的号：rt_sigaction / rt_sigprocmask / kill / tkill / tgkill /
 * rt_sigreturn。asm-generic 编号，LoongArch 与 RISC-V 共用。 */
#define LX_SYS_KILL                 (129u)
#define LX_SYS_TKILL                (130u)
#define LX_SYS_TGKILL               (131u)
#define LX_SYS_RT_SIGACTION         (134u)
#define LX_SYS_RT_SIGPROCMASK       (135u)
#define LX_SYS_RT_SIGRETURN         (139u)

/* errno（asm-generic 编号，程序侧读到的是 -errno） */
#define LX_EPERM                    (1)
#define LX_ESRCH                    (3)
#define LX_EINTR                    (4)
#define LX_ENOENT                   (2)
#define LX_EIO                      (5)
#define LX_E2BIG                    (7)
#define LX_ENOEXEC                  (8)
#define LX_EBADF                    (9)
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
#define LX_ENOSYS                   (38)

/* 每个任务可见的 fd 上限；0/1/2 是标准流，不占表项。 */
#define LX_MAX_FD                   (8u)
#define LX_PATH_MAX                 (64u)
#define LX_IO_CHUNK                 (128u)

/* fd 的两类用途。文件类 fd 的 path/size/off 有效；管道类 fd 只用一个
 * 管道下标，读写都落到那块环形缓冲上。 */
#define LX_FD_FILE                  (0u)
#define LX_FD_PIPE_R                (1u)
#define LX_FD_PIPE_W                (2u)

/* 管道容量与同时存在的条数。256 字节够一次 read/write 的常见块，也不至于
 * 让 .bss 多出一大块；条数按「一个程序同时用两条管道」给。
 * 为什么不做阻塞：本内核没有 fork，fd 表也是全局单份，管道两端通常就在
 * 同一个任务里——没有人能在这期间把数据写进来，阻塞就是自死锁。空读/满写
 * 一律回 -EAGAIN，读程序必须自己重试（这与 Linux 的阻塞语义不同，文档里写死）。 */
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
#define LX_O_NONBLOCK               (0x800u)

/* mmap 标志位（asm-generic） */
#define LX_MAP_PRIVATE              (0x02u)
#define LX_MAP_FIXED                (0x10u)
#define LX_MAP_ANONYMOUS            (0x20u)

/* clockid */
#define LX_CLOCK_REALTIME           (0u)
#define LX_CLOCK_MONOTONIC          (1u)
#define LX_CLOCK_MONOTONIC_RAW      (4u)

/* 计数器下标 */
#define LX_STAT_HANDLED             (0u)
#define LX_STAT_ENOSYS              (1u)
#define LX_STAT_EFAULT              (2u)
#define LX_STAT_OTHER               (3u)
#define LX_STAT_MPROTECT            (4u)    /* mprotect 接受但未强制（整窗 RWX） */
#define LX_STAT_MAX                 (5u)

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
    uint8  kind;        /* LX_FD_FILE / LX_FD_PIPE_R / LX_FD_PIPE_W */
    uint8  pipe;        /* kind 是管道时，s_pipe 的下标 */
    uint32 size;
    uint32 off;
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

/* fd 表是内核全局的：本版本的 ELF 装载器一次只跑一个程序，装载器在
 * 每次起跑前调用 arkrtos_linux_abi_reset() 把它清空。 */
static lx_fd_t s_fd[LX_MAX_FD];
/* 管道对象独立于 fd 表：一条管道被两个 fd 引用，fd 关掉一半时管道还要在。 */
static lx_pipe_t s_pipe[LX_PIPE_MAX];
static uint32  s_stat[LX_STAT_MAX];

/* ------------------------------------------------------------- 信号状态 */

typedef struct
{
    unsigned long handler;      /* SIG_DFL(0) / SIG_IGN(1) / 用户函数地址 */
    unsigned long flags;        /* 原样保存，本实现只做「处理期间屏蔽」 */
    unsigned long restorer;     /* 必须非 0：没有 restorer 就没有返回路径 */
    unsigned long mask;         /* 处理期间附加的屏蔽位 */
} lx_sigaction_t;

/* 32 项动作表：1..31 有效，0 号不用。一个任务一份（fd 表也是这个口径：
 * 本版本一次只跑一个 ELF 程序）。 */
static lx_sigaction_t s_sig[LX_NSIG];
static uint32 s_sig_pending;        /* 已收未投递 */
static uint32 s_sig_blocked;        /* 当前屏蔽字 */
static uint32 s_sig_delivered;      /* 真投递进处理函数的次数 */
static uint32 s_sig_ignored;        /* SIG_IGN 或默认忽略 */
static uint32 s_sig_refused;        /* kill 的目标不是自己 */
static uint32 s_sig_no_restorer;    /* 装处理函数时没给 restorer */
static uint32 s_sig_stop_ignored;   /* 默认动作是「挂起」但本内核没有作业控制 */
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

/* 从管道读。空且还有写端 -> -EAGAIN（不阻塞，见 LX_PIPE_BUF 上的注释）；
 * 空且写端全关 -> 0（EOF，与 Linux 一致）。 */
static int32 lx_pipe_read(int32 idx, unsigned long ua, uint32 want)
{
    int32  pi = (int32)s_fd[idx].pipe;
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;

    if((pi < 0) || (pi >= (int32)LX_PIPE_MAX) || (s_pipe[pi].used == 0u))
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
                return (done > 0u) ? (int32)done : -LX_EFAULT;
            }
            s_pipe[pi].tail  = tail;
            s_pipe[pi].count = (uint16)((uint32)s_pipe[pi].count - n);
        }
        done += n;
    }

    if(done == 0u)
    {
        return (s_pipe[pi].writers == 0u) ? 0 : -LX_EAGAIN;
    }
    return (int32)done;
}

/* 往管道写。写端全关但读端还在时没有意义（管道的写端已被关掉），
 * 读端全关 -> -EPIPE；缓冲满 -> -EAGAIN；能写多少写多少，短写是正常结果，
 * 不假装写全。 */
static int32 lx_pipe_write(int32 idx, unsigned long ua, uint32 want)
{
    int32  pi = (int32)s_fd[idx].pipe;
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;

    if((pi < 0) || (pi >= (int32)LX_PIPE_MAX) || (s_pipe[pi].used == 0u))
    {
        return -LX_EBADF;
    }
    if(s_pipe[pi].readers == 0u)
    {
        return -LX_EPIPE;
    }
    if(want == 0u)
    {
        return 0;
    }
    if(ua == 0u)
    {
        return -LX_EFAULT;
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
            return (done > 0u) ? (int32)done : -LX_EFAULT;
        }
        for(k = 0u; k < n; k++)
        {
            s_pipe[pi].buf[s_pipe[pi].head] = buf[k];
            s_pipe[pi].head = (uint16)(((uint32)s_pipe[pi].head + 1u) % (uint32)LX_PIPE_BUF);
            s_pipe[pi].count++;
        }
        done += n;
    }

    if(done == 0u)
    {
        return -LX_EAGAIN;
    }
    return (int32)done;
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
    if((flags & ~(uint32)LX_O_NONBLOCK) != 0u)
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
    s_fd[i1].used     = 1u;
    s_fd[i1].kind     = LX_FD_PIPE_W;
    s_fd[i1].pipe     = (uint8)pi;
    s_fd[i1].size     = 0u;
    s_fd[i1].off      = 0u;
    s_fd[i1].path[0]  = 0;

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
    int32  rc;
    uint32 i;

    rc = lx_copy_path(p_ctx, 1u, path, (uint32)sizeof(path));
    if(rc != 0)
    {
        return rc;
    }

    /* 这台板上的用户态文件系统是只读的（文件由内核侧写进去）。想写就
     * 明确回 -EROFS，而不是给一个"能打开、写不进去"的句柄。 */
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
    s_fd[i].used = 1u;

    return (int32)(3u + i);
}

static int32 lx_close(void *p_ctx)
{
    int32 fd = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    int32 idx;

    if((fd >= 0) && (fd <= 2))
    {
        return 0;       /* 标准流：关掉是空操作，不制造假句柄 */
    }

    idx = lx_fd_find(fd);
    if(idx < 0)
    {
        return -LX_EBADF;
    }

    if(s_fd[idx].kind != LX_FD_FILE)
    {
        int32 pi = (int32)s_fd[idx].pipe;

        if((pi >= 0) && (pi < (int32)LX_PIPE_MAX) && (s_pipe[pi].used != 0u))
        {
            if(s_fd[idx].kind == LX_FD_PIPE_R)
            {
                if(s_pipe[pi].readers != 0u)
                {
                    s_pipe[pi].readers--;
                }
            }
            else
            {
                if(s_pipe[pi].writers != 0u)
                {
                    s_pipe[pi].writers--;
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

    s_fd[idx].used = 0u;
    s_fd[idx].kind = LX_FD_FILE;
    s_fd[idx].pipe = 0u;
    s_fd[idx].off  = 0u;
    s_fd[idx].size = 0u;
    s_fd[idx].path[0] = 0;

    return 0;
}

static int32 lx_read(void *p_ctx)
{
    int32  fd   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 want = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;
    int32  idx;

    if(fd == 0)
    {
        return 0;       /* 没有 stdin：读到 EOF，这不是错误 */
    }

    idx = lx_fd_find(fd);
    if(idx < 0)
    {
        return -LX_EBADF;
    }
    if(s_fd[idx].kind != LX_FD_FILE)
    {
        if(s_fd[idx].kind != LX_FD_PIPE_R)
        {
            return -LX_EBADF;       /* 管道的写端不能读 */
        }
        return lx_pipe_read(idx, ua, want);
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

static int32 lx_write(void *p_ctx)
{
    int32  fd   = (int32)ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long ua = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 want = ARKRTOS_SVC_ARG(p_ctx, 2u);
    uint8  buf[LX_IO_CHUNK];
    uint32 done = 0u;

    if((fd != 1) && (fd != 2))
    {
        int32 pidx = lx_fd_find(fd);

        /* 文件写仍是 -EROFS（openat 就不让可写打开）；这条路只认管道写端。 */
        if((pidx < 0) || (s_fd[pidx].kind != LX_FD_PIPE_W))
        {
            return -LX_EBADF;
        }
        return lx_pipe_write(pidx, ua, want);
    }
    if((ua == 0u) && (want != 0u))
    {
        return -LX_EFAULT;
    }

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
            return (done > 0u) ? (int32)done : -LX_EFAULT;
        }
        for(i = 0u; i < n; i++)
        {
            arkrtos_board_putc((char)buf[i]);
        }
        done += n;
    }

    return (int32)want;
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

/* mprotect(addr, len, prot)：整窗是一块 PS=20 的 RWX 映射，硬件上拆不出
 * 只读/不可执行页。这里**接受并记账**（返回 0），但保护不生效——单独计数
 * 就是为了让"接受"与"强制"在读数时能分开，不冒充有页级保护。 */
static int32 lx_mprotect(void *p_ctx)
{
    unsigned long addr = ARKRTOS_SVC_ARG(p_ctx, 0u);
    unsigned long len  = ARKRTOS_SVC_ARG(p_ctx, 1u);
    unsigned long win_base = (unsigned long)ARKRTOS_ELF_WINDOW_BASE;
    unsigned long win_end  = win_base + (uint32)ARKRTOS_ELF_WINDOW_SIZE;

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
    /* 只认匿名私有映射；MAP_FIXED、文件映射等一律不假装支持。 */
    if(((flags & LX_MAP_ANONYMOUS) == 0u) ||
       ((flags & LX_MAP_PRIVATE) == 0u) ||
       ((flags & LX_MAP_FIXED) != 0u))
    {
        return -LX_ENOSYS;
    }

    if(s_heap_cur == 0u)
    {
        s_heap_cur = base;
    }

    start = lx_align_up(s_heap_cur);
    if(start == 0u)
    {
        return -LX_ENOMEM;
    }
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

    s_heap_cur = end;
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

    /* 窗口是整块静态映射，没有页级取消映射这件事。这里只记账成功，
     * 绝不把地址还给分配器——否则下一次 mmap 会在程序还认为有效的地址
     * 上返回一段"新内存"（静默错答案）。 */
    return 0;
}

/* ------------------------------------------------ execve（同任务换镜像） */

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

    return LX_RET_EXEC_DONE;
}

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
    case LX_SYS_GETPID:
        rc = (int32)arkrtos_current_task_id;
        break;
    case LX_SYS_GETUID:
    case LX_SYS_GETEUID:
        rc = 0;         /* 没有用户模型：唯一身份是 0，不编造 uid */
        break;
    case LX_SYS_SET_TID_ADDRESS:
        /* Linux 把这个指针登记给 robust futex 清理用。本内核不提供 futex，
         * 因此只回报自己的 tid，不保存、也不往用户内存里写一个字
         * （glibc 传进来的就是它自己的 tid 变量，替它清零是副作用）。 */
        rc = (int32)arkrtos_current_task_id;
        break;
    case LX_SYS_EXIT:
    case LX_SYS_EXIT_GROUP:
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

void arkrtos_linux_abi_reset(void)
{
    uint32 i;

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    for(i = 0u; i < (uint32)LX_NR_SLOTS; i++)
    {
        s_nr[i].nr   = 0u;
        s_nr[i].hits = 0u;
    }
    s_nr_used     = 0u;
    s_nr_overflow = 0u;
#endif
    for(i = 0u; i < LX_MAX_FD; i++)
    {
        s_fd[i].used = 0u;
        s_fd[i].size = 0u;
        s_fd[i].off  = 0u;
        s_fd[i].kind = LX_FD_FILE;
        s_fd[i].pipe = 0u;
        s_fd[i].path[0] = 0;
    }

    for(i = 0u; i < LX_PIPE_MAX; i++)
    {
        s_pipe[i].used    = 0u;
        s_pipe[i].readers = 0u;
        s_pipe[i].writers = 0u;
        s_pipe[i].head    = 0u;
        s_pipe[i].tail    = 0u;
        s_pipe[i].count   = 0u;
    }

    /* 信号：动作表、pending、屏蔽字全部回到默认。新程序不该继承上一个
     * 程序的信号处理函数（那是一个已经不存在的代码地址）。 */
    for(i = 0u; i < (uint32)LX_NSIG; i++)
    {
        s_sig[i].handler  = LX_SIG_DFL;
        s_sig[i].flags    = 0UL;
        s_sig[i].restorer = 0UL;
        s_sig[i].mask     = 0UL;
    }
    s_sig_pending = 0u;
    s_sig_blocked = 0u;
    s_sig_delivered = 0u;
    s_sig_ignored = 0u;
    s_sig_refused = 0u;
    s_sig_no_restorer = 0u;
    s_sig_stop_ignored = 0u;

    s_heap_cur = 0u;
}

#endif /* ARKRTOS_USE_LINUX_ABI */
