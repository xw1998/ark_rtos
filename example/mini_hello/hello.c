/**
* @file hello.c
* @brief 标准 POSIX 的 hello world，作为 Ark RTOS 小程序编译并运行
* @details 这个文件里没有一句 Ark RTOS 的东西：只有 <unistd.h>、write() 和
*          MiniMain()。它在 Linux 上是 `cc hello.c && ./hello`，在这里是
*          `/mnt/nor 里 make，然后 mini run hello.svcm`。
*
*          为什么入口叫 MiniMain() 而不是 main()：
*          小程序 SDK（kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c）提供 main()，
*          它调 MiniMain()，返回后调 arkrtos_thread_exit() 把两块 RAM 交还内核。
*          也就是说「程序体」由本文件提供，「进程收尾」由 SDK 提供 —— 和桌面
*          上由 C 运行时替你 exit 是一回事，只是这里收尾的是内核。
*
*          为什么用 write() 而不是 printf()：
*          这里没有 C 库。printf 需要一个带 va_list 的格式化器，而 arkc 目前
*          没有实现 varargs；SDK 的格式化器（arkrtos_ulog.h）用 #if
*          !defined(__ARK_CC__) 整段排除了。所以小程序的控制台输出就是
*          POSIX 的 write()。整条演示（工程怎么进 NOR、怎么 make、SDK 各文件
*          干什么、有哪些硬限制）见 docs/arkc板上直跑-M4-hello演示.md。
*
*          编译方式（板上，项目文件在 NOR 上）：
*            cd /mnt/nor
*            make                 # 读 makefile，四遍差分打包
*            mini run hello.svcm  # 装载执行，退出后两块 RAM 归还
*/

#include <unistd.h>

#define GREETING "hello, world\n"

void MiniMain(void)
{
    /* sizeof("...") - 1 是字符数（C11 6.5.3.4，结尾 '\0' 不算）。
     * 不写死长度：改字面量不会漏改长度。 */
    (void)write(STDOUT_FILENO, GREETING, sizeof(GREETING) - 1);
}
