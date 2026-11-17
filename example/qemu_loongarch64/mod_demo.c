/**
* @file mod_demo.c
* @brief QEMU LoongArch64 上的可下载模块演示任务
* @details 与 elf_demo.c 并列，但走的是**另一条装载路径**：
*          elf_demo 把 hello.elf（ET_EXEC，地址链接期定死）交给 exec，
*          跑在 PLV3 窗口里；本文件把 modhello.o（ET_REL，没有地址）交给
*          模块装载器，在板上现场链接后跑在 PLV0。
*
*          两次演示都保留、互不影响：它们证明的是两件不同的事，把其中
*          一个去掉就没法对照「两种应用模型」的差别了。
*
*          演示完之后模块文件留在 /mnt/ram/modhello.o，shell 里可以：
*              ld /mnt/ram/modhello.o a b
*              lkup
*              modules
*              unld 0
*
* @note 只在这个示例里存在：别的板子没有 ramfs，也没有"编进去的示例模块"。
* @author xw
*/

#include "arkrtos_config.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_vfs.h"
#include "arkrtos_modld.h"
#include "arkrtos_board.h"
#include "modhello_blob.h"
#if (ARKRTOS_USE_EXEC_SIGN == 1)
#include "modhello_bad_blob.h"
#endif

#if (ARKRTOS_USE_MODLOAD == 1)

#define MOD_DEMO_PATH       "/mnt/ram/modhello.o"
#if (ARKRTOS_USE_EXEC_SIGN == 1)
/* 反例文件：与 modhello.o 同一份内容，只是末尾 tag 的最后一个字节被翻过。 */
#define MOD_DEMO_BAD_PATH   "/mnt/ram/modhello_tampered.o"
#endif

/* 装载器是示例里最深的调用栈（读 ELF、解析节、拷段、逐条重定位、现场查符号），
 * 实测单次 arkrtos_mod_load 需要 4 KB 量级的任务栈；512 字（2 KB）会向下溢出，
 * 打坏紧邻任务的保存帧。这里给 8 KB 并保留余量。 */
#define MOD_DEMO_STACK_WORDS 2048u
#define MOD_DEMO_WAIT_MS    3000u
#define MOD_DEMO_POLL_MS    20u

static uint32 s_mod_demo_stack[MOD_DEMO_STACK_WORDS];

static void mod_demo_puts(const char *s)
{
    arkrtos_board_puts(s);
}

static void mod_demo_putu(uint32 v)
{
    arkrtos_board_putu(v);
}

static void task_mod_demo(void)
{
    int32  rc = -1;
    uint32 tries = 0u;
    uint32 waited = 0u;
    arkrtos_mod_info_t info;
    char  *argv[3];

    /* 等 VFS：卷是另一个任务 format + mount 出来的。写失败就让开 100 ms 重试，
     * 超时明确报"没等到"，不静默跳过。 */
    for (tries = 0u; tries < 100u; tries++)
    {
        rc = arkrtos_vfs_write_file(MOD_DEMO_PATH, ark_example_modhello,
                                    (uint32)ARK_EXAMPLE_MODHELLO_LEN);
        if (rc >= 0)
        {
            break;
        }
        arkrtos_task_wait_internal(100u);
    }

    mod_demo_puts("[MOD] write ");
    mod_demo_puts(MOD_DEMO_PATH);
    mod_demo_puts(" rc=");
    mod_demo_putu((uint32)rc);
    mod_demo_puts(" size=");
    mod_demo_putu((uint32)ARK_EXAMPLE_MODHELLO_LEN);
    if (rc < 0)
    {
        mod_demo_puts(" -> FAIL (VFS never came up; see [FS] lines)\n");
        for (;;)
        {
            arkrtos_task_wait_internal(1000u);
        }
    }
    mod_demo_puts("\n");

    /* argv[0] 用文件名，和真 VxWorks 的 ld 一致（模块自己就是 argv[0]）。 */
    argv[0] = "modhello.o";
    argv[1] = "alpha";
    argv[2] = "42";

    rc = arkrtos_mod_load(MOD_DEMO_PATH, 3, argv);

    mod_demo_puts("[MOD] ld rc=");
    mod_demo_putu((uint32)rc);
    if (rc != ARKRTOS_MOD_OK)
    {
        mod_demo_puts(" (");
        mod_demo_puts(arkrtos_mod_error_name(rc));
        mod_demo_puts(")");
        if ((rc == ARKRTOS_MOD_ERR_SYMBOL) &&
            (arkrtos_mod_missing_symbol()[0] != '\0'))
        {
            mod_demo_puts(" missing=");
            mod_demo_puts(arkrtos_mod_missing_symbol());
        }
        else if (rc == ARKRTOS_MOD_ERR_RELOC)
        {
            mod_demo_puts(" reloc_type=");
            mod_demo_putu(arkrtos_mod_bad_reloc());
        }
        mod_demo_puts("\n");
        for (;;)
        {
            arkrtos_task_wait_internal(1000u);
        }
    }

    if (arkrtos_mod_info(0, &info) == 0)
    {
        mod_demo_puts(" [base=");
        mod_demo_putu(info.base);
        mod_demo_puts(" size=");
        mod_demo_putu(info.size);
        mod_demo_puts(" entry=");
        mod_demo_putu(info.entry);
        mod_demo_puts(" relocs=");
        mod_demo_putu(info.relocs);
        mod_demo_puts(" imports=");
        mod_demo_putu(info.imports);
        mod_demo_puts(" task=");
        mod_demo_putu((uint32)info.task);
        mod_demo_puts("]\n");
    }

    /* 等模块任务跑完，这样 [MOD] 的读数与模块自己的输出按时间顺序读得通。 */
    for (;;)
    {
        if (arkrtos_mod_info(0, &info) != 0)
        {
            break;
        }
        if (info.state == 0)
        {
            break;
        }
        if (waited >= MOD_DEMO_WAIT_MS)
        {
            mod_demo_puts("[MOD] module task still running after ");
            mod_demo_putu(MOD_DEMO_WAIT_MS);
            mod_demo_puts(" ms (see 'modules')\n");
            break;
        }
        arkrtos_task_wait_internal(MOD_DEMO_POLL_MS);
        waited += MOD_DEMO_POLL_MS;
    }

    if (info.state == 0)
    {
        mod_demo_puts("[MOD] module finished after ");
        mod_demo_putu(waited);
        mod_demo_puts(" ms; try: ld ");
        mod_demo_puts(MOD_DEMO_PATH);
        mod_demo_puts(" a b\n");
    }

#if (ARKRTOS_USE_EXEC_SIGN == 1)
    /* 反假绿：模块这条路径同样必须被驱动过。tag 翻一个字节后 ld 必须拒，
     * 而且拒在解析节表之前（否则一个坏 tag 的 .o 会先占槽再报别的错）。 */
    rc = arkrtos_vfs_write_file(MOD_DEMO_BAD_PATH,
                                ark_example_modhello_tampered,
                                (uint32)ARK_EXAMPLE_MODHELLO_TAMPERED_LEN);
    if (rc < 0)
    {
        mod_demo_puts("[MOD] tamper check FAIL (cannot write ");
        mod_demo_puts(MOD_DEMO_BAD_PATH);
        mod_demo_puts(")\n");
    }
    else
    {
        argv[0] = "modhello_tampered.o";
        rc = arkrtos_mod_load(MOD_DEMO_BAD_PATH, 1, argv);
        if (rc == ARKRTOS_MOD_ERR_SIGN)
        {
            mod_demo_puts("[MOD] tamper check PASS (bad tag refused, rc=");
            mod_demo_putu((uint32)rc);
            mod_demo_puts(")\n");
        }
        else
        {
            mod_demo_puts("[MOD] tamper check FAIL (ld accepted a bad tag, rc=");
            mod_demo_putu((uint32)rc);
            mod_demo_puts(")\n");
        }
    }
#endif

    for (;;)
    {
        arkrtos_task_wait_internal(1000u);
    }
}

int32 arkrtos_mod_demo_register(void)
{
    int32 id = arkrtos_task_register(task_mod_demo, s_mod_demo_stack,
                                     (uint32)sizeof(s_mod_demo_stack), 15u, 0u);

    if (id > 0)
    {
        mod_demo_puts("[MOD] demo task id=");
        mod_demo_putu((uint32)id);
        mod_demo_puts(" (writes ");
        mod_demo_puts(MOD_DEMO_PATH);
        mod_demo_puts(" and loads it)\n");
    }
    return id;
}

#endif /* ARKRTOS_USE_MODLOAD */
