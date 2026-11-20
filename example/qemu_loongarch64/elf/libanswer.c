/**
* @file libanswer.c
* @brief 动态链接夹具里的"共享库"（QEMU LoongArch64）
* @details 它被 tools/elf/dynfixture.py 封装成一个带 .dynsym / .rela 的
*          ET_DYN 镜像——本机工具链的 ld 2.31.1 不支持 -shared，所以动态
*          元数据由 Python 构建器按清单生成。这里只放"要被重定位的东西"：
*
*            g_base        导出的数据符号（值 100）
*            g_ptr         一个指针槽，靠 R_LARCH_RELATIVE 在装载时被写成
*                          &g_base（链接期的值是 0，不是链接器算出来的）
*            abs_ptr       一个指针槽，靠 R_LARCH_64 在装载时被写成 &g_base
*            got_answer    一个调用槽，靠 R_LARCH_JUMP_SLOT 被写成
*                          answer_add 的运行期地址
*            lib_g_addr()  用 PC 相对寻址现算 &g_base，作为"真值"与上面
*                          三条重定位的结果对照：只有重定位真的发生过，
*                          它们才会相等
*
*           lib_tls       一个 __thread 变量（初始 4242）。本对象的代码不能
*                          直接读它：库被排在线程块的偏移 16，而 local-exec
*                          的 `$tp + 0` 是静态链接时按"块起点 = 0"算出来的。
*                          所以这里只经 got_libtls（由 R_LARCH_TLS_TPREL64
*                          在装载时写成"相对 $tp 的偏移"）访问——这正是
*                          动态 TLS 要证明的那条路径。
*
* @note 这里没有任何 libc 调用；volatile 是为了不让 -O2 把槽读成常量。
* @author xw
*/

volatile long g_base = 100;
volatile long *g_ptr = 0;
long *abs_ptr = 0;
long got_answer = 0;

__thread long lib_tls = 4242;
long got_libtls = 0;    /* R_LARCH_TLS_TPREL64 -> lib_tls（相对 $tp） */

static unsigned long lib_tp(void)
{
    unsigned long v;

    __asm__ volatile("move %0, $tp" : "=r"(v));
    return v;
}

long *lib_tls_addr(void)
{
    return (long *)(lib_tp() + (unsigned long)*(volatile long *)&got_libtls);
}

/* 库自己的 TPREL 槽内容（相对 $tp 的偏移）——给主程序做跨对象对照用：
 * 主程序拿 lib_tls_addr() - $tp 与它比，等式成立才说明符号解析是跨对象
 * 找到本对象的 tls_off，而不是拿主程序自己的偏移凑出来的。 */
long lib_tls_off(void)
{
    return *(volatile long *)&got_libtls;
}

long lib_tls_read(void)
{
    return *(volatile long *)lib_tls_addr();
}

void lib_tls_write(long v)
{
    *(volatile long *)lib_tls_addr() = v;
}

long answer_add(long x)
{
    return x + g_base;
}

long *lib_g_addr(void)
{
    return (long *)&g_base;
}
