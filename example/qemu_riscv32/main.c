/**
* @file main.c
* @brief Ark RTOS 在 QEMU `virt`（RISC-V RV32）上的验证示例
* @details 这不是一个"跑个空任务证明能启动"的最小例子，而是把内核真正会
*          依赖硬件的四条链路各自压一遍边：
*
*          1) **节拍**：CLINT 机器定时器每 500 us 进一次 trap，
*             tick 与 ms 时间必须同步增长（读数不一致就说明节拍换算错了）；
*          2) **任务切换**：两个任务 + 空闲任务三方轮转，靠 CLINT 软件中断
*             （PendSV 的对应物）完成，切换次数必须随节拍增长；
*          3) **同步原语**：信号量 create/post/wait 全部经 `ecall` 进内核，
*             这是对 trap 帧 a7/a0 约定与 mepc+4 的端到端验证；
*          4) **阻塞态唤醒**：任务用 arkrtos_task_wait() 挂进延时链，
*             到期由节拍唤醒 —— 走的是"trap 里改调度状态、软件中断里换栈"
*             这条最容易出问题的组合路径；
*          5) **U 模式 + PMP 内存保护**：任务 C 以 is_priv = 0 运行，
*             它读 CLINT（不在任何 PMP 窗口里）必须被打回并重启 ——
*             这条负对照是整套保护机制唯一的现场证据，不是推断。
*
*          打印全部走板级轮询 16550，不依赖 libc 的 printf。
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
#include "arkrtos_blk.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "arkrtos_shell.h"     /* arkrtos_shell_init()：内核 Shell 控制台 */
#include "arkrtos_ptable.h"
#if (ARKRTOS_USE_ELF == 1)
#include "hello32_blob.h"
#endif
#include "arkrtos_crash.h"
#include "arkrtos_guard.h"   /* 心跳窗口守卫（ARKRTOS_USE_KERNEL_GUARD） */
#include "drvblk.h"

/* 任务栈：RISC-V 没有硬件压栈，只有软件帧，512 字（2 KB）很宽裕。
 * 栈必须是静态的（QEMU 上也没有堆分配器可用）。 */
#define TASK_STACK_WORDS     512u
/* 本目标实测深度 504 B（1 KB 与 4 KB 两档读数一致，说明不是被截断的），
 * 仍与 la64 同取 1024 字 = 4 KB：三个示例的 idle 栈尺寸保持一致，避免
 * 「换个目标就换一套余量」的隐性口径漂移。见 docs/QEMU模拟器移植与验证.md
 * §5.13.4 复核。 */
#define IDLE_STACK_WORDS     1024u
#define FS_STACK_WORDS       1024u   /* 存储链路实测最深的栈（见 s_stack_fs） */

static uint32 s_stack_a[TASK_STACK_WORDS];
static uint32 s_stack_b[TASK_STACK_WORDS];
static uint32 s_stack_u[TASK_STACK_WORDS];
static uint32 s_stack_rx[TASK_STACK_WORDS];
/* 存储任务单独给 4 KB：littlefs + VFS + 日志格式化这条链是本示例里
 * 最深的调用栈 —— 2 KB 时 shell `stack` 命令实测峰值就已到 1408/2048
 * （68%），而写文件/挂载的瞬时峰值比这个观测点还深。栈不够的后果不是
 * "变慢"，是回到别处的一个随机指针（本轮就抓到过一次 memcpy 从 0x100
 * 读的事故），所以这里按实测留一倍余量，不按"看起来够"留。 */
static uint32 s_stack_fs[FS_STACK_WORDS];
static uint32 s_idle_stack[IDLE_STACK_WORDS];

static int32 s_sem = -1;
static int32 s_sem_uart = -1;   /* 串口接收：ISR 里 post、任务里 wait */

/* 统计变量：让读数本身可被观察，而不是"看起来在跑" */
static volatile uint32 s_a_loops = 0u;
static volatile uint32 s_b_loops = 0u;

/**
* @brief 任务 A：周期性打印节拍与毫秒时间，并释放一次信号量
* @details 500 ms 一个周期。两次打印之间的差值就是节拍读数，能直接看出
*          定时器是不是按 10 MHz / 500 us 在走。
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
         * 入口；在调度器内部拿 SVC 换毫秒只是多一次 ecall 往返。 */
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
        arkrtos_board_puts("\n");

        tick_prev = tick;
        ms_prev   = ms;
        round++;
        s_a_loops++;

        /* SVC 通路自检。内核任务跑在内核态，本可以直接读节拍；这里每轮
         * 仍走一次 ecall（arkrtos_get_time_ms），把 「trap → SVC_Server →
         * 返回 a0」整条链也当成被测对象：它坏了，ms_svc 就会和 ms 对不上。
         *
         * 比的是「紧贴 ecall 之前重读一次节拍」而不是上面那次快照：上面那次
         * 快照和这里之间隔了一整行打印（约 60 个字符），在 16550 上一轮就是
         * 2~3 ms，够把 ±2 ms 的容差顶穿 —— 实测因此打出过假的 SVC_MISMATCH。
         * 两侧都紧贴同一个时刻取值，容差才只需要 ±1。 */
        {
            uint32 ms_now = (arkrtos_kernel_get_tick() * ARKRTOS_TICK_PERIOD_US) / 1000u;
            uint32 ms_svc = arkrtos_get_time_ms();

            arkrtos_board_puts(" ms_svc=");
            arkrtos_board_putu(ms_svc);
            arkrtos_board_puts((ms_svc + 1u < ms_now || ms_now + 1u < ms_svc) ? " SVC_MISMATCH" : " svc_ok");
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
 * 任务 C'：中断驱动串口接收
 * @details 这一条验证的是「外设中断能不能从 PLIC 一路走到内核里，并且
 *          在中断里唤醒一个任务」。三件事各有一个读数：
 *          1) `isr=`   串口 ISR 进了多少次（来自板级计数器）—— 证明 PLIC
 *                      的 claim/complete 通了，不是轮询装出来的；
 *          2) `rx=`    中断里收了多少字节 —— 证明 16550 的 RDA 中断真的
 *                      被清干净了（没清干净会中断风暴，节拍会停）；
 *          3) `wake=`  本任务被信号量唤醒的次数 —— 信号量是在 **ISR 里**
 *                      post 的，唤醒则发生在中断返回之后（本 port 的换栈
 *                      不在 trap 里做，而是靠 mret 之后取走的软件中断）。
 *          回显本身（把收到的字节原样打回去）也走同一个串口，所以终端
 *          上看到 "[R] ... -> hello" 就是端到端证据。
 * ============================================================ */
static volatile uint32 s_rx_wakes = 0u;

/* 唤醒报告的上次打印时刻（ms）。只用于给"控制台归 shell"模式节流，
 * 免得每敲一个字符就插一行，把交互界面糊满。 */
static uint32 s_rx_report_ms = 0u;

/**
* @brief 串口接收钩子（在中断里执行）
* @details 只做一件事：post 信号量。这里刻意**不**直接调用调度器的切换
*          函数 —— 中断里能做的只应该是"唤醒谁"，"换给谁"由内核在中断
*          返回后的软件中断里决定。
*/
void arkrtos_board_uart_rx_hook(uint8 byte)
{
    (void)byte;
    (void)arkrtos_sem_post_from_isr(s_sem_uart);
}

static void task_rx(void)
{
    for(;;)
    {
        if(arkrtos_sem_wait_internal(s_sem_uart, -1) == 0)
        {
            s_rx_wakes++;

#if (SHELL_ENABLE == 1)
            /* 有 shell 时，控制台归 shell 独占：同一个 16550 RX 环形缓冲只能
             * 有一个读者，本任务再去 read 会把用户刚敲的命令字节吃掉（实测
             * 表现为"提示符在、输入没反应"）。所以这里只把"ISR 里 post 的
             * 信号量真的把阻塞任务唤醒了"这件事打出来 —— 这仍然是中断链路
             * 的证据，而且是逐字节的（rx= 由 ISR 累加）。
             *
             * 但**每条命令都打一行会把交互界面糊满**（每敲一个字符唤醒一次），
             * 所以这里按 1 s 节流：唤醒次数是累计值，报告频率与打字速度无关。
             * 判据仍然是"这一行出现过、且 isr/rx 非零"。 */
            {
                uint32 now = arkrtos_kernel_get_time();

                if((now - s_rx_report_ms) < 1000u)
                {
                    continue;
                }
                s_rx_report_ms = now;
            }

            arkrtos_board_puts("[R] wake=");
            arkrtos_board_putu(s_rx_wakes);
            arkrtos_board_puts(" isr=");
            arkrtos_board_putu(arkrtos_board_uart_isr_count());
            arkrtos_board_puts(" rx=");
            arkrtos_board_putu(arkrtos_board_uart_rx_bytes());
            arkrtos_board_puts(" drop=");
            arkrtos_board_putu(arkrtos_board_uart_rx_dropped());
            arkrtos_board_puts(" (console owned by shell)\n");
#else
            uint8 buf[32];
            uint32 n;
            uint32 i;

            /* 没有 shell：本任务独占控制台 RX，边收边回显（D-2 的证据）。
             * 一次可能收到多个字节（FIFO/粘贴），全部取完再打印。 */
            for(;;)
            {
                n = arkrtos_board_uart_rx_read(buf, (uint32)sizeof(buf));
                if(n == 0u)
                {
                    break;
                }

                arkrtos_board_puts("[R] wake=");
                arkrtos_board_putu(s_rx_wakes);
                arkrtos_board_puts(" n=");
                arkrtos_board_putu(n);
                arkrtos_board_puts(" isr=");
                arkrtos_board_putu(arkrtos_board_uart_isr_count());
                arkrtos_board_puts(" rx=");
                arkrtos_board_putu(arkrtos_board_uart_rx_bytes());
                arkrtos_board_puts(" drop=");
                arkrtos_board_putu(arkrtos_board_uart_rx_dropped());
                arkrtos_board_puts(" -> ");

                for(i = 0u; i < n; i++)
                {
                    if((buf[i] >= 0x20u) && (buf[i] < 0x7Fu))
                    {
                        arkrtos_board_putc((char)buf[i]);
                    }
                    else
                    {
                        arkrtos_board_putc('.');
                    }
                }
                arkrtos_board_puts("\n");
            }
#endif /* SHELL_ENABLE */
        }
    }
}

/* ============================================================
 * 任务 C：U 模式负对照（内存保护自检）
 * @details 这个任务以 is_priv = 0 注册，运行在 **U 模式**。三件事各验一条：
 *          1) 打印走板级给定的外设窗（UART 4 KB RW）—— 证明窗口本身是通的，
 *             不是"什么都没给、所以什么都没跑"；
 *          2) 读毫秒时间走 arkrtos_get_time_ms()（照旧是 ecall），
 *             证明 U 模式的 trap 入口（mcause = 8）也接得住；
 *          3) 故意读 CLINT（0x02000000）—— 该地址不在任何 PMP 窗口里。
 *             M 模式读它是合法的（PMP 不管 M），U 模式读它必然被拒
 *             （mcause = 5，mtval = 目标地址）。真被拦住时下面那行
 *             "UNEXPECTED / no isolation" 永远打不出来，这是本用例的防假绿。
 *          每次故障后内核记 MEMFAULT 并把任务重启，round 接着涨，
 *          所以连续三条 [U] round=N 本身就是"拦住了 + 能恢复"的证据。
 * @note 本任务由内核注册（rom_start = 0），因此拿到的是内核代码窗 + 整片
 *       内核 RAM —— 隔离的是**特权级**（U 碰不到窗口外的地址、执行不了
 *       csrw），不是任务间隔离。任务间隔离要等 App 镜像走 loader 拿到
 *       自己的 slot，见 docs/QEMU模拟器移植与验证.md 的未完成项。
 * ============================================================ */
#define U_FAULT_ROUNDS       3u
#define RV_CLINT_PROBE_ADDR  0x02000000u

static volatile uint32 s_u_rounds = 0u;

/* port 层的 ecall 来源计数（mcause=8 来自 U，11 来自 M）。放在这里读，
 * 是为了让"这个任务真的在 U 模式"变成可读数字，而不是靠"没崩"推断。 */
extern volatile uint32 g_rv_ecall_u;
extern volatile uint32 g_rv_ecall_m;

static void task_u(void)
{
    uint32 round = s_u_rounds + 1u;

    s_u_rounds = round;

    arkrtos_board_puts("[U] round=");
    arkrtos_board_putu(round);
    arkrtos_board_puts(" t=");
    arkrtos_board_putu(arkrtos_get_time_ms());
    arkrtos_board_puts(" ms ecall_u=");
    arkrtos_board_putu(g_rv_ecall_u);
    arkrtos_board_puts(" ecall_m=");
    arkrtos_board_putu(g_rv_ecall_m);
    arkrtos_board_puts("\n");

    if(round <= U_FAULT_ROUNDS)
    {
        volatile uint32 *p_probe = (volatile uint32 *)RV_CLINT_PROBE_ADDR;
        uint32 v;

        arkrtos_board_puts("[U] reading CLINT (outside every PMP window) ...\n");

        /* 这一句是整条用例的判据：U 模式下它必须 trap，永不返回。 */
        v = *p_probe;

        arkrtos_board_puts("[U] UNEXPECTED: read=");
        arkrtos_board_puthex(v);
        arkrtos_board_puts(" -> no isolation\n");
    }

    /* 第四条负对照：写 CSR。特权级隔离除了“碰不到窗口外地址”，还有一条
     * “改不了控制寄存器”。U 模式执行任何 csr 写指令会被硬件报 mcause=2
     * （非法指令），port 层把它归入越权处理（与 PMP 拒绝同一个出口）。
     * 没接这条之前实测：U 任务只要碰一下内核内部函数就 FATAL: mcause=0x2。 */
    if(round == (U_FAULT_ROUNDS + 1u))
    {
        arkrtos_board_puts("[U] writing mstatus (privileged CSR) ...\n");

        /* 这一句必须在 U 模式 trap，永不返回。 */
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");

        arkrtos_board_puts("[U] UNEXPECTED: csrsi mstatus ok -> no isolation\n");
    }

    /* 尾部循环的两条约束，都是实测踩出来的：
     *  a) 不能用 arkrtos_task_delay() 当循环体：它是 SVC → port 忙等，而 SVC
     *     在 trap 里执行时 mstatus.MIE = 0，忙等全程关中断；实测 200 ms 忙等
     *     让节拍只按 ~5 Hz 前进、A/B 永远排不上（8 s 宿主只跑出 18 个 tick）。
     *  b) 不能用 arkrtos_task_wait_internal()：它是内核内部函数，开头就
     *     ARKRTOS_DISABLE_IRQ()（csrrc），U 模式一执行就是 mcause=2。
     * 所以走 App 侧的 SVC 接口 arkrtos_task_wait()，一次 2 ms 忙等保留下来
     * 当 SVC 通路的轻量探针（ecall_u 仍会随轮次增长），真正的让出 CPU 靠它。 */
    for(;;)
    {
        arkrtos_task_delay(2000u);
        arkrtos_task_wait(1000u);
    }
}

/* ============================================================
 * 任务 D：存储栈自检（块设备 → littlefs → VFS → procfs）
 * @details 这一条链路是「全功能」考题里最容易被跳过的一段：不做它，示例里
 *          只剩调度器与两个同步原语，“文件系统能挂载、能读写、路径能解析”
 *          全都只是文档上的话。四道读数：
*
*          1) 块设备：ram0 注册成功且能起得来（见 board/qemu_riscv32/drvblk.c）；
*          2) littlefs：format 一个 512 KB 的卷，再 mount 上去（挂载不格式化，
*             所以必须先 format，这是它自己的契约）；
*          3) VFS：把同一个卷挂到 /mnt/ram，按路径写一个文件再读回来逐字节对比；
*          4) procfs：读 /proc/version，证明 VFS 的另一个驱动也在岗。
*
*          另加一条**负对照**：对已经写过的偏移再写一次（不擦），块设备必须
*          回 -1。这是 NOR 语义“只能 1→0”的现场证据，也是“写成功”这个读数
*          唯一能证明自己不是空转的地方。
*
* @note 本任务跑一次就阻塞到底。它必须在内核模块初始化之后（VFS 与文件系统
*       的模块状态在那里建），且在调度启动之后（它要用信号量/延时以外的
*       普通阻塞——这里用 arkrtos_task_wait）。
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
    const char *msg = "ark rv32 vfs ok\n";
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

        /* 逐字节对比：长度不对或内容不对都算失败，不给“差不多对”的结论。
         * 自己写循环而不是调 memcmp：本目标 -nostdlib，字符串函数全部来自
         * libc_shim.c，而那里没有 memcmp 的宽原型保证；这里不需要任何技巧。 */
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

#if (ARKRTOS_USE_ELF == 1)
    /* ---- 3b2) ELF32 示例程序落进 ramfs ----
     * 数组只是把文件送到板子上的手段（QEMU 没有宿主文件注入通道）；
     * 装载器读的是 VFS 里的这个文件。随后在 shell 里
     *     exec /mnt/ram/hello32 a b c
     * 走真正的装载路径。 */
    if(ok != 0u)
    {
        rc = arkrtos_vfs_write_file("/mnt/ram/hello32",
                                    (const uint8 *)ark_example_hello32,
                                    (uint32)ARK_EXAMPLE_HELLO32_LEN);
        arkrtos_board_puts("[ELF32] write hello32 rc=");
        arkrtos_board_putu((uint32)rc);
        arkrtos_board_puts(" bytes=");
        arkrtos_board_putu((uint32)ARK_EXAMPLE_HELLO32_LEN);
        arkrtos_board_puts("\n");
        if(rc != 0)
        {
            ok = 0u;
        }
    }
#endif

    /* ---- 3c) 列目录 + 读 procfs ---- */
    if(ok != 0u)
    {
        arkrtos_board_puts("[FS] ls /mnt/ram:\n");
        (void)arkrtos_vfs_list("/mnt/ram", fs_list_cb, 0);

        rc = arkrtos_vfs_read_file("/proc/version", rbuf, (uint32)sizeof(rbuf) - 1u, &n);
        if((rc == 0) && (n > 0u))
        {
            /* 只打前 40 字节：内容是内核版本串，长度由它自己决定 */
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
*          1) enter_idle：把 sp 换到空闲栈（RISC-V 上必须是汇编）；
*          2) start_timer：节拍开始走；
*          3) switch_enable：打开切换门闩并立刻置一次软件中断，
*             第一个任务在这一步之后才开始运行；
*          4) 之后进入 WFI 循环，CPU 只在有中断时醒来。
*/
static void arkrtos_start_idle(void)
{
    uint32 sp = (uint32)(&s_idle_stack[IDLE_STACK_WORDS]);

    sp &= ~0xFu;      /* RISC-V 栈 16 字节对齐（ABI 硬要求） */

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
    int32 id_u;
    int32 id_rx;
    int32 id_fs;
    int32 id_shell;

    arkrtos_port_board_init();

    arkrtos_cfg_load();

    /* 任务先注册、再开中断：irq_init() 一开 mstatus.MIE，节拍就可能来，
     * 那时任务表必须已经就位（顺序见 arkrtos_init.h 文件头 1~8 步）。 */
    id_a = arkrtos_task_register(task_a, s_stack_a, sizeof(s_stack_a), 10u, 0u);
    id_b = arkrtos_task_register(task_b, s_stack_b, sizeof(s_stack_b), 11u, 0u);
    id_u = arkrtos_task_register(task_u, s_stack_u, sizeof(s_stack_u), 12u, 0u);
    /* 接收任务优先级最高：串口 ISR 靠它把字节取走，拖着不动就会丢。 */
    id_rx = arkrtos_task_register(task_rx, s_stack_rx, sizeof(s_stack_rx), 9u, 0u);
    /* 存储自检放最低优先级：它跑一次就阻塞，不能跟 A/B 抢节拍读数 */
    id_fs = arkrtos_task_register(task_fs, s_stack_fs, sizeof(s_stack_fs), 13u, 0u);

    /* 把它降到 U 模式：内核注册出来的任务默认 is_priv = 1（见 arkrtos_cfg.c），
     * 这里显式改 0 —— 与 loader / apprun 把 App 置 0 是同一处语义，
     * 也是本目标上"特权隔离真的在起作用"的唯一入口。 */
    if(id_u > 0)
    {
        arkrtos_task_table[id_u - 1].is_priv = 0u;
    }

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
     * SHELL_TASK_*（14 / 3 KB / 2 ms）。它打开的控制台设备 ttyS0 已经在
     * arkrtos_kernel_module_init() 里由板级 arkrtos_dev_board_init() 注册，
     * 所以这一步必须在 kernel_module_init() 之后。 */
    id_shell = arkrtos_shell_init();

    arkrtos_board_puts("[boot] sem=");
    arkrtos_board_putu((uint32)s_sem);
    arkrtos_board_puts(" taskA=");
    arkrtos_board_putu((uint32)id_a);
    arkrtos_board_puts(" taskB=");
    arkrtos_board_putu((uint32)id_b);
    arkrtos_board_puts(" taskU=");
    arkrtos_board_putu((uint32)id_u);
    arkrtos_board_puts(" mpu=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_MPU);
    arkrtos_board_puts(" priv=");
    arkrtos_board_putu((uint32)ARKRTOS_USE_PRIV);
    arkrtos_board_puts(" taskRX=");
    arkrtos_board_putu((uint32)id_rx);
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
#if (ARKRTOS_USE_CRASH_LOG == 1)
    arkrtos_board_puts(" crasharea=");
    arkrtos_board_puthex(arkrtos_crash_area_base());
    arkrtos_board_puts(" boots=");
    arkrtos_board_putu(arkrtos_crash_boots());
#endif
    arkrtos_board_puts(" tick_period_us=");
    arkrtos_board_putu(ARKRTOS_TICK_PERIOD_US);
    arkrtos_board_puts("\n");

    /* 串口接收信号量：ISR 里 post、task_rx 里 wait。
     * 必须先建再开 PLIC 源 —— 钩子里 post 的句柄不能是 -1。 */
    s_sem_uart = arkrtos_sem_create_internal("qemu_uart", 0);
    if(s_sem_uart < 0)
    {
        arkrtos_board_puts("[boot] uart sem_create FAILED, halted\n");
        for(;;)
        {
            ARKRTOS_WFI();
        }
    }

    /* 开中断接收：16550 IER.RDA + PLIC UART0 源。
     * 位置很关键：必须在 irq_init()（mtvec/mie）之后，否则第一个字节
     * 可能在中断入口还没装好时到来。 */
    arkrtos_board_uart_irq_init();

    arkrtos_board_puts("[boot] uart irq on, send bytes to see [R] echo\n");

    /* 块设备驱动在这里先注册（只登记描述符，不碰介质）；真正 init 交给
     * task_fs —— 让“注册”和“起来”两件事各有一次可读的打印。 */
    (void)arkrtos_board_blk_init();

    if((id_a < 0) || (id_b < 0) || (id_u < 0) || (id_fs < 0))
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
