/**
* @file elf_demo.c
* @brief QEMU LoongArch64 上的 ELF 装载演示任务
* @details 这条任务只做一件事：把编进镜像里的 hello.elf **当文件**写进
*          ramfs，再走真正的装载路径（VFS 读 → 装进 0x02000000 的 PLV3
*          窗口 → 降权任务起跑）。装载器读的是文件，不是数组；数组只是
*          QEMU 没有宿主文件注入通道时把文件送到板子上的手段。
*
*          演示完以后 hello 留在 /mnt/ram/hello，shell 里可以直接再跑：
*              exec /mnt/ram/hello a b c
*              /mnt/ram/hello a b c        （以 / 开头的词当程序）
*
* @note 只在这个示例里存在：别的板子没有 ramfs，也没有"编进去的示例程序"。
*       它不参与内核，也不改任何内核判据。
* @author xw
*/

#include "arkrtos_config.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_vfs.h"
#include "arkrtos_elf.h"
#include "arkrtos_board.h"
/* 隔离执行（SEC-G08）那一段用 ARKRTOS_USE_USHELL_ISO 判是否编进演示。
 * 这个开关的推导只有一处（PRIV + PAGING + PAGING_PAGES + ELF），所以直接
 * 包含内核内部头，不在示例里另抄一份判据——抄一份就会在两处漂移。 */
#include "arkrtos_ushell_upcall.h"
#include "hello_blob.h"
#if (ARKRTOS_USE_LINUX_ABI == 1)
#include "linux_hello_blob.h"
#include "linux_execve_blob.h"
#include "linux_pipe_blob.h"
#include "linux_dup_blob.h"
#include "linux_signal_blob.h"
#if (ARKRTOS_USE_LINUX_FORK == 1)
#include "linux_fork_blob.h"
#endif
#include "arkrtos_linux_abi.h"
#endif
#if (ARKRTOS_USE_ELF_TLS == 1)
#include "tls_main_blob.h"
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1)
#include "hello_bad_blob.h"
#endif
#if (ARKRTOS_USE_USHELL_ISO == 1)
#include "ushell_iso_blob.h"
#endif
#if (ARKRTOS_USE_DYNLOAD == 1)
#include "ld_ark_blob.h"
#include "libanswer_blob.h"
#include "dynmain_blob.h"
#include "dynmain_exec_blob.h"
#include "dynmain_bad_blob.h"
#endif

#if (ARKRTOS_USE_ELF == 1)

/* 压力档：同一台板上反复跑「fork 一个子进程 -> 收尸」这条路。
 * 1（默认）= 只跑一次，输出与行为一个字节都不变；>1 时每个子进程退出后
 * 立刻在同一批空间/帧池上再来一轮，把「子进程退出后空间回收」与
 * 「父进程共享页降只读」两条路径压到重复次数上（就是它们出过问题）。
 * 由 -DDEMO_FORK_REPEAT=<n> 打开，不写进 Kconfig。 */
#ifndef DEMO_FORK_REPEAT
#define DEMO_FORK_REPEAT    1
#endif

#define DEMO_PATH           "/mnt/ram/hello"
#if (ARKRTOS_USE_LINUX_ABI == 1)
#define LX_DEMO_PATH        "/mnt/ram/linux_hello"
#define LX_EXEC_PATH        "/mnt/ram/linux_execve"
#define LX_PIPE_PATH        "/mnt/ram/linux_pipe"
#define LX_DUP_PATH         "/mnt/ram/linux_dup"
#define LX_SIG_PATH         "/mnt/ram/linux_signal"
#if (ARKRTOS_USE_LINUX_FORK == 1)
#define LX_FORK_PATH        "/mnt/ram/linux_fork"
#endif
#endif
#if (ARKRTOS_USE_ELF_TLS == 1)
#define TLS_DEMO_PATH       "/mnt/ram/tls_main"
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1)
/* 反例文件：与 hello 同一份内容，只是末尾 tag 的最后一个字节被翻过。 */
#define DEMO_BAD_PATH       "/mnt/ram/hello_tampered"
#endif
#if (ARKRTOS_USE_DYNLOAD == 1)
/* 动态链接这一组的四个文件：ld.so 与库是 ld.so 自己去 /mnt/ram/ 读的，
 * 主程序才是内核 exec 的那个（它的 PT_INTERP 指向 /mnt/ram/ld-ark.so）。 */
#define DYN_LDSO_PATH       "/mnt/ram/ld-ark.so"
#define DYN_LIB_PATH        "/mnt/ram/libanswer.so"
#define DYN_MAIN_PATH       "/mnt/ram/dynmain"
#define DYN_EXEC_PATH       "/mnt/ram/dynmain_exec"
/* 反假绿夹具：它的 JUMP_SLOT 指向库里根本不存在的 no_such_symbol，
 * 要求 ld.so 明确失败并以 127 退出，不允许跑到 PASS。 */
#define DYN_BAD_PATH        "/mnt/ram/dynmain_bad"
#endif
#if (ARKRTOS_USE_USHELL_ISO == 1)
#define USHELL_ISO_PATH     "/mnt/ram/ushell_iso"
/* 只等"起跑并注册"，不等退出——这个 App 是常驻的。 */
#define USHELL_ISO_SETTLE_MS 500u
#endif
/* 演示任务的栈：它自己就是跑装载器的那个任务 —— exec 一次要把 ELF 头、
 * 段表、interp 路径与整段 23 KB 的夹具文件写穿 VFS/littlefs，峰值栈用量
 * 实测（图案扫描，2026-11-21，QEMU LA64）：默认档 3980 B、dyn 演示档
 * 4172 B。原来给 4 KB 时只剩不到 120 B 余量，dyn 档多出来的那几个调用
 * 直接把栈底保护字踩掉（[STK] task 7），任务被守卫摘掉、后两轮 dyn 演示
 * 不再执行。8 KB 留一倍余量。改大小之前先量一遍，别按感觉调。 */
#define DEMO_STACK_WORDS    2048u
#define DEMO_WAIT_MS        3000u
#define DEMO_POLL_MS        20u

static uint32 s_demo_stack[DEMO_STACK_WORDS];

static void demo_puts(const char *s)
{
    arkrtos_board_puts(s);
}

static void demo_putu(uint32 v)
{
    arkrtos_board_putu(v);
}

/* 一行读数必须一次写完：否则刚起来的用户任务会在半行处抢走 CPU，日志里
 * 就剩半个 "[l"，判据会把「输出被打断」误读成「没打印」。锁只包住打印
 * 本身（UART 轮询），不包装载/等待那些长路径。 */
static void demo_lock(void)
{
    arkrtos_sched_lock_internal();
}

static void demo_unlock(void)
{
    (void)arkrtos_sched_unlock_internal();
}

#if (ARKRTOS_USE_DYNLOAD == 1)
/* 写一个动态链接夹具文件。与 hello 那条路同一个 VFS 写口，同样退让重试，
 * 失败就明确报"没等到"，不静默跳过。 */
static int32 dyn_write(const char *path, const unsigned char *blob, uint32 len)
{
    int32 rc = -1;
    uint32 tries;

    for (tries = 0u; tries < 100u; tries++)
    {
        rc = arkrtos_vfs_write_file(path, blob, len);
        if (rc >= 0)
        {
            break;
        }
        arkrtos_task_wait_internal(100u);
    }

    demo_lock();
    demo_puts("[dyn] write ");
    demo_puts(path);
    demo_puts(" rc=");
    demo_putu((uint32)rc);
    demo_puts(" size=");
    demo_putu(len);
    demo_puts("\n");
    demo_unlock();

    return (rc >= 0) ? 0 : -1;
}

/* exec 一个动态链接程序，等它跑完，再把内核侧的读数与退出码打出来。
 * 期望退出码 105 是 dynmain.c 里"三种重定位全部成立"的约定值。 */
static void dyn_run(const char *path, const char *name)
{
    arkrtos_elf_info_t info;
    char *argv[2];
    uint32 waited = 0u;
    int32  rc;

    argv[0] = (char *)name;
    argv[1] = (char *)0;

    rc = arkrtos_elf_exec(path, 1, argv);

    demo_lock();
    demo_puts("[dyn] exec ");
    demo_puts(name);
    demo_puts(" rc=");
    demo_putu((uint32)rc);
    demo_puts("\n");
    demo_unlock();
    if (rc != ARKRTOS_ELF_OK)
    {
        return;
    }

    if (arkrtos_elf_info(&info) == ARKRTOS_ELF_OK)
    {
        demo_lock();
        demo_puts("[dyn] info dynamic=");
        demo_putu(info.dynamic);
        demo_puts(" interp_base=");
        demo_putu(info.interp_base);
        demo_puts(" interp_entry=");
        demo_putu(info.interp_entry);
        demo_puts("\n");
        demo_unlock();
    }

    while ((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
    {
        arkrtos_task_wait_internal(DEMO_POLL_MS);
        waited += DEMO_POLL_MS;
    }

    demo_lock();
    demo_puts("[dyn] finished after ");
    demo_putu(waited);
    demo_puts(" ms, exit code ");
#if (ARKRTOS_USE_EXIT_CODE == 1)
    if ((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
        (info.exit_code_set != 0u))
    {
        demo_putu((uint32)info.exit_code);
    }
    else
    {
        demo_puts("(not reported)");
    }
#else
    demo_puts("(exit code channel off)");
#endif
    demo_puts("\n");
    demo_unlock();
}
#endif /* ARKRTOS_USE_DYNLOAD */

#if (ARKRTOS_USE_USHELL_ISO == 1)
/* 隔离执行演示（SEC-G08）：把 ushell_iso.elf 写进 ramfs 再走正常装载路径
 * 起起来，然后**不等它结束**。它只做一件事：向内核注册一条用户态命令
 * `hi`，成功后常驻等回调。App 一旦退出，空间与被注册的命令会一起被卸掉，
 * 回调就再也触发不了——所以这里必须留它活着。
 * 真正的读数由 tools/ushell_iso_gate.py 在 shell 里发 `hi a b` 取得：
 * App 侧要打 argc/argv/函数地址/栈指针，内核侧要打 isolated rc/tid/space/plv。
 * @note 演示任务只负责"送文件 + 起跑"，不参与任何判据。 */
static void ushell_iso_run(void)
{
    char *argv[2];
    int32 rc = -1;
    uint32 tries;
    uint32 waited = 0u;

    for (tries = 0u; tries < 100u; tries++)
    {
        rc = arkrtos_vfs_write_file(USHELL_ISO_PATH, ark_example_ushell_iso,
                                    (uint32)ARK_EXAMPLE_USHELL_ISO_LEN);
        if (rc >= 0)
        {
            break;
        }
        arkrtos_task_wait_internal(100u);
    }

    demo_lock();
    demo_puts("[ushell] write ");
    demo_puts(USHELL_ISO_PATH);
    demo_puts(" rc=");
    demo_putu((uint32)rc);
    demo_puts(" size=");
    demo_putu((uint32)ARK_EXAMPLE_USHELL_ISO_LEN);
    demo_puts("\n");
    demo_unlock();
    if (rc < 0)
    {
        return;
    }

    argv[0] = (char *)"ushell_iso";
    argv[1] = (char *)0;

    rc = arkrtos_elf_exec(USHELL_ISO_PATH, 1, argv);
    demo_lock();
    demo_puts("[ushell] exec rc=");
    demo_putu((uint32)rc);
    demo_puts("\n");
    demo_unlock();
    if (rc != ARKRTOS_ELF_OK)
    {
        return;
    }

    /* 只等到它打出注册结果，不当成"跑完"：常驻 App 的 running 一直是 1，
     * 这里必然走超时分支。 */
    while ((arkrtos_elf_running() != 0) && (waited < USHELL_ISO_SETTLE_MS))
    {
        arkrtos_task_wait_internal(DEMO_POLL_MS);
        waited += DEMO_POLL_MS;
    }

    demo_lock();
    demo_puts("[ushell] app resident after ");
    demo_putu(waited);
    demo_puts(" ms (try: hi a b)\n");
    demo_unlock();
}
#endif /* ARKRTOS_USE_USHELL_ISO */

static void task_elf_demo(void)
{
    int32  rc = -1;
    uint32 tries = 0u;
    uint32 waited = 0u;
    char  *argv[3];

    /* 等 VFS：littlefs 卷是另一个任务 format + mount 出来的，这里不能假设
     * 它就绪。写失败就退让 100 ms 重试，超时明确报"没等到"，不静默跳过。 */
    for (tries = 0u; tries < 100u; tries++)
    {
        rc = arkrtos_vfs_write_file(DEMO_PATH, ark_example_hello,
                                    (uint32)ARK_EXAMPLE_HELLO_LEN);
        if (rc >= 0)
        {
            break;
        }
        arkrtos_task_wait_internal(100u);
    }

    demo_puts("[ELF] write ");
    demo_puts(DEMO_PATH);
    demo_puts(" rc=");
    demo_putu((uint32)rc);
    demo_puts(" size=");
    demo_putu((uint32)ARK_EXAMPLE_HELLO_LEN);
    if (rc < 0)
    {
        demo_puts(" -> FAIL (VFS never came up; see [FS] lines)\n");
        for (;;)
        {
            arkrtos_task_wait_internal(1000u);
        }
    }
    demo_puts("\n");

    /* Linux 的 argv[0] 就是程序名，这里照抄：argv[0]="hello"。 */
    argv[0] = "hello";
    argv[1] = "world";
    argv[2] = "42";

    rc = arkrtos_elf_exec(DEMO_PATH, 3, argv);

    demo_lock();
    demo_puts("[ELF] exec rc=");
    demo_putu((uint32)rc);
    if (rc != ARKRTOS_ELF_OK)
    {
        demo_puts(" (");
        demo_puts(arkrtos_elf_error_name(rc));
        demo_puts(")\n");
        demo_unlock();
        for (;;)
        {
            arkrtos_task_wait_internal(1000u);
        }
    }
    demo_puts("\n");
    demo_unlock();

    while (arkrtos_elf_running() != 0)
    {
        if (waited >= DEMO_WAIT_MS)
        {
            demo_puts("[ELF] demo program still running after ");
            demo_putu(DEMO_WAIT_MS);
            demo_puts(" ms\n");
            break;
        }
        arkrtos_task_wait_internal(DEMO_POLL_MS);
        waited += DEMO_POLL_MS;
    }

    demo_puts("[ELF] demo program finished after ");
    demo_putu(waited);
    demo_puts(" ms; try: exec ");
    demo_puts(DEMO_PATH);
    demo_puts(" a b c\n");

#if (ARKRTOS_USE_EXEC_SIGN == 1)
    /* 反假绿：签名门开着这件事必须是**被驱动过的**。
     * 这里走的是与上面完全相同的装载路径（写文件 -> exec），区别只是这份
     * 副本的 tag 被翻了一个字节：装载器必须在读 ELF 头之前就把它拒掉。
     * 若这里返回 0，窗口会被这个坏文件占住，下面那句 PASS 也就不会出现。 */
    rc = arkrtos_vfs_write_file(DEMO_BAD_PATH, ark_example_hello_tampered,
                                (uint32)ARK_EXAMPLE_HELLO_TAMPERED_LEN);
    if (rc < 0)
    {
        demo_puts("[ELF] tamper check FAIL (cannot write ");
        demo_puts(DEMO_BAD_PATH);
        demo_puts(")\n");
    }
    else
    {
        argv[0] = "hello_tampered";
        rc = arkrtos_elf_exec(DEMO_BAD_PATH, 1, argv);
        if (rc == ARKRTOS_ELF_ERR_SIGN)
        {
            demo_puts("[ELF] tamper check PASS (bad tag refused, rc=");
            demo_putu((uint32)rc);
            demo_puts(")\n");
        }
        else
        {
            demo_puts("[ELF] tamper check FAIL (exec accepted a bad tag, rc=");
            demo_putu((uint32)rc);
            demo_puts(")\n");
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1)
    /* Linux 系统调用 ABI 的现场证据：同一台板、同一条装载路径，换一个只发
     * Linux 号的程序。三行读数缺一不可——写文件成功、exec 起跑、程序自己
     * 打印 PASS 并带回 exit code 7。 */
    {
        arkrtos_elf_info_t info;

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_DEMO_PATH, ark_example_linux_hello,
                                        (uint32)ARK_EXAMPLE_LINUX_HELLO_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[linux] write ");
        demo_puts(LX_DEMO_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts("\n");

        if(rc >= 0)
        {
            argv[0] = "linux_hello";
            rc = arkrtos_elf_exec(LX_DEMO_PATH, 1, argv);

            demo_lock();
            demo_puts("[linux] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[linux] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1)
    /* execve 的现场证据：linux_execve 的 stage1 用 execve(221) 把**自己**换成
     * 同一份文件的 stage2，然后验证四件事——任务号不变、fd 保留、堆游标归零、
     * .data 回到文件初值。判据是退出码 42；任何一格不对都是 1。
     * 反假绿在夹具内部：stage1 先 execve 一个不存在的路径，必须回 -ENOENT
     * 且程序继续跑——失败是可返回的，这条路径上"看起来成功"会立刻把 42
     * 变成 1（之后 stage1 打印 FAIL 并 exit 1）。 */
    {
        arkrtos_elf_info_t info;
        char *xargv[1];

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_EXEC_PATH, ark_example_linux_execve,
                                        (uint32)ARK_EXAMPLE_LINUX_EXECVE_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[execve] write ");
        demo_puts(LX_EXEC_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_LINUX_EXECVE_LEN);
        demo_puts("\n");

        if(rc >= 0)
        {
            /* argv[0] 必须是真路径：夹具用它 openat 拿 fd，也用它 execve 自己。 */
            xargv[0] = (char *)LX_EXEC_PATH;
            rc = arkrtos_elf_exec(LX_EXEC_PATH, 1, xargv);

            demo_lock();
            demo_puts("[execve] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[execve] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#if (ARKRTOS_USE_LINUX_ABI == 1)
    /* 管道的现场证据：linux_pipe 自己建一条管道，写读一轮后逐字节比对，
     * 再验四条失败面——空读 -EAGAIN、写端关掉读 EOF、O_CLOEXEC 被拒、
     * 满时短写与读端关掉回 -EPIPE。判据是退出码 55；任一条 check 不对，
     * 夹具自己把退出码变成 1，不会把失败的实现读成"看起来跑了"。 */
    {
        arkrtos_elf_info_t info;
        char *pargv[1];

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_PIPE_PATH, ark_example_linux_pipe,
                                        (uint32)ARK_EXAMPLE_LINUX_PIPE_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[pipe] write ");
        demo_puts(LX_PIPE_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_LINUX_PIPE_LEN);
        demo_puts("\n");

        if(rc >= 0)
        {
            pargv[0] = "linux_pipe";
            rc = arkrtos_elf_exec(LX_PIPE_PATH, 1, pargv);

            demo_lock();
            demo_puts("[pipe] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[pipe] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1)
    /* dup / dup3 的现场证据：linux_dup 建一条管道、dup 一份写端、经副本写、
     * 从原读端读；再 dup3 把 stdout 指到管道、close(1) 撤销；最后 dup3 覆盖
     * 一个在用的 fd，并验 dup(0) 的控制台引用与四条失败面。判据是退出码 56；
     * 任一条 check 不对，夹具自己把退出码变成 1，不会把失败的实现读成
     * "看起来跑了"。 */
    {
        arkrtos_elf_info_t info;
        char *pargv[1];

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_DUP_PATH, ark_example_linux_dup,
                                        (uint32)ARK_EXAMPLE_LINUX_DUP_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[dup] write ");
        demo_puts(LX_DUP_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_LINUX_DUP_LEN);
        demo_puts("\n");

        if(rc >= 0)
        {
            pargv[0] = "linux_dup";
            rc = arkrtos_elf_exec(LX_DUP_PATH, 1, pargv);

            demo_lock();
            demo_puts("[dup] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[dup] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1)
    /* 信号的现场证据：linux_signal 自己装处理函数、kill 自己、看处理函数有没有
     * 在系统调用返回前跑、sigreturn 有没有把 kill 的返回值装回 a0、能不能嵌套、
     * 屏蔽期间不投递而解除后立即投递、六种非法参数各回什么、SIG_IGN 是否真静音。
     * 最后它把 SIGUSR1 还原成 SIG_DFL 再 kill 自己，所以**正常收尾的退出码是
     * 138**（128 + 10），不是 0；夹具自检失败则走 exit 1。 */
    {
        arkrtos_elf_info_t info;
        char *pargv[1];

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_SIG_PATH, ark_example_linux_signal,
                                        (uint32)ARK_EXAMPLE_LINUX_SIGNAL_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[sig] write ");
        demo_puts(LX_SIG_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_LINUX_SIGNAL_LEN);
        demo_puts("\n");

        if(rc >= 0)
        {
            pargv[0] = "linux_signal";
            rc = arkrtos_elf_exec(LX_SIG_PATH, 1, pargv);

            demo_lock();
            demo_puts("[sig] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[sig] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_FORK == 1) && (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* U-21 的探测文件：文件映射要读的内容不可能由夹具自己写出来（它一被
     * exec 就要读），所以由内核侧先落进 ramfs。5000 字节，第 i 字节 =
     * 'A' + (i % 26)：规律式内容让夹具可以逐位置核对「读到的是文件的第几个
     * 字节」，而不是只比首字节 —— 只比首字节的话，把 offset 吃掉也能过。 */
    {
        static uint8 s_fm_probe[5000];
        uint32 i;

        for(i = 0u; i < 5000u; i++)
        {
            s_fm_probe[i] = (uint8)('A' + (i % 26u));
        }
        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file("/mnt/ram/filemap", s_fm_probe, 5000u);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }
        demo_puts("[filemap] write /mnt/ram/filemap rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=5000\n");
    }
#endif

#if (ARKRTOS_USE_LINUX_FORK == 1)
    /* fork 的现场证据：linux_fork 自己 clone 出一个子进程 —— 子进程改自己那份
     * .data 后 exit(42)，父进程用 wait4(WNOHANG) 收它的退出码，再确认父的全局
     * 变量没被改、记账已经被消耗掉。夹具自检失败走 exit 1，正常收尾是 exit 0。 */
    {
        arkrtos_elf_info_t info;
        char *fargv[1];

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(LX_FORK_PATH, ark_example_linux_fork,
                                        (uint32)ARK_EXAMPLE_LINUX_FORK_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[fork] write ");
        demo_puts(LX_FORK_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_LINUX_FORK_LEN);
        demo_puts("\n");

        {
            int32  wrc = rc;        /* write 的结果：循环里不能被 exec 覆盖 */
            uint32 fk;

            for(fk = 0u; (wrc >= 0) && (fk < (uint32)DEMO_FORK_REPEAT); fk++)
            {
                int32 erc;

                if(fk != 0u)
                {
                    /* 压力档才有的一行：默认档一个字节都不多打。 */
                    demo_lock();
                    demo_puts("[fork] iter=");
                    demo_putu(fk + 1u);
                    demo_puts("\n");
                    demo_unlock();
                }

                fargv[0] = "linux_fork";
                erc = arkrtos_elf_exec(LX_FORK_PATH, 1, fargv);

                demo_lock();
                demo_puts("[fork] exec rc=");
                demo_putu((uint32)erc);
                demo_puts("\n");
                demo_unlock();

                if(erc == ARKRTOS_ELF_OK)
                {
                    waited = 0u;
                    while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                    {
                        arkrtos_task_wait_internal(DEMO_POLL_MS);
                        waited += DEMO_POLL_MS;
                    }

                    demo_lock();
                    demo_puts("[fork] finished after ");
                    demo_putu(waited);
                    demo_puts(" ms, exit code ");
                    #if (ARKRTOS_USE_EXIT_CODE == 1)
                    if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                       (info.exit_code_set != 0u))
                    {
                        demo_putu((uint32)info.exit_code);
                    }
                    else
                    {
                        demo_puts("(not reported)");
                    }
                    #else
                    demo_puts("(exit code channel off)");
                    #endif
                    demo_puts("\n");
                    demo_unlock();
                }
                else
                {
                    break;
                }
            }
        }
    }
#endif
#endif

#if (ARKRTOS_USE_ELF_TLS == 1) && (ARKRTOS_USE_LINUX_ABI == 1)
    /* PT_TLS 的现场证据：同一台板、同一条装载路径，换一个带 __thread 变量的
     * 程序。它自己只用两个 syscall（write 64 / exit 93），所以配 Linux ABI
     * 档一起跑；两档都在时才有这一行读数。
     *
     * 判据是退出码 3：程序读 .tdata（0x5A5A）与写读 .tbss（0x11）都成功。
     * 这两件事都要求装载器把 $tp 指到 TLS 块起始地址——TP 还是 0 的话，
     * 第一条 TLS 访问就落在未映射地址上，根本到不了 exit 3。 */
    {
        arkrtos_elf_info_t info;

        for(tries = 0u; tries < 100u; tries++)
        {
            rc = arkrtos_vfs_write_file(TLS_DEMO_PATH, ark_example_tls_main,
                                        (uint32)ARK_EXAMPLE_TLS_MAIN_LEN);
            if(rc >= 0)
            {
                break;
            }
            arkrtos_task_wait_internal(100u);
        }

        demo_puts("[tls] write ");
        demo_puts(TLS_DEMO_PATH);
        demo_puts(" rc=");
        demo_putu((uint32)rc);
        demo_puts(" size=");
        demo_putu((uint32)ARK_EXAMPLE_TLS_MAIN_LEN);
        demo_puts("\n");

        if(rc >= 0)
        {
            argv[0] = "tls_main";
            rc = arkrtos_elf_exec(TLS_DEMO_PATH, 1, argv);

            demo_lock();
            demo_puts("[tls] exec rc=");
            demo_putu((uint32)rc);
            demo_puts("\n");
            demo_unlock();

            if(rc == ARKRTOS_ELF_OK)
            {
                waited = 0u;
                while((arkrtos_elf_running() != 0) && (waited < DEMO_WAIT_MS))
                {
                    arkrtos_task_wait_internal(DEMO_POLL_MS);
                    waited += DEMO_POLL_MS;
                }

                demo_lock();
                demo_puts("[tls] finished after ");
                demo_putu(waited);
                demo_puts(" ms, exit code ");
                #if (ARKRTOS_USE_EXIT_CODE == 1)
                if((arkrtos_elf_info(&info) == ARKRTOS_ELF_OK) &&
                   (info.exit_code_set != 0u))
                {
                    demo_putu((uint32)info.exit_code);
                }
                else
                {
                    demo_puts("(not reported)");
                }
                #else
                demo_puts("(exit code channel off)");
                #endif
                demo_puts("\n");
                demo_unlock();
            }
        }
    }
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    /* 按号直方图的现场读数。为什么打在演示里而不是只在 shell 里：
     * 这个板的读数全是「启动后被自己跑一遍」拿到的，没有交互式输入；
     * 直方图如果不落进串口，就只是一个没人验过的函数。
     * 行格式刻意做成机器可读：`[lxstat] nr <号> x<次数>`。 */
    {
        uint32 i;
        uint32 slots = arkrtos_linux_abi_nr_slots();

        demo_lock();
        demo_puts("[lxstat] slots=");
        demo_putu(slots);
        demo_puts(" overflow=");
        demo_putu(arkrtos_linux_abi_nr_overflow());
        demo_puts("\n");
        for(i = 0u; i < slots; i++)
        {
            uint32 nr = 0u;
            uint32 hits = arkrtos_linux_abi_nr_slot(i, &nr);

            demo_puts("[lxstat] nr ");
            demo_putu(nr);
            demo_puts(" x");
            demo_putu(hits);
            demo_puts("\n");
        }
        demo_unlock();
    }
#endif

#if (ARKRTOS_USE_DYNLOAD == 1)
    /* Linux 动态链接（PT_INTERP + ld.so）的现场证据：同一台板、同一条装载
     * 路径，exec 一个 PT_INTERP 指向 /mnt/ram/ld-ark.so 的程序。内核应该
     * 先把解释器装到 0x02100000、把入口换成 ld.so，ld.so 再自己 mmap 装库、
     * 做三种重定位，最后跳回主程序。四行读数缺一不可：
     *   write 四个文件成功 / info 里 dynamic=1 且 interp_base=0x02100000 /
     *   ld.so 自己打的 [ldso] 行 / 程序退出码 105。
     * 两个变体各跑一遍：dynmain 是 ET_DYN（装载基址 0x02000000），
     * dynmain_exec 是 ET_EXEC（vaddr 就是绝对地址，走另一条 bias 分支）。 */
    if ((dyn_write(DYN_LDSO_PATH, ark_example_ld_ark,
                   (uint32)ARK_EXAMPLE_LD_ARK_LEN) == 0) &&
        (dyn_write(DYN_LIB_PATH, ark_example_dynlib,
                   (uint32)ARK_EXAMPLE_DYNLIB_LEN) == 0) &&
        (dyn_write(DYN_MAIN_PATH, ark_example_dynmain,
                   (uint32)ARK_EXAMPLE_DYNMAIN_LEN) == 0) &&
        (dyn_write(DYN_EXEC_PATH, ark_example_dynmain_exec,
                   (uint32)ARK_EXAMPLE_DYNMAIN_EXEC_LEN) == 0))
    {
        dyn_run(DYN_MAIN_PATH, "dynmain");
        dyn_run(DYN_EXEC_PATH, "dynmain_exec");
        /* 反假绿：故意把 JUMP_SLOT 指向库里没有的符号，期望
         * [ldso] FAIL: unresolved PLT symbol no_such_symbol + 退出码 127，
         * 且绝不出现 [dyn] PASS。 */
        if (dyn_write(DYN_BAD_PATH, ark_example_dynmain_bad,
                      (uint32)ARK_EXAMPLE_DYNMAIN_BAD_LEN) == 0)
        {
            dyn_run(DYN_BAD_PATH, "dynmain_bad");
        }
        else
        {
            demo_puts("[dyn] FAIL: cannot stage dynmain_bad\n");
        }
    }
    else
    {
        demo_puts("[dyn] FAIL: cannot stage the fixtures in /mnt/ram/\n");
    }
#endif

#if (ARKRTOS_USE_USHELL_ISO == 1)
    /* 隔离执行（SEC-G08）：放最后一段，只起不等待。前面的演示都会等自己
     * 的子程序退出，这一段起的是常驻 App，等它就等不到了。 */
    ushell_iso_run();
#endif

    /* 演示结束，本任务留着空转（不自杀：示例里的任务死掉会让 'task' 表
     * 少一行，看起来像泄漏，其实是设计）。 */
    for (;;)
    {
        arkrtos_task_wait_internal(1000u);
    }
}

int32 arkrtos_elf_demo_register(void)
{
    int32 id = arkrtos_task_register(task_elf_demo, s_demo_stack,
                                     (uint32)sizeof(s_demo_stack), 14u, 0u);

    if (id > 0)
    {
        demo_puts("[ELF] demo task id=");
        demo_putu((uint32)id);
        demo_puts(" (writes ");
        demo_puts(DEMO_PATH);
        demo_puts(" and runs it)\n");
    }
    return id;
}

#endif /* ARKRTOS_USE_ELF */
