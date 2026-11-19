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
