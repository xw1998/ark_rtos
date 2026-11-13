/**
* @file  main.c
* @brief 一个普通的标准 C 程序：在 Ark RTOS 上作为**小程序**运行。
* @details 与 `HELLO_APP/Src/main.c` 逐字相同——同一段源码既能编成 App，
*          也能编成小程序，差别只在工程里链接哪份 SDK 启动桩：
*
*              App   : kernelsrc/sdk/app_sdk   （常驻槽位，开机自启）
*              小程序: kernelsrc/sdk/mini_sdk  （文件系统里的 .svcapp，跑完归还内存）
*
*          `main` 返回即程序结束；小程序的内核钩子随即归还代码块与 RAM 块。
*/

#include <stdio.h>

int main()
{
    printf("Hello, World!\n");
    return 0;
}
