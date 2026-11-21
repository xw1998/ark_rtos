/**
* @file arkrtos_fdt_res.c
* @brief 定时器 / 时基：树怎么说、板级是什么、两者是否一致
* @details 判据全部落在这一份文件里，板级只提供"我应该是什么"四个数
*          （timebase / 定时器 compatible / 定时器窗口 / 定时器频率）。
*
*          刻意不做的事：
*            - 不在运行期按树重编程定时器（节拍已经在跑；改频率 = 让已经
*              累计的 tick 与新的换算口径对不上，读数会变成假证据）；
*            - 不一致时不"挑一个能用就好"，而是返回具体原因码 —— 树与板级
*              矛盾是**配置错误**；
*            - 不把"树里没写"读成"树说 0"：两者用不同的原因码。
*
*          查找定时器节点按 compatible 走（这是设备树的正常做法），因此本
*          模块不假设路径，也不在 CPU 内部定时器没有节点时编一个出来 ——
*          那种情况返回 ARKRTOS_RES_NO_TIMER，并在读数里写清楚。
*
*          开关 ARKRTOS_USE_FDT_RES 默认 0：关档时本文件不产生符号。
*
* @author xw
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_FDT_RES == 1)

#include "arkrtos_fdt_res.h"

static int res_str_eq(const char *a, const char *b)
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

static void res_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (src == 0) || (cap < 2u))
    {
        return;
    }
    while((src[i] != '\0') && ((i + 1u) < cap))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- 1) /cpus/timebase-frequency ---- */
static int res_check_timebase(const arkrtos_fdt_res_expect_t *expect,
                              arkrtos_fdt_res_t *out)
{
    arkrtos_fdt_node_t cpus;
    uint32             v = 0u;

    if(arkrtos_fdt_node_by_path("/cpus", &cpus) != ARKRTOS_FDT_OK)
    {
        return ARKRTOS_RES_NO_CPUS;
    }
    if(arkrtos_fdt_prop_u32(&cpus, "timebase-frequency", 0u, &v) == 0)
    {
        out->timebase = v;
        out->has_timebase = 1u;
    }
    if(expect->expect_timebase != 0u)
    {
        if(out->has_timebase == 0u)
        {
            return ARKRTOS_RES_NO_TIMEBASE;
        }
        if(out->timebase != expect->expect_timebase)
        {
            return ARKRTOS_RES_TIMEBASE_DIFF;
        }
    }
    return ARKRTOS_RES_OK;
}

/* ---- 2) 定时器节点（按 compatible 找，不假设路径） ---- */
static int res_find_timer(const char *compat, arkrtos_fdt_node_t *cur)
{
    arkrtos_fdt_iter_t it;
    int                found = 0;
    int                rc;

    if(arkrtos_fdt_iter_init(&it) != ARKRTOS_FDT_OK)
    {
        return ARKRTOS_RES_NO_TIMER;
    }
    for(;;)
    {
        const char *cp;

        rc = arkrtos_fdt_next(&it, cur);
        if(rc <= 0)
        {
            break;
        }
        if(arkrtos_fdt_node_enabled(cur) == 0)
        {
            continue;
        }
        cp = arkrtos_fdt_prop_str(cur, "compatible");
        if(res_str_eq(cp, compat) != 0)
        {
            found = 1;
            break;
        }
    }
    return (found != 0) ? ARKRTOS_RES_OK : ARKRTOS_RES_NO_TIMER;
}

/* ---- 3) 把找到的节点抄进 out，再按 expect 核对 reg / clock-frequency ---- */
static int res_check_timer(const arkrtos_fdt_res_expect_t *expect,
                           arkrtos_fdt_res_t *out,
                           const arkrtos_fdt_node_t *cur)
{
    const char *cp;
    uint32      v = 0u;

    out->has_timer = 1u;
    res_copy(out->timer_node, (uint32)sizeof(out->timer_node),
             (cur->name != 0) ? cur->name : "");
    cp = arkrtos_fdt_prop_str(cur, "compatible");
    res_copy(out->timer_compat, (uint32)sizeof(out->timer_compat),
             (cp != 0) ? cp : "");
    if(arkrtos_fdt_prop_u32(cur, "reg", 0u, &v) == 0)
    {
        out->timer_reg = v;
        out->has_timer_reg = 1u;
    }
    if(arkrtos_fdt_prop_u32(cur, "clock-frequency", 0u, &v) == 0)
    {
        out->timer_hz = v;
        out->has_timer_hz = 1u;
    }

    if(expect->expect_timer_reg != 0u)
    {
        if(out->has_timer_reg == 0u)
        {
            return ARKRTOS_RES_TIMER_NO_REG;
        }
        if(out->timer_reg != expect->expect_timer_reg)
        {
            return ARKRTOS_RES_TIMER_REG_DIFF;
        }
    }
    if(expect->expect_timer_hz != 0u)
    {
        if(out->has_timer_hz == 0u)
        {
            return ARKRTOS_RES_TIMER_NO_HZ;
        }
        if(out->timer_hz != expect->expect_timer_hz)
        {
            return ARKRTOS_RES_TIMER_HZ_DIFF;
        }
    }
    return ARKRTOS_RES_OK;
}

int arkrtos_fdt_res_check(const arkrtos_fdt_res_expect_t *expect,
                          arkrtos_fdt_res_t *out)
{
    arkrtos_fdt_node_t cur;
    int                rc;

    if((expect == 0) || (out == 0))
    {
        return ARKRTOS_RES_NO_CPUS;
    }

    out->timebase = 0u;
    out->has_timebase = 0u;
    out->timer_node[0] = '\0';
    out->timer_compat[0] = '\0';
    out->timer_reg = 0u;
    out->timer_hz = 0u;
    out->has_timer = 0u;
    out->has_timer_reg = 0u;
    out->has_timer_hz = 0u;

    if(arkrtos_fdt_ready() == 0)
    {
        return ARKRTOS_RES_NO_CPUS;
    }

    rc = res_check_timebase(expect, out);
    if(rc != ARKRTOS_RES_OK)
    {
        return rc;
    }

    if(expect->timer_compat == 0)
    {
        return ARKRTOS_RES_OK;   /* 调用方不要求核对定时器节点 */
    }

    rc = res_find_timer(expect->timer_compat, &cur);
    if(rc != ARKRTOS_RES_OK)
    {
        return rc;
    }
    return res_check_timer(expect, out, &cur);
}

const char *arkrtos_fdt_res_reason(int rc)
{
    switch(rc)
    {
        case ARKRTOS_RES_OK:              return "ok";
        case ARKRTOS_RES_NO_CPUS:         return "no_cpus_node";
        case ARKRTOS_RES_NO_TIMEBASE:     return "timebase_absent";
        case ARKRTOS_RES_TIMEBASE_DIFF:   return "timebase_mismatch";
        case ARKRTOS_RES_NO_TIMER:        return "timer_node_absent";
        case ARKRTOS_RES_TIMER_NO_REG:    return "timer_reg_absent";
        case ARKRTOS_RES_TIMER_REG_DIFF:  return "timer_reg_mismatch";
        case ARKRTOS_RES_TIMER_NO_HZ:     return "timer_clock_absent";
        case ARKRTOS_RES_TIMER_HZ_DIFF:   return "timer_clock_mismatch";
        default:                          return "?";
    }
}

#endif /* ARKRTOS_USE_FDT_RES */
