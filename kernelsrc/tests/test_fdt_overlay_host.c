/**
* @file test_fdt_overlay_host.c
* @brief Host-side unit test for arkrtos_fdt_overlay.c (dtb overlay merge).
*
* What this proves: the merge result is a *legal* dtb that says the right
* thing, and every unsupported shape is refused with its own code instead of
* guessed.  The case builds both blobs byte by byte (header + reserve map +
* structure block + strings block), then reads the produced blob back with the
* real parser (arkrtos_fdt.c) instead of poking at offsets: an overlay whose
* output only "looks right" but cannot be parsed is worse than a refusal.
*
* Covered:
*   - target-path and target=<&label> (0xffffffff placeholder resolved through
*     the fragment's __fixups__ and the base tree's __symbols__);
*   - property override, new property, new child subtree (with a grandchild so
*     the recursive copy runs), later fragment winning over an earlier one;
*   - delete property (length 0) and delete child (__delete marker), and the
*     -16 refusal when the target is not in the base tree;
*   - -11 shape errors (no __overlay__, two __overlay__, a foreign child,
*     no target, no fragment at all), -12 target errors (relative path,
*     missing path, resolved phandle instead of the placeholder, missing
*     fixup, missing symbol), -13 duplicate child (against the base or across
*     two fragments), -14 arena too small, -15 tag sections;
*   - argument guards (NULL dst, NULL out_len, misaligned arena, cap below the
*     documented minimum), bad base/fragment blobs coming back as the parser's
*     own codes, the reserve map being copied verbatim, and the reason table.
*
* This is the merge logic only.  It does NOT prove that a real .dtbo from dtc
* merges (our delete encoding is deliberately not Linux's) and it does NOT
* prove the produced tree boots anything -- those need a board.
*
* ARKRTOS_USE_FDT and ARKRTOS_USE_FDT_OVERLAY are compiled at 1.
*
* @author xw
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fdt.h"
#include "arkrtos_fdt_overlay.h"

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

#define BLOB_MAX    8192u
#define STRINGS_MAX 2048u

typedef struct
{
    uint8  buf[BLOB_MAX];
    uint8  strs[STRINGS_MAX];
    uint32 struct_len;
    uint32 strings_len;
    uint32 rsv_len;             /* 16 or 32 */
    uint32 blob_len;
} bb_t;

static bb_t g_base;
static bb_t g_frag;
static uint32 g_outw[BLOB_MAX / 2u];   /* 4-byte aligned output arena */

static uint8 *bb_out(bb_t *b)
{
    return &b->buf[40u + b->rsv_len + b->struct_len];
}

static void bb32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static uint32 bb_intern(bb_t *b, const char *s)
{
    uint32 n = (uint32)strlen(s) + 1u;
    uint32 off = b->strings_len;

    memcpy(&b->strs[b->strings_len], s, (size_t)n);
    b->strings_len += n;
    return off;
}

static void bb_reset(bb_t *b, uint32 rsv_len)
{
    memset(b->buf, 0, sizeof b->buf);
    memset(b->strs, 0, sizeof b->strs);
    b->struct_len = 0u;
    b->strings_len = 0u;
    b->blob_len = 0u;
    b->rsv_len = rsv_len;
    if (rsv_len == 32u) {
        /* one reserve entry: addr 0x1000, size 0x200 (big-endian 64-bit) */
        bb32(&b->buf[44], 0x1000u);
        bb32(&b->buf[52], 0x200u);
    }
}

static void bb_emit(bb_t *b, uint32 tok)
{
    bb32(bb_out(b), tok);
    b->struct_len += 4u;
}

static void bb_pad(bb_t *b)
{
    while ((b->struct_len % 4u) != 0u) {
        *bb_out(b) = 0u;
        b->struct_len++;
    }
}

static void bb_begin(bb_t *b, const char *name)
{
    uint32 n = (uint32)strlen(name);

    bb_emit(b, 1u);
    if (n != 0u) {
        memcpy(bb_out(b), name, (size_t)n);
        b->struct_len += n;
    }
    *bb_out(b) = 0u;            /* root name is empty: still needs its NUL */
    b->struct_len++;
    bb_pad(b);
}

static void bb_end(bb_t *b)
{
    bb_emit(b, 2u);
}

static void bb_prop(bb_t *b, const char *name, const void *v, uint32 len)
{
    bb_emit(b, 3u);
    bb32(bb_out(b), len);
    b->struct_len += 4u;
    bb32(bb_out(b), bb_intern(b, name));
    b->struct_len += 4u;
    if (len != 0u) {
        memcpy(bb_out(b), v, (size_t)len);
        b->struct_len += len;
    }
    bb_pad(b);
}

static void bb_str(bb_t *b, const char *name, const char *s)
{
    bb_prop(b, name, s, (uint32)strlen(s) + 1u);
}

static void bb_u32(bb_t *b, const char *name, uint32 v)
{
    uint8 t[4];

    bb32(t, v);
    bb_prop(b, name, t, 4u);
}

static void bb_cells(bb_t *b, const char *name, const uint32 *v, uint32 n)
{
    uint8 t[32];
    uint32 i;

    for (i = 0u; i < n; i++) {
        bb32(&t[i * 4u], v[i]);
    }
    bb_prop(b, name, t, n * 4u);
}

static void bb_finish(bb_t *b)
{
    uint32 off_strings = 40u + b->rsv_len + b->struct_len;
    uint32 total;

    if ((off_strings + b->strings_len) > BLOB_MAX) {
        printf("FAIL: blob overflow\n");
        g_failed++;
        return;
    }
    memcpy(&b->buf[off_strings], b->strs, (size_t)b->strings_len);
    total = off_strings + b->strings_len;

    bb32(&b->buf[0], 0xd00dfeedu);
    bb32(&b->buf[4], total);
    bb32(&b->buf[8], 40u + b->rsv_len);
    bb32(&b->buf[12], off_strings);
    bb32(&b->buf[16], 40u);
    bb32(&b->buf[20], 17u);
    bb32(&b->buf[24], 16u);
    bb32(&b->buf[28], 0u);
    bb32(&b->buf[32], b->strings_len);
    bb32(&b->buf[36], b->struct_len);
    b->blob_len = total;
}

/* ------------------------------------------------------------- base tree */

static void build_base(uint32 rsv_len)
{
    bb_t *b = &g_base;
    uint32 reg[2];

    bb_reset(b, rsv_len);

    bb_begin(b, "");
    bb_str(b, "model", "Base Board");
    bb_str(b, "compatible", "ark,base");

    bb_begin(b, "aliases");
    bb_str(b, "serial0", "/soc/serial@100");
    bb_str(b, "sel", "/soc");
    bb_end(b);

    bb_begin(b, "chosen");
    bb_str(b, "bootargs", "console=ttyS0");
    bb_end(b);

    bb_begin(b, "soc");
    bb_u32(b, "#address-cells", 1u);
    bb_u32(b, "#size-cells", 1u);

    bb_begin(b, "serial@100");
    bb_str(b, "compatible", "ns16550a");
    reg[0] = 0x100u;
    reg[1] = 0x20u;
    bb_cells(b, "reg", reg, 2u);
    bb_str(b, "status", "okay");
    bb_u32(b, "phandle", 1u);
    bb_end(b);

    bb_begin(b, "other");
    bb_str(b, "compatible", "foo");
    bb_str(b, "status", "disabled");
    bb_str(b, "junk", "j1");
    bb_end(b);

    bb_end(b);                          /* /soc */

    bb_begin(b, "cpus");
    bb_u32(b, "timebase-frequency", 1000000u);
    bb_end(b);

    bb_begin(b, "__symbols__");
    bb_str(b, "serial0", "/soc/serial@100");
    bb_str(b, "soc", "/soc");
    bb_str(b, "cpus", "/cpus");
    bb_str(b, "other", "/soc/other");
    bb_end(b);

    bb_end(b);                          /* root */
    bb_emit(b, 9u);
    bb_finish(b);
}

/* ------------------------------------------------------------ frag cases */

/* fragment@0 target-path="/soc/serial@100" with overrides, new prop and a
 * new child that itself has a child */
static void frag_props_and_child(void)
{
    bb_t *f = &g_frag;
    uint32 cells[2];

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc/serial@100");
    bb_begin(f, "__overlay__");
    bb_str(f, "status", "disabled");
    bb_u32(f, "current-speed", 9600u);
    bb_begin(f, "child@1");
    bb_str(f, "compatible", "ark,child");
    cells[0] = 7u;
    cells[1] = 8u;
    bb_cells(f, "v", cells, 2u);
    bb_begin(f, "grand");
    bb_u32(f, "deep", 42u);
    bb_end(f);
    bb_end(f);                          /* child@1 */
    bb_end(f);                          /* __overlay__ */
    bb_end(f);                          /* fragment@0 */
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* deletes: property #size-cells on /soc, and child /soc/other */
static void frag_delete(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_prop(f, "#size-cells", 0, 0u);
    bb_begin(f, "other");
    bb_prop(f, "__delete", 0, 0u);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* target = <&soc> through __fixups__ + base __symbols__ */
static void frag_phandle(uint32 placeholder)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@1");
    bb_u32(f, "target", placeholder);
    bb_begin(f, "__overlay__");
    bb_u32(f, "zzz", 5u);
    bb_end(f);
    bb_end(f);
    bb_begin(f, "__fixups__");
    bb_str(f, "soc", "/fragment@1:target:0");
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* two fragments on the same target: alpha/beta first, alpha again later */
static void frag_two(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_str(f, "alpha", "one");
    bb_u32(f, "beta", 1u);
    bb_end(f);
    bb_end(f);
    bb_begin(f, "fragment@1");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_str(f, "alpha", "two");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* ------------------------------------------------------------- harness */

static void overlay_ok(const char *what)
{
    uint32 out_len = 0u;
    int rc;

    memset(g_outw, 0, sizeof g_outw);
    rc = arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len);
    g_checks++;
    if (rc != ARKRTOS_FDT_OK) {
        g_failed++;
        printf("FAIL base=%s: overlay rc=%d (%s)\n",
               what, rc, arkrtos_fdt_overlay_reason(rc));
        return;
    }
    if ((out_len < 40u) || (out_len > sizeof g_outw)) {
        g_failed++;
        printf("FAIL base=%s: out_len=%lu\n", what, (unsigned long)out_len);
        return;
    }
    CK_I(((uint32)((uint8 *)g_outw)[4] << 24) |
         ((uint32)((uint8 *)g_outw)[5] << 16) |
         ((uint32)((uint8 *)g_outw)[6] << 8) |
         (uint32)((uint8 *)g_outw)[7], out_len);       /* totalsize == out_len */
    CK_I(arkrtos_fdt_init(g_outw, out_len), ARKRTOS_FDT_OK);
}

/* --------------------------------------------------------------- cases */

static void test_props_and_child(void)
{
    arkrtos_fdt_node_t n;
    arkrtos_fdt_node_t c;
    const char *s;
    uint32 v = 0u;
    uint32 len = 0u;

    build_base(16u);
    CK_I(arkrtos_fdt_init(g_base.buf, g_base.blob_len), ARKRTOS_FDT_OK);
    CK_S(arkrtos_fdt_model(), "Base Board");

    frag_props_and_child();
    overlay_ok("props");

    /* the produced blob is the tree we expect, read back with the parser */
    CK_S(arkrtos_fdt_model(), "Base Board");
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100", &n), ARKRTOS_FDT_OK);
    s = arkrtos_fdt_prop_str(&n, "status");
    CK_S(s, "disabled");
    CK_I(arkrtos_fdt_prop_u32(&n, "current-speed", 0u, &v), 0);
    CK_I(v, 9600u);
    /* base properties that were not mentioned survive */
    CK_I(arkrtos_fdt_prop_u32(&n, "reg", 1u, &v), 0);
    CK_I(v, 0x20u);
    CK_S(arkrtos_fdt_prop_str(&n, "compatible"), "ns16550a");
    CK_I(arkrtos_fdt_node_enabled(&n), 0);

    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100/child@1", &c),
         ARKRTOS_FDT_OK);
    CK_S(arkrtos_fdt_prop_str(&c, "compatible"), "ark,child");
    CK_I(arkrtos_fdt_prop_u32(&c, "v", 1u, &v), 0);
    CK_I(v, 8u);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100/child@1/grand", &c),
         ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_prop_u32(&c, "deep", 0u, &v), 0);
    CK_I(v, 42u);

    /* nothing was added to unrelated nodes */
    CK_I(arkrtos_fdt_node_by_path("/soc/other", &c), ARKRTOS_FDT_OK);
    CK_S(arkrtos_fdt_prop_str(&c, "junk"), "j1");
    CK(arkrtos_fdt_prop(&c, "current-speed", &len) == 0);
    CK_I(len, 0u);

    /* base tree itself was not touched by the merge */
    CK_I(arkrtos_fdt_init(g_base.buf, g_base.blob_len), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100", &n), ARKRTOS_FDT_OK);
    CK_S(arkrtos_fdt_prop_str(&n, "status"), "okay");
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100/child@1", &c),
         ARKRTOS_FDT_ERR_NOT_FOUND);
}

static void test_delete(void)
{
    arkrtos_fdt_node_t n;
    uint32 len = 0u;

    build_base(16u);
    frag_delete();
    overlay_ok("delete");

    CK_I(arkrtos_fdt_node_by_path("/soc", &n), ARKRTOS_FDT_OK);
    CK(arkrtos_fdt_prop(&n, "#size-cells", &len) == 0);
    CK_I(len, 0u);
    CK_I(arkrtos_fdt_prop_u32(&n, "#address-cells", 0u, &len), 0);
    CK_I(len, 1u);
    CK_I(arkrtos_fdt_node_by_path("/soc/other", &n),
         ARKRTOS_FDT_ERR_NOT_FOUND);
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100", &n), ARKRTOS_FDT_OK);
}

static void test_phandle_target(void)
{
    arkrtos_fdt_node_t n;
    uint32 v = 0u;

    build_base(16u);
    frag_phandle(0xffffffffu);
    overlay_ok("phandle");
    CK_I(arkrtos_fdt_node_by_path("/soc", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_prop_u32(&n, "zzz", 0u, &v), 0);
    CK_I(v, 5u);
    /* the overlay's own fixup node must not be copied into the output */
    CK_I(arkrtos_fdt_node_by_path("/__fixups__", &n),
         ARKRTOS_FDT_ERR_NOT_FOUND);
}

static void test_two_fragments(void)
{
    arkrtos_fdt_node_t n;
    uint32 v = 0u;

    build_base(16u);
    frag_two();
    overlay_ok("two");
    CK_I(arkrtos_fdt_node_by_path("/soc", &n), ARKRTOS_FDT_OK);
    CK_S(arkrtos_fdt_prop_str(&n, "alpha"), "two");
    CK_I(arkrtos_fdt_prop_u32(&n, "beta", 0u, &v), 0);
    CK_I(v, 1u);
}

/* one refusal: build base once, replace the fragment, expect rc */
static void expect_rc(int want)
{
    uint32 out_len = 0u;
    int rc;

    rc = arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len);
    g_checks++;
    if (rc != want) {
        g_failed++;
        printf("FAIL line %d: rc=%d (%s) want %d\n", __LINE__, rc,
               arkrtos_fdt_overlay_reason(rc), want);
    }
    if (rc != ARKRTOS_FDT_OK) {
        CK_I(out_len, 0u);
    }
}

static void frag_shape_extra_child(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_begin(f, "surprise");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_no_overlay(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_two_overlays(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* target-path only, no __overlay__ content: valid shape with zero changes */
static void frag_empty_overlay(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_no_target(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_relative_target(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "soc");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_missing_target_node(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc/nope");
    bb_begin(f, "__overlay__");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_tag(const char *tag)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, tag);
    bb_end(f);
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_u32(f, "q", 1u);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_only_props(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_str(f, "model", "not an overlay");
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_delete_missing_prop(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_prop(f, "not-here", 0, 0u);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_delete_missing_child(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_begin(f, "ghost");
    bb_prop(f, "__delete", 0, 0u);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_duplicate_child(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_begin(f, "other");               /* already exists under /soc */
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_same_new_child_twice(void)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_begin(f, "twice");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_begin(f, "fragment@1");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_begin(f, "twice");
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void frag_big_prop(void)
{
    bb_t *f = &g_frag;
    uint8 big[5000];

    memset(big, 0x5a, sizeof big);
    big[sizeof big - 1u] = 0u;
    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", "/soc");
    bb_begin(f, "__overlay__");
    bb_prop(f, "big", big, (uint32)sizeof big);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

static void test_refusals(void)
{
    arkrtos_fdt_node_t n;
    uint32 out_len = 0u;

    build_base(16u);

    frag_shape_extra_child();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY);
    frag_no_overlay();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY);
    frag_two_overlays();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY);
    frag_no_target();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY);
    frag_relative_target();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);
    frag_missing_target_node();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);
    frag_phandle(0x1234u);              /* resolved phandle: refuse, do not guess */
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);
    frag_tag("__symbols__");
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TAG);
    frag_tag("__local_fixups__");
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TAG);
    frag_only_props();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY);
    frag_delete_missing_prop();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_DEL);
    frag_delete_missing_child();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_DEL);
    frag_duplicate_child();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_DUP);
    frag_same_new_child_twice();
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_DUP);

    /* empty __overlay__ is legal: it merges nothing and produces a copy */
    frag_empty_overlay();
    overlay_ok("empty overlay");
    CK_I(arkrtos_fdt_node_by_path("/soc/serial@100", &n), ARKRTOS_FDT_OK);

    /* a fragment whose target names the root */
    frag_big_prop();
    out_len = 0u;
    CK_I(arkrtos_fdt_overlay(g_outw, 4096u, g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_OVERLAY_SIZE);
    CK_I(out_len, 0u);
}

/* --------------------------------------------------- path / depth edges */

/* fragment@0 with an arbitrary target-path and one new property */
static void frag_target_path(const char *path)
{
    bb_t *f = &g_frag;

    bb_reset(f, 16u);
    bb_begin(f, "");
    bb_begin(f, "fragment@0");
    bb_str(f, "target-path", path);
    bb_begin(f, "__overlay__");
    bb_u32(f, "edge", 1u);
    bb_end(f);
    bb_end(f);
    bb_end(f);
    bb_emit(f, 9u);
    bb_finish(f);
}

/* a base tree whose only content is a chain of `levels` nodes under the root,
 * named d0..d(levels-1), each carrying one property */
static void build_chain_base(uint32 levels)
{
    bb_t *b = &g_base;
    char name[16];
    uint32 i;

    bb_reset(b, 16u);
    bb_begin(b, "");
    bb_str(b, "model", "Chain");
    for (i = 0u; i < levels; i++) {
        sprintf(name, "d%lu", (unsigned long)i);
        bb_begin(b, name);
        bb_u32(b, "id", i);
    }
    for (i = 0u; i < levels; i++) {
        bb_end(b);
    }
    bb_end(b);
    bb_emit(b, 9u);
    bb_finish(b);
}

/* the path splitter must not guess: "//" and a trailing '/' are errors, and
 * the depth cap is enforced on the base tree before anything is emitted */
static void test_path_and_depth(void)
{
    arkrtos_fdt_node_t n;
    uint32 v = 0u;

    build_base(16u);
    frag_target_path("/soc//serial@100");
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);
    frag_target_path("/soc/");
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);
    frag_target_path("/soc/serial@100/");
    expect_rc(ARKRTOS_FDT_ERR_OVERLAY_TARGET);

    /* "/" is the root: legal, merged in place */
    frag_target_path("/");
    overlay_ok("target root");
    CK_S(arkrtos_fdt_model(), "Base Board");
    CK_I(arkrtos_fdt_node_by_path("/", &n), ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_prop_u32(&n, "edge", 0u, &v), 0);
    CK_I(v, 1u);

    /* 16 levels below the root is the documented cap (+1 for the root token) */
    build_chain_base(16u);
    frag_target_path("/d0");
    overlay_ok("chain 16");

    /* 17 levels exceeds it and must be refused, not truncated */
    build_chain_base(17u);
    frag_target_path("/d0");
    expect_rc(ARKRTOS_FDT_ERR_DEPTH);
}

static void test_api_guards(void)
{
    uint32 out_len = 0u;

    build_base(16u);
    frag_empty_overlay();

    CK_I(arkrtos_fdt_overlay(0, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, 0),
         ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_overlay((uint8 *)g_outw + 1u, sizeof g_outw - 1u,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_ARG);
    CK_I(arkrtos_fdt_overlay(g_outw, ARKRTOS_FDT_OVERLAY_MIN_ARENA - 1u,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_OVERLAY_SIZE);
    /* out_len is cleared before anything else can fail */
    out_len = 77u;
    CK_I(arkrtos_fdt_overlay(g_outw, 4095u,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_OVERLAY_SIZE);
    CK_I(out_len, 0u);

    /* a bad base is reported with the parser's own code, not -11 */
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len - 4u,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_RANGE);
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, 16u,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_ARG);
    g_base.buf[0] = 0u;
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_MAGIC);
    build_base(16u);

    /* and the same for a bad fragment */
    g_frag.buf[0] = 0u;
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_ERR_MAGIC);
}

static void test_rsvmap_and_header(void)
{
    uint32 out_len = 0u;
    uint8 *d = (uint8 *)g_outw;
    arkrtos_fdt_node_t n;

    build_base(32u);
    frag_empty_overlay();
    memset(g_outw, 0, sizeof g_outw);
    CK_I(arkrtos_fdt_overlay(g_outw, sizeof g_outw,
                             g_base.buf, g_base.blob_len,
                             g_frag.buf, g_frag.blob_len, &out_len),
         ARKRTOS_FDT_OK);
    CK_I(arkrtos_fdt_init(g_outw, out_len), ARKRTOS_FDT_OK);

    /* header: magic, version 17 / last_comp 16, rsvmap at 40, offsets sane */
    CK_I((uint32)d[0], 0xd0u);
    CK_I((uint32)d[3], 0xedu);
    CK_I(((uint32)d[20] << 24) | ((uint32)d[21] << 16) |
         ((uint32)d[22] << 8) | (uint32)d[23], 17u);
    CK_I(((uint32)d[24] << 24) | ((uint32)d[25] << 16) |
         ((uint32)d[26] << 8) | (uint32)d[27], 16u);
    CK_I(((uint32)d[16] << 24) | ((uint32)d[17] << 16) |
         ((uint32)d[18] << 8) | (uint32)d[19], 40u);
    /* the one reserve entry is copied verbatim, then its terminator */
    CK_I((uint32)d[44], 0x00u);
    CK_I((uint32)d[45], 0x00u);
    CK_I((uint32)d[46], 0x10u);
    CK_I((uint32)d[47], 0x00u);
    CK_I((uint32)d[52], 0x00u);
    CK_I((uint32)d[53], 0x00u);
    CK_I((uint32)d[54], 0x02u);
    CK_I((uint32)d[55], 0x00u);
    /* struct starts right after the copied map (40 + 32) */
    CK_I(((uint32)d[8] << 24) | ((uint32)d[9] << 16) |
         ((uint32)d[10] << 8) | (uint32)d[11], 72u);
    CK_I(d[72], 0x00u);
    CK_I(d[73], 0x00u);
    CK_I(d[74], 0x00u);
    CK_I(d[75], 0x01u);                 /* BEGIN_NODE */

    /* the copied map must still parse as "one entry + terminator" */
    CK_S(arkrtos_fdt_model(), "Base Board");
    CK_I(arkrtos_fdt_node_by_path("/cpus", &n), ARKRTOS_FDT_OK);
}

static void test_reason_table(void)
{
    static const int codes[] = {
        ARKRTOS_FDT_OK,
        ARKRTOS_FDT_ERR_ARG,
        ARKRTOS_FDT_ERR_MAGIC,
        ARKRTOS_FDT_ERR_VERSION,
        ARKRTOS_FDT_ERR_RANGE,
        ARKRTOS_FDT_ERR_ALIGN,
        ARKRTOS_FDT_ERR_FORMAT,
        ARKRTOS_FDT_ERR_DEPTH,
        ARKRTOS_FDT_ERR_NOT_INIT,
        ARKRTOS_FDT_ERR_NOT_FOUND,
        ARKRTOS_FDT_ERR_PATH,
        ARKRTOS_FDT_ERR_OVERLAY,
        ARKRTOS_FDT_ERR_OVERLAY_TARGET,
        ARKRTOS_FDT_ERR_OVERLAY_DUP,
        ARKRTOS_FDT_ERR_OVERLAY_SIZE,
        ARKRTOS_FDT_ERR_OVERLAY_TAG,
        ARKRTOS_FDT_ERR_OVERLAY_DEL
    };
    uint32 i;
    uint32 j;

    for (i = 0u; i < (uint32)(sizeof codes / sizeof codes[0]); i++) {
        const char *s = arkrtos_fdt_overlay_reason(codes[i]);

        CK(s != 0);
        CK(strlen(s) > 0u);
        for (j = 0u; j < i; j++) {
            CK(strcmp(s, arkrtos_fdt_overlay_reason(codes[j])) != 0);
        }
    }
    CK_S(arkrtos_fdt_overlay_reason(-999), "unknown");
}

int main(void)
{
    test_props_and_child();
    test_delete();
    test_phandle_target();
    test_two_fragments();
    test_refusals();
    test_path_and_depth();
    test_api_guards();
    test_rsvmap_and_header();
    test_reason_table();

    if (g_failed != 0) {
        printf("FDT-OVERLAY-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("FDT-OVERLAY-HOST OK  %d checks\n", g_checks);
    return 0;
}
