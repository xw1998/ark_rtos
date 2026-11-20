/**
* @file arkrtos_fdt_console.c
* @brief /chosen/stdout-path 的消费：树怎么说、板级是什么、两者是否一致
* @details 判据全部落在这份文件里，板级只提供"我应该是什么"两个常量
*          （compatible + 窗口基址），所以同一个判断能原样用在别的板上。
*
*          刻意不做的事：
*            - 不在运行期改控制台地址（打印自己要用它；改地址会把"谁在
*              说话"变成随配置变化的量）；
*            - 不一致时不"挑一个能用就好"，而是返回具体原因码——树与板级
*              不一致是**配置错误**，不是运行时可以兜底的情况。
*
*          开关 ARKRTOS_USE_FDT_CONSOLE 默认 0：关档时本文件不产生符号。
*
* @author xw
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_FDT_CONSOLE == 1)

#include "arkrtos_fdt_console.h"

static int console_str_eq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }
    while(*a != '\0')
    {
        if(*a != *b)
        {
            return 0;
        }
        a++;
        b++;
    }
    return (*b == '\0') ? 1 : 0;
}

static int console_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (src == 0) || (cap < 2u))
    {
        return -1;
    }
    while(src[i] != '\0')
    {
        if((i + 1u) >= cap)
        {
            return -1;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 0;
}

int arkrtos_console_from_fdt(const arkrtos_console_expect_t *expect,
                             arkrtos_console_bind_t *out)
{
    arkrtos_fdt_console_t c;
    arkrtos_fdt_node_t    n;
    const char           *compat;
    uint32                reg = 0u;
    uint32                speed = 0u;
    int32                 rc;

    if(out == 0)
    {
        return ARKRTOS_CONSOLE_BAD_PATH;
    }
    out->path[0] = '\0';
    out->compatible[0] = '\0';
    out->reg_base = 0u;
    out->baud = 0u;
    out->bits = 0u;
    out->parity = 0;
    out->enabled = 0u;
    out->from_alias = 0u;

    rc = arkrtos_fdt_stdout_resolve(&c);
    if(rc != ARKRTOS_FDT_OK)
    {
        if(rc == ARKRTOS_FDT_ERR_NOT_INIT)
        {
            return ARKRTOS_CONSOLE_NO_STDOUT;
        }
        if(rc == ARKRTOS_FDT_ERR_NOT_FOUND)
        {
            return ARKRTOS_CONSOLE_NO_STDOUT;
        }
        return ARKRTOS_CONSOLE_BAD_PATH;
    }
    out->baud = c.baud;
    out->bits = c.bits;
    out->parity = c.parity;
    out->from_alias = c.from_alias;
    (void)console_copy(out->path, ARKRTOS_FDT_PATH_MAX, c.path);

    rc = arkrtos_fdt_node_by_path(c.path, &n);
    if(rc != ARKRTOS_FDT_OK)
    {
        return ARKRTOS_CONSOLE_NO_NODE;
    }
    out->enabled = (uint8)((arkrtos_fdt_node_enabled(&n) != 0) ? 1u : 0u);
    compat = arkrtos_fdt_prop_str(&n, "compatible");
    if(compat != 0)
    {
        (void)console_copy(out->compatible, (uint32)sizeof(out->compatible),
                           compat);
    }
    (void)arkrtos_fdt_prop_u32(&n, "reg", 0u, &reg);
    out->reg_base = reg;

    if(out->enabled == 0u)
    {
        return ARKRTOS_CONSOLE_DISABLED;
    }
    /* 树自己的一致性：stdout-path 写了 115200，节点却说 current-speed 是
     * 别的，那这张树内部就矛盾——报错，不选一个"更可信"的。 */
    if((c.baud != 0u) &&
       (arkrtos_fdt_prop_u32(&n, "current-speed", 0u, &speed) == 0) &&
       (speed != 0u) && (speed != c.baud))
    {
        return ARKRTOS_CONSOLE_BAUD;
    }
    if(expect != 0)
    {
        if((expect->compatible != 0) &&
           (console_str_eq(out->compatible, expect->compatible) == 0))
        {
            return ARKRTOS_CONSOLE_COMPAT;
        }
        if((expect->reg_base != 0u) && (reg != expect->reg_base))
        {
            return ARKRTOS_CONSOLE_ADDR;
        }
    }
    return ARKRTOS_CONSOLE_OK;
}

const char *arkrtos_console_reason(int rc)
{
    switch(rc)
    {
        case ARKRTOS_CONSOLE_OK:         return "ok";
        case ARKRTOS_CONSOLE_NO_STDOUT:  return "no_stdout_path";
        case ARKRTOS_CONSOLE_BAD_PATH:   return "bad_value";
        case ARKRTOS_CONSOLE_NO_NODE:    return "node_missing";
        case ARKRTOS_CONSOLE_DISABLED:   return "node_disabled";
        case ARKRTOS_CONSOLE_COMPAT:     return "compatible_mismatch";
        case ARKRTOS_CONSOLE_ADDR:       return "reg_mismatch";
        case ARKRTOS_CONSOLE_BAUD:       return "baud_conflict";
        default:                         return "?";
    }
}

#endif /* ARKRTOS_USE_FDT_CONSOLE */
