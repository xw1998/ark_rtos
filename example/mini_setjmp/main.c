/*
 * main.c -- setjmp / longjmp 在 Ark RTOS 小程序里跑（arkc + SDK 的 setjmp.s）
 *
 * 这个工程里只有标准 C（<setjmp.h> + setjmp + longjmp）——在 Linux 上就是
 * `cc main.c && ./a.out`。在这里是：
 *
 *   cd /mnt/nor/sj
 *   make
 *   mini run sj.svcm
 *
 * ---- 为什么值得单独一条工程 ----
 * setjmp 必须保存"调用它的那个人的状态"：调用点的 sp 与返回地址。用 C 写一层
 * 转发就错了，而且错得很像对的——转发函数的帧在它 return 之后就还给了栈，下一
 * 次调用就会把里面存的东西盖掉。这里实测过：先把 blob 包在一个 C 函数里，跨
 * 帧 longjmp 之后控制流"正常返回"到了 main，带回的值是 deep() 的返回值而不是
 * longjmp 的值。所以 setjmp/longjmp 本身就是那两段指令序列（见 /mnt/nor/setjmp.s），
 * 由调用者直接进入。
 *
 * ---- 三条读数各证明什么 ----
 *   1) first pass r=0        直接调用返回 0
 *   2) jumped back r=7       跨 6 层递归帧 longjmp(env, 7)，值原样带回
 *   3) longjmp(env,0) -> 1   C99 7.13.2.1p2：val 为 0 时必须读回 1
 * 只有 2) 能分辨"真的跳回来了"与"函数顺着正常返回路径回来的"：后者会走下面
 * 那条 FAIL 分支并打出 dive 的返回值和 0 帧——这两行一旦出现，失败是显式的，
 * 不会伪装成通过。
 */
#include <stdio.h>
#include <setjmp.h>

static jmp_buf back;      /* setjmp 的目标上下文 */
static int     frames;    /* dive() 进过几层，用来把"跳回了多远"变成读数 */

/* 每层留一点活着的局部（volatile 保证它真的占栈），到最深处跳走。 */
static int dive(int n)
{
    volatile int mark[4];

    mark[0] = n;
    mark[1] = n + 1;
    frames++;

    if (n == 0)
        longjmp(back, 7);          /* 不返回 */

    return dive(n - 1) + mark[0] + mark[1];
}

int main(void)
{
    int r;
    int z;

    r = setjmp(back);
    if (r == 0) {
        printf("sj: first pass r=%d\n", r);
        frames = 0;
        (void)dive(5);
        /* 走到这里说明 longjmp 没跳回来，而是让 dive 正常返回了。 */
        printf("sj: dive returned normally, frames=%d -- FAIL\n", frames);
        return 1;
    }

    printf("sj: jumped back r=%d (want 7)\n", r);
    printf("sj: frames abandoned = %d (want 6)\n", frames);
    if (r != 7)
        return 2;

    /* 同一个 jmp_buf 可以重用：第二次 setjmp 覆盖它。val=0 必须变成 1。 */
    z = setjmp(back);
    if (z == 0)
        longjmp(back, 0);
    printf("sj: longjmp(env, 0) -> %d (want 1)\n", z);
    if (z != 1)
        return 3;

    printf("sj: ok\n");
    return 0;
}
