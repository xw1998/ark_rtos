/**
* @file test_loader_host.c
* @brief 镜像头校验与重定位打补丁的宿主用例（不需要板子）
*
* 只覆盖两个纯函数：
*   - arkrtos_loader_check_header()：镜像头的每一条拒绝理由；
*   - arkrtos_loader_reloc_apply()：整字 / MOVW+MOVT 两类表项的补丁、跨块切断时
*     的可提交长度与前进指针；
*   - arkrtos_loader_crc_recheck()（SEC-G17，随
*     -DARKRTOS_USE_IMAGE_CRC_RECHECK=1 编入）：把落盘形态反算回标称形态再比
*     crc32 —— 正向（打过 ROM 补丁的镜像判 0）、负向（改坏一个字节判 -5、
*     runtime_ram_base 未固化 / 参数为空判 -1、cap 不足判 -4、表项回退判 -13）。
*     这一条盯的正是"安装完再复核"整条链里唯一能在宿主上跑的那一步。
*
* 为什么这两个值得在宿主上钉死：重定位错一位不会报错，只会让镜像在板上跑到
* 别的地址上，现象是"偶发 HardFault"或"读到错常量"。四遍差分的产物是否正确
* 落进池里，全靠这两个函数。
*
* 明确没验证：装载/擦除/搬移/卸载（要设备与 Flash 端口）、签名、崩溃计数、
* 任务真正启动。那些只能上板，不要把本文件读成"loader 已验证"。
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* 搬移用例要把镜像池映射到它**真实的**绝对地址（IMAGE_POOL_BASE）：
 * 压实/选址两条路径写死用这个编译期常量扫内存，宿主上只有把这段地址
 * 真映射出来才能跑，否则只能测纯函数。 */
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "arkrtos_loader.h"
#include "arkrtos_app_image.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_ptable.h"
#include "arkrtos_layout.h"
#include "arkrtos_crash.h"
#include "arkrtos_mini.h"
#include "arkrtos_mpu.h"
#include "arkrtos_hal.h"
#include "arkrtos_log.h"
#include "arkrtos_dev.h"

/* ------------------------------------------------------------------ */
/* 内核桩：loader.c 里用得到、但本用例根本不走到的那些               */
/* ------------------------------------------------------------------ */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count = 0;

void arkrtos_ready_del(int32 task_idx)        { (void)task_idx; }
void arkrtos_delay_disarm(int32 task_idx)     { (void)task_idx; }
void arkrtos_sched_activate_higher(uint8 p)   { (void)p; }
void arkrtos_task_release_resources(int32 t)  { (void)t; }

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)entry; (void)stack_bottom; (void)stack_size;
    (void)priority; (void)period_ms;
    return -1;
}

void arkrtos_port_disable_irq(void) {}
void arkrtos_port_enable_irq(void)  {}
void arkrtos_port_switch_task(void) {}
void arkrtos_port_wdg_feed(void)    {}

/* 本文件默认只跑纯函数，不碰 Flash / 槽位表；下面这几面旗子由
 * test_load_buffer_e2e() 打开，让同一份桩既能当"什么都没有"，也能当
 * "一块可写的 RAM 假装 Flash + 一份固定槽位配置"。 */
static int   g_layout_on  = 0;
static int   g_flash_on   = 0;
static int   g_flash_fail = 0;
static int   g_alloc_n    = 0;
static int   g_free_n     = 0;
static arkrtos_cfg_slot_t g_fake_slot;

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    if(!g_flash_on)
    {
        return -1;
    }
    memset((void *)(uintptr_t)addr, 0xFF, size);   /* 擦除 == 全 0xFF，与 NOR 同义 */
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    if(!g_flash_on || g_flash_fail)
    {
        return -1;
    }
    memcpy((void *)(uintptr_t)addr, data, len);
    return 0;
}

uint32 arkrtos_log_get_level(void) { return 0u; }

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

uint32 arkrtos_crash_fault(uint32 slot, uint32 reason) { (void)slot; (void)reason; return 0u; }
uint32 arkrtos_crash_disabled(uint32 slot)             { (void)slot; return 0u; }
void   arkrtos_crash_forget(uint32 slot)               { (void)slot; }
const char *arkrtos_crash_reason_name(uint32 reason)   { (void)reason; return "stub"; }
void   arkrtos_fault_record(uint32 type, int32 task_id) { (void)type; (void)task_id; }

int32 arkrtos_mini_on_fault(int32 task_id) { (void)task_id; return 0; }

void arkrtos_mpu_build_task(arkrtos_task_t *p_task) { (void)p_task; }
void arkrtos_mpu_reset(void) {}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle; (void)pdata; (void)len;
    return -1;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle; (void)pdata; (void)len;
    return -1;
}

static arkrtos_partition_table_t g_pt;


/* 扫描用例要的三面旗子：一段"已被裸镜像占用"的区间、RAM 块分配是否成功、
 * 固定 RAM 块是否还留着。默认全关 —— 前面所有用例走的仍是拆分前那套桩。 */
static int    g_scan_on       = 0;
static uint32 g_raw_base      = 0u;
static uint32 g_raw_size      = 0u;
static int    g_ram_alloc_ok  = 0;
static int    g_ram_reserve_ok = 0;
static uint32 g_ram_reserve_fail_base = 0u;
static uint32 g_bind_n        = 0u;
static uint32 g_bind_base     = 0u;
static uint32 g_bind_size     = 0u;

arkrtos_partition_table_t *arkrtos_ptable_get(void) { return &g_pt; }

/* 真实的 clear_slots 会清空整张槽位表（开机扫描的第一件事就是丢弃共享 RAM
 * 里掉电没清掉的旧记录）。桩也照做，否则"扫描前清表"这一步在宿主上测不出。 */
void arkrtos_ptable_clear_slots(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)ARKRTOS_SLOT_ARRAY_MAX; i++)
    {
        g_pt.slot_type[i]      = 0u;
        g_pt.slot_state[i]     = 0u;
        g_pt.slot_base[i]      = 0u;
        g_pt.slot_size[i]      = 0u;
        g_pt.slot_ram_base[i]  = 0u;
        g_pt.slot_ram_size[i]  = 0u;
        g_pt.slot_entry[i]     = 0u;
        g_pt.slot_task_id[i]   = 0u;
        g_pt.slot_crash_cnt[i] = 0u;
        g_pt.slot_autostart[i] = 0u;
    }
}

int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size)
{
    int32 slot = 0;

    if(!g_layout_on)
    {
        return -1;
    }
    /* 真实的分配器总是返回下一个空槽、并把几何写进槽位表；宿主上的默认桩
     * 固定返 0 且不记账（前面那些用例的断言就是按这个口径写的）。
     * 扫描用例要核"登记到的是哪个地址"，只有 g_scan_on 打开时才照真分配器
     * 的行为来：返回递增槽号并记下 base/size/type。 */
    if(g_scan_on)
    {
        slot = (int32)g_alloc_n;
        if((uint32)slot < (uint32)ARKRTOS_SLOT_ARRAY_MAX)
        {
            g_pt.slot_base[slot] = base;
            g_pt.slot_size[slot] = size;
            g_pt.slot_type[slot] = type;
        }
    }
    g_alloc_n++;
    return slot;
}
void arkrtos_ptable_free(uint32 slot) { (void)slot; g_free_n++; }
int32 arkrtos_ptable_move(uint32 slot, uint32 new_base)
{
    if(slot >= (uint32)ARKRTOS_SLOT_ARRAY_MAX)
    {
        return -1;
    }
    g_pt.slot_base[slot] = new_base;      /* 搬移用例需要真记账，否则"搬成功"读不出来 */
    return 0;
}
int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    if(g_scan_on && (g_raw_size != 0u) &&
       (addr >= g_raw_base) && (addr < (g_raw_base + g_raw_size)))
    {
        *p_base = g_raw_base;
        *p_size = g_raw_size;
        return 0;
    }
    (void)addr; (void)p_base; (void)p_size; return -1;
}
int32 arkrtos_ptable_find_task(uint32 task_id) { (void)task_id; return -1; }
int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    if(slot >= (uint32)ARKRTOS_SLOT_ARRAY_MAX)
    {
        return -1;
    }
    g_pt.slot_state[slot]   = state;
    g_pt.slot_entry[slot]   = entry;
    g_pt.slot_task_id[slot] = task_id;
    return 0;
}
int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    (void)slot; (void)p_state; (void)p_entry; (void)p_task_id; return -1;
}
int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size)
{
    if(!g_ram_alloc_ok)
    {
        (void)bytes; (void)p_base; (void)p_size; return -1;
    }
    *p_base = (uint32)0x20010000u;
    *p_size = bytes;
    return 0;
}
int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size)
{
    if(!g_ram_reserve_ok || (base == g_ram_reserve_fail_base))
    {
        (void)base; (void)size; return -1;
    }
    return 0;
}
int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size)
{
    if(!g_ram_reserve_ok && !g_ram_alloc_ok)
    {
        (void)slot; (void)base; (void)size; return -1;
    }
    g_bind_n++;
    g_bind_base = base;
    g_bind_size = size;
    if(slot < (uint32)ARKRTOS_SLOT_ARRAY_MAX)
    {
        g_pt.slot_ram_base[slot] = base;
        g_pt.slot_ram_size[slot] = size;
    }
    return 0;
}
int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size)
{
    (void)slot; (void)p_base; (void)p_size; return -1;
}

uint32 arkrtos_layout_slot_count(void) { return g_layout_on ? 1u : 0u; }
uint32 arkrtos_layout_raw_allow(void)  { return 0u; }
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void)
{
    return g_layout_on ? &g_fake_slot : (const arkrtos_cfg_slot_t *)0;
}
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    return (g_layout_on && (index == 0u)) ? &g_fake_slot : (const arkrtos_cfg_slot_t *)0;
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond) do {                                                   \
        if(cond) { g_pass++; }                                             \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while(0)

static const uint32 VALID_TYPES = 0x000000FFu;

static void hdr_init(arkrtos_app_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->magic         = ARKRTOS_APP_MAGIC;
    h->hw_compat_id  = ARKRTOS_HW_COMPAT_ID;
    h->type          = 0u;
    h->image_size    = 64u;
    h->entry_offset  = 0u;
    h->reloc_offset  = ARKRTOS_APP_RELOC_OFFSET;
    h->reloc_count   = 0u;
    h->reloc_kind    = ARKRTOS_APP_RELOC_KIND_CUR;
    h->payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(h);
    /* check_header 还会挡 ram_size==0 与超过单块上限：这两个字段
     * 不是「可省略」的，漏填会让一个本来合法的头被判 SIZE。 */
    h->ram_size      = 4096u;
}

/* MOVW/MOVT 的 16 位立即数编解码（测试侧独立实现，不调内核的静态函数） */
static uint32 mov_decode(uint16 h1, uint16 h2)
{
    return ((uint32)(h1 & 0x000Fu) << 12)
         | ((uint32)((h1 >> 10) & 1u) << 11)
         | ((uint32)((h2 >> 12) & 7u) << 8)
         |  (uint32)(h2 & 0x00FFu);
}

static void mov_encode(uint16 *h1, uint16 *h2, uint32 imm16)
{
    *h1 = (uint16)((*h1 & 0xFBFFu) | (uint16)(((imm16 >> 11) & 1u) << 10)
                 | (uint16)((imm16 >> 12) & 0x000Fu));
    *h2 = (uint16)(((uint16)((imm16 >> 8) & 7u) << 12) | (uint16)(*h2 & 0x0F00u)
                 | (uint16)(imm16 & 0x00FFu));
}

static void test_check_header(void)
{
    arkrtos_app_header_t h;
    uint32 rel[2];
    uint32 payload;

    hdr_init(&h);
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == 0);

    hdr_init(&h); h.magic = 0u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_MAGIC);

    hdr_init(&h); h.hw_compat_id ^= 1u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_COMPAT);

    /* type >= 32 必须自己挡住，不能拿 1u << 33 当掩码用 */
    hdr_init(&h); h.type = 32u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, 0xFFFFFFFFu) == ARKRTOS_LOADER_ERR_PARAM);

    hdr_init(&h); h.type = 1u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, 0x00000001u) == ARKRTOS_LOADER_ERR_PARAM);

    hdr_init(&h); h.image_size = 0u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_SIZE);

    /* 回绕型长度：不挡就会被后面的"总长 > 可用空间"放过去 */
    hdr_init(&h); h.image_size = 0xFFFFFFFFu;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_SIZE);

    hdr_init(&h); h.entry_offset = h.image_size;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_ENTRY);

    hdr_init(&h); h.reloc_offset = ARKRTOS_APP_RELOC_OFFSET + 4u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    hdr_init(&h); h.reloc_count = ARKRTOS_APP_RELOC_MAX + 1u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 表编码版本：当前只接受 KIND_CUR；写成旧的 32 位版本必须被拒 */
    hdr_init(&h); h.reloc_kind = ARKRTOS_APP_RELOC_KIND_32;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* payload_offset 必须等于 256 + count*4；reloc_count==0 时两者恰好相等，
     * 所以这条用例必须带非空表才有分辨力 */
    hdr_init(&h); h.reloc_count = 2u; h.payload_offset = ARKRTOS_APP_HEADER_SIZE;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 表体：两条对齐、落在负载内、按升序 —— 必须通过 */
    hdr_init(&h);
    h.reloc_count = 2u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    payload = h.payload_offset;
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(payload, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(payload + 4u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == 0);

    /* 表项指向负载之前（这里指到表自己） */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(payload - 4u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(payload + 4u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 整字表项未按 4 字节对齐 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(payload + 2u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(payload + 4u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 指令类表项半字对齐即可（整字表项要 4 字节，见上一条）——
     * 注意：arkrtos_app_image.h 的 RELOC_ENTRY 编码是 (off/2)<<2，
     * 奇数偏移无法被表达（会被折叠成偶数），所以内核里那条 off%2 的
     * 检查在真实镜像上不可达，属于防御性保留，这里不构造不可达用例。 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(payload + 2u, ARKRTOS_APP_RELOC_KIND_ROM_MOVW);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(payload + 8u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == 0);

    /* 表项越过镜像末尾 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(ARKRTOS_APP_TOTAL_LEN(&h) - 2u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(ARKRTOS_APP_TOTAL_LEN(&h) - 2u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 乱序：流式打补丁靠前进指针，乱序会静默漏改 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(payload + 8u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(payload + 4u, ARKRTOS_APP_RELOC_KIND_RAM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* 空表永远合法（没有需要打补丁的绝对地址） */
    hdr_init(&h);
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == 0);
}

static void test_reloc_words(void)
{
    arkrtos_app_header_t h;
    uint32 rel[4];
    uint8  buf[32];
    uint32 idx;

    hdr_init(&h);
    h.reloc_count = 3u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);

    memset(buf, 0, sizeof(buf));
    *(uint32 *)(buf + 0)  = 0x00001000u;   /* ROM：加 delta_rom */
    *(uint32 *)(buf + 4)  = 0x00002000u;   /* RAM：加 delta_ram */
    *(uint32 *)(buf + 8)  = 0x00003000u;   /* ROM：同一块里第二条 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(0u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(4u, ARKRTOS_APP_RELOC_KIND_RAM);
    rel[2] = ARKRTOS_APP_RELOC_ENTRY(8u, ARKRTOS_APP_RELOC_KIND_ROM);

    idx = 0u;
    CHECK(arkrtos_loader_reloc_apply(buf, 0u, sizeof(buf), &h, rel, &idx,
                                     0x100u, 0x400u) == sizeof(buf));
    CHECK(idx == 3u);
    CHECK(*(uint32 *)(buf + 0) == 0x00001100u);
    CHECK(*(uint32 *)(buf + 4) == 0x00002400u);
    CHECK(*(uint32 *)(buf + 8) == 0x00003100u);

    /* delta 为 0：字节必须逐位不变 */
    *(uint32 *)(buf + 0) = 0xDEADBEEFu;
    idx = 0u;
    (void)arkrtos_loader_reloc_apply(buf, 0u, sizeof(buf), &h, rel, &idx, 0u, 0u);
    CHECK(*(uint32 *)(buf + 0) == 0xDEADBEEFu);

    /* 空表：可提交长度等于块尾，指针不动 */
    h.reloc_count = 0u;
    idx = 7u;
    CHECK(arkrtos_loader_reloc_apply(buf, 0u, 16u, &h, rel, &idx, 1u, 1u) == 16u);
    CHECK(idx == 7u);
}

static void test_reloc_movw(void)
{
    arkrtos_app_header_t h;
    uint32 rel[1];
    uint8  buf[16];
    uint32 idx;
    uint16 h1, h2;
    /* 刻意不用 0x080xxxxx：那是 flash 地址形态，会被 single-address-source
     * 门禁当成硬编码布局常量（它不区分测试夹具）。这里要的只是「高低 16 位
     * 都非零」的 32 位值，用 0x12345678 反而把 MOVW/MOVT 两半都覆盖到了。 */
    uint32 base = 0x12345678u;

    hdr_init(&h);
    h.reloc_count = 1u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(0u, ARKRTOS_APP_RELOC_KIND_ROM_MOVW);

    memset(buf, 0, sizeof(buf));
    h1 = 0xF240u;                 /* MOVW R0, #imm16 */
    h2 = 0xF2C0u;                 /* MOVT R0, #imm16 */
    /* 低 16 位进 MOVW，高 16 位进 MOVT */
    mov_encode(&h1, &h2, base & 0xFFFFu);
    *(uint16 *)(buf + 0) = h1;
    *(uint16 *)(buf + 2) = h2;
    h1 = 0xF240u; h2 = 0xF2C0u;
    mov_encode(&h1, &h2, base >> 16);
    *(uint16 *)(buf + 4) = h1;
    *(uint16 *)(buf + 6) = h2;

    /* 还原一份基线，供"补丁后前缀位不变"比对 */
    {
        uint16 w1 = *(uint16 *)(buf + 0);
        uint16 w2 = *(uint16 *)(buf + 2);
        uint16 t1 = *(uint16 *)(buf + 4);
        uint16 t2 = *(uint16 *)(buf + 6);
        uint32 expect = base + 0x1000u;

        idx = 0u;
        CHECK(arkrtos_loader_reloc_apply(buf, 0u, sizeof(buf), &h, rel, &idx,
                                         0x1000u, 0u) == sizeof(buf));
        CHECK(idx == 1u);
        CHECK(mov_decode(*(uint16 *)(buf + 0), *(uint16 *)(buf + 2)) == (expect & 0xFFFFu));
        CHECK(mov_decode(*(uint16 *)(buf + 4), *(uint16 *)(buf + 6)) == (expect >> 16));
        /* 立即数位之外的编码（Rd / 指令种类）必须原样保留 */
        /* 掩码 0xFBF0 = 保留 bits15:11 与 bits9:4，清掉 i(bit10) 与
         * imm4(bits3:0) —— 这两个就是立即数位，必须和内核 mov_set_imm16 一致 */
        CHECK((*(uint16 *)(buf + 0) & 0xFBF0u) == (w1 & 0xFBF0u));
        CHECK((*(uint16 *)(buf + 2) & 0x0F00u) == (w2 & 0x0F00u));
        CHECK((*(uint16 *)(buf + 4) & 0xFBF0u) == (t1 & 0xFBF0u));
        CHECK((*(uint16 *)(buf + 6) & 0x0F00u) == (t2 & 0x0F00u));
    }

    /* RAM 类 MOVW 走 delta_ram，不走 delta_rom */
    {
        uint32 before = mov_decode(*(uint16 *)(buf + 0), *(uint16 *)(buf + 2));
        rel[0] = ARKRTOS_APP_RELOC_ENTRY(0u, ARKRTOS_APP_RELOC_KIND_RAM_MOVW);
        idx = 0u;
        (void)arkrtos_loader_reloc_apply(buf, 0u, sizeof(buf), &h, rel, &idx, 0x8000u, 0x10u);
        CHECK(mov_decode(*(uint16 *)(buf + 0), *(uint16 *)(buf + 2)) == ((before + 0x10u) & 0xFFFFu));
    }

    /* 跨块切断：本块只到表项中间 → 可提交长度退回到表项起点，指针不动 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(8u, ARKRTOS_APP_RELOC_KIND_ROM_MOVW);
    idx = 0u;
    CHECK(arkrtos_loader_reloc_apply(buf, 0u, 12u, &h, rel, &idx, 0x100u, 0u) == 8u);
    CHECK(idx == 0u);
    /* 下一块从表项起点开始，8 字节齐全 → 打上并前进 */
    idx = 0u;
    CHECK(arkrtos_loader_reloc_apply(buf, 8u, 8u, &h, rel, &idx, 0x100u, 0u) == 16u);
    CHECK(idx == 1u);

    /* 整字表项同样不能跨块：3 个字节装不下 4 字节 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(0u, ARKRTOS_APP_RELOC_KIND_ROM);
    idx = 0u;
    CHECK(arkrtos_loader_reloc_apply(buf, 0u, 3u, &h, rel, &idx, 1u, 0u) == 0u);
    CHECK(idx == 0u);
}

static void test_reloc_offsets(void)
{
    arkrtos_app_header_t h;
    uint32 rel[3];
    uint8  buf[64];
    uint32 idx;

    hdr_init(&h);
    h.reloc_count = 3u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);

    memset(buf, 0, sizeof(buf));
    *(uint32 *)(buf + 0)  = 1u;
    *(uint32 *)(buf + 16) = 2u;
    *(uint32 *)(buf + 32) = 3u;
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(0u,  ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(16u, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[2] = ARKRTOS_APP_RELOC_ENTRY(32u, ARKRTOS_APP_RELOC_KIND_ROM);

    /* buf 是整块镜像的镜像副本；传进去的指针对应镜像偏移 buf_off。
     * 从偏移 16 起处理：off=0 那条落在本块之前，必须被"防御性跳过"
     * （不补、但指针前进且块外字节不被碰） */
    idx = 0u;
    CHECK(arkrtos_loader_reloc_apply(buf + 16u, 16u, 16u, &h, rel, &idx,
                                     0x10u, 0u) == 32u);
    CHECK(idx == 2u);
    CHECK(*(uint32 *)(buf + 0) == 1u);
    CHECK(*(uint32 *)(buf + 16) == 0x12u);

    /* 指针复用：继续处理偏移 32 起的一块 */
    CHECK(arkrtos_loader_reloc_apply(buf + 32u, 32u, 16u, &h, rel, &idx,
                                     0x10u, 0u) == 48u);
    CHECK(idx == 3u);
    CHECK(*(uint32 *)(buf + 32) == 0x13u);
}

#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
/* 按打包工具的同一口径算「标称形态」的 CRC：头（crc32/state/ram_base 按 0 代入）
 * 然后逐段表体 + 负载。测试侧自己拼起来，是为了不借内核的实现互相证明。 */
static uint32 nominal_crc(const uint8 *img, const arkrtos_app_header_t *h)
{
    uint32 crc = arkrtos_loader_crc_header(h);
    uint32 rt_len = h->reloc_count * ARKRTOS_APP_RELOC_SIZE;

    if(rt_len != 0u)
    {
        crc = arkrtos_crc32(img + ARKRTOS_APP_RELOC_OFFSET, rt_len, crc);
    }
    return arkrtos_crc32(img + ARKRTOS_APP_PAYLOAD_OFFSET(h), h->image_size, crc);
}

static void test_crc_recheck(void)
{
    uint8 img[512];
    arkrtos_app_header_t h;
    arkrtos_app_header_t saved;
    uint32 rel[2];
    uint32 got;
    uint32 nominal_base = 0x80300000u;
    uint32 base         = 0x80305000u;
    uint32 delta_rom;
    uint32 total;
    uint32 pay_off;
    uint32 w0_off;
    uint32 w1_off;
    uint32 i;

    memset(img, 0, sizeof(img));
    hdr_init(&h);
    h.type           = ARKRTOS_APP_TYPE_APP;  /* 池只收 APP/DRIVER，0 会被 check_header 判 PARAM */
    h.image_size     = 64u;
    h.reloc_count    = 2u;
    h.nominal_base   = nominal_base;
    h.nominal_ram_base = 0x20000000u;
    h.runtime_ram_base = 0x20001000u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off = h.payload_offset;
    total   = ARKRTOS_APP_TOTAL_LEN(&h);
    delta_rom = (base + pay_off) - nominal_base;

    /* 负载：两处放"标称形态的绝对地址"，其余填固定花样 */
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0xA0u + i);
    }
    w0_off = pay_off;
    w1_off = pay_off + 16u;
    *(uint32 *)(img + w0_off) = nominal_base + 0x100u;
    *(uint32 *)(img + w1_off) = nominal_base + 0x200u;

    /* 表项按升序、偏移按半字存放 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(w0_off, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(w1_off, ARKRTOS_APP_RELOC_KIND_ROM);

    /* 表体先落进 img：标称形态的 CRC 覆盖的是"头 + 表 + 负载"，
     * 表区若还是 memset 的 0，算出来的就不是同一个镜像了。 */
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));

    /* 标称形态的 CRC（此时头部除 crc32 外都已填好） */
    h.crc32 = 0u;
    memcpy(&saved, &h, sizeof(h));
    h.crc32 = nominal_crc(img, &h);
    memcpy(&saved, &h, sizeof(h));

    /* 落盘形态：表体原样 + 负载里那两处加上 ROM 增量 */
    *(uint32 *)(img + w0_off) += delta_rom;
    *(uint32 *)(img + w1_off) += delta_rom;
    memcpy(img, &saved, sizeof(saved));

    /* 正向：落点不是标称基址，但反算回去应当一致 */
    got = 0xDEADBEEFu;
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) == 0);
    CHECK(got == saved.crc32);

    /* 负向 1：改坏负载里一个字节 —— 这正是"装好之后被改坏" */
    img[pay_off + 5u] ^= 0xFFu;
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) ==
          (int32)ARKRTOS_LOADER_ERR_CRC);
    img[pay_off + 5u] ^= 0xFFu;
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) == 0);

    /* 负向 2：参数为空 */
    CHECK(arkrtos_loader_crc_recheck((const uint8 *)0, base, total, &got) ==
          (int32)ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_crc_recheck(img, base, total, (uint32 *)0) ==
          (int32)ARKRTOS_LOADER_ERR_PARAM);

    /* 负向 3：cap 比镜像总长少 4 -> 没资格复核，必须报 SIZE 而不是"不一致" */
    CHECK(arkrtos_loader_crc_recheck(img, base, total - 4u, &got) ==
          (int32)ARKRTOS_LOADER_ERR_SIZE);

    /* 负向 4：runtime_ram_base 未固化 -> "算不出来"，不是"对"
     * （池里真出现过这种镜像：装到一半断电时头还没固化） */
    memcpy(&saved, &h, sizeof(h));
    saved.runtime_ram_base = 0u;
    memcpy(img, &saved, sizeof(saved));
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) ==
          (int32)ARKRTOS_LOADER_ERR_PARAM);
    memcpy(img, &h, sizeof(h));

    /* 负向 5：表项往回退（同一偏移出现两次）-> RELOC，不是拿重复段算 CRC */
    rel[1] = rel[0];
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) ==
          (int32)ARKRTOS_LOADER_ERR_RELOC);
}

/* load_buffer 的整条链：verify -> program -> commit。宿主上用一块 RAM 假装
 * Flash、用一份固定槽位配置假装分区表，所以「擦/写/回读」都真的发生了，
 * 只是介质在内存里。这条用例盯的是拆分后的三段有没有把活干完，以及失败
 * 路径有没有把槽还回去。 */
static void test_load_buffer_e2e(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[512] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    const arkrtos_app_header_t *ph;
    uint32 rel[1];
    uint32 base;
    uint32 ram_base;
    uint32 total;
    uint32 pay_off;
    uint32 i;

    memset(pool, 0xFF, sizeof(pool));
    memset(ram, 0, sizeof(ram));
    memset(img, 0, sizeof(img));

    base     = (uint32)(uintptr_t)pool;
    ram_base = (uint32)(uintptr_t)ram;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;          /* 擦除粒度；槽位大小必须是它的整数倍 */
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);
    g_layout_on = 1;
    g_flash_on  = 1;
    g_flash_fail = 0;

    hdr_init(&h);
    h.type             = ARKRTOS_APP_TYPE_APP;
    h.image_size       = 32u;
    h.reloc_count      = 1u;
    h.nominal_base     = base;
    h.nominal_ram_base = ram_base;
    h.payload_offset   = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off = h.payload_offset;
    total   = ARKRTOS_APP_TOTAL_LEN(&h);

    /* 一条 ROM 整字表项：落盘后那个字要变成「标称值 + delta_rom」 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(pay_off, ARKRTOS_APP_RELOC_KIND_ROM);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0xA0u + i);
    }
    *(uint32 *)(img + pay_off) = base + 0x1234u;

    /* 头最后落盘：crc32 覆盖「头 + 表 + 负载」，所以要在表与负载都就位之后再算 */
    memcpy(img, &h, sizeof(h));
    ((arkrtos_app_header_t *)(void *)img)->crc32 = nominal_crc(img, &h);

    /* 正向：三段全走完，槽位真的被提交成 VALID */
    g_alloc_n = 0;
    g_free_n  = 0;
    CHECK(arkrtos_loader_load_buffer(img, total) == 0);
    CHECK(g_alloc_n == 1);
    CHECK(g_free_n == 0);

    ph = (const arkrtos_app_header_t *)(const void *)pool;
    CHECK(ph->state == ARKRTOS_APP_STATE_VALID);
    CHECK(ph->runtime_ram_base == ram_base);
    /* delta_rom = (base + payload_offset) - nominal_base = payload_offset */
    CHECK(*(uint32 *)(pool + pay_off) == (base + 0x1234u + pay_off));
    CHECK(pool[pay_off + 4u] == (uint8)(0xA4u + 0u));
    CHECK(pool[pay_off + 31u] == (uint8)(0xA0u + 31u));

    /* 负向 1：长度不足 —— 在碰任何槽位之前就该被挡下 */
    g_alloc_n = 0;
    g_free_n  = 0;
    CHECK(arkrtos_loader_load_buffer(img, (uint32)ARKRTOS_APP_HEADER_SIZE - 4u) ==
          (int32)ARKRTOS_LOADER_ERR_PARAM);
    CHECK(g_alloc_n == 0);
    CHECK(g_free_n == 0);

    /* 负向 2：负载被改坏一个字节 —— 整镜像 CRC 不过，同样不该动槽位 */
    img[pay_off + 5u] ^= 0xFFu;
    g_alloc_n = 0;
    g_free_n  = 0;
    CHECK(arkrtos_loader_load_buffer(img, total) == (int32)ARKRTOS_LOADER_ERR_CRC);
    CHECK(g_alloc_n == 0);
    CHECK(g_free_n == 0);
    img[pay_off + 5u] ^= 0xFFu;

    /* 负向 3：Flash 写失败 —— 槽位必须还回去，而且只还一次 */
    g_flash_fail = 1;
    g_alloc_n = 0;
    g_free_n  = 0;
    CHECK(arkrtos_loader_load_buffer(img, total) == (int32)ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_alloc_n == 1);
    CHECK(g_free_n == 1);
    g_flash_fail = 0;

    g_layout_on = 0;
    g_flash_on  = 0;
}

#endif /* ARKRTOS_USE_IMAGE_CRC_RECHECK */

/* ------------------------------------------------------------------ */
/* 搬移（move_slot）：把镜像池 map 到真实绝对地址后，跑一次 compress     */
/* ------------------------------------------------------------------ */

static int g_pool_mapped = 0;

static void *map_pool_at(uint32 addr, uint32 size)
{
    if(g_pool_mapped)
    {
        return (void *)(uintptr_t)addr;   /* 同一进程只能映射一次，第二次必然失败 */
    }
    g_pool_mapped = 1;
#if defined(_WIN32)
    return VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)addr, (size_t)size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return (p == MAP_FAILED) ? (void *)0 : p;
#endif
}

/* ------------------------------------------------------------------ */
/* 池扫描（scan_pool）：开机把池里的带头镜像重新登记回槽位表            */
/* ------------------------------------------------------------------ */

/* 把一个"合法镜像头"写进池里：只填 scan 会读的字段（magic / hw / 类型 /
 * 尺寸 / 状态 / RAM 需求），表与负载都不需要 —— arkrtos_loader_accept() 只
 * 校验头字段、总长与 state，不比 crc（落盘形态的 CRC 与标称形态对不上，
 * 这是设计，见 accept 的注释）。 */
static void scan_put_image(uint32 base, uint32 type, uint32 ram_size,
                           uint32 fixed_ram, uint32 autostart)
{
    arkrtos_app_header_t h;

    hdr_init(&h);
    h.type              = type;
    h.state             = ARKRTOS_APP_STATE_VALID;
    h.image_size        = 64u;
    h.ram_size          = ram_size;
    h.runtime_ram_base  = fixed_ram;
    h.flags             = autostart ? (uint32)ARKRTOS_APP_FLAG_AUTOSTART : 0u;
    h.payload_offset    = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    memcpy((void *)(uintptr_t)base, &h, sizeof(h));
}

/* 这一条覆盖拆分后的 loader_scan_begin / skip_raw / accept / rebind_ram 与
 * 入口的循环。为什么要真映射：扫描写死按 IMAGE_POOL_BASE 线性遍历绝对地址，
 * 宿主上不映射就是读未映射内存，只能测到 0 行。映射不上明说 SKIP。 */
static void test_scan_pool_e2e(void)
{
    uint32 pool   = (uint32)IMAGE_POOL_BASE;
    uint32 base_a = pool;                                   /* 落在裸镜像区间里 */
    uint32 base_b = pool + (uint32)IMAGE_POOL_SECTOR;       /* 固定 RAM，要得到 */
    uint32 base_c = base_b + (uint32)ARKRTOS_POOL_ALLOC_UNIT; /* 普通 App */
    uint32 base_d = base_c + (uint32)ARKRTOS_POOL_ALLOC_UNIT; /* 驱动 + 固定 RAM 要不到 */
    uint32 i;
    uint32 hit = 0u;

    if(map_pool_at(pool, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_scan_pool_e2e: cannot map image pool at 0x%08X\n",
               (unsigned)pool);
        return;
    }

    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_SIZE);

    scan_put_image(base_a, ARKRTOS_APP_TYPE_APP, 4096u, 0u, 0u);
    scan_put_image(base_b, ARKRTOS_APP_TYPE_APP, 4096u, (uint32)0x20008000u, 1u);
    scan_put_image(base_c, ARKRTOS_APP_TYPE_APP, 8192u, 0u, 0u);
    scan_put_image(base_d, ARKRTOS_APP_TYPE_DRIVER, 4096u, (uint32)0x20009000u, 0u);

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base   = pool;
    g_pt.pool_sector = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units  = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size   = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    /* 共享 RAM 里的旧记录：扫描必须先把它清掉，不能被它骗过去 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = 0x12340000u;
    g_pt.slot_size[0]  = 0x400u;

    /* 裸镜像区间盖住 base_a 那一个扇区；base_b/c/d 都在外面 */
    g_scan_on        = 1;
    g_raw_base       = pool;
    g_raw_size       = (uint32)IMAGE_POOL_SECTOR;
    g_layout_on      = 1;
    g_ram_alloc_ok   = 1;
    g_ram_reserve_ok = 1;
    g_ram_reserve_fail_base = (uint32)0x20009000u;         /* 只让 d 的固定块要不到 */
    g_alloc_n        = 0u;
    g_free_n         = 0u;
    g_bind_n         = 0u;

    /* 正向：只有 b/c/d 三个被登记，a 整段跳过 */
    CHECK(arkrtos_loader_scan() == 2u);                    /* 两个 App（b、c） */
    CHECK(arkrtos_loader_scan_driver() == 1u);             /* 一个驱动（d） */
    CHECK(g_alloc_n == 3u);

    /* b / c / d 的落点都在，a 的落点不在（被裸镜像区间跳过） */
    for(i = 0u; i < (uint32)ARKRTOS_SLOT_ARRAY_MAX; i++)
    {
        if(g_pt.slot_base[i] == pool)
        {
            hit |= 1u;
        }
        if(g_pt.slot_base[i] == base_b)
        {
            hit |= 2u;
        }
        if(g_pt.slot_base[i] == base_c)
        {
            hit |= 4u;
        }
        if(g_pt.slot_base[i] == base_d)
        {
            hit |= 8u;
        }
    }
    CHECK((hit & 1u) == 0u);                               /* 裸镜像区间里的没登记 */
    CHECK((hit & 2u) != 0u);
    CHECK((hit & 4u) != 0u);
    CHECK((hit & 8u) != 0u);

    /* 三者的状态都必须是 LOADED（扫描只登记，不启动） */
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(g_pt.slot_state[1] == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(g_pt.slot_state[2] == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(g_pt.slot_autostart[0] == 1u);                   /* b 的 autostart 位 */

    /* RAM 重绑：b 走固定块（要得到）、c 走分配、d 的固定块要不到 → 不绑 */
    CHECK(g_bind_n == 2u);
    CHECK(g_pt.slot_ram_base[0] == (uint32)0x20008000u);   /* b：固定块要得到 */
    CHECK(g_pt.slot_ram_size[0] == 4096u);
    CHECK(g_pt.slot_ram_base[1] == (uint32)0x20010000u);   /* c：走分配 */
    CHECK(g_pt.slot_ram_size[1] == 8192u);
    CHECK(g_pt.slot_ram_base[2] == 0u);                    /* d：固定块要不到 → 不绑 */

    /* 闸门：扫过一次就不再扫，否则同一批镜像会被重复登记 */
    g_alloc_n = 0u;
    CHECK(arkrtos_loader_scan() == 2u);
    CHECK(g_alloc_n == 0u);

    g_scan_on        = 0;
    g_ram_alloc_ok   = 0;
    g_ram_reserve_ok = 0;
    g_ram_reserve_fail_base = 0u;
    g_layout_on      = 0;
}

/* arkrtos_loader_move_slot() 只能通过 arkrtos_loader_reclaim() 的压实路径到达。
 * 它里面的池边界判断与「找干净空位」都写死用 IMAGE_POOL_BASE 扫绝对地址，
 * 所以宿主上必须先把那段地址真映射出来（Windows 用 VirtualAlloc，类 Unix 用
 * MAP_FIXED），否则这条路径在宿主上永远只能读到 0 行覆盖。
 * 映射不上就明说 SKIP —— 不假装跑过。 */
static void test_move_slot_e2e(void)
{
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    const arkrtos_app_header_t *ph;
    uint32 rel[1];
    uint32 pool = (uint32)IMAGE_POOL_BASE;
    uint32 old_base;
    uint32 total;
    uint32 pay_off;
    uint32 word_before;
    uint32 i;

    /* 映射长度必须用 IMAGE_POOL_SIZE 而不是 IMAGE_POOL_USABLE_SIZE：
     * 回收的逐扇区循环跑的是 pool_units（= IMAGE_POOL_SIZE / 扇区）个扇区，
     * 末尾那个保留扇区在 USABLE 之外，但它照样会被 span_erased() 读一遍。
     * 只映射 USABLE 的话，宿主上这一步就是读未映射地址 —— 直接段错误，
     * 而不是"少测一点"。 */
    if(map_pool_at(pool, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_move_slot_e2e: cannot map image pool at 0x%08X\n",
               (unsigned)pool);
        return;
    }

    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_SIZE);

    hdr_init(&h);
    h.type  = ARKRTOS_APP_TYPE_APP;
    h.state = ARKRTOS_APP_STATE_VALID;
    h.image_size  = 64u;
    h.reloc_count = 1u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off = h.payload_offset;
    total   = ARKRTOS_APP_TOTAL_LEN(&h);

    /* 一个比池底高一个扇区的落点：压实必然要把它往下搬 */
    old_base = pool + (uint32)IMAGE_POOL_SECTOR;

    memset(img, 0xEE, sizeof(img));
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(pay_off, ARKRTOS_APP_RELOC_KIND_ROM);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0x10u + i);
    }
    /* 已落盘的形态：这个字是「标称值 + 旧落点增量」，搬移要在它上面再加一次
     * (新落点 - 旧落点)，所以断言值就是下面这个数加同一个差 */
    word_before = old_base + 0x1234u;
    memcpy(img + pay_off, &word_before, sizeof(word_before));
    memcpy(img, &h, sizeof(h));

    memcpy((void *)(uintptr_t)old_base, img, (size_t)total);

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode  = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base    = pool;
    g_pt.pool_sector  = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units   = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size    = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.reclaim_mode = (uint32)ARKRTOS_CFG_RECLAIM_GLOBAL;
    g_pt.slot_max     = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0] = old_base;
    g_pt.slot_size[0] = total;

    g_flash_on   = 1;
    g_flash_fail = 0;

    /* 正向：压实把它搬到池底，头重新提交成 VALID，ROM 补丁再叠一次落点差。
     * 返回值是「实际擦掉的扇区数」，不是状态码：搬完之后旧副本所在的扇区
     * 已经没人占，逐扇区那一轮会把它擦掉，所以这里正好是 1。 */
    CHECK(arkrtos_loader_reclaim() == 1);
    CHECK(g_pt.slot_base[0] == pool);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);

    ph = (const arkrtos_app_header_t *)(const void *)(uintptr_t)pool;
    CHECK(ph->state == ARKRTOS_APP_STATE_VALID);
    CHECK(ph->image_size == total - pay_off);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)(pool + pay_off) ==
          (word_before + (pool - old_base)));
    CHECK(*((const uint8 *)(uintptr_t)pool + pay_off + 4u) == (uint8)0x14u);
    CHECK(*((const uint8 *)(uintptr_t)pool + pay_off + 63u) == (uint8)(0x10u + 63u));

    /* 旧位置必须整段回到擦除态：搬完还留旧副本等于池里有两份有效镜像 */
    CHECK(*(const volatile uint32 *)(const void *)(uintptr_t)old_base == 0xFFFFFFFFu);
    CHECK(*(const volatile uint32 *)(const void *)(uintptr_t)(old_base + total - 4u) == 0xFFFFFFFFu);

    /* 再回收一次不该再动：已经贴在池底，没有更低的位置可去 */
    CHECK(arkrtos_loader_reclaim() == 0);
    CHECK(g_pt.slot_base[0] == pool);
    CHECK(ph->state == ARKRTOS_APP_STATE_VALID);

    g_flash_on = 0;
}

int main(void)
{

    test_check_header();
    test_reloc_words();
    test_reloc_movw();
    test_reloc_offsets();
#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
    test_crc_recheck();
    test_load_buffer_e2e();
#endif
    test_scan_pool_e2e();
    test_move_slot_e2e();

    printf("loader-host: %d passed, %d failed\n", g_pass, g_fail);
    if(g_fail == 0)
    {
        printf("LOADER-HOST OK\n");
    }
    return (g_fail == 0) ? 0 : 1;
}
