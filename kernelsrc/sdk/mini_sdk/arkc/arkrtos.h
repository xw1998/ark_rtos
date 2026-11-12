/**
* @file arkrtos.h
* @brief 小程序 SDK 的 arkc 版最小内核声明（只够编 miniapp）
* @details 这份头为什么存在：`sdk/mini_sdk/arkrtos_mini_main.c` 里写的是
*          `#include "arkrtos.h"`，而完整的那一份（`kernelsrc/include/arkrtos.h`）
*          把内核对外的一整套类型、SVC 门面、日志接口都摊开，编进小程序时：
*            - 日志格式化器需要 va_list，arkc 还没有（arkrtos_ulog.h 已按
*              `__ARK_CC__` 整段排除，但类型与宏仍然会被解析）；
*            - 这些声明在小程序里一个都用不到，只会拖长编译输入。
*
*          所以 arkc 的小程序工程把 `-I` 指到本目录，`"arkrtos.h"` 就落在
*          这里。里面只有 miniapp 真正需要的东西：
*            - 入口约定：**工程自己写 `int main(void)`**，与 Linux 上一模一样。
*              SDK 的启动桩（`arkrtos_mini_start_arkc.c`）调它，它 return
*              之后由桩调 `arkrtos_thread_exit()` 收尾。SDK 不提供 main()，
*              漏定义就是链接期 `undefined symbol "main"`。
*              三个工具链现在是同一条约定：入口 = 工程自己写的标准
*              `int main(void)`，SDK 不提供 main。
*            - `ARKRTOS_WEAK`：弱符号属性。arkc 档自己也用不到它，留着是因为
*              工程可能想给自己的回调加弱定义。
*            - `arkrtos_thread_exit()`：结束小程序、交还两块 RAM。
*
*          用别的工具链（Keil / GCC）编小程序时不要包含本目录，否则会拿到
*          只有两个声明的 arkrtos.h。
*/

#ifndef __ARKRTOS_MINI_ARKC_ARKRTOS_H__
#define __ARKRTOS_MINI_ARKC_ARKRTOS_H__

/* 与 kernelsrc/include/arkrtos_types.h 里的定义相同：弱符号。
 * arkc 档自己不依赖它（入口是本工程的 int main），留着是因为工程可能想给
 * 自己的回调加弱定义。注意 arkc 的自包含对象模型：同一翻译单元内的调用在
 * 汇编期就绑死，弱定义覆盖不到，别拿它当"可覆盖的默认实现"用。 */
#ifndef ARKRTOS_WEAK
#define ARKRTOS_WEAK                    __attribute__((weak))
#endif

/* 入口不在这个头里：入口就是标准 C 的 `int main(void)`，由工程自己定义，
 * 不需要声明（启动桩里有 `extern int main(void);`）。
 * 旧写法 `void MiniMain(void)` 已经取消：Keil / GCC 档同样是工程自己写 main，
 * SDK 只提供退出胶水（arkrtos_mini_main.c，arkc 档 #error 拦住）。 */

/**
* @brief 结束当前小程序，把代码块与 RAM 块交还给内核
* @note 实现见 sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c：SVC 0x1B 子命令 3，
*       与 app_sdk 的 arkrtos_oslib.c 是同一条。
*/
void arkrtos_thread_exit(void);

#endif /* __ARKRTOS_MINI_ARKC_ARKRTOS_H__ */
