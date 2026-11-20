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
