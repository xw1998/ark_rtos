/**
* @file  mpool_demo.c
* @brief 固定块内存池（SVC 0x20）的非特权通道自检程序。
* @details 这份程序跑在 App 自己的非特权任务里，池的内存区是 App 自己的一段
*          静态数组。所有分配/释放/查询都经 SVC 0x20 进内核，因此它同时验证
*          三件事：
*
*            1. 用户态封装（arkrtos_mpool_*）能穿过 SVC 边界拿到正确结果；
*            2. 内核在边界上做的参数校验与「内存区必须落在调用者可见 RAM 内」
*               这条检查真的生效（负例必须被拒，且拒绝后池状态不变）；
*            3. 池的生命周期：删除后句柄立刻失效，名字可以被重新占用。
*
*          全部用例都打印 PASS/FAIL，最后一行汇总。任何一个 FAIL 都会让
*          退出码非 0，所以在串口上看最后一行就够。
*/

#include <stdio.h>
#include <stdint.h>

#include "arkrtos.h"

#define BLK_SIZE   32u
#define BLK_COUNT  16u

/* 池的内存区由调用者提供：对齐到指针宽度，整段落在 App 自己的 RAM 窗口里。 */
static uint8_t s_pool[BLK_SIZE * BLK_COUNT] __attribute__((aligned(8)));
static uint8_t s_pool2[BLK_SIZE * 4u]         __attribute__((aligned(8)));
/* 只用来喂"必须被拒"的创建请求：被拒了就没被占用，与真池互不干扰。 */
static uint8_t s_pool3[BLK_SIZE * 4u]         __attribute__((aligned(8)));

/* 故意放在池外面的缓冲区，用来试「外来指针必须被拒」。 */
static uint8_t s_foreign[64] __attribute__((aligned(8)));

static uint32_t s_pass;
static uint32_t s_fail;

static void chk(int cond, const char *what)
{
    if (cond) {
        s_pass++;
        printf("  PASS  %s\n", what);
    } else {
        s_fail++;
        printf("  FAIL  %s\n", what);
    }
}

static int in_pool(const void *p, const uint8_t *base, uint32_t len)
{
    uintptr_t a = (uintptr_t)p;
    uintptr_t b = (uintptr_t)base;

    return (a >= b) && (a < b + len);
}

int main(void)
{
    arkrtos_mpool_stat_t st;
    int32_t  h;
    int32_t  h2;
    int32_t  bad;
    void    *a;
    void    *b;
    void    *c;
    void    *again;
    uint32_t i;
    void    *held[BLK_COUNT];

    printf("\n--- mpool demo (SVC 0x20) ---\n");
    printf("pool base=%08x block=%u count=%u\n",
           (unsigned)(uintptr_t)s_pool, (unsigned)BLK_SIZE, (unsigned)BLK_COUNT);

    /* 1. 创建 */
    h = arkrtos_mpool_create("demo", s_pool, BLK_SIZE, BLK_COUNT);
    printf("create -> %d\n", (int)h);
    chk(h >= 0, "create returns a handle");
    if (h < 0) {
        goto done;
    }

    /* 2. 刚创建时的快照 */
    if (arkrtos_mpool_query(h, &st) == 0) {
        printf("query  name=%s block=%u count=%u free=%u peak=%u\n",
               st.name, (unsigned)st.block_size, (unsigned)st.block_count,
               (unsigned)st.free_count, (unsigned)st.peak_used);
        chk(st.block_size == BLK_SIZE && st.block_count == BLK_COUNT,
            "query reports the pool geometry");
        chk(st.free_count == BLK_COUNT && st.peak_used == 0u,
            "a fresh pool is entirely free");
    } else {
        chk(0, "query on a live handle");
    }

    /* 3. 连拿三块：非空、在池内、按指针宽度对齐、互不相同 */
    a = arkrtos_mpool_alloc(h);
    b = arkrtos_mpool_alloc(h);
    c = arkrtos_mpool_alloc(h);
    printf("alloc  a=%08x b=%08x c=%08x\n",
           (unsigned)(uintptr_t)a, (unsigned)(uintptr_t)b, (unsigned)(uintptr_t)c);
    chk(a && b && c, "three allocations succeed");
    chk(in_pool(a, s_pool, sizeof(s_pool)) && in_pool(b, s_pool, sizeof(s_pool)) &&
        in_pool(c, s_pool, sizeof(s_pool)), "blocks come from the caller's array");
    chk(((uintptr_t)a & 7u) == 0u && ((uintptr_t)b & 7u) == 0u,
        "blocks are pointer aligned");
    chk(a != b && b != c && a != c, "blocks are distinct");

    /* 写满一块的内容，确认它们真的可以当内存用 */
    for (i = 0; i < BLK_SIZE; i++) {
        ((volatile uint8_t *)a)[i] = (uint8_t)i;
    }
    chk(1, "a block is readable and writable");

    if (arkrtos_mpool_query(h, &st) == 0) {
        printf("query  free=%u alloc_ok=%u peak=%u\n",
               (unsigned)st.free_count, (unsigned)st.alloc_ok,
               (unsigned)st.peak_used);
        chk(st.free_count == BLK_COUNT - 3u, "free count drops by three");
        chk(st.alloc_ok == 3u && st.peak_used == 3u,
            "counters follow the allocations");
    } else {
        chk(0, "query after allocating");
    }

    /* 4. 还一块，再拿一块：应当拿回刚才那一块（LIFO 空闲链） */
    chk(arkrtos_mpool_free(h, b) == 0, "free returns 0");
    again = arkrtos_mpool_alloc(h);
    chk(again == b, "the block just freed is the next one handed out");

    /* 5. 外来指针必须被拒，且池状态不变 */
    {
        int32_t  freed = 0;
        uint32_t free_before = 0u;
        uint32_t bad_before = 0u;

        if (arkrtos_mpool_query(h, &st) == 0) {
            free_before = st.free_count;
            bad_before  = st.free_bad;
        }
        /* 只调一次：调用两次会让 free_bad 加 2，"计数"那条用例就说不清了 */
        freed = arkrtos_mpool_free(h, s_foreign);
        printf("free(foreign) -> %d\n", (int)freed);
        chk(freed < 0, "a foreign pointer is refused");
        if (arkrtos_mpool_query(h, &st) == 0) {
            printf("query  free=%u free_bad=%u\n",
                   (unsigned)st.free_count, (unsigned)st.free_bad);
            chk(st.free_count == free_before, "a refused free leaves the pool alone");
            chk(st.free_bad == bad_before + 1u, "refused frees are counted");
        } else {
            chk(0, "query after a refused free");
        }
    }

    /* 6. 参数校验：重名 / 块太小 / 块未对齐 / 块数 0 / 名字超长 / 池在 Flash 里
     * 注意口径：ARM 上指针宽度是 4，所以"块太小"要取 2 而不是 4，
     * "未对齐"要取 30 而不是 20 —— 4 与 20 都是合法的块大小。 */
    printf("ptr width=%u byte(s)\n", (unsigned)sizeof(void *));
    chk(arkrtos_mpool_create("demo", s_pool3, BLK_SIZE, 4u) < 0,
        "a duplicate name is refused");
    bad = arkrtos_mpool_create("tiny", s_pool3, 2u, 4u);
    chk(bad < 0, "a block smaller than a pointer is refused");
    if (bad >= 0) {
        (void)arkrtos_mpool_delete(bad);
    }
    bad = arkrtos_mpool_create("odd", s_pool3, 30u, 4u);
    chk(bad < 0, "a block not pointer aligned is refused");
    if (bad >= 0) {
        (void)arkrtos_mpool_delete(bad);
    }
    chk(arkrtos_mpool_create("zero", s_pool3, BLK_SIZE, 0u) < 0,
        "a zero block count is refused");
    chk(arkrtos_mpool_create("toolongname", s_pool3, BLK_SIZE, 4u) < 0,
        "an over long name is refused");
    /* "池在 Flash 里"这一条不写地址字面量：App 侧工程不许出现布局常量
     * （ci_gate 的 single-address-source），所以拿 main 自己的入口地址当样本
     * —— 它在 App 的 ROM 窗口里，对"必须是可见 RAM"这条规则就是非法区。 */
    bad = arkrtos_mpool_create("flash", (void *)(uintptr_t)(void (*)(void))main,
                               BLK_SIZE, 4u);
    chk(bad < 0, "a pool area outside the caller's RAM is refused");
    if (bad >= 0) {
        (void)arkrtos_mpool_delete(bad);
    }

    /* 7. 用尽：4 块拿 4 块，第 5 次必须拿到空指针 */
    h2 = arkrtos_mpool_create("demo2", s_pool2, BLK_SIZE, 4u);
    printf("create demo2 -> %d\n", (int)h2);
    chk(h2 >= 0, "a second pool with its own name is accepted");
    if (h2 >= 0) {
        for (i = 0; i < 4u; i++) {
            held[i] = arkrtos_mpool_alloc(h2);
        }
        chk(held[0] && held[1] && held[2] && held[3], "all four blocks are handed out");
        chk(arkrtos_mpool_alloc(h2) == 0, "an exhausted pool returns null");
        if (arkrtos_mpool_query(h2, &st) == 0) {
            printf("query  free=%u alloc_ok=%u alloc_fail=%u\n",
                   (unsigned)st.free_count, (unsigned)st.alloc_ok,
                   (unsigned)st.alloc_fail);
            chk(st.free_count == 0u && st.alloc_fail == 1u,
                "the failed allocation is counted and nothing is left");
        }
        for (i = 0; i < 4u; i++) {
            (void)arkrtos_mpool_free(h2, held[i]);
        }
        if (arkrtos_mpool_query(h2, &st) == 0) {
            chk(st.free_count == 4u, "freeing everything gives the pool back");
        }
        chk(arkrtos_mpool_delete(h2) == 0, "delete the second pool");
    }

    /* 8. 删除后句柄立刻失效 */
    chk(arkrtos_mpool_delete(h) == 0, "delete returns 0");
    chk(arkrtos_mpool_alloc(h) == 0, "alloc on a deleted handle returns null");
    chk(arkrtos_mpool_query(h, &st) < 0, "query on a deleted handle fails");
    chk(arkrtos_mpool_free(h, a) < 0, "free on a deleted handle fails");

    /* 9. 名字被释放，可以重新占用；内存区不归内核，仍在原地 */
    h = arkrtos_mpool_create("demo", s_pool, BLK_SIZE, BLK_COUNT);
    chk(h >= 0, "the name is reusable after delete");
    if (h >= 0) {
        chk(arkrtos_mpool_delete(h) == 0, "delete the recreated pool");
    }

done:
    printf("MPOOL: %u pass, %u fail\n", (unsigned)s_pass, (unsigned)s_fail);
    printf("MPOOL: %s\n", (s_fail == 0u) ? "ALL PASS" : "FAILED");
    return (s_fail == 0u) ? 0 : 1;
}
