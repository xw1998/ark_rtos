/**
* @file arkrtos_shell.c
* @brief Ark RTOS 内核 Shell 控制台任务与内核命令集（控制台本体）
* @details 把 ark-shell（kernelsrc/shell/ark_shell/，上游原样引入）挂成一个
*          内核任务，并持有命令表与唯一的注册点。命令实现按族拆在同目录：
*            arkrtos_shell_image.c    info / app / drv / install
*            arkrtos_shell_diag.c     task / stack / sched / audit / guard /
*                                     crash / fault / log / syncinfo / rt /
*                                     sec / utest
*            arkrtos_shell_cfg.c      pool / cfg
*            arkrtos_shell_fs.c       blk / fs / pwd / cd / df / VFS 命令
*            arkrtos_shell_mod.c      exec / wait / ld / lkup / modules / unld
*            arkrtos_shell_pmap.c     pmap
*            arkrtos_shell_ushell.c   App 侧命令注册表与跳板（SVC 0x1A）
*            arkrtos_shell_misc.c     history / uname / free / ps / which /
*                                     man / dmesg / hwsafe
*          本文件只保留：控制台实例与栈、跨族共享的小工具、命令注册、
*          控制台任务本体。
*
*          「开机是否自启」在打包时写进镜像头（tools/pack_app.py --autostart），
*          运行期由 `app list` / `drv list` 的 auto 列展示；本 shell 只负责
*          运行期启停（start / stop），不改写自启标志。
* @note 本模块属于内核特权态代码。
*/

#include "arkrtos_shell_internal.h"
#if (SHELL_ENABLE == 1)

/* 控制台实例（含行编辑缓冲与命令历史，放 .bss 而不是任务栈） */
ark_shell_t g_shell;

/* 控制台任务栈（内核任务，取自内核 RAM）。
 * 板级可用 ARKRTOS_SHELL_STACK_SIZE 覆盖：这个任务要扛整条命令链
 * （解析 + VFS 读 + ld 的链接与打印），LA64 上一帧陷阱 256 B，
 * 3 KB 在 ld 期间实测被用到 100%。见 QEMU 文档 §5.15。 */
#ifndef ARKRTOS_SHELL_STACK_SIZE
#define ARKRTOS_SHELL_STACK_SIZE    SHELL_TASK_STACK_SIZE
#endif
static uint32 g_shell_stack[ARKRTOS_SHELL_STACK_SIZE / 4u];

/* ============================================================
 * 小工具
 * ============================================================ */

void sh_out(const char *s)
{
    platform_uart_send_string(s);
}

/* 解析十进制非负整数；非法返回 -1 */
int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }

    while(*s != '\0')
    {
        if((*s < '0') || (*s > '9'))
        {
            return -1;
        }
        v = v * 10u + (uint32)(*s - '0');
        s++;
    }

    *out = v;
    return 0;
}

const char *slot_state_name(uint32 st)
{
    switch(st)
    {
        case ARKRTOS_APP_SLOT_EMPTY:      return "EMPTY";
        case ARKRTOS_APP_SLOT_LOADED:     return "LOADED";
        case ARKRTOS_APP_SLOT_RUNNING:    return "RUNNING";
        case ARKRTOS_APP_SLOT_INVALID:    return "INVALID";
        case ARKRTOS_APP_SLOT_INSTALLING: return "INSTALL";
        case ARKRTOS_APP_SLOT_RAW:        return "RAW";
        default:                        return "?";
    }
}

const char *slot_type_name(uint32 t)
{
    switch(t)
    {
        case ARKRTOS_SLOT_APP:    return "app";
        case ARKRTOS_SLOT_DRIVER: return "drv";
        case ARKRTOS_SLOT_FREE:   return "free";
        default:                return "?";
    }
}

const char *cfg_mode_text(uint32 mode)
{
    if(mode == ARKRTOS_LAYOUT_MODE_FIXED)
    {
        return "fixed";
    }
    if(mode == ARKRTOS_LAYOUT_MODE_AUTO)
    {
        return "auto";
    }
    return "unknown";
}

const char *cfg_source_text(uint32 src)
{
    if(src == ARKRTOS_LAYOUT_SOURCE_CONFIG)
    {
        return "device config region";
    }
    if(src == ARKRTOS_LAYOUT_SOURCE_DEFAULT)
    {
        return "compile-time default";
    }
    return "unknown";
}

const char *cfg_reclaim_text(uint32 rm)
{
    if(rm == ARKRTOS_CFG_RECLAIM_GLOBAL)
    {
        return "global";
    }
    if(rm == ARKRTOS_CFG_RECLAIM_MINIMAL)
    {
        return "minimal";
    }
    if(rm == ARKRTOS_CFG_RECLAIM_NONE)
    {
        return "none";
    }
    return "unknown";
}

const char *cfg_type_text(uint32 t)
{
    if(t == ARKRTOS_CFG_TYPE_APP)
    {
        return "app";
    }
    if(t == ARKRTOS_CFG_TYPE_DRIVER)
    {
        return "drv";
    }
    return "-";
}

// moved up: cmd_pool (defined below) prints the fixed slot types too
const char *task_state_name(uint32 st)
{
    switch(st)
    {
        case ARKRTOS_TASK_INVALID: return "INVALID";
        case ARKRTOS_TASK_READY:   return "READY";
        case ARKRTOS_TASK_WAIT:    return "WAIT";
        case ARKRTOS_TASK_RUNNING: return "RUNNING";
        default:                 return "?";
    }
}

const char *fault_type_name(uint32 t)
{
    switch(t)
    {
        case ARKRTOS_FAULT_HARDFAULT:    return "HARDFAULT";
        case ARKRTOS_FAULT_STACKOVF:     return "STACKOVF";
        case ARKRTOS_FAULT_TASKKILL:     return "TASKKILL";
        case ARKRTOS_FAULT_RECOVER:      return "RECOVER";
        case ARKRTOS_FAULT_SCHEDLOCK:    return "SCHEDLOCK";
        case ARKRTOS_FAULT_APPDISABLED:  return "APPDISABLED";
        case ARKRTOS_FAULT_NOSLOT:       return "NOSLOT";
        case ARKRTOS_FAULT_MEMFAULT:     return "MEMFAULT";
        case ARKRTOS_FAULT_BUSFAULT:     return "BUSFAULT";
        case ARKRTOS_FAULT_USGFAULT:     return "USGFAULT";
        case ARKRTOS_FAULT_INSTALLFAIL:  return "INSTALLFAIL";
        default:                       return "?";
    }
}

static int register_kernel_commands(void)
{
    int idx = g_cmd_count;
    /* Upper bound with every optional feature compiled in.  Kept above the
     * real count so a future command cannot silently overflow g_cmd_table. */
    int need = 50   /* 48 shipped + "stack" + "pmap" (paging backend only) */
             + (ARKRTOS_USE_ARKVIM ? 1 : 0)
             + (ARKRTOS_USE_ARKC ? 1 : 0)
             + (ARKRTOS_USE_RTSTAT ? 1 : 0)
             + (ARKRTOS_USE_ELF ? 1 : 0)
             + (ARKRTOS_USE_MODLOAD ? 4 : 0)
             + (ARKRTOS_USE_SECCHECK ? 1 : 0)
             + (ARKRTOS_USE_PAGING ? 1 : 0)
             + (((ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)) ? 1 : 0)
             + ((ARKRTOS_USE_EXIT_CODE == 1) ? 1 : 0)
             + ((ARKRTOS_USE_SHELL_AUTH == 1) ? 2 : 0)
             + ((ARKRTOS_USE_ROLLBACK == 1) ? 1 : 0)
             + ((ARKRTOS_USE_SELFTEST == 1) ? 1 : 0)
             + ((ARKRTOS_USE_DEVICE_ID == 1) ? 1 : 0)
             + (ARKRTOS_USE_FDT ? 1 : 0);

    if((idx + need) > ARK_SHELL_MAX_COMMANDS)
    {
        return -1;
    }

    g_cmd_table[idx++] = ARK_SHELL_CMD("info", cmd_info,
        "Kernel, partition and task capacity info", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("app", cmd_app,
        "App slots: app [list | install <path> [slot] | start <slot> | stop <slot> | uninstall <slot>]", 4);
#if (ARKRTOS_USE_ELF == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("exec", cmd_exec,
        "Run an ELF64 executable: exec <path> [args...] (or just type /path/prog)", 8);
#endif /* ARKRTOS_USE_ELF */
#if (ARKRTOS_USE_EXIT_CODE == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("wait", cmd_wait,
        "Wait for a task to exit and report its exit code: wait <task id> [timeout ms]", 3);
#endif /* ARKRTOS_USE_EXIT_CODE */
#if (ARKRTOS_USE_MODLOAD == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("ld", cmd_ld,
        "Link and run a relocatable module: ld <path.o> [args...]", 8);
    g_cmd_table[idx++] = ARK_SHELL_CMD("lkup", cmd_lkup,
        "Kernel export table: lkup [symbol]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("modules", cmd_modules,
        "List resident modules (see also unld)", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("unld", cmd_unld,
        "Unload a finished module: unld <index>", 2);
#endif /* ARKRTOS_USE_MODLOAD */
    g_cmd_table[idx++] = ARK_SHELL_CMD("drv", cmd_drv,
        "Driver slots: drv [list | start <slot> | stop <slot> | uninstall <slot>]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("dt", cmd_dt,
        "Dump the compiled device tree (model, nodes, compatible, reg)", 1);
#if (ARKRTOS_USE_FDT == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("fdt", cmd_fdt,
        "Device tree: fdt | fdt path <abs> | fdt phandle <n> | fdt chosen | "
        "fdt get <path> <prop> | fdt overlay | fdt save <file> | "
        "fdt load <file>", 4);
#endif
    g_cmd_table[idx++] = ARK_SHELL_CMD("task", cmd_task,
        "List kernel tasks, or dump one task frame: task <id> (ids start at 1)", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("stack", cmd_stack,
        "Per-task stack usage, measured (fill pattern + low PSP): stack [check]", 2);
#if (ARKRTOS_USE_PAGING == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("pmap", cmd_pmap,
        "Paging: pmap [map|check|rd|rb|wrb|test|space|u1|elf|u2|look|unmap|dyn|cu] ...",
        6);
#endif /* ARKRTOS_USE_PAGING */
    g_cmd_table[idx++] = ARK_SHELL_CMD("sched", cmd_sched,
        "Scheduler readiness self-check (0 = consistent)", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("fault", cmd_fault,
        "Show recorded faults", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("install", cmd_install,
        "Open a one-shot install window on the console UART: install [slot]", 2);

#if ARKRTOS_USE_LOG
    g_cmd_table[idx++] = ARK_SHELL_CMD("log", cmd_log,
        "Get or set runtime log level: log [0..4]", 2);
#endif /* ARKRTOS_USE_LOG */
    g_cmd_table[idx++] = ARK_SHELL_CMD("syncinfo", cmd_syncinfo,
        "Dump semaphore / mutex / condition tables with their waiters", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("pool", cmd_pool,
        "Free space left in the image pool", 1);
#if ARKRTOS_USE_MDK_TRACE
    g_cmd_table[idx++] = ARK_SHELL_CMD("trace", arkrtos_trace_shell_cmd,
        "Kernel event trace: trace [start | stop | reset | dump | mark <n>]", 3);
#endif /* ARKRTOS_USE_MDK_TRACE */
    g_cmd_table[idx++] = ARK_SHELL_CMD("cfg", cmd_cfg,
        "Device layout config: cfg [show | load | clear]", 2);
#if ARKRTOS_USE_BLK
    g_cmd_table[idx++] = ARK_SHELL_CMD("blk", cmd_blk,
        "Block devices: blk [list | probe | rd | wr | erase | test]", 5);
#endif /* ARKRTOS_USE_BLK */

#if ARKRTOS_USE_FS
    g_cmd_table[idx++] = ARK_SHELL_CMD("fs", cmd_fs,
        "File system: fs [mount | unmount | format | info | ls | wr | rd | put | rm | test | err]", 9);
#endif /* ARKRTOS_USE_FS */
#if ARKRTOS_USE_VFS
    g_cmd_table[idx++] = ARK_SHELL_CMD("mount", cmd_vfs_mount,
        "List mount points, or: mount <target> | mount <dev> <off> <size> <target>", 5);
    g_cmd_table[idx++] = ARK_SHELL_CMD("umount", cmd_vfs_umount,
        "Drop a mount point: umount <target>", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("ls", cmd_vfs_ls,
        "List a directory: ls [-l] [-a] [path]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("cat", cmd_vfs_cat,
        "Print a file, or standard input: cat [path]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("rm", cmd_vfs_rm,
        "Remove files: rm <path>...", 8);
    g_cmd_table[idx++] = ARK_SHELL_CMD("mkdir", cmd_vfs_mkdir,
        "Create directories: mkdir <path>...", 8);
    g_cmd_table[idx++] = ARK_SHELL_CMD("rmdir", cmd_vfs_rmdir,
        "Remove empty directories: rmdir <path>...", 8);
    g_cmd_table[idx++] = ARK_SHELL_CMD("mv", cmd_vfs_mv,
        "Rename or move: mv <src> <dst>", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("cp", cmd_vfs_cp,
        "Copy a file: cp <src> <dst>", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("touch", cmd_vfs_touch,
        "Create empty files: touch <path>...", 8);
    g_cmd_table[idx++] = ARK_SHELL_CMD("stat", cmd_vfs_stat,
        "Show file type, mode, owner and size: stat <path>", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("chmod", cmd_vfs_chmod,
        "Change access bits: chmod <mode> <path>  (mode is octal, e.g. 644)", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("chown", cmd_vfs_chown,
        "Change numeric owner: chown <uid>[:<gid>] <path>", 3);
#endif /* ARKRTOS_USE_VFS */

#if ARKRTOS_USE_MINIAPP
    g_cmd_table[idx++] = ARK_SHELL_CMD("mini", cmd_mini,
        "MiniApp from the file system: mini [list] | mini run <path> | mini stop [n] |"
        " mini count [n] | mini limit [bytes] | mini forget <path> |"
        " mini autostart [list | add <path> | del <path> | now]", 4);
#endif /* ARKRTOS_USE_MINIAPP */
#if ARKRTOS_USE_AUDIT
    g_cmd_table[idx++] = ARK_SHELL_CMD("audit", cmd_audit,
        "Structure self-audit: partition / slots / tasks vs their invariants", 1);
#endif /* ARKRTOS_USE_AUDIT */
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("auditlog", cmd_auditlog,
        "Persistent audit trail: auditlog | auditlog dump [n] (oldest first)", 3);
#endif /* ARKRTOS_USE_AUDIT_LOG */
#if (ARKRTOS_USE_ROLLBACK == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("rollback", cmd_rollback,
        "Rollback whitelist: rollback [list] | arm <id> <ver> [uses] | clear", 4);
#endif /* ARKRTOS_USE_ROLLBACK */
#if (ARKRTOS_USE_SELFTEST == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("selftest", cmd_selftest,
        "Component self-test: selftest [all | ram | cpu | flash | clock | "
        "patrol | rebaseline]", 2);
#endif /* ARKRTOS_USE_SELFTEST */
#if (ARKRTOS_USE_FAULT_INJECT == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("fi", cmd_fi,
        "Fault injection / tamper watch: fi [list] | arm <alloc|task|tick> "
        "[n] | clear | test | watch <i> <addr> <len> | scan [i]", 5);
#endif /* ARKRTOS_USE_FAULT_INJECT */
#if (ARKRTOS_USE_DEVICE_ID == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("devid", cmd_devid,
        "Device identity (SEC-G13): devid | prove <hex-challenge> | "
        "check <hex-challenge> <hex-mac>", 4);
#endif /* ARKRTOS_USE_DEVICE_ID */

#if ARKRTOS_USE_KERNEL_GUARD
    g_cmd_table[idx++] = ARK_SHELL_CMD("guard", cmd_guard,
        "Watchdog state and per-slot heartbeat contracts", 1);
#endif /* ARKRTOS_USE_KERNEL_GUARD */

#if ARKRTOS_HW_SAFE
    g_cmd_table[idx++] = ARK_SHELL_CMD("hwsafe", cmd_hwsafe,
        "Hardware safe-state (will) ledger: owners, registered devices, neutralized count", 1);
#endif /* ARKRTOS_HW_SAFE */

#if (ARKRTOS_USE_TASK_SWAP == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("swap", cmd_swap,
        "Task hot-swap self-test: frame check / block move / frame rewrite / resume path", 3);
#endif /* ARKRTOS_USE_TASK_SWAP */

#if ARKRTOS_USE_CRASH_LOG
    g_cmd_table[idx++] = ARK_SHELL_CMD("crash", cmd_crash,
        "Cross-reset crash journal: boots, per-slot fault counts, held slots and"
        " disabled MiniApps", 1);
#endif /* ARKRTOS_USE_CRASH_LOG */

#if ARKRTOS_USE_SOFTBUS
    g_cmd_table[idx++] = ARK_SHELL_CMD("bus", arkrtos_softbus_shell_cmd,
        "Soft bus (ark_softbus): bus [info | selftest | pump [n] | peers | rx |"
        " pub <topic> <text> | rpc <text> | stream <n>]", 4);
#endif /* ARKRTOS_USE_SOFTBUS */

#if ARKRTOS_USE_MEMPOOL
    g_cmd_table[idx++] = ARK_SHELL_CMD("mpool", cmd_mpool,
        "Fixed-block memory pools: mpool [list | test]", 2);
#endif /* ARKRTOS_USE_MEMPOOL */
#if ARKRTOS_USE_UTEST
    g_cmd_table[idx++] = ARK_SHELL_CMD("utest", cmd_utest,
        "On-board unit tests: utest [list | run [pattern]]", 3);
#endif /* ARKRTOS_USE_UTEST */
#if (ARKRTOS_USE_RTSTAT == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("rt", cmd_rt,
        "Real-time latency (DWT cycles): rt [show | reset | run [n]]", 3);
#endif /* ARKRTOS_USE_RTSTAT */
#if (ARKRTOS_USE_SECCHECK == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("sec", cmd_sec,
        "Security baseline self-check: sec [all | pass | fail | find | skip]", 2);
#endif /* ARKRTOS_USE_SECCHECK */

    g_cmd_table[idx++] = ARK_SHELL_CMD("ps", cmd_ps,
        "List kernel tasks and installed images", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("pwd", cmd_pwd,
        "Print the working directory", 1);
#if ARKRTOS_USE_VFS
    g_cmd_table[idx++] = ARK_SHELL_CMD("cd", cmd_cd,
        "Change the working directory: cd [dir]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("df", cmd_df,
        "Show mount points and volume usage", 1);
#endif /* ARKRTOS_USE_VFS */
    g_cmd_table[idx++] = ARK_SHELL_CMD("history", cmd_history,
        "Show the command history", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("uname", cmd_uname,
        "System and build information: uname [-a]", 2);
#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
    g_cmd_table[idx++] = ARK_SHELL_CMD("lxstat", cmd_lxstat,
        "Linux ABI: totals + which syscall numbers were actually issued", 1);
#endif
    g_cmd_table[idx++] = ARK_SHELL_CMD("free", cmd_free,
        "Memory summary (kernel image pool + kernel RAM window)", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("which", cmd_which,
        "Resolve an alias to the real command: which <command>", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("man", cmd_man,
        "Show a command's help text: man <command>", 2);
#if ARKRTOS_USE_LOG
    g_cmd_table[idx++] = ARK_SHELL_CMD("dmesg", cmd_dmesg,
        "Print the kernel log ring: dmesg [-c]", 2);
#endif /* ARKRTOS_USE_LOG */

#if (ARKRTOS_USE_ARKVIM == 1)
    /* The editor owns its own port and file backend; all it needs from the
     * kernel is one slot in this table.  It returns idx unchanged when the
     * table is full, and the sentinel below then lands on the right slot. */
    idx = arkvim_shell_register(idx);
#endif

#if (ARKRTOS_USE_ARKC == 1)
    /* Same contract: the compiler brings its own port and file hooks and
     * only asks for one command table slot. */
    idx = arkc_shell_register(idx);
#endif

#if (ARKRTOS_USE_ARKMAKE == 1)
    /* ark make 只在编译器已经在场时才有意义（它调的就是 cc --pack），
     * 所以注册点紧跟在 cc 后面：读命令表的人能直接看出这个先后关系。 */
    idx = arkmake_shell_register(idx);
#endif

#if (ARKRTOS_SHELL_BOARD_CMDS == 1)
    /* Board-owned commands.  This is the only place where g_cmd_table is
     * final: ark_shell_init() has already rebuilt it from the builtin list,
     * so an entry appended here survives -- unlike one written during board
     * init, which the rebuild silently overwrites. */
    idx = arkrtos_board_shell_register(idx);
#endif

#if (ARKRTOS_USE_SHELL_AUTH == 1)
    /* Two slots, always present when the switch is on: the gate exempts them
     * while the console is locked, so they must exist in both states. */
    g_cmd_table[idx++] = ARK_SHELL_CMD("login", arkrtos_shell_auth_cmd_login,
        "Unlock the console: login <password>", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("logout", arkrtos_shell_auth_cmd_logout,
        "Lock the console again: logout", 1);
#endif /* ARKRTOS_USE_SHELL_AUTH */

    g_cmd_table[idx].name = NULL;
    g_cmd_table[idx].func = NULL;
    g_cmd_table[idx].help = NULL;
    g_cmd_table[idx].max_args = 0;
    g_cmd_count = idx;
    return 0;
}

/* ============================================================
 * 控制台任务
 * ============================================================ */
static void arkrtos_shell_task(void)
{
    /* ark_shell_init 内部会调用 platform_uart_init()，即打开控制台串口 */
    ark_shell_init(&g_shell, (uint32_t)SHELL_DEV_ARG);

#if (ARKRTOS_USE_SHELL_AUTH == 1)
    /* Boot starts locked.  ark_shell_init() has already printed the banner and
     * drawn a prompt; print the state on its own line and redraw the prompt,
     * so the operator sees why the first command will be refused instead of
     * having to guess from a "login: console is locked" reply. */
    arkrtos_shell_auth_init();
    ark_shell_set_gate(&g_shell, arkrtos_shell_auth_gate);
    sh_out("\r\nconsole: locked - type 'login <password>'\r\n");
    line_editor_redraw(&g_shell.editor);
#endif

    if(register_kernel_commands() != 0)
    {
        sh_out("shell: command table full, kernel commands unavailable\r\n");
    }

#if ARKRTOS_USE_VFS
    /* Arguments complete against the VFS; the shell core stays filesystem-
     * agnostic and only calls back here for non-first words. */
    ark_shell_set_completer(&g_shell, vfs_complete);
#endif

    /* No separate "console ready" line: the shell prints its own
     * welcome banner (see ark_shell_config.h) before the first prompt,
     * and the prompt is the real "ready" marker. */
    for(;;)
    {
        uint32 budget = (uint32)SHELL_RX_DRAIN_MAX;
        int    ch;

        /* Drain a bounded burst per round instead of a single byte.  One
         * byte per 2 ms caps the console at 500 B/s while the host pushes
         * a whole line in well under a millisecond: the receive FIFO fills
         * up, arkrtos_fifo_write() refuses the tail - the CR included - the
         * command never terminates and the console looks dead even though
         * the task is still scheduled and still transmitting.  Bounded, so
         * a human typing can never keep the task off the CPU. */
        while(budget-- > 0u)
        {
            ch = platform_uart_recv();
            if(ch < 0)
            {
                break;
            }
            ark_shell_process_byte(&g_shell, (unsigned char)ch);
        }

        /* A full line buffer means the byte stream was cut somewhere (RX
         * bytes dropped, or a host that never sent CR): such a line can
         * never be recognised, and every later byte only piles onto the
         * wreckage.  Drop the fragment and say so, instead of swallowing
         * every command from then on. */
        if(g_shell.editor.len >= (ARK_SHELL_LINE_SIZE - 1))
        {
            line_editor_clear(&g_shell.editor);
            g_shell.state = STATE_NORMAL;
            sh_out("\r\nconsole: line too long, discarded\r\n");
        }

        /* 让出 CPU：控制台优先级最低，不能让交互拖住业务任务 */
        arkrtos_task_wait_internal((uint32)SHELL_TASK_PERIOD_MS);
    }
}

int32 arkrtos_shell_init(void)
{
    return arkrtos_task_register(arkrtos_shell_task,
                               g_shell_stack,
                               (uint32)sizeof(g_shell_stack),
                               (uint8)SHELL_TASK_PRIORITY,
                               (uint32)SHELL_TASK_PERIOD_MS);
}

/**
* @brief Drop every user command registered by one task.
* @param task_id owner task id; 0 means "kernel", which owns nothing to drop
* @return number of entries released
* @details Called when a task is torn down (App stop / uninstall / overwrite,
*          fault kill, thread exit). Without this the name stays reserved
*          forever and a later image placed over the same address range would
*          pass the liveness check while pointing at a different function.
*/
int32 arkrtos_shell_release_task(uint32 task_id)
{
    uint32 i;
    int32  dropped = 0;

    if(task_id == 0u)
    {
        return 0;
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (g_ushell[i].owner_task != task_id))
        {
            continue;
        }

        ushell_drop(i);
        dropped++;
    }

    return dropped;
}

#else   /* SHELL_ENABLE == 0 */

int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd)
{
    (void)cmd;
    return ARKRTOS_USHELL_ERR_OFF;
}

int32 arkrtos_shell_ext_unregister(const char *name)
{
    (void)name;
    return ARKRTOS_USHELL_ERR_OFF;
}

int32 arkrtos_shell_release_task(uint32 task_id)
{
    (void)task_id;
    return 0;       /* nothing was ever registered: the shell is compiled out */
}


int32 arkrtos_shell_print_n(const char *msg, uint32 len)
{
    (void)msg;
    (void)len;
    return ARKRTOS_USHELL_ERR_OFF;
}


int32 arkrtos_shell_init(void)
{
    return 0;
}

#endif
