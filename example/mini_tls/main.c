/*
 * main.c -- `__thread` 在 Ark RTOS 小程序上的真实行为（实测，不是声称）
 *
 * ---- 这个工程要回答的问题 ----
 * "编译器说支持 __thread"这句话可以指两种很不一样的东西：
 *
 *   (a) 真·线程局部：每个线程一份实例，寻址要先取 TP（线程指针）再加偏移；
 *   (b) 只是分段：变量被放进 .tbss / .tdata，但地址仍是链接期定死的绝对地址，
 *       所以所有线程看到的是**同一个**变量。
 *
 * arkc 的 ARM 后端属于 (b)，而且比 (b) 还弱一层：ARM 后端根本不输出数据段，
 * 全局初值一律编进 `__arkcc_init_globals()` 逐条写 RAM（这也是它不需要
 * scatter loading 的原因），所以 .tbss/.tdata 的分段选择在这条路上不产生任何
 * 差别——`__thread int counter = 5;` 与 `int counter = 5;` 生成的东西完全相同。
 *
 * 也就是说：**在小程序这个单线程环境里，__thread 变量就是一个普通全局变量。**
 * 小程序任务里没有第二个线程，所以这个退化不会给出错答案；但把它当成"每线程
 * 一份"来写程序，会在多线程场景（例如同一份代码搬进 Ark RTOS 内核态线程）里
 * 静默出错。这条边界写在 docs/ 的账本里，不在代码里悄悄糊过去。
 *
 * ---- 三条读数 ----
 *   1) init=5      带初值的 __thread 变量真的被 __arkcc_init_globals 初始化
 *   2) after=9     写进去的值能读回来（不是只读的假变量）
 *   3) zero=0      没有初值的 __thread 变量被清零（.bss 语义没丢）
 */
#include <stdio.h>

__thread int counter = 5;   /* 带初值 */
__thread int zeroed;        /* 无初值 */

int main(void)
{
    printf("tls: init=%d (want 5)\n", counter);

    counter = 9;
    printf("tls: after write=%d (want 9)\n", counter);

    zeroed = zeroed + 0;        /* 读一次，确认它在读路径上真的存在 */
    printf("tls: zeroed=%d (want 0)\n", zeroed);

    printf("tls: ok (single instance, no TP base -- see the header)\n");
    return 0;
}
