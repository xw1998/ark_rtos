/**
* @brief Ark RTOS 小程序 SDK - 入口与退出胶水（Keil / GCC 档）
* @details 入口是**用户自己写的标准 C `main()`**：
*
*              #include <stdio.h>
*
*              int main(void)
*              {
*                  printf("Hello, World!\n");
*                  return 0;
*              }
*
*          启动桩（`arkrtos_mini_start.s` / `arkrtos_mini_start_gcc.s`）完成
*          `.data` 初值拷贝与 `.bss` 清零后直接调用 `main()`。SDK **不再
*          提供 `main()`**，也不再要求用户写 `MiniMain()`。
*
*          返回语义：小程序是"跑完就退"的一次性程序，`main` 返回（或调用
*          `exit()`）即结束任务，内核立刻归还代码块与 RAM 块——与标准 C
*          的进程退出一致。
*
*          arkc 档**不要编这个文件**：arkc 的入口同样是工程自己的
*          `int main(void)`，但它的退出由启动桩
*          （`arkrtos_mini_start_arkc.c`）负责，不需要这里的胶水；更重要的
*          是 arkc 把每个 `.c` 编成自包含对象，同一翻译单元内的调用在汇编期
*          就绑死，多一份骨架只会让入口静默失效。这里直接 `#error`。
*/

#if defined(__ARK_CC__)
#error "arkc: do not add mini_main.c; write int main(void) in your own source instead"
#endif

#include "arkrtos.h"

/**
* @brief 标准 C 的退出路径：结束当前小程序任务
* @param code 退出码（保留；当前内核不按退出码做判定）
* @note 不会返回。内核在任务退出钩子里归还代码块与 RAM 块。
*/
void arkrtos_mini_exit(int code)
{
    (void)code;

    arkrtos_thread_exit();

    for(;;)
    {
        /* 不会返回；防御性兜底 */
    }
}

#if defined(__CC_ARM) || defined(__ARMCC_VERSION) || defined(__GNUC__)
/* Keil(microlib/AC6) 走 `_sys_exit`，GCC(newlib) 走 `_exit`。 */
void _sys_exit(int code)
{
    arkrtos_mini_exit(code);

    /* arkrtos_mini_exit() 不会返回；这一句只是让编译器确信本函数没有可达的末尾
     * （GCC 把 `_exit` 声明为 noreturn，走到末尾会报
     * "'noreturn' function does return"）。 */
    for(;;)
    {
    }
}

void _exit(int code)
{
    arkrtos_mini_exit(code);

    /* arkrtos_mini_exit() 不会返回；这一句只是让编译器确信本函数没有可达的末尾
     * （GCC 把 `_exit` 声明为 noreturn，走到末尾会报
     * "'noreturn' function does return"）。 */
    for(;;)
    {
    }
}
#endif
