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

#if (ARKRTOS_USE_ELF == 1)

#define DEMO_PATH           "/mnt/ram/hello"
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

    demo_puts("[ELF] exec rc=");
    demo_putu((uint32)rc);
    if (rc != ARKRTOS_ELF_OK)
    {
        demo_puts(" (");
        demo_puts(arkrtos_elf_error_name(rc));
        demo_puts(")\n");
        for (;;)
        {
            arkrtos_task_wait_internal(1000u);
        }
    }
    demo_puts("\n");

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
