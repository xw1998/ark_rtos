/**
* @file hello32.c
* @brief Ark RTOS 上跑的 ELF32 示例程序（QEMU RISC-V RV32）
* @details 与 example/qemu_loongarch64/elf/hello.c 是同一个东西的 32 位版本：
*          入口是 _start，入口时 sp 指向 [argc][argv0][argv1]...[NULL][NULL]，
*          地址与指针都是 4 字节。它证明的是同一个装载器在 32 位 port 上
*          铺的栈、走的 SVC、拿的退出码都对。
*
*          两处与 64 位版的**真实差别**，写在这里免得被当成复制粘贴的笔误：
*            1) 系统调用是 `ecall`，号在 a7（RISC-V 没有立即数字段）；
*            2) argv 起点是 sp + 4，不是 sp + 8（字宽随 class 变）。
*
*          其余（参数块是 32 位字数组、控制台走 DEV_IO、退出走
*          THREAD_CTRL 子 4 带退出码）与 64 位版一模一样 —— 那是内核 SVC
*          协议的形状，不随位数变。
* @note 退出码故意等于 argc：exec 打印的 exit code 与这里打印的 argc 互为
*       对证，两边对不上就是通道断了。
* @author xw
*/

#define ARK_SVC_DEV_IO            0x10u
#define ARK_SVC_THREAD_CTRL       0x1Bu

#define ARK_DEV_SUB_OPEN          1u
#define ARK_DEV_SUB_WRITE         3u
#define ARK_THREAD_SUB_EXIT_CODE  4u
#define ARK_THREAD_SUB_EXIT       3u

typedef unsigned int ark_u32;

/* ecall：号走 a7，参数块指针走 a0，返回值在 a0。 */
static int ark_svc(ark_u32 num, ark_u32 *params)
{
    register unsigned long a0 __asm__("a0") = (unsigned long)params;
    register unsigned long a7 __asm__("a7") = (unsigned long)num;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int)a0;
}

static int ark_svc_dev_io(ark_u32 *params)
{
    return ark_svc(ARK_SVC_DEV_IO, params);
}

static int ark_svc_thread_ctrl(ark_u32 *params)
{
    return ark_svc(ARK_SVC_THREAD_CTRL, params);
}

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
    char buf[16];
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

/* 入口垫片：C 的序言会先动 sp，所以在 C 里读 sp 读到的是序言之后的那个。
 * 垫片在序言之前把入口 sp 放进 a0，再跳进 C。 */
void hello_main(unsigned long sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  mv   a0, sp\n"
    "  call hello_main\n"
    "1:\n"
    "  j    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void hello_main(unsigned long sp)
{
    long argc;
    char **argv;
    long i;

    argc = *(long *)sp;
    argv = (char **)(sp + sizeof(long));

    open_console();
    out("[hello32] entry sp=");
    out_num((long)sp);
    out(" argc=");
    out_num(argc);
    out("\n");

    out("[hello32] hello from an ELF32 program\n");
    out("[hello32] argc=");
    out_num(argc);
    out("\n");

    for (i = 0; i < argc; i++)
    {
        out("[hello32] argv[");
        out_num(i);
        out("]=");
        out((argv[i] != 0) ? argv[i] : "(null)");
        out("\n");
    }

    out("[hello32] bye, exit code = argc\n");

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

    /* 兜底：本档（RV32 默认配置）ARKRTOS_USE_EXIT_CODE 可能是 0，此时
     * 子 4 在 svc_thread_ctrl 里走 default 返回 -1，程序就会掉进下面的
     * 死循环 —— ELF 任务优先级不低于 shell，shell 永远拿不回 CPU（本轮
     * 实测：exec 之后 shell 无任何输出）。子 3（EXIT）是 ABI 里始终存在的
     * 那条，因此退回它；只有当子 3 也返回时才认为真的退不出去。 */
    {
        ark_u32 params[6];

        params[0] = ARK_THREAD_SUB_EXIT;
        params[1] = 0u;
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
