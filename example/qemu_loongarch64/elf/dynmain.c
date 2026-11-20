/**
* @file dynmain.c
* @brief 走 PT_INTERP 动态链接路径的主程序（QEMU LoongArch64）
* @details 链接成 ET_DYN（链接基址 0），由 tools/elf/dynfixture.py 加上
*          PT_INTERP / PT_DYNAMIC / .rela，内核按 Linux 约定把 ld.so 装在
*          0x02100000 并跳过去；ld.so 负责加载 libanswer.so、做完重定位
*          后再跳回本文件的 _start。
*
*          它检查的四件事，正好覆盖三种重定位：
*            1) JUMP_SLOT：got_answer 被写成 answer_add 的运行期地址，
*               调过去 answer_add(5) 必须是 105（libanswer 的 g_base=100）；
*            2) RELATIVE：m_rel 槽被写成 &m_base，*m_rel 必须是 7；
*            3) R_LARCH_64：got_gptr / got_absptr 被写成 &g_ptr / &abs_ptr，
*               顺链路读到的地址必须等于 lib_g_addr()（PC 相对现算的真值）；
*            4) 数据本身：*g_ptr == 100。
*
*          另一个 JUMP_SLOT：lib_g_addr 也不能直接 bl（本文件不与 libanswer.c
*          一起静态链接，直接调用会在链接期报未定义符号），所以同样经
*          got_gaddr 槽间接调用——这就是最小号的 PLT 等价物。
*
*          第 4 组检查动态 TLS（R_LARCH_TLS_TPREL64 + 装载器建线程块）：
*            m_tls_pad / m_tls 两个 __thread 变量，m_tls 故意排在偏移非 0
*            处——"重定位没做、槽还是 0、正好读到偏移 0"这种假绿必须被
*            抓到；库里还有一个 lib_tls，只能经库自己的 TPREL64 槽访问，
*            用来证明符号解析是跨对象找 tls_off 的，不是拿本对象的偏移凑。
*          任一条不成立就打印 FAIL 并以 1 退出，全部成立以 105 退出。
*
* @note 这里没有一行 Ark SDK 调用，也没有 libc：只用 asm-generic 系统调用
*       号（write 64 / exit 93）。入口仍是 Linux 的 sp 约定。
* @author xw
*/

#define LX_WRITE            64
#define LX_EXIT             93

typedef long lx_long;

static lx_long lx3(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a1 __asm__("a1") = a1v;
    register lx_long r_a2 __asm__("a2") = a2v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a7)
                     : "memory");
    return r_a0;
}

static lx_long lx1(lx_long n, lx_long a0v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0" : "+r"(r_a0) : "r"(r_a7) : "memory");
    return r_a0;
}

static unsigned long s_len(const char *s)
{
    unsigned long n = 0ul;

    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    unsigned long n = s_len(s);

    if (n != 0ul)
    {
        (void)lx3(LX_WRITE, 1, (lx_long)(unsigned long)s, (lx_long)n);
    }
}

static void out_num(lx_long v)
{
    char buf[24];
    int i = 0;
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

static int g_fail = 0;

static void check(const char *what, long got, long want)
{
    out("[dyn] ");
    out(what);
    out(" -> ");
    out_num(got);
    if (got != want)
    {
        out(" FAIL (want ");
        out_num(want);
        out(")\n");
        g_fail++;
    }
    else
    {
        out(" ok\n");
    }
}

/* 重定位槽：内容由 ld.so 在跳回来之前写好，这里只读。 */
long got_answer = 0;    /* R_LARCH_JUMP_SLOT -> answer_add */
long got_gaddr = 0;     /* R_LARCH_JUMP_SLOT -> lib_g_addr */
long got_absptr = 0;    /* R_LARCH_64       -> abs_ptr */
long got_gptr = 0;      /* R_LARCH_64       -> g_ptr   */
long m_base = 7;
long m_rel = 0;         /* R_LARCH_RELATIVE -> &m_base */

/* 线程变量：实测本仓 gcc 按"后声明者放前"排 .tdata，所以把 pad 写在后面，
 * m_tls 的块内偏移就是 8 而不是 0——"槽没被重定位、还是 0、正好读到偏移 0"
 * 这种假绿必须能被下面那条 check 抓到。 */
__thread long m_tls = 77;
__thread long m_tls_pad = 0x1111;
long got_mtls = 0;      /* R_LARCH_TLS_TPREL64 -> m_tls（相对 $tp） */
long got_libread = 0;   /* R_LARCH_JUMP_SLOT  -> lib_tls_read */
long got_libwrite = 0;  /* R_LARCH_JUMP_SLOT  -> lib_tls_write */
long got_libaddr = 0;   /* R_LARCH_JUMP_SLOT  -> lib_tls_addr */
long got_liboff = 0;    /* R_LARCH_JUMP_SLOT  -> lib_tls_off  */

/* libanswer.so 的导出：一律经重定位槽间接调用（不直接 bl，否则链接期未定义） */
extern long answer_add(long x);
extern long *lib_g_addr(void);
extern long *lib_tls_addr(void);
extern long lib_tls_off(void);
extern long lib_tls_read(void);
extern void lib_tls_write(long v);

static unsigned long main_tp(void)
{
    unsigned long v;

    __asm__ volatile("move %0, $tp" : "=r"(v));
    return v;
}

static long *main_tls_addr(void)
{
    return &m_tls;
}

void dyn_main(unsigned long sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  move $a0, $sp\n"
    "  bl   dyn_main\n"
    "1:\n"
    "  b    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void dyn_main(unsigned long sp)
{
    long r;
    long *pa;
    long *pb;
    long *truth;

    (void)sp;

    out("[dyn] main entered through ld.so\n");

    /* 1) JUMP_SLOT：通过被重定位的调用槽调 answer_add(5) */
    r = ((long (*)(long))(*(volatile long *)&got_answer))(5);
    check("JUMP_SLOT answer_add(5)", r, 105);

    /* 2) RELATIVE：m_rel 槽里应是 &m_base，顺指针读到的值应是 7 */
    check("RELATIVE *m_rel", *(volatile long *)&m_rel, (long)(unsigned long)&m_base);
    pa = (long *)*(volatile long *)&m_rel;
    check("RELATIVE value", (pa == &m_base) ? *pa : -1, 7);

    /* 3) R_LARCH_64：got_gptr / got_absptr 里分别是 &g_ptr / &abs_ptr；
     *    这两个槽自己又都存着 &g_base（g_ptr 由 RELATIVE 写成，abs_ptr 由
     *    libanswer 自己的 R_LARCH_64 写成），所以再解一层必须等于
     *    lib_g_addr() 现算出来的真值。 */
    pa = (long *)*(volatile long *)&got_gptr;
    pb = (long *)*(volatile long *)&got_absptr;
    truth = ((long *(*)(void))(*(volatile long *)&got_gaddr))();
    check("R_LARCH_64 g_ptr", (pa != (long *)0) ? (long)(unsigned long)(*pa) : -1,
          (long)(unsigned long)truth);
    check("R_LARCH_64 abs_ptr", (pb != (long *)0) ? (long)(unsigned long)(*pb) : -1,
          (long)(unsigned long)truth);
    check("g_ptr data",
          ((pa != (long *)0) && (*pa != 0)) ? *(long *)(*pa) : -1, 100);

    /* 4) 动态 TLS：槽里存的是"相对 $tp 的偏移"，不是绝对地址 */
    {
        unsigned long tp = main_tp();
        long moff = *(volatile long *)&got_mtls;
        long *lib_addr;

        check("TPREL64 m_tls offset", moff,
              (long)((unsigned long)main_tls_addr() - tp));
        check("TPREL64 offset nonzero", (moff != 0) ? 1 : 0, 1);
        check("TLS main value", m_tls, 77);

        lib_addr = ((long *(*)(void))(*(volatile long *)&got_libaddr))();
        check("TLS lib offset", (long)((unsigned long)lib_addr - tp),
              ((long (*)(void))(*(volatile long *)&got_liboff))());
        check("TLS lib distinct", (lib_addr != main_tls_addr()) ? 1 : 0, 1);
        check("TLS lib value",
              ((long (*)(void))(*(volatile long *)&got_libread))(), 4242);
        ((void (*)(long))(*(volatile long *)&got_libwrite))(4243);
        check("TLS lib after write",
              ((long (*)(void))(*(volatile long *)&got_libread))(), 4243);
        check("TLS main untouched", m_tls, 77);
    }

    if (g_fail == 0)
    {
        out("[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT + TLS all applied\n");
    }
    else
    {
        out("[dyn] FAIL: ");
        out_num(g_fail);
        out(" checks failed\n");
    }

    (void)lx1(LX_EXIT, (g_fail == 0) ? 105 : 1);

    for (;;)
    {
    }
}
