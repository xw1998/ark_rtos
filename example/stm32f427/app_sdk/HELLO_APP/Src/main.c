/**
* @file  main.c
* @brief 一个普通的标准 C 程序：在 Ark RTOS 上作为 App 运行。
* @details 这份源码不需要任何 RTOS 头文件、不需要 `AppMain()`、不需要
*          `while(1)`。它和你在 Linux / Windows 上写的那段程序逐字相同：
*
*              cc main.c && ./a.out      # 主机上
*
*          `printf` 由 SDK（`kernelsrc/sdk/app_sdk/arkrtos_stdio.c`）接到内核
*          控制台；`return 0` 等于进程结束——板上没有进程，于是结束这个 App
*          任务，栈由内核回收。
*/

#include <stdio.h>

int main()
{
    printf("Hello, World!\n");
    return 0;
}
