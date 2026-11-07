/*
 * va.c -- C 变参函数在 Ark RTOS 小程序里跑（arkc 的 stdarg.h）
 *
 * 这份工程存在的理由：变参支持是"编译器里最容易被自己的门禁骗过去"的一块。
 * 宿主上门禁（examples/run_va_gate.py）拿 unicorn 跑 arkcc 的产物，但那走的
 * 是主机侧汇编器；板上的 `cc --pack` 走 arkc 自己的内部汇编器，只有真跑过
 * 才算数。
 *
 * 每条打印对应一项：寄存器实参、栈实参、两者混合、long long、float 提升成
 * double、以及"栈参数很多"（20 个参数：4 个在寄存器 + 16 个在栈上）。
 */
#include <stdio.h>
#include <stdarg.h>

/* 求和：n 之后的 n 个 int 全在变参里。6 个实参里有 2 个必须走栈。 */
static int sum(int n, ...)
{
    va_list ap;
    int s = 0, i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

/* 64 位实参：读 va_arg(ap, long long) 的高字最高字节（0x1122334455667788 ->
 * 0x11 = 17）。64 位实参在 ARM 上占相邻两个字，va_arg 要先取整到 8 字节；
 * 这条是整套变参里唯一能分辨"取整到 8 字节"对错的用例——取整没生效时读到
 * 的是低字最高字节 0x55 = 85。 */
static int ll_hi(int n, ...)
{
    union { long long v; int w[2]; } u;
    va_list ap;
    va_start(ap, n);
    u.v = va_arg(ap, long long);
    va_end(ap);
    return (u.w[1] >> 24) & 0xFF;
}

/* 命名参数把寄存器用掉，变参尾巴才从栈上开始（1 个命名 + 5 个变参：
 * r1..r3 三个 + 栈上两个）。结果是 1+2+3+4+5+6 = 21。 */
static int tail(int a, ...)
{
    va_list ap;
    int s = a, i, x;
    va_start(ap, a);
    for (i = 0; i < 5; i++) {
        x = va_arg(ap, int);
        s += x;
    }
    va_end(ap);
    return s;
}

/* float 实参按 C99 6.5.2.2p6 提升成 double：1.5 的位模式高字是 0x3FF80000，
 * 取 0x3FF 的低 8 位 = 255。这条能过，说明提升 + 64 位读都对。 */
static int dbl_tag(int n, ...)
{
    union { double d; unsigned int w[2]; } u;
    va_list ap;
    va_start(ap, n);
    u.d = va_arg(ap, double);
    va_end(ap);
    return (u.w[1] >> 20) & 0xFF;
}

int main(void)
{
    printf("va: companion built with the SDK's stdarg.h\r\n");
    printf("va sum6      = %d (want 21)\r\n", sum(6, 1, 2, 3, 4, 5, 6));
    printf("va sum20     = %d (want 210)\r\n", sum(20, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 20));
    printf("va ll_hi     = %d (want 17)\r\n", ll_hi(1, 0x1122334455667788LL));
    printf("va tail      = %d (want 21)\r\n", tail(1, 2, 3, 4, 5, 6));
    printf("va dbl_tag   = %d (want 255)\r\n", dbl_tag(1, 1.5f));
    printf("va: done\r\n");
    return 0;
}
