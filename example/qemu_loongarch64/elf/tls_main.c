/**
* @file tls_main.c
* @brief 带 PT_TLS 的静态程序（QEMU LoongArch64）
* @details 这个文件存在的唯一理由是把「装载器读不读 PT_TLS」变成一条可跑的
*          读数。它有两个 __thread 变量：
*
*            tv_data  在 .tdata，初值 0x5A5A（模板字节，必须由装载器从文件
*                     搬到 TLS 块里）；
*            tv_bss   在 .tbss，没有初值（必须由装载器清零，否则读到的是窗口
*                     里上一份内容留下的字节）。
*
*          访问方式由编译器按 local-exec 生成：`$r2`（TP）加上一个常量偏移。
*          也就是说这个程序**能不能给出正确退出码，完全取决于装载器有没有
*          把 TP 指到 TLS 块起始地址**——TP 还是 0 的话，读 0x5A5A 会先碰到
*          未映射地址（PLV3 下是重填异常），程序根本到不了 PASS。
*
*          两种写法都用同一个 volatile 修饰，是为了防住 -O2 把常量折掉：
*          第一版不带 volatile 时 gcc 直接把整段 TLS 访问优化没了，链接出来
*          连 PT_TLS 都不剩——那会是一个「测了但什么也没测」的假夹具。
*
* @note 不链 libc、不链启动代码；系统调用走 Linux 号（write=64 / exit=93），
*       所以配套的读数行与 linux_hello 一样落在控制台上。
* @author xw
*/

/* asm-generic 号表（Linux UAPI），只用到两个 */
#define LX_WRITE            64
#define LX_EXIT             93

/* 两个 __thread 变量。初值 0x5A5A 是任取的一个非零图案：0 会和「没铺模板」
 * 分不开，而 0x5A5A 在未初始化的窗口里几乎不可能碰巧出现。 */
volatile __thread int tv_data = 0x5A5A;
volatile __thread int tv_bss;

/* 三参数系统调用：号在 $a7，参数在 $a0..$a2，返回值在 $a0。 */
static long tls_sys3(long nr, long a, long b, long c)
{
    register long a0 __asm__("$r4") = a;
    register long a1 __asm__("$r5") = b;
    register long a2 __asm__("$r6") = c;
    register long a7 __asm__("$r11") = nr;

    __asm__ volatile("syscall 0"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
}

static void tls_exit(long code)
{
    (void)tls_sys3((long)LX_EXIT, code, 0, 0);
    for(;;)
    {
        /* exit 之后不该回来；回来就原地停住，不往下跑。 */
    }
}

/**
* @brief 退出码就是读数：0 = 这一格坏了，3 = 两种 TLS 变量都对
* @details 逐格带非 0 退出码，方便在串口上直接分辨是哪一格坏的：
*            9 = .tdata 模板没铺对（TP 错或没拷进来）
*           10 = .tbss 没清零（读回来的不是刚写进去的 0x11）
*            3 = 两格都过（成功值，和 linux_hello 的 7 一样是约定值）
*/
void _start(void)
{
    static const char ok_msg[] = "TLSOK\n";
    long rc;

    if(tv_data != 0x5A5A)
    {
        tls_exit(9);
    }

    tv_bss = 0x11;
    if(tv_bss != 0x11)
    {
        tls_exit(10);
    }

    rc = tls_sys3((long)LX_WRITE, 1, (long)ok_msg, 6);
    if(rc != 6)
    {
        tls_exit(11);
    }

    tls_exit(3);
}
