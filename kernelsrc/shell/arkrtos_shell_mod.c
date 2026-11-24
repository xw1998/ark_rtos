/**
* @file arkrtos_shell_mod.c
* @brief 可执行与可下载模块：exec / wait / ld / lkup / modules / unld。
*          两条模型：exec 跑链接期定好地址的 ET_EXEC（PLV3 隔离），
*          ld 跑板上现场链接的 ET_REL（PLV0，直接调内核 API）。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"
#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif

#if (SHELL_ENABLE == 1)

#if (ARKRTOS_USE_ELF == 1)
/* ============================================================
 * exec：把 ELF64 可执行文件当命令跑
 * @details 与 app run 的区别：那条走裸镜像 + 固定入口约定；这条走标准
 *          ELF64，入口按 Linux 约定（sp 指向 argc/argv/NULL/envp NULL），
 *          所以一个普通 -static 程序不需要为 Ark 改一行。
 *          U-2（2026-11-17）之后装载器会把每个程序放进一个空地址空间，
 *          所以这里不再自己判"窗口是否被占"——那件事由装载器按空间台账回答，
 *          全部空间都在用时它返回 ARKRTOS_ELF_ERR_BUSY。
 * ============================================================ */

/* 等程序结束的上限。程序自己卡死时不能让 shell 永远不回来，但也不能
 * 杀掉它——那是用户的东西。到点就交还提示符，用 'task' 能看到还在跑。 */
#define SHELL_EXEC_WAIT_MS     30000u
#define SHELL_EXEC_POLL_MS     20u

static int shell_exec_file(const char *path, int argc, char *argv[])
{
    arkrtos_elf_info_t info;
    int32 rc;
    uint32 waited = 0u;
    uint32 space = 0u;
    uint32 task_id = 0u;

    rc = arkrtos_elf_exec_space(path, argc, argv, &space);
    if (rc != ARKRTOS_ELF_OK)
    {
        /* 错误名来自装载器，不在 shell 里另写一套文案：同一个 rc 在两处
         * 说法不一致时，读数就没法对账了。BUSY 的含义现在是"所有地址空间
         * 都有程序在跑"，不是"窗口只有一块"。 */
        ark_shell_printf("\r\n%s: %s (rc=%d)\r\n", path,
                         arkrtos_elf_error_name(rc), (int)rc);
        return 1;
    }

    if (arkrtos_elf_info_space(space, &info) == 0)
    {
        task_id = (uint32)info.task;
        /* entry/sp 是 unsigned long：宽 VA 档的窗口基址在 0x1_2000_0000 以上，
         * 用 %08X 会把它截成低 32 位（实测真值是 0x1200008E0，打出来却是
         * 0x200008E0）——读数是假的，不是装载器装错了。这里按原生字宽打。*/
        ark_shell_printf("\r\n[ELF] %s: %u B, %u segment(s), entry 0x%016lX, "
                         "sp 0x%016lX, argc %u, task %d, space %u\r\n",
                         path, (unsigned)info.bytes, (unsigned)info.segs,
                         (unsigned long)info.entry, (unsigned long)info.sp,
                         (unsigned)info.argc, (int)info.task, (unsigned)space);
    }

#if (ARKRTOS_USE_AUDIT_LOG == 1)
    /* "This image ran." Only the launch site knows the task id, and a trail
     * that names a guessed task is worse than one that names none, so 0 means
     * "the info read failed" rather than a plausible-looking number. */
    (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_START,
                                   (uint32)arkrtos_current_task_id, task_id);
#endif

    /* 等它跑完。必须让出 CPU 而不是忙等：本函数在 shell 任务里，忙等期间
     * 同优先级上下的程序任务拿不到时间片的概率全看调度器，读数会飘。
     * 等的只是这个空间里的那个任务：别的空间里还有程序在跑时，本命令
     * 不该跟着一起等（那会让"并发多实例"在 shell 上看起来又不成立）。 */
    while (arkrtos_elf_running_in(space) != 0)
    {
        if (waited >= SHELL_EXEC_WAIT_MS)
        {
            ark_shell_printf("[ELF] %s: still running after %u ms, prompt is "
                             "back (see 'task')\r\n",
                             path, (unsigned)SHELL_EXEC_WAIT_MS);
            return 0;
        }
        arkrtos_task_wait_internal(SHELL_EXEC_POLL_MS);
        waited += SHELL_EXEC_POLL_MS;
    }

    /* U-8（2026-11-17）之前这里只说「结束了」：EXIT 子命令没有 code 参数，
     * TCB 里也没有字段接着。现在子 4（EXIT_CODE）可以带码，装载器在任务
     * 变 INVALID 的那一次观察里把码取走存进 info。没上报过就只说时长，
     * 不编一个看起来像退出码的数字。 */
#if (ARKRTOS_USE_EXIT_CODE == 1)
    {
        arkrtos_elf_info_t fin;

        if((arkrtos_elf_info_space(space, &fin) == 0) && (fin.exit_code_set != 0u))
        {
            ark_shell_printf("[ELF] %s: finished after %u ms (space %u, exit code %d)\r\n",
                             path, (unsigned)waited, (unsigned)space,
                             (int)fin.exit_code);
#if (ARKRTOS_USE_AUDIT_LOG == 1)
            (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_EXIT,
                                           (uint32)arkrtos_current_task_id,
                                           (uint32)fin.exit_code);
#endif
            return 0;
        }
    }
#endif
    ark_shell_printf("[ELF] %s: finished after %u ms (space %u)\r\n",
                     path, (unsigned)waited, (unsigned)space);
    return 0;
}

int cmd_exec(int argc, char *argv[])
{
    if (argc < 2)
    {
        sh_out("usage: exec <path> [args...]   run an ELF64 executable\r\n");
        return 1;
    }
    return shell_exec_file(argv[1], argc - 1, &argv[1]);
}

#if (ARKRTOS_USE_EXIT_CODE == 1)
/* U-8 的另一半：等一个已经起跑的任务结束，再把它的退出码取出来。
 *
 * 和 exec 的区别只有一点：exec 等的是"我刚起跑的那个空间"，wait 等的是
 * 任意任务号——于是后台还在跑的东西（比如 exec 到 30 s 上限就交还提示符
 * 的那个程序，或者 ld 装进去的模块）也能在这里收尾。
 *
 * 退出码从 TCB 里读，不清它：装载器的实例台账是"观察到 INVALID 的那一次"
 * 取一次码，两边只读不写，所以谁先看到都不影响读数。槽位被复用后字段归零
 * （创建任务时清），那时读到的是新任务，不是旧任务的码。
 *
 * 诚实边界：这不是 POSIX 的 wait()——没有父子关系、没有僵尸队列、也没有
 * "只能由父进程回收一次"的约束；同时也没有反向的保证：任务号被复用后，
 * 这里的读数属于新任务。 */
int cmd_wait(int argc, char *argv[])
{
    uint32 id32 = 0u;
    uint32 limit = 5000u;
    uint32 waited = 0u;

    if ((argc < 2) || (parse_u32(argv[1], &id32) != 0) ||
        (id32 == 0u) || (id32 > (uint32)ARKRTOS_TASK_MAX_NUM))
    {
        sh_out("usage: wait <task id> [timeout ms]   (0 = wait until it exits)\r\n");
        return 1;
    }
    if (argc >= 3)
    {
        if (parse_u32(argv[2], &limit) != 0)
        {
            sh_out("wait: timeout must be a decimal number of milliseconds\r\n");
            return 1;
        }
    }
    if ((int32)id32 == arkrtos_current_task_id)
    {
        sh_out("wait: that is the shell's own task\r\n");
        return 1;
    }
    if (arkrtos_task_table[id32 - 1u].stack_ptr == 0u)
    {
        ark_shell_printf("wait: no such task %u\r\n", (unsigned)id32);
        return 1;
    }

    for (;;)
    {
        if (arkrtos_task_table[id32 - 1u].status == ARKRTOS_TASK_INVALID)
        {
            int32 code = 0;

            if (arkrtos_task_exit_code((int32)id32, &code) != 0)
            {
                ark_shell_printf("task %u exited (exit code %d)\r\n",
                                 (unsigned)id32, (int)code);
            }
            else
            {
                ark_shell_printf("task %u exited (no exit code reported)\r\n",
                                 (unsigned)id32);
            }
            return 0;
        }

        if ((limit != 0u) && (waited >= limit))
        {
            ark_shell_printf("task %u still running after %u ms (status %s)\r\n",
                             (unsigned)id32, (unsigned)waited,
                             task_state_name((uint32)arkrtos_task_table[id32 - 1u].status));
            return 0;
        }

        arkrtos_task_wait_internal(SHELL_EXEC_POLL_MS);
        waited += SHELL_EXEC_POLL_MS;
    }
}
#endif /* ARKRTOS_USE_EXIT_CODE */

/* 未知命令的回落点（ark_shell_eval.c 在报 command not found 之前调）。
 * 只有长得像路径的词（/x、./x、../x）才接，其他原样返回 -1 让 shell
 * 照旧报 command not found——否则打错的命令名会被当成文件名去开门。 */
int ark_shell_exec_fallback(int argc, char *argv[])
{
    const char *p;

    if ((argc <= 0) || (argv == 0) || (argv[0] == 0))
    {
        return -1;
    }
    p = argv[0];
    if ((p[0] == '/') ||
        ((p[0] == '.') && (p[1] == '/')) ||
        ((p[0] == '.') && (p[1] == '.') && (p[2] == '/')))
    {
        return shell_exec_file(argv[0], argc, argv);
    }
    return -1;
}
#endif /* ARKRTOS_USE_ELF */

#if (ARKRTOS_USE_MODLOAD == 1)
/* ============================================================
 * ld / lkup / modules / unld：VxWorks 式可下载模块
 * @details 与 exec 是两种模型，不是同一件事的两步：
 *          exec 跑的是链接期定好地址的 ET_EXEC（PLV3，隔离）；
 *          ld 跑的是板上现场链接的 ET_REL（PLV0，直接调内核 API）。
 *          所以 ld 不等模块结束——VxWorks 的 ld 也是这样，装载动作本身
 *          就把模块当任务挂起来了，输出由模块自己的任务打。
 * ============================================================ */

int cmd_ld(int argc, char *argv[])
{
    arkrtos_mod_info_t info;
    int32 rc;
    int32 before;
    int32 idx;

    if (argc < 2)
    {
        sh_out("usage: ld <path.o> [args...]   link and run a relocatable module\r\n");
        return 1;
    }

    before = arkrtos_mod_count();
    rc = arkrtos_mod_load(argv[1], argc - 1, &argv[1]);
    if (rc != ARKRTOS_MOD_OK)
    {
        /* 失败原因必须点到具体符号/类型号：只说一个 rc 等于让人去猜。 */
        ark_shell_printf("\r\n%s: %s (rc=%d)\r\n", argv[1],
                         arkrtos_mod_error_name(rc), (int)rc);
        if ((rc == ARKRTOS_MOD_ERR_SYMBOL) &&
            (arkrtos_mod_missing_symbol()[0] != '\0'))
        {
            ark_shell_printf("  missing symbol: %s\r\n",
                             arkrtos_mod_missing_symbol());
            sh_out("  (add it to kernelsrc/src/arkrtos_exports.c, or drop the "
                   "reference from the module)\r\n");
        }
        else if (rc == ARKRTOS_MOD_ERR_RELOC)
        {
            ark_shell_printf("  unsupported relocation type: %u\r\n",
                             (unsigned)arkrtos_mod_bad_reloc());
        }
        return 1;
    }

    idx = arkrtos_mod_count() - 1;
    if ((idx >= before) && (arkrtos_mod_info(idx, &info) == 0))
    {
        ark_shell_printf("[MOD] %s: base 0x%08X, %u B, entry 0x%08X, "
                         "%u reloc(s), %u import(s), task %d\r\n",
                         info.name, (unsigned)info.base, (unsigned)info.size,
                         (unsigned)info.entry, (unsigned)info.relocs,
                         (unsigned)info.imports, (int)info.task);
    }
    return 0;
}

int cmd_lkup(int argc, char *argv[])
{
    const arkrtos_export_t *tab;
    uint32 n = 0u;
    uint32 i;
    unsigned long addr = 0ul;

    if (argc >= 2)
    {
        if (arkrtos_export_lookup(argv[1], &addr) != 0)
        {
            ark_shell_printf("%s: not exported\r\n", argv[1]);
            return 1;
        }
        ark_shell_printf("%s = 0x%08X\r\n", argv[1], (unsigned)addr);
        return 0;
    }

    tab = arkrtos_export_table(&n);
    if (tab == 0)
    {
        sh_out("export table not compiled in\r\n");
        return 1;
    }
    ark_shell_printf("kernel exports (%u):\r\n", (unsigned)n);
    for (i = 0u; i < n; i++)
    {
        ark_shell_printf("  0x%08X  %s\r\n",
                         (unsigned)tab[i].addr,
                         (tab[i].name != 0) ? tab[i].name : "?");
    }
    return 0;
}

int cmd_modules(int argc, char *argv[])
{
    arkrtos_mod_info_t info;
    int32 n;
    int32 i;

    (void)argc;
    (void)argv;

    n = arkrtos_mod_count();
    if (n <= 0)
    {
        sh_out("no module loaded\r\n");
        return 0;
    }
    sh_out("idx  name             base      size   entry     reloc  imp  task  state\r\n");
    for (i = 0; i < n; i++)
    {
        if (arkrtos_mod_info(i, &info) != 0)
        {
            continue;
        }
        ark_shell_printf("%-4d %-16s 0x%08X %6u 0x%08X %5u %4u %5d %s\r\n",
                         (int)i, info.name, (unsigned)info.base,
                         (unsigned)info.size, (unsigned)info.entry,
                         (unsigned)info.relocs, (unsigned)info.imports,
                         (int)info.task,
                         (info.state != 0) ? "run" : "done");
    }
    return 0;
}

int cmd_unld(int argc, char *argv[])
{
    uint32 idx = 0u;
    int32 rc;

    if (argc < 2)
    {
        sh_out("usage: unld <index>   (index comes from 'modules')\r\n");
        return 1;
    }
    if (parse_u32(argv[1], &idx) != 0)
    {
        ark_shell_printf("unld: bad index '%s'\r\n", argv[1]);
        return 1;
    }
    rc = arkrtos_mod_unload((int32)idx);
    if (rc != ARKRTOS_MOD_OK)
    {
        ark_shell_printf("unld %u: %s (rc=%d)\r\n", (unsigned)idx,
                         arkrtos_mod_error_name(rc), (int)rc);
        return 1;
    }
    ark_shell_printf("unld %u: slot released\r\n", (unsigned)idx);
    return 0;
}
#endif /* ARKRTOS_USE_MODLOAD */

#endif /* SHELL_ENABLE */
