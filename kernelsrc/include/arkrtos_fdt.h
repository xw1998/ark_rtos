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

#endif /* __ARKRTOS_FDT_H__ */
