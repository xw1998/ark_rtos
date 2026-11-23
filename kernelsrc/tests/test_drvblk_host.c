/**
* @file test_drvblk_host.c
* @brief board/stm32f427/drvblk.c 的宿主用例：块设备注册 + 两条适配器
* @details 这个文件只有两个薄适配器，但它们是「装载器 / 文件系统 / shell」
*          与「内部 Flash 池 / 外部 SPI NOR」之间唯一的缝。宿主上钉住三件事：
*
*            1. 注册时**不碰硬件**：两个描述符的字段（名字、容量口径、擦除
*               单位、bringup / size_fn / 三个 IO 指针）逐项与契约一致，且
*               调用 init 不会触发任何 Flash 或 SPI 访问；
*            2. int0 的地址算术：读是 memcpy、写擦必须换算成
*               IMAGE_POOL_BASE + off 再交给 port 层，长度与数据原样透传；
*            3. nor0 是纯转发：五个入口原样交给 drvnor，容量由芯片回答
*               （描述符里的 size 恒 0，尺寸必须问 size_fn）。
*
*          内部池用 VirtualAlloc / mmap MAP_FIXED 真映射到 IMAGE_POOL_BASE
*          （F427 上就是 0x08060000）。映射失败直接报错退出，绝不跳过--
*          跳过会让 memcpy 断言在非法地址上跑，读起来像通过。
*
*          **不证明**真 Flash 控制器时序、真 SPI 收发、真擦写耗时与掉电窗口：
*          port 层与 drvnor 在这里都是桩，用例证明的是「这一层把什么交给了
*          谁」，不是「芯片真的写进去了」。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_types.h"
#include "arkrtos_blk.h"
#include "arkrtos_partition.h"
#include "arkrtos_hal.h"
#include "drvblk.h"
#include "drvnor.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n", what, got, want, __LINE__);
    }
}

/* ---------------- 绝对地址映射 ---------------- */

static int map_at(unsigned long base, unsigned long size)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)base, (SIZE_T)size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return (p == (void *)(uintptr_t)base) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)base, (size_t)size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    return (p == MAP_FAILED) ? -1 : 0;
#endif
}

/* ---------------- 桩：块设备注册表 ---------------- */

static arkrtos_blk_dev_t g_regs[ARKRTOS_BLK_MAX_DEVS];
static int g_reg_n;
static int g_reg_fail_at = -1;

int32 arkrtos_blk_register(const arkrtos_blk_dev_t *dev)
{
    int idx = g_reg_n;

    g_reg_n++;
    if(dev == 0)
    {
        return -1;
    }
    if(idx == g_reg_fail_at)
    {
        return -1;
    }
    if(idx >= (int)ARKRTOS_BLK_MAX_DEVS)
    {
        return -1;
    }
    g_regs[idx] = *dev;
    return 0;
}

/* ---------------- 桩：内部 Flash 端口层 ---------------- */

static int g_pf_write_calls;
static int g_pf_erase_calls;
static uint32 g_pf_addr;
static uint32 g_pf_len;
static const uint8 *g_pf_data;
static int32 g_pf_rc;

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    g_pf_write_calls++;
    g_pf_addr = addr;
    g_pf_len = len;
    g_pf_data = data;
    return g_pf_rc;
}

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    g_pf_erase_calls++;
    g_pf_addr = addr;
    g_pf_len = size;
    return g_pf_rc;
}

/* ---------------- 桩：外部 SPI NOR ---------------- */

static int g_nor_init_calls;
static int g_nor_read_calls;
static int g_nor_write_calls;
static int g_nor_erase_calls;
static uint32 g_nor_addr;
static uint32 g_nor_len;
static const uint8 *g_nor_data;
static uint32 g_nor_capacity_value = 16u * 1024u * 1024u;
static int32 g_nor_rc;

int32 arkrtos_nor_init(void)
{
    g_nor_init_calls++;
    return g_nor_rc;
}

uint8 arkrtos_nor_present(void)
{
    return 1u;
}

uint32 arkrtos_nor_jedec_id(void)
{
    return 0xEF4018u;
}

uint32 arkrtos_nor_capacity(void)
{
    return g_nor_capacity_value;
}

uint32 arkrtos_nor_sector_size(void)
{
    return (uint32)ARKRTOS_NOR_SECTOR_BYTES;
}

int32 arkrtos_nor_read(uint32 addr, uint8 *buf, uint32 len)
{
    g_nor_read_calls++;
    g_nor_addr = addr;
    g_nor_len = len;
    if(buf != 0)
    {
        memset(buf, 0x5A, (size_t)len);
    }
    return g_nor_rc;
}

int32 arkrtos_nor_write(uint32 addr, const uint8 *data, uint32 len)
{
    g_nor_write_calls++;
    g_nor_addr = addr;
    g_nor_len = len;
    g_nor_data = data;
    return g_nor_rc;
}

int32 arkrtos_nor_erase_sector(uint32 addr)
{
    (void)addr;
    return 0;
}

int32 arkrtos_nor_erase(uint32 addr, uint32 size)
{
    g_nor_erase_calls++;
    g_nor_addr = addr;
    g_nor_len = size;
    return g_nor_rc;
}

int32 arkrtos_nor_wait_ready(uint32 timeout_ms)
{
    (void)timeout_ms;
    return 0;
}

/* ---------------- 夹具 ---------------- */

static void reset_all(void)
{
    memset(g_regs, 0, sizeof(g_regs));
    g_reg_n = 0;
    g_reg_fail_at = -1;

    g_pf_write_calls = 0;
    g_pf_erase_calls = 0;
    g_pf_addr = 0u;
    g_pf_len = 0u;
    g_pf_data = 0;
    g_pf_rc = 0;

    g_nor_init_calls = 0;
    g_nor_read_calls = 0;
    g_nor_write_calls = 0;
    g_nor_erase_calls = 0;
    g_nor_addr = 0u;
    g_nor_len = 0u;
    g_nor_data = 0;
    g_nor_rc = 0;
}

/* ---------------- 用例 ---------------- */

/* 注册成功路径：两个描述符的每个字段都按契约填好，且 init 不碰硬件。 */
static void test_register(void)
{
    int32 rc;
    const arkrtos_blk_dev_t *in;
    const arkrtos_blk_dev_t *nor;

    reset_all();
    rc = arkrtos_board_blk_init();

    ck(rc == 0, "both backends register");
    ck_u((unsigned long)g_reg_n, 2u, "two descriptors passed to the registry");
    ck_u((unsigned long)g_pf_write_calls, 0u, "registration does not write flash");
    ck_u((unsigned long)g_pf_erase_calls, 0u, "registration does not erase flash");
    ck_u((unsigned long)g_nor_init_calls, 0u, "registration does not touch SPI");

    in = &g_regs[0];
    nor = &g_regs[1];

    ck(in->name != 0 && strcmp(in->name, "int0") == 0, "internal device name is int0");
    ck_u((unsigned long)in->size, (unsigned long)IMAGE_POOL_SIZE, "internal size is the pool size");
    ck_u((unsigned long)in->erase_unit, (unsigned long)IMAGE_POOL_SECTOR, "internal erase unit is one sector");
    ck(in->bringup == 0, "internal device needs no bringup (always mapped)");
    ck(in->size_fn == 0, "internal size is a compile-time constant");
    ck(in->read != 0 && in->write != 0 && in->erase != 0, "internal IO pointers are set");

    ck(nor->name != 0 && strcmp(nor->name, "nor0") == 0, "external device name is nor0");
    ck_u((unsigned long)nor->size, 0u, "external size is unknown until the chip answers");
    ck_u((unsigned long)nor->erase_unit, (unsigned long)ARKRTOS_NOR_SECTOR_BYTES,
         "external erase unit is the NOR sector");
    ck(nor->bringup == arkrtos_nor_init, "external bringup is the NOR init");
    ck(nor->size_fn == arkrtos_nor_capacity, "external size is answered by the chip");
    ck(nor->read != 0 && nor->write != 0 && nor->erase != 0, "external IO pointers are set");
}

/* 注册失败：任何一个被拒都返回 -1，但另一个仍然尝试注册（不静默短路）。 */
static void test_register_failure(void)
{
    int32 rc;

    reset_all();
    g_reg_fail_at = 0;
    rc = arkrtos_board_blk_init();

    ck(rc == -1, "first refusal makes init fail");
    ck_u((unsigned long)g_reg_n, 2u, "second descriptor is still offered after a refusal");
    ck(g_regs[1].name != 0 && strcmp(g_regs[1].name, "nor0") == 0,
       "the second descriptor that survived is nor0");

    reset_all();
    g_reg_fail_at = 1;
    rc = arkrtos_board_blk_init();

    ck(rc == -1, "second refusal makes init fail");
    ck_u((unsigned long)g_reg_n, 2u, "both descriptors were offered");
    ck(g_regs[0].name != 0 && strcmp(g_regs[0].name, "int0") == 0,
       "the first descriptor that survived is int0");
}

/* 内部池读：memcpy 自 IMAGE_POOL_BASE + off，不经过 port 层。 */
static void test_internal_read(void)
{
    uint8 *pool = (uint8 *)(uintptr_t)IMAGE_POOL_BASE;
    uint8 out[16];
    int32 rc;

    reset_all();
    ck(arkrtos_board_blk_init() == 0, "init for read test");

    pool[0x1234u] = 0xA5u;
    memset(&pool[0x1234u], 0x3C, sizeof(out));

    memset(out, 0, sizeof(out));
    rc = g_regs[0].read(0x1234u, out, (uint32)sizeof(out));

    ck(rc == 0, "internal read succeeds");
    ck(out[0] == 0x3C && out[sizeof(out) - 1u] == 0x3C, "internal read copies from IMAGE_POOL_BASE + off");
    ck_u((unsigned long)g_pf_write_calls, 0u, "read does not go through the port write path");
}

/* 内部池写 / 擦：地址必须换算成 IMAGE_POOL_BASE + off，参数原样透传。 */
static void test_internal_write_erase(void)
{
    uint8 data[8];
    int32 rc;

    reset_all();
    ck(arkrtos_board_blk_init() == 0, "init for write test");
    memset(data, 0x11, sizeof(data));

    rc = g_regs[0].write(0x200u, data, (uint32)sizeof(data));
    ck(rc == 0, "internal write forwards to the port layer");
    ck_u((unsigned long)g_pf_write_calls, 1u, "one port write call");
    ck_u((unsigned long)g_pf_addr, (unsigned long)(IMAGE_POOL_BASE + 0x200u),
         "write address is IMAGE_POOL_BASE + off");
    ck_u((unsigned long)g_pf_len, (unsigned long)sizeof(data), "write length is passed through");
    ck(g_pf_data == data, "write keeps the caller buffer, no copy");

    rc = g_regs[0].erase(0x400u, (uint32)IMAGE_POOL_SECTOR);
    ck(rc == 0, "internal erase forwards to the port layer");
    ck_u((unsigned long)g_pf_erase_calls, 1u, "one port erase call");
    ck_u((unsigned long)g_pf_addr, (unsigned long)(IMAGE_POOL_BASE + 0x400u),
         "erase address is IMAGE_POOL_BASE + off");
    ck_u((unsigned long)g_pf_len, (unsigned long)IMAGE_POOL_SECTOR, "erase length is passed through");

    g_pf_rc = -1;
    rc = g_regs[0].write(0u, data, 1u);
    ck(rc == -1, "port failure is returned unchanged (write)");
    rc = g_regs[0].erase(0u, (uint32)IMAGE_POOL_SECTOR);
    ck(rc == -1, "port failure is returned unchanged (erase)");
}

/* 外部 NOR：五个入口都是纯转发，容量问 size_fn。 */
static void test_nor_forwarding(void)
{
    uint8 buf[4];
    uint8 data[4];
    int32 rc;

    reset_all();
    ck(arkrtos_board_blk_init() == 0, "init for nor test");
    memset(buf, 0, sizeof(buf));
    memset(data, 0x22, sizeof(data));

    rc = g_regs[1].bringup();
    ck(rc == 0, "nor bringup returns the driver result");
    ck_u((unsigned long)g_nor_init_calls, 1u, "bringup reaches arkrtos_nor_init");

    ck_u((unsigned long)g_regs[1].size_fn(), (unsigned long)g_nor_capacity_value,
         "size_fn returns the live chip capacity");

    rc = g_regs[1].read(0x10u, buf, (uint32)sizeof(buf));
    ck(rc == 0, "nor read returns the driver result");
    ck_u((unsigned long)g_nor_read_calls, 1u, "read reaches arkrtos_nor_read");
    ck_u((unsigned long)g_nor_addr, 0x10u, "nor read offset is passed through");
    ck_u((unsigned long)g_nor_len, (unsigned long)sizeof(buf), "nor read length is passed through");
    ck(buf[0] == 0x5A, "buffer is the caller's, filled by the driver");

    rc = g_regs[1].write(0x20u, data, (uint32)sizeof(data));
    ck(rc == 0, "nor write returns the driver result");
    ck_u((unsigned long)g_nor_write_calls, 1u, "write reaches arkrtos_nor_write");
    ck_u((unsigned long)g_nor_addr, 0x20u, "nor write offset is passed through");
    ck_u((unsigned long)g_nor_len, (unsigned long)sizeof(data), "nor write length is passed through");
    ck(g_nor_data == data, "nor write keeps the caller buffer");

    rc = g_regs[1].erase(0x8000u, (uint32)ARKRTOS_NOR_SECTOR_BYTES);
    ck(rc == 0, "nor erase returns the driver result");
    ck_u((unsigned long)g_nor_erase_calls, 1u, "erase reaches arkrtos_nor_erase");
    ck_u((unsigned long)g_nor_addr, 0x8000u, "nor erase offset is passed through");
    ck_u((unsigned long)g_nor_len, (unsigned long)ARKRTOS_NOR_SECTOR_BYTES,
         "nor erase length is passed through");

    g_nor_rc = -1;
    rc = g_regs[1].read(0u, buf, 1u);
    ck(rc == -1, "nor failure is returned unchanged");
}

/* ---------------- main ---------------- */

int main(void)
{
    if(map_at((unsigned long)IMAGE_POOL_BASE, (unsigned long)IMAGE_POOL_SIZE) != 0)
    {
        printf("DRVBLK-HOST FAIL: cannot map the image pool at 0x%08lX\n",
               (unsigned long)IMAGE_POOL_BASE);
        return 1;
    }

    test_register();
    test_register_failure();
    test_internal_read();
    test_internal_write_erase();
    test_nor_forwarding();

    if(g_fails != 0)
    {
        printf("DRVBLK-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("DRVBLK-HOST OK  %d checks\n", g_checks);
    return 0;
}