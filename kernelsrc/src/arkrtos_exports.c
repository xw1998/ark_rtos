/**
* @file arkrtos_exports.c
* @brief 内核符号导出表：可下载模块能按名字用到的那一份白名单
* @details 表的每一行都是「模块里写 extern，装载器按名字找到这里」。
*          三条约束写死在这里：
*            1. **只导出内核自己的符号**，不导出示例程序里的东西
*               （例如 QEMU 示例的 libc_shim.c 里的 snprintf）——那种符号
*               换一块板就不存在，导出它等于给模块一个只在演示里成立的承诺；
*            2. 可能让模块把内核弄坏的符号不进表（改 TCB 内部字段、
*               删别的任务、写 MPU 寄存器）；
*            3. 名字必须与 nm 里的符号逐字相同，装载器不做任何模糊匹配。
*
*          arkrtos_mod_printf 是本文件自己实现的：内核里有 snprintf，
*          但它来自平台侧（示例的 libc_shim / newlib / armcc），换目标不一定
*          在；模块的打印输出不该赌一个平台的库函数在不在。
*
* @note 开关：ARKRTOS_USE_MODLOAD。关闭时本文件是空目标（零体积零符号）。
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_MODLOAD == 1)

#include <stdarg.h>

#include "arkrtos_export.h"
#include "arkrtos_log.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"        /* arkrtos_task_table：槽号要按 TCB 下标算 */

/* ---------------------------------------------------------------- 模块打印 */

#ifndef ARKRTOS_MOD_PRINTF_BUF
#define ARKRTOS_MOD_PRINTF_BUF      (192u)
#endif

/* 一个任务一份缓冲：模块可能同时有几个任务在打，共用一份会把两行搅在一起。
 * 用「当前任务号取模」而不是全局一份——错了顶多是两个人抢一个槽，
 * 不会出现「A 的行里混进 B 的字」那种更难查的形态。 */
static char s_mod_printf_buf[4][ARKRTOS_MOD_PRINTF_BUF];

static uint32 mod_puts(char *dst, uint32 cap, uint32 at, const char *s)
{
    uint32 i = 0u;

    if(s == 0)
    {
        s = "(null)";
    }
    while((s[i] != '\0') && ((at + 1u) < cap))
    {
        dst[at] = s[i];
        at++;
        i++;
    }
    return at;
}

static uint32 mod_putu(char *dst, uint32 cap, uint32 at, unsigned long v, uint32 base)
{
    char tmp[24];
    uint32 n = 0u;

    if(v == 0ul)
    {
        tmp[n] = '0';
        n++;
    }
    while(v != 0ul)
    {
        uint32 d = (uint32)(v % (unsigned long)base);

        tmp[n] = (char)((d < 10u) ? ('0' + d) : ('a' + (d - 10u)));
        n++;
        v /= (unsigned long)base;
    }
    while((n > 0u) && ((at + 1u) < cap))
    {
        n--;
        dst[at] = tmp[n];
        at++;
    }
    return at;
}

/**
* @brief 模块用的 printf：%s %c %d %i %u %x %p %% ，其余按字面输出
* @return 写进控制台的字节数
* @note 不支持宽度/精度/浮点——模块要精确格式就自己拼。
*       这里故意不做「尽量支持」：半支持的 printf 会让人以为 %5.2f 能用。
*/
int32 arkrtos_mod_printf(const char *fmt, ...);

int32 arkrtos_mod_printf(const char *fmt, ...)
{
    char *out;
    uint32 at = 0u;
    uint32 i = 0u;
    uint32 slot = 0u;
    va_list ap;

    if(fmt == 0)
    {
        return (int32)-1;
    }

    {
        arkrtos_task_t *p_self = arkrtos_task_get_current();

        if(p_self != 0)
        {
            /* 用 TCB 指针在表里的下标做槽号：不依赖全局任务号变量，
             * 也不会因为任务号复用而两行交错。 */
            slot = ((uint32)(p_self - arkrtos_task_table)) & 3u;
        }
    }
    out = s_mod_printf_buf[slot];

    va_start(ap, fmt);
    while((fmt[i] != '\0') && ((at + 1u) < ARKRTOS_MOD_PRINTF_BUF))
    {
        if(fmt[i] != '%')
        {
            out[at] = fmt[i];
            at++;
            i++;
            continue;
        }
        i++;
        switch(fmt[i])
        {
        case 's':
            at = mod_puts(out, ARKRTOS_MOD_PRINTF_BUF, at,
                          va_arg(ap, const char *));
            break;
        case 'c':
            out[at] = (char)va_arg(ap, int);
            at++;
            break;
        case 'd':
        case 'i':
        {
            long v = va_arg(ap, long);

            if(v < 0)
            {
                if((at + 1u) < ARKRTOS_MOD_PRINTF_BUF)
                {
                    out[at] = '-';
                    at++;
                }
                at = mod_putu(out, ARKRTOS_MOD_PRINTF_BUF, at,
                              (unsigned long)(-v), 10u);
            }
            else
            {
                at = mod_putu(out, ARKRTOS_MOD_PRINTF_BUF, at,
                              (unsigned long)v, 10u);
            }
            break;
        }
        case 'u':
            at = mod_putu(out, ARKRTOS_MOD_PRINTF_BUF, at,
                          va_arg(ap, unsigned long), 10u);
            break;
        case 'x':
            at = mod_putu(out, ARKRTOS_MOD_PRINTF_BUF, at,
                          va_arg(ap, unsigned long), 16u);
            break;
        case 'p':
            at = mod_puts(out, ARKRTOS_MOD_PRINTF_BUF, at, "0x");
            at = mod_putu(out, ARKRTOS_MOD_PRINTF_BUF, at,
                          va_arg(ap, unsigned long), 16u);
            break;
        case '%':
            out[at] = '%';
            at++;
            break;
        case '\0':
            i--;    /* 末尾一个孤立的 %：不再前进，交给循环条件结束 */
            break;
        default:
            out[at] = '%';
            at++;
            if((at + 1u) < ARKRTOS_MOD_PRINTF_BUF)
            {
                out[at] = fmt[i];
                at++;
            }
            break;
        }
        if(fmt[i] != '\0')
        {
            i++;
        }
    }
    va_end(ap);

    (void)arkrtos_console_write((const uint8 *)out, at);
    return (int32)at;
}

/* ---------------------------------------------------------------- 表 */

/**
* @note 加一行的判据：**模块必须能靠它完成一件正当的事**，
*       且它不会让模块绕过内核的管理（不能改别的任务、不能改 TCB）。
*/
static const arkrtos_export_t s_exports[] = {
    { "arkrtos_mod_printf",         (unsigned long)(void *)arkrtos_mod_printf },
    { "arkrtos_console_write",      (unsigned long)(void *)arkrtos_console_write },
    { "arkrtos_kernel_get_tick",    (unsigned long)(void *)arkrtos_kernel_get_tick },
    { "arkrtos_task_delay_internal",(unsigned long)(void *)arkrtos_task_delay_internal },
    { "arkrtos_task_get_current",   (unsigned long)(void *)arkrtos_task_get_current },
    { "arkrtos_thread_exit_internal",(unsigned long)(void *)arkrtos_thread_exit_internal },
};

int32 arkrtos_export_lookup(const char *name, unsigned long *p_addr)
{
    uint32 i;

    if((name == 0) || (p_addr == 0) || (name[0] == '\0'))
    {
        return (int32)-1;
    }
    for(i = 0u; i < (uint32)(sizeof(s_exports) / sizeof(s_exports[0])); i++)
    {
        const char *a = s_exports[i].name;
        const char *b = name;
        uint32 k = 0u;

        while((a[k] != '\0') && (a[k] == b[k]))
        {
            k++;
        }
        if((a[k] == '\0') && (b[k] == '\0'))
        {
            *p_addr = s_exports[i].addr;
            return 0;
        }
    }
    return (int32)-1;
}

const arkrtos_export_t *arkrtos_export_table(uint32 *p_count)
{
    if(p_count != 0)
    {
        *p_count = (uint32)(sizeof(s_exports) / sizeof(s_exports[0]));
    }
    return s_exports;
}

#endif /* ARKRTOS_USE_MODLOAD */
