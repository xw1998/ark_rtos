/**
* @file arkrtos_shell_misc.c
* @brief 杂项：history / uname / free / ps / which / man / dmesg / hwsafe /
*        swap（任务热切换自检，仅 ARKRTOS_USE_TASK_SWAP=1 时存在）/
*        devid（设备身份读数，仅 ARKRTOS_USE_DEVICE_ID=1 时存在）。
*          共同点是只读、且输出直接来自别的模块的公开读数。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (ARKRTOS_USE_DEVICE_ID == 1)
#include "arkrtos_devid.h"
#endif

#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
#include "arkrtos_linux_abi.h"
#endif

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
                         (unsigned)(arkrtos_uptr_t)arkrtos_task_table[i].entry);
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

#if (ARKRTOS_USE_TASK_SWAP == 1)
/* ============================================================
 * swap：任务热切换的内核自检（A/B 双缓冲）
 *
 * 这条命令验证的是内核那四步（帧判据 / 整块搬迁 / 帧改写 / 恢复路径换帧）
 * 在**当前架构**上真的成立，而不是「开关能编过」。
 *
 * 做法：注册一个循环桩 v1 跑在静态块 A 上；等它真的跑起来（计数上涨）后，
 * 把「换到块 B、入口 v2」挂上；v1 的下一次 SVC（它自己那一次 wait）落地切换。
 * 判据只有两条：
 *   - v1 的计数在切换后不再变化（旧镜像真的停住了）；
 *   - v2 的计数开始上涨（新入口真的在跑）。
 *
 * 诚实边界：两个入口都在内核镜像里，所以这条读数证明的是「帧层面换得动、
 * 整块搬得动、恢复路径认得新帧」，**不是**「镜像被重定位」。PIC / 装载属于
 * mini/app 装载器，已由 ARM 上板读数覆盖，与架构无关。
 *
 * 每个启动只跑一次：本内核没有「按任务号杀任务」的接口，v2 会一直跑着。
 * ============================================================ */
#include <string.h>

#define SWAP_TEST_BLOCK_BYTES   4096u

#if defined(__CC_ARM)
#define SWAP_TEST_ALIGN(n)      __align(n)
#else
#define SWAP_TEST_ALIGN(n)      __attribute__((aligned(n)))
#endif

/* 两块的地址与大小都要满足 swap 的前提：2 的幂且按自身对齐。 */
static SWAP_TEST_ALIGN(4096) uint8 g_swap_blk_a[SWAP_TEST_BLOCK_BYTES];
static SWAP_TEST_ALIGN(4096) uint8 g_swap_blk_b[SWAP_TEST_BLOCK_BYTES];

static volatile uint32 g_swap_v1_loops = 0u;
static volatile uint32 g_swap_v2_loops = 0u;
static int32  g_swap_test_task = 0;
static uint32 g_swap_test_done = 0u;
static uint32 g_swap_test_pass = 0u;

/* 自检桩必须走一次真正的 SVC（热切换只在 SVC 帧上落地），所以用 oslib 的
 * 公开入口，而不是内核内部的 arkrtos_task_wait_internal()。 */
void arkrtos_task_wait(uint32 ms);

static void swap_test_v1(void)
{
    for(;;)
    {
        g_swap_v1_loops++;
        arkrtos_task_wait(2u);
    }
}

static void swap_test_v2(void)
{
    for(;;)
    {
        g_swap_v2_loops++;
        arkrtos_task_wait(2u);
    }
}

int cmd_swap(int argc, char *argv[])
{
    uint32 i;
    uint32 a1;
    uint32 a2;
    uint32 b;
    int32  rc;

    if((argc >= 2) && (strcmp(argv[1], "stat") == 0))
    {
        ark_shell_printf("swap: pending=%d task=%d v1=%u v2=%u done=%u pass=%u\r\n",
                         (int)arkrtos_task_swap_pending(), (int)g_swap_test_task,
                         (unsigned)g_swap_v1_loops, (unsigned)g_swap_v2_loops,
                         (unsigned)g_swap_test_done, (unsigned)g_swap_test_pass);
        return 0;
    }

    if(g_swap_test_done != 0u)
    {
        sh_out("swap: self-test already ran this boot (reboot to run it again)\r\n");
        return 1;
    }

    g_swap_v1_loops = 0u;
    g_swap_v2_loops = 0u;
    (void)arkrtos_task_swap_cancel(0);

    g_swap_test_task = arkrtos_task_register(swap_test_v1,
                                             (uint32 *)(void *)g_swap_blk_a,
                                             SWAP_TEST_BLOCK_BYTES, 8u, 0u);
    if(g_swap_test_task <= 0)
    {
        g_swap_test_task = 0;
        sh_out("swap: cannot register the test task\r\n");
        return 1;
    }

    /* 等 v1 真的跑起来：注册只让它进就绪队列，计数上不来就是任务没起来。 */
    for(i = 0u; (i < 100u) && (g_swap_v1_loops == 0u); i++)
    {
        arkrtos_task_wait_internal(10u);
    }
    if(g_swap_v1_loops == 0u)
    {
        sh_out("swap: test task did not start\r\n");
        return 1;
    }

    arkrtos_task_swap_selftest_arm(g_swap_test_task);
    rc = arkrtos_task_swap_arm(g_swap_test_task, (uint32)CHIP_FLASH_BASE,
                               SWAP_TEST_BLOCK_BYTES,
                               (uint32)(void *)g_swap_blk_b, SWAP_TEST_BLOCK_BYTES,
                               (uint32)(void *)swap_test_v2);
    if(rc != 0)
    {
        ark_shell_printf("swap: arm rc=%d\r\n", (int)rc);
        return 1;
    }

    /* 切换落在 v1 的下一次 SVC 上（它每 2 ms 一次 wait，很快就到）。 */
    arkrtos_task_wait_internal(100u);
    a1 = g_swap_v1_loops;
    arkrtos_task_wait_internal(100u);
    a2 = g_swap_v1_loops;
    b  = g_swap_v2_loops;

    g_swap_test_done = 1u;
    g_swap_test_pass = ((a1 == a2) && (b > 0u)) ? 1u : 0u;

    ark_shell_printf("swap: v1=%u -> %u  v2=%u\r\n",
                     (unsigned)a1, (unsigned)a2, (unsigned)b);
    if(g_swap_test_pass != 0u)
    {
        sh_out("swap: PASS (old image stopped, new entry running)\r\n");
        return 0;
    }
    sh_out("swap: FAIL (see counters above)\r\n");
    return 1;
}
#endif /* ARKRTOS_USE_TASK_SWAP == 1 */

#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
/* ============================================================
 * lxstat：Linux ABI 计数 + 按号直方图
 *
 * 名字表只为了好读：号是权威，名字是注解。表里的号都是从 Linux
 * UAPI 抄的，与 arkrtos_linux_abi.c 里的 LX_SYS_* 同源；这里不复用
 * 那些宏，是因为直方图里也会出现**没实现**的号（那才是要看的），
 * 而那些号在内核里没有宏。
 * ============================================================ */
typedef struct
{
    uint32      nr;
    const char *name;
} lxstat_name_t;

static const lxstat_name_t g_lxstat_names[] =
{
    {  56u, "openat"          },
    {  57u, "close"           },
    {  63u, "read"            },
    {  64u, "write"           },
    {  67u, "pread64"         },
    {  93u, "exit"            },
    {  94u, "exit_group"      },
    {  96u, "set_tid_address" },
    {  98u, "futex"           },
    { 113u, "clock_gettime"   },
    { 160u, "uname"           },
    { 172u, "getpid"          },
    { 174u, "getuid"          },
    { 175u, "geteuid"         },
    { 178u, "gettid"          },
    { 214u, "brk"             },
    { 215u, "munmap"          },
    { 222u, "mmap"            },
    { 226u, "mprotect"        },
    { 233u, "madvise"         },
    { 261u, "prlimit64"       },
    { 278u, "getrandom"       },
    { 293u, "rseq"            },
};

static const char *lxstat_name(uint32 nr)
{
    uint32 i;
    for(i = 0u; i < (uint32)(sizeof(g_lxstat_names) / sizeof(g_lxstat_names[0])); i++)
    {
        if(g_lxstat_names[i].nr == nr)
        {
            return g_lxstat_names[i].name;
        }
    }
    return "?";
}

int cmd_lxstat(int argc, char *argv[])
{
    uint32 i;
    uint32 slots;

    (void)argc;
    (void)argv;

    ark_shell_printf("lxstat: handled=%u enosys=%u efault=%u other=%u mprotect=%u\r\n",
                     (unsigned)arkrtos_linux_abi_stat(0u),
                     (unsigned)arkrtos_linux_abi_stat(1u),
                     (unsigned)arkrtos_linux_abi_stat(2u),
                     (unsigned)arkrtos_linux_abi_stat(3u),
                     (unsigned)arkrtos_linux_abi_stat(4u));

    slots = arkrtos_linux_abi_nr_slots();
    if(slots == 0u)
    {
        sh_out("lxstat: no Linux syscall seen since the last exec\r\n");
        return 0;
    }

    for(i = 0u; i < slots; i++)
    {
        uint32 nr = 0u;
        uint32 hits = arkrtos_linux_abi_nr_slot(i, &nr);
        ark_shell_printf("  nr %3u %-16s x%u\r\n",
                         (unsigned)nr, lxstat_name(nr), (unsigned)hits);
    }

    if(arkrtos_linux_abi_nr_overflow() != 0u)
    {
        ark_shell_printf("  (+%u calls on numbers past the %u-slot table)\r\n",
                         (unsigned)arkrtos_linux_abi_nr_overflow(),
                         (unsigned)slots);
    }
    return 0;
}
#endif /* ARKRTOS_USE_LINUX_ABI == 1 && ARKRTOS_LINUX_ABI_SYSSTAT == 1 */


/* ============================================================
 * devid：设备身份（SEC-G13）的现场读数
 *
 *   devid                          这份身份从哪来、能不能应答
 *   devid prove <hex-challenge>    用本机密钥算一个应答（看是否失败关闭）
 *   devid check <hex-ch> <hex-mac> 校验一个应答（对照别的机器算出来的）
 *
 * 这个命令只读不写：不改密钥、不改 ID、不做任何"信任我"的判断。它的用处
 * 是在板上直接看出三件事——名字是什么、密钥来自板子还是固件、拿不到密钥
 * 时是不是干脆拒绝（而不是回一个全零 MAC 让人误以为通过）。
 * ============================================================ */
#if (ARKRTOS_USE_DEVICE_ID == 1)

/* 十六进制串 -> 字节。接受 "A501FF" 与 "A5 01 ff"（空格跳过）。
 * 长度奇数、非十六进制字符、超长一律返回 -1，不猜、不截断。 */
static int devid_hex(const char *p_s, uint8 *p_out, uint32 cap, uint32 *p_len)
{
    uint32 n = 0u;

    if((p_s == 0) || (*p_s == '\0'))
    {
        return -1;
    }

    while(*p_s != '\0')
    {
        uint32 hi;
        uint32 lo;
        char  hc;

        while(*p_s == ' ')
        {
            p_s++;
        }
        if(*p_s == '\0')
        {
            break;
        }
        hc = *p_s;
        if((hc >= '0') && (hc <= '9'))      { hi = (uint32)(hc - '0');      }
        else if((hc >= 'a') && (hc <= 'f')) { hi = (uint32)(hc - 'a' + 10); }
        else if((hc >= 'A') && (hc <= 'F')) { hi = (uint32)(hc - 'A' + 10); }
        else                                { return -1; }
        p_s++;
        while(*p_s == ' ')
        {
            p_s++;
        }
        if(*p_s == '\0')
        {
            return -1;      /* 奇数个数字：这不是一个字节 */
        }
        hc = *p_s;
        if((hc >= '0') && (hc <= '9'))      { lo = (uint32)(hc - '0');      }
        else if((hc >= 'a') && (hc <= 'f')) { lo = (uint32)(hc - 'a' + 10); }
        else if((hc >= 'A') && (hc <= 'F')) { lo = (uint32)(hc - 'A' + 10); }
        else                                { return -1; }
        p_s++;

        if(n >= cap)
        {
            return -1;
        }
        p_out[n] = (uint8)((hi << 4) | lo);
        n++;
    }

    *p_len = n;
    return 0;
}

static void devid_print_id(void)
{
    arkrtos_devid_status_t st;
    uint32 i;

    if(arkrtos_devid_status_get(&st) != ARKRTOS_DEVID_OK)
    {
        sh_out("devid: no status\r\n");
        return;
    }

    ark_shell_printf("source   : %s\r\n", arkrtos_devid_src_name(st.source));
    ark_shell_printf("ready    : %s\r\n", (st.ready != 0u) ? "yes (can answer a challenge)" : "no");
    ark_shell_printf("uid      : %u byte(s)\r\n", (unsigned)st.uid_len);
    ark_shell_printf("secret   : %u byte(s)\r\n", (unsigned)st.secret_len);
    if(st.id_len == 0u)
    {
        sh_out("id       : (none - no uid, so there is nothing to name)\r\n");
        return;
    }

    ark_shell_printf("id       : %u byte(s) ", (unsigned)st.id_len);
    for(i = 0u; i < st.id_len; i++)
    {
        ark_shell_printf("%02X", (unsigned)st.id[i]);
    }
    sh_out("\r\n");
    if(st.source == (uint32)ARKRTOS_DEVID_SRC_UID_ONLY)
    {
        sh_out("note     : uid only - the ID names this chip, but nothing can be proved\r\n");
    }
    else if(st.source == (uint32)ARKRTOS_DEVID_SRC_BUILTIN)
    {
        sh_out("note     : secret lives in this firmware - a reflash can clone it\r\n");
    }
}

int cmd_devid(int argc, char *argv[])
{
    static uint8 s_ch[ARKRTOS_DEVID_CHALLENGE_MAX];
    static uint8 s_mac[ARKRTOS_DEVID_MAC_LEN];
    uint32 ch_len = 0u;
    uint32 mac_len = 0u;
    uint32 i;
    int rc;

    if((argc == 1) || (strcmp(argv[1], "show") == 0))
    {
        devid_print_id();
        return 0;
    }

    if(strcmp(argv[1], "prove") == 0)
    {
        if(argc != 3)
        {
            sh_out("usage: devid prove <hex-challenge>\r\n");
            return -1;
        }
        if(devid_hex(argv[2], s_ch, sizeof(s_ch), &ch_len) != 0)
        {
            ark_shell_printf("devid: bad challenge hex (1..%u bytes, even digits)\r\n",
                             (unsigned)sizeof(s_ch));
            return -1;
        }
        rc = arkrtos_devid_prove(s_ch, ch_len, s_mac);
        if(rc == ARKRTOS_DEVID_ERR_NOKEY)
        {
            sh_out("devid: no usable secret - refusing (fail closed), no MAC produced\r\n");
            return -1;
        }
        if(rc != ARKRTOS_DEVID_OK)
        {
            ark_shell_printf("devid: prove failed (%d)\r\n", rc);
            return -1;
        }
        sh_out("mac      : ");
        for(i = 0u; i < (uint32)ARKRTOS_DEVID_MAC_LEN; i++)
        {
            ark_shell_printf("%02X", (unsigned)s_mac[i]);
        }
        sh_out("\r\n");
        return 0;
    }

    if(strcmp(argv[1], "check") == 0)
    {
        if(argc != 4)
        {
            sh_out("usage: devid check <hex-challenge> <hex-mac>\r\n");
            return -1;
        }
        if(devid_hex(argv[2], s_ch, sizeof(s_ch), &ch_len) != 0)
        {
            sh_out("devid: bad challenge hex\r\n");
            return -1;
        }
        if(devid_hex(argv[3], s_mac, sizeof(s_mac), &mac_len) != 0)
        {
            sh_out("devid: bad mac hex\r\n");
            return -1;
        }
        if(mac_len != (uint32)ARKRTOS_DEVID_MAC_LEN)
        {
            ark_shell_printf("devid: mac must be %u bytes, got %u\r\n",
                             (unsigned)ARKRTOS_DEVID_MAC_LEN, (unsigned)mac_len);
            return -1;
        }
        rc = arkrtos_devid_check(s_ch, ch_len, s_mac);
        if(rc == ARKRTOS_DEVID_OK)
        {
            sh_out("devid: MAC matches - this board holds that secret\r\n");
            return 0;
        }
        if(rc == ARKRTOS_DEVID_ERR_MISMATCH)
        {
            sh_out("devid: MAC does NOT match - different secret or different challenge\r\n");
            return -1;
        }
        if(rc == ARKRTOS_DEVID_ERR_NOKEY)
        {
            sh_out("devid: no usable secret here - cannot check\r\n");
            return -1;
        }
        ark_shell_printf("devid: check failed (%d)\r\n", rc);
        return -1;
    }

    sh_out("usage: devid [show] | prove <hex-challenge> | check <hex-challenge> <hex-mac>\r\n");
    return -1;
}

#endif /* ARKRTOS_USE_DEVICE_ID */

#endif /* SHELL_ENABLE */
