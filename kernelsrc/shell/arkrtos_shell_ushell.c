/**
* @file arkrtos_shell_ushell.c
* @brief 用户命令（SVC 0x1A）：App 侧命令注册表与内核跳板。
*          注册表、输出缓冲、跳板与卸载清理都在这里；控制台任务只调用
*          ushell_drop() 清理某个镜像留下的命令。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

/* ============================================================
 * 用户命令（SVC 0x1A）：注册表 + 内核跳板
 *
 * 内核不改 ark_shell（上游原样引入），而是在命令表里追加一条指向本文件的
 * 跳板命令：
 *   - 命令名与帮助文本注册时复制进内核 RAM，镜像卸载后不留下悬垂指针；
 *   - 跳板执行前先确认处理器仍落在某个「已装载」镜像的区间内，
 *     镜像被卸载后命令自动失效并给出提示，而不是跳到已擦除的 Flash 上。
 * ============================================================ */



arkrtos_ushell_entry_t g_ushell[ARKRTOS_USHELL_MAX_CMDS];
static char g_ushell_out[ARKRTOS_USHELL_OUT_CHUNK + 1];

/* 命令名比较：不区分大小写，且要求两边同时结束 */
static int ushell_name_eq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }

    while((*a != '\0') && (*b != '\0'))
    {
        uint32 ca = (uint32)(uint8)*a;
        uint32 cb = (uint32)(uint8)*b;

        if((ca >= 'A') && (ca <= 'Z'))
        {
            ca += 32u;
        }
        if((cb >= 'A') && (cb <= 'Z'))
        {
            cb += 32u;
        }
        if(ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

static void ushell_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (cap == 0u))
    {
        return;
    }

    if(src != 0)
    {
        while((src[i] != '\0') && ((i + 1u) < cap))
        {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\0';
}

/* Defined below, next to the dispatch itself: the drop helper needs the
 * trampoline address to find its row in the command table. */
static int arkrtos_shell_ext_trampoline(int argc, char **argv);

/* Remove one entry: the trampoline row is deleted by shifting the later rows
 * down (their order is the help display order, so no hole is left), then the
 * slot itself is cleared. Register / unregister / task-exit all go through
 * here, so the bookkeeping cannot drift between the three.
 * Runs with interrupts off (called from a critical section); it only touches
 * this file's tables and performs no blocking call. */
void ushell_drop(uint32 i)
{
    uint32 k;

    for(k = 0u; (k < (uint32)g_cmd_count) && (k < (uint32)ARK_SHELL_MAX_COMMANDS); k++)
    {
        if((g_cmd_table[k].func == arkrtos_shell_ext_trampoline) &&
           (g_cmd_table[k].name == g_ushell[i].name))
        {
            uint32 j;

            for(j = k; (j + 1u) < (uint32)g_cmd_count; j++)
            {
                g_cmd_table[j] = g_cmd_table[j + 1u];
            }
            g_cmd_count--;
            break;
        }
    }

    g_ushell[i].in_use = 0u;
    g_ushell[i].func = 0;
    g_ushell[i].owner_task = 0u;
    g_ushell[i].name[0] = '\0';
    g_ushell[i].help[0] = '\0';
}

/* 处理器是否还活着：必须仍落在某个已装载镜像的 Flash 区间内 */
static int ushell_func_alive(const void *p)
{
    const arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 addr = (uint32)p;
    uint32 i;

    if((p == 0) || (pt == 0))
    {
        return 0;
    }

    for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] == ARKRTOS_SLOT_FREE) || (pt->slot_base[i] == 0u))
        {
            continue;
        }
        if((pt->slot_state[i] != ARKRTOS_APP_SLOT_LOADED) &&
           (pt->slot_state[i] != ARKRTOS_APP_SLOT_RUNNING))
        {
            continue;
        }
        if((addr >= pt->slot_base[i]) &&
           (addr < (pt->slot_base[i] + pt->slot_size[i])))
        {
            return 1;
        }
    }

    return 0;
}

/* 所有用户命令共用同一个跳板：用 argv[0] 反查注册表项 */
static int arkrtos_shell_ext_trampoline(int argc, char **argv)
{
    uint32 i;

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (argv == 0) || (argc <= 0) || (argv[0] == 0))
        {
            continue;
        }

        if(ushell_name_eq(g_ushell[i].name, argv[0]) == 0)
        {
            continue;
        }

        if(ushell_func_alive((const void *)g_ushell[i].func) == 0)
        {
            ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                             g_ushell[i].name);
            return -1;
        }

        {
            /* The callback belongs to an App but runs on the kernel shell task
             * stack: the buffers it renders match no user RAM window, so every
             * buffered SVC service call from inside the callback would be
             * rejected. Raise the flag for the call and drop it right after. */
            extern uint32 arkrtos_kernel_ushell_active;
            int rc;

            arkrtos_kernel_ushell_active = 1u;
            rc = g_ushell[i].func(argc, argv);
            arkrtos_kernel_ushell_active = 0u;
            return rc;
        }
    }

    sh_out("user command not available\r\n");
    return -1;
}

int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd)
{
    uint32 i;
    uint32 len;

    if((cmd == 0) || (cmd->name == 0) || (cmd->func == 0))
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    len = (uint32)strlen(cmd->name);
    if((len == 0u) || (len > (uint32)ARKRTOS_USHELL_NAME_MAX))
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    if(g_cmd_count >= ARK_SHELL_MAX_COMMANDS)
    {
        return ARKRTOS_USHELL_ERR_FULL;
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use != 0u) && (ushell_name_eq(g_ushell[i].name, cmd->name) != 0))
        {
            return ARKRTOS_USHELL_ERR_DUP;
        }
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if(g_ushell[i].in_use == 0u)
        {
            break;
        }
    }
    if(i >= ARKRTOS_USHELL_MAX_CMDS)
    {
        return ARKRTOS_USHELL_ERR_FULL;
    }

    ushell_copy(g_ushell[i].name, (uint32)sizeof(g_ushell[i].name), cmd->name);
    ushell_copy(g_ushell[i].help, (uint32)sizeof(g_ushell[i].help), cmd->help);
    g_ushell[i].func = cmd->func;
    g_ushell[i].max_args = cmd->max_args;
    g_ushell[i].in_use = 1u;
    /* Remember who owns it. A kernel side registration (task id 0) is not tied
     * to any task lifetime and must survive every App stopping. */
    g_ushell[i].owner_task = (arkrtos_current_task_id > 0)
                             ? (uint32)arkrtos_current_task_id : 0u;

    g_cmd_table[g_cmd_count].name = g_ushell[i].name;
    g_cmd_table[g_cmd_count].func = arkrtos_shell_ext_trampoline;
    g_cmd_table[g_cmd_count].help = g_ushell[i].help;
    g_cmd_table[g_cmd_count].max_args = (int)cmd->max_args;
    g_cmd_count++;

    return ARKRTOS_USHELL_OK;
}

int32 arkrtos_shell_ext_unregister(const char *name)
{
    uint32 i;

    if(name == 0)
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (ushell_name_eq(g_ushell[i].name, name) == 0))
        {
            continue;
        }

        /* 摘掉命令表里的跳板条目：表内顺序即 help 的显示顺序，
         * 所以用后项前移而不是留空洞 */
        ushell_drop(i);

        return ARKRTOS_USHELL_OK;
    }

    return ARKRTOS_USHELL_ERR_NOTFND;
}

int32 arkrtos_shell_print_n(const char *msg, uint32 len)
{
    uint32 off = 0u;

    if(msg == 0)
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    while(off < len)
    {
        uint32 n = len - off;
        uint32 i;

        if(n > (uint32)ARKRTOS_USHELL_OUT_CHUNK)
        {
            n = (uint32)ARKRTOS_USHELL_OUT_CHUNK;
        }

        for(i = 0u; i < n; i++)
        {
            g_ushell_out[i] = msg[off + i];
        }
        g_ushell_out[n] = '\0';

        sh_out(g_ushell_out);
        off += n;
    }

    return ARKRTOS_USHELL_OK;
}

#endif /* SHELL_ENABLE */
