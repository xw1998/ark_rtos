/**
* @file main.c
* @brief Ark RTOS 在 QEMU `virt`（LoongArch64）上的验证示例
* @details 这不是一个"跑个空任务证明能启动"的最小例子，而是把内核真正会
*          依赖硬件的六条链路各自压一遍边：
*
*          1) **节拍**：TCFG 周期定时器每 500 us 进一次异常，
*             tick 与 ms 时间必须同步增长（读数不一致就说明节拍换算错了）；
*          2) **任务切换**：两个任务 + 空闲任务三方轮转，走「就地切换」
*             （LoongArch 没有可用软中断，见 port 文件头）完成，切换次数必须随节拍增长；
*          3) **同步原语**：信号量 create/post/wait 全部经 `syscall` 进内核，
*             这是对 syscall 指令编码解号与 ERA+4 的端到端验证；
*          4) **阻塞态唤醒**：任务用 arkrtos_task_wait() 挂进延时链，
*             到期由节拍唤醒 —— 走的是"异常里改调度状态、异常出口结算切换"
*             这条最容易出问题的组合路径；
*          5) **存储栈**：块设备 → littlefs → VFS → procfs，写在 RAM 介质上
*             读回逐字节一致，并以 NOR 语义负对照（1→0 才允许、0→1 必须被拒）
*             证明这条链不是"假成功"（见 docs/QEMU模拟器移植与验证.md §5.10）。
*          6) **控制台 Shell**：ttyS0 → ark_shell（行编辑 / 命令分发 / 历史 /
*             补全），收是 16550 轮询（本移植没接外部中断控制器）；
*
*          打印全部走板级轮询 ns16550a，不依赖 libc 的 printf。
*
* @note 启动顺序严格照 arkrtos_init.c 文件头的八步：
*       board_init → cfg_load → irq_init → kernel_module_init →
*       注册任务 → start_idle（内部再起节拍并开门闩）
* @author xw
*/

#include "arkrtos_cfg.h"
#include "arkrtos_init.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_sync.h"      /* 内核侧 *_internal 接口 */
#include "arkrtos_config.h"
#include "arkrtos.h"
#include "arkrtos_board.h"
#include "arkrtos_shell.h"     /* arkrtos_shell_init()：内核 Shell 控制台 */
#include "arkrtos_blk.h"
#include "arkrtos_smp.h"       /* SMP 自检：每核状态 / 在线掩码 / IPI（默认关时空头） */
#if (ARKRTOS_USE_SMP_SCHED == 1)
#include "arkrtos_smp_sched.h" /* 调度分核自检：次核运行队列 / 显式迁移（默认关时空头） */
#endif
#include "arkrtos_ptable.h"    /* 分区表：崩溃日志按槽位记账，必须先于它初始化 */
#include "arkrtos_crash.h"
#include "arkrtos_guard.h"   /* 心跳窗口守卫（ARKRTOS_USE_KERNEL_GUARD） */     /* 跨复位崩溃日志（ARKRTOS_USE_CRASH_LOG） */
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "arkrtos_elf.h"       /* ARKRTOS_USE_ELF：ELF64 装载器（M1） */
#include "drvblk.h"
#if (ARKRTOS_USE_FDT == 1)
#include "arkrtos_fdt.h"       /* MMU 档：启动时解析镜像里的标准 .dtb */
#endif
#if (ARKRTOS_USE_FDT_CONSOLE == 1)
#include "arkrtos_fdt_console.h" /* 消费 /chosen/stdout-path */
#include "arkrtos_fdt_res.h"   /* 把树当驱动配置源：时基/定时器口径核对 */
#endif

/* PLV3 探针与 ELF 装载器抢同一块 2 MB 窗口（0x02000000）：
 * 探针的代码/栈全在 .appprobe 里，而装载器要往同一页拷程序，先跑探针再
 * 覆盖它等于让程序踩正在执行的代码。所以二选一——本板打开 ELF 装载器时
 * 不注册探针任务，PLV3 的现场证据改由跑在 PLV3 的 ELF 程序给出
 * （它只能走 SVC，读内核页会 PPI）。 */
#define ARK_EXAMPLE_PLV3_PROBE   ((ARKRTOS_USE_PRIV == 1) && (ARKRTOS_USE_ELF == 0))

/* 演示任务：把编进镜像的 hello.elf 写进 ramfs 再跑（见 elf_demo.c）。 */
#if (ARKRTOS_USE_ELF == 1)
extern int32 arkrtos_elf_demo_register(void);
#endif
/* 演示任务：把编进镜像的 modhello.o 写进 ramfs 再现场链接跑（见 mod_demo.c）。
 * 与上面的 ELF 演示**并列**：两条路都跑一遍，才能在串口上直接对照
 * 「Linux 式（PLV3、定址）」与「VxWorks 式（PLV0、装载期定址）」的差别。 */
#if (ARKRTOS_USE_MODLOAD == 1)
extern int32 arkrtos_mod_demo_register(void);
#endif

/* 任务栈：LoongArch 没有硬件压栈，只有软件帧，512 字（4 KB）很宽裕。
 * 栈必须是静态的（QEMU 上也没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
/* 实测深度 984 B（在线水位 low=top-984，另有 QEMU monitor 直读逐字节核对，
 * 见 docs/QEMU模拟器移植与验证.md §5.13.4 复核）：这块栈在启动之后仍要承接
 * 空闲上下文的陷阱帧与调用深度。1 KB（256 字）实测只剩 28 B 余量、128 字实测
 * 溢出（守卫报 context stack overflow），故取 1024 字 = 4 KB 留 3 KB 余量。 */
#define IDLE_STACK_WORDS     1024u
#define FS_STACK_WORDS       1024u   /* 存储链路实测最深的栈（见 s_stack_fs） */

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
/* 存储任务单独给 4 KB：littlefs + VFS 这条链是本示例里最深的调用栈。
 * 栈不够的后果不是"变慢"，是回到别处的一个随机指针，所以按实测留余量。 */
static uint32 s_stack_fs[FS_STACK_WORDS];

/* 容量读数用的静态缓冲：640 KiB 正好比**旧整卷**（512 KiB）大，也大于
 * 真实发行版静态 hello 的 623728 B。任务栈只有 4 KB，所以缓冲必须是静态的；
 * 内容按偏移现算、读回来现比，不在内存里留第二份"原文"。 */
#define FS_CAP_BYTES           (640u * 1024u)
static uint8 s_cap_buf[FS_CAP_BYTES];
static uint32 s_idle_stack[IDLE_STACK_WORDS];
#if (ARKRTOS_USE_SMP_SCHED == 1)
static uint32 s_stack_chk[TASK_STACK_WORDS];
#endif

static int32 s_sem = -1;

/* 统计变量：让读数本身可被观察，而不是"看起来在跑" */
static volatile uint32 s_a_loops = 0u;
static volatile uint32 s_b_loops = 0u;

/**
* @brief 任务 A：周期性打印节拍与毫秒时间，并释放一次信号量
* @details 500 ms 一个周期。两次打印之间的差值就是节拍读数，能直接看出
*          定时器是不是按 25 MHz / 500 us 在走。
*/
static void task_a(void)
{
    uint32 tick_prev = 0u;
    uint32 ms_prev   = 0u;
    uint32 round     = 0u;

    for(;;)
    {
        uint32 tick = arkrtos_kernel_get_tick();
#if !defined(ARKRTOS_QEMU_DEMO_QUIET) || (ARKRTOS_QEMU_DEMO_QUIET == 0)
        /* 毫秒由节拍换算，不经 SVC：内核任务跑在内核态，SVC 是给 App 的
         * 入口；在调度器内部拿 SVC 换毫秒只是多一次 syscall 往返。 */
        uint32 ms   = (tick * ARKRTOS_TICK_PERIOD_US) / 1000u;

        arkrtos_board_puts("[A] round=");
        arkrtos_board_putu(round);
        arkrtos_board_puts(" tick=");
        arkrtos_board_putu(tick);
        arkrtos_board_puts(" d_tick=");
        arkrtos_board_putu(tick - tick_prev);
        arkrtos_board_puts(" ms=");
        arkrtos_board_putu(ms);
        arkrtos_board_puts(" d_ms=");
        arkrtos_board_putu(ms - ms_prev);

        tick_prev = tick;
        ms_prev   = ms;
        round++;
        s_a_loops++;

        /* SVC 通路自检。内核任务跑在内核态，本可以直接读节拍；这里每轮
         * 仍走一次 syscall（arkrtos_get_time_ms），把 「异常 → SVC_Server →
         * 返回 a0」整条链也当成被测对象：它坏了，ms_svc 就会和 ms 对不上。 */
        {
            uint32 ms_svc = arkrtos_get_time_ms();

            arkrtos_board_puts(" ms_svc=");
            arkrtos_board_putu(ms_svc);
            arkrtos_board_puts((ms_svc + 2u < ms || ms + 2u < ms_svc) ? " SVC_MISMATCH" : " svc_ok");
            arkrtos_board_puts("\n");
        }
#endif

        /* 释放信号量：唤醒任务 B */
        (void)arkrtos_sem_post_internal(s_sem);

        /* 阻塞 500 ms：挂进延时链，由节拍唤醒 */
        arkrtos_task_wait_internal(500u);
    }
}

/**
* @brief 任务 B：等信号量，拿到就打印一次
* @details 与 A 的 500 ms 周期一一对应。B 的 round 落后 A 一拍是正常的
*          （A 先 post 再 sleep，B 被唤醒后要等下一次调度）。
*/
static void task_b(void)
{
    uint32 round = 0u;

    for(;;)
    {
        if(arkrtos_sem_wait_internal(s_sem, -1) == 0)
        {
            round++;
            s_b_loops++;

#if !defined(ARKRTOS_QEMU_DEMO_QUIET) || (ARKRTOS_QEMU_DEMO_QUIET == 0)
            arkrtos_board_puts("[B] woke, round=");
            arkrtos_board_putu(round);
            arkrtos_board_puts(" tick=");
            arkrtos_board_putu(arkrtos_kernel_get_tick());
            arkrtos_board_puts("\n");
#endif
        }
    }
}

/* ============================================================
 * 任务 D：存储栈自检（块设备 → littlefs → VFS → procfs）
 * @details 这一条链路是「全功能」考题里最容易被跳过的一段：不做它，示例里
 *          只剩调度器与两个同步原语，"文件系统能挂载、能读写、路径能解析"
 *          全都只是文档上的话。四道读数：
*
*          1) 块设备：ram0 注册成功且能起得来（见 board/qemu_loongarch64/drvblk.c）；
*          2) littlefs：format 一个 4 MB 的卷，再 mount 上去（挂载不格式化，
*             所以必须先 format，这是它自己的契约）；
*          3) VFS：把同一个卷挂到 /mnt/ram，按路径写一个文件再读回来逐字节对比；
*          4) procfs：读 /proc/version，证明 VFS 的另一个驱动也在岗。
*
*          另加一条**负对照**：对已经写过的偏移再写一次（不擦），块设备必须
*          回 -1。这是 NOR 语义"只能 1→0"的现场证据，也是"写成功"这个读数
*          唯一能证明自己不是空转的地方。
*
* @note 本任务跑一次就阻塞到底。它必须在内核模块初始化之后（VFS 与文件系统
*       的模块状态在那里建），且在调度启动之后（它要用普通阻塞——
*       这里用 arkrtos_task_wait）。
 * ============================================================ */
static volatile uint32 s_fs_ok = 0u;

static int fs_list_cb(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    (void)arg;

    arkrtos_board_puts("  ");
    arkrtos_board_puts(name);
    arkrtos_board_puts(((st != 0) &&
                       ((st->mode & ARKRTOS_VFS_S_IFMT) == ARKRTOS_VFS_S_IFDIR)) ? "/" : "");
    arkrtos_board_puts(" size=");
    arkrtos_board_putu((st != 0) ? (uint32)st->size : 0u);
    arkrtos_board_puts("\n");

    return 0;
}

static void task_fs(void)
{
    const arkrtos_blk_dev_t *dev;
    uint8 rbuf[64];
    uint32 n = 0u;
    uint32 i;
    uint32 diff = 0u;
    int32 rc;
    const char *msg = "ark la64 vfs ok\n";
    uint32 ok = 1u;

    /* ---- 1) 块设备 ---- */
    dev = arkrtos_blk_find(ARKRTOS_QEMU_BLK_NAME);
    arkrtos_board_puts("[FS] blk ");
    arkrtos_board_puts(ARKRTOS_QEMU_BLK_NAME);
    arkrtos_board_puts(" found=");
    arkrtos_board_putu((dev != 0) ? 1u : 0u);
    if(dev == 0)
    {
        arkrtos_board_puts(" -> no storage, skip\n");
        for(;;) { arkrtos_task_wait(1000u); }
    }

    rc = arkrtos_blk_init(dev);
    arkrtos_board_puts(" init=");
    arkrtos_board_putu((uint32)(rc == 0 ? 1u : 0u));
    arkrtos_board_puts(" size=");
    arkrtos_board_putu(arkrtos_blk_size(dev));
    arkrtos_board_puts(" erase=");
    arkrtos_board_putu(ARKRTOS_QEMU_BLK_ERASE);
    arkrtos_board_puts("\n");

    /* ---- 2) littlefs：format 再 mount ---- */
    rc = arkrtos_fs_format(ARKRTOS_QEMU_BLK_NAME, 0u, (uint32)ARKRTOS_QEMU_BLK_SIZE);
    arkrtos_board_puts("[FS] format rc=");
    arkrtos_board_putu((uint32)rc);
    if(rc != 0)
    {
        arkrtos_board_puts(" last_error=");
        arkrtos_board_putu((uint32)arkrtos_fs_last_error());
        arkrtos_board_puts(" -> FAIL\n");
        ok = 0u;
    }
    arkrtos_board_puts("\n");

    /* format 成功时会自己把卷挂上；接下来走 VFS 的挂载点，所以先摘掉，
     * 让 arkrtos_vfs_mount_volume() 按同一窗口重新挂一次（它会把几何对齐核对）。 */
    if(ok != 0u)
    {
        (void)arkrtos_fs_unmount();
    }

    /* ---- 3) VFS：/mnt/ram ---- */
    if(ok != 0u)
    {
        rc = arkrtos_vfs_init();
        arkrtos_board_puts("[FS] vfs_init rc=");
        arkrtos_board_putu((uint32)rc);

        rc = arkrtos_vfs_mount_volume(ARKRTOS_QEMU_BLK_NAME, 0u,
                                     (uint32)ARKRTOS_QEMU_BLK_SIZE, "/mnt/ram");
        arkrtos_board_puts(" mount rc=");
        arkrtos_board_putu((uint32)rc);
        arkrtos_board_puts(" mounts=");
        arkrtos_board_putu((uint32)arkrtos_vfs_mount_count());
        arkrtos_board_puts("\n");
        if(rc != 0)
        {
            ok = 0u;
        }
    }

    /* ---- 3b) 按路径写一个文件，再读回来逐字节对比 ---- */
    if(ok != 0u)
    {
        rc = arkrtos_vfs_write_file("/mnt/ram/hello.txt", (const uint8 *)msg, 17u);
        arkrtos_board_puts("[FS] write rc=");
        arkrtos_board_putu((uint32)rc);

        rc = arkrtos_vfs_read_file("/mnt/ram/hello.txt", rbuf, (uint32)sizeof(rbuf), &n);
        arkrtos_board_puts(" read rc=");
        arkrtos_board_putu((uint32)rc);
        arkrtos_board_puts(" len=");
        arkrtos_board_putu(n);

        /* 逐字节对比：长度不对或内容不对都算失败，不给"差不多对"的结论。
         * 自己写循环而不是调 memcmp：本目标 -nostdlib，字符串函数全部来自
         * libc_shim.c，这里不需要任何技巧。 */
        diff = 0u;
        if(n != 17u)
        {
            diff = 1u;
        }
        else
        {
            for(i = 0u; i < n; i++)
            {
                if(rbuf[i] != (uint8)msg[i]) { diff = 1u; break; }
            }
        }
        if(diff != 0u)
        {
            ok = 0u;
            arkrtos_board_puts(" -> MISMATCH");
        }
        else
        {
            arkrtos_board_puts(" -> match");
        }
        arkrtos_board_puts("\n");
    }

    /* ---- 3b2) 容量：写一个比旧整卷还大的文件，读回来逐字节核对 ----
     * 这条读数存在的理由很具体：真实发行版二进制（本机龙芯 glibc 工具链
     * 编出的静态 hello 623728 B、它依赖的 libc.so.6 1571208 B）根本塞不进
     * 旧卷。这里不看容量宏说什么，只看"真的写进去、真的读回来、字节对得上"。
     * 负对照：长度或内容任一不符都判 FAIL，不给"差不多"的结论。 */
    if(ok != 0u)
    {
        uint32 ncap = 0u;
        uint32 bad  = 0u;
        uint32 rcw;

        for(i = 0u; i < FS_CAP_BYTES; i++)
        {
            s_cap_buf[i] = (uint8)((i * 31u + 7u) & 0xFFu);
        }
        rcw = (uint32)arkrtos_vfs_write_file("/mnt/ram/cap.bin", s_cap_buf,
                                             (uint32)FS_CAP_BYTES);
        arkrtos_board_puts("[FS] cap write rc=");
        arkrtos_board_putu(rcw);
        arkrtos_board_puts(" bytes=");
        arkrtos_board_putu((uint32)FS_CAP_BYTES);

        for(i = 0u; i < FS_CAP_BYTES; i++)
        {
            s_cap_buf[i] = 0u;
        }
        if(rcw == 0u)
        {
            (void)arkrtos_vfs_read_file("/mnt/ram/cap.bin", s_cap_buf,
                                        (uint32)FS_CAP_BYTES, &ncap);
        }
        arkrtos_board_puts(" read len=");
        arkrtos_board_putu(ncap);

        if((rcw != 0u) || (ncap != (uint32)FS_CAP_BYTES))
        {
            bad = 1u;
        }
        else
        {
            for(i = 0u; i < FS_CAP_BYTES; i++)
            {
                if(s_cap_buf[i] != (uint8)((i * 31u + 7u) & 0xFFu))
                {
                    bad = 1u;
                    break;
                }
            }
        }
        if(bad != 0u)
        {
            ok = 0u;
            arkrtos_board_puts(" -> CAP MISMATCH");
        }
        else
        {
            arkrtos_board_puts(" -> match");
        }
        arkrtos_board_puts("\n");
    }

    /* ---- 3c) 列目录 + 读 procfs ---- */
    if(ok != 0u)
    {
        arkrtos_board_puts("[FS] ls /mnt/ram:\n");
        (void)arkrtos_vfs_list("/mnt/ram", fs_list_cb, 0);

        rc = arkrtos_vfs_read_file("/proc/version", rbuf, (uint32)sizeof(rbuf) - 1u, &n);
        if((rc == 0) && (n > 0u))
        {
            if(n > 40u) { n = 40u; }
            arkrtos_board_puts("[FS] /proc/version: ");
            for(i = 0u; i < n; i++)
            {
                arkrtos_board_putc((char)(((rbuf[i] >= 0x20u) && (rbuf[i] < 0x7Fu)) ? rbuf[i] : (uint8)'.'));
            }
            arkrtos_board_puts("\n");
        }
        else
        {
            arkrtos_board_puts("[FS] /proc/version rc=");
            arkrtos_board_putu((uint32)rc);
            arkrtos_board_puts(" -> FAIL\n");
            ok = 0u;
        }
    }

    /* ---- 4) 负对照：不擦就覆写，NOR 语义必须拒绝 ---- */
    {
        const uint8 probe[4] = { 0x11u, 0x22u, 0x33u, 0x44u };
        uint32 rej_before = arkrtos_board_blk_write_rejected();

        /* 卷的首个 4 字节已经被 littlefs 写过，此刻不可能是 0xFF */
        rc = arkrtos_blk_write(dev, 0u, probe, 4u);
        arkrtos_board_puts("[FS] overwrite-without-erase rc=");
        arkrtos_board_putu((uint32)rc);
        arkrtos_board_puts(" rejected=");
        arkrtos_board_putu(arkrtos_board_blk_write_rejected() - rej_before);
        if(rc != -1)
        {
            arkrtos_board_puts(" -> UNEXPECTED: NOR semantics not enforced");
            ok = 0u;
        }
        arkrtos_board_puts("\n");
    }

    arkrtos_board_puts("[FS] counters written=");
    arkrtos_board_putu(arkrtos_board_blk_bytes_written());
    arkrtos_board_puts(" rejected=");
    arkrtos_board_putu(arkrtos_board_blk_write_rejected());
    arkrtos_board_puts(" erases=");
    arkrtos_board_putu(arkrtos_board_blk_erase_count());
    arkrtos_board_puts("\n");

    s_fs_ok = ok;
    arkrtos_board_puts(ok ? "[FS] PASS\n" : "[FS] FAIL\n");

    for(;;)
    {
        arkrtos_task_wait(1000u);
    }
}

/* ============================================================
 * 任务 E：PLV3 App 保护自检（分页 + TLB 条目 PLV 的现场证据）
 * @details 这个任务以 is_priv = 0 注册，于是 port 在返回它的帧时把
 *          PRMD.PPLV 写成 3，由 ertn 落到 CRMD.PLV = 3。它与内核的隔离
 *          完全来自 TLB 条目的 PLV 位（内核页 PLV0、App 页 PLV3），
 *          不是靠「什么都没给」。
 *
 *          四条探针，每条都必须「被拒」才算通过：
 *            round 1：读 0x00300000（内核 RAM，PLV0 页）=> PPI(7)
 *            round 2：读 0x05000000（没有任何条目）    => refill(1)
 *            round 3：csrrd CRMD（特权 CSR）           => IPE(14)
 *            round 4：syscall 取时间 + 延时            => 正常返回
 *          被拒之后内核把本任务原地重启并重建栈帧，所以 round 会接着涨 ——
 *          连续几条 [U] round=N 本身就是「拦住了 + 恢复了」的证据。
 *
 *          两个必须写在代码里的前提：
 *          1) **App 的代码/数据/字符串全部放 .appprobe**。这一节被链接到
 *             0x02000000（App 页），页条目是 PLV3；内核的 .text/.rodata
 *             在 PLV0 页里，App 连取指都过不去。字面量若落 .rodata，
 *             被测的就不是保护机制了。
 *          2) **App 不能调内核封装**（arkrtos_get_time_ms 之类是 PLV0 代码），
 *             只能自己内联 syscall 指令。
 * @note 已知边界（见 docs/QEMU模拟器移植与验证.md 的 LA64 PLV3 小节）：
 *       App 页整页 RWX（不做 W^X）；没有页表遍历 / 按需换页 / swap；
 *       App 的 sp 由内核给的栈顶决定，App 自己改坏了就是它自己的事。
 * ============================================================ */
#if (ARKRTOS_USE_PRIV == 1)

/* App 页里的一切都带这一节属性。noinline 是必需的：一个被内联进内核
 * 函数的 App 辅助函数会落在内核页里，App 一调就 PPI。
 * 代码 / 只读常量 / 可写数据分三个节名：汇编器不允许同名节带两种类型
 * （"ax" 与 "aw" 撞在一起会直接报 section type conflict）。三个名字都被
 * 链接脚本收进同一个输出段 .appprobe。 */
#define LA_APP_DATA     __attribute__((section(".appprobe"), used))
#define LA_APP_RODATA   __attribute__((section(".appprobe.rodata"), used))
#define LA_APP_CODE     __attribute__((section(".appprobe.text"), used, noinline))
#define LA_APP_STR(n,t) static LA_APP_RODATA const char n[] = t

/* 探针地址。第一个是内核 RAM 的首地址（PLV0 页，内核稍后会读成功一次），
 * 第二个在 16 MB 以上、没有任何条目覆盖。 */
#define LA_APP_KERNEL_ADDR   0x00300000u
#define LA_APP_UNMAPPED      0x05000000u

#define LA_APP_STACK_WORDS   1024u

static LA_APP_DATA uint32 s_app_stack[LA_APP_STACK_WORDS];
/* round 必须活过「故障 -> 重建栈帧 -> 重启」，所以不能放栈上 */
static LA_APP_DATA volatile uint32 s_app_round = 0u;
/* 装载自证：内核读这个值等于魔数，说明 ELF 的 .appprobe 段真的被
 * QEMU 按 p_paddr 装进了 0x02000000（否则读到的是 DRAM 复位值）。 */
LA_APP_RODATA const unsigned long la_app_magic = 0x4C413634UL;

LA_APP_STR(la_s_round,      "[U] round=");
LA_APP_STR(la_s_nl,         "\n");
LA_APP_STR(la_s_kern_probe, "[U] probe: read kernel page 0x00300000 ...\n");
LA_APP_STR(la_s_unm_probe,  "[U] probe: read unmapped 0x05000000 ...\n");
LA_APP_STR(la_s_csr_probe,  "[U] probe: csrrd CRMD (privileged) ...\n");
LA_APP_STR(la_s_unexpect,   "[U] UNEXPECTED: no protection, value=");
LA_APP_STR(la_s_svc_ms,     " ms=");
LA_APP_STR(la_s_svc_ok,     " svc_ok\n");
LA_APP_STR(la_s_fatal,      "[U] FATAL: protection not enforced, halted\n");

/* 10 的幂表：App 里不用除法（不依赖 libgcc 的 __udivsi3） */
static LA_APP_RODATA const uint32 la_p10[10] = {
    1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
    10000u, 1000u, 100u, 10u, 1u
};

/* UART 用 PLV3 别名页：0x02200000 页内 0x1E0 对应 PA 0x1FE001E0 */
#define LA_APP_UART_PTR  ((volatile uint8 *)(unsigned long)ARKRTOS_QEMU_APP_UART)

static LA_APP_CODE void app_putc(char c)
{
    /* LSR(5) bit5 = THR 空 */
    while((LA_APP_UART_PTR[5] & 0x20u) == 0u)
    {
    }
    LA_APP_UART_PTR[0] = (uint8)c;
}

static LA_APP_CODE void app_puts(const char *str)
{
    while(*str != 0)
    {
        app_putc(*str);
        str++;
    }
}

static LA_APP_CODE void app_putu(uint32 val)
{
    uint32 i;
    uint32 started = 0u;

    for(i = 0u; i < 10u; i++)
    {
        uint32 digit = 0u;

        while(val >= la_p10[i])
        {
            val -= la_p10[i];
            digit++;
        }
        if((digit != 0u) || (started != 0u) || (i == 9u))
        {
            app_putc((char)('0' + digit));
            started = 1u;
        }
    }
}

static LA_APP_CODE void app_puthex(uint32 val)
{
    int32 i;
    static LA_APP_RODATA const char hex[] = "0123456789abcdef";

    app_puts("0x");
    for(i = 7; i >= 0; i--)
    {
        app_putc(hex[(val >> (uint32)(i * 4)) & 0xFu]);
    }
}

/* syscall 的号码编在指令立即数里（见 port 文件头），子功能在 a0、参数在 a1。
 * 返回值由内核写回帧里的 a0，所以这里 a0 既是入参也是出参。 */
static LA_APP_CODE uint32 app_get_time_ms(void)
{
    register unsigned long r_a0 __asm__("$a0") = (unsigned long)ARKRTOS_SYS_SUB_TIME;

    __asm__ volatile("syscall 0x12" : "+r"(r_a0) : : "memory");

    return (uint32)r_a0;
}

static LA_APP_CODE void app_delay_us(uint32 us)
{
    register unsigned long r_a0 __asm__("$a0") = (unsigned long)ARKRTOS_TASK_SUB_DELAY;
    register unsigned long r_a1 __asm__("$a1") = (unsigned long)us;

    __asm__ volatile("syscall 0x11" : "+r"(r_a0), "+r"(r_a1) : : "memory");
}

/* 让出 CPU 的等待（SUB_WAIT，单位毫秒）。与 SUB_DELAY 的区别是实测出来的：
 * SUB_DELAY 是 port 里的忙等，而 SVC 在 trap 里执行时 CRMD.IE=0 —— 忙等
 * 全程关中断，节拍走不动。实测 App 每轮忙等 500 ms，主机过了 20 s 而
 * arkrtos_get_time_ms() 只前进 37 ms（丢掉了约 39 个节拍）。所以「读数
 * 随时间增长」这条正对照必须靠 SUB_WAIT（真正阻塞让出），忙等那条只留
 * 一个 2 ms 的轻探针证明它没坏。 */
static LA_APP_CODE void app_wait_ms(uint32 ms)
{
    register unsigned long r_a0 __asm__("$a0") = (unsigned long)ARKRTOS_TASK_SUB_WAIT;
    register unsigned long r_a1 __asm__("$a1") = (unsigned long)ms;

    __asm__ volatile("syscall 0x11" : "+r"(r_a0), "+r"(r_a1) : : "memory");
}

static LA_APP_CODE void la_app_task(void)
{
    uint32 round = s_app_round + 1u;

    s_app_round = round;

    app_puts(la_s_round);
    app_putu(round);
    app_puts(la_s_nl);

    if(round == 1u)
    {
        volatile uint32 *probe = (volatile uint32 *)(unsigned long)LA_APP_KERNEL_ADDR;

        app_puts(la_s_kern_probe);
        (void)*probe;                       /* 这一句必须 PPI，永不返回 */
        app_puts(la_s_unexpect);            /* 走到这里就是没有隔离 */
        app_puts(la_s_fatal);
        for(;;) { }
    }

    if(round == 2u)
    {
        volatile uint32 *probe = (volatile uint32 *)(unsigned long)LA_APP_UNMAPPED;

        app_puts(la_s_unm_probe);
        (void)*probe;                       /* 这一句必须 refill，永不返回 */
        app_puts(la_s_unexpect);
        app_puts(la_s_fatal);
        for(;;) { }
    }

    if(round == 3u)
    {
        unsigned long crmd = 0ul;

        app_puts(la_s_csr_probe);
        __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd) : : "memory");  /* 必须 IPE */
        app_puts(la_s_unexpect);
        app_puthex((uint32)crmd);
        app_puts(la_s_fatal);
        for(;;) { }
    }

    /* round >= 4：只剩「合法路径也通」这条正对照。写成循环而不是只跑一轮，
     * 是为了让读数随时间增长 —— 静止的数字说明不了任何事。 */
    for(;;)
    {
        app_puts(la_s_round);
        app_putu(round);
        app_puts(la_s_svc_ms);
        app_putu(app_get_time_ms());
        app_puts(la_s_svc_ok);

        round++;
        s_app_round = round;

        app_delay_us(2000u);                /* 忙等通路：2 ms 轻探针 */
        app_wait_ms(500u);                  /* 让出 CPU：500 ms */
    }
}

#endif /* ARKRTOS_USE_PRIV == 1 */

/**
* @brief 进入空闲上下文并启动调度
* @details 顺序不可换：
*          1) enter_idle：把 sp 换到空闲栈（必须是汇编）；
*          2) start_timer：节拍开始走；
*          3) switch_enable：打开切换门闩并立刻置一次软件中断，
*             第一个任务在这一步之后才开始运行；
*          4) 之后进入 WFI 循环，CPU 只在有中断时醒来。
*/
#if (ARKRTOS_USE_STACK_GUARD == 1)
/* 栈底守卫的演示任务：先把自己的栈底保护字写坏，再主动让出 CPU。
 * 守卫打开时应该看到一行 [STK] task N: ... 且该任务被摘除（task 里 INVALID）；
 * 守卫关闭时这整段不参与编译，行为与之前逐字节相同。
 * 写的是它自己的栈底字，不会碰到别人的栈。 */
#define STKGUARD_STACK_WORDS   512u   /* 与 A/B 同档：LA64 一帧 256 B + 调用链 */
static uint32 s_stack_guard_demo[STKGUARD_STACK_WORDS];

static void task_stack_guard_demo(void)
{
    volatile uint32 *bottom;

    arkrtos_task_delay(200u);      /* 等 A/B 先跑起来，串口顺序好读 */

    bottom = (volatile uint32 *)(unsigned long)s_stack_guard_demo;
    *bottom = 0u;                  /* 模拟一次越界写：保护字不再是 0xed01 */

    arkrtos_board_puts("[boot] stack guard demo: bottom word clobbered\n");

    for(;;)
    {
        arkrtos_task_delay(100u);  /* 下一次切走时由守卫发现并摘除 */
    }
}
#endif

#if (ARKRTOS_USE_SMP == 1)
/* ============================================================
 * SMP 自检（只在 ARKRTOS_USE_SMP=1 的镜像里编进来）
 * @details 判据是**串口上的原始读数**，不是"编过了"：
 *            1) 等在线掩码长成"每核一位"（有界自旋，超时如实报 FAIL）；
 *            2) 给每个非引导核发 5 次 IPI，等它的 ipi_count 涨到 5。
 *          自旋里不用 WFI：此刻定时器还没启动，idle 下去没人唤醒。
 *          用 arkrtos_port_spin_hint()（nop）让 QEMU 的另一个 vCPU 线程
 *          有机会被调度。
 * ============================================================ */
#define SMP_SELF_TEST_IPI     5u
#define SMP_SELF_TEST_SPIN    20000000u

static void arkrtos_smp_self_test(void)
{
    uint32 cpu;
    uint32 i;
    uint32 full;
    uint32 online = 0u;
    uint32 sent_ok = 1u;
    uint32 spin;

#if (ARKRTOS_CPU_NUM < 2)
    arkrtos_board_puts("[smp] cores=1 ipi=0 result=skip(no secondary)\n");
    return;
#else
    full = ((uint32)1u << (uint32)ARKRTOS_CPU_NUM) - 1u;

    for(i = 0u; i < SMP_SELF_TEST_SPIN; i++)
    {
        online = arkrtos_smp_online_mask_get();
        if(online == full)
        {
            break;
        }
        arkrtos_port_spin_hint();
    }

    arkrtos_board_puts("[smp] cores=");
    arkrtos_board_putu((uint32)ARKRTOS_CPU_NUM);
    arkrtos_board_puts(" mask=");
    arkrtos_board_puthex(online);
    if(online != full)
    {
        arkrtos_board_puts(" ipi=0 result=FAIL(secondary offline)\n");
        return;
    }

    /* 一发一等地发：连续用同一个向量号发多次，硬件会把它们合并成一次
     * 中断（QEMU 与真机同语义：status 只置一位），5 发只数到 1 次会被
     * 误读成"通路坏了"。逐个发、逐个等计数涨上去，才是真的 5 次往返。 */
    for(cpu = 1u; cpu < (uint32)ARKRTOS_CPU_NUM; cpu++)
    {
        for(i = 1u; i <= SMP_SELF_TEST_IPI; i++)
        {
            if(arkrtos_smp_send_ipi(cpu) != 0)
            {
                sent_ok = 0u;
                break;
            }
            for(spin = 0u; spin < SMP_SELF_TEST_SPIN; spin++)
            {
                if(arkrtos_smp_cpu[cpu].ipi_count >= i)
                {
                    break;
                }
                arkrtos_port_spin_hint();
            }
            if(arkrtos_smp_cpu[cpu].ipi_count < i)
            {
                break;                    /* 这一发没到，不再追后面的 */
            }
        }
    }

    arkrtos_board_puts(" sent=");
    arkrtos_board_putu(sent_ok);
    for(cpu = 1u; cpu < (uint32)ARKRTOS_CPU_NUM; cpu++)
    {
        arkrtos_board_puts(" ipi");
        arkrtos_board_putu(cpu);
        arkrtos_board_puts("=");
        arkrtos_board_putu(arkrtos_smp_cpu[cpu].ipi_count);
    }

    if(sent_ok != 0u)
    {
        for(cpu = 1u; cpu < (uint32)ARKRTOS_CPU_NUM; cpu++)
        {
            if(arkrtos_smp_cpu[cpu].ipi_count < SMP_SELF_TEST_IPI)
            {
                sent_ok = 0u;
                break;
            }
        }
    }

    if(sent_ok != 0u)
    {
        arkrtos_board_puts(" result=ok\n");
    }
    else
    {
        arkrtos_board_puts(" result=FAIL(ipi not received)\n");
    }
#endif
}
#endif /* ARKRTOS_USE_SMP == 1 */

#if (ARKRTOS_USE_SMP_SCHED == 1)
/* ============================================================
 * 调度分核自检（只在 ARKRTOS_USE_SMP_SCHED=1 的镜像里编进来）
 * @details 判据同样是**串口上的原始读数**，不是"编过了"：
 *          1) 把一个纯计算任务迁到次核（arkrtos_smp_sched_task_to）；
 *          2) 在 CPU0 上读它的循环计数——它只可能由次核推进，因为 CPU0
 *             的就绪链里已经没有它了；
 *          3) 读次核的 cur/切换/迁入迁出计数，确认它真的挂在次核上；
 *          4) 要回 CPU0（arkrtos_smp_sched_task_back），确认归属回到 0、
 *             次核空闲、计数继续涨（这次是 CPU0 在跑它）。
 *
 *          **这个任务不调任何内核 API**：次核没有跨核保护，同步对象 /
 *          文件系统 / 设备表都不能碰；它只累加自己的计数，延时用
 *          arkrtos_port_delay_us（纯 rdtime 忙等，不碰全局结构）。
 * ============================================================ */

static volatile uint32 s_w_loops = 0u;
static uint32 s_stack_w[TASK_STACK_WORDS];
static int32  s_id_w = -1;

static void task_w(void)
{
    for(;;)
    {
        s_w_loops++;
        arkrtos_port_delay_us(200u);
    }
}

static void task_smp_chk(void)
{
    uint32 out[8];
    uint32 prev;
    uint32 round;

    for(round = 1u; round <= 6u; round++)
    {
        arkrtos_task_wait_internal(300u);

        prev = s_w_loops;
        arkrtos_task_wait_internal(200u);

        (void)arkrtos_smp_sched_selftest(out);

        arkrtos_board_puts("[sched] r=");
        arkrtos_board_putu(round);
        arkrtos_board_puts(" cur1=");
        arkrtos_board_puthex(out[0]);
        arkrtos_board_puts(" q=");
        arkrtos_board_putu(out[1]);
        arkrtos_board_puts(" sw=");
        arkrtos_board_putu(out[2]);
        arkrtos_board_puts(" in=");
        arkrtos_board_putu(out[3]);
        arkrtos_board_puts(" out=");
        arkrtos_board_putu(out[4]);
        arkrtos_board_puts(" t=");
        arkrtos_board_putu(out[5]);
        arkrtos_board_puts(" owned=");
        arkrtos_board_putu(out[7]);
        arkrtos_board_puts(" loops=+");
        arkrtos_board_putu(s_w_loops - prev);
        arkrtos_board_puts("\n");

        if(round == 3u)
        {
            int32 rc = arkrtos_smp_sched_task_back(s_id_w);

            arkrtos_board_puts("[sched] back rc=");
            arkrtos_board_putu((uint32)rc);
            arkrtos_board_puts("\n");
        }
    }
}
#endif /* ARKRTOS_USE_SMP_SCHED == 1 */

#if (ARKRTOS_DEMO_HWSAFE == 1)
/* ============================================================
 * A5：遗言机制在非 ARM 上的三条通道（QEMU 算上板，不是真机）
 * @details 三个任务的生死形态各不相同，因此三条通道互不顶替：
 *            task_hw_dead_will —— 开 led0、留遗言（ctrl 0 = 关），正常退出；
 *            task_hw_dead_cb   —— 开 led1、**不留遗言**，正常退出
 *                                 （内核必须退回驱动注册的安全态回调）；
 *            task_hw_hold      —— 开 led2 并一直活着（台账里始终有所有者，
 *                                 证明中和只针对死者）。
 *          读完应看到 hwsafe: devs=3 owners=1 neutralized=2 (will=1 cb=1)，
 *          以及 led0=0 led1=0 led2=1。
 * ============================================================ */
#include "arkrtos_hwsafe.h"
#include "arkrtos_dev.h"   /* 内核侧 *_internal：演示任务跑在内核态 */

extern uint32 arkrtos_board_hw_demo_safe_hits(void);
extern uint32 arkrtos_board_hw_demo_level(uint32 idx);

static uint32 s_hw_stack_dead_will[TASK_STACK_WORDS];
static uint32 s_hw_stack_dead_cb[TASK_STACK_WORDS];
static uint32 s_hw_stack_hold[TASK_STACK_WORDS];

static void task_hw_dead_will(void)
{
    /* 这三个演示任务是**内核任务**，名字/参数块都在内核 Flash 里，走 App 侧
     * （SVC）的 arkrtos_dev_open() 会被 arkrtos_kernel_user_name_ok() 按
     * 「不在用户 RAM / 固件窗口内」拒掉。所以这里用内核侧 *_internal 入口，
     * 与 task_fs / shell 打开 ttyS0 的方式一致。 */
    int32 h = arkrtos_dev_open_internal("led0", 0u);
    int32 rc = arkrtos_hwsafe_set_will("led0", arkrtos_current_task_id, 0, 0);

    arkrtos_board_puts("[hwsafe] task ");
    arkrtos_board_putu((uint32)arkrtos_current_task_id);
    arkrtos_board_puts(" opened led0 level=");
    arkrtos_board_putu(arkrtos_board_hw_demo_level(0u));
    arkrtos_board_puts(" will_rc=");
    arkrtos_board_putu((uint32)rc);
    arkrtos_board_puts(" handle=");
    arkrtos_board_putu((uint32)h);
    arkrtos_board_puts(" -> exits normally\n");

    arkrtos_task_kill_internal();
    for(;;)
    {
    }
}

static void task_hw_dead_cb(void)
{
    int32 h = arkrtos_dev_open_internal("led1", 0u);

    arkrtos_board_puts("[hwsafe] task ");
    arkrtos_board_putu((uint32)arkrtos_current_task_id);
    arkrtos_board_puts(" opened led1 level=");
    arkrtos_board_putu(arkrtos_board_hw_demo_level(1u));
    arkrtos_board_puts(" no will, handle=");
    arkrtos_board_putu((uint32)h);
    arkrtos_board_puts(" -> exits normally\n");

    arkrtos_task_kill_internal();
    for(;;)
    {
    }
}

static void task_hw_hold(void)
{
    int32 h = arkrtos_dev_open_internal("led2", 0u);

    arkrtos_board_puts("[hwsafe] task ");
    arkrtos_board_putu((uint32)arkrtos_current_task_id);
    arkrtos_board_puts(" holds led2 (live owner), handle=");
    arkrtos_board_putu((uint32)h);
    arkrtos_board_puts("\n");

    arkrtos_task_wait_internal(500u);

    arkrtos_board_puts("[hwsafe] after deaths: devs=");
    arkrtos_board_putu((uint32)arkrtos_hwsafe_dev_count());
    arkrtos_board_puts(" owners=");
    arkrtos_board_putu((uint32)arkrtos_hwsafe_owner_count());
    arkrtos_board_puts(" neutralized=");
    arkrtos_board_putu(arkrtos_hwsafe_neutralized_count());
    arkrtos_board_puts(" safe_hits=");
    arkrtos_board_putu(arkrtos_board_hw_demo_safe_hits());
    arkrtos_board_puts(" led0=");
    arkrtos_board_putu(arkrtos_board_hw_demo_level(0u));
    arkrtos_board_puts(" led1=");
    arkrtos_board_putu(arkrtos_board_hw_demo_level(1u));
    arkrtos_board_puts(" led2=");
    arkrtos_board_putu(arkrtos_board_hw_demo_level(2u));
    arkrtos_board_puts("\n");

    for(;;)
    {
        arkrtos_task_wait_internal(1000u);
    }
}
#endif /* ARKRTOS_DEMO_HWSAFE */

static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(unsigned long)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0xFu;      /* LoongArch 栈 16 字节对齐（ABI 硬要求） */

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 必须在 enter_idle 之前布防：这一刻 idle 栈还没被当作栈用，整段填图案
     * 是安全的；进入之后 main 的活帧就搬到这块栈上了。 */
    arkrtos_stack_guard_watch_context((uint32)(unsigned long)&s_idle_stack[0],
                                      (uint32)sizeof(s_idle_stack), sp);
    #endif

    arkrtos_port_enter_idle(sp, 1u);

    arkrtos_port_start_timer(ARKRTOS_TICK_PERIOD_US);

    arkrtos_port_switch_enable();

    for(;;)
    {
#if (ARKRTOS_USE_TICKLESS == 1)
        /* 与 ARM 三个例程同一入口；TCFG 由 start_timer_counts 重新编程成
         * 一段更长的周期，醒来按实际经过的时间补拍。 */
        (void)arkrtos_tickless_idle();
#else
        ARKRTOS_WFI();
#endif
    }
}

int main(void)
{
    int32 id_a;
    int32 id_b;
    int32 id_fs;
    int32 id_shell;
#if ARK_EXAMPLE_PLV3_PROBE
    int32 id_u;
#endif

    arkrtos_port_board_init();

    arkrtos_cfg_load();

    /* 任务先注册、再开中断：irq_init() 一开 mstatus.MIE，节拍就可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);
    /* 存储任务优先级低于 A/B：它是自检，不该抢占节拍/切换观测 */
    id_fs = arkrtos_task_register(task_fs, s_stack_fs, sizeof(s_stack_fs), 13u, 0u);

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 栈底守卫演示任务：优先级最低，只在 A/B/存储/装载都跑过之后踩自己的栈底。 */
    (void)arkrtos_task_register(task_stack_guard_demo, s_stack_guard_demo,
                                sizeof(s_stack_guard_demo), 18u, 0u);
    #endif

    #if ARK_EXAMPLE_PLV3_PROBE
    /* PLV3 探针。栈在 App 页（.appprobe）里，代码也在那里 ——
     * 注册时内核只按地址造帧，所以这两点都由链接脚本保证。 */
    id_u = arkrtos_task_register(la_app_task, s_app_stack, sizeof(s_app_stack), 12u, 0u);
    if(id_u > 0)
    {
        /* 内核注册出来的任务默认 is_priv = 1，必须显式降级，
         * 否则 port 会按 PLV0 放它下去，探针就都在内核态跑（假绿）。 */
        arkrtos_task_table[id_u - 1].is_priv = 0u;
    }
    #endif

    #if (ARKRTOS_USE_SMP_SCHED == 1)
    /* 调度分核演示：一个纯计算任务 + 一个 CPU0 侧的观测任务。
     * worker 优先级 16（低于 A/B/存储/ELF），迁走之后 CPU0 上谁也不会
     * 因为它的优先级而受影响。 */
    s_id_w = arkrtos_task_register(task_w, s_stack_w, sizeof(s_stack_w), 16u, 0u);
    (void)arkrtos_task_register(task_smp_chk, s_stack_chk, sizeof(s_stack_chk), 9u, 0u);
    #endif

    #if (ARKRTOS_USE_ELF == 1)
    /* ELF 装载演示：优先级 14（比存储任务 13 低），等 ramfs 挂上再写文件。 */
    (void)arkrtos_elf_demo_register();
    #endif

    #if (ARKRTOS_USE_MODLOAD == 1)
    /* 模块装载演示：优先级 15，排在 ELF 演示之后，输出顺序读得通。 */
    (void)arkrtos_mod_demo_register();
    #endif

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

#if (ARKRTOS_USE_FDT == 1)
    /* 设备树：MMU 档按正常项目的做法，镜像里带一块标准 .dtb，启动时解析。
     * 解不出来就打印真实错误码并继续（不拿 C 平表凑一棵"看起来对"的树）——
     * 把"dtb 坏了"变成"设备悄悄少了一个"是更坏的失败方式。 */
    {
        uint32 dtb_len = 0u;
        const void *dtb = arkrtos_board_dtb(&dtb_len);
        int32 rc = arkrtos_fdt_init(dtb, dtb_len);

        arkrtos_board_puts("[fdt] blob=");
        arkrtos_board_putu(dtb_len);
        arkrtos_board_puts(" rc=");
        if(rc < 0)
        {
            arkrtos_board_puts("-");
            arkrtos_board_putu((uint32)(0 - rc));
        }
        else
        {
            arkrtos_board_putu((uint32)rc);
        }
        if(rc == ARKRTOS_FDT_OK)
        {
            const char *model = arkrtos_fdt_model();

            arkrtos_board_puts(" model=");
            arkrtos_board_puts((model != 0) ? model : "(none)");
            arkrtos_board_puts(" nodes=");
            arkrtos_board_putu(arkrtos_fdt_node_count());
        }
        else
        {
            arkrtos_board_puts(" (blob refused, tree NOT parsed)");
        }
        arkrtos_board_puts("\n");
    }
#endif

#if (ARKRTOS_USE_FDT_CONSOLE == 1)
    /* 消费 /chosen/stdout-path：树说控制台是哪个设备，这里核对它是否就是
     * 镜像按板级常量起来的这一个。一致才叫绑上；不一致保留板级默认并打出
     * 原因码——树与板级矛盾是配置错误，不能悄悄挑一个继续。 */
    {
        arkrtos_console_expect_t exp;
        arkrtos_console_bind_t   bind;
        int32 crc;

        exp.compatible = "ns16550a";
        exp.reg_base = ARKRTOS_QEMU_UART_BASE;
        exp.clock_hz = ARKRTOS_QEMU_UART_CLK;
        crc = arkrtos_console_from_fdt(&exp, &bind);
        arkrtos_board_puts("[fdt] console path=");
        arkrtos_board_puts((bind.path[0] != 0) ? bind.path : "(none)");
        arkrtos_board_puts(" baud=");
        arkrtos_board_putu(bind.baud);
        arkrtos_board_puts(" clk=");
        arkrtos_board_putu(bind.clock_hz);
        arkrtos_board_puts(" reason=");
        arkrtos_board_puts(arkrtos_console_reason(crc));
        arkrtos_board_puts(" match=");
        arkrtos_board_putu((crc == ARKRTOS_CONSOLE_OK) ? 1u : 0u);
        arkrtos_board_puts("\n");
    }
#endif

#if (ARKRTOS_USE_FDT_RES == 1)
    /* 把树当驱动配置源：内核按 ARKRTOS_PORT_RDTIME_HZ 算节拍，树里的
     * /cpus/timebase-frequency 必须与它一致，否则时间口径有两套，症状是
     * 时间走得快/慢而看起来像调度器坏了。本板定时器 TCFG 是 CPU 内部 CSR，
     * 树里没有对应内存窗口节点，因此 timer_compat=0（不核对定时器节点）；
     * 这不是"漏了一项"，而是该硬件本来就没有可描述的 reg。
     * 只读：不改定时器、不改节拍。 */
    {
        arkrtos_fdt_res_expect_t rexp;
        arkrtos_fdt_res_t        res;
        int32 rrc;

        rexp.expect_timebase = ARKRTOS_PORT_RDTIME_HZ;
        rexp.timer_compat = 0;
        rexp.expect_timer_reg = 0u;
        rexp.expect_timer_hz = 0u;
        rrc = arkrtos_fdt_res_check(&rexp, &res);
        arkrtos_board_puts("[fdt] res timebase=");
        arkrtos_board_putu(res.timebase);
        arkrtos_board_puts(" expect=");
        arkrtos_board_putu(ARKRTOS_PORT_RDTIME_HZ);
        arkrtos_board_puts(" timer=");
        arkrtos_board_puts((res.has_timer != 0) ? res.timer_node : "(none described)");
        arkrtos_board_puts(" rc=");
        arkrtos_board_puts(arkrtos_fdt_res_reason(rrc));
        arkrtos_board_puts("\n");
    }
#endif

#if (ARKRTOS_USE_SMP == 1)
    /* 次核是在 kernel_module_init() 里拉起来的，这里等它上线并发 IPI 验证。
     * 放在任何任务注册/启动之前：失败就在串口上一行看清，不埋在后续输出里。 */
    arkrtos_smp_self_test();
#endif

#if (ARKRTOS_USE_SMP_SCHED == 1)
    /* 把 worker 迁到 1 号核。此刻 worker 还是 READY（调度尚未启动），
     * 是迁移 API 唯一接受的形态；迁完之后 CPU0 的就绪链里没有它。 */
    if(s_id_w > 0)
    {
        int32 rc = arkrtos_smp_sched_task_to(s_id_w, 1u);

        arkrtos_board_puts("[sched] to cpu1 rc=");
        arkrtos_board_putu((uint32)rc);
        arkrtos_board_puts("\n");
    }
#endif

    /* 分区表 + 跨复位崩溃日志。
     * 顺序与 STM32 工程一致（见 example/stm32f427 的 arkrtos_kernel_init）：
     * 分区表先就位，崩溃日志才能按槽位记账；crash_init() 只校验/播种日志，
     * 不碰池扫描；scan_apply() 把日志里记着的「已禁用」落回槽位记录。
     * 日志关闭时两者都是空实现，这两行的代价是零。 */
    arkrtos_ptable_init();
#if (ARKRTOS_USE_CRASH_LOG == 1)
    arkrtos_crash_init();
    arkrtos_crash_scan_apply();
#endif

    /* 内核守护：先声明「谁应该按时心跳」，再由任务自己打拍（arkrtos_guard_beat）。
     * 开关关闭时 init/beat 都是空实现，这里的代价是零。 */
#if (ARKRTOS_USE_KERNEL_GUARD == 1)
    arkrtos_guard_init();
#endif

    /* 块设备驱动在这里先注册（只登记描述符，不碰介质）；真正 init 交给
     * task_fs —— 让"注册"和"起来"两件事各有一次可读的打印。 */
    (void)arkrtos_board_blk_init();

#if (ARKRTOS_DEMO_HWSAFE == 1)
    /* A5 读数装置：三个生死形态不同的任务（优先级比所有自检都低，
     * 只在别人跑完之后依次上演，不干扰节拍/切换读数）。 */
    (void)arkrtos_task_register(task_hw_dead_will, s_hw_stack_dead_will,
                                sizeof(s_hw_stack_dead_will), 20u, 0u);
    (void)arkrtos_task_register(task_hw_dead_cb, s_hw_stack_dead_cb,
                                sizeof(s_hw_stack_dead_cb), 21u, 0u);
    (void)arkrtos_task_register(task_hw_hold, s_hw_stack_hold,
                                sizeof(s_hw_stack_hold), 22u, 0u);
#endif

    /* 信号量表由 kernel_module_init() 里的同步模块初始化，所以在这里创建 */
    s_sem = arkrtos_sem_create_internal("qemu_sem", 0);
    if(s_sem < 0)
    {
        arkrtos_board_puts("[boot] sem_create FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    /* Shell 控制台任务：优先级 / 栈 / 周期取 config/arkrtos_partition.h 的
     * SHELL_TASK_*（14 / 3 KB / 2 ms）。它打开的 ttyS0 由板级
     * arkrtos_dev_board_init() 在 kernel_module_init() 里注册，
     * 所以这一步必须在 kernel_module_init() 之后。 */
    id_shell = arkrtos_shell_init();

    arkrtos_board_puts("[boot] sem=");
    arkrtos_board_putu((uint32)s_sem);
    arkrtos_board_puts(" taskA=");
    arkrtos_board_putu((uint32)id_a);
    arkrtos_board_puts(" taskB=");
    arkrtos_board_putu((uint32)id_b);
    arkrtos_board_puts(" taskFS=");
    arkrtos_board_putu((uint32)id_fs);
    arkrtos_board_puts(" blk=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_BLK);
    arkrtos_board_puts(" fs=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_FS);
    arkrtos_board_puts(" vfs=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_VFS);
    arkrtos_board_puts(" shell=");
    arkrtos_board_putu((uint32)id_shell);
    arkrtos_board_puts(" log=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_LOG);
    #if ARK_EXAMPLE_PLV3_PROBE
    arkrtos_board_puts(" taskU=");
    arkrtos_board_putu((uint32)id_u);
    #endif
    #if (ARKRTOS_USE_PRIV == 1)
    arkrtos_board_puts(" app_base=");
    arkrtos_board_puthex(ARKRTOS_QEMU_APP_BASE);
    arkrtos_board_puts(" elf=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_ELF);
    #endif
    #if (ARKRTOS_USE_CRASH_LOG == 1)
    arkrtos_board_puts(" crasharea=");
    arkrtos_board_puthex(arkrtos_crash_area_base());
    arkrtos_board_puts(" boots=");
    arkrtos_board_putu(arkrtos_crash_boots());
    #endif
    arkrtos_board_puts(" utest=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_UTEST);
    arkrtos_board_puts(" tick_period_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    #if ARK_EXAMPLE_PLV3_PROBE
    if(id_u < 0)
    {
        arkrtos_board_puts("[boot] PLV3 probe task register FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }
    #endif
    #if ((ARKRTOS_USE_PRIV == 1) && (ARKRTOS_USE_ELF == 1))
    arkrtos_board_puts("[PRIV] PLV3 probe task skipped: ARKRTOS_USE_ELF owns "
                       "the app window\n");
    #endif

    if((id_a < 0) || (id_b < 0) || (id_fs < 0) || (id_shell < 0))
    {
        arkrtos_board_puts("[boot] task register FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    #if (ARKRTOS_USE_PRIV == 1)
    /* 两条正对照，都在 PLV0 侧做，作用是让「App 被拒」这件事有参照：
     *  a) 内核读 0x00300000（同一页，App 读它必须 PPI）—— 读得到说明
     *     映射在、是特权级拒绝而不是「没映射」；
     *  b) 读 App 页里的装载魔数 —— 等于定值说明 .appprobe 被 QEMU 按
     *     p_paddr 装进了 0x02000000，App 的代码真的在那里。 */
    arkrtos_board_puts("[boot] paging on: kernel read 0x00300000=");
    arkrtos_board_puthex(*(volatile uint32 *)(unsigned long)0x00300000u);
    arkrtos_board_puts(" app_magic=");
    arkrtos_board_puthex((uint32)la_app_magic);
    arkrtos_board_puts((((uint32)la_app_magic) == 0x4C413634u) ? " (probe image loaded)\n"
                                                             : " -> APP IMAGE NOT LOADED\n");
    #endif

    arkrtos_start_idle();

    return 0;
}
