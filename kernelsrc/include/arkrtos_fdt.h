/**
* @brief Ark RTOS flattened device tree (DTB) reader.
* @details The MCU tier links a const C table generated from the DTS
*          (arkrtos_dt.h).  The MMU tier is the other shape: a standard
*          flattened blob sits in the image and is parsed at boot, exactly
*          like a Linux-style project.  This is that reader - read only,
*          no allocation, no recursion, no hardware access.
*
*          Both descriptions come from the same DTS, so the two tiers agree
*          by construction; this header is what lets the MMU tier ask the
*          same questions the C table answers.
*
*          Design rules, because a device tree that parses "almost right"
*          is worse than one that refuses:
*            - every offset, length and alignment is checked against the
*              blob before it is used; anything out of range is an error
*              code, never a clamped read;
*            - the structure block must be strictly nested and terminate
*              exactly at the end of the block - a blob that merely has a
*              valid magic is not accepted;
*            - nesting is capped (ARKRTOS_FDT_MAX_DEPTH) so a self-nesting
*              blob cannot walk the caller into a stack or time trap;
*            - one blob per kernel (init stores it once), matching the
*              C table's single-tree model.
*
*          Enabled by ARKRTOS_USE_FDT.  Default off: the .c compiles to
*          nothing, the shell gains nothing, and the image is byte-for-byte
*          what it was before this file existed.
*/

#ifndef __ARKRTOS_FDT_H__
#define __ARKRTOS_FDT_H__

#include "arkrtos_types.h"

/* Return codes.  Anything below zero is a refusal, and every refusal names
 * the property of the blob that failed rather than a line number. */
#define ARKRTOS_FDT_OK              0
#define ARKRTOS_FDT_ERR_ARG       (-1)   /* null blob, zero size, size < header */
#define ARKRTOS_FDT_ERR_MAGIC     (-2)   /* not 0xd00dfeed */
#define ARKRTOS_FDT_ERR_VERSION   (-3)   /* version < 16 (not a DTB we know) */
#define ARKRTOS_FDT_ERR_RANGE     (-4)   /* a block offset/size runs past size */
#define ARKRTOS_FDT_ERR_ALIGN     (-5)   /* a block offset is not 4-byte aligned */
#define ARKRTOS_FDT_ERR_FORMAT    (-6)   /* structure block is not well formed */
#define ARKRTOS_FDT_ERR_DEPTH     (-7)   /* nesting deeper than the cap */
#define ARKRTOS_FDT_ERR_NOT_INIT  (-8)   /* query before a successful init */

/* Deeper than any real board file; a blob that walks past this is either
 * corrupt or hostile, and both deserve a refusal instead of a long walk. */
#define ARKRTOS_FDT_MAX_DEPTH     16u

/** @brief One node, as a view into the blob (no copy, no allocation). */
typedef struct
{
    const char *name;      /* node name, NUL terminated inside the blob */
    uint32      depth;     /* 0 for the root's children, 1 for theirs, ... */
    uint32      n_props;   /* properties directly on this node */
    uint32      props_off; /* offset of its first PROP token in the blob */
} arkrtos_fdt_node_t;

/** @brief Walk cursor.  Treat the fields as private. */
typedef struct
{
    uint32 off;            /* next token to read */
    uint32 depth;          /* nodes opened and not yet closed */
} arkrtos_fdt_iter_t;

/** @brief Parse and validate the header; compute the node count. */
int arkrtos_fdt_init(const void *blob, uint32 size);

/** @brief 1 when the last init succeeded. */
int arkrtos_fdt_ready(void);

/** @brief Number of nodes, root excluded (matches arkrtos_dt_count()). */
uint32 arkrtos_fdt_node_count(void);

/** @brief The root "model" string, or 0 when the DTS has none. */
const char *arkrtos_fdt_model(void);

/** @brief Start a depth-first walk.  Returns ARKRTOS_FDT_ERR_NOT_INIT if not ready. */
int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it);

/**
* @brief Next node in depth-first order.
* @return 1 and fill *out for a node, 0 at the end of the tree,
*         a negative ARKRTOS_FDT_ERR_* for a malformed blob.
*/
int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out);

/**
* @brief Property value inside the blob.
* @param len Receives the byte length (may be 0 for a valueless property).
* @return The value pointer, or 0 when the node has no such property.
*/
const void *arkrtos_fdt_prop(const arkrtos_fdt_node_t *node, const char *name,
                             uint32 *len);

/** @brief First string of a string-list property, or 0. */
const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name);

/** @brief The idx-th 32-bit cell of a property.  Returns -1 on any mismatch. */
int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out);

/** @brief 1 when status is absent or "okay" - same rule as the C table. */
int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node);

/* --------------------------------------------------- 动态装载与换树 */
/* 解析器是一个只读视图：blob 在哪儿（镜像里的 const 数据、RAM 里的副本、
 * bootloader 交过来的地址）它不关心，只关心字节流是否合规。所以"动态加载"
 * 在这里只是一件小事——把 blob 拷进调用方的 RAM，再 init 它；换树失败
 * 不毁掉上一棵树。
 */

#define ARKRTOS_FDT_ERR_NOT_FOUND (-9)  /* 路径 / 别名 / 属性不存在 */
#define ARKRTOS_FDT_ERR_PATH      (-10) /* 路径或 stdout-path 格式不认识 */

/* 路径与字符串缓冲区上限。超了就是错误，不截断。 */
#define ARKRTOS_FDT_PATH_MAX      128u

/** @brief 当前正在被解析的 blob 与它的字节数（未 init 时返回 0）。 */
const void *arkrtos_fdt_blob(void);
uint32 arkrtos_fdt_size(void);

/**
* @brief 把 blob 复制进调用方 RAM 再解析——动态装载的落地形态。
* @param dst 目标缓冲区（4 字节对齐，容量 >= len）
* @param cap 目标缓冲区字节数；len > cap 直接拒绝，不是截断
* @details 失败时**保持上一棵树可用**：解析是事务性的，坏 blob 只换来一个
*          错误码，不会让这台机器"突然一个设备都没有"。
* @return ARKRTOS_FDT_OK 或负的错误码（ERR_ARG 含容量不足）
*/
int arkrtos_fdt_load(void *dst, uint32 cap, const void *src, uint32 len);

/** @brief 按绝对路径取节点（"/soc/serial@1fe001e0"；"/" 是根节点）。 */
int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out);

/** @brief /aliases/<name> 指向的路径，拷进 buf（含 NUL）。 */
int arkrtos_fdt_alias(const char *name, char *buf, uint32 cap);

/** @brief /chosen/stdout-path 的原始字符串；没有这个节点或属性就是 0。 */
const char *arkrtos_fdt_stdout_path(void);

/** @brief /chosen/stdout-path 的原始字节与长度（可能是字符串，也可能是句柄）。 */
const void *arkrtos_fdt_stdout_raw(uint32 *len);

/** @brief 按 phandle 取节点（phandle / linux,phandle 两种属性都认）。 */
int arkrtos_fdt_node_by_phandle(uint32 ph, arkrtos_fdt_node_t *out);

/** @brief 把一个 phandle 还原成绝对路径，拷进 buf（含 NUL）。 */
int arkrtos_fdt_path_of_phandle(uint32 ph, char *buf, uint32 cap);

/** @brief stdout-path 的解析结果（路径 + 选项）。 */
typedef struct
{
    char   path[ARKRTOS_FDT_PATH_MAX]; /* 解析后的绝对路径 */
    uint32 baud;                        /* 0 = 没写 */
    uint32 bits;                        /* 0 = 没写 */
    uint32 stop;                        /* 0 = 没写 */
    char   parity;                      /* 0 = 没写，否则 'n' / 'o' / 'e' */
    uint8  from_alias;                  /* 1 = 原值经由 /aliases 解析而来 */
} arkrtos_fdt_console_t;

/**
* @brief 解析 /chosen/stdout-path（"serial0:115200n8" 或绝对路径形式）。
* @return OK；ERR_NOT_INIT / ERR_NOT_FOUND（别名/路径不存在）/
*         ERR_PATH（选项写法不认识——不猜一个默认波特率）
*/
int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out);

#endif /* __ARKRTOS_FDT_H__ */
