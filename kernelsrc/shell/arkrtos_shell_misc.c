/**
* @file arkrtos_shell_misc.c
* @brief 杂项：history / uname / free / ps / which / man / dmesg / hwsafe。
*          共同点是只读、且输出直接来自别的模块的公开读数。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

/* ============================================================
 * history：打印命令历史（最旧 → 最新）
 * ============================================================ */
int cmd_history(int argc, char *argv[])
{
    int i;

    (void)argc;
    (void)argv;

    for(i = 0; i < g_shell.history.count; i++)
    {
        ark_shell_printf("%5d  %s\r\n", i + 1, g_shell.history.buf[i]);
    }
    return 0;
}

/* ============================================================
 * uname：系统与构建信息
 * ============================================================ */
int cmd_uname(int argc, char *argv[])
{
    int all = ((argc >= 2) && (strcmp(argv[1], "-a") == 0));

    if(all != 0)
    {
        ark_shell_printf("Ark RTOS arkrtos " ARKRTOS_VERSION_STRING
                         " %s %s %s\r\n",
                         ARKRTOS_ARCH_NAME, __DATE__, __TIME__);
    }
    else
    {
        ark_shell_printf("Ark RTOS " ARKRTOS_VERSION_STRING " %s %s %s\r\n",
                         ARKRTOS_ARCH_NAME, __DATE__, __TIME__);
    }
    return 0;
}

/* ============================================================
 * free：内存概览
 *
 * 内核不拥有堆，所以这里没有 Linux 的 Mem/Swap 可比。给出的只是这台
 * 设备真实持有的账：镜像池的分区口径统计，以及内核 RAM 段的容量。
 * 内核 RAM 的 used/free 内核不记录，一律打 "n/a"，绝不编一个数字。
 * ============================================================ */
int cmd_free(int argc, char *argv[])
{
    const arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 used = 0u;
    uint32 total = 0u;
    uint32 frag = 0u;

    (void)argc;
    (void)argv;

    (void)arkrtos_ptable_stats(&used, &total, &frag);

    sh_out("\r\n              total     used     free\r\n");
    ark_shell_printf("pool(image)   %-8u  %-8u %-8u  (%u%% used, %u fragments)\r\n",
                     (unsigned)total, (unsigned)used,
                     (unsigned)((total >= used) ? (total - used) : 0u),
                     (unsigned)((total != 0u) ? ((used * 100u) / total) : 0u),
                     (unsigned)frag);
    ark_shell_printf("kernel RAM    %-8u  %-8s %-8s  (partition window)\r\n",
                     (unsigned)pt->kernel_ram_size, "n/a", "n/a");
    return 0;
}

/* ============================================================
 * ps：内核任务与已装镜像一览
 *
 * 内核任务没有名字、没有控制终端、也没有累计 CPU 时间，所以只列内核
 * 确实知道的列；缺的列不编（不是完整 ps）。
 * ============================================================ */
int cmd_ps(int argc, char *argv[])
{
    const arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    int32 i;

    (void)argc;
    (void)argv;

    ark_shell_printf("\r\n  PID  PRIO  STATE     ENTRY\r\n");
    for(i = 0; i < arkrtos_task_count; i++)
    {
        ark_shell_printf("  %-4d %-5u %-8s  0x%08X\r\n",
                         (int)(i + 1),
                         (unsigned)arkrtos_task_table[i].priority,
                         task_state_name((uint32)arkrtos_task_table[i].status),
                         (unsigned)arkrtos_task_table[i].entry);
    }

    ark_shell_printf("\r\n  SLOT TYPE  STATE     TASK  ENTRY\r\n");
    for(i = 0; (i < (int32)pt->slot_max) && (i < (int32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] == ARKRTOS_SLOT_FREE)
        {
            continue;
        }
        ark_shell_printf("  %-4d %-4s  %-8s  %-4u  0x%08X\r\n",
                         (int)i, slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (unsigned)pt->slot_task_id[i],
                         (unsigned)pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * which：把别名解析成真实命令
 * ============================================================ */
int cmd_which(int argc, char *argv[])
{
    const ark_shell_cmd_t *c;
    const char *a1 = 0;
    const char *a2 = 0;
    const char *target;

    if(argc < 2)
    {
        sh_out("usage: which <command>\r\n");
        return 1;
    }

    target = ark_shell_alias_target(argv[1], &a1, &a2);
    if(target != 0)
    {
        ark_shell_printf("%s: aliased to %s", argv[1], target);
        if(a1 != 0) { ark_shell_printf(" %s", a1); }
        if(a2 != 0) { ark_shell_printf(" %s", a2); }
        sh_out("\r\n");
        return 0;
    }

    c = ark_shell_find_command(argv[1]);
    if(c != 0)
    {
        ark_shell_printf("%s\r\n", c->name);
        return 0;
    }

    /* Like which(1): print nothing and fail. */
    return 1;
}

/* ============================================================
 * man：打印命令帮助
 * ============================================================ */
int cmd_man(int argc, char *argv[])
{
    const ark_shell_cmd_t *c;
    const char *a1 = 0;
    const char *a2 = 0;
    const char *target;

    if(argc < 2)
    {
        sh_out("What manual page do you want?\r\n");
        return 1;
    }

    target = ark_shell_alias_target(argv[1], &a1, &a2);
    if(target != 0)
    {
        c = ark_shell_find_command(target);
        ark_shell_printf("%s: aliased to %s", argv[1], target);
        if(a1 != 0) { ark_shell_printf(" %s", a1); }
        if(a2 != 0) { ark_shell_printf(" %s", a2); }
        sh_out("\r\n");
        if(c != 0) { ark_shell_printf("  %s - %s\r\n", c->name, c->help); }
        return 0;
    }

    c = ark_shell_find_command(argv[1]);
    if(c == 0)
    {
        ark_shell_printf("No manual entry for %s\r\n", argv[1]);
        return 1;
    }

    ark_shell_printf("%s - %s\r\n", c->name, c->help);
    return 0;
}

#if ARKRTOS_USE_LOG
/* ============================================================
 * dmesg：打印内核日志环
 * ============================================================ */
static void dmesg_put(const char *data, uint32 len, void *arg)
{
    (void)arg;
    platform_uart_send_buf((const unsigned char *)data, (uint16_t)len);
}

int cmd_dmesg(int argc, char *argv[])
{
    int clear = 0;

    if(argc >= 2)
    {
        if(strcmp(argv[1], "-c") == 0)
        {
            clear = 1;
        }
        else
        {
            sh_out("usage: dmesg [-c]\r\n");
            return 1;
        }
    }

    if(arkrtos_log_ring_capacity() == 0u)
    {
        sh_out("\r\nlog ring unavailable (ARKRTOS_USE_LOG_RING is off)\r\n");
        return 1;
    }

    if(arkrtos_log_ring_used() == 0u)
    {
        sh_out("\r\nlog ring is empty\r\n");
        return 0;
    }

    /* The ring is written under the console lock in arkrtos_log_emit(); take
     * the same lock so a line cannot be spliced mid-dump. */
    platform_console_begin();
    (void)arkrtos_log_ring_dump(dmesg_put, 0);
    platform_console_end();

    if(clear != 0)
    {
        arkrtos_log_ring_clear();
    }
    return 0;
}
#endif /* ARKRTOS_USE_LOG */

#if ARKRTOS_HW_SAFE
/* Hardware safe-state ("will") ledger: which task owns which device, which
 * devices declared a safe state, and what the kernel has neutralized so far.
 * Read-only view; see docs/App崩溃硬件安全与遗言机制.md. */
int cmd_hwsafe(int argc, char *argv[])
{
    char  name[16];
    int32 i, n;
    int32 owner, dead, has_will, wcmd, wval;
    int32 devs   = arkrtos_hwsafe_dev_count();
    int32 owners = arkrtos_hwsafe_owner_count();

    (void)argc;
    (void)argv;

    {
        uint32 nw = 0u;
        uint32 nf = 0u;
        uint32 nc = 0u;
        uint32 nn = 0u;
        uint32 nh = 0u;

        arkrtos_hwsafe_path_counts(&nw, &nf, &nc, &nn, &nh);
        ark_shell_printf("hwsafe: enabled, devs=%d owners=%d neutralized=%u"
                         " (will=%u cb=%u none=%u fail=%u handover=%u) last_owner=%d\r\n",
                         (int)devs, (int)owners,
                         (unsigned)arkrtos_hwsafe_neutralized_count(),
                         (unsigned)nw, (unsigned)nc, (unsigned)nn,
                         (unsigned)nf, (unsigned)nh,
                         (int)arkrtos_hwsafe_last_owner());
    }

    for(i = 0; i < devs; i++)
    {
        if(arkrtos_hwsafe_dev_at(i, name, sizeof(name)) != 0)
        {
            break;
        }
        ark_shell_printf("  dev%d: %s  (safe-state callback registered)\r\n",
                         (int)i, name);
    }

    for(n = 0; n < owners; n++)
    {
        if(arkrtos_hwsafe_info(n, name, sizeof(name), &owner, &dead) != 0)
        {
            break;
        }
        ark_shell_printf("  own%d: dev=%s owner=%d dead=%d will=",
                         (int)n, name, (int)owner, (int)dead);
        if((arkrtos_hwsafe_will(n, &has_will, &wcmd, &wval) == 0) && (has_will != 0))
        {
            ark_shell_printf("%d:%d\r\n", (int)wcmd, (int)wval);
        }
        else
        {
            sh_out("none\r\n");
        }
    }

    if(owners == 0)
    {
        sh_out("  (no device ownership recorded)\r\n");
    }
    return 0;
}
#endif /* ARKRTOS_HW_SAFE */

#endif /* SHELL_ENABLE */
