/**
* @brief Ark RTOS App / 小程序 SDK - 标准 C 控制台输出
* @details 让用户源码里最常见的这几句直接可用：
*
*              #include <stdio.h>
*              printf("Hello, World!\n");
*
*          实现不依赖任何 C 库的 retarget（`_write` / `fputc` / `__io_putchar`
*          各工具链名字都不一样），而是直接用 SDK 已有的格式化器
*          （`arkrtos_ufmt_v`）把结果渲染到栈上缓冲，再走一条
*          `arkrtos_shell_print()`（SVC 0x1A）交给内核控制台。
*
*          这样 AC5 / AC6 / GCC 三个工具链是同一份代码、同一条输出路径，
*          也不需要为 `printf` 把整套 C 库的重定向机器链进来。
*
*          **换行**：串口控制台是裸字节流，`'\n'` 只把光标往下移、不回列。
*          内核自己的输出一律写 `"\r\n"`，而标准 C 程序写的是 `'\n'`。
*          所以这里按终端惯例做 ONLCR：`'\n'` 前面没有 `'\r'` 时补一个
*          `'\r'`。用户写 `"\r\n"` 不会被补成 `"\r\r\n"`。
*
*          支持的转换与 `arkrtos_ulog.h` 完全一致：
*              %d %i %u %x %X %s %c %%
*              '0' 补零与十进制宽度（如 %08X）
*          单条输出上限 `ARKRTOS_UFMT_BUF_SIZE`（默认 128 字节，含结尾 NUL），
*          这也是内核 SVC 0x1A 子命令 3 的硬上限（内核侧按 128 字节查长度）。
*
*          @note 本文件定义的是**强符号** `printf` / `puts` / `putchar`。
*                如果工程里已经用 `arkrtos_libc_glue.c`（POSIX_DEMO /
*                FS_DEMO / SOCKET_DEMO 那一套）走 newlib/AC6 的重定向，
*                就不要再编本文件——两套输出只能选一套，否则链接期会出现
*                重复定义。
*/

#include <stdio.h>
#include <stdarg.h>

#include "arkrtos_ulog.h"

/* 一次 SVC 最多送多少字节（内核侧上限 128，留出余量）。分段是为了让"补 '\r'"
 * 不会把一条已满的缓冲顶爆。 */
#define ARKRTOS_STDIO_CHUNK     (96u)

/**
* @brief 把一段文本送到内核控制台，`'\n'` → `"\r\n"`
* @param s NUL 结尾字符串（可为 NULL）
* @note 分段发送：每段不超过 ARKRTOS_STDIO_CHUNK 字节，因为内核 SVC 0x1A
*       子命令 3 会拒收 128 字节及以上的串。分段是按字节边界切的，切点只可能
*       落在两个完整字节之间，不会把 `"\r\n"` 拆坏（`'\r'` 先入缓冲）。
*/
static void stdio_out(const char *s)
{
    char seg[ARKRTOS_STDIO_CHUNK + 1u];
    uint32 n = 0u;
    char prev = '\0';

    if(s == 0)
    {
        return;
    }

    while(*s != '\0')
    {
        char c = *s;

        if((c == '\n') && (prev != '\r'))
        {
            seg[n] = '\r';
            n++;
        }

        seg[n] = c;
        n++;
        prev = c;
        s++;

        if((c == '\n') || (n >= ARKRTOS_STDIO_CHUNK))
        {
            seg[n] = '\0';
            (void)arkrtos_shell_print(seg);
            n = 0u;
        }
    }

    if(n != 0u)
    {
        seg[n] = '\0';
        (void)arkrtos_shell_print(seg);
    }
}

/**
* @brief 格式化并输出到内核控制台（标准 C `printf` 语义）
* @return 写入的字符数（不含结尾 NUL）；被截断时返回实际格式化的字符数
*/
int printf(const char *fmt, ...)
{
    char    buf[ARKRTOS_UFMT_BUF_SIZE];
    va_list ap;
    int     n;

    if(fmt == 0)
    {
        return -1;
    }

    va_start(ap, fmt);
    n = arkrtos_ufmt_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if(n > 0)
    {
        stdio_out(buf);
    }

    return n;
}

/**
* @brief 输出字符串并追加换行（标准 C `puts` 语义）
* @return 成功返回非负数，参数为空返回 -1
*/
int puts(const char *s)
{
    if(s == 0)
    {
        return -1;
    }

    stdio_out(s);
    stdio_out("\n");

    return 0;
}

/**
* @brief 输出单个字符（标准 C `putchar` 语义）
* @return 写出的字符；参数为 EOF 时返回 EOF
*/
int putchar(int c)
{
    char buf[2];

    if(c == EOF)
    {
        return EOF;
    }

    buf[0] = (char)c;
    buf[1] = '\0';
    stdio_out(buf);

    return (unsigned char)c;
}
