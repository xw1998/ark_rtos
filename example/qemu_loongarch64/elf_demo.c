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
#include "hello_blob.h"
#if (ARKRTOS_USE_LINUX_ABI == 1)
#include "linux_hello_blob.h"
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1)
#include "hello_bad_blob.h"
#endif

#if (ARKRTOS_USE_ELF == 1)

#define DEMO_PATH           "/mnt/ram/hello"
#if (ARKRTOS_USE_LINUX_ABI == 1)
#define LX_DEMO_PATH        "/mnt/ram/linux_hello"
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1)
/* 反例文件：与 hello 同一份内容，只是末尾 tag 的最后一个字节被翻过。 */
#define DEMO_BAD_PATH       "/mnt/ram/hello_tampered"
#endif
#define DEMO_STACK_WORDS    1024u
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
