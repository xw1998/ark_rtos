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
*          明确不做的事（文档里逐条列出）：fork/clone、信号、父子 fd 继承与
*          管道、socket、futex，以及真 mmap 语义。动态链接的**内核侧**
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

/* errno（asm-generic 编号，程序侧读到的是 -errno） */
#define LX_EPERM                    (1)
#define LX_ENOENT                   (2)
#define LX_EIO                      (5)
#define LX_E2BIG                    (7)
#define LX_ENOEXEC                  (8)
#define LX_EBADF                    (9)
#define LX_EACCES                   (13)
#define LX_ENOMEM                   (12)
#define LX_EFAULT                   (14)
#define LX_EBUSY                    (16)
#define LX_EISDIR                   (21)
#define LX_EINVAL                   (22)
#define LX_ENFILE                   (23)
#define LX_EROFS                    (30)
#define LX_ENAMETOOLONG             (36)
#define LX_ENOSYS                   (38)

/* 每个任务可见的 fd 上限；0/1/2 是标准流，不占表项。 */
#define LX_MAX_FD                   (8u)
#define LX_PATH_MAX                 (64u)
#define LX_IO_CHUNK                 (128u)

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
    uint32 size;
    uint32 off;
    char   path[LX_PATH_MAX];
} lx_fd_t;

/* fd 表是内核全局的：本版本的 ELF 装载器一次只跑一个程序，装载器在
 * 每次起跑前调用 arkrtos_linux_abi_reset() 把它清空。 */
static lx_fd_t s_fd[LX_MAX_FD];
static uint32  s_stat[LX_STAT_MAX];

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

    s_fd[idx].used = 0u;
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
        return -LX_EBADF;       /* 只支持控制台的两个流 */
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

int32 arkrtos_linux_abi_try(void *p_svc_ctx)
{
    uint32 num = arkrtos_port_linux_syscall_num(p_svc_ctx);
    int32  rc  = 0;

    if(num == 0xFFFFFFFFu)
    {
        return 0;       /* 不是 Linux ABI 调用，交回 Ark 号表 */
    }

#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    /* 在分发之前记：走到 default 的 -ENOSYS 与 exit 提前返回的调用
     * 也要进直方图，否则「程序卡在哪一号」恰好在最关键的那几号上
     * 看不见。 */
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

    if(rc == LX_RET_PTR_DONE)
    {
        /* 地址类返回值已由处理函数按整字宽写回，这里不许再覆盖。 */
        s_stat[LX_STAT_HANDLED]++;
        return 1;
    }
    if(rc == LX_RET_EXEC_DONE)
    {
        /* execve 成功：帧已经是新镜像的入口，没有返回值可写（Linux 语义）。 */
        s_stat[LX_STAT_HANDLED]++;
        return 1;
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
        s_fd[i].path[0] = 0;
    }

    s_heap_cur = 0u;
}

#endif /* ARKRTOS_USE_LINUX_ABI */
