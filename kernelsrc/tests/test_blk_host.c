/**
* @file test_blk_host.c
* @brief 块设备注册表（arkrtos_blk.c）的宿主单元测试
* @details 这一层是 loader / 文件系统唯一看得见的"非易失存储"接缝。它自己
*          不碰任何硬件，所以全部行为都能在宿主机上摆出来；而它一旦记错账，
*          症状总是"镜像写坏了一块"或"擦掉了邻居记录"，在板子上极难定位。
*
*          被测的是真实的 arkrtos_blk.c，后端用本文件里的假描述符代替。
*
*          覆盖范围（如实说明）：
*            已验证：描述符校验（空指针/缺回调/空名/超长名）、重复注册、
*                    表满、注册顺序与按名查找、名称前缀不算命中、
*                    bring-up 只跑一次且失败不置就绪、容量/擦除单位的
*                    合法性判定、就绪前的读写擦一律拒绝、越界与对齐检查、
*                    参数原样透传给后端、后端返回码原样上抛。
*            未说明：真实存储介质的时序、掉电中断、并发注册。这些要上板，
*                    宿主全绿不等于板端已验证。
* @note 用法：由 tools/coverage_gate.py 的 blk / blk-off 两档编译并运行，
*       输出 BLK-HOST OK。
*/

#include "arkrtos_blk.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

#if ARKRTOS_USE_BLK

/* ------------------------------------------------------------------
 * 假后端：把每次调用记下来，方便断言"参数原样透传"
 * ------------------------------------------------------------------ */
static int32  g_bringup_rc    = 0;
static int    g_bringup_calls = 0;
static uint32 g_size_fn_val   = 0u;
static int32  g_read_rc       = 0;
static int32  g_write_rc      = 0;
static int32  g_erase_rc      = 0;
static int    g_io_calls      = 0;
static uint32 g_io_off        = 0u;
static uint32 g_io_len        = 0u;
static const void *g_io_buf   = 0;

static int32 be_bringup(void)
{
    g_bringup_calls++;
    return g_bringup_rc;
}

static uint32 be_size(void)
{
    return g_size_fn_val;
}

static int32 be_read(uint32 off, uint8 *buf, uint32 len)
{
    g_io_calls++;
    g_io_off = off;
    g_io_len = len;
    g_io_buf = buf;
    return g_read_rc;
}

static int32 be_write(uint32 off, const uint8 *buf, uint32 len)
{
    g_io_calls++;
    g_io_off = off;
    g_io_len = len;
    g_io_buf = buf;
    return g_write_rc;
}

static int32 be_erase(uint32 off, uint32 len)
{
    g_io_calls++;
    g_io_off = off;
    g_io_len = len;
    return g_erase_rc;
}

/* 正常后端：静态容量 0x1000、擦除单位 0x100 */
static arkrtos_blk_dev_t g_dev_a = {
    "int0", 0x1000u, 0x100u, be_bringup, 0, be_read, be_write, be_erase
};

/* 容量只由 size_fn 回答的后端；名字正好 ARKRTOS_BLK_NAME_MAX-1 个字符 */
static arkrtos_blk_dev_t g_dev_b = {
    "abcdefghijklmno", 0u, 0x100u, 0, be_size, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_c = {
    "nor0", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_d = {
    "mmc0", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_e = {
    "sd0", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

/* 下面这几个故意不合法，注册时必须被拒（而且不能占槽位） */
static arkrtos_blk_dev_t g_dev_no_read = {
    "nr", 0x800u, 0x100u, 0, 0, 0, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_no_name = {
    0, 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_empty_name = {
    "", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_long_name = {
    "0123456789abcdef", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static arkrtos_blk_dev_t g_dev_unregistered = {
    "ghost", 0x800u, 0x100u, 0, 0, be_read, be_write, be_erase
};

static void io_reset(void)
{
    g_io_calls = 0;
    g_io_off   = 0u;
    g_io_len   = 0u;
    g_io_buf   = 0;
}

/* ------------------------------------------------------------------
 * 1. 描述符校验：不合法的注册一个槽位都不许占
 * ------------------------------------------------------------------ */
static void test_register_rejects(void)
{
    ck(arkrtos_blk_register(0) == ARKRTOS_BLK_ERR_PARAM,
       "null descriptor is refused");
    ck(arkrtos_blk_register(&g_dev_no_name) == ARKRTOS_BLK_ERR_PARAM,
       "descriptor without a name is refused");
    ck(arkrtos_blk_register(&g_dev_no_read) == ARKRTOS_BLK_ERR_PARAM,
       "descriptor without a read hook is refused");
    ck(arkrtos_blk_register(&g_dev_empty_name) == ARKRTOS_BLK_ERR_PARAM,
       "empty name is refused");
    ck(arkrtos_blk_register(&g_dev_long_name) == ARKRTOS_BLK_ERR_PARAM,
       "name of ARKRTOS_BLK_NAME_MAX bytes is refused (no room for the zero)");
    ck(arkrtos_blk_count() == 0u,
       "a refused registration leaves the table empty");

    /* 缺 write / erase 的两支 */
    g_dev_no_read.read  = be_read;
    g_dev_no_read.write = 0;
    ck(arkrtos_blk_register(&g_dev_no_read) == ARKRTOS_BLK_ERR_PARAM,
       "descriptor without a write hook is refused");
    g_dev_no_read.write = be_write;
    g_dev_no_read.erase = 0;
    ck(arkrtos_blk_register(&g_dev_no_read) == ARKRTOS_BLK_ERR_PARAM,
       "descriptor without an erase hook is refused");
    ck(arkrtos_blk_count() == 0u,
       "still nothing registered after the hook checks");
}

/* ------------------------------------------------------------------
 * 2. 注册与查找
 * ------------------------------------------------------------------ */
static void test_register_find(void)
{
    ck(arkrtos_blk_register(&g_dev_a) == 0, "a well formed backend registers");
    ck(arkrtos_blk_count() == 1u, "count follows registration");
    ck(arkrtos_blk_at(0) == &g_dev_a, "at(0) is the first registered backend");
    ck(arkrtos_blk_at(1) == 0, "at(count) is NULL, not garbage");
    ck(arkrtos_blk_find("int0") == &g_dev_a, "find by exact name hits");
    ck(arkrtos_blk_find("int") == 0, "a shorter name is not a hit");
    ck(arkrtos_blk_find("int00") == 0, "a longer name is not a hit");
    ck(arkrtos_blk_find("INT0") == 0, "names are case sensitive");
    ck(arkrtos_blk_find(0) == 0, "find(NULL) is not a lookup");
    ck(arkrtos_blk_find("nor0") == 0, "an unregistered name is not found");

    ck(arkrtos_blk_register(&g_dev_a) == ARKRTOS_BLK_ERR_EXIST,
       "registering the same descriptor twice is refused");
    ck(arkrtos_blk_count() == 1u, "the refused duplicate did not take a slot");

    /* 最长合法名字（15 字符 + 零） */
    ck(arkrtos_blk_register(&g_dev_b) == 0,
       "a name of ARKRTOS_BLK_NAME_MAX-1 characters registers");
    ck(arkrtos_blk_count() == 2u, "the long-name backend took the second slot");
    ck(arkrtos_blk_find("abcdefghijklmno") == &g_dev_b,
       "the longest legal name is found by its full text");
    ck(arkrtos_blk_find("abcdefghijklmn") == 0,
       "a 14 character prefix does not hit the 15 character name");
}

/* ------------------------------------------------------------------
 * 3. bring-up：失败不置就绪，成功只跑一次
 * ------------------------------------------------------------------ */
static void test_init(void)
{
    ck(arkrtos_blk_init(0) == ARKRTOS_BLK_ERR_PARAM,
       "init(NULL) is refused");
    ck(arkrtos_blk_init(&g_dev_unregistered) == ARKRTOS_BLK_ERR_NOTFOUND,
       "init of an unregistered descriptor is refused");

    /* bring-up 失败：不许置就绪，也不许留下半开状态 */
    g_bringup_rc    = -1;
    g_bringup_calls = 0;
    ck(arkrtos_blk_init(&g_dev_a) == ARKRTOS_BLK_ERR_BRINGUP,
       "a failing bring-up is reported");
    ck(g_bringup_calls == 1, "bring-up ran exactly once");
    ck(arkrtos_blk_is_ready(&g_dev_a) == 0u,
       "a failed bring-up does not mark the device ready");

    /* 容量既没有静态值也没有 size_fn */
    g_bringup_rc       = 0;
    g_bringup_calls    = 0;
    g_dev_a.size       = 0u;
    g_dev_a.size_fn    = 0;
    ck(arkrtos_blk_init(&g_dev_a) == ARKRTOS_BLK_ERR_DESC,
       "a descriptor that cannot answer its size is refused");
    g_dev_a.size    = 0x1000u;

    /* 有 size_fn 但回答 0 */
    g_size_fn_val   = 0u;
    g_dev_a.size_fn = be_size;
    ck(arkrtos_blk_init(&g_dev_a) == ARKRTOS_BLK_ERR_SIZE,
       "a size probe that answers zero is refused");
    g_size_fn_val   = 0x2000u;
    g_dev_a.size_fn = 0;

    /* 擦除单位必须是 2 的幂 */
    g_dev_a.erase_unit = 0u;
    ck(arkrtos_blk_init(&g_dev_a) == ARKRTOS_BLK_ERR_PARAM,
       "erase_unit 0 is refused");
    g_dev_a.erase_unit = 3u;
    ck(arkrtos_blk_init(&g_dev_a) == ARKRTOS_BLK_ERR_PARAM,
       "a non power of two erase_unit is refused");
    ck(arkrtos_blk_is_ready(&g_dev_a) == 0u,
       "none of the failed attempts marked the device ready");
    g_dev_a.erase_unit = 0x100u;

    /* 正常路径 + 幂等 */
    g_bringup_calls = 0;
    ck(arkrtos_blk_init(&g_dev_a) == 0, "a valid backend comes up");
    ck(g_bringup_calls == 1, "bring-up ran once");
    ck(arkrtos_blk_is_ready(&g_dev_a) == 1u, "the device is ready after init");
    ck(arkrtos_blk_init(&g_dev_a) == 0, "the second init is free");
    ck(g_bringup_calls == 1, "the second init did not re-run bring-up");

    ck(arkrtos_blk_is_ready(0) == 0u, "is_ready(NULL) is false");
    ck(arkrtos_blk_is_ready(&g_dev_unregistered) == 0u,
       "is_ready of an unregistered descriptor is false");
}

/* ------------------------------------------------------------------
 * 4. 容量查询
 * ------------------------------------------------------------------ */
static void test_size(void)
{
    ck(arkrtos_blk_size(0) == 0u, "size(NULL) is zero");
    ck(arkrtos_blk_size(&g_dev_a) == 0x1000u, "static size is reported");
    g_size_fn_val = 0x2000u;
    ck(arkrtos_blk_size(&g_dev_b) == 0x2000u,
       "size_fn wins over the static size");
    g_size_fn_val = 0x3000u;
    ck(arkrtos_blk_size(&g_dev_b) == 0x3000u,
       "size is a live probe, not a cached value");
}

/* ------------------------------------------------------------------
 * 5. 受保护访问：参数 / 就绪 / 越界 / 对齐
 * ------------------------------------------------------------------ */
static void test_access_guards(void)
{
    uint8 buf[16];
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        buf[i] = (uint8)i;
    }

    io_reset();

    /* 参数 */
    ck(arkrtos_blk_read(&g_dev_a, 0u, 0, 4u) == ARKRTOS_BLK_ERR_PARAM,
       "read with a null buffer is refused");
    ck(arkrtos_blk_read(&g_dev_b, 0u, buf, 4u) == ARKRTOS_BLK_ERR_CHECK,
       "read of a backend that never came up is refused");
    ck(arkrtos_blk_read(&g_dev_a, 0u, buf, 0u) == ARKRTOS_BLK_ERR_CHECK,
       "zero length is refused");
    ck(arkrtos_blk_read(0, 0u, buf, 4u) == ARKRTOS_BLK_ERR_CHECK,
       "read from a null descriptor is refused");
    ck(g_io_calls == 0, "no refused read reached the backend");

    /* 越界：len > size 与 off > size-len 两支 */
    ck(arkrtos_blk_read(&g_dev_a, 0u, buf, 0x1001u) == ARKRTOS_BLK_ERR_CHECK,
       "a request longer than the device is refused");
    ck(arkrtos_blk_read(&g_dev_a, 0x1000u, buf, 1u) == ARKRTOS_BLK_ERR_CHECK,
       "a request starting past the end is refused");
    ck(arkrtos_blk_read(&g_dev_a, 0xff0u, buf, 0x20u) == ARKRTOS_BLK_ERR_CHECK,
       "a request that runs off the end is refused");
    ck(g_io_calls == 0, "no out of range read reached the backend");

    /* 边界：正好占满整块是合法的 */
    ck(arkrtos_blk_read(&g_dev_a, 0u, buf, 0x1000u) == 0,
       "a request covering the whole device is legal");
    ck(g_io_calls == 1 && g_io_off == 0u && g_io_len == 0x1000u
       && g_io_buf == buf, "read parameters reached the backend unchanged");

    /* 后端返回码原样上抛 */
    io_reset();
    g_read_rc = -7;
    ck(arkrtos_blk_read(&g_dev_a, 0x100u, buf, 4u) == -7,
       "the backend return code is passed through, not flattened");
    g_read_rc = 0;
    ck(g_io_off == 0x100u && g_io_len == 4u, "offset and length reached the backend");

    /* 写 */
    io_reset();
    ck(arkrtos_blk_write(&g_dev_a, 0u, 0, 4u) == ARKRTOS_BLK_ERR_PARAM,
       "write with a null buffer is refused");
    ck(arkrtos_blk_write(&g_dev_b, 0u, buf, 4u) == ARKRTOS_BLK_ERR_CHECK,
       "write to a backend that never came up is refused");
    ck(g_io_calls == 0, "no refused write reached the backend");
    ck(arkrtos_blk_write(&g_dev_a, 0x200u, buf, 4u) == 0, "a valid write runs");
    ck(g_io_calls == 1 && g_io_off == 0x200u && g_io_len == 4u
       && g_io_buf == buf, "write parameters reached the backend unchanged");
    ck(arkrtos_blk_write(&g_dev_a, 0x1000u, buf, 4u) == ARKRTOS_BLK_ERR_CHECK,
       "a write past the end is refused");
    io_reset();
    g_write_rc = -3;
    ck(arkrtos_blk_write(&g_dev_a, 0u, buf, 4u) == -3,
       "the write return code is passed through");
    g_write_rc = 0;

    /* 擦除：多了对齐这一支 */
    io_reset();
    ck(arkrtos_blk_erase(0, 0u, 0x100u) == ARKRTOS_BLK_ERR_PARAM,
       "erase on a null descriptor is refused");
    ck(arkrtos_blk_erase(&g_dev_b, 0u, 0x100u) == ARKRTOS_BLK_ERR_CHECK,
       "erase on a backend that never came up is refused");
    ck(arkrtos_blk_erase(&g_dev_a, 0u, 0u) == ARKRTOS_BLK_ERR_CHECK,
       "zero length erase is refused");
    ck(arkrtos_blk_erase(&g_dev_a, 0x40u, 0x100u) == ARKRTOS_BLK_ERR_CHECK,
       "an erase offset that is not a multiple of erase_unit is refused");
    ck(arkrtos_blk_erase(&g_dev_a, 0u, 0x180u) == ARKRTOS_BLK_ERR_CHECK,
       "an erase length that is not a multiple of erase_unit is refused");
    ck(arkrtos_blk_erase(&g_dev_a, 0xf00u, 0x200u) == ARKRTOS_BLK_ERR_CHECK,
       "an erase that runs off the end is refused");
    ck(g_io_calls == 0, "no refused erase reached the backend");

    ck(arkrtos_blk_erase(&g_dev_a, 0x100u, 0x200u) == 0, "a valid erase runs");
    ck(g_io_calls == 1 && g_io_off == 0x100u && g_io_len == 0x200u,
       "erase parameters reached the backend unchanged");

    io_reset();
    g_erase_rc = -5;
    ck(arkrtos_blk_erase(&g_dev_a, 0u, 0x100u) == -5,
       "the erase return code is passed through");
    g_erase_rc = 0;
}

/* ------------------------------------------------------------------
 * 6. 表满：第 ARKRTOS_BLK_MAX_DEVS+1 个必须被拒
 * ------------------------------------------------------------------ */
static void test_table_full(void)
{
    ck(arkrtos_blk_register(&g_dev_c) == 0, "the third backend registers");
    ck(arkrtos_blk_register(&g_dev_d) == 0, "the fourth backend registers");
    ck(arkrtos_blk_count() == ARKRTOS_BLK_MAX_DEVS,
       "the table holds exactly ARKRTOS_BLK_MAX_DEVS backends");
    ck(arkrtos_blk_register(&g_dev_e) == ARKRTOS_BLK_ERR_NOSLOT,
       "one backend past the table size is refused");
    ck(arkrtos_blk_count() == ARKRTOS_BLK_MAX_DEVS,
       "the refused backend did not take a slot");
    ck(arkrtos_blk_at(ARKRTOS_BLK_MAX_DEVS) == 0,
       "at(table size) is NULL");
    ck(arkrtos_blk_at(0xffffffffu) == 0, "at(huge) is NULL");
    ck(arkrtos_blk_find("nor0") == &g_dev_c,
       "a backend registered last is still findable");
    ck(arkrtos_blk_find("mmc0") == &g_dev_d,
       "the last slot is findable by name");
}

int main(void)
{
    test_register_rejects();
    test_register_find();
    test_init();
    test_size();
    test_access_guards();
    test_table_full();

    if(g_fails != 0)
    {
        printf("BLK-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("BLK-HOST OK %d checks\n", g_checks);
    return 0;
}

#else   /* ARKRTOS_USE_BLK == 0 */

static arkrtos_blk_dev_t g_dev = {
    "int0", 0x1000u, 0x100u, 0, 0, 0, 0, 0
};

int main(void)
{
    uint8 buf[4];

    /* 关档时这一层不接入任何后端：每个入口都必须拒绝，且不碰指针 */
    ck(arkrtos_blk_register(&g_dev) == ARKRTOS_BLK_ERR_DISABLED,
       "register is refused when the layer is compiled out");
    ck(arkrtos_blk_count() == 0u, "count is zero when the layer is compiled out");
    ck(arkrtos_blk_at(0) == 0, "at() is NULL when the layer is compiled out");
    ck(arkrtos_blk_find("int0") == 0,
       "find() is NULL when the layer is compiled out");
    ck(arkrtos_blk_init(&g_dev) == ARKRTOS_BLK_ERR_DISABLED,
       "init is refused when the layer is compiled out");
    ck(arkrtos_blk_is_ready(&g_dev) == 0u,
       "nothing is ready when the layer is compiled out");
    ck(arkrtos_blk_size(&g_dev) == 0u,
       "size is zero when the layer is compiled out");
    ck(arkrtos_blk_read(&g_dev, 0u, buf, 4u) == ARKRTOS_BLK_ERR_DISABLED,
       "read is refused when the layer is compiled out");
    ck(arkrtos_blk_write(&g_dev, 0u, buf, 4u) == ARKRTOS_BLK_ERR_DISABLED,
       "write is refused when the layer is compiled out");
    ck(arkrtos_blk_erase(&g_dev, 0u, 0x100u) == ARKRTOS_BLK_ERR_DISABLED,
       "erase is refused when the layer is compiled out");

    if(g_fails != 0)
    {
        printf("BLK-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("BLK-HOST OFF OK %d checks\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_BLK */
