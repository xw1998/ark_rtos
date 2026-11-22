/**
* @file test_layout_host.c
* @brief 设备端布局解析（arkrtos_layout.c）的宿主用例
* @details 被测对象是「把设备端配置区里的一条记录读出来、校验、采用，并发布到
*          共享分区表」这一段。它自己不做 Flash 时序：配置区是绝对地址
*          CONFIG_BASE 起的一整个擦除单元，所以宿主上先把那段区间真映射出来
*          （Windows VirtualAlloc / POSIX mmap MAP_FIXED），再用两个 RAM 后端
*          桩（erase / write）把它变成一块可写的「Flash」。映射失败直接报错
*          退出，绝不跳过——跳过会让断言在空地址上跑，读起来像通过。
*
*          钉住的是这些容易只在现场暴露的行为：
*            - 记录校验的**优先级**与原因码：magic 先于 version，version 先于
*              CRC，CRC 先于硬件签名；槽位字段、重叠、运行期纽子各有自己的码；
*            - 「配置里写了但内核没接」的字段必须当场拒绝（NOT_IMPL），不默默
*              当 0 用；保留字非 0 同样拒绝（记录来自更新的工具）；
*            - 扫描只认「顺序追加」：遇到擦除态就停，半写或陌生内容跳过，
*              有效记录里取 seq 最大的那条；
*            - 启动期无论如何先落地编译期默认布局：坏记录只写原因码，不让设备
*              无布局可用；
*            - 写入由内核盖章 seq 与硬件签名、重算 CRC，写完回读校验，成功后
*              立即生效（不重启就能看到新布局）；
*            - 区满只擦一次再从头写（p_erased 置 1），擦失败/写失败/回读不符
*              各有自己的返回码，且失败时**不**采纳半成品。
*
*          CRC32 与内核别的模块共用（定义在 arkrtos_loader.c）。这里不链接
*          loader 整个模块，而是在用例里放一份**表驱动**实现并在开头用 zlib
*          的已知答案（空串 0、123456789 -> CBF43926）与累积等价性自证；
*          被测代码算出来的值必须与它逐位相同，否则所有记录都校验不过。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_partition.h"
#include "arkrtos_layout.h"
#include "arkrtos_layout_def.h"
#include "arkrtos_ptable.h"
#include "arkrtos_hal.h"
#include "arkrtos_app_image.h"
#include "arkrtos_log.h"
#include "arkrtos_log_defs.h"

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

static void ck_s(const char *got, const char *want, const char *what)
{
    g_checks++;
    if((got == 0) || (strcmp(got, want) != 0))
    {
        g_fails++;
        printf("FAIL %s: got \"%s\" want \"%s\" (line %d)\n",
               what, (got == 0) ? "(null)" : got, want, __LINE__);
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

/* ---------------- CRC32（IEEE 802.3，zlib 语义）---------------- */
/* 表驱动实现：与本仓库 loader.c 的逐位实现是同一个函数的两种写法，
 * 算不出来的话所有记录都会校验不过，不会静默放过。 */

static uint32 crc_table[256];
static int    crc_table_ready;

static void crc_table_init(void)
{
    uint32 i;
    uint32 j;
    uint32 c;

    for(i = 0u; i < 256u; i++)
    {
        c = i;
        for(j = 0u; j < 8u; j++)
        {
            c = ((c & 1u) != 0u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 c;
    uint32 i;

    if(crc_table_ready == 0)
    {
        crc_table_init();
    }
    c = crc ^ 0xFFFFFFFFu;
    for(i = 0u; i < len; i++)
    {
        c = crc_table[(c ^ (uint32)p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- 端口桩 ---------------- */

static uint8 *g_flash;                 /* 指向已映射的 CONFIG 区 */
static int    g_erase_ret;
static int    g_write_ret;
static int    g_write_corrupt;
static int    g_erase_calls;
static int    g_write_calls;
static uint32 g_last_erase_addr;
static uint32 g_last_erase_size;
static uint32 g_last_write_addr;
static uint32 g_last_write_len;

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    g_erase_calls++;
    g_last_erase_addr = addr;
    g_last_erase_size = size;
    if(g_erase_ret != 0)
    {
        return (int32)g_erase_ret;
    }
    if((addr < (uint32)CONFIG_BASE) || ((addr + size) > ((uint32)CONFIG_BASE + (uint32)CONFIG_SIZE)))
    {
        return -1;
    }
    memset(g_flash + (addr - (uint32)CONFIG_BASE), 0xFF, (size_t)size);
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    g_write_calls++;
    g_last_write_addr = addr;
    g_last_write_len = len;
    if(g_write_ret != 0)
    {
        return (int32)g_write_ret;
    }
    if((addr < (uint32)CONFIG_BASE) || ((addr + len) > ((uint32)CONFIG_BASE + (uint32)CONFIG_SIZE)))
    {
        return -1;
    }
    memcpy(g_flash + (addr - (uint32)CONFIG_BASE), data, (size_t)len);
    if(g_write_corrupt != 0)
    {
        g_flash[(addr - (uint32)CONFIG_BASE) + 8u] ^= 0x01u;
    }
    return 0;
}

static arkrtos_partition_table_t g_pt;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

static uint32 g_log_level;
static int    g_log_calls;

void arkrtos_log_set_level(uint32 level)
{
    g_log_level = level;
    g_log_calls++;
}

/* ---------------- 记录构造 ---------------- */

static arkrtos_cfg_record_t *rec_at(uint32 index)
{
    return (arkrtos_cfg_record_t *)(uintptr_t)
           ((uint32)CONFIG_BASE + (index * ARKRTOS_CFG_RECORD_SIZE));
}

static void store_at(uint32 index, const arkrtos_cfg_record_t *r)
{
    memcpy((void *)(uintptr_t)((uint32)CONFIG_BASE + (index * ARKRTOS_CFG_RECORD_SIZE)),
           (const void *)r, (size_t)ARKRTOS_CFG_RECORD_SIZE);
}

static void rec_seal(arkrtos_cfg_record_t *r)
{
    r->crc32 = arkrtos_layout_crc(r);
}

static void rec_base(arkrtos_cfg_record_t *r, uint32 mode, uint32 seq)
{
    memset((void *)r, 0, sizeof(*r));
    r->magic        = ARKRTOS_CFG_MAGIC;
    r->version      = ARKRTOS_CFG_VERSION;
    r->hw_compat_id = ARKRTOS_HW_COMPAT_ID;
    r->mode         = mode;
    r->seq          = seq;
    r->reclaim_mode = ARKRTOS_CFG_RECLAIM_GLOBAL;
}

static void rec_auto(arkrtos_cfg_record_t *r, uint32 seq)
{
    rec_base(r, ARKRTOS_LAYOUT_MODE_AUTO, seq);
    rec_seal(r);
}

/* 一个合法的固定槽：整扇区、落在池首、RAM 窗口 4K 对齐 */
static void slot_fixed(arkrtos_cfg_slot_t *s, uint32 unit_index)
{
    memset((void *)s, 0, sizeof(*s));
    s->base     = (uint32)IMAGE_POOL_BASE + (unit_index * (uint32)IMAGE_POOL_SECTOR);
    s->size     = (uint32)IMAGE_POOL_SECTOR;
    s->type     = ARKRTOS_CFG_TYPE_APP;
    s->ram_base = (uint32)SLOT_RAM_BASE + (unit_index * 4096u);
    s->ram_size = 4096u;
    s->autostart = 1u;
}

static void rec_fixed(arkrtos_cfg_record_t *r, uint32 seq, uint32 slots)
{
    uint32 i;

    rec_base(r, ARKRTOS_LAYOUT_MODE_FIXED, seq);
    r->slot_count = slots;
    for(i = 0u; i < slots; i++)
    {
        slot_fixed(&r->slot[i], i);
    }
    rec_seal(r);
}

static void flash_blank(void)
{
    memset(g_flash, 0xFF, (size_t)CONFIG_SIZE);
}

/* ---------------- 用例：CRC ---------------- */

static void test_crc(void)
{
    arkrtos_cfg_record_t r;
    uint32 c1;
    uint32 c2;

    /* zlib 已知答案：不先自证这个函数，下面的记录校验全是自说自话 */
    ck_u((unsigned long)arkrtos_crc32("", 0u, 0u), 0ul, "crc32 of empty input is 0");
    ck_u((unsigned long)arkrtos_crc32("123456789", 9u, 0u), 0xCBF43926ul,
         "crc32 of 123456789 is the zlib vector");
    c1 = arkrtos_crc32("1234", 4u, 0u);
    c2 = arkrtos_crc32("56789", 5u, c1);
    ck_u((unsigned long)c2, 0xCBF43926ul, "crc32 accumulates across segments");

    /* 记录的 CRC 把最后一个字当 0 看待：改 crc 字段本身不影响计算结果 */
    rec_auto(&r, 1u);
    c1 = arkrtos_layout_crc(&r);
    r.crc32 = 0xDEADBEEFu;
    ck_u((unsigned long)arkrtos_layout_crc(&r), (unsigned long)c1,
         "the crc word itself is taken as zero");
    ck(c1 != 0u, "crc of a real record is not zero");
    /* 改任何有效字节都会改变结果 */
    r.seq = 2u;
    ck(arkrtos_layout_crc(&r) != c1, "a changed field changes the crc");
}

/* ---------------- 用例：整条记录的头部校验 ---------------- */

static void test_validate_header(void)
{
    arkrtos_cfg_record_t r;

    ck_u((unsigned long)arkrtos_layout_validate(0), (unsigned long)ARKRTOS_CFG_ERR_MAGIC,
         "a null record is a bad magic, not a crash");

    rec_auto(&r, 1u);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul, "a minimal AUTO record is valid");

    rec_auto(&r, 1u);
    r.magic = 0u;
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_MAGIC, "bad magic");

    rec_auto(&r, 1u);
    r.version = 99u;
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_VERSION, "bad version");

    rec_auto(&r, 1u);
    r.crc32 ^= 1u;
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_CRC, "bad crc");

    rec_auto(&r, 1u);
    r.hw_compat_id = 0u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_HW, "hw compat mismatch");

    rec_auto(&r, 1u);
    r.mode = 7u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_MODE, "bad mode");

    rec_auto(&r, 1u);
    r.slot_count = (uint32)ARKRTOS_CFG_SLOT_MAX + 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_COUNT_MAX,
         "more slots than the kernel can hold");

    /* 头部先于槽位：magic 坏了就没必要再看槽位 */
    rec_fixed(&r, 1u, 1u);
    r.magic = 0u;
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_MAGIC,
         "the header is judged before the slot table");

    /* AUTO 不许带槽表；FIXED 至少要有一个可用槽 */
    rec_fixed(&r, 1u, 1u);
    r.mode = ARKRTOS_LAYOUT_MODE_AUTO;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOTS_IN_MODE,
         "AUTO mode must not carry a slot table");

    rec_fixed(&r, 1u, 0u);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOTS_IN_MODE,
         "FIXED mode needs at least one usable slot");

    /* 只有一个合法固定槽时也必须通过 */
    rec_fixed(&r, 1u, 1u);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul, "a one-slot FIXED record is valid");
}

/* ---------------- 用例：槽位字段与重叠 ---------------- */

static void test_validate_slots(void)
{
    arkrtos_cfg_record_t r;

    rec_fixed(&r, 1u, 1u);
    r.slot[0].type = ARKRTOS_CFG_TYPE_UNUSED;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOTS_IN_MODE,
         "an all-unused table is the same as no table for FIXED");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].type = 9u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_TYPE, "bad slot type");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].flags = 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_RESERVED,
         "per-slot flags are reserved");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].reserved = 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_RESERVED,
         "per-slot reserved word must be zero");

    /* Flash 区间：池外（低）、池尾、越过池尾 */
    rec_fixed(&r, 1u, 1u);
    r.slot[0].base = (uint32)IMAGE_POOL_BASE - (uint32)IMAGE_POOL_SECTOR;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RANGE,
         "a slot below the pool is out of range");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].base = (uint32)IMAGE_POOL_END;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RANGE,
         "a slot starting at the pool end is out of range");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].size = ((uint32)IMAGE_POOL_END - (uint32)IMAGE_POOL_BASE) + (uint32)IMAGE_POOL_SECTOR;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RANGE,
         "a slot running past the pool end is out of range");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].size = (uint32)APP_IMAGE_HEADER_SIZE + 255u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_SIZE,
         "a slot smaller than header + 256 is too small");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].size = (uint32)APP_IMAGE_HEADER_SIZE + 256u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_ALIGN,
         "FIXED slots must be whole sectors even when the size is big enough");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].base = (uint32)IMAGE_POOL_BASE + (uint32)ARKRTOS_POOL_ALLOC_UNIT;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_ALIGN,
         "FIXED slot bases must be whole sectors");

    /* 连分配粒度都不对齐：这条先于「整扇区」被报出来 */
    rec_fixed(&r, 1u, 1u);
    r.slot[0].base = (uint32)IMAGE_POOL_BASE + 512u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_ALIGN,
         "a base not aligned to the allocation unit is refused");

    /* 槽表中间留一个空项：空项不参与重叠判定，后面的槽仍然有效 */
    rec_fixed(&r, 1u, 2u);
    r.slot[0].type = ARKRTOS_CFG_TYPE_UNUSED;
    r.slot[1].base = (uint32)IMAGE_POOL_BASE;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul,
         "an unused hole in the table does not block the slot after it");

    /* RAM 窗口 */
    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_base = 0u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "FIXED needs a pinned RAM window");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_size = 0u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a zero RAM window is not pinned");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_size = 3000u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window must be a power of two");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_size = (uint32)SLOT_RAM_MIN_BLOCK / 2u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window below the minimum block is refused");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_size = (uint32)SLOT_RAM_MAX_BLOCK * 2u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window above the maximum block is refused");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_base = (uint32)SLOT_RAM_BASE + 1024u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window must start on its own size");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_base = (uint32)SLOT_RAM_BASE - 4096u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window below the pool is refused");

    rec_fixed(&r, 1u, 1u);
    r.slot[0].ram_base = (uint32)SLOT_RAM_BASE + (uint32)SLOT_RAM_TOTAL - 2048u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_RAM,
         "a RAM window running past the pool end is refused");

    /* 重叠：Flash 与 RAM 各一次；相邻不算重叠 */
    rec_fixed(&r, 1u, 2u);
    r.slot[1].base = r.slot[0].base;      /* 同一个扇区：两边都对齐，只能判重叠 */
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_OVERLAP,
         "overlapping flash ranges are refused");

    rec_fixed(&r, 1u, 2u);
    r.slot[1].base     = r.slot[0].base + (uint32)IMAGE_POOL_SECTOR;
    r.slot[1].ram_base = r.slot[0].ram_base;
    r.slot[1].ram_size = r.slot[0].ram_size;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_SLOT_OVERLAP,
         "overlapping RAM windows are refused");

    rec_fixed(&r, 1u, 2u);
    r.slot[1].base     = r.slot[0].base + (uint32)IMAGE_POOL_SECTOR;
    r.slot[1].ram_base = r.slot[0].ram_base + 4096u;
    r.slot[1].ram_size = 4096u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul,
         "two adjacent slots do not overlap");

    /* 槽表里超过 slot_count 的部分不参与判定 */
    rec_fixed(&r, 1u, 1u);
    r.slot[1].base = r.slot[0].base;
    r.slot[1].type = ARKRTOS_CFG_TYPE_APP;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul,
         "entries beyond slot_count are ignored");
}

/* ---------------- 用例：运行期纽子 ---------------- */

static void test_validate_knobs(void)
{
    arkrtos_cfg_record_t r;

    rec_auto(&r, 1u);
    r.reclaim_mode = 9u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_RECLAIM, "bad reclaim mode");

    rec_auto(&r, 1u);
    r.log_level = (uint32)ARKRTOS_LOG_DEBUG + 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_KNOB,
         "a log level above DEBUG is refused");

    rec_auto(&r, 1u);
    r.fault_restart_max = 65u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_KNOB,
         "a restart budget above 64 is refused");

    rec_auto(&r, 1u);
    r.flags = 0x2u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_RESERVED,
         "unknown record flags are refused rather than ignored");

#if (APP_ALLOW_RAW_IMAGE != 1)
    rec_auto(&r, 1u);
    r.flags = ARKRTOS_CFG_FLAG_RAW_ALLOW;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_NOT_IMPL,
         "asking for the raw-image path in a build without it is refused");
#endif

    rec_auto(&r, 1u);
    r.boot_delay_ms = (uint32)ARKRTOS_CFG_BOOT_DELAY_MAX + 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_KNOB,
         "an endless boot delay is refused");

    rec_auto(&r, 1u);
    r.watchdog_ms = 1000u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_NOT_IMPL,
         "a knob this kernel does not implement is refused, not ignored (watchdog)");

    rec_auto(&r, 1u);
    r.heap_size = 4096u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_NOT_IMPL,
         "the kernel owns no heap, so heap_size must be 0");

    rec_auto(&r, 1u);
    r.thread_stack_default = 512u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_NOT_IMPL,
         "stacks are caller-declared, so thread_stack_default must be 0");

    rec_auto(&r, 1u);
    r.reserved[47] = 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), (unsigned long)ARKRTOS_CFG_ERR_RESERVED,
         "a record written by a newer tool is refused");

    /* 边界值本身必须通过 */
    rec_auto(&r, 1u);
    r.log_level = (uint32)ARKRTOS_LOG_DEBUG;
    r.fault_restart_max = 64u;
    r.boot_delay_ms = (uint32)ARKRTOS_CFG_BOOT_DELAY_MAX;
    r.reclaim_mode = ARKRTOS_CFG_RECLAIM_MINIMAL;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul,
         "the documented upper bounds are still accepted");

    rec_auto(&r, 1u);
    r.tool_version = 7u;                /* 只作参考，不参与判定 */
    r.log_level = 1u;
    r.fault_restart_max = 1u;
    r.boot_delay_ms = 1u;
    rec_seal(&r);
    ck_u((unsigned long)arkrtos_layout_validate(&r), 0ul,
         "tool_version is informational only");
}

/* ---------------- 用例：原因码文案 ---------------- */

static void test_reason_text(void)
{
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_OK), "ok", "reason 0");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_MAGIC), "bad magic", "reason magic");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_VERSION), "bad version", "reason version");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_CRC), "bad crc", "reason crc");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_HW), "hw compat mismatch", "reason hw");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_MODE), "bad mode", "reason mode");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_COUNT), "bad slot count", "reason count");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_TYPE), "bad slot type", "reason slot type");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_RANGE), "slot outside pool", "reason range");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_SIZE), "slot too small", "reason size");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_ALIGN), "slot not aligned", "reason align");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_OVERLAP), "slots overlap", "reason overlap");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_RAM), "bad ram window", "reason ram");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOT_COUNT_MAX), "too many slots", "reason too many");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_RECLAIM), "bad reclaim mode", "reason reclaim");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_KNOB), "runtime knob out of range", "reason knob");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_NOT_IMPL), "field not implemented by this kernel",
         "reason not impl");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_RESERVED), "reserved words not zero", "reason reserved");
    ck_s(arkrtos_layout_reason_text(ARKRTOS_CFG_ERR_SLOTS_IN_MODE), "slot table does not match the mode",
         "reason slots in mode");
    ck_s(arkrtos_layout_reason_text(999u), "unknown", "an unknown reason is named as such");
}

/* ---------------- 用例：读取（扫描）---------------- */

static void test_read(void)
{
    arkrtos_cfg_record_t r;
    arkrtos_cfg_record_t out;
    uint32 seq = 0u;
    uint32 i;

    flash_blank();
    ck_u((unsigned long)arkrtos_layout_read(0, &seq), (unsigned long)ARKRTOS_LAYOUT_ERR_ARG,
         "read needs somewhere to put the record");

    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), (unsigned long)ARKRTOS_LAYOUT_ERR_INVALID,
         "an erased region has no record");

    rec_auto(&r, 5u);
    store_at(0u, &r);
    memset(&out, 0, sizeof(out));
    seq = 0u;
    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), 0ul, "one record is readable");
    ck_u((unsigned long)seq, 5ul, "the sequence number comes back");
    ck_u((unsigned long)out.magic, (unsigned long)ARKRTOS_CFG_MAGIC, "the record is copied out");
    ck((&out != rec_at(0u)), "the caller gets a copy, not the flash address");

    /* 追加第二条：取 seq 大的那条 */
    rec_auto(&r, 9u);
    store_at(1u, &r);
    seq = 0u;
    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), 0ul, "two records, still readable");
    ck_u((unsigned long)seq, 9ul, "the newest sequence wins");

    /* 坏 CRC 的记录被跳过；坏魔数的也跳过；两者都不能挡住后面那条 */
    flash_blank();
    rec_auto(&r, 1u);
    r.crc32 ^= 1u;
    store_at(0u, &r);
    rec_auto(&r, 2u);
    r.magic = 0u;
    store_at(1u, &r);
    rec_auto(&r, 3u);
    store_at(2u, &r);
    seq = 0u;
    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), 0ul, "bad records are skipped");
    ck_u((unsigned long)seq, 3ul, "the valid record after them is still found");

    /* 顺序追加：擦除态之后的内容不再属于这块区 */
    flash_blank();
    rec_auto(&r, 4u);
    store_at(0u, &r);
    store_at(1u, &r);            /* 故意留成擦除态：先清掉 */
    flash_blank();
    rec_auto(&r, 4u);
    store_at(0u, &r);
    rec_auto(&r, 8u);
    store_at(2u, &r);
    seq = 0u;
    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), 0ul, "a hole stops the scan");
    ck_u((unsigned long)seq, 4ul, "entries after an erased slot are not considered");

    /* 区里的每条记录都能被读出来（相当于扫描到底）*/
    flash_blank();
    for(i = 0u; i < 4u; i++)
    {
        rec_auto(&r, i + 1u);
        store_at(i, &r);
    }
    seq = 0u;
    ck_u((unsigned long)arkrtos_layout_read(&out, &seq), 0ul, "several records are scanned");
    ck_u((unsigned long)seq, 4ul, "the last appended record wins");
}

/* ---------------- 用例：启动期解析 ---------------- */

static void test_init_default(void)
{
    uint32 i;

    flash_blank();
    memset(&g_pt, 0, sizeof(g_pt));
    g_log_level = 0u;
    g_log_calls = 0;

    arkrtos_layout_init();

    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "no record: the compile-time default is in effect");
    ck_u((unsigned long)arkrtos_layout_mode(), (unsigned long)ARKRTOS_LAYOUT_DEFAULT_MODE,
         "the compile-time default mode");
    ck_u((unsigned long)arkrtos_layout_reason(), 0ul, "no reason code when there is no record");
    ck_u((unsigned long)arkrtos_layout_slot_count(), 0ul, "the default table is empty by default");
    ck_u((unsigned long)arkrtos_layout_reclaim_mode(), (unsigned long)ARKRTOS_RECLAIM_MODE,
         "the default reclaim mode");
    ck_u((unsigned long)arkrtos_layout_cfg_log_level(), 0ul, "no log level requested");
    ck_u((unsigned long)arkrtos_layout_cfg_restart_max(), (unsigned long)APP_CRASH_RESTART_MAX,
         "the compile-time restart budget");
    ck_u((unsigned long)arkrtos_layout_boot_delay_ms(), 0ul, "no boot delay by default");
    ck_u((unsigned long)arkrtos_layout_raw_allow(), (unsigned long)APP_ALLOW_RAW_IMAGE,
         "the compile-time raw-image policy");
    ck_u((unsigned long)g_log_calls, 0ul, "a zero log level leaves the console alone");

    /* 默认表的每条都是「未使用」并且被推到 RAM 池的访问器上 */
    for(i = 0u; i < (uint32)ARKRTOS_CFG_SLOT_MAX; i++)
    {
        const arkrtos_cfg_slot_t *s = arkrtos_layout_slot(i);

        ck(s != 0, "every default slot entry is reachable");
        if(s != 0)
        {
            ck_u((unsigned long)s->type, (unsigned long)ARKRTOS_CFG_TYPE_UNUSED,
                 "the default table has no used entry");
        }
    }
    ck(arkrtos_layout_slot((uint32)ARKRTOS_CFG_SLOT_MAX) == 0, "an out-of-range slot is null");
    ck(arkrtos_layout_slots() != 0, "the table pointer is never null");

    /* 发布到共享分区表：布局字段都是这一段写进去的 */
    ck_u((unsigned long)g_pt.layout_mode, (unsigned long)ARKRTOS_LAYOUT_DEFAULT_MODE,
         "publish writes the layout mode");
    ck_u((unsigned long)g_pt.layout_source, (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "publish writes the layout source");
    ck_u((unsigned long)g_pt.config_base, (unsigned long)CONFIG_BASE, "publish writes config_base");
    ck_u((unsigned long)g_pt.config_size, (unsigned long)CONFIG_SIZE, "publish writes config_size");
    ck_u((unsigned long)g_pt.cfg_slot_count, 0ul, "publish writes the slot count");
    ck_u((unsigned long)g_pt.cfg_restart_max, (unsigned long)APP_CRASH_RESTART_MAX,
         "publish writes the restart budget");
    ck_u((unsigned long)g_pt.cfg_boot_delay_ms, 0ul, "publish writes the boot delay");
    ck_u((unsigned long)g_pt.cfg_raw_allow, (unsigned long)APP_ALLOW_RAW_IMAGE,
         "publish writes the raw-image policy");
}

static void test_init_adopt(void)
{
    arkrtos_cfg_record_t r;

    /* AUTO 记录：日志级别、重启预算、自启等待都要落地，且立刻生效 */
    flash_blank();
    rec_auto(&r, 7u);
    r.log_level = 3u;
    r.fault_restart_max = 5u;
    r.boot_delay_ms = 1500u;
    r.reclaim_mode = ARKRTOS_CFG_RECLAIM_MINIMAL;
    rec_seal(&r);
    store_at(0u, &r);

    memset(&g_pt, 0, sizeof(g_pt));
    g_log_level = 0u;
    g_log_calls = 0;
    arkrtos_layout_init();

    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_CONFIG,
         "a valid record overrides the default");
    ck_u((unsigned long)arkrtos_layout_mode(), (unsigned long)ARKRTOS_LAYOUT_MODE_AUTO,
         "the record's mode is adopted");
    ck_u((unsigned long)arkrtos_layout_reason(), 0ul, "an adopted record leaves no reason");
    ck_u((unsigned long)arkrtos_layout_cfg_log_level(), 3ul, "the log level is adopted");
    ck_u((unsigned long)arkrtos_layout_cfg_restart_max(), 5ul, "the restart budget is adopted");
    ck_u((unsigned long)arkrtos_layout_boot_delay_ms(), 1500ul, "the boot delay is adopted");
    ck_u((unsigned long)arkrtos_layout_reclaim_mode(), (unsigned long)ARKRTOS_CFG_RECLAIM_MINIMAL,
         "AUTO mode keeps the record's reclaim mode");
    ck_u((unsigned long)g_log_calls, 1ul, "the log level is applied exactly once");
    ck_u((unsigned long)g_log_level, 3ul, "the console really got the new level");
    ck_u((unsigned long)g_pt.cfg_log_level, 3ul, "publish carries the log level");
    ck_u((unsigned long)g_pt.cfg_boot_delay_ms, 1500ul, "publish carries the boot delay");

    /* FIXED 记录：只用一个槽；RAM 窗口已钉死因此不需要再分配；
     * 回收力度被强制成 NONE（固定槽从不搬移），重启预算 0 回落到编译期值 */
    flash_blank();
    rec_fixed(&r, 3u, 1u);
    r.reclaim_mode = ARKRTOS_CFG_RECLAIM_MINIMAL;
    r.fault_restart_max = 0u;
    r.log_level = 0u;
    rec_seal(&r);
    store_at(0u, &r);

    memset(&g_pt, 0, sizeof(g_pt));
    g_log_calls = 0;
    arkrtos_layout_init();

    ck_u((unsigned long)arkrtos_layout_mode(), (unsigned long)ARKRTOS_LAYOUT_MODE_FIXED,
         "FIXED mode is adopted");
    ck_u((unsigned long)arkrtos_layout_slot_count(), 1ul, "one usable slot is counted");
    ck_u((unsigned long)arkrtos_layout_reclaim_mode(), (unsigned long)ARKRTOS_CFG_RECLAIM_NONE,
         "FIXED mode never reclaims, whatever the record says");
    ck_u((unsigned long)arkrtos_layout_cfg_restart_max(), (unsigned long)APP_CRASH_RESTART_MAX,
         "a zero restart budget falls back to the compile-time value");
    ck_u((unsigned long)g_log_calls, 0ul, "a zero log level does not touch the console");
    ck_u((unsigned long)g_pt.cfg_slot_count, 1ul, "publish carries the slot count");

    {
        const arkrtos_cfg_slot_t *s = arkrtos_layout_slot(0u);

        ck(s != 0, "the adopted slot is reachable");
        if(s != 0)
        {
            ck_u((unsigned long)s->base, (unsigned long)IMAGE_POOL_BASE, "the slot base is adopted");
            ck_u((unsigned long)s->size, (unsigned long)IMAGE_POOL_SECTOR, "the slot size is adopted");
            ck_u((unsigned long)s->ram_base, (unsigned long)SLOT_RAM_BASE, "the RAM window base is adopted");
            ck_u((unsigned long)s->ram_size, 4096ul, "the RAM window size is adopted");
            ck_u((unsigned long)s->autostart, 1ul, "autostart is adopted");
        }
    }

    /* 同一个基址能在槽表里反查到，池外地址如实报 -1 */
    ck_u((unsigned long)arkrtos_layout_slot_by_base((uint32)IMAGE_POOL_BASE + 8u), 0ul,
         "an address inside the slot maps back to it");
    ck_u((unsigned long)arkrtos_layout_slot_by_base((uint32)IMAGE_POOL_BASE - 1u),
         (unsigned long)-1, "an address below the pool maps to nothing");
    /* 未使用的槽位不参与反查 */
    ck_u((unsigned long)arkrtos_layout_slot_by_base((uint32)IMAGE_POOL_BASE +
         (uint32)IMAGE_POOL_SECTOR + 8u), (unsigned long)-1,
         "an address in an unused slot maps to nothing");
}

static void test_init_bad_record(void)
{
    arkrtos_cfg_record_t r;

    /* 坏 CRC：默认布局留下，原因码报出来 */
    flash_blank();
    rec_auto(&r, 1u);
    r.crc32 ^= 1u;
    store_at(0u, &r);
    g_log_calls = 0;
    arkrtos_layout_init();
    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "a bad record keeps the default layout");
    ck_u((unsigned long)arkrtos_layout_reason(), (unsigned long)ARKRTOS_CFG_ERR_CRC,
         "a bad record is reported by reason code");
    ck_u((unsigned long)g_log_calls, 0ul, "a rejected record never touches the console");

    /* 模式与槽表不一致：同样回退，原因码是 SLOTS_IN_MODE */
    flash_blank();
    rec_fixed(&r, 1u, 0u);
    store_at(0u, &r);
    arkrtos_layout_init();
    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "a record whose slots do not match its mode keeps the default");
    ck_u((unsigned long)arkrtos_layout_reason(), (unsigned long)ARKRTOS_CFG_ERR_SLOTS_IN_MODE,
         "the mismatch is reported as such");

    /* 没有任何记录时原因码必须是 0（上一次拒绝不能残留） */
    flash_blank();
    arkrtos_layout_init();
    ck_u((unsigned long)arkrtos_layout_reason(), 0ul, "a clean boot clears the last reason");
}

/* ---------------- 用例：写入 ---------------- */

static void test_write(void)
{
    arkrtos_cfg_record_t r;
    uint32 erased = 0xFFFFFFFFu;
    int32 idx;

    flash_blank();
    arkrtos_layout_init();

    /* 参数与内容闸门 */
    rec_auto(&r, 1u);
    ck_u((unsigned long)arkrtos_layout_write(0, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased),
         (unsigned long)ARKRTOS_LAYOUT_ERR_ARG, "a null record is an argument error");
    ck_u((unsigned long)arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE - 1u, &erased),
         (unsigned long)ARKRTOS_LAYOUT_ERR_ARG, "a short record is an argument error");
    ck_u((unsigned long)g_write_calls, 0ul, "nothing reached the flash backend yet");

    r.magic = 0u;
    ck_u((unsigned long)arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased),
         (unsigned long)ARKRTOS_LAYOUT_ERR_INVALID, "a record without magic is refused");

    rec_auto(&r, 1u);
    r.crc32 ^= 1u;
    ck_u((unsigned long)arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased),
         (unsigned long)ARKRTOS_LAYOUT_ERR_CRC, "a record with a bad crc is refused");

    /* 第一次写入：落在 0 号位，序号与硬件签名由内核盖章 */
    memset(&g_pt, 0, sizeof(g_pt));
    g_write_calls = 0;
    rec_auto(&r, 99u);
    r.hw_compat_id = 0u;
    r.log_level = 2u;
    rec_seal(&r);
    erased = 0xFFFFFFFFu;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, 0ul, "the first record lands at index 0");
    ck_u((unsigned long)erased, 0ul, "no erase was needed");
    ck_u((unsigned long)g_last_write_addr, (unsigned long)CONFIG_BASE, "the write goes to the region");
    ck_u((unsigned long)g_last_write_len, (unsigned long)ARKRTOS_CFG_RECORD_SIZE, "the whole record is written");
    ck_u((unsigned long)rec_at(0u)->seq, 1ul, "the kernel stamps seq itself (1)");
    ck_u((unsigned long)rec_at(0u)->hw_compat_id, (unsigned long)ARKRTOS_HW_COMPAT_ID,
         "the kernel stamps its own hardware id");
    ck_u((unsigned long)rec_at(0u)->crc32, (unsigned long)arkrtos_layout_crc(rec_at(0u)),
         "the stored crc matches the stamped record");
    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_CONFIG,
         "a written record takes effect immediately");
    ck_u((unsigned long)arkrtos_layout_cfg_log_level(), 2ul, "the new log level is live");
    ck_u((unsigned long)g_log_level, 2ul, "and applied to the console");

    /* 第二次写入：追加、序号递增、seq 由内核覆盖调用方给的值 */
    rec_auto(&r, 1u);
    r.fault_restart_max = 9u;
    rec_seal(&r);
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, 1ul, "the second record is appended");
    ck_u((unsigned long)rec_at(1u)->seq, 2ul, "seq keeps increasing");
    ck_u((unsigned long)arkrtos_layout_cfg_restart_max(), 9ul, "the second record is adopted");

    /* 写入后端失败：报 WRITE，且不采纳半成品 */
    rec_auto(&r, 1u);
    r.boot_delay_ms = 777u;
    rec_seal(&r);
    g_write_ret = -3;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, (unsigned long)ARKRTOS_LAYOUT_ERR_WRITE, "a backend write error is reported");
    ck_u((unsigned long)arkrtos_layout_boot_delay_ms(), 0ul, "a failed write is not adopted");
    g_write_ret = 0;

    /* 回读校验：写进去的字节被改坏必须当场暴露 */
    rec_auto(&r, 1u);
    r.boot_delay_ms = 888u;
    rec_seal(&r);
    g_write_corrupt = 1;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, (unsigned long)ARKRTOS_LAYOUT_ERR_VERIFY, "a failed read-back is reported");
    ck_u((unsigned long)arkrtos_layout_boot_delay_ms(), 0ul, "a record that did not read back is not adopted");
    g_write_corrupt = 0;

    /* 盖章之后才发现的非法记录：写完校验不过 ⇒ INVALID，也不落盘 */
    rec_fixed(&r, 1u, 0u);
    g_write_calls = 0;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, (unsigned long)ARKRTOS_LAYOUT_ERR_INVALID,
         "a record that fails validation after stamping is refused");
    ck_u((unsigned long)g_write_calls, 0ul, "and never reaches the flash backend");
}

static void test_write_full_region(void)
{
    arkrtos_cfg_record_t r;
    uint32 erased = 0xFFFFFFFFu;
    uint32 records = (uint32)CONFIG_SIZE / (uint32)ARKRTOS_CFG_RECORD_SIZE;
    uint32 i;
    int32 idx = -1;

    flash_blank();
    arkrtos_layout_init();
    g_erase_calls = 0;

    for(i = 0u; i < records; i++)
    {
        rec_auto(&r, 1u);
        r.boot_delay_ms = i;
        rec_seal(&r);
        erased = 0xFFFFFFFFu;
        idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
        if(idx != (int32)i)
        {
            break;
        }
        if(erased != 0u)
        {
            break;
        }
    }
    ck_u((unsigned long)idx, (unsigned long)(records - 1u), "the region holds exactly one record per slot");
    ck_u((unsigned long)g_erase_calls, 0ul, "filling the region needs no erase");

    /* 再来一条：区满 ⇒ 整体擦一次、从头写、p_erased 置 1、序号从 1 重来 */
    rec_auto(&r, 1u);
    rec_seal(&r);
    erased = 0xFFFFFFFFu;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, 0ul, "a full region restarts at index 0");
    ck_u((unsigned long)erased, 1ul, "and reports that it had to erase");
    ck_u((unsigned long)g_erase_calls, 1ul, "the region is erased exactly once");
    ck_u((unsigned long)g_last_erase_addr, (unsigned long)CONFIG_BASE, "the erase covers the region");
    ck_u((unsigned long)g_last_erase_size, (unsigned long)CONFIG_SIZE, "the erase covers the whole region");
    ck_u((unsigned long)rec_at(0u)->seq, 1ul, "sequence numbering restarts after the erase");
    ck_u((unsigned long)rec_at(1u)->magic, 0xFFFFFFFFul, "the rest of the region is blank again");

    /* 擦除失败：报 ERASE，且一条也不写 */
    flash_blank();
    arkrtos_layout_init();
    for(i = 0u; i < records; i++)
    {
        rec_auto(&r, 1u);
        rec_seal(&r);
        if(arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased) != (int32)i)
        {
            break;
        }
    }
    ck_u((unsigned long)i, (unsigned long)records, "the region was refilled for the erase-failure case");
    rec_auto(&r, 1u);
    rec_seal(&r);
    g_write_calls = 0;
    g_erase_ret = -5;
    idx = arkrtos_layout_write((const uint8 *)&r, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);
    ck_u((unsigned long)idx, (unsigned long)ARKRTOS_LAYOUT_ERR_ERASE, "a failed erase is reported");
    ck_u((unsigned long)g_write_calls, 0ul, "and nothing is written into a region we could not erase");
    g_erase_ret = 0;
}

/* ---------------- 用例：擦除 ---------------- */

static void test_erase(void)
{
    arkrtos_cfg_record_t r;

    flash_blank();
    rec_fixed(&r, 1u, 1u);
    store_at(0u, &r);
    memset(&g_pt, 0, sizeof(g_pt));
    arkrtos_layout_init();
    ck_u((unsigned long)arkrtos_layout_mode(), (unsigned long)ARKRTOS_LAYOUT_MODE_FIXED,
         "a fixed record is in effect before the erase");

    g_erase_calls = 0;
    ck_u((unsigned long)arkrtos_layout_erase(), 0ul, "the erase succeeds");
    ck_u((unsigned long)g_erase_calls, 1ul, "the backend was asked once");
    ck_u((unsigned long)arkrtos_layout_source(), (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "after the erase the compile-time default is back");
    ck_u((unsigned long)arkrtos_layout_slot_count(), 0ul, "and the default table is empty");
    ck_u((unsigned long)arkrtos_layout_reason(), 0ul, "an erase is not a rejection");
    ck_u((unsigned long)g_pt.layout_source, (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT,
         "publish reflects the fallback");
    ck_u((unsigned long)rec_at(0u)->magic, 0xFFFFFFFFul, "the region really is blank");

    /* 擦除失败：报错，且不假装成功 */
    rec_fixed(&r, 2u, 1u);
    rec_seal(&r);
    store_at(0u, &r);
    arkrtos_layout_init();
    g_erase_ret = -7;
    ck_u((unsigned long)arkrtos_layout_erase(), (unsigned long)ARKRTOS_LAYOUT_ERR_ERASE,
         "a failed erase is reported");
    g_erase_ret = 0;
    ck_u((unsigned long)arkrtos_layout_mode(), (unsigned long)ARKRTOS_LAYOUT_MODE_FIXED,
         "a failed erase leaves the current layout alone");
}

/* ---------------- main ---------------- */

int main(void)
{
    if(map_at((unsigned long)CONFIG_BASE, (unsigned long)CONFIG_SIZE) != 0)
    {
        printf("LAYOUT-HOST FAIL: cannot map the config region at 0x%08lX\n",
               (unsigned long)CONFIG_BASE);
        return 1;
    }
    g_flash = (uint8 *)(uintptr_t)(uint32)CONFIG_BASE;
    crc_table_init();

    test_crc();
    test_validate_header();
    test_validate_slots();
    test_validate_knobs();
    test_reason_text();
    test_read();
    test_init_default();
    test_init_adopt();
    test_init_bad_record();
    test_write();
    test_write_full_region();
    test_erase();

    if(g_fails != 0)
    {
        printf("LAYOUT-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("LAYOUT-HOST OK  %d checks\n", g_checks);
    return 0;
}
