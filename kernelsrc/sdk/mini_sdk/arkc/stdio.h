/**
* @file stdio.h
* @brief 小程序 SDK 的 POSIX 控制台输出子集（arkc 版）：printf / puts / putchar
* @details 这里只有「打印到控制台」这一件事，目的是让一份标准写法的程序
*          （`#include <stdio.h>` + `printf("...")` + `int main(void)`）原样
*          在板上编出来跑。文件、格式化输入、缓冲区管理一概没有。
*
*          【printf 是真的变参，不是宏分派】
*          做法是在 SDK 的 rt.c 里把 printf 定义成一个 `__asm` 函数：函数序言
*          把 r0-r3 与调用方压栈的 4 个字一起落到自己栈上的 8 个槽，再把这块
*          内存交给一个普通 C 函数去按格式串取值（见
*          arkrtos_mini_printf_words()）。对调用方来说它就是一条普通调用，
*          格式串里的转换与参数按出现顺序一一对应，和桌面上的 printf 一样。
*
*          【为什么不用 <stdarg.h> 重写它】
*          arkc 现在有 va_list（`#include <stdarg.h>`，见 SDK 里那份），所以
*          printf 完全可以用 `va_start`/`va_arg` 写。这里保留取参桩有两个理由：
*          这条桩已经在板上验证过；而且它是 SDK 的一部分，不依赖编译器那套
*          变参实现，编译器以后换实现也不影响它。自己写变参函数直接用
*          `#include <stdarg.h>` 就行，代价只有 16 字节栈。
*
*          【硬上限：一次调用最多 8 个参数（格式串 + 7 个）】
*          这个 8 不是拍出来的，是 arkc 自己的调用约定上限：前 4 个参数走
*          r0-r3，第 5-8 个参数由调用方压栈，超过 8 个的部分编译器直接不传
*          （实测：9 个参数时第 9 个根本没进镜像）。所以 printf 也看不第 9 个
*          —— 传多了不会报错，多出来的那个就是拿不到。要在板上验证这一点，
*          看 SDK 的 utest 或 docs/arkc板上直跑-M4-hello演示.md。
*
*          【支持的转换】
*            %d %i   十进制有符号（int）
*            %u      十进制无符号
*            %x %X   十六进制，小写 / 大写
*            %c      单个字符
*            %s      NUL 结尾字符串（空指针打印 "(null)"）
*            %p      指针，按 0x 加 8 位十六进制
*            %%      一个百分号
*          标志与宽度：`-`（左对齐）、`0`（补零）、1-2 位十进制宽度，例如
*          %5d / %-5s / %05x / %08x。
*          **没有**：精度（`%.3s`）、长度修饰（`%ld`/`%lld`）、浮点（`%f`）、
*          字段宽度里的 `*`。这些不是"以后再补"，是这个 SDK 不做——用到就
*          自己按需拼字符串，别指望它。
*
*          【返回值】
*          与 C 库相同：写出的字符数。写失败也返回字符数（控制台失败对小
*          程序没有可做的事，返回值不假装能反映它）。
*/

#ifndef __ARKRTOS_MINI_STDIO_H__
#define __ARKRTOS_MINI_STDIO_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief 按格式串输出到控制台
* @param fmt 格式串，支持的转换见文件头
* @return 写出的字符数
* @note 一次调用最多 8 个参数（含格式串）。格式化结果按 96 字节一段写出，
*       不会因为调用方的缓冲而截断。
*/
int printf(const char *fmt, ...);

/**
* @brief 输出一个字符串再加一个换行
* @return 非负值（成功）／-1（空指针）
* @note 与 C 库一致：换行由 puts 加，字符串本身不要带。
*/
int puts(const char *s);

/**
* @brief 输出一个字符
* @param c 要输出的字符（只用低 8 位）
* @return 写出的字符（即 c）
*/
int putchar(int c);

#ifdef __cplusplus
}
#endif

#endif /* __ARKRTOS_MINI_STDIO_H__ */
