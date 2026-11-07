/**
* @brief Ark RTOS 小程序 SDK - 入口骨架
* @details 小程序由内核的装载器从文件系统读进 RAM 执行（见
*          docs/小程序设计.md），因此它：
*            - 不需要关心自己被放在哪里：镜像里的地址由重定位表在装载时改好；
*            - 没有固定的分区槽位，退出后内核立刻归还代码块与 RAM 块。
*
*          MiniMain() 返回即视为「程序结束」：这里调用 arkrtos_thread_exit()
*          把自己交给内核回收，之后再被调度到也不会继续跑。
*/

#if defined(__ARK_CC__)
/* arkc 档不要把这个文件放进工程。
 *
 * 小程序在 arkc 上按标准 C 写 `int main(void)`：入口由启动桩
 * （arkrtos_mini_start_arkc.c）直接调用，程序 return 之后由桩调
 * arkrtos_thread_exit() 收尾。这里那份「main() 调 MiniMain()」的骨架是
 * Keil / GCC 档的写法（那两个工具链没有本文件同翻译单元弱符号的问题）。
 *
 * 两个档混用的结果不是报错而是**两套入口都在**：多一个 main 只会让链接器
 * 挑一个，另一个静默失效。所以这里直接 #error。 */
#error "arkc: do not add mini_main.c; write int main(void) in your own source instead"
#else
#include "arkrtos.h"

/* 【arkc 为什么没有这一份默认实现】
 * 这里本来是一个 `ARKRTOS_WEAK void MiniMain(void) {}`，让没定义 MiniMain 的
 * 小程序也能链接。在 Keil / GCC 下它是对的：那个空实现与 main() 不在同一个
 * 翻译单元，链接器按"强符号覆盖弱符号"把 main() 的调用改到工程给的那份。
 *
 * arkc 不是这样工作的：它把每个 .c 编成一个自包含对象（代码 + 符号表），
 * **同一个翻译单元内**对已定义符号的调用在汇编期就绑死了，链接期不再改。
 * 于是空实现与 main() 同处一文件时，main() 调的就是这个空函数——工程自己
 * 那份强 MiniMain 永远进不来。板上实测：不打印、创建当拍就退出（不报错，
 * 也没有任何诊断），比链接失败难查得多。
 *
 * 所以 arkc 档**不提供默认实现**：小程序必须自己定义 MiniMain()，没定义就
 * 在链接期报 `undefined symbol MiniMain`——这正是想要的答案。
 * 用别的工具链（Keil / GCC）时这个宏不成立，行为与本文件旧版逐字节相同。 */
#if !defined(__ARK_CC__)
ARKRTOS_WEAK void MiniMain(void)
{
}
#endif

int main(void)
{
    MiniMain();

    /* 一次性程序：跑完就退。内核在任务退出钩子里归还两块 RAM，
     * 下一次 "mini run <path>" 会重新从文件系统装载。 */
    arkrtos_thread_exit();

    /* arkrtos_thread_exit() 不会再返回；这一句只是防御性的兜底，
     * 保证就算退出路径出了意外也不会跑到别的代码上。 */
    for(;;)
    {
    }
}
#endif /* __ARK_CC__ */
