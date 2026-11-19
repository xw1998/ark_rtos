/**
* @file main.c
* @brief Ark RTOS 在 LoongArch32 参考目标上的示例（**只编译/链接，不运行**）
* @details 与 la64 的 QEMU 示例同构：功能面、任务划分、自检判据一致，区别只有
*          架构与「没有执行载体」这一个事实。
*
*          ## 验证状态（必读）
*
*          **这套代码没有任何 LA32 机器跑过。** QEMU 9.2.3 只提供
*          qemu-system-loongarch64，上游没有 loongarch32 系统模拟器；本机也
*          没有 LA32 板子。所以下面六组链路全部只到「编译 + 链接」这一档：
*          能证明的是「32 位帧布局 / 指令选择 / 参数传递构成一份完整 ELF」，
*          **不是「能跑」**。保留完整示例而不写成空壳，是为了让编译面与实际
*          使用面一致：真正接上 LA32 载体时，第一件事就是照 la64 的方式看
*          这六组读数。
*
*          la64 上已验证、本目标待验证的六条链路：
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
*          5) **存储栈**：块设备（RAM 模拟 NOR）→ littlefs → VFS → procfs，
*             写在介质上读回逐字节一致，并以 NOR 语义负对照（1→0 才允许、
*             0→1 必须被拒）证明这条链不是"假成功"；
*          6) **控制台 Shell**：ttyS0 → ark_shell（行编辑 / 命令分发 / 历史 /
*             补全），收是 16550 轮询（本移植没接外部中断控制器）；
*
*          打印全部走板级轮询 ns16550a，不依赖 libc 的 printf：也就是说,这套
*          读数只有在真的接上载体时才会第一次出现在串口上。
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
#include "arkrtos_ptable.h"
#include "arkrtos_crash.h"
#include "arkrtos_guard.h"   /* 心跳窗口守卫（ARKRTOS_USE_KERNEL_GUARD） */
#include "arkrtos_blk.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "drvblk.h"

/* 任务栈：LoongArch 没有硬件压栈，只有软件帧。LA32 的寄存器与帧槽是 32 位
 * （帧 128 字节，见 port 文件头），512 字（2 KB）；与 la64 同构留足余量。
 * 栈必须是静态的（本目标同样没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
/* la32 没有可用的系统模拟器（QEMU 9.2.3 不提供 la32），本目标无法实测，
 * 取与 la64 同一档：la64 实测深度 984 B，1024 字 = 4 KB 留 3 KB 余量
 * （docs/QEMU模拟器移植与验证.md §5.13.4 复核）。 */
#define IDLE_STACK_WORDS     1024u
#define FS_STACK_WORDS       1024u   /* 存储链路实测最深的栈（见 s_stack_fs） */

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
/* 存储任务单独给 4 KB：littlefs + VFS 这条链是本示例里最深的调用栈。
 * 栈不够的后果不是"变慢"，是回到别处的一个随机指针；这里按 la64 的实测
 * 留同一份余量（本目标无法实测深度）。 */
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
*          1) 块设备：ram0 注册成功且能起得来（见 board/la32_generic/drvblk.c）；
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
* @warning 这一整段在 LA32 上**从未被执行**：判据本身没有被验证过，能证明的
*          只有「这条调用链在 32 位 ABI 下编得过、链得上」。
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
    const char *msg = "ark la32 vfs ok\n";
    uint32 ok = 1u;

    /* ---- 1) 块设备 ---- */
    dev = arkrtos_blk_find(ARKRTOS_LA32_BLK_NAME);
    arkrtos_board_puts("[FS] blk ");
    arkrtos_board_puts(ARKRTOS_LA32_BLK_NAME);
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
    arkrtos_board_putu(ARKRTOS_LA32_BLK_ERASE);
    arkrtos_board_puts("\n");

    /* ---- 2) littlefs：format 再 mount ---- */
    rc = arkrtos_fs_format(ARKRTOS_LA32_BLK_NAME, 0u, (uint32)ARKRTOS_LA32_BLK_SIZE);
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

        rc = arkrtos_vfs_mount_volume(ARKRTOS_LA32_BLK_NAME, 0u,
                                     (uint32)ARKRTOS_LA32_BLK_SIZE, "/mnt/ram");
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
        ARKRTOS_WFI();
    }
}

int main(void)
{
    int32 id_a;
    int32 id_b;
    int32 id_fs;
    int32 id_shell;

    arkrtos_port_board_init();

    arkrtos_cfg_load();

    /* 任务先注册、再开中断：irq_init() 一开 mstatus.MIE，节拍就可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);
    /* 存储任务优先级低于 A/B：它是自检，不该抢占节拍/切换观测 */
    id_fs = arkrtos_task_register(task_fs, s_stack_fs, sizeof(s_stack_fs), 13u, 0u);

    arkrtos_port_irq_init();

    arkrtos_kernel_module_init();

    /* 分区表 + 跨复位崩溃日志（顺序与 STM32 工程一致）。日志关闭时两者都是
     * 空实现，这两行的代价是零。 */
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
    s_sem = arkrtos_sem_create_internal("la32_sem", 0);
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
     * 所以这一步必须在 kernel_module_init() 之后。
     * 本目标上 arkrtos_shell_init() 只被链接过：没有任务真的跑起来过。 */
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
    arkrtos_board_puts(" utest=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_UTEST);
#if (ARKRTOS_USE_CRASH_LOG == 1)
    arkrtos_board_puts(" crasharea=");
    arkrtos_board_puthex(arkrtos_crash_area_base());
    arkrtos_board_puts(" boots=");
    arkrtos_board_putu(arkrtos_crash_boots());
#endif
    arkrtos_board_puts(" tick_period_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    if((id_a < 0) || (id_b < 0) || (id_fs < 0) || (id_shell < 0))
    {
        arkrtos_board_puts("[boot] task register FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    arkrtos_start_idle();

    return 0;
}
