/**
* @file test_fdt.c
* @brief Host self-test: the DTB reader must answer what the DTS said.
* @details The C table (board/<b>/arkrtos_dt.c) and the flattened blob are two
*          outputs of the same parse.  This test treats them as independent
*          witnesses: it links the generated table and the FDT reader into one
*          program, parses the .dtb, and asserts node for node that name,
*          compatible, reg and enabled agree - plus that the node order and the
*          root model agree.
*
*          It also proves the reader refuses damaged blobs instead of reading
*          past the end: a wrong magic, a truncated total size and a struct
*          block that stops before FDT_END must each come back as an error.
*
*          Build/run through tools/dts/test_fdt.py (it generates the .dtb).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkrtos_dt.h"
#include "arkrtos_fdt.h"
#include "arkrtos_fdt_console.h"
#include "arkrtos_features.h"
#if (ARKRTOS_USE_FDT_OVERLAY == 1)
#include "arkrtos_fdt_overlay.h"
#endif

static int g_fail;

static void check(int cond, const char *what)
{
    if(!cond)
    {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static unsigned char *load(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if(f == NULL)
    {
        printf("FAIL: cannot open %s\n", path);
        g_fail++;
        return NULL;
    }
    if(fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if((buf == NULL) || (fread(buf, 1, (size_t)n, f) != (size_t)n))
    {
        printf("FAIL: cannot read %s\n", path);
        fclose(f);
        free(buf);
        g_fail++;
        return NULL;
    }
    fclose(f);
    *len = n;
    return buf;
}

static int same(const char *a, const char *b)
{
    if((a == NULL) || (b == NULL))
    {
        return (a == b) ? 1 : 0;
    }
    return (strcmp(a, b) == 0) ? 1 : 0;
}


#if (ARKRTOS_USE_FDT_OVERLAY == 1)
/* ============================ F-1c: overlay ============================
 * 这一段的读数是"把 fragment 合并进一棵真树，产出的新 blob 能被解析器
 * 完整读回来"。它不是"overlay 语法全支持"的证明：本子集只认
 * target-path、只做属性覆盖与子节点追加，其余一律拒绝。所以下面既验
 * 成功路径，也逐条验拒绝路径必须报出**具体**错误码，而不是"随便失败"。
 *
 * 负面用例用这里的小构造器现造 blob，而不是再加 dts 文件：要覆盖的
 * 形状（target = <&label>、__symbols__、同名子节点、坏魔数）在 DTS 里
 * 要么写不出来（dtc_lite 会替我们整理成另一种形状），要么写出来也不
 * 保证是我们想要的字节。
 */

#define BLD_SD 1024u
#define BLD_ST 512u
#define BLD_OUT 2048u

typedef struct
{
    unsigned char sd[BLD_SD];
    unsigned      sn;
    char          st[BLD_ST];
    unsigned      stn;
    unsigned char out[BLD_OUT];
} bld_t;

static void bld_init(bld_t *b)
{
    memset(b, 0, sizeof(*b));
    b->st[0] = '\0';
    b->stn = 1u;
}

static void bld_o32(unsigned char *p, uint32 v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void bld_w32(bld_t *b, uint32 v)
{
    bld_o32(b->sd + b->sn, v);
    b->sn += 4u;
}

static void bld_align(bld_t *b)
{
    while((b->sn & 3u) != 0u)
    {
        b->sd[b->sn++] = 0u;
    }
}

static uint32 bld_str(bld_t *b, const char *s)
{
    unsigned i = 0u;
    size_t n = strlen(s);

    while(i < b->stn)
    {
        if(strcmp(b->st + i, s) == 0)
        {
            return (uint32)i;
        }
        i += (unsigned)strlen(b->st + i) + 1u;
    }
    if((b->stn + n + 1u) > BLD_ST)
    {
        return 0u;
    }
    memcpy(b->st + b->stn, s, n + 1u);
    i = b->stn;
    b->stn += (unsigned)n + 1u;
    return (uint32)i;
}

static void bld_begin(bld_t *b, const char *name)
{
    size_t n = strlen(name);

    bld_w32(b, 1u);
    memcpy(b->sd + b->sn, name, n + 1u);
    b->sn += (unsigned)n + 1u;
    bld_align(b);
}

static void bld_end(bld_t *b)
{
    bld_w32(b, 2u);
}

static void bld_prop(bld_t *b, const char *name, const void *val, uint32 len)
{
    bld_w32(b, 3u);
    bld_w32(b, len);
    bld_w32(b, bld_str(b, name));
    memcpy(b->sd + b->sn, val, len);
    b->sn += len;
    bld_align(b);
}

static void bld_pstr(bld_t *b, const char *name, const char *val)
{
    bld_prop(b, name, val, (uint32)strlen(val) + 1u);
}

static void bld_pu32(bld_t *b, const char *name, uint32 v)
{
    unsigned char c[4];

    bld_o32(c, v);
    bld_prop(b, name, c, 4u);
}

static uint32 bld_finish(bld_t *b)
{
    uint32 off_struct = 56u;                 /* header 40 + rsvmap 16 */
    uint32 off_strings;
    uint32 total;

    bld_w32(b, 9u);                          /* FDT_END */
    off_strings = off_struct + b->sn;
    total = off_strings + b->stn;
    if(total > BLD_OUT)
    {
        return 0u;
    }
    memset(b->out, 0, (size_t)total);
    bld_o32(b->out + 0u, 0xd00dfeedu);
    bld_o32(b->out + 4u, total);
    bld_o32(b->out + 8u, off_struct);
    bld_o32(b->out + 12u, off_strings);
    bld_o32(b->out + 16u, 40u);              /* off_mem_rsvmap */
    bld_o32(b->out + 20u, 17u);              /* version */
    bld_o32(b->out + 24u, 16u);              /* last_comp_version */
    bld_o32(b->out + 28u, 0u);               /* boot_cpuid_phys */
    bld_o32(b->out + 32u, b->stn);           /* size_dt_strings */
    bld_o32(b->out + 36u, b->sn);            /* size_dt_struct */
    memcpy(b->out + off_struct, b->sd, b->sn);
    memcpy(b->out + off_strings, b->st, b->stn);
    return total;
}

/* 一个 fragment@0：target-path + __overlay__ { 一个属性, 可选一个子节点 }
 * 负面用例只换其中一个参数——这样"报错"就能归因到那一处不同。 */
static uint32 bld_frag(bld_t *b, const char *target_path,
                       const char *prop_name, const char *prop_val,
                       const char *child_name, int with_symbols)
{
    bld_init(b);
    bld_begin(b, "");
    bld_begin(b, "fragment@0");
    bld_pstr(b, "target-path", target_path);
    bld_begin(b, "__overlay__");
    if(prop_name != NULL)
    {
        bld_pstr(b, prop_name, prop_val);
    }
    if(child_name != NULL)
    {
        bld_begin(b, child_name);
        bld_pstr(b, "compatible", "ark-overlay-demo");
        bld_pu32(b, "overlay-value", 0x2au);
        bld_end(b);
    }
    bld_end(b);                              /* __overlay__ */
    bld_end(b);                              /* fragment@0 */
    if(with_symbols != 0)
    {
        bld_begin(b, "__symbols__");
        bld_pstr(b, "demo", "/");
        bld_end(b);
    }
    bld_end(b);                              /* root */
    return bld_finish(b);
}
#endif /* ARKRTOS_USE_FDT_OVERLAY */

int main(int argc, char **argv)
{
    long len = 0;
    unsigned char *blob;
    unsigned char *copy;
    arkrtos_fdt_iter_t it;
    arkrtos_fdt_node_t node;
    uint32 i = 0u;
    uint32 total;
    int rc;

    if(argc < 2)
    {
        printf("usage: %s <dtb>\n", argv[0]);
        return 2;
    }
    blob = load(argv[1], &len);
    if(blob == NULL)
    {
        return 1;
    }

    /* ---- negative cases first: a reader that accepts anything proves nothing */
    copy = (unsigned char *)malloc((size_t)len);
    if(copy == NULL)
    {
        free(blob);
        return 1;
    }
    memcpy(copy, blob, (size_t)len);

    copy[0] ^= 0xffu;                              /* magic */
    check(arkrtos_fdt_init(copy, (uint32)len) == ARKRTOS_FDT_ERR_MAGIC,
          "wrong magic refused");
    memcpy(copy, blob, (size_t)len);

    copy[6] = 0x7fu;                               /* totalsize: far past the file */
    copy[7] = 0xffu;
    check(arkrtos_fdt_init(copy, (uint32)len) == ARKRTOS_FDT_ERR_RANGE,
          "totalsize past the buffer refused");
    memcpy(copy, blob, (size_t)len);

    check(arkrtos_fdt_init(copy, 8u) == ARKRTOS_FDT_ERR_ARG,
          "short buffer refused");
    check(arkrtos_fdt_init(NULL, (uint32)len) == ARKRTOS_FDT_ERR_ARG,
          "null blob refused");

    /* A struct block that ends without FDT_END: cut size_struct by one word.
     * The header stays self-consistent, so only a real walk finds it. */
    {
        unsigned char *hdr = copy;
        memcpy(hdr, blob, (size_t)len);
        hdr[39] ^= 0x10u;                          /* size_dt_struct low bits */
        rc = arkrtos_fdt_init(hdr, (uint32)len);
        check(rc != ARKRTOS_FDT_OK, "struct block without FDT_END refused");
    }
    free(copy);

    /* ---- the real parse */
    rc = arkrtos_fdt_init(blob, (uint32)len);
    if(rc != ARKRTOS_FDT_OK)
    {
        printf("FAIL: init on a valid blob returned %d\n", rc);
        free(blob);
        return 1;
    }

    total = arkrtos_fdt_node_count();
    check(total == arkrtos_dt_count(), "node count matches the C table");
    check(same(arkrtos_fdt_model(), arkrtos_dt_model()),
          "root model matches the C table");

    rc = arkrtos_fdt_iter_init(&it);
    check(rc == ARKRTOS_FDT_OK, "iterator starts");

    while((rc = arkrtos_fdt_next(&it, &node)) == 1)
    {
        const arkrtos_dt_node_t *n = (i < arkrtos_dt_count())
                                     ? arkrtos_dt_node(i) : NULL;
        uint32 reg0 = 0u;
        uint32 reg1 = 0u;
        const char *compat;
        char what[128];

        if(n == NULL)
        {
            printf("FAIL: blob has a node the table does not\n");
            g_fail++;
            break;
        }
        snprintf(what, sizeof(what), "node %u name", (unsigned)i);
        check(same(node.name, n->name), what);

        compat = arkrtos_fdt_prop_str(&node, "compatible");
        snprintf(what, sizeof(what), "node %u compatible", (unsigned)i);
        check(same(compat, n->compatible), what);

        (void)arkrtos_fdt_prop_u32(&node, "reg", 0u, &reg0);
        (void)arkrtos_fdt_prop_u32(&node, "reg", 1u, &reg1);
        snprintf(what, sizeof(what), "node %u reg", (unsigned)i);
        check((reg0 == n->reg_base) && (reg1 == n->reg_size), what);

        snprintf(what, sizeof(what), "node %u enabled", (unsigned)i);
        check(arkrtos_fdt_node_enabled(&node) == (int)n->status_okay, what);

        snprintf(what, sizeof(what), "node %u property count", (unsigned)i);
        check(node.n_props == (uint32)n->n_props, what);

        i++;
    }
    check(rc == 0, "iterator ends cleanly");
    check(i == total, "blob yields exactly the counted nodes");

    /* ---- F-1a: path / phandle / alias round-trips ----
     * 这三条是「按路径查设备」的全部地基：路径解析要是多一层或少一层，
     * 上层看到的永远是"查不到设备"，而查不到的原因会被误读成"板子没这个外设"。
     */
    {
        arkrtos_fdt_node_t root;
        char abuf[ARKRTOS_FDT_PATH_MAX];

        check(arkrtos_fdt_node_by_path("/", &root) == ARKRTOS_FDT_OK,
              "node_by_path(/) finds the root");
        check(same(root.name, ""), "root node name is empty");
        check(arkrtos_fdt_node_by_path("/no-such-node", NULL)
              == ARKRTOS_FDT_ERR_NOT_FOUND,
              "node_by_path refuses a missing node");
        check(arkrtos_fdt_node_by_path("chosen", NULL) == ARKRTOS_FDT_ERR_PATH,
              "node_by_path refuses a relative path");
        check(arkrtos_fdt_node_by_path("/chosen", NULL) == ARKRTOS_FDT_OK,
              "node_by_path(/chosen) finds the chosen node");

        /* 每块板都有 /aliases 的话就应该能查到；没有就只认"查不到"。
         * 两种结果都是真话，唯独"报成功却指向不存在的节点"不是。 */
        rc = arkrtos_fdt_alias("serial0", abuf, (uint32)sizeof(abuf));
        check((rc == ARKRTOS_FDT_OK) || (rc == ARKRTOS_FDT_ERR_NOT_FOUND),
              "alias lookup answers ok or not-found, never garbage");
        if(rc == ARKRTOS_FDT_OK)
        {
            check(arkrtos_fdt_node_by_path(abuf, NULL) == ARKRTOS_FDT_OK,
                  "alias serial0 points at a node that exists");
        }
        check(arkrtos_fdt_alias("no-such-alias", abuf, (uint32)sizeof(abuf))
              == ARKRTOS_FDT_ERR_NOT_FOUND,
              "unknown alias is not-found");
    }

    /* phandle -> 路径 -> 节点，必须回到同一个名字 */
    {
        uint32 n_ph = 0u;
        rc = arkrtos_fdt_iter_init(&it);
        while((rc = arkrtos_fdt_next(&it, &node)) == 1)
        {
            uint32 ph = 0u;
            char   pb[ARKRTOS_FDT_PATH_MAX];
            arkrtos_fdt_node_t back;

            if(arkrtos_fdt_prop_u32(&node, "phandle", 0u, &ph) != ARKRTOS_FDT_OK)
            {
                continue;
            }
            n_ph++;
            check(arkrtos_fdt_path_of_phandle(ph, pb, (uint32)sizeof(pb))
                  == ARKRTOS_FDT_OK, "phandle resolves to a path");
            check(arkrtos_fdt_node_by_path(pb, &back) == ARKRTOS_FDT_OK,
                  "that path names an existing node");
            check(same(back.name, node.name),
                  "phandle round-trip returns the same node");
        }
        check(n_ph > 0u, "the blob carries at least one phandle");
        {
            char pb[ARKRTOS_FDT_PATH_MAX];

            check(arkrtos_fdt_path_of_phandle(0xdeadbeefu, pb,
                                              (uint32)sizeof(pb)) < 0,
                  "unknown phandle is refused");
        }
    }

    /* ---- F-1a: load() 的事务性——坏 blob 不许毁掉上一棵树 ---- */
    {
        unsigned char *copy2;
        uint32 before = arkrtos_fdt_node_count();
        const char *model_before = arkrtos_fdt_model();

        check(arkrtos_fdt_load((void *)blob, (uint32)len - 1u, blob, (uint32)len)
              == ARKRTOS_FDT_ERR_ARG, "load refuses a destination that is too small");

        copy2 = (unsigned char *)malloc((size_t)len);
        check(copy2 != NULL, "load test buffer allocated");
        if(copy2 != NULL)
        {
            check(arkrtos_fdt_load(copy2, (uint32)len, blob, (uint32)len)
                  == ARKRTOS_FDT_OK, "load of the good blob succeeds");
            check(arkrtos_fdt_node_count() == before,
                  "loaded tree has the same node count");
            check(same(arkrtos_fdt_model(), model_before),
                  "loaded tree has the same model");

            /* 坏 blob：魔数错。必须被拒，且上一棵树还站着。 */
            copy2[0] ^= 0xffu;
            check(arkrtos_fdt_load(copy2, (uint32)len, copy2, (uint32)len) < 0,
                  "load of a bad blob is refused");
            check(arkrtos_fdt_ready() == 1, "previous tree survived the refusal");
            check(arkrtos_fdt_node_count() == before,
                  "previous tree kept its node count");
            check(same(arkrtos_fdt_model(), model_before),
                  "previous tree kept its model");
            /* load() 换的是全局视图：这里必须换回来，否则视图指向的
             * 内存马上被 free，后面任何读取都是读已释放的堆。 */
            check(arkrtos_fdt_init(blob, (uint32)len) == ARKRTOS_FDT_OK,
                  "base tree restored after the load test");
            free(copy2);
        }
    }

    /* ---- F-1b: /chosen/stdout-path 的解析与板级核对 ---- */
    {
        arkrtos_fdt_console_t cc;
        uint32 rlen = 0u;
        const void *raw;

        raw = arkrtos_fdt_stdout_raw(&rlen);
        check(raw != NULL, "chosen/stdout-path is present");
        check(raw != NULL && rlen > 0u, "chosen/stdout-path has bytes");

        memset(&cc, 0, sizeof(cc));
        rc = arkrtos_fdt_stdout_resolve(&cc);
        check(rc == ARKRTOS_FDT_OK, "stdout-path resolves");
        check(cc.path[0] == '/', "resolved console path is absolute");
        check(arkrtos_fdt_node_by_path(cc.path, NULL) == ARKRTOS_FDT_OK,
              "resolved console path names a real node");

        /* 句柄形态（&uart0 / &usart1）没有选项，所以 baud 必须是 0；
         * 字符串形态（serial0:115200n8）必须真的把选项读出来。 */
        if(cc.from_alias != 0u)
        {
            check(cc.baud == 115200u, "alias form carries baud=115200");
            check(cc.bits == 8u, "alias form carries bits=8");
            check(cc.parity == 'n', "alias form carries parity=n");
        }
        else
        {
            check(cc.baud == 0u, "handle form carries no baud (nothing was written)");
        }

        /* 与板级核对：一致才 bind，不一致必须给出具体原因。 */
        {
            arkrtos_fdt_node_t cn;
            arkrtos_console_expect_t exp;
            arkrtos_console_bind_t bind;
            uint32 reg0 = 0u;

            check(arkrtos_fdt_node_by_path(cc.path, &cn) == ARKRTOS_FDT_OK,
                  "console node is reachable for the bind check");
            exp.compatible = arkrtos_fdt_prop_str(&cn, "compatible");
            (void)arkrtos_fdt_prop_u32(&cn, "reg", 0u, &reg0);
            exp.reg_base = reg0;

            memset(&bind, 0, sizeof(bind));
            check(arkrtos_console_from_fdt(&exp, &bind) == ARKRTOS_CONSOLE_OK,
                  "board-level bind check passes on a consistent tree");
            check(same(bind.path, cc.path), "bind reports the resolved path");

            /* 反假绿：把期望地址改错，必须报 ADDR，而不是照样绑定。 */
            exp.reg_base = reg0 ^ 0x1000u;
            check(arkrtos_console_from_fdt(&exp, &bind) == ARKRTOS_CONSOLE_ADDR,
                  "wrong expected address is refused as ADDR");
            check(arkrtos_console_from_fdt(NULL, &bind) == ARKRTOS_CONSOLE_OK,
                  "null expectation means tree-only check (and does not crash)");
        }

        check(arkrtos_console_reason(ARKRTOS_CONSOLE_OK) != NULL,
              "every reason code has text");
        check(arkrtos_console_reason(-99) != NULL,
              "unknown reason code still yields text");
    }


#if (ARKRTOS_USE_FDT_OVERLAY == 1)
    /* ---- F-1c: overlay 最小子集 ----
     * 用命令行给的 fragment dtb（tools/dts/overlay/demo.dts 编出来的），
     * 合并进真树，然后把新 blob 交给解析器读回来——只看返回值不够，
     * 必须让第二个实现（解析器）重新走一遍这棵树。
     */
    {
        unsigned char *arena;
        uint32 base_nodes = arkrtos_fdt_node_count();
        char base_model_buf[128];
        uint32 out_len = 0u;
        bld_t b;
        uint32 flen;
        int rc2;

        strncpy(base_model_buf, arkrtos_fdt_model() ? arkrtos_fdt_model() : "",
                sizeof(base_model_buf) - 1u);
        base_model_buf[sizeof(base_model_buf) - 1u] = '\0';

        arena = (unsigned char *)malloc(65536u);
        check(arena != NULL, "overlay arena allocated");
        if(arena != NULL)
        {
            const unsigned char *frag = NULL;
            long frag_len = 0;

            if(argc >= 3)
            {
                frag = load(argv[2], &frag_len);
            }
            check(frag != NULL, "fragment dtb given as argv[2]");

            if(frag != NULL)
            {
                /* --- 1. 成功路径 --- */
                rc2 = arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                         frag, (uint32)frag_len, &out_len);
                check(rc2 == ARKRTOS_FDT_OK, "overlay of the demo fragment succeeds");
                check(out_len > 0u, "overlay reports a non-zero output length");

                if(rc2 == ARKRTOS_FDT_OK)
                {
                    arkrtos_fdt_node_t n;
                    uint32 u = 0u;

                    /* 换树：新 blob 必须能被解析器完整接受 */
                    check(arkrtos_fdt_init(arena, out_len) == ARKRTOS_FDT_OK,
                          "merged blob parses");
                    check(arkrtos_fdt_node_count() == base_nodes + 1u,
                          "merged tree has exactly one new node");
                    check(same(arkrtos_fdt_model(), "fdt-overlay-demo"),
                          "model was overridden by the later fragment");

                    check(arkrtos_fdt_node_by_path("/overlay-demo-node", &n)
                          == ARKRTOS_FDT_OK,
                          "new child node is reachable by path");
                    check(same(arkrtos_fdt_prop_str(&n, "compatible"),
                               "ark-overlay-demo"),
                          "new child keeps its compatible");
                    check(arkrtos_fdt_prop_u32(&n, "overlay-value", 0u, &u)
                          == ARKRTOS_FDT_OK, "new child keeps its u32 property");
                    check(u == 0x2au, "new child property value is 0x2a");

                    /* 根上的新属性也得在 */
                    check(arkrtos_fdt_node_by_path("/", &n) == ARKRTOS_FDT_OK,
                          "root reachable in the merged tree");
                    check(same(arkrtos_fdt_prop_str(&n, "overlay-demo"), "yes"),
                          "root gained the overlay-demo property");

                    /* 基树原有的节点没有被改动：随便挑一个必须还在 */
                    check(arkrtos_fdt_node_count() == base_nodes + 1u,
                          "no base node was lost");

                    /* 恢复基树，后面的用例与最终报表都基于它 */
                    check(arkrtos_fdt_init(blob, (uint32)len) == ARKRTOS_FDT_OK,
                          "base tree restored after the overlay test");
                }

                /* --- 2. 拒绝路径：每一条必须报它自己的错误码 --- */
                flen = bld_frag(&b, "/no-such-node", "overlay-demo", "yes",
                                "overlay-new", 0);
                check(flen > 0u, "bad-target fragment built");
                check(arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                         b.out, flen, &out_len)
                      == ARKRTOS_FDT_ERR_OVERLAY_TARGET,
                      "unknown target-path -> OVERLAY_TARGET");
                check(arkrtos_fdt_node_count() == base_nodes,
                      "base node count unchanged after a refused target");
                check(same(arkrtos_fdt_model(), base_model_buf),
                      "base model unchanged after a refused target");

                /* target = <&label>：句柄形式，本子集不支持 */
                bld_init(&b);
                bld_begin(&b, "");
                bld_begin(&b, "fragment@0");
                bld_pu32(&b, "target", 0x1234u);
                bld_begin(&b, "__overlay__");
                bld_pstr(&b, "overlay-demo", "yes");
                bld_end(&b);
                bld_end(&b);
                bld_end(&b);
                flen = bld_finish(&b);
                check(flen > 0u, "handle-target fragment built");
                check(arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                         b.out, flen, &out_len)
                      == ARKRTOS_FDT_ERR_OVERLAY_TARGET,
                      "target = <phandle> -> OVERLAY_TARGET");

                /* 同名子节点：拿基树根的第一个直接子节点名 */
                {
                    arkrtos_fdt_iter_t it2;
                    arkrtos_fdt_node_t n2;
                    char dup_name[64];

                    dup_name[0] = '\0';
                    if(arkrtos_fdt_iter_init(&it2) == ARKRTOS_FDT_OK &&
                       arkrtos_fdt_next(&it2, &n2) == 1)
                    {
                        strncpy(dup_name, n2.name, sizeof(dup_name) - 1u);
                        dup_name[sizeof(dup_name) - 1u] = '\0';
                    }
                    check(dup_name[0] != '\0', "found an existing child name");

                    flen = bld_frag(&b, "/", "overlay-demo", "yes", dup_name, 0);
                    check(flen > 0u, "duplicate-child fragment built");
                    check(arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                             b.out, flen, &out_len)
                          == ARKRTOS_FDT_ERR_OVERLAY_DUP,
                          "duplicate child name -> OVERLAY_DUP");
                    check(arkrtos_fdt_node_count() == base_nodes,
                          "base node count unchanged after a duplicate child");
                }

                /* __symbols__：标签段，一律拒绝 */
                flen = bld_frag(&b, "/", "overlay-demo", "yes", NULL, 1);
                check(flen > 0u, "__symbols__ fragment built");
                check(arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                         b.out, flen, &out_len)
                      == ARKRTOS_FDT_ERR_OVERLAY_TAG,
                      "__symbols__ -> OVERLAY_TAG");

                /* arena 太小 */
                flen = bld_frag(&b, "/", "overlay-demo", "yes", NULL, 0);
                check(flen > 0u, "size fragment built");
                check(arkrtos_fdt_overlay(arena, 64u, blob, (uint32)len,
                                         b.out, flen, &out_len)
                      == ARKRTOS_FDT_ERR_OVERLAY_SIZE,
                      "tiny arena -> OVERLAY_SIZE");
                check(out_len == 0u, "out_len stays 0 when the arena is too small");

                /* 坏魔数：报解析器的 MAGIC，不是 overlay 的码 */
                flen = bld_frag(&b, "/", "overlay-demo", "yes", NULL, 0);
                check(flen > 0u, "magic fragment built");
                b.out[0] ^= 0xffu;
                check(arkrtos_fdt_overlay(arena, 65536u, blob, (uint32)len,
                                         b.out, flen, &out_len)
                      == ARKRTOS_FDT_ERR_MAGIC,
                      "bad fragment magic -> FDT_ERR_MAGIC");

                /* 每个错误码都要有文字说明 */
                check(arkrtos_fdt_overlay_reason(ARKRTOS_FDT_ERR_OVERLAY) != NULL,
                      "overlay reason text exists");
                check(arkrtos_fdt_overlay_reason(-99) != NULL,
                      "unknown overlay code still yields text");

                free((void *)frag);
            }
            free(arena);
        }
    }
#endif /* ARKRTOS_USE_FDT_OVERLAY */

    free(blob);
    if(g_fail != 0)
    {
        printf("test_fdt: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_fdt: %u node(s) match the C table, damaged blobs refused "
           "(model=%s)\n", (unsigned)total, arkrtos_dt_model());
    return 0;
}
