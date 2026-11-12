/**
* @brief Ark RTOS App SDK - 入口与退出胶水
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
*          启动桩（`arkrtos_app_start.s` / `arkrtos_app_start_gcc.s`）完成
*          `.data` 初值拷贝与 `.bss` 清零后直接调用 `main()`。SDK **不再
*          提供 `main()`**，也不再要求用户写 `AppMain()`——用户源码里就是
*          一段能在 Linux / Windows 上直接编译的普通 C 程序。
*
*          返回语义：C 标准里 `main` 返回等于进程结束；这里没有进程，只有
*          任务，所以 `return` 或 `exit()` 都会结束当前 App 任务（SVC
*          0x1B/3），由内核回收该任务的栈。App 若另有线程，它们继续运行。
*
*          Keil(microlib/AC6) 的退出链是 `exit -> _sys_exit`，GCC(newlib)
*          是 `exit -> _exit`。这里两个都给出**强定义**，避免落到库的默认桩
*          （默认桩是 `BKPT` 或死循环，程序会莫名其妙停在 HardFault）。
*          同一个工程里不要再把 `arkrtos_libc_glue.c` 也编进来：那份文件
*          提供了另一套 `_sys_exit`，二者只能选一（普通 App 工程默认不含它；
*          POSIX_DEMO / FS_DEMO / SOCKET_DEMO 用的是 libc_glue 那一套）。
*/

#include "arkrtos.h"

/**
* @brief 标准 C 的退出路径：结束当前线程
* @param code 退出码（保留；当前内核不按退出码做判定）
* @note 不会返回。App 的其它线程不受影响。
*/
void arkrtos_app_exit(int code)
{
    (void)code;

    arkrtos_thread_exit();

    /* arkrtos_thread_exit() 不再返回；这一句只是防御性兜底，保证就算退出
     * 路径出了意外也不会跑到别的代码上。 */
    for(;;)
    {
    }
}

#if defined(__CC_ARM) || defined(__ARMCC_VERSION) || defined(__GNUC__)
/* 两条退出链都接上：AC5/AC6 走 `_sys_exit`，GCC/newlib 走 `_exit`。 */
void _sys_exit(int code)
{
    arkrtos_app_exit(code);

    /* arkrtos_app_exit() 不会返回；这一句只是让编译器确信本函数没有可达的末尾
     * （GCC 把 `_exit` 声明为 noreturn，走到末尾会报
     * "'noreturn' function does return"）。 */
    for(;;)
    {
    }
}

void _exit(int code)
{
    arkrtos_app_exit(code);

    /* arkrtos_app_exit() 不会返回；这一句只是让编译器确信本函数没有可达的末尾
     * （GCC 把 `_exit` 声明为 noreturn，走到末尾会报
     * "'noreturn' function does return"）。 */
    for(;;)
    {
    }
}
#endif
