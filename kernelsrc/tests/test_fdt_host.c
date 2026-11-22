/**
* @file test_fdt_host.c
* @brief Host-side unit test for arkrtos_fdt.c (flattened device tree reader).
*
* What this proves: the parser's refusals, not just its happy path.  A device
* tree that parses "almost right" is worse than one that refuses, so the case
* builds real DTB blobs byte by byte (header + structure block + strings
* block) and then breaks them one field at a time:
*   - every header field that can be wrong has its own code: null / short
*     buffer (ARG), bad magic, version below 16 or last_comp above version,
*     totalsize out of range, misaligned struct/strings/rsvmap offsets;
*   - the structure block must be strictly nested and end exactly at the end
*     of the block: a missing END, an END with nodes still open, bytes after
*     END, an END_NODE at depth 0, an unknown token, a node name without a
*     NUL, a property name offset outside the strings block, and a property
*     length past the block are all FORMAT;
*   - a blob nested deeper than ARKRTOS_FDT_MAX_DEPTH is DEPTH, not a walk;
*   - init is transactional: a bad blob leaves the previous tree usable.
*
* Then the read side: depth-first iteration order, the meta nodes
* (__symbols__ / __fixups__ / __local_fixups__) being skipped from the count
* and from the walk, property lookup / string / u32 cells / enabled status,
* absolute paths, phandle and linux,phandle, aliases in both the string and
* the phandle form, and /chosen/stdout-path in all three shapes (alias with
* options, absolute path, single phandle cell) plus every option spelling
* that must be refused instead of guessed.
*
* This is the reader only.  It does NOT prove that a board's real dtb parses
* (tools/fdt_gate.py does that against the shipped dtbs) and it does NOT
* prove that any driver uses the node it found -- those are separate gates.
*
* ARKRTOS_USE_FDT is compiled at 1 for this TU.
*
* @author xw
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fdt.h"

/* ------------------------------------------------------------ assertions */

static int g_checks;
static int g_failed;

#define CK(cond)                                                       \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                              \
    } while (0)

#define CK_I(got, want)                                                \
    do {                                                               \
        long g_ = (long)(got);                                         \
        long w_ = (long)(want);                                        \
        g_checks++;                                                    \
        if (g_ != w_) {                                                \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %ld want %ld)\n",            \
                   __LINE__, #got, g_, w_);                            \
        }                                                              \
    } while (0)

#define CK_S(got, want)                                                \
    do {                                                               \
        const char *g_ = (got);                                        \
        const char *w_ = (want);                                       \
        g_checks++;                                                    \
        if ((g_ == 0) || (strcmp(g_, w_) != 0)) {                      \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, (g_ != 0) ? g_ : "(null)", w_);     \
        }                                                              \
    } while (0)

/* --------------------------------------------------------- blob builder */

#define BLOB_MAX   4096u
#define STRUCT_OFF 48u          /* 40-byte header + 8-byte empty rsvmap */
#define STRINGS_MAX 1024u

static uint8  g_buf[BLOB_MAX];
static uint8  g_strings[STRINGS_MAX];
static uint32 g_blob_len;
static uint32 g_struct_len;
static uint32 g_strings_len;

/* what build_tree() should emit for the optional pieces */
static int         g_have_aliases = 1;
static int         g_have_chosen = 1;
static int         g_alias_phandle = 0; /* 1: uart1 is a phandle cell */
static int         g_stdout_mode = 0;   /* 0: string, 1: phandle cell, 2: raw */
static const char *g_stdout = "serial0:115200n8";

static void b32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static uint32 b32v(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | (uint32)p[3];
}

static uint32 s_intern(const char *s)
{
    uint32 n = (uint32)strlen(s) + 1u;
    uint32 off = g_strings_len;

    memcpy(&g_strings[g_strings_len], s, (size_t)n);
    g_strings_len += n;
    return off;
}

static void emit(uint32 tok)
{
    b32(&g_buf[STRUCT_OFF + g_struct_len], tok);
    g_struct_len += 4u;
}

static void b_reset(void)
{
    memset(g_buf, 0, sizeof g_buf);
    memset(g_strings, 0, sizeof g_strings);
    g_blob_len = 0u;
    g_struct_len = 0u;
    g_strings_len = 0u;
}

static void b_pad(void)
{
    while ((g_struct_len % 4u) != 0u) {
        g_buf[STRUCT_OFF + g_struct_len] = 0u;
        g_struct_len++;
    }
}

static void b_begin(const char *name)
{
    uint32 n = (uint32)strlen(name);

    emit(1u);
    if (n != 0u) {
        memcpy(&g_buf[STRUCT_OFF + g_struct_len], name, (size_t)n);
        g_struct_len += n;
    }
    /* 名字字段是 NUL 结尾并按 4 字节对齐（根节点名是空串，也必须占满一个
     * 对齐单元）；只靠 b_pad() 补零在 n == 0 时补不出任何字节。 */
    g_buf[STRUCT_OFF + g_struct_len] = 0u;
    g_struct_len++;
    b_pad();
}

static void b_end(void)
{
    emit(2u);
}

static void b_prop_raw(const char *name, const void *v, uint32 len)
{
    emit(3u);
    b32(&g_buf[STRUCT_OFF + g_struct_len], len);
    g_struct_len += 4u;
    b32(&g_buf[STRUCT_OFF + g_struct_len], s_intern(name));
    g_struct_len += 4u;
    if (len != 0u) {
        memcpy(&g_buf[STRUCT_OFF + g_struct_len], v, (size_t)len);
        g_struct_len += len;
    }
    b_pad();
}

static void b_prop_str(const char *name, const char *s)
{
    b_prop_raw(name, s, (uint32)strlen(s) + 1u);
}

static void b_prop_u32(const char *name, uint32 v)
{
    uint8 t[4];

    b32(t, v);
    b_prop_raw(name, t, 4u);
}

static void b_prop_cells(const char *name, const uint32 *v, uint32 n)
{
    uint8 t[16];
    uint32 i;

    for (i = 0u; i < n; i++) {
        b32(&t[i * 4u], v[i]);
    }
    b_prop_raw(name, t, n * 4u);
}

/* header only; the caller has already emitted every token it wants */
static void b_finish_raw(void)
{
    uint32 off_strings = STRUCT_OFF + g_struct_len;
    uint32 total;

    if (g_struct_len > BLOB_MAX - STRUCT_OFF) {
        printf("FAIL: struct overflow\n");
        g_failed++;
        return;
    }
    memcpy(&g_buf[off_strings], g_strings, (size_t)g_strings_len);
    total = off_strings + g_strings_len;

    b32(&g_buf[0], 0xd00dfeedu);          /* magic */
    b32(&g_buf[4], total);                /* totalsize */
    b32(&g_buf[8], STRUCT_OFF);           /* off_dt_struct */
    b32(&g_buf[12], off_strings);         /* off_dt_strings */
    b32(&g_buf[16], 40u);                 /* off_mem_rsvmap (8-aligned) */
    b32(&g_buf[20], 17u);                 /* version */
    b32(&g_buf[24], 16u);                 /* last_comp_version */
    b32(&g_buf[28], 0u);                  /* boot_cpuid_phys */
    b32(&g_buf[32], g_strings_len);       /* size_dt_strings */
    b32(&g_buf[36], g_struct_len);        /* size_dt_struct */
    g_blob_len = total;
}

static void b_finish(void)
{
    emit(9u);
    b_finish_raw();
}

/* ------------------------------------------------------------- the tree */

static void build_tree(void)
{
    static const uint32 reg0[4] = { 0u, 0x1fe001e0u, 0u, 0x100u };
    static const uint32 bad3[3] = { 1u, 2u, 3u };

    b_reset();
    b_begin("");
    b_prop_str("model", "Ark Test Board");
    b_prop_u32("#address-cells", 2u);

    if (g_have_aliases) {
        b_begin("aliases");
        b_prop_str("serial0", "/soc/serial@1fe001e0");
        if (g_alias_phandle) {
            b_prop_u32("uart1", 2u);
        } else {
            b_prop_str("uart1", "/soc/serial@1fe00200");
        }
        b_prop_u32("gost", 0x99u);
        b_prop_raw("weird", "abc", 3u);
        b_prop_str("rel", "soc/serial@1fe001e0");
        b_end();
    }
    if (g_have_chosen) {
        b_begin("chosen");
        if (g_stdout_mode == 0) {
            b_prop_str("stdout-path", g_stdout);
        } else if (g_stdout_mode == 1) {
            b_prop_u32("stdout-path", 1u);
        } else {
            b_prop_raw("stdout-path", "abc", 3u);
        }
        b_end();
    }
    b_begin("soc");
    b_begin("serial@1fe001e0");
    b_prop_str("compatible", "ns16550a");
    b_prop_cells("reg", reg0, 4u);
    b_prop_u32("phandle", 1u);
    b_prop_str("status", "okay");
    b_prop_u32("clock-frequency", 100000000u);
    b_end();
    b_begin("serial@1fe00200");
    b_prop_str("compatible", "ns16550a");
    b_prop_u32("linux,phandle", 2u);
    b_prop_str("status", "disabled");
    b_end();
    b_begin("deep");
    b_begin("deeper");
    b_begin("deepest");
    b_prop_str("label", "bottom");
    b_prop_cells("cells", bad3, 3u);
    b_end();
    b_end();
    b_end();
    b_begin("broken");
    b_prop_str("status", "fail");
    b_end();
    b_end();
    b_begin("__symbols__");
    b_prop_str("uart0", "/soc/serial@1fe001e0");
    b_end();
    b_end();
    b_finish();
}

/* node count: aliases, chosen, soc, 4 under soc, deep, deeper, deepest = 9
 * (__symbols__ is a meta node and is excluded, exactly like arkrtos_dt_count) */
#define TREE_NODES 9u

/* --------------------------------------------------------------- cases */

static void test_not_init(void)
{
    arkrtos_fdt_iter_t it;
    arkrtos_fdt_node_t n;
    arkrtos_fdt_console_t c;
    char buf[64];
    uint32 len = 0u;

    memset(&it, 0, sizeof it);
    memset(&n, 0, sizeof n);
    memset(&c, 0, sizeof c);

    CK_I(arkrtos_fdt_ready(), 0);
    CK_I(arkrtos_fdt_node_count(), 0);
    CK(arkrtos_fdt_model() == 0);
    CK(arkrtos_fdt_blob() == 0);
    CK_I(arkrtos_fdt_size(), 0);
    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_ERR_NOT_INIT);
    CK_I(arkrtos_fdt_iter_init(0), ARKRTOS_FDT_ERR_NOT_INIT);
    CK_I(arkrtos_fdt_next(&it, &n), ARKRTOS_FDT_ERR_NOT_INIT);
    CK(arkrtos_fdt_prop(&n, "model", &len) == 0);
    CK_I(arkrtos_fdt_node_by_path("/", &n), ARKRTOS_FDT_ERR_NOT_INIT);
    CK_I(arkrtos_fdt_node_by_phandle(1u, &n), ARKRTOS_FDT_ERR_NOT_INIT);
    CK_I(arkrtos_fdt_path_of_phandle(1u, buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_INIT);
    CK_I(arkrtos_fdt_alias("serial0", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_INIT);
    CK(arkrtos_fdt_stdout_raw(&len) == 0);
    CK(arkrtos_fdt_stdout_path() == 0);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_ERR_NOT_INIT);
}

/* every header / structure-block field that can be wrong, one at a time */
static void test_header_refusals(void)
{
    uint8 tmp[BLOB_MAX];

    build_tree();

    CK_I(arkrtos_fdt_init(0, g_blob_len), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_init(g_buf, 39u), ARKRTOS_FDT_ERR_ARG);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[0], 0xdeadbeefu);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_MAGIC);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[20], 15u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_VERSION);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[24], 18u);                     /* last_comp > version */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_VERSION);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[4], 20u);                      /* totalsize < header */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[4], g_blob_len + 4u);          /* totalsize > size */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[8], STRUCT_OFF + 1u);          /* off_struct not 4-aligned */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_ALIGN);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[12], b32v(&tmp[12]) + 1u);      /* off_strings not 4-aligned */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_ALIGN);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[16], 41u);                     /* off_rsvmap not 8-aligned */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_ALIGN);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[8], (g_blob_len + 8u) & ~3u);  /* off_struct past totalsize, aligned */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[36], g_blob_len);              /* size_struct past the end */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[12], (g_blob_len + 8u) & ~3u); /* off_strings past totalsize, aligned */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[32], g_blob_len);              /* size_strings past the end */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    /* a valid tree must still parse after all those refusals */
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_ready(), 1);
}

static void test_struct_refusals(void)
{
    uint8 tmp[BLOB_MAX];
    uint32 off_prop;

    /* one-node tree: BEGIN "" / END_NODE / FDT_END */
    b_reset();
    b_begin("");
    b_end();
    b_finish();

    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    /* no FDT_END inside the block */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[36], g_struct_len - 4u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* END while the root is still open */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[STRUCT_OFF + 8u], 9u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* END_NODE at depth 0 (first token) */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[STRUCT_OFF], 2u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* unknown token */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[STRUCT_OFF], 7u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* bytes after FDT_END */
    b_reset();
    b_begin("");
    b_end();
    emit(9u);
    emit(4u);                    /* an extra NOP past END */
    b_finish_raw();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* node name with no NUL before the end of the block */
    b_reset();
    emit(1u);
    memset(&g_buf[STRUCT_OFF + 4u], 'A', 16u);
    g_struct_len = 4u + 16u;
    b_finish_raw();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* property name offset outside the strings block */
    b_reset();
    b_begin("");
    off_prop = g_struct_len;
    b_prop_str("model", "x");
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[STRUCT_OFF + off_prop + 8u], 0xffffu);   /* nameoff */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* property length running past the block */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[STRUCT_OFF + off_prop + 4u], 0xffffu);   /* len */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* property header itself truncated (fewer than 8 bytes left) */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[36], off_prop + 4u + 4u);               /* cut inside the header */
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);
}

static void test_depth_cap(void)
{
    uint32 i;

    b_reset();
    b_begin("");
    for (i = 0u; i < 18u; i++) {
        b_begin("n");
    }
    for (i = 0u; i < 18u; i++) {
        b_end();
    }
    b_end();
    b_finish();

    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_DEPTH);

    /* 拒绝之后一棵正常的树仍然能装进来：深度上限只拒那一份 blob */
    b_reset();
    b_begin("");
    b_begin("ok");
    b_end();
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_count(), 1u);
}

static void test_transactional(void)
{
    uint8 tmp[BLOB_MAX];
    uint32 before;

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    before = arkrtos_fdt_node_count();
    CK_I(before, TREE_NODES);

    /* a bad blob must not take the working tree down with it */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[0], 0x12345678u);
    CK_I(arkrtos_fdt_init(tmp, g_blob_len), ARKRTOS_FDT_ERR_MAGIC);
    CK_I(arkrtos_fdt_ready(), 1);
    CK_I(arkrtos_fdt_node_count(), before);
    CK_S(arkrtos_fdt_model(), "Ark Test Board");
    CK(arkrtos_fdt_blob() == (const void *)g_buf);
    CK_I(arkrtos_fdt_size(), g_blob_len);
}

static void test_init_and_model(void)
{
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_ready(), 1);
    CK_I(arkrtos_fdt_node_count(), TREE_NODES);
    CK_S(arkrtos_fdt_model(), "Ark Test Board");
    CK(arkrtos_fdt_blob() == (const void *)g_buf);
    CK_I(arkrtos_fdt_size(), g_blob_len);

    /* no model property -> NULL, and the other root props are not "model" */
    b_reset();
    b_begin("");
    b_prop_str("not-model", "nope");
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_model() == 0);

    /* a model that is not NUL terminated is not a model */
    b_reset();
    b_begin("");
    b_prop_raw("model", "abcd", 4u);
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_model() == 0);
}

static void test_iteration(void)
{
    static const char *const names[TREE_NODES] = {
        "aliases", "chosen", "soc",
        "serial@1fe001e0", "serial@1fe00200", "deep", "deeper", "deepest",
        "broken"
    };
    /* depth 从 1 起算：根的直接子节点是 1（根自身用 "/" 查到才是 0） */
    static const uint32 depths[TREE_NODES] = { 1u, 1u, 1u, 2u, 2u, 2u, 3u, 4u, 2u };
    arkrtos_fdt_iter_t it;
    arkrtos_fdt_node_t n;
    uint32 i = 0u;
    int rc;

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_iter_init(0), ARKRTOS_FDT_ERR_NOT_INIT);

    for (;;) {
        rc = arkrtos_fdt_next(&it, &n);
        if (rc != 1) {
            break;
        }
        if (i < TREE_NODES) {
            CK_S(n.name, names[i]);
            CK_I(n.depth, depths[i]);
        }
        i++;
    }
    CK_I(rc, 0);
    CK_I(i, TREE_NODES);

    /* __symbols__ is skipped by the walk, so it never shows up */
    CK_I(arkrtos_fdt_node_by_path("/__symbols__", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_next(0, 0), ARKRTOS_FDT_ERR_NOT_INIT);

    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_next(&it, 0), ARKRTOS_FDT_ERR_NOT_INIT);
}

static void test_props(void)
{
    arkrtos_fdt_node_t n;
    arkrtos_fdt_node_t z;
    uint32 v = 0u;
    uint32 len = 0u;
    const uint32 *p;

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@1fe001e0", &n), ARKRTOS_FDT_OK);
    CK_I(n.n_props, 5);
    CK_I(n.depth, 2);
    CK_S(n.name, "serial@1fe001e0");

    CK_S(arkrtos_fdt_prop_str(&n, "compatible"), "ns16550a");
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 0u, &v), 0);
    CK_I(v, 0u);
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 1u, &v), 0);
    CK_I(v, 0x1fe001e0u);
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 3u, &v), 0);
    CK_I(v, 0x100u);
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 4u, &v), -1);      /* past the cells */
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 0u, 0), -1);       /* nowhere to write */

    p = (const uint32 *)arkrtos_fdt_prop(&n, "reg", &len);
    CK(p != 0);
    CK_I(len, 16u);

    CK(arkrtos_fdt_prop(&n, "nope", &len) == 0);
    CK(arkrtos_fdt_prop(0, "reg", &len) == 0);
    CK(arkrtos_fdt_prop(&n, 0, &len) == 0);

    /* a non-NUL-terminated value is not a string */
    CK_I(arkrtos_fdt_node_by_path("/aliases", &n), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_prop_str(&n, "weird") == 0);
    CK(arkrtos_fdt_prop_str(&n, "nope") == 0);

    /* enabled: absent status is enabled, "okay" is enabled, anything else is not */
    CK_I(arkrtos_fdt_node_by_path("/soc", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_enabled(&n), 1);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@1fe001e0", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_enabled(&n), 1);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@1fe00200", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_enabled(&n), 0);
    CK_I(arkrtos_fdt_node_by_path("/soc/broken", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_enabled(&n), 0);
    CK_I(arkrtos_fdt_node_enabled(0), 1);       /* no node -> prop 0 -> enabled */

    /* a node view with no properties at all */
    memset(&z, 0, sizeof z);
    z.props_off = STRUCT_OFF + g_struct_len;    /* first non-PROP token */
    CK(arkrtos_fdt_prop(&z, "x", &len) == 0);
    CK_I(z.n_props, 0);
}

static void test_paths(void)
{
    arkrtos_fdt_node_t n;
    char big[200];
    char shortpath[64];
    uint32 i;

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    CK_I(arkrtos_fdt_node_by_path("/", &n), ARKRTOS_FDT_OK);
    CK_S(n.name, "");
    CK_I(n.depth, 0);

    CK_I(arkrtos_fdt_node_by_path("/soc", &n), ARKRTOS_FDT_OK);
    CK_S(n.name, "soc");
    CK_I(n.depth, 1);

    CK_I(arkrtos_fdt_node_by_path("/soc/serial@1fe001e0", &n), ARKRTOS_FDT_OK);
    CK_S(n.name, "serial@1fe001e0");
    CK_I(n.depth, 2);

    CK_I(arkrtos_fdt_node_by_path("/soc/deep/deeper/deepest", &n), ARKRTOS_FDT_OK);
    CK_I(n.depth, 4);
    CK_S(arkrtos_fdt_prop_str(&n, "label"), "bottom");

    CK_I(arkrtos_fdt_node_by_path("/soc/", &n), ARKRTOS_FDT_OK);   /* trailing slash */
    CK_S(n.name, "soc");

    CK_I(arkrtos_fdt_node_by_path("/nosuch", &n), ARKRTOS_FDT_ERR_NOT_FOUND);
    CK_I(arkrtos_fdt_node_by_path("/soc/nosuch", &n), ARKRTOS_FDT_ERR_NOT_FOUND);
    CK_I(arkrtos_fdt_node_by_path("/soc/deep/deeper/nosuch", &n),
         ARKRTOS_FDT_ERR_NOT_FOUND);

    CK_I(arkrtos_fdt_node_by_path("soc", &n), ARKRTOS_FDT_ERR_PATH);
    CK_I(arkrtos_fdt_node_by_path("", &n), ARKRTOS_FDT_ERR_PATH);
    CK_I(arkrtos_fdt_node_by_path(0, &n), ARKRTOS_FDT_ERR_PATH);

    /* longer than ARKRTOS_FDT_PATH_MAX: refused, never truncated */
    big[0] = '/';
    for (i = 1u; i < (uint32)sizeof big; i++) {
        big[i] = 'a';
    }
    big[sizeof big - 1u] = '\0';
    CK_I(arkrtos_fdt_node_by_path(big, &n), ARKRTOS_FDT_ERR_PATH);

    /* more components than the depth cap allows */
    strcpy(shortpath, "/a");
    for (i = 0u; i < 20u; i++) {
        strcat(shortpath, "/b");
    }
    CK_I(arkrtos_fdt_node_by_path(shortpath, &n), ARKRTOS_FDT_ERR_PATH);

    /* out == 0 is allowed: the callers that only want "does it exist" use it */
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@1fe001e0", 0), ARKRTOS_FDT_OK);
}

static void test_phandles(void)
{
    arkrtos_fdt_node_t n;
    char buf[64];

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    CK_I(arkrtos_fdt_node_by_phandle(1u, &n), ARKRTOS_FDT_OK);
    CK_S(n.name, "serial@1fe001e0");
    CK_I(n.depth, 2);
    CK_I(arkrtos_fdt_node_by_phandle(2u, &n), ARKRTOS_FDT_OK);   /* linux,phandle */
    CK_S(n.name, "serial@1fe00200");
    CK_I(arkrtos_fdt_node_by_phandle(99u, &n), ARKRTOS_FDT_ERR_NOT_FOUND);
    CK_I(arkrtos_fdt_node_by_phandle(0u, &n), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_node_by_phandle(1u, 0), ARKRTOS_FDT_OK);    /* out optional */

    CK_I(arkrtos_fdt_path_of_phandle(1u, buf, (uint32)sizeof buf), ARKRTOS_FDT_OK);
    CK_S(buf, "/soc/serial@1fe001e0");
    CK_I(arkrtos_fdt_path_of_phandle(2u, buf, (uint32)sizeof buf), ARKRTOS_FDT_OK);
    CK_S(buf, "/soc/serial@1fe00200");
    CK_I(arkrtos_fdt_path_of_phandle(1u, buf, 8u), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_path_of_phandle(1u, buf, 1u), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_path_of_phandle(1u, 0, 10u), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_path_of_phandle(0u, buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_path_of_phandle(99u, buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_FOUND);
}

static void test_aliases(void)
{
    char buf[64];

    g_alias_phandle = 0;
    g_have_aliases = 1;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    CK_I(arkrtos_fdt_alias("serial0", buf, (uint32)sizeof buf), ARKRTOS_FDT_OK);
    CK_S(buf, "/soc/serial@1fe001e0");
    CK_I(arkrtos_fdt_alias("uart1", buf, (uint32)sizeof buf), ARKRTOS_FDT_OK);
    CK_S(buf, "/soc/serial@1fe00200");

    CK_I(arkrtos_fdt_alias("gost", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_FOUND);                    /* phandle that is not there */
    CK_I(arkrtos_fdt_alias("rel", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_PATH);                          /* relative form */
    CK_I(arkrtos_fdt_alias("weird", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_PATH);                          /* neither string nor cell */
    CK_I(arkrtos_fdt_alias("nope", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_FOUND);
    CK_I(arkrtos_fdt_alias("serial0", buf, 8u), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_alias("serial0", buf, 1u), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_alias(0, buf, (uint32)sizeof buf), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_alias("serial0", 0, 32u), ARKRTOS_FDT_ERR_ARG);

    /* the phandle form of "uart1 = &uart2" */
    g_alias_phandle = 1;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_alias("uart1", buf, (uint32)sizeof buf), ARKRTOS_FDT_OK);
    CK_S(buf, "/soc/serial@1fe00200");
    g_alias_phandle = 0;

    /* no /aliases node at all */
    g_have_aliases = 0;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_alias("serial0", buf, (uint32)sizeof buf),
         ARKRTOS_FDT_ERR_NOT_FOUND);
    g_have_aliases = 1;
}

static void test_stdout(void)
{
    arkrtos_fdt_console_t c;
    uint32 len = 0u;

    /* 1) alias with options */
    g_have_chosen = 1;
    g_stdout_mode = 0;
    g_stdout = "serial0:115200n8";
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    CK(arkrtos_fdt_stdout_raw(&len) != 0);
    CK_I(len, 17u);                                     /* "serial0:115200n8" + NUL */
    CK_S(arkrtos_fdt_stdout_path(), "serial0:115200n8");
    CK_I(arkrtos_fdt_stdout_raw(0) != 0, 1);            /* len is optional */

    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_OK);
    CK_S(c.path, "/soc/serial@1fe001e0");
    CK_I(c.baud, 115200u);
    CK_I(c.bits, 8u);
    CK_I(c.parity, 'n');
    CK_I(c.from_alias, 1u);
    CK_I(arkrtos_fdt_stdout_resolve(0), ARKRTOS_FDT_ERR_ARG);

    /* 2) absolute path, every option present, no alias involved */
    g_stdout = "/soc/serial@1fe00200:9600e8";
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_OK);
    CK_S(c.path, "/soc/serial@1fe00200");
    CK_I(c.baud, 9600u);
    CK_I(c.bits, 8u);
    CK_I(c.parity, 'e');
    CK_I(c.from_alias, 0u);

    /* 3) no ':' at all -> no options, and that is not an error */
    g_stdout = "serial0";
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_OK);
    CK_S(c.path, "/soc/serial@1fe001e0");
    CK_I(c.baud, 0u);
    CK_I(c.bits, 0u);
    CK_I(c.parity, 0);

    /* 一个空的选项串（"serial0:"）也算「什么都没写」而不是错误 */
    g_stdout = "serial0:";
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_OK);
    CK_S(c.path, "/soc/serial@1fe001e0");
    CK_I(c.baud, 0u);
    g_stdout = "serial0";

    /* 4) a phandle cell */
    g_stdout_mode = 1;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_stdout_raw(&len) != 0, 1);
    CK_I(len, 4u);
    CK(arkrtos_fdt_stdout_path() == 0);                  /* not a string */
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_OK);
    CK_S(c.path, "/soc/serial@1fe001e0");
    CK_I(c.baud, 0u);

    /* 5) a 3-byte value: neither a string nor a cell */
    g_stdout_mode = 2;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_stdout_path() == 0);
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_ERR_PATH);

    g_stdout_mode = 0;

    /* 6) refusals that must not become a guessed baud rate */
    {
        static const struct {
            const char *value;
            int         want;
        } bad[] = {
            { "nosuchdev:115200",       ARKRTOS_FDT_ERR_NOT_FOUND },
            { "serial0:115200x",        ARKRTOS_FDT_ERR_PATH },
            { "serial0:9600115200",     ARKRTOS_FDT_ERR_PATH },  /* two bauds */
            { "serial0:8n7",            ARKRTOS_FDT_ERR_PATH },  /* two bit counts */
            { "serial0:nn",             ARKRTOS_FDT_ERR_PATH },  /* two parities */
            { ":115200",                ARKRTOS_FDT_ERR_PATH },  /* empty device */
            { "serial0:12345678",       ARKRTOS_FDT_ERR_PATH },  /* > 7 digits */
            { "/soc/nosuch:115200",     ARKRTOS_FDT_ERR_NOT_FOUND },
        };
        uint32 i;

        for (i = 0u; i < (uint32)(sizeof bad / sizeof bad[0]); i++) {
            g_stdout = bad[i].value;
            build_tree();
            CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
            memset(&c, 0, sizeof c);
            CK_I(arkrtos_fdt_stdout_resolve(&c), bad[i].want);
        }
        g_stdout = "serial0:115200n8";
    }

    /* 7) no /chosen at all */
    g_have_chosen = 0;
    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_stdout_raw(&len) == 0);
    CK(arkrtos_fdt_stdout_path() == 0);
    memset(&c, 0, sizeof c);
    CK_I(arkrtos_fdt_stdout_resolve(&c), ARKRTOS_FDT_ERR_NOT_FOUND);
    g_have_chosen = 1;
}

static void test_load(void)
{
    uint8 ram[BLOB_MAX];
    uint8 small[10];
    uint8 tmp[BLOB_MAX];
    uint32 before;

    build_tree();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    before = arkrtos_fdt_node_count();

    CK_I(arkrtos_fdt_load(&ram[1], sizeof ram - 1u, g_buf, g_blob_len),
         ARKRTOS_FDT_ERR_ARG);                        /* dst not 4-aligned */
    CK_I(arkrtos_fdt_load(ram, 8u, g_buf, g_blob_len), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_load(0, sizeof ram, g_buf, g_blob_len), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_load(ram, sizeof ram, 0, g_blob_len), ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_load(ram, sizeof ram, g_buf, 0u), ARKRTOS_FDT_ERR_ARG);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[0], 0x0badf00du);
    CK_I(arkrtos_fdt_load(ram, sizeof ram, tmp, g_blob_len), ARKRTOS_FDT_ERR_MAGIC);

    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[4], g_blob_len + 8u);
    CK_I(arkrtos_fdt_load(ram, sizeof ram, tmp, g_blob_len), ARKRTOS_FDT_ERR_RANGE);

    /* a short source is copied and then refused by init, not by the copy */
    memset(small, 0, sizeof small);
    CK_I(arkrtos_fdt_load(ram, sizeof ram, small, (uint32)sizeof small),
         ARKRTOS_FDT_ERR_ARG);

    /* the working tree survived all of that */
    CK_I(arkrtos_fdt_node_count(), before);
    CK_I(arkrtos_fdt_ready(), 1);

    /* a real copy becomes the live tree and points at the caller's RAM */
    memset(ram, 0, sizeof ram);
    CK_I(arkrtos_fdt_load(ram, (uint32)sizeof ram, g_buf, g_blob_len),
         ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_blob() == (const void *)ram);
    CK_I(arkrtos_fdt_size(), g_blob_len);
    CK_I(arkrtos_fdt_node_count(), before);

    /* and a bad blob after that still leaves the RAM copy in place */
    memcpy(tmp, g_buf, (size_t)g_blob_len);
    b32(&tmp[20], 1u);
    CK_I(arkrtos_fdt_load(ram, (uint32)sizeof ram, tmp, g_blob_len),
         ARKRTOS_FDT_ERR_VERSION);
    CK(arkrtos_fdt_blob() == (const void *)ram);
    CK_I(arkrtos_fdt_node_count(), before);
}

static void test_struct_edges(void)
{
    arkrtos_fdt_iter_t it;
    arkrtos_fdt_node_t n;
    uint32 i;

    /* NOP tokens are allowed anywhere and must be ignored by both the scan
     * and the walk */
    b_reset();
    emit(4u);
    b_begin("");
    emit(4u);
    b_begin("a");
    b_prop_u32("v", 7u);
    emit(4u);
    b_end();
    b_end();
    b_finish();

    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_count(), 1u);
    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_next(&it, &n), 1);
    CK_S(n.name, "a");
    CK_I(arkrtos_fdt_next(&it, &n), 0);
    CK_I(arkrtos_fdt_node_by_path("/a", &n), ARKRTOS_FDT_OK);

    /* a node view must be able to count its own properties */
    CK_I(n.n_props, 1u);

    /* walking a truncated structure block from the cursor is a FORMAT error,
     * not a silent end */
    b_reset();
    b_begin("");
    b_begin("a");
    b_end();
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);

    /* 截断只能从 init 进：视图在 init 时就把 size_struct 定下了，事后再改头
     * 字段不会被任何查询看见（这样反而证明大小快照真的只有一份）。 */
    b32(&g_buf[36], 8u);                   /* only BEGIN "" is inside now */
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);
    b32(&g_buf[36], g_struct_len);
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_by_path("/a", &n), ARKRTOS_FDT_OK);

    /* empty tree: root with no children, no model */
    b_reset();
    b_begin("");
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_count(), 0u);
    CK_I(arkrtos_fdt_node_by_path("/", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_by_path("/a", &n), ARKRTOS_FDT_ERR_NOT_FOUND);

    /* __fixups__ and __local_fixups__ are excluded from the count too */
    b_reset();
    b_begin("");
    b_begin("__fixups__");
    b_prop_str("uart0", "/soc:target:0");
    b_begin("inner");
    b_end();
    b_end();
    b_begin("__local_fixups__");
    b_end();
    b_begin("real");
    b_end();
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_count(), 1u);
    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_next(&it, &n), 1);
    CK_S(n.name, "real");
    CK_I(arkrtos_fdt_next(&it, &n), 0);

    /* the meta node itself is still reachable by path, subtree included */
    CK_I(arkrtos_fdt_node_by_path("/__fixups__", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_by_path("/__fixups__/inner", &n), ARKRTOS_FDT_OK);

    CK_I(arkrtos_fdt_node_by_path("/real", &n), ARKRTOS_FDT_OK);

    /* a meta subtree may legitimately carry NOP padding; the skip must not
     * be confused by tokens that the scanner otherwise ignores */
    b_reset();
    b_begin("");
    b_begin("__fixups__");
    emit(4u);
    b_begin("inner");
    emit(4u);
    b_end();
    b_end();
    b_begin("real");
    b_end();
    b_end();
    b_finish();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_count(), 1u);
    CK_I(arkrtos_fdt_iter_init(&it), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_next(&it, &n), 1);
    CK_S(n.name, "real");
    CK_I(arkrtos_fdt_next(&it, &n), 0);
    CK_I(arkrtos_fdt_node_by_path("/__fixups__/inner", &n), ARKRTOS_FDT_OK);

    /* a node name that runs to the very end of the structure block has no
     * NUL terminator -> FORMAT, not a name that reads past the block */
    b_reset();
    b_begin("");
    emit(1u);                              /* BEGIN_NODE, then bare bytes */
    memcpy(&g_buf[STRUCT_OFF + g_struct_len], "abcd", 4u);
    g_struct_len += 4u;
    b_finish_raw();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    /* a property that declares more bytes than the block holds: the header
     * itself is inside the block, the padded value is not */
    b_reset();
    b_begin("");
    emit(3u);
    b32(&g_buf[STRUCT_OFF + g_struct_len], 100u);        /* len */
    g_struct_len += 4u;
    b32(&g_buf[STRUCT_OFF + g_struct_len], s_intern("p"));
    g_struct_len += 4u;                    /* block ends before the value */
    b_finish_raw();
    CK_I(arkrtos_fdt_init(g_buf, g_blob_len), ARKRTOS_FDT_ERR_FORMAT);

    (void)i;
}

int main(void)
{
    test_not_init();
    test_header_refusals();
    test_struct_refusals();
    test_depth_cap();
    test_transactional();
    test_init_and_model();
    test_iteration();
    test_props();
    test_paths();
    test_phandles();
    test_aliases();
    test_stdout();
    test_load();
    test_struct_edges();

    if (g_failed != 0) {
        printf("FDT-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("FDT-HOST OK  %d checks\n", g_checks);
    return 0;
}
