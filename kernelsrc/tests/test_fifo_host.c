/**
* @file test_fifo_host.c
* @brief arkrtos_fifo 的宿主测试（不依赖板子、不依赖调度器）
*
* @details 覆盖 arkrtos_fifo_create / write / read 的字节搬运语义，以及两个
*          「只能读不能猜」的计数器（write_refused / bad_magic[_bytes]）。
*
*          已验证：
*          - create 的尺寸门槛：size <= 8 返回 0 且不碰缓冲；size > 8 时
*            有效容量是 size - 9（头部 8 字节 + 一格用于区分空/满）。
*          - 写满后拒绝剩余字节并把拒绝量记进 refused，已写内容不被覆盖。
*          - 环形回绕：写—读—再写跨过数据区末尾，读出顺序仍是发送顺序。
*          - 部分读：请求多于存量时返回存量。
*          - 头魔数损坏：write 返回 0 并计 bad_magic(+1) 与 bad_magic_bytes(+len)；
*            read 返回 0 并只计 bad_magic（没有字节因「读」而丢）。
*          - NULL 句柄走同一条破损路径（不解引用，计数照记）。
*          - len == 0 与 len < 0 都是空操作，不污染 refused。
*          - 计数器是全局累计，跨多个 FIFO 求和。
*
*          刻意没验证：
*          - 单生产者-单消费者之外的并发（本模块按契约不内建临界区）；
*          - size 超过 65543（size 字段是 uint16，会静默截断——这是已知限制，
*            不在测试里制造未定义行为）；
*          - 手工构造 size == 0 且魔数正确时 write 会除零（同上，属调用方违约）。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fifo.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                   \
        if(cond) { g_pass++; }                                             \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while(0)

/* 每个用例前把缓冲刷成已知值，便于证明「没写进去的字节确实没被动」 */
static void fill(uint8 *p, int32 n, uint8 v)
{
    int32 i;
    for(i = 0; i < n; i++) { p[i] = v; }
}

static void test_create(void)
{
    uint8 buf[32];

    /* size <= 8 一律拒绝：头部自身就要 8 字节，没有数据区 */
    fill(buf, sizeof(buf), 0xA5u);
    CHECK(arkrtos_fifo_create(buf, 8) == 0);
    CHECK(arkrtos_fifo_create(buf, 0) == 0);
    CHECK(arkrtos_fifo_create(buf, -4) == 0);
    CHECK(buf[0] == 0xA5u);            /* 被拒时不得改写调用方缓冲 */

    /* size = 9 时数据区 1 字节、可用 0 字节：写不进任何东西，
     * 这是「留一格判满」的直接后果，必须能从行为上看出来 */
    CHECK(arkrtos_fifo_create(buf, 9) != 0);
    {
        arkrtos_fifo_t *f = (arkrtos_fifo_t *)buf;
        uint8 one = 0x5Au;
        CHECK(f->size == 1u);
        CHECK(f->wt_idx == 0u);
        CHECK(f->rd_idx == 0u);
        CHECK(arkrtos_fifo_write(f, &one, 1) == 0);
    }
}

static void test_basic(void)
{
    uint8 buf[16];
    uint8 in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8 out[8];
    arkrtos_fifo_t *f;
    uint32 refused0;

    /* size = 16 → 数据区 8 字节 → 可用 7 字节 */
    f = arkrtos_fifo_create(buf, 16);
    CHECK(f != 0);
    CHECK(f->size == 8u);

    refused0 = arkrtos_fifo_write_refused();
    CHECK(arkrtos_fifo_write(f, in, 7) == 7);
    CHECK(arkrtos_fifo_write_refused() == refused0);   /* 没满就不记 */

    /* 满了：第 8 字节被拒，且拒绝量记进计数器 */
    CHECK(arkrtos_fifo_write(f, in + 7, 1) == 0);
    CHECK(arkrtos_fifo_write_refused() == refused0 + 1u);

    memset(out, 0, sizeof(out));
    CHECK(arkrtos_fifo_read(f, out, 7) == 7);
    CHECK(memcmp(out, in, 7) == 0);

    /* 读空后再读 */
    CHECK(arkrtos_fifo_read(f, out, 8) == 0);

    /* 请求多于存量：返回存量，不填多余字节 */
    CHECK(arkrtos_fifo_write(f, in, 3) == 3);
    memset(out, 0xEE, sizeof(out));
    CHECK(arkrtos_fifo_read(f, out, 8) == 3);
    CHECK(out[0] == 1u && out[1] == 2u && out[2] == 3u);
    CHECK(out[3] == 0xEEu);
}

static void test_wrap(void)
{
    uint8 buf[16];
    uint8 in[16];
    uint8 out[16];
    arkrtos_fifo_t *f;
    int32 i;

    for(i = 0; i < 16; i++) { in[i] = (uint8)(0x40 + i); }

    f = arkrtos_fifo_create(buf, 16);      /* 可用 7 字节 */
    CHECK(f != 0);

    /* 先写满再读掉，让读写指针都停在中间，然后写一段必然跨尾的 */
    CHECK(arkrtos_fifo_write(f, in, 5) == 5);
    CHECK(arkrtos_fifo_read(f, out, 5) == 5);
    CHECK(memcmp(out, in, 5) == 0);

    /* 此时 wt=rd=5；再写 7 字节会按 6,7,0,1,2,3,4 绕回去 */
    CHECK(arkrtos_fifo_write(f, in + 5, 7) == 7);
    memset(out, 0, sizeof(out));
    CHECK(arkrtos_fifo_read(f, out, 7) == 7);
    CHECK(memcmp(out, in + 5, 7) == 0);

    /* 绕回后再写满一轮，确认 dev 指针没有离开 [0, size) */
    CHECK(arkrtos_fifo_write(f, in, 7) == 7);
    CHECK(arkrtos_fifo_read(f, out, 7) == 7);
    CHECK(memcmp(out, in, 7) == 0);
    CHECK(f->wt_idx < f->size);
    CHECK(f->rd_idx < f->size);
}

static void test_bad_magic(void)
{
    uint8 buf[16];
    uint8 in[4] = { 9, 9, 9, 9 };
    uint8 out[4];
    arkrtos_fifo_t *f;
    uint32 call0, byte0;

    f = arkrtos_fifo_create(buf, 16);
    CHECK(f != 0);
    f->magic = 0u;                       /* 模拟头部被踩坏 */

    call0 = arkrtos_fifo_bad_magic();
    byte0 = arkrtos_fifo_bad_magic_bytes();

    /* 写：整批字节都在这里丢掉，必须两边都记 */
    CHECK(arkrtos_fifo_write(f, in, 4) == 0);
    CHECK(arkrtos_fifo_bad_magic() == call0 + 1u);
    CHECK(arkrtos_fifo_bad_magic_bytes() == byte0 + 4u);

    /* 读：调用失败但没有「因读而丢的字节」，只记调用次数 */
    CHECK(arkrtos_fifo_read(f, out, 4) == 0);
    CHECK(arkrtos_fifo_bad_magic() == call0 + 2u);
    CHECK(arkrtos_fifo_bad_magic_bytes() == byte0 + 4u);

    /* NULL 句柄走同一条路径（note_bad 不解引用句柄） */
    CHECK(arkrtos_fifo_write((arkrtos_fifo_t *)0, in, 3) == 0);
    CHECK(arkrtos_fifo_bad_magic() == call0 + 3u);
    CHECK(arkrtos_fifo_bad_magic_bytes() == byte0 + 7u);
    CHECK(arkrtos_fifo_read((arkrtos_fifo_t *)0, out, 3) == 0);
    CHECK(arkrtos_fifo_bad_magic() == call0 + 4u);
    CHECK(arkrtos_fifo_bad_magic_bytes() == byte0 + 7u);
}

static void test_empty_and_negative(void)
{
    uint8 buf[16];
    uint8 in[4] = { 1, 2, 3, 4 };
    uint8 out[4];
    arkrtos_fifo_t *f;
    uint32 refused0;

    f = arkrtos_fifo_create(buf, 16);
    CHECK(f != 0);
    memset(out, 0, sizeof(out));

    refused0 = arkrtos_fifo_write_refused();
    CHECK(arkrtos_fifo_write(f, in, 0) == 0);
    CHECK(arkrtos_fifo_write(f, in, -3) == 0);
    CHECK(arkrtos_fifo_write_refused() == refused0);   /* 空操作不算拒绝 */
    CHECK(arkrtos_fifo_read(f, out, 0) == 0);
    CHECK(arkrtos_fifo_read(f, out, -3) == 0);

    /* 空操作之后 FIFO 仍可用 */
    CHECK(arkrtos_fifo_write(f, in, 4) == 4);
    CHECK(arkrtos_fifo_read(f, out, 4) == 4);
    CHECK(memcmp(out, in, 4) == 0);
}

static void test_refused_is_global(void)
{
    uint8 b1[16];
    uint8 b2[16];
    uint8 in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    arkrtos_fifo_t *f1;
    arkrtos_fifo_t *f2;
    uint32 refused0;

    /* 两个 FIFO、各自 size=12 → 数据区 4、可用 3 字节。
     * 计数器是所有 FIFO 的和，不是每个实例各一份。 */
    f1 = arkrtos_fifo_create(b1, 12);
    f2 = arkrtos_fifo_create(b2, 12);
    CHECK(f1 != 0);
    CHECK(f2 != 0);

    refused0 = arkrtos_fifo_write_refused();
    CHECK(arkrtos_fifo_write(f1, in, 5) == 3);
    CHECK(arkrtos_fifo_write(f2, in, 5) == 3);
    CHECK(arkrtos_fifo_write_refused() == refused0 + 4u);
}

int main(void)
{
    test_create();
    test_basic();
    test_wrap();
    test_bad_magic();
    test_empty_and_negative();
    test_refused_is_global();

    printf("fifo-host: %d passed, %d failed\n", g_pass, g_fail);
    if(g_fail == 0)
    {
        printf("FIFO-HOST OK\n");
        return 0;
    }
    return 1;
}
