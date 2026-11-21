/**
* @file arkrtos_fdt_overlay.c
* @brief 最小子集的 dtb overlay：读基树 + 读 fragment，重拼出一棵新树
* @details 语义与拒绝清单写在 arkrtos_fdt_overlay.h 里；这里只说实现形状。
*
*          做法是"两遍扫描 + 重拼"：
*            第一遍（measure）只算字节数，不写 dst：struct 段长度与 strings
*            段的上界（不去重，是上界）。任何拒绝条件（同名子节点、无法还
*            原的句柄 target、删除目标不存在等）都在这一遍暴露，于是 dst
*            一个字节都不会被写过；
*            第二遍（write）按第一遍算出的布局真正落笔，strings 顺带去重。
*          两遍之后核对"写出来的 struct 长度 == 第一遍算的"，不一致就报错
*          返回——宁可报错，也不给一个长度对不上的 blob。
*
*          输出布局（标准 dtb）：
*            0                 header 40 B
*            40                memory reserve map（基树的原样搬过来）
*            off_struct        struct 段（基树 + overlay 增补）
*            off_strings       strings 段（只放真正用到的属性名）
*            totalsize         = off_strings + strings 实际长度
*
*          为什么不是"原地改字节"：dtb 的 struct 段里属性必须排在子节点
*          之前，插属性会顶掉后面所有偏移，strings 段也要跟着重排。重拼
*          一遍比就地挪字节少一整类"偏移算错但不报错"的错。
*
*          开关 ARKRTOS_USE_FDT_OVERLAY 默认 0；关档时本文件不产生符号。
*
* @author xw
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_FDT_OVERLAY == 1)

#include "arkrtos_fdt_overlay.h"
#include "arkrtos_fdt.h"

/* ---------------------------------------------------------------- 常量 */

#define OVL_TOK_BEGIN_NODE      1u
#define OVL_TOK_END_NODE        2u
#define OVL_TOK_PROP            3u
#define OVL_TOK_NOP             4u
#define OVL_TOK_END             9u

#define OVL_HEADER_SIZE         40u
#define OVL_MAGIC_VALUE         0xd00dfeedu
#define OVL_VERSION             17u
#define OVL_LAST_COMP           16u

/* header 字段偏移（大端 32 位），与解析器同一套 */
#define H_MAGIC                 0u
#define H_TOTALSIZE             4u
#define H_OFF_STRUCT            8u
#define H_OFF_STRINGS           12u
#define H_OFF_RSVMAP            16u
#define H_VERSION               20u
#define H_LAST_COMP             24u
#define H_BOOT_CPUID            28u
#define H_SIZE_STRINGS          32u
#define H_SIZE_STRUCT           36u

/* 一块 fragment blob 里最多认几个 fragment@N 节点：真实板子上 1~3 个，
 * 8 是"够用且能在一个循环里比完"的数；超了明确拒绝，不猜。 */
#define OVL_MAX_FRAGS           8u

/* 删子节点的标记属性：__overlay__ 里与目标同名的子节点上放一条长度 0 的
 * 本属性。Linux 侧 dtc 对 /delete-node/ 的编码本仓库没有对拍，因此这里
 * 用自己的一套编码，并在文档里写清"不声称与 Linux dtbo 逐字节一致"。 */
#define OVL_DEL_PROP            "__delete"

/* ---------------------------------------------------------------- 类型 */

/* 一块 blob 的头字段（只在解析成功后填） */
typedef struct
{
    const uint8 *b;
    uint32       total;
    uint32       off_struct;
    uint32       size_struct;
    uint32       off_strings;
    uint32       size_strings;
    uint32       off_rsvmap;
} ovl_blob_t;

/* 一个 fragment：目标绝对路径 + __overlay__ 节点的内容区间 */
typedef struct
{
    char   path[ARKRTOS_FDT_PATH_MAX];
    uint32 props_off;          /* __overlay__ 的属性区起点 */
    uint32 end_off;            /* __overlay__ 的 END_NODE 令牌偏移 */
} ovl_frag_t;

/* 重拼上下文：measure 档 buf == 0，只累加 sp / str_used */
typedef struct
{
    const ovl_blob_t *base;
    const ovl_blob_t *frag;
    const ovl_frag_t *frags;
    uint32            nfrags;
    uint8            *buf;
    uint32            cap;
    uint32            write;   /* 1 = 真正写 dst */
    uint32            sp;      /* struct 游标（measure 档是字节数） */
    uint32            str_base;/* strings 段起点（write 档） */
    uint32            str_used;/* strings 段已用字节 */
    uint32            overflow;/* 越界写哨兵：宁可最后报错，也不静默截断 */
} ovl_ctx_t;

/* ---------------------------------------------------------------- 基础 */

static uint32 ovl_rd32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | (uint32)p[3];
}

static void ovl_wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static uint32 ovl_pad4(uint32 n)
{
    return (n + 3u) & ~3u;
}

static uint32 ovl_strlen(const char *s)
{
    uint32 n = 0u;

    if(s == 0)
    {
        return 0u;
    }
    while(s[n] != '\0')
    {
        n++;
    }
    return n;
}

static int ovl_streq(const char *a, const char *b)
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

/* 节点名与一个路径分量比较：分量不需要以 NUL 结尾 */
static int ovl_name_eq(const char *name, const char *seg, uint32 seglen)
{
    uint32 i;

    for(i = 0u; i < seglen; i++)
    {
        if(name[i] != seg[i])
        {
            return 0;
        }
    }
    return (name[seglen] == '\0') ? 1 : 0;
}

/* 标签段：__fixups__ 由 fragment 侧使用（target=<&label>），__symbols__ 由
 * 基树侧使用（label -> 路径）。位图在 ovl_blob_scan 里累加：
 *   bit0 = __fixups__，bit1 = __symbols__，bit2 = __local_fixups__。
 * __local_fixups__ 仍一律拒绝（见头文件）。 */
#define OVL_TAG_FIXUPS          1u
#define OVL_TAG_SYMBOLS         2u
#define OVL_TAG_LOCAL           4u

static int ovl_is_tag_node(const char *name)
{
    if(ovl_streq(name, "__fixups__") != 0)
    {
        return 1;
    }
    if(ovl_streq(name, "__symbols__") != 0)
    {
        return 1;
    }
    if(ovl_streq(name, "__local_fixups__") != 0)
    {
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 输出 */

static void ovl_put8(ovl_ctx_t *c, uint8 v)
{
    if(c->write != 0u)
    {
        if(c->sp < c->cap)
        {
            c->buf[c->sp] = v;
        }
        else
        {
            c->overflow = 1u;
        }
    }
    c->sp++;
}

static void ovl_put32(ovl_ctx_t *c, uint32 v)
{
    ovl_put8(c, (uint8)(v >> 24));
    ovl_put8(c, (uint8)(v >> 16));
    ovl_put8(c, (uint8)(v >> 8));
    ovl_put8(c, (uint8)v);
}

static void ovl_put_bytes(ovl_ctx_t *c, const uint8 *p, uint32 n)
{
    uint32 i;

    if(c->write != 0u)
    {
        for(i = 0u; i < n; i++)
        {
            if((c->sp + i) < c->cap)
            {
                c->buf[c->sp + i] = p[i];
            }
            else
            {
                c->overflow = 1u;
            }
        }
    }
    c->sp += n;
}

/* 对齐是按 struct 段起点算的；measure 档起点为 0、write 档起点 4 对齐，
 * 所以直接用绝对游标的低两位是对的。 */
static void ovl_align4(ovl_ctx_t *c)
{
    uint32 n = (4u - (c->sp & 3u)) & 3u;

    while(n != 0u)
    {
        ovl_put8(c, 0u);
        n--;
    }
}

/* 属性名进驻 strings 段：measure 档只累计上界（不去重），write 档线性去重。
 * 返回 strings 段内的偏移；measure 档返回值不参与任何布局计算。 */
static uint32 ovl_str(ovl_ctx_t *c, const char *name)
{
    uint32 n = ovl_strlen(name);
    uint32 i;

    if(c->write == 0u)
    {
        c->str_used += n + 1u;
        return 0u;
    }
    i = 0u;
    while(i < c->str_used)
    {
        const char *s = (const char *)(c->buf + c->str_base + i);

        if(ovl_streq(s, name) != 0)
        {
            return i;
        }
        i += ovl_strlen(s) + 1u;
    }
    if((c->str_base + c->str_used + n + 1u) > c->cap)
    {
        c->overflow = 1u;
    }
    else
    {
        uint32 k;

        for(k = 0u; k < n; k++)
        {
            c->buf[c->str_base + c->str_used + k] = (uint8)name[k];
        }
        c->buf[c->str_base + c->str_used + n] = 0u;
    }
    i = c->str_used;
    c->str_used += n + 1u;
    return i;
}

/* ---------------------------------------------------------------- 解析 */

/* 头字段校验：与 arkrtos_fdt_init 同一套判据。这里不扫结构块。 */
static int ovl_blob_parse(const void *p, uint32 len, ovl_blob_t *bl)
{
    const uint8 *b = (const uint8 *)p;
    uint32 totalsize;
    uint32 version;
    uint32 last_comp;

    bl->b = 0;
    bl->total = 0u;
    bl->off_struct = 0u;
    bl->size_struct = 0u;
    bl->off_strings = 0u;
    bl->size_strings = 0u;
    bl->off_rsvmap = 0u;

    if((b == 0) || (len < OVL_HEADER_SIZE))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    if(ovl_rd32(b + H_MAGIC) != OVL_MAGIC_VALUE)
    {
        return ARKRTOS_FDT_ERR_MAGIC;
    }
    totalsize = ovl_rd32(b + H_TOTALSIZE);
    version = ovl_rd32(b + H_VERSION);
    last_comp = ovl_rd32(b + H_LAST_COMP);
    if((version < OVL_LAST_COMP) || (last_comp > version))
    {
        return ARKRTOS_FDT_ERR_VERSION;
    }
    if((totalsize < OVL_HEADER_SIZE) || (totalsize > len))
    {
        return ARKRTOS_FDT_ERR_RANGE;
    }
    bl->off_struct = ovl_rd32(b + H_OFF_STRUCT);
    bl->size_struct = ovl_rd32(b + H_SIZE_STRUCT);
    bl->off_strings = ovl_rd32(b + H_OFF_STRINGS);
    bl->size_strings = ovl_rd32(b + H_SIZE_STRINGS);
    bl->off_rsvmap = ovl_rd32(b + H_OFF_RSVMAP);

    if(((bl->off_struct & 3u) != 0u) || ((bl->off_strings & 3u) != 0u) ||
       ((bl->off_rsvmap & 7u) != 0u))
    {
        return ARKRTOS_FDT_ERR_ALIGN;
    }
    if((bl->off_rsvmap < OVL_HEADER_SIZE) || (bl->off_rsvmap >= totalsize) ||
       (bl->off_struct > totalsize) ||
       (bl->size_struct > (totalsize - bl->off_struct)) ||
       (bl->off_strings > totalsize) ||
       (bl->size_strings > (totalsize - bl->off_strings)))
    {
        return ARKRTOS_FDT_ERR_RANGE;
    }
    bl->b = b;
    bl->total = totalsize;
    return ARKRTOS_FDT_OK;
}

static uint32 ovl_struct_end(const ovl_blob_t *bl)
{
    return bl->off_struct + bl->size_struct;
}

/* strings 段第 off 字节起必须是一个以 NUL 结尾的名字 */
static const char *ovl_strname(const ovl_blob_t *bl, uint32 off)
{
    uint32 i;

    if(off >= bl->size_strings)
    {
        return 0;
    }
    for(i = off; i < bl->size_strings; i++)
    {
        if(bl->b[bl->off_strings + i] == 0u)
        {
            return (const char *)(bl->b + bl->off_strings + off);
        }
    }
    return 0;
}

/* 读节点名：*off 指向 BEGIN_NODE 令牌之后，返回时指向名字对齐后的位置 */
static int ovl_node_name(const ovl_blob_t *bl, uint32 *off, const char **name)
{
    uint32 start = *off;
    uint32 end = ovl_struct_end(bl);

    while((*off < end) && (bl->b[*off] != 0u))
    {
        (*off)++;
    }
    if(*off >= end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    *name = (const char *)(bl->b + start);
    (*off)++;
    *off = bl->off_struct + ovl_pad4(*off - bl->off_struct);
    if(*off > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    return ARKRTOS_FDT_OK;
}

/* 读属性头：*off 指向 PROP 令牌之后，返回时停在值的第一字节 */
static int ovl_prop_head(const ovl_blob_t *bl, uint32 *off, uint32 *plen,
                         const char **pname)
{
    uint32 end = ovl_struct_end(bl);
    uint32 len;
    uint32 nameoff;

    if((*off + 8u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    len = ovl_rd32(bl->b + *off);
    nameoff = ovl_rd32(bl->b + *off + 4u);
    *off += 8u;

    *pname = ovl_strname(bl, nameoff);
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

/* 跳过一个节点的全部内容：*off 指向内容起点，返回时指向 END_NODE 之后 */
static int ovl_skip_node(const ovl_blob_t *bl, uint32 *off, uint32 depth)
{
    uint32 end = ovl_struct_end(bl);

    if(depth > ARKRTOS_FDT_MAX_DEPTH)
    {
        return ARKRTOS_FDT_ERR_DEPTH;
    }
    for(;;)
    {
        uint32 tok;

        if(*off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = ovl_rd32(bl->b + *off);
        *off += 4u;

        if(tok == OVL_TOK_NOP)
        {
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            return ARKRTOS_FDT_OK;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            const char *pn;
            int rc = ovl_prop_head(bl, off, &len, &pn);

            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            *off += ovl_pad4(len);
            if(*off > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_BEGIN_NODE)
        {
            const char *nm;
            int rc = ovl_node_name(bl, off, &nm);

            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            rc = ovl_skip_node(bl, off, depth + 1u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

/* 整块结构校验：严格嵌套、恰好以 FDT_END 收在块尾。顺带报"有没有标签段"。 */
static uint32 ovl_tag_bit(const char *name)
{
    if(ovl_streq(name, "__fixups__") != 0)
    {
        return OVL_TAG_FIXUPS;
    }
    if(ovl_streq(name, "__symbols__") != 0)
    {
        return OVL_TAG_SYMBOLS;
    }
    if(ovl_streq(name, "__local_fixups__") != 0)
    {
        return OVL_TAG_LOCAL;
    }
    return 0u;
}

static int ovl_blob_scan(const ovl_blob_t *bl, uint32 *tag_found)
{
    uint32 off = bl->off_struct;
    uint32 end = ovl_struct_end(bl);
    uint32 depth = 0u;

    if(tag_found != 0)
    {
        *tag_found = 0u;
    }
    for(;;)
    {
        uint32 tok;

        if(off >= end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = ovl_rd32(bl->b + off);
        off += 4u;

        if(tok == OVL_TOK_NOP)
        {
            continue;
        }
        if(tok == OVL_TOK_END)
        {
            if((depth != 0u) || (off != end))
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            return ARKRTOS_FDT_OK;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            if(depth == 0u)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            depth--;
            continue;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            const char *pn;
            int rc = ovl_prop_head(bl, &off, &len, &pn);

            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            off += ovl_pad4(len);
            if(off > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_BEGIN_NODE)
        {
            const char *nm;
            int rc = ovl_node_name(bl, &off, &nm);

            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            depth++;
            if(depth > (ARKRTOS_FDT_MAX_DEPTH + 1u))
            {
                return ARKRTOS_FDT_ERR_DEPTH;
            }
            if(tag_found != 0)
            {
                *tag_found |= ovl_tag_bit(nm);
            }
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

/* 在节点内容区里找一个直接子节点（名字全等）。1 = 有，0 = 没有，负 = 坏 blob */
static int ovl_find_child(const ovl_blob_t *bl, uint32 body, const char *name)
{
    uint32 pos = body;
    uint32 end = ovl_struct_end(bl);

    while((pos + 4u) <= end)
    {
        uint32 tok = ovl_rd32(bl->b + pos);

        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            const char *pn;
            int rc;

            pos += 4u;
            rc = ovl_prop_head(bl, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            pos += ovl_pad4(len);
            if(pos > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_BEGIN_NODE)
        {
            const char *cn;
            int rc;

            pos += 4u;
            rc = ovl_node_name(bl, &pos, &cn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(ovl_streq(cn, name) != 0)
            {
                return 1;
            }
            rc = ovl_skip_node(bl, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            return 0;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    return ARKRTOS_FDT_ERR_FORMAT;
}

/* 按绝对路径在基树里找节点，返回它的内容起点 */
static int ovl_find_path(const ovl_blob_t *bl, const char *path, uint32 *body)
{
    uint32 pos;
    uint32 end = ovl_struct_end(bl);
    const char *p = path;
    const char *rootname;
    int rc;

    if((path == 0) || (path[0] != '/'))
    {
        return ARKRTOS_FDT_ERR_PATH;
    }
    if((bl->off_struct + 4u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(ovl_rd32(bl->b + bl->off_struct) != OVL_TOK_BEGIN_NODE)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    pos = bl->off_struct + 4u;
    rc = ovl_node_name(bl, &pos, &rootname);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    p++;                                   /* 跳过前导 '/' */
    if(*p == '\0')
    {
        *body = pos;                       /* "/" 就是根节点 */
        return ARKRTOS_FDT_OK;
    }

    while(*p != '\0')
    {
        const char *seg = p;
        uint32 seglen = 0u;
        uint32 cp = pos;
        uint32 found = 0u;

        while((p[seglen] != '\0') && (p[seglen] != '/'))
        {
            seglen++;
        }
        if(seglen == 0u)
        {
            return ARKRTOS_FDT_ERR_PATH;    /* "//" 不认，不猜 */
        }
        while((cp + 4u) <= end)
        {
            uint32 tok = ovl_rd32(bl->b + cp);

            if(tok == OVL_TOK_NOP)
            {
                cp += 4u;
                continue;
            }
            if(tok == OVL_TOK_PROP)
            {
                uint32 len;
                const char *pn;

                cp += 4u;
                rc = ovl_prop_head(bl, &cp, &len, &pn);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
                cp += ovl_pad4(len);
                if(cp > end)
                {
                    return ARKRTOS_FDT_ERR_FORMAT;
                }
                continue;
            }
            if(tok == OVL_TOK_BEGIN_NODE)
            {
                const char *cn;

                cp += 4u;
                rc = ovl_node_name(bl, &cp, &cn);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
                if(ovl_name_eq(cn, seg, seglen) != 0)
                {
                    pos = cp;
                    found = 1u;
                    break;
                }
                rc = ovl_skip_node(bl, &cp, 0u);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
                continue;
            }
            if(tok == OVL_TOK_END_NODE)
            {
                break;
            }
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        if(found == 0u)
        {
            return ARKRTOS_FDT_ERR_NOT_FOUND;
        }
        p += seglen;
        if(*p == '/')
        {
            p++;
            if(*p == '\0')
            {
                return ARKRTOS_FDT_ERR_PATH; /* 尾随 '/' 不认 */
            }
        }
    }
    *body = pos;
    return ARKRTOS_FDT_OK;
}

/* 节点属性区里有没有叫 name 的属性（只看属性区，不递归） */
static int ovl_node_has_prop(const ovl_blob_t *bl, uint32 props_off, uint32 end,
                             const char *name)
{
    uint32 pos = props_off;

    while((pos + 4u) <= end)
    {
        uint32 len;
        const char *pn;
        int rc;

        if(ovl_rd32(bl->b + pos) != OVL_TOK_PROP)
        {
            break;
        }
        pos += 4u;
        rc = ovl_prop_head(bl, &pos, &len, &pn);
        if(rc != ARKRTOS_FDT_OK)
        {
            return 0;
        }
        if(ovl_streq(pn, name) != 0)
        {
            return 1;
        }
        pos += ovl_pad4(len);
    }
    return 0;
}

/* ------------------------------------------- 标签还原与删除语义（A4） */

/* 找一个直接子节点并返回它的内容起点（名字之后）。1=有 0=没有 负=坏 blob */
static int ovl_find_child_body(const ovl_blob_t *bl, uint32 body,
                               const char *name, uint32 *child_body)
{
    uint32 pos = body;
    uint32 end = ovl_struct_end(bl);

    while((pos + 4u) <= end)
    {
        uint32 tok = ovl_rd32(bl->b + pos);

        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            const char *pn;
            int rc;

            pos += 4u;
            rc = ovl_prop_head(bl, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            pos += ovl_pad4(len);
            if(pos > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_BEGIN_NODE)
        {
            const char *cn;
            int rc;

            pos += 4u;
            rc = ovl_node_name(bl, &pos, &cn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(ovl_streq(cn, name) != 0)
            {
                *child_body = pos;
                return 1;
            }
            rc = ovl_skip_node(bl, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            return 0;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    return ARKRTOS_FDT_ERR_FORMAT;
}

/* 根节点的内容起点（名字必须是空串） */
static int ovl_root_body(const ovl_blob_t *bl, uint32 *body)
{
    uint32 pos;
    const char *nm;
    int rc;

    if((bl->off_struct + 4u) > ovl_struct_end(bl))
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(ovl_rd32(bl->b + bl->off_struct) != OVL_TOK_BEGIN_NODE)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    pos = bl->off_struct + 4u;
    rc = ovl_node_name(bl, &pos, &nm);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(nm[0] != '\0')
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    *body = pos;
    return ARKRTOS_FDT_OK;
}

/* 基树 __symbols__：label -> 绝对路径。1=找到 0=没有 负=坏 blob */
static int ovl_symbol_path(const ovl_blob_t *bl, const char *label,
                           char *out, uint32 cap)
{
    uint32 rb;
    uint32 sb = 0u;
    uint32 pos;
    uint32 end = ovl_struct_end(bl);
    int rc;

    rc = ovl_root_body(bl, &rb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_find_child_body(bl, rb, "__symbols__", &sb);
    if(rc != 1)
    {
        return (rc == 0) ? 0 : rc;
    }
    pos = sb;
    while((pos + 4u) <= end)
    {
        uint32 len;
        const char *pn;
        int32 seen;

        if(ovl_rd32(bl->b + pos) != OVL_TOK_PROP)
        {
            break;
        }
        pos += 4u;
        rc = ovl_prop_head(bl, &pos, &len, &pn);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        seen = ovl_streq(pn, label);
        if((seen != 0) && (len >= 1u) &&
           (bl->b[pos + len - 1u] == 0u) && (len <= cap))
        {
            uint32 i;

            for(i = 0u; i < len; i++)
            {
                out[i] = (char)bl->b[pos + i];
            }
            return 1;
        }
        pos += ovl_pad4(len);
        if(pos > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
    }
    return 0;
}

/* 前缀段比较（不要求 a 在那一位是 NUL） */
static int ovl_seg_eq(const char *a, const char *b, uint32 n)
{
    uint32 i;

    for(i = 0u; i < n; i++)
    {
        if(a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

/* fragment 的 __fixups__：找 "<label> = /fragment@N:target:0" 里的 label。
 * 1=找到 0=没有 负=坏 blob。 */
static int ovl_fixup_target_label(const ovl_blob_t *bl, const char *fragname,
                                  char *out, uint32 cap)
{
    uint32 rb;
    uint32 fb = 0u;
    uint32 pos;
    uint32 end = ovl_struct_end(bl);
    uint32 flen = ovl_strlen(fragname);
    int rc;

    rc = ovl_root_body(bl, &rb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_find_child_body(bl, rb, "__fixups__", &fb);
    if(rc != 1)
    {
        return (rc == 0) ? 0 : rc;
    }
    pos = fb;
    while((pos + 4u) <= end)
    {
        uint32 len;
        const char *pn;
        uint32 v;

        if(ovl_rd32(bl->b + pos) != OVL_TOK_PROP)
        {
            break;
        }
        pos += 4u;
        rc = ovl_prop_head(bl, &pos, &len, &pn);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        for(v = 0u; v < len; )
        {
            const char *s = (const char *)(bl->b + pos + v);
            uint32 sl = 0u;
            uint32 c1 = 0u;
            uint32 c2 = 0u;
            uint32 o = 0u;
            uint32 k;

            while(((v + sl) < len) && (s[sl] != '\0'))
            {
                sl++;
            }
            if((v + sl) >= len)
            {
                break;                       /* 没有结尾 NUL：坏 blob */
            }
            while((c1 < sl) && (s[c1] != ':'))
            {
                c1++;
            }
            if(c1 < sl)
            {
                k = c1 + 1u;
                while((k < sl) && (s[k] != ':'))
                {
                    k++;
                }
                c2 = k;
            }
            /* dtc 写的值是 "/fragment@N:target:0"，路径带前导 '/'；fragname
             * 是节点名本身（"fragment@N"）。所以从第一个非 '/' 字符开始比较。 */
            o = (s[0] == '/') ? 1u : 0u;
            if((s[0] == '/') && ((c1 - o) == flen) && (c2 > c1) && (c2 < sl) &&
               (ovl_seg_eq(s + o, fragname, flen) != 0) &&
               (ovl_seg_eq(s + c1 + 1u, "target", c2 - c1 - 1u) != 0))
            {
                uint32 n = ovl_strlen(pn);

                if((n + 1u) > cap)
                {
                    return ARKRTOS_FDT_ERR_OVERLAY_TARGET;
                }
                for(k = 0u; k < n; k++)
                {
                    out[k] = pn[k];
                }
                out[n] = '\0';
                return 1;
            }
            v += sl + 1u;
        }
        pos += ovl_pad4(len);
        if(pos > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
    }
    return 0;
}

/* fragment 里 name 这条属性的长度（1=有，写进 *len；0=没有）。
 * 长度 0 的那条就是"删属性"的标记。 */
static uint32 ovl_frag_prop_len(const ovl_ctx_t *c, const uint32 *mi, uint32 nm,
                                const char *name, uint32 *len)
{
    uint32 a;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;

        while((pos + 4u) <= end)
        {
            uint32 l;
            const char *pn;
            int rc;

            if(ovl_rd32(c->frag->b + pos) != OVL_TOK_PROP)
            {
                break;
            }
            pos += 4u;
            rc = ovl_prop_head(c->frag, &pos, &l, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return 0u;
            }
            if(ovl_streq(pn, name) != 0)
            {
                *len = l;
                return 1u;
            }
            pos += ovl_pad4(l);
        }
    }
    return 0u;
}

/* name 这个子节点在 fragment 里是不是"删子节点"的标记（1=是）。
 * 只认一条形状：子节点里恰好一个长度 0 的 __delete 属性、没有别的属性、
 * 也没有子节点。任一不满足就当普通新节点处理（同名会被判重复）。 */
static uint32 ovl_is_delete_marker(const ovl_ctx_t *c, const uint32 *mi,
                                   uint32 nm, const char *name)
{
    uint32 a;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;

        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_PROP))
        {
            uint32 l;
            const char *pn;

            pos += 4u;
            if(ovl_prop_head(c->frag, &pos, &l, &pn) != ARKRTOS_FDT_OK)
            {
                return 0u;
            }
            pos += ovl_pad4(l);
        }
        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_BEGIN_NODE))
        {
            const char *cn;
            uint32 body;
            uint32 k;
            uint32 nprops = 0u;
            uint32 marker = 0u;
            uint32 clean = 1u;

            pos += 4u;
            if(ovl_node_name(c->frag, &pos, &cn) != ARKRTOS_FDT_OK)
            {
                return 0u;
            }
            body = pos;
            k = body;
            while(((k + 4u) <= end) &&
                  (ovl_rd32(c->frag->b + k) == OVL_TOK_PROP))
            {
                uint32 l;
                const char *pn;

                k += 4u;
                if(ovl_prop_head(c->frag, &k, &l, &pn) != ARKRTOS_FDT_OK)
                {
                    return 0u;
                }
                nprops++;
                if((ovl_streq(pn, OVL_DEL_PROP) != 0) && (l == 0u))
                {
                    marker = 1u;
                }
                else
                {
                    clean = 0u;
                }
                k += ovl_pad4(l);
            }
            /* 标记节点里不许再有子节点：属性区之后必须直接是 END_NODE */
            if(((k + 4u) > end) || (ovl_rd32(c->frag->b + k) != OVL_TOK_END_NODE))
            {
                clean = 0u;
            }
            if((ovl_streq(cn, name) != 0) && (nprops == 1u) &&
               (marker != 0u) && (clean != 0u))
            {
                return 1u;
            }
            if(ovl_skip_node(c->frag, &pos, 0u) != ARKRTOS_FDT_OK)
            {
                return 0u;
            }
        }
    }
    return 0u;
}

/* 删除必须落在真目标上：要删的属性/子节点在基树里不存在就报 -16 */
static int ovl_check_deletions(const ovl_ctx_t *c, const uint32 *mi, uint32 nm,
                               const ovl_blob_t *base, uint32 node_body)
{
    uint32 a;
    int rc;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;

        while((pos + 4u) <= end)
        {
            uint32 len;
            const char *pn;

            if(ovl_rd32(c->frag->b + pos) != OVL_TOK_PROP)
            {
                break;
            }
            pos += 4u;
            rc = ovl_prop_head(c->frag, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if((len == 0u) &&
               (ovl_node_has_prop(base, node_body, ovl_struct_end(base), pn) == 0))
            {
                return ARKRTOS_FDT_ERR_OVERLAY_DEL;
            }
            pos += ovl_pad4(len);
        }
        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_BEGIN_NODE))
        {
            const char *cn;
            int found;

            pos += 4u;
            rc = ovl_node_name(c->frag, &pos, &cn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(ovl_is_delete_marker(c, mi + a, 1u, cn) != 0u)
            {
                found = ovl_find_child(base, node_body, cn);
                if(found != 1)
                {
                    return (found < 0) ? found : ARKRTOS_FDT_ERR_OVERLAY_DEL;
                }
            }
            rc = ovl_skip_node(c->frag, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
        }
    }
    return ARKRTOS_FDT_OK;
}

/* ------------------------------------------------------------ fragment */

/* 读一个 fragment 节点的 target-path / target=<&label> 与 __overlay__ 区间 */
/* fragment 自己的属性：target = <&label>（经 __fixups__ + __symbols__ 换成路径）
 * 或 target-path = "/abs/path"。返回时 *p_pos 停在属性区之后，
 * *p_target 指向 target_buf 或 fb 里的字符串。 */
static int ovl_frag_target(const ovl_blob_t *fb, const ovl_blob_t *bb,
                           uint32 body, const char *fragname,
                           char *target_buf, uint32 buf_size,
                           const char **p_target, uint32 *p_pos)
{
    uint32 pos = body;
    uint32 end = ovl_struct_end(fb);
    const char *target_path = 0;
    int rc;

    while((pos + 4u) <= end)
    {
        uint32 tok = ovl_rd32(fb->b + pos);
        uint32 len;
        const char *pn;

        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok != OVL_TOK_PROP)
        {
            break;
        }
        pos += 4u;
        rc = ovl_prop_head(fb, &pos, &len, &pn);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        if(ovl_streq(pn, "target") != 0)
        {
            /* target = <&label>：dtc 写的是 0xffffffff 占位值，真正的标签在
             * fragment 的 __fixups__ 里；再用基树 __symbols__ 换成路径。
             * 占位值不是 0xffffffff 就说明这是一个已经解析过的 phandle ——
             * 本模块不做 phandle 反查，拒绝而不是猜它指向哪个节点。 */
            char label[64];
            int hit;

            if((len != 4u) || (ovl_rd32(fb->b + pos) != 0xffffffffu))
            {
                return ARKRTOS_FDT_ERR_OVERLAY_TARGET;
            }
            hit = ovl_fixup_target_label(fb, fragname, label, sizeof(label));
            if(hit != 1)
            {
                return (hit < 0) ? hit : ARKRTOS_FDT_ERR_OVERLAY_TARGET;
            }
            hit = ovl_symbol_path(bb, label, target_buf, buf_size);
            if(hit != 1)
            {
                return (hit < 0) ? hit : ARKRTOS_FDT_ERR_OVERLAY_TARGET;
            }
            target_path = target_buf;
            pos += ovl_pad4(len);
            continue;
        }
        if(ovl_streq(pn, "target-path") != 0)
        {
            if((len == 0u) || (fb->b[pos + len - 1u] != 0u))
            {
                return ARKRTOS_FDT_ERR_OVERLAY;
            }
            target_path = (const char *)(fb->b + pos);
        }
        pos += ovl_pad4(len);
        if(pos > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
    }
    if(target_path == 0)
    {
        return ARKRTOS_FDT_ERR_OVERLAY;      /* 既没 target 也没 target-path */
    }
    if(target_path[0] != '/')
    {
        return ARKRTOS_FDT_ERR_OVERLAY_TARGET;
    }
    *p_target = target_path;
    *p_pos = pos;
    return ARKRTOS_FDT_OK;
}

/* 子节点：必须恰好有一个 __overlay__；别的一律拒绝（不猜它的意思）。
 * 返回 __overlay__ 的属性区起点与它 END_NODE 令牌的偏移。 */
static int ovl_frag_overlay(const ovl_blob_t *fb, uint32 pos,
                            uint32 *p_body, uint32 *p_end)
{
    uint32 end = ovl_struct_end(fb);
    uint32 ovl_body = 0u;
    uint32 ovl_end = 0u;
    uint32 have_ovl = 0u;
    int rc;

    while((pos + 4u) <= end)
    {
        uint32 tok = ovl_rd32(fb->b + pos);
        const char *cn;

        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            break;
        }
        if(tok != OVL_TOK_BEGIN_NODE)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        pos += 4u;
        rc = ovl_node_name(fb, &pos, &cn);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
        if(ovl_is_tag_node(cn) != 0)
        {
            return ARKRTOS_FDT_ERR_OVERLAY_TAG;
        }
        if(ovl_streq(cn, "__overlay__") != 0)
        {
            if(have_ovl != 0u)
            {
                return ARKRTOS_FDT_ERR_OVERLAY;
            }
            ovl_body = pos;
            rc = ovl_skip_node(fb, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            ovl_end = pos - 4u;              /* END_NODE 令牌的偏移 */
            have_ovl = 1u;
            continue;
        }
        return ARKRTOS_FDT_ERR_OVERLAY;
    }
    if(have_ovl == 0u)
    {
        return ARKRTOS_FDT_ERR_OVERLAY;
    }
    *p_body = ovl_body;
    *p_end = ovl_end;
    return ARKRTOS_FDT_OK;
}

/* 目标节点必须真的存在（写在不存在的地方不是"以后再补"，是错），
 * 然后把路径与区间抄进 out[]。 */
static int ovl_frag_store(const ovl_blob_t *bb, const char *target_path,
                          uint32 ovl_body, uint32 ovl_end,
                          ovl_frag_t *out, uint32 max, uint32 *n_io)
{
    uint32 tb;
    uint32 i;
    int rc;

    rc = ovl_find_path(bb, target_path, &tb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return ARKRTOS_FDT_ERR_OVERLAY_TARGET;
    }

    if(*n_io >= max)
    {
        return ARKRTOS_FDT_ERR_OVERLAY;
    }
    i = 0u;
    while(target_path[i] != '\0')
    {
        if(i >= (ARKRTOS_FDT_PATH_MAX - 1u))
        {
            return ARKRTOS_FDT_ERR_OVERLAY_TARGET;
        }
        out[*n_io].path[i] = target_path[i];
        i++;
    }
    out[*n_io].path[i] = '\0';
    out[*n_io].props_off = ovl_body;
    out[*n_io].end_off = ovl_end;
    (*n_io)++;
    return ARKRTOS_FDT_OK;
}

static int ovl_collect_one(const ovl_blob_t *fb, const ovl_blob_t *bb,
                           uint32 body, const char *fragname,
                           ovl_frag_t *out, uint32 max, uint32 *n_io)
{
    char target_buf[ARKRTOS_FDT_PATH_MAX];
    const char *target_path = 0;
    uint32 ovl_body = 0u;
    uint32 ovl_end = 0u;
    uint32 pos = body;
    int rc;

    rc = ovl_frag_target(fb, bb, body, fragname,
                         target_buf, sizeof(target_buf), &target_path, &pos);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_frag_overlay(fb, pos, &ovl_body, &ovl_end);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    return ovl_frag_store(bb, target_path, ovl_body, ovl_end, out, max, n_io);
}

/* 扫 fragment blob 的根节点，逐个 fragment 收集 */
static int ovl_collect_frags(const ovl_blob_t *fb, const ovl_blob_t *bb,
                             ovl_frag_t *out, uint32 max, uint32 *n_out)
{
    uint32 pos;
    uint32 end = ovl_struct_end(fb);
    uint32 n = 0u;
    const char *rootname;
    int rc;

    if((fb->off_struct + 4u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(ovl_rd32(fb->b + fb->off_struct) != OVL_TOK_BEGIN_NODE)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    pos = fb->off_struct + 4u;
    rc = ovl_node_name(fb, &pos, &rootname);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(rootname[0] != '\0')
    {
        return ARKRTOS_FDT_ERR_FORMAT;       /* 根节点必须没有名字 */
    }
    for(;;)
    {
        uint32 tok;

        if((pos + 4u) > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = ovl_rd32(fb->b + pos);
        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            const char *pn;

            pos += 4u;
            rc = ovl_prop_head(fb, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            pos += ovl_pad4(len);
            if(pos > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            *n_out = n;
            return ARKRTOS_FDT_OK;
        }
        if(tok != OVL_TOK_BEGIN_NODE)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        pos += 4u;
        {
            const char *fname;
            uint32 body;

            rc = ovl_node_name(fb, &pos, &fname);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(ovl_streq(fname, "__fixups__") != 0)
            {
                /* fragment 侧的标签表：collect_one 会按需查它，这里整棵跳过 */
                rc = ovl_skip_node(fb, &pos, 0u);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
                continue;
            }
            if(ovl_is_tag_node(fname) != 0)
            {
                return ARKRTOS_FDT_ERR_OVERLAY_TAG;
            }
            body = pos;
            rc = ovl_collect_one(fb, bb, body, fname, out, max, &n);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            /* 必须把这个 fragment 节点整棵跳过：collect_one 的游标是它自己的
             * 局部变量，这里不动 pos 的话，下一轮会把 __overlay__ 当成根的
             * 另一个子节点再 collect 一次（它没有 target-path，于是整块被
             * 报成"形状不认识"）。这个错不报错、只报一个笼统的 -11。 */
            rc = ovl_skip_node(fb, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
        }
    }
}

/* ------------------------------------------------------------ 重拼 */

/* 一个 fragment 的子节点里，与 name 同名的有几个（跨全部命中的 fragment） */
static uint32 ovl_child_count_in_frags(const ovl_ctx_t *c, const uint32 *mi,
                                       uint32 nm, const char *name)
{
    uint32 a;
    uint32 cnt = 0u;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;

        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_PROP))
        {
            uint32 len;
            const char *pn;

            pos += 4u;
            if(ovl_prop_head(c->frag, &pos, &len, &pn) != ARKRTOS_FDT_OK)
            {
                return cnt;
            }
            pos += ovl_pad4(len);
        }
        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_BEGIN_NODE))
        {
            const char *cn;

            pos += 4u;
            if(ovl_node_name(c->frag, &pos, &cn) != ARKRTOS_FDT_OK)
            {
                return cnt;
            }
            if((ovl_streq(cn, name) != 0) &&
               (ovl_is_delete_marker(c, mi, nm, name) == 0u))
            {
                cnt++;
            }
            if(ovl_skip_node(c->frag, &pos, 0u) != ARKRTOS_FDT_OK)
            {
                return cnt;
            }
        }
    }
    return cnt;
}

/* 原样拷一棵子树（fragment 里的新节点）：令牌、属性、值逐字节搬 */
static int ovl_copy_node(ovl_ctx_t *c, const ovl_blob_t *bl, uint32 *off,
                         uint32 depth)
{
    const char *name;
    uint32 pos;
    uint32 end = ovl_struct_end(bl);
    uint32 n;
    int rc;

    if(depth > ARKRTOS_FDT_MAX_DEPTH)
    {
        return ARKRTOS_FDT_ERR_DEPTH;
    }
    rc = ovl_node_name(bl, off, &name);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    ovl_put32(c, OVL_TOK_BEGIN_NODE);
    n = ovl_strlen(name);
    ovl_put_bytes(c, (const uint8 *)name, n + 1u);
    ovl_align4(c);

    pos = *off;
    for(;;)
    {
        uint32 tok;

        if((pos + 4u) > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = ovl_rd32(bl->b + pos);
        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_END_NODE)
        {
            pos += 4u;
            *off = pos;
            ovl_put32(c, OVL_TOK_END_NODE);
            return ARKRTOS_FDT_OK;
        }
        if(tok == OVL_TOK_PROP)
        {
            uint32 len;
            uint32 nameoff;
            const char *pn;

            pos += 4u;
            rc = ovl_prop_head(bl, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            ovl_put32(c, OVL_TOK_PROP);
            ovl_put32(c, len);
            nameoff = ovl_str(c, pn);
            ovl_put32(c, nameoff);
            ovl_put_bytes(c, bl->b + pos, len);
            ovl_align4(c);
            pos += ovl_pad4(len);
            if(pos > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
            continue;
        }
        if(tok == OVL_TOK_BEGIN_NODE)
        {
            pos += 4u;
            rc = ovl_copy_node(c, bl, &pos, depth + 1u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

/* 把命中的 fragment 的 __overlay__ 属性写进当前节点（后面的 fragment 覆盖前面的） */
static int ovl_emit_frag_props(ovl_ctx_t *c, const uint32 *mi, uint32 nm)
{
    uint32 a;
    uint32 b;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;

        while((pos + 4u) <= end)
        {
            uint32 tok = ovl_rd32(c->frag->b + pos);
            uint32 len;
            uint32 nameoff;
            const char *pn;
            uint32 later = 0u;
            int rc;

            if(tok == OVL_TOK_NOP)
            {
                pos += 4u;
                continue;
            }
            if(tok != OVL_TOK_PROP)
            {
                break;
            }
            pos += 4u;
            rc = ovl_prop_head(c->frag, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(len == 0u)
            {
                /* 长度 0 = 删属性：基树那条已经在重拼时被跳过，这里也不写 */
                pos += ovl_pad4(len);
                continue;
            }
            for(b = a + 1u; b < nm; b++)
            {
                const ovl_frag_t *g = &c->frags[mi[b]];

                if(ovl_node_has_prop(c->frag, g->props_off, g->end_off, pn) != 0)
                {
                    later = 1u;
                    break;
                }
            }
            if(later == 0u)
            {
                ovl_put32(c, OVL_TOK_PROP);
                ovl_put32(c, len);
                nameoff = ovl_str(c, pn);
                ovl_put32(c, nameoff);
                ovl_put_bytes(c, c->frag->b + pos, len);
                ovl_align4(c);
            }
            pos += ovl_pad4(len);
            if(pos > end)
            {
                return ARKRTOS_FDT_ERR_FORMAT;
            }
        }
    }
    return ARKRTOS_FDT_OK;
}

/* 把命中的 fragment 的新子节点追到当前节点末尾（同名一律拒绝） */
static int ovl_emit_frag_children(ovl_ctx_t *c, const uint32 *mi, uint32 nm,
                                  const ovl_blob_t *base, uint32 node_body)
{
    uint32 a;

    for(a = 0u; a < nm; a++)
    {
        const ovl_frag_t *f = &c->frags[mi[a]];
        uint32 pos = f->props_off;
        uint32 end = f->end_off;
        int rc;

        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_PROP))
        {
            uint32 len;
            const char *pn;

            pos += 4u;
            rc = ovl_prop_head(c->frag, &pos, &len, &pn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            pos += ovl_pad4(len);
        }
        while(((pos + 4u) <= end) &&
              (ovl_rd32(c->frag->b + pos) == OVL_TOK_BEGIN_NODE))
        {
            const char *cn;
            uint32 name_off;

            pos += 4u;
            name_off = pos;                    /* 名字起点（BEGIN_NODE 之后） */
            rc = ovl_node_name(c->frag, &pos, &cn);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            if(ovl_is_delete_marker(c, mi, nm, cn) != 0u)
            {
                /* 删子节点标记：基树里那个子节点已在重拼时被跳过 */
                rc = ovl_skip_node(c->frag, &pos, 0u);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
                continue;
            }
            if(ovl_find_child(base, node_body, cn) > 0)
            {
                return ARKRTOS_FDT_ERR_OVERLAY_DUP;
            }
            if(ovl_child_count_in_frags(c, mi, nm, cn) > 1u)
            {
                return ARKRTOS_FDT_ERR_OVERLAY_DUP;
            }
            /* ovl_copy_node 自己也要读一次名字，游标必须退回名字起点：
             * 名字只读一次就往下走，会让它把属性区当成名字来读（-6）。 */
            pos = name_off;
            rc = ovl_copy_node(c, c->frag, &pos, 0u);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
        }
    }
    return ARKRTOS_FDT_OK;
}

/* 下面四个 helper 里 ovl_emit_base_child 会递归回调 ovl_emit_node，
 * 而后者定义在它们之后，所以先声明。 */
static int ovl_emit_node(ovl_ctx_t *c, const ovl_blob_t *bl, uint32 *off,
                         char *path, uint32 path_len, const char *name,
                         uint32 depth);

/* 把 name 接到父路径后面（就地写，父路径字节不动）：根的一级子节点写成
 * "/name"，其余写成 "父/name"。越界只报错不截断——截断出来的路径会和
 * 别的节点撞名，而撞名的后果是"删错了人"。*p_len 交出新的路径长度。 */
static int ovl_path_push(char *path, uint32 path_len, const char *name,
                         uint32 *p_len)
{
    uint32 n = ovl_strlen(name);
    uint32 k;

    if((path_len == 1u) && (path[0] == '/'))
    {
        if((1u + n + 1u) > ARKRTOS_FDT_PATH_MAX)  /* "/" + name + NUL */
        {
            return ARKRTOS_FDT_ERR_PATH;
        }
        for(k = 0u; k < n; k++)
        {
            path[1u + k] = name[k];
        }
        path[1u + n] = '\0';
        *p_len = 1u + n;
        return ARKRTOS_FDT_OK;
    }
    if((path_len + n + 2u) > ARKRTOS_FDT_PATH_MAX)
    {
        return ARKRTOS_FDT_ERR_PATH;
    }
    path[path_len] = '/';
    for(k = 0u; k < n; k++)
    {
        path[path_len + 1u + k] = name[k];
    }
    path[path_len + 1u + n] = '\0';
    *p_len = path_len + 1u + n;
    return ARKRTOS_FDT_OK;
}

/* 一条基树属性：被 fragment 覆盖过（含"删属性"那条长度 0）就不输出。
 * *pos 进来时指在 TOK_PROP 之后的负载起点，出去时指在该属性末尾之后。 */
static int ovl_emit_base_prop(ovl_ctx_t *c, const ovl_blob_t *bl,
                              const uint32 *mi, uint32 nm, uint32 end,
                              uint32 *pos)
{
    uint32 len;
    uint32 nameoff;
    const char *pn;
    uint32 overridden = 0u;
    uint32 flen = 0u;
    int rc;

    rc = ovl_prop_head(bl, pos, &len, &pn);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(ovl_frag_prop_len(c, mi, nm, pn, &flen) != 0u)
    {
        /* 长度 0 的那条是"删属性"：基树这条不输出，fragment 侧也不会
         * 把它写出来（ovl_emit_frag_props 跳过长度 0）。 */
        overridden = (flen == 0u) ? 2u : 1u;
    }
    if(overridden == 0u)
    {
        ovl_put32(c, OVL_TOK_PROP);
        ovl_put32(c, len);
        nameoff = ovl_str(c, pn);
        ovl_put32(c, nameoff);
        ovl_put_bytes(c, bl->b + *pos, len);
        ovl_align4(c);
    }
    *pos += ovl_pad4(len);
    if(*pos > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    return ARKRTOS_FDT_OK;
}

/* 一条基树子节点：命中"删子节点"标记就整段跳过，否则递归重拼。
 * 两种情形的调用方都是"继续读下一条 token"，所以成功时统一返回 OK。 */
static int ovl_emit_base_child(ovl_ctx_t *c, const ovl_blob_t *bl, uint32 *pos,
                               char *path, uint32 path_len, const uint32 *mi,
                               uint32 nm, uint32 depth)
{
    const char *cn;
    int rc;

    rc = ovl_node_name(bl, pos, &cn);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if((nm != 0u) && (ovl_is_delete_marker(c, mi, nm, cn) != 0u))
    {
        return ovl_skip_node(bl, pos, 0u);
    }
    return ovl_emit_node(c, bl, pos, path, path_len, cn, depth + 1u);
}

/* 收尾：先把 fragment 自己的子节点补上（它们必须排在基树子节点之后、
 * END_NODE 之前），再写 END_NODE 并把游标交回调用方。 */
static int ovl_emit_node_end(ovl_ctx_t *c, const ovl_blob_t *bl,
                             const uint32 *mi, uint32 nm, uint32 node_body,
                             uint32 *pos, uint32 *off)
{
    int rc;

    if(nm != 0u)
    {
        rc = ovl_emit_frag_children(c, mi, nm, bl, node_body);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
    }
    *pos += 4u;
    *off = *pos;
    ovl_put32(c, OVL_TOK_END_NODE);
    return ARKRTOS_FDT_OK;
}

/* 重拼一个节点：*off 指向内容起点，name 是已读出的节点名，
 * path 是父路径（就地拼接，避免每层一份栈上路径缓冲） */
static int ovl_emit_node(ovl_ctx_t *c, const ovl_blob_t *bl, uint32 *off,
                         char *path, uint32 path_len, const char *name,
                         uint32 depth)
{
    uint32 node_body = *off;
    uint32 mi[OVL_MAX_FRAGS];
    uint32 nm = 0u;
    uint32 props_done = 0u;
    uint32 pos;
    uint32 end = ovl_struct_end(bl);
    uint32 k;
    int rc;

    if(depth > ARKRTOS_FDT_MAX_DEPTH)
    {
        return ARKRTOS_FDT_ERR_DEPTH;
    }

    /* 路径就地拼接：只写在 path_len 之后，父路径字节不动 */
    rc = ovl_path_push(path, path_len, name, &path_len);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }

    /* 找命中的 fragment */
    for(k = 0u; k < c->nfrags; k++)
    {
        if(ovl_streq(c->frags[k].path, path) != 0)
        {
            if(nm >= OVL_MAX_FRAGS)
            {
                return ARKRTOS_FDT_ERR_OVERLAY;   /* 不可能：nfrags <= OVL_MAX_FRAGS */
            }
            mi[nm] = k;
            nm++;
        }
    }

    ovl_put32(c, OVL_TOK_BEGIN_NODE);
    ovl_put_bytes(c, (const uint8 *)name, ovl_strlen(name) + 1u);
    ovl_align4(c);

    if(nm != 0u)
    {
        /* 删除必须落在真目标上：先验，后写——写了一半再报错会留下半个 blob */
        rc = ovl_check_deletions(c, mi, nm, bl, node_body);
        if(rc != ARKRTOS_FDT_OK)
        {
            return rc;
        }
    }

    pos = node_body;
    for(;;)
    {
        uint32 tok;

        if((pos + 4u) > end)
        {
            return ARKRTOS_FDT_ERR_FORMAT;
        }
        tok = ovl_rd32(bl->b + pos);

        if(tok == OVL_TOK_NOP)
        {
            pos += 4u;
            continue;
        }
        if(tok == OVL_TOK_PROP)
        {
            pos += 4u;
            rc = ovl_emit_base_prop(c, bl, mi, nm, end, &pos);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }

        /* 属性区结束：fragment 的属性必须先落在子节点之前 */
        if(props_done == 0u)
        {
            props_done = 1u;
            if(nm != 0u)
            {
                rc = ovl_emit_frag_props(c, mi, nm);
                if(rc != ARKRTOS_FDT_OK)
                {
                    return rc;
                }
            }
        }

        if(tok == OVL_TOK_BEGIN_NODE)
        {
            pos += 4u;
            rc = ovl_emit_base_child(c, bl, &pos, path, path_len, mi, nm,
                                     depth);
            if(rc != ARKRTOS_FDT_OK)
            {
                return rc;
            }
            continue;
        }

        if(tok == OVL_TOK_END_NODE)
        {
            return ovl_emit_node_end(c, bl, mi, nm, node_body, &pos, off);
        }
        return ARKRTOS_FDT_ERR_FORMAT;
    }
}

/* 从根开始重拼，最后补 FDT_END */
static int ovl_emit_tree(ovl_ctx_t *c)
{
    char path[ARKRTOS_FDT_PATH_MAX];
    uint32 pos;
    uint32 end = ovl_struct_end(c->base);
    const char *rootname;
    int rc;

    if((c->base->off_struct + 4u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(ovl_rd32(c->base->b + c->base->off_struct) != OVL_TOK_BEGIN_NODE)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    pos = c->base->off_struct + 4u;
    rc = ovl_node_name(c->base, &pos, &rootname);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(rootname[0] != '\0')
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    path[0] = '/';
    path[1] = '\0';
    rc = ovl_emit_node(c, c->base, &pos, path, 1u, rootname, 0u);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if((pos + 4u) > end)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    if(ovl_rd32(c->base->b + pos) != OVL_TOK_END)
    {
        return ARKRTOS_FDT_ERR_FORMAT;
    }
    ovl_put32(c, OVL_TOK_END);
    return ARKRTOS_FDT_OK;
}

static int ovl_prepare(const void *base, uint32 base_len,
                       const void *frag, uint32 frag_len,
                       ovl_blob_t *bb, ovl_blob_t *fb)
{
    uint32 tag = 0u;
    int rc;

    rc = ovl_blob_parse(base, base_len, bb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_blob_scan(bb, 0);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_blob_parse(frag, frag_len, fb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    rc = ovl_blob_scan(fb, &tag);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    /* fragment 侧只允许 __fixups__（target=<&label>）；__symbols__ 与
     * __local_fixups__ 都不接受。 */
    if((tag & (OVL_TAG_SYMBOLS | OVL_TAG_LOCAL)) != 0u)
    {
        return ARKRTOS_FDT_ERR_OVERLAY_TAG;
    }
    return ARKRTOS_FDT_OK;
}

/* 基树的 memory reserve map 原样搬走：那块描述的是"给固件留的洞"，
 * overlay 无权修改。长度按 16 字节/项数到全零结束项为止。 */
static int ovl_rsvmap_len(const ovl_blob_t *bb, uint32 *p_len)
{
    uint32 p = bb->off_rsvmap;

    for(;;)
    {
        if((p + 16u) > bb->total)
        {
            return ARKRTOS_FDT_ERR_RANGE;
        }
        if((ovl_rd32(bb->b + p) == 0u) && (ovl_rd32(bb->b + p + 4u) == 0u) &&
           (ovl_rd32(bb->b + p + 8u) == 0u) && (ovl_rd32(bb->b + p + 12u) == 0u))
        {
            *p_len = (p - bb->off_rsvmap) + 16u;
            break;
        }
        p += 16u;
    }
    return ARKRTOS_FDT_OK;
}

static void ovl_write_head(uint8 *d, const ovl_blob_t *bb,
                           uint32 off_struct, uint32 off_strings,
                           uint32 struct_bytes, uint32 rsvmap_len)
{
    uint32 i;

    for(i = 0u; i < off_struct; i++)
    {
        d[i] = 0u;
    }
    ovl_wr32(d + H_MAGIC, OVL_MAGIC_VALUE);
    ovl_wr32(d + H_TOTALSIZE, 0u);           /* 尾部回填 */
    ovl_wr32(d + H_OFF_STRUCT, off_struct);
    ovl_wr32(d + H_OFF_STRINGS, off_strings);
    ovl_wr32(d + H_OFF_RSVMAP, OVL_HEADER_SIZE);
    ovl_wr32(d + H_VERSION, OVL_VERSION);
    ovl_wr32(d + H_LAST_COMP, OVL_LAST_COMP);
    ovl_wr32(d + H_BOOT_CPUID, 0u);
    ovl_wr32(d + H_SIZE_STRINGS, 0u);
    ovl_wr32(d + H_SIZE_STRUCT, struct_bytes);
    for(i = 0u; i < rsvmap_len; i++)
    {
        d[OVL_HEADER_SIZE + i] = bb->b[bb->off_rsvmap + i];
    }
}

/* ---- 第一遍：只算长度，不写 dst ---- */
static int ovl_measure(ovl_ctx_t *c, uint32 *p_struct_bytes, uint32 *p_str_upper)
{
    int rc;

    c->buf = 0;
    c->write = 0u;
    c->sp = 0u;
    c->str_base = 0u;
    c->str_used = 0u;
    c->overflow = 0u;
    rc = ovl_emit_tree(c);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    *p_struct_bytes = c->sp;
    *p_str_upper = c->str_used;              /* strings 上界（未去重） */
    return ARKRTOS_FDT_OK;
}

/* ---- 第二遍：真正落笔，并与第一遍对账 ---- */
static int ovl_write(ovl_ctx_t *c, uint8 *d,
                     uint32 off_struct, uint32 off_strings,
                     uint32 struct_bytes, uint32 str_upper)
{
    int rc;

    c->buf = d;
    c->write = 1u;
    c->sp = off_struct;
    c->str_base = off_strings;
    c->str_used = 0u;
    c->overflow = 0u;
    rc = ovl_emit_tree(c);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(c->overflow != 0u)
    {
        return ARKRTOS_FDT_ERR_OVERLAY_SIZE;
    }
    if((c->sp != (off_struct + struct_bytes)) || (c->str_used > str_upper))
    {
        /* 两遍不一致：说明遍历逻辑里有依赖输出状态的分支。
         * 与其交一块长度对不上的 blob，不如报错。 */
        return ARKRTOS_FDT_ERR_OVERLAY;
    }
    return ARKRTOS_FDT_OK;
}

/* ---------------------------------------------------------------- API */

int arkrtos_fdt_overlay(void *dst, uint32 cap,
                        const void *base, uint32 base_len,
                        const void *frag, uint32 frag_len,
                        uint32 *out_len)
{
    ovl_blob_t bb;
    ovl_blob_t fb;
    ovl_frag_t frags[OVL_MAX_FRAGS];
    ovl_ctx_t c;
    uint32 nf = 0u;
    uint32 rsvmap_len = 0u;
    uint32 off_struct;
    uint32 off_strings;
    uint32 struct_bytes;
    uint32 str_upper;
    uint32 totalsize;
    uint8 *d = (uint8 *)dst;
    int rc;

    if(out_len != 0)
    {
        *out_len = 0u;
    }
    if((dst == 0) || (out_len == 0) ||
       (((uint32)(unsigned long)dst & 3u) != 0u))
    {
        return ARKRTOS_FDT_ERR_ARG;
    }
    if(cap < ARKRTOS_FDT_OVERLAY_MIN_ARENA)
    {
        return ARKRTOS_FDT_ERR_OVERLAY_SIZE;
    }

    rc = ovl_prepare(base, base_len, frag, frag_len, &bb, &fb);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }

    rc = ovl_collect_frags(&fb, &bb, frags, OVL_MAX_FRAGS, &nf);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    if(nf == 0u)
    {
        return ARKRTOS_FDT_ERR_OVERLAY;      /* 一个 fragment 都没有，不算 overlay */
    }

    rc = ovl_rsvmap_len(&bb, &rsvmap_len);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }
    off_struct = ovl_pad4(OVL_HEADER_SIZE + rsvmap_len);

    c.base = &bb;
    c.frag = &fb;
    c.frags = frags;
    c.nfrags = nf;
    c.cap = cap;

    rc = ovl_measure(&c, &struct_bytes, &str_upper);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }

    off_strings = off_struct + struct_bytes;
    if((off_strings < off_struct) || ((off_strings + str_upper) < off_strings))
    {
        return ARKRTOS_FDT_ERR_OVERLAY_SIZE; /* 相加溢出 */
    }
    if((off_strings + str_upper) > cap)
    {
        return ARKRTOS_FDT_ERR_OVERLAY_SIZE;
    }

    ovl_write_head(d, &bb, off_struct, off_strings, struct_bytes, rsvmap_len);

    rc = ovl_write(&c, d, off_struct, off_strings, struct_bytes, str_upper);
    if(rc != ARKRTOS_FDT_OK)
    {
        return rc;
    }

    totalsize = off_strings + c.str_used;
    ovl_wr32(d + H_TOTALSIZE, totalsize);
    ovl_wr32(d + H_SIZE_STRINGS, c.str_used);
    *out_len = totalsize;
    return ARKRTOS_FDT_OK;
}

const char *arkrtos_fdt_overlay_reason(int rc)
{
    switch(rc)
    {
        case ARKRTOS_FDT_OK:
            return "ok";
        case ARKRTOS_FDT_ERR_ARG:
            return "bad argument";
        case ARKRTOS_FDT_ERR_MAGIC:
            return "blob magic is not d00dfeed";
        case ARKRTOS_FDT_ERR_VERSION:
            return "blob version is not a DTB we know";
        case ARKRTOS_FDT_ERR_RANGE:
            return "a block offset/size runs past the blob";
        case ARKRTOS_FDT_ERR_ALIGN:
            return "a block offset is not aligned";
        case ARKRTOS_FDT_ERR_FORMAT:
            return "structure block is not well formed";
        case ARKRTOS_FDT_ERR_DEPTH:
            return "nesting deeper than the cap";
        case ARKRTOS_FDT_ERR_NOT_INIT:
            return "not initialised";
        case ARKRTOS_FDT_ERR_NOT_FOUND:
            return "path not found";
        case ARKRTOS_FDT_ERR_PATH:
            return "path not understood";
        case ARKRTOS_FDT_ERR_OVERLAY:
            return "fragment shape not supported";
        case ARKRTOS_FDT_ERR_OVERLAY_TARGET:
            return "target-path missing or not found";
        case ARKRTOS_FDT_ERR_OVERLAY_DUP:
            return "fragment child duplicates an existing node";
        case ARKRTOS_FDT_ERR_OVERLAY_SIZE:
            return "output arena too small";
        case ARKRTOS_FDT_ERR_OVERLAY_DEL:
            return "delete target not found";
        case ARKRTOS_FDT_ERR_OVERLAY_TAG:
            return "__fixups__/__symbols__ not supported";
        default:
            return "unknown";
    }
}

#endif /* ARKRTOS_USE_FDT_OVERLAY */
