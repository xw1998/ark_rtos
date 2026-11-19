/**
* @file arkrtos_fdt.c
* @brief 扁平设备树（DTB）只读解析器
* @details MCU 档 link 的是 tools/dts/dtc_lite.py 生成的 C 平表
*          （arkrtos_dt.h）。带 MMU 的档位不是这个形状：镜像里放一块标准
*          DTB，启动时解析——正常 Linux 形态项目都这么做。本文件就是那个
*          读侧：只读、零分配、不递归、不碰硬件。
*
*          两条硬规矩：
*            1) 任何偏移/长度/对齐在使用前都要先和 blob 自己的头核对；
*               越界一律返回错误码，绝不"夹一下继续读"——夹过的读数看起来
*               像成功，实际上读的是别人的字节；
*            2) 结构块必须严格嵌套、并且恰好以 FDT_END 收在块尾；只有
*               magic 对的 blob 不算数。
*
*          与 C 平表的关系：两边来自同一份 DTS（同一个解析器分叉出两个
*          出口），所以节点集与属性值按构造一致。门禁里用同一块板的 dtb 与
*          生成的 C 表逐节点对账，正是为了让"按构造一致"变成读数。
*
*          开关 ARKRTOS_USE_FDT 默认 0：关档时本文件不产生任何符号，
*          镜像与命令表和没有这个文件时逐字节相同。
*
* @author xw
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_FDT == 1)

#include "arkrtos_fdt.h"

/* ---------------------------------------------------------------- 常量 */

/* Devicetree Specification v0.4，与 tools/dts/dtc_lite.py 的写入侧同一套。 */
#define FDT_TOK_BEGIN_NODE      1u
#define FDT_TOK_END_NODE        2u
#define FDT_TOK_PROP            3u
#define FDT_TOK_NOP             4u
#define FDT_TOK_END             9u

#define FDT_HEADER_SIZE         40u
#define FDT_MAGIC_VALUE         0xd00dfeedu
#define FDT_MIN_VERSION         16u      /* last_comp_version of v17 blobs */

/* 头部字段偏移（大端 32 位） */
#define HDR_MAGIC               0u
#define HDR_TOTALSIZE           4u
#define HDR_OFF_STRUCT          8u
#define HDR_OFF_STRINGS         12u
#define HDR_OFF_RSVMAP          16u
#define HDR_VERSION             20u
#define HDR_LAST_COMP           24u
#define HDR_SIZE_STRINGS        32u
#define HDR_SIZE_STRUCT         36u

/* ---------------------------------------------------------------- 状态 */

/* 一个内核一份设备树，与 C 平表的单树模型一致。 */
static struct
{
    uint8        ready;
    const uint8 *blob;
    uint32       size;
    uint32       off_struct;
    uint32       size_struct;
    uint32       off_strings;
    uint32       size_strings;
    uint32       node_count;
    const char  *model;
} s_fdt;

/* ---------------------------------------------------------------- 工具 */

/* 大端 32 位读。逐字节拼而不是强制转换：LA64/ARM 上未对齐的 u32 指针
 * 会触发对齐异常（本仓的 -mstrict-align 正是为此），而 DTB 里的属性值
 * 只保证 4 字节对齐，不能假设调用方给的总线宽对齐。 */
static uint32 fdt_rd32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | (uint32)p[3];
}

static uint32 fdt_pad4(uint32 n)
{
    return (n + 3u) & ~3u;
}

static int fdt_streq(const char *a, const char *b)
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

static uint32 fdt_struct_end(void)
{
    return s_fdt.off_struct + s_fdt.size_struct;
}

/* strings 段里的第 off 字节起必须是一个以 NUL 结尾的名字；越界或没有
 * NUL 都是坏 blob，返回 0。 */
static const char *fdt_string(uint32 off)
{
    uint32 i;

    if(off >= s_fdt.size_strings)
    {
        return 0;
    }
    for(i = off; i < s_fdt.size_strings; i++)
    {
        if(s_fdt.blob[s_fdt.off_strings + i] == 0u)
        {
            return (const char *)(s_fdt.blob + s_fdt.off_strings + off);
        }
    }
    return 0;
}

/* 读一个节点名：*off 指向 BEGIN_NODE 令牌之后，返回时指向名字对齐后的下一个令牌 */
static int fdt_node_name(uint32 *off, const char **name)
{
    uint32 start = *off;
    uint32 end = fdt_struct_end();

    while((*off < end) && (s_fdt.blob[*off] != 0u))
    {
        (*off)++;
    }
    if(*off >= end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    *name = (const char *)(s_fdt.blob + start);
    (*off)++;
    *off = s_fdt.off_struct + fdt_pad4(*off - s_fdt.off_struct);
    if(*off > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    return ARKRTOS_FDT_OK;
}

/* 读一个属性头：*off 指向 PROP 令牌之后（长度/名字偏移两个字），
 * 返回时停在值的第一字节。值长度与名字偏移都要落在各自段内。 */
static int fdt_prop_head(uint32 *off, uint32 *plen, const char **pname)
{
    uint32 end = fdt_struct_end();
    uint32 len;
    uint32 nameoff;

    if((*off + 8u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    len = fdt_rd32(s_fdt.blob + *off);
    nameoff = fdt_rd32(s_fdt.blob + *off + 4u);
    *off += 8u;

    *pname = fdt_string(nameoff);
    if(*pname == 0)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(len > (end - *off))
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    *plen = len;
    return ARKRTOS_FDT_OK;
}

/* 从 props_off 起数直接挂在这个节点上的属性；遇到任何非 PROP 令牌就停。 */
static uint32 fdt_count_props(uint32 props_off)
{
    uint32 off = props_off;
    uint32 end = fdt_struct_end();
    uint32 count = 0u;

    while((off + 4u) <= end)
    {
        uint32 len;
        const char *pname;

        if(fdt_rd32(s_fdt.blob + off) != FDT_TOK_PROP)
        {
            break;
        }
        off += 4u;
        if(fdt_prop_head(&off, &len, &pname) != ARKRTOS_FDT_OK)
        {
            break;
        }
        off += fdt_pad4(len);
        if(off > end)
        {
            break;
        }
        count++;
    }
    return count;
}

/* 在一段属性表里找 "model"，返回第一个字符串（含 NUL）。 */
static const char *fdt_find_model(uint32 props_off)
{
    uint32 off = props_off;
    uint32 end = fdt_struct_end();

    while((off + 4u) <= end)
    {
        uint32 len;
        const char *pname;

        if(fdt_rd32(s_fdt.blob + off) != FDT_TOK_PROP)
        {
            break;
        }
        off += 4u;
        if(fdt_prop_head(&off, &len, &pname) != ARKRTOS_FDT_OK)
        {
            return 0;
        }
        if((fdt_streq(pname, "model") != 0) && (len > 0u))
        {
            if(s_fdt.blob[off + len - 1u] == 0u)
            {
                return (const char *)(s_fdt.blob + off);
            }
            return 0;
        }
        off += fdt_pad4(len);
        if(off > end)
        {
            break;
        }
    }
    return 0;
}

/* 数节点、取 model。整套校验都在这里做一次，之后查询不必重复。 */
static int fdt_scan(uint32 *count_out)
{
    uint32 off = s_fdt.off_struct;
    uint32 end = fdt_struct_end();
    uint32 depth = 0u;
    uint32 count = 0u;

    for(;;)
    {
        uint32 tok;

        if(off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;      /* 没见到 FDT_END 就出块了 */
        }
        tok = fdt_rd32(s_fdt.blob + off);
        off += 4u;

        if(tok == FDT_TOK_NOP)
        {
            continue;
        }
        if(tok == FDT_TOK_END)
        {
            if(depth != 0u)
            {
                return ARKRTOS_FDT_ERR_FORMAT;  /* 还有节点没闭合 */
            }
            if(off != end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            *count_out = count;
            return ARKRTOS_FDT_OK;
        }
        if(tok == FDT_TOK_END_NODE)
        {
            if(depth == 0u)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            depth--;
            continue;
        }
        if(tok == FDT_TOK_PROP)
        {
            uint32 len;
            const char *pname;

            if(fdt_prop_head(&off, &len, &pname) != ARKRTOS_FDT_OK)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            off += fdt_pad4(len);
            if(off > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == FDT_TOK_BEGIN_NODE)
        {
            const char *name;

            if(fdt_node_name(&off, &name) != ARKRTOS_FDT_OK)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            depth++;
            if(depth > (ARKRTOS_FDT_MAX_DEPTH + 1u))
            {
                return ARKRTOS_FDT_ERR_DEPTH;
            }
            if(depth == 1u)
            {
                /* 根节点的属性就在名字之后：model 只可能在这里 */
                s_fdt.model = fdt_find_model(off);
                continue;
            }
            count++;
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

/* ---------------------------------------------------------------- API */

int arkrtos_fdt_init(const void *blob, uint32 size)
{
    const uint8 *b = (const uint8 *)blob;
    uint32 totalsize;
    uint32 off_rsvmap;
    uint32 version;
    uint32 last_comp;
    int rc;

    s_fdt.ready = 0u;
    s_fdt.blob = 0;
    s_fdt.size = 0u;
    s_fdt.node_count = 0u;
    s_fdt.model = 0;

    if((b == 0) || (size < FDT_HEADER_SIZE))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    if(fdt_rd32(b + HDR_MAGIC) != FDT_MAGIC_VALUE)
    {
        return ARKRTOS_FDT_ERR_MAGIC;
    }
    totalsize = fdt_rd32(b + HDR_TOTALSIZE);
    version = fdt_rd32(b + HDR_VERSION);
    last_comp = fdt_rd32(b + HDR_LAST_COMP);
    if((version < FDT_MIN_VERSION) || (last_comp > version))
    {
        return ARKRTOS_FDT_ERR_VERSION;
    }
    if((totalsize < FDT_HEADER_SIZE) || (totalsize > size))
    {
        return ARKRTOS_FDT_ERR_RANGE;
    }

    s_fdt.off_struct = fdt_rd32(b + HDR_OFF_STRUCT);
    s_fdt.size_struct = fdt_rd32(b + HDR_SIZE_STRUCT);
    s_fdt.off_strings = fdt_rd32(b + HDR_OFF_STRINGS);
    s_fdt.size_strings = fdt_rd32(b + HDR_SIZE_STRINGS);
    off_rsvmap = fdt_rd32(b + HDR_OFF_RSVMAP);

    if(((s_fdt.off_struct & 3u) != 0u) || ((s_fdt.off_strings & 3u) != 0u) ||
       ((off_rsvmap & 7u) != 0u))
    {
        return ARKRTOS_FDT_ERR_ALIGN;
    }
    /* 先比大小再相加：off_struct + size_struct 溢出会绕成一个"合法"的小值。 */
    if((s_fdt.off_struct > totalsize) ||
       (s_fdt.size_struct > (totalsize - s_fdt.off_struct)) ||
       (s_fdt.off_strings > totalsize) ||
       (s_fdt.size_strings > (totalsize - s_fdt.off_strings)))
    {
        return ARKRTOS_FDT_ERR_RANGE;
    }

    /* 节点数在扫描里一起数：第二次遍历会引入第二套边界判断，
     * 而两套判断总有一天会不一致。 */
    s_fdt.blob = b;
    s_fdt.size = totalsize;
    rc = fdt_scan(&s_fdt.node_count);
    if(rc != ARKRTOS_FDT_OK)
    {
        s_fdt.blob = 0;
        s_fdt.size = 0u;
        s_fdt.node_count = 0u;
        s_fdt.model = 0;
        return rc;
    }
    s_fdt.ready = 1u;
    return ARKRTOS_FDT_OK;
}

int arkrtos_fdt_ready(void)
{
    return (s_fdt.ready != 0u) ? 1 : 0;
}

uint32 arkrtos_fdt_node_count(void)
{
    return s_fdt.node_count;
}

const char *arkrtos_fdt_model(void)
{
    return s_fdt.model;
}

int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it)
{
    if((s_fdt.ready == 0u) || (it == 0))
    {
        return ARKRTOS_FDT_ERR_NOT_INIT;
    }
    it->off = s_fdt.off_struct;
    it->depth = 0u;
    return ARKRTOS_FDT_OK;
}

int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out)
{
    uint32 end;

    if((s_fdt.ready == 0u) || (it == 0) || (out == 0))
    {
        return ARKRTOS_FDT_ERR_NOT_INIT;
    }
    end = fdt_struct_end();

    for(;;)
    {
        uint32 tok;

        if(it->off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = fdt_rd32(s_fdt.blob + it->off);
        it->off += 4u;

        if(tok == FDT_TOK_NOP)
        {
            continue;
        }
        if(tok == FDT_TOK_END)
        {
            if(it->depth != 0u)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            return (it->off == end) ? 0 : ARKRTOS_FDT_ERR_FORMAT;
        }
        if(tok == FDT_TOK_END_NODE)
        {
            if(it->depth == 0u)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            it->depth--;
            continue;
        }
        if(tok == FDT_TOK_PROP)
        {
            uint32 len;
            const char *pname;

            if(fdt_prop_head(&it->off, &len, &pname) != ARKRTOS_FDT_OK)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            it->off += fdt_pad4(len);
            if(it->off > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == FDT_TOK_BEGIN_NODE)
        {
            const char *name;

            if(fdt_node_name(&it->off, &name) != ARKRTOS_FDT_OK)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            it->depth++;
            if(it->depth > (ARKRTOS_FDT_MAX_DEPTH + 1u))
            {
                return ARKRTOS_FDT_ERR_DEPTH;
            }
            if(it->depth == 1u)
            {
                continue;       /* 根节点不报，与 arkrtos_dt_count() 口径一致 */
            }
            out->name = name;
            out->depth = it->depth - 1u;
            out->props_off = it->off;
            out->n_props = fdt_count_props(it->off);
            return 1;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

const void *arkrtos_fdt_prop(const arkrtos_fdt_node_t *node, const char *name,
                             uint32 *len)
{
    uint32 off;
    uint32 end;

    if((s_fdt.ready == 0u) || (node == 0) || (name == 0))
    {
        return 0;
    }
    off = node->props_off;
    end = fdt_struct_end();

    while((off + 4u) <= end)
    {
        uint32 plen;
        const char *pname;

        if(fdt_rd32(s_fdt.blob + off) != FDT_TOK_PROP)
        {
            return 0;
        }
        off += 4u;
        if(fdt_prop_head(&off, &plen, &pname) != ARKRTOS_FDT_OK)
        {
            return 0;
        }
        if(fdt_streq(pname, name) != 0)
        {
            if(len != 0)
            {
                *len = plen;
            }
            return s_fdt.blob + off;
        }
        off += fdt_pad4(plen);
        if(off > end)
        {
            return 0;
        }
    }
    return 0;
}

const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name)
{
    uint32 len = 0u;
    const uint8 *v = (const uint8 *)arkrtos_fdt_prop(node, name, &len);

    if((v == 0) || (len == 0u))
    {
        return 0;
    }
    if(v[len - 1u] != 0u)
    {
        return 0;       /* 不是以 NUL 收尾的字符串列表，按"没有"处理 */
    }
    return (const char *)v;
}

int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out)
{
    uint32 len = 0u;
    const uint8 *v = (const uint8 *)arkrtos_fdt_prop(node, name, &len);

    if((v == 0) || (out == 0))
    {
        return -1;
    }
    if(len < ((idx + 1u) * 4u))
    {
        return -1;
    }
    *out = fdt_rd32(v + (idx * 4u));
    return 0;
}

int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node)
{
    const char *status = arkrtos_fdt_prop_str(node, "status");

    if(status == 0)
    {
        return 1;
    }
    return (fdt_streq(status, "okay") != 0) ? 1 : 0;
}

#endif /* ARKRTOS_USE_FDT */
