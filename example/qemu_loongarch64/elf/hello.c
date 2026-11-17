/**
* @file hello.c
* @brief Ark RTOS 上跑的 ELF64 示例程序（QEMU LoongArch64）
* @details 这是一个普普通通的静态可执行文件：入口是 _start，入口时 sp 指向
*          [argc][argv0][argv1]...[NULL][NULL]——就是 Linux 的进程入口布局，
*          所以它不需要为 Ark 改一行调用约定。
*
*          它只做三件事，正好覆盖装载器必须成立的三条：
*            1) 读到正确的 argc/argv（证明装载器铺的栈对）；
*            2) 通过 SVC 0x10（DEV_IO）写控制台（证明降权任务能走系统调用，
*               且参数块落在内核认可的用户窗口里）；
*            3) 通过 SVC 0x1B 子 4 带退出码结束自己（证明程序能自己退出、任务被
*               回收，且退出码能一路回到 shell 的 exec：exit code = argc）。
*
*          不链 libc、不链启动代码：本文件里的 main 不是 C 语义的 main，
*          是链接脚本指定的入口。任何一个按 Linux 静态程序写的东西也能跑，
*          只是这个例子故意把每一层都摊开，免得把"能跑"当成"内核支持多"。
*
* @note 退出码走 SVC 0x1B 子 4（U-8，2026-11-17）：带一个 int32 参数，内核记进
*       TCB，装载器在任务变 INVALID 时取走，shell 的 exec 打印 exit code N。
*       这里故意让退出码等于 argc，于是「打印出来的 argc」与「exec 报的退出码」
*       互为对证——两边对不上就是通道断了，不是读数漂移。
* @author xw
*/

/* ------------------------------------------------------------------
 * 系统调用：只有一个参数块指针（a0），和内核 side 的 svc_dev_io /
 * svc_thread_ctrl 解析方式一致。用显式寄存器变量钉住 a0，避免编译器
 * 在进出内联汇编时自己找寄存器。
 * ------------------------------------------------------------------ */
#define ARK_SVC_DEV_IO          0x10u
#define ARK_SVC_THREAD_CTRL     0x1Bu

#define ARK_DEV_SUB_OPEN        1u
#define ARK_DEV_SUB_WRITE       3u
#define ARK_THREAD_SUB_EXIT_CODE 4u

/* 参数块是 **32 位字** 的数组，这是内核 SVC 协议的既有形状（内核侧按
 * uint32* 解引用），不是本程序的偏好：64 位程序的指针也必须以 32 位字传。
 * 用户窗口在 0x02000000，栈也在其中，所以截到 32 位不丢信息——但这是
 * 一条真实的边界，写在这里免得后来者以为可以塞 64 位地址。 */
typedef unsigned int ark_u32;

static int ark_svc_dev_io(ark_u32 *params)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)params;

    __asm__ volatile("syscall 0x10" : "+r"(a0) : : "memory");
    return (int)a0;
}

static int ark_svc_thread_ctrl(ark_u32 *params)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)params;

    __asm__ volatile("syscall 0x1B" : "+r"(a0) : : "memory");
    return (int)a0;
}

/* ------------------------------------------------------------------
 * 极小的运行时：没有 libc，所以 strlen / 数字转换自己写。
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
    ark_u32 params[4];

    if ((g_console < 0) || (s == 0))
    {
        return;
    }
    params[0] = ARK_DEV_SUB_WRITE;
    params[1] = (ark_u32)g_console;
    params[2] = (ark_u32)(unsigned long)s;
    params[3] = (ark_u32)str_len(s);
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

static void open_console(void)
{
    ark_u32 params[4];
    static char dev_name[] = "ttyS0";

    params[0] = ARK_DEV_SUB_OPEN;
    params[1] = (ark_u32)(unsigned long)dev_name;
    params[2] = 0u;
    params[3] = 0u;
    g_console = ark_svc_dev_io(params);
}

/* ------------------------------------------------------------------
 * 入口。sp 由内核按 Linux 布局铺好，这里只读，不改。
 *
 * **必须用汇编垫片进 C**：C 函数的序言会在任何语句之前先
 * `addi.d $sp,$sp,-N`，于是在 C 里读 $sp 读到的是「序言之后」的 sp，
 * 不是入口 sp。实测差 128 字节，argc 于是读成了栈上的垃圾（0）。
 * 垫片在序言之前把入口 sp 放进 a0（psABI 的第一个整数参数），
 * 再跳进 C——C 侧拿到的就是真正的入口 sp，与 Linux 入口约定一致。
 * ------------------------------------------------------------------ */
void hello_main(unsigned long sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  move $a0, $sp\n"
    "  bl   hello_main\n"
    "1:\n"
    "  b    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void hello_main(unsigned long sp)
{
    long argc;
    char **argv;
    long i;

    argc = *(long *)sp;
    argv = (char **)(sp + 8);

    open_console();
    /* 这一行是「入口约定成立」的现场证据：sp 就是装载器铺的那一个
     * （32 KB 栈之上、argv 区起点），argc 是从它指向的字里读出来的。 */
    out("[hello] entry sp=");
    out_num((long)sp);
    out(" argc=");
    out_num(argc);
    out("\n");

    out("[hello] hello from an ELF64 program\n");
    out("[hello] argc=");
    out_num(argc);
    out("\n");

    for (i = 0; i < argc; i++)
    {
        out("[hello] argv[");
        out_num(i);
        out("]=");
        out((argv[i] != 0) ? argv[i] : "(null)");
        out("\n");
    }

    out("[hello] bye, exit code = argc\n");

    /* 结束自己：内核回收任务、释放它注册过的设备，之后同一个窗口就能再装
     * 下一个程序。返回是不允许的——从 _start 返回等于跳到垃圾地址。
     * U-8：退出码就是 argc，于是 exec 打印的 exit code 与上面打印的 argc
     * 是同一个数，两个读数能互相对证。 */
    {
        ark_u32 params[6];

        params[0] = ARK_THREAD_SUB_EXIT_CODE;
        params[1] = (ark_u32)argc;
        params[2] = 0u;
        params[3] = 0u;
        params[4] = 0u;
        params[5] = 0u;
        (void)ark_svc_thread_ctrl(params);
    }

    for (;;)
    {
    }
}
