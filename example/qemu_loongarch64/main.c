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
#include "arkrtos_ptable.h"    /* 分区表：崩溃日志按槽位记账，必须先于它初始化 */
#include "arkrtos_crash.h"
#include "arkrtos_guard.h"   /* 心跳窗口守卫（ARKRTOS_USE_KERNEL_GUARD） */     /* 跨复位崩溃日志（ARKRTOS_USE_CRASH_LOG） */
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "drvblk.h"

/* 任务栈：LoongArch 没有硬件压栈，只有软件帧，512 字（4 KB）很宽裕。
 * 栈必须是静态的（QEMU 上也没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
#define IDLE_STACK_WORDS     256u
#define FS_STACK_WORDS       1024u   /* 存储链路实测最深的栈（见 s_stack_fs） */

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
/* 存储任务单独给 4 KB：littlefs + VFS 这条链是本示例里最深的调用栈。
 * 栈不够的后果不是"变慢"，是回到别处的一个随机指针，所以按实测留余量。 */
static uint32 s_stack_fs[FS_STACK_WORDS];
static uint32 s_idle_stack[IDLE_STACK_WORDS];

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

            arkrtos_board_puts("[B] woke, round=");
            arkrtos_board_putu(round);
            arkrtos_board_puts(" tick=");
            arkrtos_board_putu(arkrtos_kernel_get_tick());
            arkrtos_board_puts("\n");
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
*          2) littlefs：format 一个 512 KB 的卷，再 mount 上去（挂载不格式化，
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
static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(unsigned long)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0xFu;      /* LoongArch 栈 16 字节对齐（ABI 硬要求） */

    arkrtos_port_enter_idle(sp, 1u);

    arkrtos_port_start_timer(ARKRTOS_TICK_PERIOD_US);

    arkrtos_port_switch_enable();

    for(;;)
    {
        ARKRTOS_WFI();
    }
}

int main(void)
{
    int32 id_a;
    int32 id_b;
    int32 id_fs;
    int32 id_shell;
#if (ARKRTOS_USE_PRIV == 1)
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

    #if (ARKRTOS_USE_PRIV == 1)
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

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

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
    #if (ARKRTOS_USE_PRIV == 1)
    arkrtos_board_puts(" taskU=");
    arkrtos_board_putu((uint32)id_u);
    arkrtos_board_puts(" app_base=");
    arkrtos_board_puthex(ARKRTOS_QEMU_APP_BASE);
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

    #if (ARKRTOS_USE_PRIV == 1)
    if(id_u < 0)
    {
        arkrtos_board_puts("[boot] PLV3 probe task register FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }
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
