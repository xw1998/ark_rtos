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
typedef struct
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
} fdt_view_t;

/* 一个内核一份设备树，与 C 平表的单树模型一致。
 *
 * 换树是**事务性**的：init 直接在工作副本上解析，成功才留下结果，失败把
 * 上一棵树原样还回去。这样"动态加载一块坏 dtb"的后果只是一个错误码，
 * 而不是整台机器突然一个设备都没有——后者会让后续每个驱动都去走默认值，
 * 而默认值看起来像"设备就是这些"。 */
static fdt_view_t s_fdt;

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
    fdt_view_t prev = s_fdt;     /* 事务快照：失败时原样还回去 */
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
        rc = ARKRTOS_FDT_ERR_ARG;
        goto fail;
    }
    if(fdt_rd32(b + HDR_MAGIC) != FDT_MAGIC_VALUE)
    {
        rc = ARKRTOS_FDT_ERR_MAGIC;
        goto fail;
    }
    totalsize = fdt_rd32(b + HDR_TOTALSIZE);
    version = fdt_rd32(b + HDR_VERSION);
    last_comp = fdt_rd32(b + HDR_LAST_COMP);
    if((version < FDT_MIN_VERSION) || (last_comp > version))
    {
        rc = ARKRTOS_FDT_ERR_VERSION;
        goto fail;
    }
    if((totalsize < FDT_HEADER_SIZE) || (totalsize > size))
    {
        rc = ARKRTOS_FDT_ERR_RANGE;
        goto fail;
    }

    s_fdt.off_struct = fdt_rd32(b + HDR_OFF_STRUCT);
    s_fdt.size_struct = fdt_rd32(b + HDR_SIZE_STRUCT);
    s_fdt.off_strings = fdt_rd32(b + HDR_OFF_STRINGS);
    s_fdt.size_strings = fdt_rd32(b + HDR_SIZE_STRINGS);
    off_rsvmap = fdt_rd32(b + HDR_OFF_RSVMAP);

    if(((s_fdt.off_struct & 3u) != 0u) || ((s_fdt.off_strings & 3u) != 0u) ||
       ((off_rsvmap & 7u) != 0u))
    {
        rc = ARKRTOS_FDT_ERR_ALIGN;
        goto fail;
    }
    /* 先比大小再相加：off_struct + size_struct 溢出会绕成一个"合法"的小值。 */
    if((s_fdt.off_struct > totalsize) ||
       (s_fdt.size_struct > (totalsize - s_fdt.off_struct)) ||
       (s_fdt.off_strings > totalsize) ||
       (s_fdt.size_strings > (totalsize - s_fdt.off_strings)))
    {
        rc = ARKRTOS_FDT_ERR_RANGE;
        goto fail;
    }

    /* 节点数在扫描里一起数：第二次遍历会引入第二套边界判断，
     * 而两套判断总有一天会不一致。 */
    s_fdt.blob = b;
    s_fdt.size = totalsize;
    rc = fdt_scan(&s_fdt.node_count);
    if(rc != ARKRTOS_FDT_OK)
    {
        goto fail;
    }
    s_fdt.ready = 1u;
    return ARKRTOS_FDT_OK;

fail:
    s_fdt = prev;
    return rc;
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

/* ---------------------------------------- 动态装载 / 路径 / 别名 / stdout */

const void *arkrtos_fdt_blob(void)
{
    return (s_fdt.ready != 0u) ? (const void *)s_fdt.blob : 0;
}

uint32 arkrtos_fdt_size(void)
{
    return (s_fdt.ready != 0u) ? s_fdt.size : 0u;
}

int arkrtos_fdt_load(void *dst, uint32 cap, const void *src, uint32 len)
{
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    uint32 i;

    if((d == 0) || (s == 0) || (len == 0u) || (len > cap) ||
       (((uint32)(unsigned long)d & 3u) != 0u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    /* 先看源 blob 的头：totalsize 比 len 大就没必要搬。
     * 这一条独立于 init，因为它回答的是"这份文件本身是不是完整"。 */
    if(len >= FDT_HEADER_SIZE)
    {
        if(fdt_rd32(s + HDR_MAGIC) != FDT_MAGIC_VALUE)
        {
            return ARKRTOS_FDT_ERR_MAGIC;
        }
        if(fdt_rd32(s + HDR_TOTALSIZE) > len)
        {
            return ARKRTOS_FDT_ERR_RANGE;
        }
    }
    for(i = 0u; i < len; i++)
    {
        d[i] = s[i];
    }
    /* init 是事务性的：它拒绝这份副本时，上一棵树仍然可用。 */
    return arkrtos_fdt_init(d, len);
}

/* 把 '/a/b' 就地拆成组件指针（buf 里的斜杠被改成 NUL）。
 * 返回组件数（根 "/" 是 0），负值表示路径不合法。 */
static int fdt_split_path(const char *path, char *buf, uint32 cap,
                          const char **comp, uint32 maxc)
{
    uint32 i = 0u;
    uint32 n = 0u;

    if((path == 0) || (path[0] != '/'))
    {
        return -1;
    }
    while(path[i] != '\0')
    {
        if((i + 1u) >= cap)
        {
            return -1;
        }
        buf[i] = path[i];
        i++;
    }
    buf[i] = '\0';

    i = 0u;
    while(buf[i] != '\0')
    {
        if(buf[i] == '/')
        {
            i++;
            continue;
        }
        if(n >= maxc)
        {
            return -1;
        }
        comp[n] = &buf[i];
        n++;
        while((buf[i] != '\0') && (buf[i] != '/'))
        {
            i++;
        }
        if(buf[i] == '/')
        {
            buf[i] = '\0';
            i++;
        }
    }
    return (int)n;
}

/* 沿树走一遍、按组件逐层比对；只有整条路径都对上才写 *out。
 * stack 记的是"当前打开的每一层节点名"，所以同时也就有了祖先路径。 */
static int fdt_walk_path(const char *path, arkrtos_fdt_node_t *out,
                         char *pathbuf, uint32 pathcap)
{
    char        buf[ARKRTOS_FDT_PATH_MAX];
    const char *comp[ARKRTOS_FDT_MAX_DEPTH + 2u];
    const char *stack[ARKRTOS_FDT_MAX_DEPTH + 2u];
    uint32      off;
    uint32      end;
    uint32      depth = 0u;
    uint32      i;
    int         n;

    if(s_fdt.ready == 0u)
    {
        return ARKRTOS_FDT_ERR_NOT_INIT;
    }
    n = fdt_split_path(path, buf, (uint32)sizeof(buf), comp,
                       (uint32)(ARKRTOS_FDT_MAX_DEPTH + 2u));
    if(n < 0)
    {
        return ARKRTOS_FDT_ERR_PATH;
    }
    off = s_fdt.off_struct;
    end = fdt_struct_end();

    for(;;)
    {
        uint32 tok;

        if(off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = fdt_rd32(s_fdt.blob + off);
        off += 4u;

        if(tok == FDT_TOK_NOP)
        {
            continue;
        }
        if(tok == FDT_TOK_END)
        {
            if(off != end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            return ARKRTOS_FDT_ERR_NOT_FOUND;
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
            if(depth > (ARKRTOS_FDT_MAX_DEPTH + 1u))
            {
                return ARKRTOS_FDT_ERR_DEPTH;
            }
            stack[depth] = name;
            depth++;

            /* stack[0] 是根节点名（""），路径第 k 个组件对应 stack[k]。 */
            if((int)depth == (n + 1))
            {
                int hit = 1;

                for(i = 0u; (int)i < n; i++)
                {
                    if(fdt_streq(comp[i], stack[i + 1u]) == 0)
                    {
                        hit = 0;
                        break;
                    }
                }
                if(hit != 0)
                {
                    if(out != 0)
                    {
                        out->name = name;
                        out->depth = depth - 1u;
                        out->props_off = off;
                        out->n_props = fdt_count_props(off);
                    }
                    if(pathbuf != 0)
                    {
                        uint32 w = 0u;
                        uint32 k;

                        /* k=1 是根节点（名字为空），不产生路径组件；从 k=2 起才是子节点。 */
    for(k = 2u; k <= depth; k++)
                        {
                            const char *nm = stack[k - 1u];
                            uint32 c = 0u;

                            if(w >= (pathcap - 1u))
                            {
                                return ARKRTOS_FDT_ERR_ARG;
                            }
                            pathbuf[w] = '/';
                            w++;
                            while((nm[c] != '\0') && (w < (pathcap - 1u)))
                            {
                                pathbuf[w] = nm[c];
                                w++;
                                c++;
                            }
                            if(nm[c] != '\0')
                            {
                                return ARKRTOS_FDT_ERR_ARG;
                            }
                        }
                        pathbuf[w] = '\0';
                    }
                    return ARKRTOS_FDT_OK;
                }
            }
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out)
{
    return fdt_walk_path(path, out, 0, 0u);
}

/* 在 pathbuf 里拼出"当前打开的这几层"的绝对路径。 */
static int fdt_build_path(const char *const *stack, uint32 depth,
                          char *pathbuf, uint32 pathcap)
{
    uint32 w = 0u;
    uint32 k;

    if((pathbuf == 0) || (pathcap < 2u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    for(k = 2u; k <= depth; k++)
    {
        const char *nm = stack[k - 1u];
        uint32 c = 0u;

        if(w >= (pathcap - 1u))
        {
            return ARKRTOS_FDT_ERR_ARG;
        }
        pathbuf[w] = '/';
        w++;
        while(nm[c] != '\0')
        {
            if(w >= (pathcap - 1u))
            {
                return ARKRTOS_FDT_ERR_ARG;
            }
            pathbuf[w] = nm[c];
            w++;
            c++;
        }
    }
    pathbuf[w] = '\0';
    return ARKRTOS_FDT_OK;
}

/* 一次遍历同时回答两个问题：按 phandle 找（ph != 0），或按
 * (名字, 深度) 找（ph == 0）；找到就填 *out，并按需拼出绝对路径。
 * 用名字+深度而不是节点地址，是因为节点只是一个视图，没有身份。 */
static int fdt_walk_match(uint32 ph, const char *want_name, uint32 want_depth,
                          arkrtos_fdt_node_t *out, char *pathbuf,
                          uint32 pathcap)
{
    const char *stack[ARKRTOS_FDT_MAX_DEPTH + 2u];
    uint32      off;
    uint32      end;
    uint32      depth = 0u;

    if(s_fdt.ready == 0u)
    {
        return ARKRTOS_FDT_ERR_NOT_INIT;
    }
    off = s_fdt.off_struct;
    end = fdt_struct_end();

    for(;;)
    {
        uint32 tok;

        if(off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = fdt_rd32(s_fdt.blob + off);
        off += 4u;

        if(tok == FDT_TOK_NOP)
        {
            continue;
        }
        if(tok == FDT_TOK_END)
        {
            if(off != end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            return ARKRTOS_FDT_ERR_NOT_FOUND;
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
            int         hit = 0;

            if(fdt_node_name(&off, &name) != ARKRTOS_FDT_OK)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            if(depth > (ARKRTOS_FDT_MAX_DEPTH + 1u))
            {
                return ARKRTOS_FDT_ERR_DEPTH;
            }
            stack[depth] = name;
            depth++;

            if(ph != 0u)
            {
                arkrtos_fdt_node_t tmp;
                uint32 v = 0u;

                tmp.name = name;
                tmp.depth = depth - 1u;
                tmp.props_off = off;
                tmp.n_props = 0u;
                if(((arkrtos_fdt_prop_u32(&tmp, "phandle", 0u, &v) == 0) ||
                    (arkrtos_fdt_prop_u32(&tmp, "linux,phandle", 0u, &v) == 0)) &&
                   (v == ph))
                {
                    hit = 1;
                }
            }
            else if((depth == want_depth) && (fdt_streq(name, want_name) != 0))
            {
                hit = 1;
            }

            if(hit != 0)
            {
                if(out != 0)
                {
                    out->name = name;
                    out->depth = depth - 1u;
                    out->props_off = off;
                    out->n_props = fdt_count_props(off);
                }
                if(pathbuf != 0)
                {
                    return fdt_build_path(stack, depth, pathbuf, pathcap);
                }
                return ARKRTOS_FDT_OK;
            }
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

int arkrtos_fdt_node_by_phandle(uint32 ph, arkrtos_fdt_node_t *out)
{
    if(ph == 0u)
    {
        return ARKRTOS_FDT_ERR_ARG;         /* 0 不是合法 phandle */
    }
    return fdt_walk_match(ph, 0, 0u, out, 0, 0u);
}

int arkrtos_fdt_path_of_phandle(uint32 ph, char *buf, uint32 cap)
{
    if(ph == 0u)
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    if((buf == 0) || (cap < 2u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    return fdt_walk_match(ph, 0, 0u, 0, buf, cap);
}

int arkrtos_fdt_alias(const char *name, char *buf, uint32 cap)
{
    arkrtos_fdt_node_t n;
    const void *v;
    uint32 len = 0u;
    uint32 i;
    int rc;

    if((name == 0) || (buf == 0) || (cap < 2u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    rc = fdt_walk_path("/aliases", &n, 0, 0u);
    if(rc != ARKRTOS_FDT_OK)
    {
        return (rc == ARKRTOS_FDT_ERR_NOT_FOUND) ? ARKRTOS_FDT_ERR_NOT_FOUND : rc;
    }
    v = arkrtos_fdt_prop(&n, name, &len);
    if(v == 0)
    {
        return ARKRTOS_FDT_ERR_NOT_FOUND;
    }
    if((len >= 1u) && (((const uint8 *)v)[len - 1u] == 0u))
    {
        /* 字符串形式：别名里直接写路径（两种写法规范都收）。 */
        const char *s = (const char *)v;

        if(s[0] != '/')
        {
            return ARKRTOS_FDT_ERR_PATH;
        }
        i = 0u;
        while(s[i] != '\0')
        {
            if((i + 1u) >= cap)
            {
                return ARKRTOS_FDT_ERR_ARG;
            }
            buf[i] = s[i];
            i++;
        }
        buf[i] = '\0';
        return ARKRTOS_FDT_OK;
    }
    if(len == 4u)
    {
        /* 句柄形式：dtc 把 "serial0 = &uart0" 编成一个 phandle 单元。 */
        return arkrtos_fdt_path_of_phandle(fdt_rd32((const uint8 *)v), buf, cap);
    }
    return ARKRTOS_FDT_ERR_PATH;
}

const void *arkrtos_fdt_stdout_raw(uint32 *len)
{
    arkrtos_fdt_node_t n;

    if(s_fdt.ready == 0u)
    {
        return 0;
    }
    if(fdt_walk_path("/chosen", &n, 0, 0u) != ARKRTOS_FDT_OK)
    {
        return 0;
    }
    return arkrtos_fdt_prop(&n, "stdout-path", len);
}

const char *arkrtos_fdt_stdout_path(void)
{
    uint32 len = 0u;
    const uint8 *v = (const uint8 *)arkrtos_fdt_stdout_raw(&len);

    if((v == 0) || (len == 0u) || (v[len - 1u] != 0u))
    {
        return 0;
    }
    return (const char *)v;
}

/* "115200n8" 这类选项：一个大的十进制数（波特率）+ 可选的小数字（数据位）
 * + 可选的一个校验位字母。别的一律 ERR_PATH——不猜一个"差不多"的波特率。 */
static uint32 fdt_parse_dec(const char **pp, uint32 *ok)
{
    const char *p = *pp;
    uint32 v = 0u;
    uint32 n = 0u;

    while((*p >= '0') && (*p <= '9'))
    {
        if(n >= 7u)
        {
            *ok = 0u;
            return 0u;
        }
        v = (v * 10u) + (uint32)(*p - '0');
        p++;
        n++;
    }
    if(n == 0u)
    {
        *ok = 0u;
        return 0u;
    }
    *ok = 1u;
    *pp = p;
    return v;
}

static int fdt_copy_str(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (src == 0) || (cap < 2u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    while(src[i] != '\0')
    {
        if((i + 1u) >= cap)
        {
            return ARKRTOS_FDT_ERR_ARG;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return ARKRTOS_FDT_OK;
}

int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out)
{
    const uint8 *v;
    uint32       len = 0u;
    char         dev[ARKRTOS_FDT_PATH_MAX];
    uint32       i = 0u;
    int          rc;

    if(s_fdt.ready == 0u)
    {
        return ARKRTOS_FDT_ERR_NOT_INIT;
    }
    if(out == 0)
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    out->path[0] = '\0';
    out->baud = 0u;
    out->bits = 0u;
    out->stop = 0u;
    out->parity = 0;
    out->from_alias = 0u;

    v = (const uint8 *)arkrtos_fdt_stdout_raw(&len);
    if(v == 0)
    {
        return ARKRTOS_FDT_ERR_NOT_FOUND;
    }

    if((len == 4u) && (v[3] != 0u))
    {
        /* 句柄形式：/chosen/stdout-path = <&uart0>。 */
        rc = arkrtos_fdt_path_of_phandle(fdt_rd32(v), out->path,
                                         ARKRTOS_FDT_PATH_MAX);
        return rc;
    }
    if((len == 0u) || (v[len - 1u] != 0u))
    {
        return ARKRTOS_FDT_ERR_PATH;
    }

    /* 字符串形式：<设备或别名>[:<选项>]。 */
    while((((const char *)v)[i] != '\0') && (((const char *)v)[i] != ':'))
    {
        if((i + 1u) >= (uint32)sizeof(dev))
        {
            return ARKRTOS_FDT_ERR_PATH;
        }
        dev[i] = (char)v[i];
        i++;
    }
    dev[i] = '\0';
    if(i == 0u)
    {
        return ARKRTOS_FDT_ERR_PATH;
    }

    if(dev[0] == '/')
    {
        rc = arkrtos_fdt_node_by_path(dev, 0);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        rc = fdt_copy_str(out->path, ARKRTOS_FDT_PATH_MAX, dev);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
    }
    else
    {
        char aliasbuf[ARKRTOS_FDT_PATH_MAX];

        rc = arkrtos_fdt_alias(dev, aliasbuf, (uint32)sizeof(aliasbuf));
        if(rc != ARKRTOS_FDT_OK)
        {
            return (rc == ARKRTOS_FDT_ERR_NOT_FOUND) ? ARKRTOS_FDT_ERR_NOT_FOUND : rc;
        }
        rc = arkrtos_fdt_node_by_path(aliasbuf, 0);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        rc = fdt_copy_str(out->path, ARKRTOS_FDT_PATH_MAX, aliasbuf);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        out->from_alias = 1u;
    }

    if(v[i] == ':')
    {
        const char *p = (const char *)v + i + 1u;

        while(*p != '\0')
        {
            uint32 ok = 0u;
            uint32 num;

            if((*p >= '0') && (*p <= '9'))
            {
                num = fdt_parse_dec(&p, &ok);
                if(ok == 0u)
                {
                    return ARKRTOS_FDT_ERR_PATH;
                }
                if(num <= 8u)
                {
                    if(out->bits != 0u)
                    {
                        return ARKRTOS_FDT_ERR_PATH;   /* 第二个小数：不认识 */
                    }
                    out->bits = num;
                }
                else
                {
                    if(out->baud != 0u)
                    {
                        return ARKRTOS_FDT_ERR_PATH;
                    }
                    out->baud = num;
                }
            }
            else if((*p == 'n') || (*p == 'o') || (*p == 'e'))
            {
                if(out->parity != 0)
                {
                    return ARKRTOS_FDT_ERR_PATH;
                }
                out->parity = *p;
                p++;
            }
            else
            {
                return ARKRTOS_FDT_ERR_PATH;
            }
        }
    }
    return ARKRTOS_FDT_OK;
}

#endif /* ARKRTOS_USE_FDT */
