/**
* @file ushell_iso.c
* @brief SEC-G08 隔离执行的 QEMU 夹具：App 自己注册一条 shell 命令并常驻
* @details 这个程序是「跳板把 App 回调放到 App 自己的地址空间里以 PLV3 跑」
*          的现场证据。它只做四件事：
*            1) 打开控制台（SVC 0x10），把入口 sp/argc 打出来；
*            2) 用 SVC 0x1A 子 1 把命令 `hi` 注册到内核 shell；
*            3) 打印注册返回码，然后 SVC 0x11 子 3 睡下去**不退出**——
*               退出会把地址空间连同这条命令一起卸掉；
*            4) 回调 `hi_cmd` 里打印 argc / 每一行 argv / func 地址 / 当前
*               $sp。这几行是判据：argc 与 argv 证明参数搬家成立（回调读到
*               的是窗口里的副本，不是内核内存），func 与 sp 证明回调确实
*               在 App 的窗口里跑。
*
* @note 为什么常驻而不是「注册完就退出」：`exec` 是阻塞等程序退出的，而
*       App 命令要被 shell 反复调用，注册者必须活着（`arkrtos_elf_running_in`
*       与 upcall 区的几何都挂在这条活着的实例上）。
* @note 不链 libc、不链启动代码：入口是链接脚本指定的 `_start`，入口 sp 指向
*       [argc][argv0]...[NULL]，与 Linux 的进程入口布局一致。
* @author xw
*/

#define ARK_SVC_DEV_IO          0x10u
#define ARK_SVC_TASK_CTRL       0x11u
#define ARK_SVC_SHELL           0x1Au

#define ARK_DEV_SUB_OPEN        1u
#define ARK_DEV_SUB_WRITE       3u
#define ARK_TASK_SUB_WAIT       1u
#define ARK_SHELL_SUB_REGISTER  1u

/* 参数块是 32 位字（默认档，见 arkrtos_svc_abi.h）。窗口 VA 在 0x02000000，
 * 截到 32 位不丢信息——这是真实的边界，写在这里免得后来者以为能塞 64 位。 */
typedef unsigned int ark_arg;

typedef struct
{
    const char *name;
    int       (*func)(int argc, char **argv);
    const char *help;
    unsigned int max_args;
} ark_cmd_t;

static int ark_svc_dev_io(ark_arg *params)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)params;

    __asm__ volatile("syscall 0x10" : "+r"(a0) : : "memory");
    return (int)a0;
}

static int ark_svc_shell(ark_arg *params)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)params;

    __asm__ volatile("syscall 0x1A" : "+r"(a0) : : "memory");
    return (int)a0;
}

/* ------------------------------------------------------------------
 * 极小的运行时（没有 libc）
 * ------------------------------------------------------------------ */
static unsigned long str_len(const char *s)
{
    unsigned long n = 0ul;

    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static int g_console = -1;

static void out(const char *s)
{
    ark_arg params[4];

    if ((g_console < 0) || (s == 0))
    {
        return;
    }
    params[0] = (ark_arg)ARK_DEV_SUB_WRITE;
    params[1] = (ark_arg)(unsigned long)g_console;
    params[2] = (ark_arg)(unsigned long)s;
    params[3] = (ark_arg)str_len(s);
    (void)ark_svc_dev_io(params);
}

static void out_num(long v)
{
    char buf[24];
    int  i = 0;
    unsigned long u;

    if (v < 0)
    {
        out("-");
        u = (unsigned long)(-v);
    }
    else
    {
        u = (unsigned long)v;
    }
    if (u == 0ul)
    {
        out("0");
        return;
    }
    while ((u != 0ul) && (i < (int)(sizeof(buf) - 1)))
    {
        buf[i] = (char)('0' + (int)(u % 10ul));
        u /= 10ul;
        i++;
    }
    while (i > 0)
    {
        char one[2];

        i--;
        one[0] = buf[i];
        one[1] = '\0';
        out(one);
    }
}

/* 地址按 0x + 小写 hex 打：门禁要按前缀判它落在哪个区间 */
static void out_hex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int  i = 0;
    int  k;

    out("0x");
    if (v == 0ul)
    {
        out("0");
        return;
    }
    while ((v != 0ul) && (i < (int)sizeof(buf)))
    {
        buf[i] = digits[(int)(v & 0xFul)];
        v >>= 4;
        i++;
    }
    for (k = i - 1; k >= 0; k--)
    {
        char one[2];

        one[0] = buf[k];
        one[1] = '\0';
        out(one);
    }
}

static void open_console(void)
{
    ark_arg params[4];
    static char dev_name[] = "ttyS0";

    params[0] = (ark_arg)ARK_DEV_SUB_OPEN;
    params[1] = (ark_arg)(unsigned long)dev_name;
    params[2] = 0u;
    params[3] = 0u;
    g_console = ark_svc_dev_io(params);
}

/* 阻塞睡眠：SVC 0x11 子 1（task wait，单位 ms）。
 *
 * 两处口径必须写清楚，写错了都不会报错、只会让控制台看起来死了：
 *  1) **参数是寄存器 ABI，不是参数块**。0x11 的两个参数走 $a0/$a1
 *     （`arkrtos_call_task_ctrl(sub, arg)`），与 0x10/0x1A 那种「$a0 指向
 *     一块 uint32 参数块」不同。按块传时内核把 $a0 当成子命令字读，
 *     读到的是指针，落到 default，于是 wait 变成一次空调用——App 立刻
 *     返回继续循环，看起来"睡了"，其实一秒都没睡。
 *  2) **用子 1（阻塞）而不是子 3（delay 忙等）**。App 以优先级 12 起跑，
 *     忙等就是一直占着 CPU 不放，优先级更低的 shell（14）再也拿不到时间片，
 *     控制台会像串口坏了一样（实测：心跳与回显全停，而优先级 10/11 的
 *     A/B 任务照常打印——非常容易误判）。 */
static void sleep_ms(unsigned int ms)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)ARK_TASK_SUB_WAIT;
    register unsigned long a1 __asm__("a1") = (unsigned long)ms;

    __asm__ volatile("syscall 0x11" : "+r"(a0) : "r"(a1) : "memory");
}

/* 当前栈指针：回调跑在 upcall 栈上，这个值就是「在窗口里跑」的直接读数。
 * 用内联汇编读，不经过任何函数序言——它必须是此刻的 sp，不是本函数的帧。 */
static unsigned long read_sp(void)
{
    unsigned long sp;

    __asm__ volatile("move %0, $sp" : "=r"(sp));
    return sp;
}

/* ------------------------------------------------------------------
 * 回调本体：内核通过桩把 argc/argv 交进来，跑在 PLV3
 * ------------------------------------------------------------------ */
static int hi_cmd(int argc, char **argv)
{
    int i;

    out("[ushell_iso] cb argc=");
    out_num(argc);
    out("\n");
    for (i = 0; i < argc; i++)
    {
        out("[ushell_iso] argv[");
        out_num(i);
        out("]=");
        out((argv != 0 && argv[i] != 0) ? argv[i] : "(null)");
        out("\n");
    }
    out("[ushell_iso] cb argv=");
    out_hex((unsigned long)(void *)argv);
    out(" func=");
    out_hex((unsigned long)(void *)hi_cmd);
    out(" sp=");
    out_hex(read_sp());
    out("\n");
    return 0;
}

/* 描述符放在 .data（可写、在窗口里、be 内核按「调用者 RAM」认）；
 * name/help 也放可写数组，理由同上：内核会从 App 的窗口里读它们。 */
static ark_cmd_t g_hi_cmd =
{
    "hi",
    hi_cmd,
    "SEC-G08 isolated execution probe",
    4u
};

/* ------------------------------------------------------------------
 * 入口：汇编垫片先于 C 序言拿到入口 sp
 * ------------------------------------------------------------------ */
void ushell_iso_main(unsigned long sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  move $a0, $sp\n"
    "  bl   ushell_iso_main\n"
    "1:\n"
    "  b    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void ushell_iso_main(unsigned long sp)
{
    ark_arg params[4];
    long argc;
    int  rc;

    argc = *(long *)sp;

    open_console();
    out("[ushell_iso] entry sp=");
    out_hex(sp);
    out(" argc=");
    out_num(argc);
    out("\n");

    /* 注册：p[0] = 1（REGISTER）、p[1] = 描述符地址（在 App 自己的窗口里） */
    params[0] = (ark_arg)ARK_SHELL_SUB_REGISTER;
    params[1] = (ark_arg)(unsigned long)(void *)&g_hi_cmd;
    params[2] = 0u;
    params[3] = 0u;
    rc = ark_svc_shell(params);

    out("[ushell_iso] register rc=");
    out_num(rc);
    out("\n");
    if (rc != 0)
    {
        /* 注册失败就不能假装成功：门禁等的是 `registered` 这行，它不该出现 */
        out("[ushell_iso] FAIL: cannot register; staying idle\n");
        for (;;)
        {
            sleep_ms(1000u);
        }
    }

    out("[ushell_iso] registered, waiting for callbacks\n");

    /* 常驻：不退出（退出会把地址空间连同这条命令一起卸掉）。
     * 200 ms 一轮的阻塞睡眠；回调在另一条 upcall 线程上跑，与这里互不阻塞。 */
    for (;;)
    {
        sleep_ms(200u);
    }
}
