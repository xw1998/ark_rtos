/**
* @brief Ark RTOS device tree query API.
* @details The data behind this API is *generated* from dts/<board>.dts by
*          tools/dts/dtc_lite.py into board/<chip>/arkrtos_dt.c.  The header
*          below is hand written and stable; only the data file changes when
*          the tree changes, so a driver can be written against a fixed API.
*
*          A node is "bindable" (is_device != 0) when it carries a
*          `compatible` property; arkrtos_dt_find() only returns those, and
*          only when the node is enabled (status is absent or "okay").
*          Structural nodes are still present so that a phandle reference
*          always resolves to something.
*
*          Nothing here touches the hardware: the tree describes what is on
*          the board, a driver decides what to do with it.
*/

#ifndef __ARKRTOS_DT_H__
#define __ARKRTOS_DT_H__

#include "arkrtos_types.h"

/* Property kinds.  A property is one of these, never a mix: dtc_lite refuses
 * a property that mixes strings and cells. */
#define ARKRTOS_DT_PROP_U32     0u
#define ARKRTOS_DT_PROP_STR     1u
#define ARKRTOS_DT_PROP_BYTES   2u   /* count only; the bytes are not emitted */

/** @brief One property of a node.  Only the field for its kind is valid. */
typedef struct
{
    const char        *name;
    uint8              kind;    /* ARKRTOS_DT_PROP_* */
    uint16             count;   /* number of elements in u32[] / str[] */
    const uint32      *u32;     /* kind == U32 */
    const char *const *str;     /* kind == STR, NULL terminated */
} arkrtos_dt_prop_t;

/** @brief One node of the compiled tree. */
typedef struct
{
    const char              *name;         /* full name, e.g. serial@40011000 */
    const char              *compatible;   /* first compatible string, or 0 */
    uint32                   reg_base;     /* first <reg> cell */
    uint32                   reg_size;     /* second <reg> cell, 0 if absent */
    uint32                   phandle;      /* 0 when the node has no label */
    uint16                   parent;       /* parent index, 65535 for top level */
    uint16                   n_props;
    uint8                    status_okay;  /* 1 unless status = "disabled" */
    uint8                    is_device;    /* 1 when `compatible` is present */
    const arkrtos_dt_prop_t *props;
} arkrtos_dt_node_t;

/** @brief Number of nodes in the compiled tree (root excluded). */
uint32 arkrtos_dt_count(void);

/** @brief Node by index, or 0 when idx is out of range. */
const arkrtos_dt_node_t *arkrtos_dt_node(uint32 idx);

/** @brief First enabled node whose first compatible string matches. */
const arkrtos_dt_node_t *arkrtos_dt_find(const char *compatible);

/** @brief Iterate: pass the previous result (0 to start) to get the next. */
const arkrtos_dt_node_t *arkrtos_dt_find_next(const char *compatible,
                                             const arkrtos_dt_node_t *from);

/** @brief The root `model` string, or the board name when the tree has none. */
const char *arkrtos_dt_model(void);

/** @brief Property by name, or 0 when the node has no such property. */
const arkrtos_dt_prop_t *arkrtos_dt_prop(const arkrtos_dt_node_t *node,
                                         const char *name);

/** @brief Read the idx-th u32 of a U32 property.  Returns -1 on any mismatch. */
int32 arkrtos_dt_prop_u32(const arkrtos_dt_node_t *node, const char *name,
                          uint32 idx, uint32 *out);

/** @brief First string of a STR property, or 0. */
const char *arkrtos_dt_prop_str(const arkrtos_dt_node_t *node,
                                const char *name);

/** @brief NULL-safe string equality; 1 when equal and both non-NULL. */
int arkrtos_dt_str_eq(const char *a, const char *b);

#endif /* __ARKRTOS_DT_H__ */
