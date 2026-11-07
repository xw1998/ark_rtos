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
*            - `MiniMain()` 的声明：**必须由工程自己定义**。SDK 的
*              `arkrtos_mini_main.c` 在 arkc 档不给默认空实现（原因写在那个文件
*              里：同 TU 内的弱符号在 arkc 的单对象模型下会被本地绑定，强定义
*              覆盖不进来），所以漏定义会得到一个诚实的 `undefined symbol`。
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
 * 小程序用它是为了让 SDK 提供一个空的 MiniMain()，工程自己定义的那份
 * 覆盖它——没有这一条，SDK 的 main() 会因为没有 MiniMain 而链接失败。 */
#ifndef ARKRTOS_WEAK
#define ARKRTOS_WEAK                    __attribute__((weak))
#endif

/**
* @brief 小程序主体：由工程实现，SDK 的 main() 调用它
* @note 必须定义。返回即视为程序结束（`arkrtos_thread_exit()` 交还两块 RAM）。
* @note 这里只有声明，没有弱定义的默认实现——arkc 的同一个翻译单元内的调用在
*       汇编期就绑死，弱符号覆盖不到，带一个空默认实现只会让"程序什么都没做
*       就退出"变成最难查的一种静默错误。详见 `arkrtos_mini_main.c`。
*/
void MiniMain(void);

/**
* @brief 结束当前小程序，把代码块与 RAM 块交还给内核
* @note 实现见 sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c：SVC 0x1B 子命令 3，
*       与 app_sdk 的 arkrtos_oslib.c 是同一条。
*/
void arkrtos_thread_exit(void);

#endif /* __ARKRTOS_MINI_ARKC_ARKRTOS_H__ */
