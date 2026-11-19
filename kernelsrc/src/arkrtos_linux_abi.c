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
*          明确不做的事（文档里逐条列出）：动态链接、fork/execve、信号、
*          fd 继承与管道、socket、futex，以及真 mmap 语义。mmap/brk 只是
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

/* ---------------------------------------------------------------- 常量 */

/* asm-generic 号表：RISC-V 与 LoongArch 的 Linux UAPI 都用这一套。 */
#define LX_SYS_OPENAT               (56u)
#define LX_SYS_CLOSE                (57u)
#define LX_SYS_READ                 (63u)
#define LX_SYS_WRITE                (64u)
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
#define LX_SYS_MMAP                 (222u)

/* errno（asm-generic 编号，程序侧读到的是 -errno） */
#define LX_EPERM                    (1)
#define LX_ENOENT                   (2)
#define LX_EIO                      (5)
#define LX_EBADF                    (9)
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
static uint32  s_stat[4];

/* 堆游标（brk / mmap 共用）。窗口是整块静态映射，所以这里的"分配"只是
 * 在窗口内划出一段不重叠的地址，不建立任何新映射。 */
static uint32  s_heap_cur;

/* ---------------------------------------------------------------- 小工具 */

static void lx_ret(void *p_ctx, int32 value)
{
    arkrtos_port_svc_set_ret_signed(p_ctx, value);
}

/**
* @brief 从用户地址拷一个以 NUL 结尾的路径
* @return 0 成功；-EFAULT 指针非法；-ENAMETOOLONG 没有 NUL
* @details 逐字节过 copy_from_user：它会对每一个字节核对是否落在调用者的
*          窗口内，既不会越界读，也不会把内核地址当用户地址用。
*/
static int32 lx_copy_path(void *p_ctx, uint32 idx, char *out, uint32 out_size)
{
    uint32 uaddr = ARKRTOS_SVC_ARG(p_ctx, idx);
    uint32 i;

    if((uaddr == 0u) || (out_size == 0u))
    {
        return -LX_EFAULT;
    }

    for(i = 0u; i < out_size; i++)
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

static uint32 lx_heap_base(void)
{
    return (uint32)ARKRTOS_ELF_WINDOW_BASE + ((uint32)ARKRTOS_ELF_WINDOW_SIZE / 2u);
}

static uint32 lx_heap_end(void)
{
    /* 窗口顶端是装载器切的栈与参数区（默认 32K + 16K），这里再留 128K，
     * 不与之重叠。 */
    return (uint32)ARKRTOS_ELF_WINDOW_BASE + ((uint32)ARKRTOS_ELF_WINDOW_SIZE
                                              - (128u * 1024u));
}

static uint32 lx_align_up(uint32 v)
{
    uint32 mask = 4095u;

    if(v > (0xFFFFFFFFu - mask))
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
    uint32 ua   = ARKRTOS_SVC_ARG(p_ctx, 1u);
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
    uint32 ua   = ARKRTOS_SVC_ARG(p_ctx, 1u);
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
    uint32 ua  = ARKRTOS_SVC_ARG(p_ctx, 1u);
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
    uint32 ua = ARKRTOS_SVC_ARG(p_ctx, 0u);
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

static int32 lx_brk(void *p_ctx)
{
    uint32 want  = ARKRTOS_SVC_ARG(p_ctx, 0u);
    uint32 base  = lx_heap_base();
    uint32 limit = lx_heap_end();

    if(s_heap_cur == 0u)
    {
        s_heap_cur = base;
    }

    if(want == 0u)
    {
        return (int32)s_heap_cur;
    }
    if((want < base) || (want > limit))
    {
        /* Linux 的约定：失败时返回当前 break，不返回负值。 */
        return (int32)s_heap_cur;
    }

    s_heap_cur = want;
    return (int32)s_heap_cur;
}

static int32 lx_mmap(void *p_ctx)
{
    uint32 len   = ARKRTOS_SVC_ARG(p_ctx, 1u);
    uint32 flags = ARKRTOS_SVC_ARG(p_ctx, 3u);
    uint32 base  = lx_heap_base();
    uint32 limit = lx_heap_end();
    uint32 start;
    uint32 end;

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
    return (int32)start;
}

static int32 lx_munmap(void *p_ctx)
{
    uint32 addr = ARKRTOS_SVC_ARG(p_ctx, 0u);

    if(addr == 0u)
    {
        return -LX_EINVAL;
    }

    /* 窗口是整块静态映射，没有页级取消映射这件事。这里只记账成功，
     * 绝不把地址还给分配器——否则下一次 mmap 会在程序还认为有效的地址
     * 上返回一段"新内存"（静默错答案）。 */
    return 0;
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
    case LX_SYS_CLOCK_GETTIME:
        rc = lx_clock_gettime(p_svc_ctx);
        break;
    case LX_SYS_UNAME:
        rc = lx_uname(p_svc_ctx);
        break;
    case LX_SYS_BRK:
        rc = lx_brk(p_svc_ctx);
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
    if(which >= 4u)
    {
        return 0u;
    }
    return s_stat[which];
}

void arkrtos_linux_abi_reset(void)
{
    uint32 i;

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