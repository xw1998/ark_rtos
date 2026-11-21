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

/* 安装 / 启停用例要的“活”分区表：默认关，前面的用例仍走纯桩口径。 */
static int    g_pt_live       = 0;
/* arkrtos_task_register 的回答：-1 = 注册失败（默认），>0 = 装出来的任务号。 */
static int32  g_task_reg_ret  = -1;
/* arkrtos_crash_disabled 的回答：0 = 没被禁用，非 0 = 已禁用（值就是原因码）。 */
static uint32 g_crash_disabled = 0u;
/* arkrtos_crash_fault 的副作用：置上就模拟“这次故障之后到达上限”。 */
static uint32 g_crash_fault_after = 0u;


void arkrtos_ready_del(int32 task_idx)        { (void)task_idx; }
void arkrtos_delay_disarm(int32 task_idx)     { (void)task_idx; }
void arkrtos_sched_activate_higher(uint8 p)   { (void)p; }
void arkrtos_task_release_resources(int32 t)  { (void)t; }

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)entry; (void)stack_bottom; (void)stack_size;
    (void)priority; (void)period_ms;
    return g_task_reg_ret;
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
static int   g_erase_fail = 0;    /* 1 = 擦除失败但写仍成功：单独一支，不和写失败混用 */
static int   g_get_slot_fail = 0; /* 1 = 分区表读槽失败（模拟记录损坏 / 未初始化） */
static int   g_ram_info_fail = 0; /* 1 = RAM 窗口查询失败 */
static int   g_alloc_fail    = 0; /* 1 = 分区表分配不出记录（槽表满） */
static int   g_slot_ptr_null = 0; /* 1 = 配置槽数得出来、但取不出那一条 */
static uint32 g_flash_write_n   = 0u; /* 本次用例里第几次写（从 1 起算） */
static uint32 g_flash_fail_at   = 0u; /* 非 0 = 只让第 n 次写失败 */
static uint32 g_flash_corrupt_at = 0u;/* 非 0 = 第 n 次写成功后篡改最后一个字节 */
static uint32 g_flash_fail_from = 0u;   /* 非 0 = 第 n 次写起一律失败（重试也不放过） */
static uint32 g_flash_corrupt_from = 0u;/* 非 0 = 第 n 次写起每次都改坏最后一个字节 */
static int   g_alloc_n    = 0;
static int   g_free_n     = 0;
static arkrtos_cfg_slot_t g_fake_slot;
/* 多槽配置：g_slot_count > 1 时用这张表（覆盖「类型不符的槽要跳过」「操作者
 * 点名了第几个槽」这两条选址分支）；等于 1 时仍用 g_fake_slot，前面那些
 * 单槽用例的断言不受影响。 */
static arkrtos_cfg_slot_t g_slot_arr[4];
static uint32 g_slot_count   = 1u;
static int    g_pt_move_fail = 0;   /* 1 = 分区表搬移失败（槽号越界的替身） */

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    if(!g_flash_on || g_erase_fail)
    {
        return -1;
    }
    memset((void *)(uintptr_t)addr, 0xFF, size);   /* 擦除 == 全 0xFF，与 NOR 同义 */
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    g_flash_write_n++;

    if(!g_flash_on || g_flash_fail ||
       ((g_flash_fail_at != 0u) && (g_flash_write_n == g_flash_fail_at)) ||
       ((g_flash_fail_from != 0u) && (g_flash_write_n >= g_flash_fail_from)))
    {
        return -1;
    }
    memcpy((void *)(uintptr_t)addr, data, len);

    /* 定点篡改：写盘成功、但落盘字节与「以为写下去的那份」不一致。
     * 这正是回读复核要抓的形态 —— Flash 位翻转 / 写保护下的半写。 */
    if(((g_flash_corrupt_at != 0u) && (g_flash_write_n == g_flash_corrupt_at) &&
        (len != 0u)) ||
       ((g_flash_corrupt_from != 0u) && (g_flash_write_n >= g_flash_corrupt_from) &&
        (len != 0u)))
    {
        ((uint8 *)(uintptr_t)addr)[len - 1u] ^= 0xFFu;
    }
    return 0;
}

uint32 arkrtos_log_get_level(void) { return 0u; }

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

uint32 arkrtos_crash_fault(uint32 slot, uint32 reason)
{
    (void)slot;
    if(g_crash_fault_after != 0u)
    {
        g_crash_disabled = g_crash_fault_after;
    }
    return reason;
}
uint32 arkrtos_crash_disabled(uint32 slot)             { (void)slot; return g_crash_disabled; }
void   arkrtos_crash_forget(uint32 slot)               { (void)slot; g_crash_disabled = 0u; }
const char *arkrtos_crash_reason_name(uint32 reason)   { (void)reason; return "stub"; }
void   arkrtos_fault_record(uint32 type, int32 task_id);
void   arkrtos_fault_record(uint32 type, int32 task_id) { (void)type; (void)task_id; }

int32 arkrtos_mini_on_fault(int32 task_id) { (void)task_id; return 0; }

void arkrtos_mpu_build_task(arkrtos_task_t *p_task) { (void)p_task; }
void arkrtos_mpu_reset(void) {}

/* 设备读端的脚本流：流式安装的“串口”。写端只认 ACK / NAK 两个字节并计数，
 * 因为“有没有告诉主机停手”和“算不算装成”是两件事。 */
static const uint8 *g_dev_rx     = 0;
static uint32 g_dev_rx_len       = 0u;
static uint32 g_dev_rx_pos       = 0u;
static int    g_dev_rx_dry       = 0;    /* 1 = 立刻报读失败 */
static uint32 g_dev_ack          = 0u;
static uint32 g_dev_nak          = 0u;

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    uint32 left;
    uint32 n;

    (void)handle;

    if((g_dev_rx == 0) || g_dev_rx_dry || (len <= 0))
    {
        return -1;
    }

    left = g_dev_rx_len - g_dev_rx_pos;
    if(left == 0u)
    {
        return -1;              /* 断流：读端空转若干次后由调用方报超时 */
    }

    n = (uint32)len;
    if(n > left)
    {
        n = left;
    }

    memcpy(pdata, g_dev_rx + g_dev_rx_pos, n);
    g_dev_rx_pos += n;

    return (int32)n;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle;

    if((pdata != 0) && (len >= 1))
    {
        if(pdata[0] == 0x06u)
        {
            g_dev_ack++;
        }
        else if(pdata[0] == 0x15u)
        {
            g_dev_nak++;
        }
    }

    return len;
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

    if(!g_layout_on || g_alloc_fail)
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
    if(g_pt_move_fail || (slot >= (uint32)ARKRTOS_SLOT_ARRAY_MAX))
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
int32 arkrtos_ptable_find_task(uint32 task_id)
{
    uint32 i;

    if(!g_pt_live)
    {
        (void)task_id;
        return -1;
    }
    if(task_id == 0u)
    {
        return -1;
    }
    for(i = 0u; (i < g_pt.slot_max) && (i < (uint32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(g_pt.slot_task_id[i] == task_id)
        {
            return (int32)i;
        }
    }
    return -1;
}
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
    if(g_get_slot_fail || !g_pt_live)
    {
        (void)slot; (void)p_state; (void)p_entry; (void)p_task_id; return -1;
    }
    if((slot >= g_pt.slot_max) || (slot >= (uint32)ARKRTOS_SLOT_ARRAY_MAX))
    {
        return -1;
    }
    if(p_state != 0)
    {
        *p_state = g_pt.slot_state[slot];
    }
    if(p_entry != 0)
    {
        *p_entry = g_pt.slot_entry[slot];
    }
    if(p_task_id != 0)
    {
        *p_task_id = g_pt.slot_task_id[slot];
    }
    return 0;
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
    if(g_ram_info_fail || !g_pt_live)
    {
        (void)slot; (void)p_base; (void)p_size; return -1;
    }
    if((slot >= g_pt.slot_max) || (slot >= (uint32)ARKRTOS_SLOT_ARRAY_MAX))
    {
        return -1;
    }
    if(p_base != 0)
    {
        *p_base = g_pt.slot_ram_base[slot];
    }
    if(p_size != 0)
    {
        *p_size = g_pt.slot_ram_size[slot];
    }
    return 0;
}

uint32 arkrtos_layout_slot_count(void)
{
    return g_layout_on ? ((g_slot_count != 0u) ? g_slot_count : 1u) : 0u;
}
static int g_raw_allow = 0;
uint32 arkrtos_layout_raw_allow(void)  { return g_raw_allow ? 1u : 0u; }
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void)
{
    if(!g_layout_on)
    {
        return (const arkrtos_cfg_slot_t *)0;
    }
    return (g_slot_count > 1u) ? g_slot_arr : &g_fake_slot;
}
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    if(!g_layout_on || g_slot_ptr_null)
    {
        return (const arkrtos_cfg_slot_t *)0;
    }
    if(g_slot_count > 1u)
    {
        return (index < g_slot_count) ? &g_slot_arr[index] : (const arkrtos_cfg_slot_t *)0;
    }
    return (index == 0u) ? &g_fake_slot : (const arkrtos_cfg_slot_t *)0;
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

    /* MOVW 表项要盖住两条 16 位指令，必须半字对齐（loader_hdr_check_reloc 的
     * off%2 分支）。但表项编码本身把偏移按半字存放：ENTRY 用整数除法，
     * 奇数偏移在编码时就被削成偶数，解码回来的 off 恒为偶数 —— 那条分支
     * 在当前编码下不可达（保留作防御）。这里断言编码口径本身。 */
    hdr_init(&h);
    h.reloc_count = 1u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    CHECK(ARKRTOS_APP_RELOC_OFF(ARKRTOS_APP_RELOC_ENTRY(h.payload_offset + 1u,
                                                         ARKRTOS_APP_RELOC_KIND_ROM_MOVW)) == h.payload_offset);

    /* 半字对齐但非 4 字节对齐的 MOVW 表项是合法的（Thumb 32 位指令只需半字），
     * 而同一偏移做成数据类表项（需要 4 字节对齐）必须被拒 */
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(h.payload_offset + 2u, ARKRTOS_APP_RELOC_KIND_ROM_MOVW);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == 0);
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(h.payload_offset + 2u, ARKRTOS_APP_RELOC_KIND_ROM);
    CHECK(arkrtos_loader_check_header(&h, rel, VALID_TYPES) == ARKRTOS_LOADER_ERR_RELOC);

    /* RAM 需求必须在单块上限内：0 与超限都必须拒（装上也启动不了） */
    hdr_init(&h); h.ram_size = 0u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_SIZE);
    hdr_init(&h); h.ram_size = (uint32)SLOT_RAM_MAX_BLOCK + 1u;
    CHECK(arkrtos_loader_check_header(&h, (const uint32 *)0, VALID_TYPES) == ARKRTOS_LOADER_ERR_SIZE);

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

    /* 负向 6：头本身不合法 -> 连反算都不该开始（报头校验的原因，不是 CRC） */
    memcpy(&saved, img, sizeof(saved));
    ((arkrtos_app_header_t *)(void *)img)->type = 0u;
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) != 0);
    memcpy(img, &saved, sizeof(saved));

    /* MOVW 表项：反算要在两个半字上做逆运算，不能按整字减 delta。
     * 落盘形态用 reloc_apply 现场打出来（它对 MOVW 的编码另有独立用例）。 */
    memset(img, 0, sizeof(img));
    hdr_init(&h);
    h.type             = ARKRTOS_APP_TYPE_APP;
    h.image_size       = 64u;
    h.reloc_count      = 1u;
    h.nominal_base     = nominal_base;
    h.nominal_ram_base = 0x20000000u;
    h.runtime_ram_base = 0x20001000u;
    h.payload_offset   = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off            = h.payload_offset;
    total              = ARKRTOS_APP_TOTAL_LEN(&h);
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0x60u + i);
    }
    rel[0] = ARKRTOS_APP_RELOC_ENTRY(pay_off, ARKRTOS_APP_RELOC_KIND_ROM_MOVW);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel[0]));
    /* 上一段的 delta_rom 是按「两条表项」的负载偏移算的，这里表项只有一条，
     * 负载偏移随之不同，增量必须按这一段重算。 */
    delta_rom = (base + pay_off) - nominal_base;
    h.crc32 = nominal_crc(img, &h);
    memcpy(img, &h, sizeof(h));
    {
        uint32 idx = 0u;

        /* buf 是「这一块的首地址」，块内偏移相对它计算：要直接把补丁打到
         * img 上，就得把 buf 指到负载起点，buf_off 也传负载偏移。 */
        (void)arkrtos_loader_reloc_apply(img + pay_off, pay_off, h.image_size, &h,
                                         rel, &idx, delta_rom, 0u);
    }
    got = 0u;
    CHECK(arkrtos_loader_crc_recheck(img, base, total, &got) == 0);
    CHECK(got == h.crc32);
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

    /* 认领的三个拒绝分支：头字段不合法（跳一个分配单位）、总长超出可用区、
     * 副本停在 UNCOMMITTED。三者都不得登记，也不得打乱 b/c/d 的登记。 */
    {
        arkrtos_app_header_t eh;
        uint32 be = base_d + (uint32)ARKRTOS_POOL_ALLOC_UNIT;

        hdr_init(&eh);
        eh.type           = 0u;            /* 池里不收的类型 */
        eh.state          = ARKRTOS_APP_STATE_VALID;
        eh.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&eh);
        memcpy((void *)(uintptr_t)be, &eh, sizeof(eh));

        hdr_init(&eh);
        eh.type           = ARKRTOS_APP_TYPE_APP;
        eh.state          = ARKRTOS_APP_STATE_VALID;
        eh.image_size     = (uint32)IMAGE_POOL_USABLE_SIZE;    /* 总长必然超出 */
        eh.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&eh);
        memcpy((void *)(uintptr_t)(be + (uint32)ARKRTOS_POOL_ALLOC_UNIT), &eh, sizeof(eh));

        hdr_init(&eh);
        eh.type           = ARKRTOS_APP_TYPE_APP;
        eh.state          = ARKRTOS_APP_STATE_UNCOMMITTED;     /* 半途掉电的残片 */
        eh.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&eh);
        memcpy((void *)(uintptr_t)(be + 2u * (uint32)ARKRTOS_POOL_ALLOC_UNIT), &eh, sizeof(eh));
    }

    /* 正向：只有 b/c/d 三个被登记，a 整段跳过。
     * 顺带把「裸镜像认领」的运行期闸门打开跑一遍：本档编译进去的是空实现
     * （APP_ALLOW_RAW_IMAGE != 1），但入口那一步必须走到。 */
    g_raw_allow = 1;
    CHECK(arkrtos_loader_scan() == 2u);                    /* 两个 App（b、c） */
    g_raw_allow = 0;
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

    /* 逐扇区搬移：reclaim_mode != GLOBAL 时不做整体压实，而是把「正在清空的
     * 扇区」里的活镜像先挪到更低的位置再擦这一扇。上面那次走的是压实分支，
     * 回收内层的这段搬移没被跑到，所以这里清池重摆一次专门走它。
     * 顺带在槽 1 放一条空记录（type 非 FREE、base/size 都是 0）：压实与回收的
     * 内层循环都必须跳过它，不能把它当成占位者，也不能把它搬走。 */
    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_SIZE);
    memcpy((void *)(uintptr_t)old_base, img, (size_t)total);

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode  = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base    = pool;
    g_pt.pool_sector  = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units   = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size    = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.reclaim_mode = (uint32)ARKRTOS_CFG_RECLAIM_MINIMAL;
    g_pt.slot_max     = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = old_base;
    g_pt.slot_size[0]  = total;
    g_pt.slot_type[1]  = (uint32)ARKRTOS_SLOT_APP;   /* 空记录：非 FREE 却没有区间 */
    g_pt.slot_state[1] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[1]  = 0u;
    g_pt.slot_size[1]  = 0u;

    /* 写不下去时搬移必须整体放弃：镜像留在原处，一个扇区都不擦。
     * 这里求的是「半份副本不许被当成装好了」，所以只看结果不看中间态。 */
    g_flash_fail = 1;
    CHECK(arkrtos_loader_reclaim() == 0);
    CHECK(g_pt.slot_base[0] == old_base);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)old_base == (uint32)ARKRTOS_APP_MAGIC);
    g_flash_fail = 0;

    CHECK(arkrtos_loader_reclaim() == 1);
    CHECK(g_pt.slot_base[0] == pool);
    CHECK(*(const volatile uint32 *)(const void *)(uintptr_t)old_base == 0xFFFFFFFFu);
    CHECK(g_pt.slot_type[1] == (uint32)ARKRTOS_SLOT_APP);   /* 空记录原样留着 */
    CHECK(g_pt.slot_base[1] == 0u);

    g_flash_on = 0;
}


/* ------------------------------------------------------------------ */
/* 流式安装（load_dev / load_driver / stream_image）                    */
/* ------------------------------------------------------------------ */

/* 标称形态的 crc32：头（crc32 / state / runtime_ram_base 三个字段按 0 代）
 * + 重定位表 + 负载。与打包工具同一套公式，写成独立助手是为了让本文件的
 * 流式安装用例不依赖 -DARKRTOS_USE_IMAGE_CRC_RECHECK 那一档。 */
static uint32 stream_nominal_crc(const uint8 *img, const arkrtos_app_header_t *h)
{
    uint32 crc = arkrtos_loader_crc_header(h);
    uint32 rt_len = h->reloc_count * ARKRTOS_APP_RELOC_SIZE;

    if(rt_len != 0u)
    {
        crc = arkrtos_crc32(img + ARKRTOS_APP_RELOC_OFFSET, rt_len, crc);
    }

    return arkrtos_crc32(img + ARKRTOS_APP_PAYLOAD_OFFSET(h), h->image_size, crc);
}

/* 造一份「打包形态」的镜像：头 + 一条 ROM 整字表项 + 负载，crc32 按标称形态算。
 * 流式安装读到的正是这串字节，所以它同时充当「串口上会来什么」。 */
static void stream_put(uint8 *img, arkrtos_app_header_t *h, uint32 nominal_base,
                       uint32 nominal_ram, uint32 type, uint32 pay_bytes)
{
    uint32 rel[1];
    uint32 pay_off;
    uint32 i;

    memset(img, 0, 1024u);
    hdr_init(h);
    h->type             = type;
    h->image_size       = pay_bytes;
    h->reloc_count      = 1u;
    h->nominal_base     = nominal_base;
    h->nominal_ram_base = nominal_ram;
    h->payload_offset   = ARKRTOS_APP_PAYLOAD_OFFSET(h);
    pay_off             = h->payload_offset;

    rel[0] = ARKRTOS_APP_RELOC_ENTRY(pay_off, ARKRTOS_APP_RELOC_KIND_ROM);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));

    for(i = 0u; i < pay_bytes; i++)
    {
        img[pay_off + i] = (uint8)(0x30u + i);
    }
    *(uint32 *)(void *)(img + pay_off) = nominal_base + 0x40u;

    memcpy(img, h, sizeof(*h));
    ((arkrtos_app_header_t *)(void *)img)->crc32 = stream_nominal_crc(img, h);
}

/* 这一条走的是真正的安装实现：头从设备读、表体落盘后校验、负载边收边打补丁、
 * 回读 CRC 对齐之后才置 VALID。为什么值得在宿主上钉：这条链上任何一处偏移算错，
 * 板上都不会报错——只会跳到错地址上执行，或者把半份镜像当成功。 */
static void test_stream_install_e2e(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 base;
    uint32 ram_base;
    uint32 total;
    uint32 pay_off;
    int32 slot;

    base     = (uint32)(uintptr_t)pool;
    ram_base = (uint32)(uintptr_t)ram;

    memset(pool, 0xFF, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_layout_on  = 1;
    g_flash_on   = 1;
    g_flash_fail = 0;
    g_scan_on    = 1;
    g_pt_live    = 1;

    stream_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 64u);
    total   = ARKRTOS_APP_TOTAL_LEN(&h);
    pay_off = h.payload_offset;

    /* 正向：头 + 表 + 负载全从设备来，落盘后提交成 VALID */
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    g_dev_ack = 0u; g_dev_nak = 0u;
    g_alloc_n = 0; g_free_n = 0;
    slot = arkrtos_loader_load_dev(7, 0u);
    CHECK(slot == 0);
    CHECK(g_alloc_n == 1);
    CHECK(g_free_n == 0);
    CHECK(g_dev_ack == 2u);            /* 表体一帧 + 负载一帧 */
    CHECK(g_dev_nak == 0u);
    CHECK(g_dev_rx_pos == g_dev_rx_len);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->state == ARKRTOS_APP_STATE_VALID);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->runtime_ram_base == ram_base);
    /* delta_rom = (base + payload_offset) - nominal_base = payload_offset */
    CHECK(*(uint32 *)(void *)(pool + pay_off) == (base + 0x40u + pay_off));
    CHECK(pool[pay_off + 4u] == (uint8)0x34u);
    CHECK(g_pt.slot_type[0] == (uint32)ARKRTOS_SLOT_APP);
    CHECK(g_pt.slot_base[0] == base);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);

    /* 负向：镜像长度比头里声明的短 —— 一个槽位都不该占 */
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0;
    CHECK(arkrtos_loader_load_dev(7, total - 1u) == ARKRTOS_LOADER_ERR_SIZE);
    CHECK(g_alloc_n == 0);

    /* 负向：设备读不出来（头都收不齐） */
    g_dev_rx_dry = 1;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_SIZE);
    g_dev_rx_dry = 0;

    /* 负向：魔数不对 —— 在分槽之前就被拒 */
    {
        uint32 saved = *(uint32 *)(void *)img;

        *(uint32 *)(void *)img = 0u;
        g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
        g_alloc_n = 0; g_dev_nak = 0u;
        CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_MAGIC);
        CHECK(g_alloc_n == 0);
        *(uint32 *)(void *)img = saved;
    }

    /* 负向：重定位表非法（表项落到负载之外）—— 表体落盘后立刻被拒并发 NAK，
     * 负载一个字节都不收（这是「改 Flash 之前先拒」那条设计） */
    {
        uint32 saved = *(uint32 *)(void *)(img + ARKRTOS_APP_RELOC_OFFSET);

        *(uint32 *)(void *)(img + ARKRTOS_APP_RELOC_OFFSET) =
            ARKRTOS_APP_RELOC_ENTRY(pay_off + 64u + 4u, ARKRTOS_APP_RELOC_KIND_ROM);
        arkrtos_ptable_clear_slots();
        g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
        g_alloc_n = 0; g_free_n = 0; g_dev_nak = 0u; g_dev_ack = 0u;
        CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_RELOC);
        CHECK(g_dev_nak == 1u);
        CHECK(g_dev_ack == 0u);
        CHECK(g_free_n == 1);
        *(uint32 *)(void *)(img + ARKRTOS_APP_RELOC_OFFSET) = saved;
    }

    /* 负向：传输丢字节（负载里改一个）—— 滚动 CRC 对不上，发 NAK，槽位还回去 */
    {
        uint8 saved = img[pay_off + 5u];

        img[pay_off + 5u] ^= 0xFFu;
        arkrtos_ptable_clear_slots();
        g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
        g_alloc_n = 0; g_free_n = 0; g_dev_nak = 0u;
        CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_CRC);
        CHECK(g_dev_nak == 1u);
        CHECK(g_free_n == 1);
        img[pay_off + 5u] = saved;
    }

    /* 负向：表体还没收完就断流 —— 读端超时，报长度错，槽位还回去 */
    arkrtos_ptable_clear_slots();
    g_dev_rx = img; g_dev_rx_len = (uint32)ARKRTOS_APP_HEADER_SIZE; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_SIZE);
    CHECK(g_free_n == 1);

    /* 负向：写头就失败 —— 槽位还回去，且只还一次 */
    arkrtos_ptable_clear_slots();
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_flash_fail = 1; g_alloc_n = 0; g_free_n = 0;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_alloc_n == 1);
    CHECK(g_free_n == 1);
    g_flash_fail = 0;

    /* 驱动：类型不对直接拒；对上了走同一条流式安装 */
    CHECK(arkrtos_loader_load_driver_dev(7, &h, 0u) == ARKRTOS_LOADER_ERR_PARAM);

    arkrtos_ptable_clear_slots();
    g_fake_slot.type = (uint32)ARKRTOS_CFG_TYPE_DRIVER;
    stream_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_DRIVER, 64u);
    total = ARKRTOS_APP_TOTAL_LEN(&h);
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0; g_dev_ack = 0u; g_dev_nak = 0u;
    CHECK(arkrtos_loader_load_driver(7, 0u) == 0);
    CHECK(g_dev_ack == 2u);
    CHECK(g_pt.slot_type[0] == (uint32)ARKRTOS_SLOT_DRIVER);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);

    /* 驱动入口连头都读不回来：报长度错，不是硬着头皮拿半份头往下走 */
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_dev_rx_dry = 1;
    CHECK(arkrtos_loader_load_driver(7, 0u) == ARKRTOS_LOADER_ERR_SIZE);
    g_dev_rx_dry = 0;

    /* 设备号非法 */
    CHECK(arkrtos_loader_load_dev(-1, 0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_load_driver(-1, 0u) == ARKRTOS_LOADER_ERR_PARAM);

    g_dev_rx = 0; g_dev_rx_len = 0u; g_dev_rx_pos = 0u;
    g_scan_on = 0; g_pt_live = 0; g_layout_on = 0; g_flash_on = 0;
}

/* ------------------------------------------------------------------ */
/* 版本把关（check_install）：三态 + 跳过无效副本与裸镜像               */
/* ------------------------------------------------------------------ */

static void test_check_install(void)
{
    static uint8 slotbuf[0x400] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    arkrtos_app_header_t old;
    arkrtos_app_header_t t;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.slot_max = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt_live     = 1;

    hdr_init(&h);
    h.type       = ARKRTOS_APP_TYPE_APP;
    h.image_id   = 0x11223344u;
    h.version    = 5u;
    h.image_size = 64u;
    h.crc32      = 0xAAAA5555u;

    CHECK(arkrtos_loader_check_install(0) == ARKRTOS_LOADER_ERR_PARAM);

    t = h;
    t.image_id = 0u;
    CHECK(arkrtos_loader_check_install(&t) == ARKRTOS_LOADER_ERR_PARAM);

    /* 池里没有同 id 的副本：首次安装放行 */
    CHECK(arkrtos_loader_check_install(&h) == 0);

    /* 摆一份已装副本：version 3、同 id 同尺寸、crc 不同 */
    memset(slotbuf, 0, sizeof(slotbuf));
    old = h;
    old.version = 3u;
    old.crc32   = 0x00CC00CCu;
    old.state   = ARKRTOS_APP_STATE_VALID;
    memcpy(slotbuf, &old, sizeof(old));

    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = (uint32)(uintptr_t)slotbuf;

    CHECK(arkrtos_loader_check_install(&h) == 0);          /* 升级：5 > 3 */

    t = h;
    t.version = 3u;
    t.crc32   = 0x00CC00CCu;
    CHECK(arkrtos_loader_check_install(&t) == ARKRTOS_LOADER_ERR_DUP);

    t.crc32 = 0x11112222u;
    CHECK(arkrtos_loader_check_install(&t) == ARKRTOS_LOADER_ERR_VERSION);   /* 同版本重建 */

    t.version = 2u;
    CHECK(arkrtos_loader_check_install(&t) == ARKRTOS_LOADER_ERR_VERSION);   /* 降级回灌 */

    /* 被禁用（INVALID）的副本不算已装：重装同一版本正是恢复手段 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_INVALID;
    t.version = 3u;
    t.crc32   = 0x00CC00CCu;
    CHECK(arkrtos_loader_check_install(&t) == 0);

    /* 裸镜像（没有魔数）跳过：不能被当成本镜像的旧副本 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    *(uint32 *)(void *)slotbuf = 0u;
    CHECK(arkrtos_loader_check_install(&t) == 0);
    *(uint32 *)(void *)slotbuf = (uint32)ARKRTOS_APP_MAGIC;

    /* 同类型槽里的**另一份**镜像：image_id 不同就必须跳过，不能参与版本比对
     * （否则装了 A 的人再装 B，B 会被 A 的版本号挡回去） */
    {
        static uint8 slotbuf2[0x400] __attribute__((aligned(8)));
        arkrtos_app_header_t other;

        hdr_init(&other);
        other.type       = ARKRTOS_APP_TYPE_APP;
        other.image_id   = 0x99887766u;
        other.version    = 99u;
        other.image_size = 64u;
        other.crc32      = 0x0BADF00Du;
        other.state      = ARKRTOS_APP_STATE_VALID;
        memcpy(slotbuf2, &other, sizeof(other));

        g_pt.slot_type[1]  = (uint32)ARKRTOS_SLOT_APP;
        g_pt.slot_state[1] = (uint32)ARKRTOS_APP_SLOT_LOADED;
        g_pt.slot_base[1]  = (uint32)(uintptr_t)slotbuf2;

        CHECK(arkrtos_loader_check_install(&h) == 0);   /* 仍按 0 号槽的 v3 判「升级」 */

        g_pt.slot_type[1]  = 0u;
        g_pt.slot_state[1] = 0u;
        g_pt.slot_base[1]  = 0u;
    }

    /* 驱动镜像只看驱动槽：APP 槽里的同 id 副本不参与比对 */
    t = h;
    t.type    = ARKRTOS_APP_TYPE_DRIVER;
    t.version = 1u;
    t.crc32   = 0x00CC00CCu;
    CHECK(arkrtos_loader_check_install(&t) == 0);

    g_pt_live = 0;
}

/* ------------------------------------------------------------------ */
/* 卸载（uninstall）与固定槽位的清槽 / 空白判定                        */
/* ------------------------------------------------------------------ */

static void test_uninstall_e2e(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    uint32 base = (uint32)(uintptr_t)pool;

    memset(pool, 0xFF, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    g_layout_on  = 1;
    g_flash_on   = 1;
    g_flash_fail = 0;
    g_pt_live    = 1;
    g_free_n     = 0;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = 0x1000u;
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = 0x20000000u;
    g_fake_slot.ram_size = 0x1000u;

    /* 槽号越界 / 空记录：都是参数错 */
    CHECK(arkrtos_loader_uninstall((uint32)ARKRTOS_SLOT_ARRAY_MAX) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_uninstall(0u) == ARKRTOS_LOADER_ERR_PARAM);

    /* 摆一份已装镜像的记录：头在池里、状态 LOADED */
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, 0x20000000u, 0u);
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = base;
    g_pt.slot_size[0]  = 0x1000u;
    g_pt.slot_entry[0] = base + ARKRTOS_APP_HEADER_SIZE;

    /* 裸镜像（RAW）不能替它卸载：那会连带擦掉不属于它的区间 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_RAW;
    CHECK(arkrtos_loader_uninstall(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;

    /* Flash 写失败：记录必须留着 —— 绝不能报「已卸载」 */
    g_flash_fail = 1;
    g_free_n = 0;
    CHECK(arkrtos_loader_uninstall(0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 0);
    g_flash_fail = 0;

    /* 正向：把头的 type 写成 0（Flash 只能把 1 写成 0，这是唯一可编程的作废标记），
     * 丢掉记录、擦掉自己那一槽，返回回收的扇区数 */
    g_free_n = 0;
    CHECK(arkrtos_loader_uninstall(0u) == 1);
    CHECK(g_free_n == 1);
    CHECK(pool[0] == 0xFFu);                    /* 固定槽位：整槽擦干净 */
    CHECK(pool[ARKRTOS_APP_HEADER_SIZE] == 0xFFu);

    /* 记录大小为 0（异常记录）：丢掉记录就返回，不擦 */
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = base;
    g_pt.slot_size[0]  = 0u;
    g_free_n = 0;
    CHECK(arkrtos_loader_uninstall(0u) == 0);
    CHECK(g_free_n == 1);

    /* clear_fixed_slot / fixed_slot_blank：只在固定槽位模式下有意义 */
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    CHECK(arkrtos_loader_clear_fixed_slot(0) == 0);
    CHECK(arkrtos_loader_fixed_slot_blank(0) == ARKRTOS_LOADER_FAIL_LAYOUT);

    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    CHECK(arkrtos_loader_clear_fixed_slot(-1) == 0);
    CHECK(arkrtos_loader_clear_fixed_slot(9) == 0);
    CHECK(arkrtos_loader_fixed_slot_blank(-1) == ARKRTOS_LOADER_FAIL_PARAM);
    CHECK(arkrtos_loader_fixed_slot_blank(9) == ARKRTOS_LOADER_FAIL_PARAM);

    /* 已擦除的槽：判定为空白，清槽直接返回（不浪费一次擦除） */
    memset(pool, 0xFF, sizeof(pool));
    CHECK(arkrtos_loader_fixed_slot_blank(0) == 1);
    CHECK(arkrtos_loader_clear_fixed_slot(0) == 0);

    /* 槽里有内容：判定不是空白，清槽把它擦回 0xFF */
    memset(pool, 0x00, sizeof(pool));
    CHECK(arkrtos_loader_fixed_slot_blank(0) == 0);
    CHECK(arkrtos_loader_clear_fixed_slot(0) == 0);
    CHECK(pool[0] == 0xFFu);
    CHECK(pool[0x0FFFu] == 0xFFu);

    /* 擦除失败要说实话 */
    memset(pool, 0x00, sizeof(pool));
    g_flash_on = 0;                       /* 擦除桩关掉 == 擦不动 */
    CHECK(arkrtos_loader_clear_fixed_slot(0) == ARKRTOS_LOADER_ERR_FLASH);
    g_flash_on = 1;

    g_pt_live = 0; g_layout_on = 0; g_flash_on = 0;
}

/* ------------------------------------------------------------------ */
/* 启停（start / stop / state / autostart / on_fault / start_manual）  */
/* ------------------------------------------------------------------ */

static void test_start_stop_e2e(void)
{
    static uint8 pool[0x1000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 i;

    memset(pool, 0, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.slot_max = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;

    g_pt_live        = 1;
    g_crash_disabled = 0u;
    g_crash_fault_after = 0u;
    g_task_reg_ret   = 1;

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 1;

    /* 一份 LOADED 的 App：有入口、有 RAM 块，任务注册成功 */
    g_pt.slot_state[0]   = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_entry[0]   = base + ARKRTOS_APP_HEADER_SIZE;
    g_pt.slot_base[0]    = base;
    g_pt.slot_size[0]    = 0x400u;
    g_pt.slot_ram_base[0] = ram_base;
    g_pt.slot_ram_size[0] = (uint32)sizeof(ram);
    g_pt.slot_task_id[0] = 0u;

    CHECK(arkrtos_loader_start(0u) == 1);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_RUNNING);
    CHECK(g_pt.slot_task_id[0] == 1u);
    CHECK(arkrtos_task_table[0].is_priv == 0u);        /* App 跑非特权 */
    CHECK(arkrtos_task_table[0].rom_start == base);
    CHECK(arkrtos_task_table[0].ram_start == ram_base);
    CHECK(arkrtos_task_table[0].ram_size == (uint32)sizeof(ram));

    /* 槽号越界 */
    CHECK(arkrtos_loader_start((uint32)ARKRTOS_SLOT_ARRAY_MAX) == ARKRTOS_LOADER_ERR_PARAM);

    /* 被崩溃策略禁用的槽不自己回来 */
    g_crash_disabled = 1u;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_crash_disabled = 0u;

    /* 状态不是 LOADED/RAW，或入口为 0 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_EMPTY;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    i = g_pt.slot_entry[0];
    g_pt.slot_entry[0] = 0u;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_pt.slot_entry[0] = i;

    /* 槽的类型既不是 App 也不是驱动 */
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_FREE;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_PARAM);
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;

    /* 没有 RAM 块：栈切不出来 */
    i = g_pt.slot_ram_base[0];
    g_pt.slot_ram_base[0] = 0u;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_pt.slot_ram_base[0] = i;

    /* 任务注册失败 */
    g_task_reg_ret = 0;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_TASK);
    g_task_reg_ret = 1;

    /* 驱动槽：同一条启动路径，但保留特权 */
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_DRIVER;
    CHECK(arkrtos_loader_start(0u) == 1);
    CHECK(arkrtos_task_table[0].is_priv == 1u);
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;

    /* 驱动专用入口先查类型 */
    CHECK(arkrtos_loader_start_driver_slot(0u) == ARKRTOS_LOADER_ERR_PARAM);
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_DRIVER;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    CHECK(arkrtos_loader_start_driver_slot(0u) == 1);
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_task_id[0] = 0u;
    CHECK(arkrtos_loader_start_driver_manual(0u) == 1);
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_APP;

    /* 状态查询：越界回 0xFFFFFFFF，其余照表 */
    CHECK(arkrtos_loader_state((uint32)ARKRTOS_SLOT_ARRAY_MAX) == 0xFFFFFFFFu);
    CHECK(arkrtos_loader_state(0u) == g_pt.slot_state[0]);

    /* 停机：任务号落在表内才能停；基址处有镜像头就回 LOADED（镜像字节还在） */
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, ram_base, 0u);
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_task_id[0] = 1u;
    arkrtos_task_table[0].ram_start = ram_base;
    arkrtos_task_table[0].ram_size  = (uint32)sizeof(ram);
    CHECK(arkrtos_loader_stop(0u) == 0);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(arkrtos_task_table[0].status == (uint32)ARKRTOS_TASK_INVALID);

    CHECK(arkrtos_loader_stop((uint32)ARKRTOS_SLOT_ARRAY_MAX) == ARKRTOS_LOADER_ERR_PARAM);
    g_pt.slot_task_id[0] = 0u;
    CHECK(arkrtos_loader_stop(0u) == ARKRTOS_LOADER_ERR_STATE);

    /* 裸镜像停完必须退回 RAW：LOADED 在压实眼里是可搬的安装镜像 */
    arkrtos_task_count = 3;
    arkrtos_task_table[2].ram_start = 0x30000000u;
    arkrtos_task_table[2].ram_size  = 0x1000u;
    g_pt.slot_task_id[0] = 3u;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_base[0] = 0u;
    CHECK(arkrtos_loader_stop(0u) == 0);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_RAW);

    /* 自动启动：只数 App / 驱动各自那一类，且只数真的起来的 */
    memset(&g_pt.slot_state[0], 0, sizeof(uint32) * (size_t)ARKRTOS_SLOT_ARRAY_MAX);
    memset(&g_pt.slot_task_id[0], 0, sizeof(uint32) * (size_t)ARKRTOS_SLOT_ARRAY_MAX);
    memset(&g_pt.slot_autostart[0], 0, sizeof(uint32) * (size_t)ARKRTOS_SLOT_ARRAY_MAX);
    g_pt.slot_max = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    for(i = 0u; i < 4u; i++)
    {
        g_pt.slot_type[i]      = (uint32)ARKRTOS_SLOT_APP;
        g_pt.slot_state[i]     = (uint32)ARKRTOS_APP_SLOT_LOADED;
        g_pt.slot_entry[i]     = base + ARKRTOS_APP_HEADER_SIZE;
        g_pt.slot_base[i]      = base;
        g_pt.slot_size[i]      = 0x400u;
        g_pt.slot_ram_base[i]  = ram_base;
        g_pt.slot_ram_size[i]  = (uint32)sizeof(ram);
        g_pt.slot_autostart[i] = 1u;
    }
    g_pt.slot_type[1] = (uint32)ARKRTOS_SLOT_DRIVER;       /* 驱动那条遍历不该数它 */
    g_pt.slot_autostart[3] = 0u;                          /* 没开自启的那个不该数 */
    g_task_reg_ret = 1;
    CHECK(arkrtos_loader_start_autostart() == 2u);        /* 槽 0 与槽 2 */
    CHECK(arkrtos_loader_start_autostart_driver() == 1u); /* 槽 1 */

    /* 故障计数：找不到槽位 -> 交给默认处理；找到且达上限 -> 禁用并回 1 */
    CHECK(arkrtos_loader_on_fault(0) == ARKRTOS_LOADER_FAIL_PARAM);
    CHECK(arkrtos_loader_on_fault(9) == ARKRTOS_LOADER_FAIL_NOTFOUND);

    g_pt.slot_type[0]    = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_task_id[0] = 2u;
    g_crash_fault_after  = 0u;
    CHECK(arkrtos_loader_on_fault(2) == 0);               /* 未达上限：调用方去恢复 */

    g_crash_fault_after = 7u;                             /* 这次故障之后到上限 */
    CHECK(arkrtos_loader_on_fault(2) == 1);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_INVALID);
    g_crash_fault_after = 0u;

    /* start_manual：被禁用但镜像仍合法 -> 操作者点名重来，清零后放行 */
    g_crash_disabled = 1u;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_INVALID;
    g_pt.slot_base[0]  = base;
    g_pt.slot_entry[0] = base + ARKRTOS_APP_HEADER_SIZE;
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, ram_base, 0u);
    CHECK(arkrtos_loader_start_manual(0u) == 1);

    /* 被禁用且镜像已不合法 -> 只能重装，不启动 */
    memset(pool, 0, sizeof(pool));
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_INVALID;
    CHECK(arkrtos_loader_start_manual(0u) == ARKRTOS_LOADER_ERR_STATE);

    CHECK(arkrtos_loader_start_manual((uint32)ARKRTOS_SLOT_ARRAY_MAX) == ARKRTOS_LOADER_ERR_PARAM);

    g_crash_disabled = 0u;
    g_pt_live = 0;
}


/* ------------------------------------------------------------------ */
/* 池余量 / 固定槽覆盖安装 / 自动选址 / 驱动包装                        */
/* ------------------------------------------------------------------ */

/* 池余量统计的是「物理空闲字节」，不是「能装下的总量」：镜像落点要求对齐，
 * 同样总量被切碎就装不下。所以两个读数分开核 —— 总空闲与最长连续段。
 * 顺带钉住 FIXED 模式下 reclaim 的早退：固定槽不是池空间，没有可回收的。 */
static void test_pool_free_e2e(void)
{
    uint32 pool = (uint32)IMAGE_POOL_BASE;
    uint32 largest = 0u;
    uint32 total;

    if(map_pool_at(pool, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_pool_free_e2e: cannot map image pool at 0x%08X\n",
               (unsigned)pool);
        return;
    }

    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_USABLE_SIZE);

    /* 全干净：总空闲就是可用区全长，最长段也是它 */
    CHECK(arkrtos_loader_pool_free(&largest) == (uint32)IMAGE_POOL_USABLE_SIZE);
    CHECK(largest == (uint32)IMAGE_POOL_USABLE_SIZE);
    /* 不关心最长段时允许传空指针 */
    CHECK(arkrtos_loader_pool_free((uint32 *)0) == (uint32)IMAGE_POOL_USABLE_SIZE);

    /* 打两个洞：一个 4 字节、一个 0x200 字节，剩下一大段连续空闲。
     * largest 若写成「总空闲」在这里就会露馅（总空闲不等于最长段）。 */
    *(volatile uint32 *)(void *)(uintptr_t)(pool + 0x1000u) = 0u;
    memset((void *)(uintptr_t)(pool + 0x3000u), 0x00, 0x200u);

    total = arkrtos_loader_pool_free(&largest);
    CHECK(total == ((uint32)IMAGE_POOL_USABLE_SIZE - 4u - 0x200u));
    CHECK(largest == ((uint32)IMAGE_POOL_USABLE_SIZE - 0x3200u));

    /* 固定槽模式的回收是空操作：槽里那块 Flash 永远属于这个槽 */
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_base   = pool;
    g_pt.pool_sector = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units  = (uint32)IMAGE_POOL_UNITS;
    g_flash_on = 1;
    g_erase_fail = 0;
    CHECK(arkrtos_loader_reclaim() == 0);
    g_flash_on = 0;

    memset(&g_pt, 0, sizeof(g_pt));
}

/* 自动选址安装：落点由池里最长干净连续区决定，RAM 由分区表给。
 * 这条路径与固定槽完全不同 —— 固定槽认配置地址，自动选址认池内容，
 * 所以三条负向（池满 / 要不到 RAM / 分配不出记录）必须各自单独核。 */
static void test_auto_install_e2e(void)
{
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 pool = (uint32)IMAGE_POOL_BASE;
    uint32 total;
    uint32 pay_off;
    int32  slot;

    if(map_pool_at(pool, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_auto_install_e2e: cannot map image pool at 0x%08X\n",
               (unsigned)pool);
        return;
    }

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode  = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base    = pool;
    g_pt.pool_sector  = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units   = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size    = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.reclaim_mode = (uint32)ARKRTOS_CFG_RECLAIM_MINIMAL;
    g_pt.slot_max     = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    g_layout_on   = 1;
    g_scan_on     = 1;
    g_pt_live     = 1;
    g_flash_on    = 1;
    g_flash_fail  = 0;
    g_erase_fail  = 0;
    g_ram_alloc_ok = 1;

    stream_put(img, &h, pool, (uint32)0x20010000u, ARKRTOS_APP_TYPE_APP, 64u);
    total   = ARKRTOS_APP_TOTAL_LEN(&h);
    pay_off = h.payload_offset;

    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_USABLE_SIZE);
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0; g_bind_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    g_dev_ack = 0u; g_dev_nak = 0u;

    /* 正向：落点就是池底（对齐后），RAM 取分区表给的那块 */
    slot = arkrtos_loader_load_dev(7, 0u);
    CHECK(slot == 0);
    CHECK(g_dev_ack == 2u);
    CHECK(g_dev_nak == 0u);
    CHECK(g_alloc_n == 1);
    CHECK(g_pt.slot_base[0] == pool);
    CHECK(g_pt.slot_type[0] == (uint32)ARKRTOS_SLOT_APP);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(g_bind_n == 1u);
    CHECK(g_bind_base == (uint32)0x20010000u);
    CHECK(((const arkrtos_app_header_t *)(const void *)(uintptr_t)pool)->state ==
          ARKRTOS_APP_STATE_VALID);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)(pool + pay_off) ==
          (pool + 0x40u + pay_off));

    /* 候选落点压在已登记的记录上：自动选址必须跳过那一段，改落到它后面。
     * 落回原处等于把一个镜像写进另一条记录声明的区间里。 */
    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_USABLE_SIZE);
    arkrtos_ptable_clear_slots();
    g_pt.slot_type[1] = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_base[1] = pool;
    g_pt.slot_size[1] = 0x1000u;
    g_alloc_n = 0; g_free_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    slot = arkrtos_loader_load_dev(7, 0u);
    CHECK(slot == 0);
    CHECK(g_pt.slot_base[0] == (pool + 0x1000u));
    CHECK(g_pt.slot_base[1] == pool);          /* 原记录不动 */
    arkrtos_ptable_clear_slots();

    /* 池里没有干净连续区 -> NOSPACE，一个记录都不该占 */
    memset((void *)(uintptr_t)pool, 0x00, (size_t)IMAGE_POOL_USABLE_SIZE);
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_NOSPACE);
    CHECK(g_alloc_n == 0);
    CHECK(g_free_n == 0);

    /* 选址成功但要不到 RAM 块 -> 同样 NOSPACE，且不能留下记录 */
    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_USABLE_SIZE);
    arkrtos_ptable_clear_slots();
    g_ram_alloc_ok = 0;
    g_alloc_n = 0; g_free_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_NOSPACE);
    CHECK(g_alloc_n == 0);

    /* 落点与 RAM 都有了，但分区表分配不出记录 -> NO_SLOT */
    g_ram_alloc_ok = 1;
    g_layout_on = 0;
    g_alloc_n = 0; g_free_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_NO_SLOT);
    CHECK(g_free_n == 0);

    g_layout_on = 1;
    g_dev_rx = 0;
    g_pt_live = 0;
    g_scan_on = 0;
    g_ram_alloc_ok = 0;
    g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 固定槽覆盖安装的两道闸：
 *   - 槽里有正在运行的镜像 -> 整帧拒绝（擦写会让它当场跑飞）；
 *   - 槽里只有已装镜像 -> 摘掉旧记录、整槽重新登记（detach 的交集分支）。
 * 第二条尤其要钉：不 detach 就会留下两条指向同一地址的记录，
 * 之后压实/回收各搬各的，池里出现两份有效镜像。 */
static void test_fixed_overwrite_e2e(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 total;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_layout_on = 1; g_flash_on = 1; g_flash_fail = 0;
    g_scan_on = 1; g_pt_live = 1; g_erase_fail = 0;

    stream_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 64u);
    total = ARKRTOS_APP_TOTAL_LEN(&h);

    memset(pool, 0xFF, sizeof(pool));

    /* 槽内是 RUNNING：拒绝，且不动任何记录 */
    g_alloc_n = 0; g_free_n = 0;
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_base[0]  = base;
    g_pt.slot_size[0]  = (uint32)sizeof(pool);
    /* 另一条 RUNNING 记录，但区间与本槽不相交：它不该让本次安装变 BUSY */
    g_pt.slot_type[1]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[1] = (uint32)ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_base[1]  = 0x00100000u;
    g_pt.slot_size[1]  = 0x1000u;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    g_dev_ack = 0u; g_dev_nak = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_BUSY);
    CHECK(g_alloc_n == 0);
    CHECK(g_free_n == 0);

    /* 覆盖安装：旧记录被 detach（free 一次），新镜像按整槽重新登记 */
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == 0);
    CHECK(g_free_n == 1);              /* detach 掉落在本槽内的旧记录 */
    CHECK(g_alloc_n == 1);
    CHECK(g_dev_ack == 2u);
    CHECK(g_dev_nak == 0u);
    CHECK(g_pt.slot_base[0] == base);
    CHECK(g_pt.slot_type[0] == (uint32)ARKRTOS_SLOT_APP);
    CHECK(g_pt.slot_state[0] == (uint32)ARKRTOS_APP_SLOT_LOADED);

    g_dev_rx = 0;
    g_pt_live = 0;
    g_scan_on = 0;
    g_layout_on = 0;
    g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 驱动侧四个包装：类型校验属于驱动门，语义属于通用门。
 * 两件事分开断言 —— 「App 槽不许从驱动门进」与「驱动槽从驱动门进得去」。 */
static void test_driver_wrappers(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x2000] __attribute__((aligned(8)));
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    g_pt_live = 1;
    g_flash_on = 1;
    g_flash_fail = 0;
    g_task_reg_ret = 1;
    arkrtos_task_count = 1;
    g_crash_disabled = 0u;

    memset(pool, 0xFF, sizeof(pool));
    scan_put_image(base, ARKRTOS_APP_TYPE_DRIVER, 4096u, ram_base, 0u);

    g_pt.slot_type[0]     = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0]    = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]     = base;
    g_pt.slot_size[0]     = (uint32)sizeof(pool);
    g_pt.slot_entry[0]    = base + ARKRTOS_APP_HEADER_SIZE;
    g_pt.slot_ram_base[0] = ram_base;
    g_pt.slot_ram_size[0] = (uint32)sizeof(ram);

    /* App 槽：四个驱动门一律拒绝/不认 */
    CHECK(arkrtos_loader_state_driver(0u) == 0xffffffffu);
    CHECK(arkrtos_loader_start_driver_slot(0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_stop_driver_slot(0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_start_driver_manual(0u) == ARKRTOS_LOADER_ERR_PARAM);

    /* 换成驱动槽：四门全部打通，且任务被标成特权（驱动的 MPU 策略） */
    g_pt.slot_type[0] = (uint32)ARKRTOS_SLOT_DRIVER;
    CHECK(arkrtos_loader_state_driver(0u) == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(arkrtos_loader_start_driver_slot(0u) == 1);
    CHECK(arkrtos_loader_state_driver(0u) == (uint32)ARKRTOS_APP_SLOT_RUNNING);
    CHECK(arkrtos_task_table[0].is_priv == 1u);

    CHECK(arkrtos_loader_stop_driver_slot(0u) == 0);
    CHECK(arkrtos_loader_state_driver(0u) == (uint32)ARKRTOS_APP_SLOT_LOADED);
    CHECK(arkrtos_loader_start_driver_manual(0u) == 1);
    CHECK(arkrtos_loader_state_driver(0u) == (uint32)ARKRTOS_APP_SLOT_RUNNING);

    /* 无参版本等价于对 0 号槽的那一次 */
    CHECK(arkrtos_loader_start_driver() == ARKRTOS_LOADER_ERR_STATE);  /* 已在运行 */

    g_task_reg_ret = -1;
    g_pt_live = 0;
    g_flash_on = 0;
    arkrtos_task_count = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}


/* 安装入口的参数与提示：提示一次性、越界即拒、以及 load_dev_hdr 一层的
 * 设备号与长度闸门（这两条在 load_dev 外层拦不住，必须直接打在 hdr 层）。 */
extern int32 arkrtos_loader_load_dev_hdr(int32 dev, const arkrtos_app_header_t *p_hdr,
                                         uint32 image_len);

static void test_hint_and_params(void)
{
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 total;

    /* 提示就是一次性的一个下标：设进去能读回来，-1 表示不指定 */
    arkrtos_loader_slot_hint_set(2);
    CHECK(arkrtos_loader_slot_hint_get() == 2);
    arkrtos_loader_slot_hint_set(-1);
    CHECK(arkrtos_loader_slot_hint_get() == -1);

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    stream_put(img, &h, (uint32)0x00400000u, (uint32)0x20000000u, ARKRTOS_APP_TYPE_APP, 64u);
    total = ARKRTOS_APP_TOTAL_LEN(&h);

    /* 设备号非法：在最外层就该拒，不进任何分配 */
    CHECK(arkrtos_loader_load_dev(-1, 0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_load_driver(-1, 0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_load_dev_hdr(-1, &h, 0u) == ARKRTOS_LOADER_ERR_PARAM);

    /* 调用方声明的长度比头里写的总长还短：整帧拒绝，不去碰 Flash */
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    CHECK(arkrtos_loader_load_dev(7, total - 1u) == ARKRTOS_LOADER_ERR_SIZE);

    g_dev_rx = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 固定槽的清理闸门：0 长度是空操作；不是整扇区倍数必须拒绝（擦除粒度是扇区，
 * 半个尾巴会伸进邻居）；配置数得出来但取不出那一条，按「没有这个槽」处理。 */
static void test_fixed_clear_edges(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    uint32 base = (uint32)(uintptr_t)pool;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base = base;
    g_fake_slot.type = (uint32)ARKRTOS_CFG_TYPE_APP;

    g_layout_on = 1;
    g_flash_on  = 1;
    g_erase_fail = 0;

    memset(pool, 0x00, sizeof(pool));       /* 非空白，任何 erase 都会真跑 */

    g_fake_slot.size = 0u;                  /* 没有区间：什么都不擦 */
    CHECK(arkrtos_loader_clear_fixed_slot(0) == 0);

    g_fake_slot.size = 0x1800u;             /* 1.5 个扇区：几何不成立 */
    CHECK(arkrtos_loader_clear_fixed_slot(0) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(pool[0] == 0x00u);                /* 拒绝时一个字节都不能动 */

    g_slot_ptr_null = 1;                    /* 槽数得出来、取不出那一条 */
    CHECK(arkrtos_loader_clear_fixed_slot(0) == 0);
    CHECK(arkrtos_loader_fixed_slot_blank(0) == ARKRTOS_LOADER_FAIL_LAYOUT);
    g_slot_ptr_null = 0;

    g_layout_on = 0;
    g_flash_on  = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 固定槽登记两个失败点：记录分配不出来（NO_SLOT）与落点擦不干净（ERR_FLASH）。
 * 后一条要求把刚登记的记录还回去 —— 不还就会留下一条指向未清干净 Flash 的记录。 */
static void test_fixed_claim_edges(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 total;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_layout_on = 1; g_flash_on = 1; g_flash_fail = 0;
    g_erase_fail = 0; g_scan_on = 1; g_pt_live = 1;

    stream_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 64u);
    total = ARKRTOS_APP_TOTAL_LEN(&h);

    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();

    /* 槽表满：一条记录都拿不到 -> NO_SLOT，池内容原样 */
    g_alloc_fail = 1;
    g_alloc_n = 0; g_free_n = 0;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_NO_SLOT);
    CHECK(g_alloc_n == 0);
    CHECK(g_free_n == 0);
    g_alloc_fail = 0;

    /* 落点擦不干净：登记要还回去，报 ERR_FLASH，不能留一条半成品记录 */
    memset(pool, 0x00, sizeof(pool));        /* 非空白 -> 会走到真擦 */
    g_erase_fail = 1;
    g_alloc_n = 0; g_free_n = 0;
    arkrtos_ptable_clear_slots();
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);                    /* 刚登记的那条被还回去了 */
    g_erase_fail = 0;

    g_dev_rx = 0;
    g_pt_live = 0;
    g_scan_on = 0;
    g_layout_on = 0;
    g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 卸载的三条边：读不到记录、正在运行（要先停任务，连带 halt_task 整条链）、
 * 以及 AUTO 模式卸载后交给 reclaim 收空间。 */
static void test_uninstall_special(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    uint32 base = (uint32)(uintptr_t)pool;

    if(map_pool_at((uint32)IMAGE_POOL_BASE, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_uninstall_special: cannot map image pool\n");
        return;
    }

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt_live = 1;
    g_flash_on = 1;
    g_flash_fail = 0;
    g_erase_fail = 0;
    g_get_slot_fail = 0;

    memset(pool, 0xFF, sizeof(pool));
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, 0u, 0u);

    g_pt.slot_type[0]    = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0]   = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]    = base;
    g_pt.slot_size[0]    = (uint32)sizeof(pool);

    /* 读不到槽位记录 -> PARAM（与「记录在但状态不对」是两种回答） */
    g_get_slot_fail = 1;
    CHECK(arkrtos_loader_uninstall(0u) == ARKRTOS_LOADER_ERR_PARAM);
    g_get_slot_fail = 0;

    /* 正在运行 + 活任务：卸载先硬停，连它自己开的线程一起收（按 RAM 窗口） */
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_task_id[0] = 1u;
    arkrtos_task_count = 1;
    arkrtos_task_table[0].ram_start = (unsigned long)base;
    arkrtos_task_table[0].ram_size  = (uint32)sizeof(pool);
    arkrtos_task_table[0].status    = ARKRTOS_TASK_RUNNING;

    CHECK(arkrtos_loader_uninstall(0u) == 4);      /* 0x4000 / 0x1000 = 4 个扇区 */
    CHECK(arkrtos_task_table[0].status == ARKRTOS_TASK_INVALID);
    CHECK(pool[0] == 0xFFu);                       /* 固定槽整槽被擦干净 */
    arkrtos_task_count = 0;

    /* 固定槽擦不动：镜像已经作废（type 被清零），但空间没交回 —— 报 FLASH */
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, 0u, 0u);
    g_pt.layout_mode   = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = base;
    g_pt.slot_size[0]  = (uint32)sizeof(pool);
    g_pt.slot_task_id[0] = 0u;
    g_erase_fail = 1;
    CHECK(arkrtos_loader_uninstall(0u) == ARKRTOS_LOADER_ERR_FLASH);
    g_erase_fail = 0;

    /* AUTO 模式：卸载把空间交回池子（reclaim），返回值是回收到的扇区数 */
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, 0u, 0u);
    memset((void *)(uintptr_t)IMAGE_POOL_BASE, 0xFF, (size_t)IMAGE_POOL_USABLE_SIZE);
    g_pt.layout_mode  = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base    = (uint32)IMAGE_POOL_BASE;
    g_pt.pool_units   = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size    = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.reclaim_mode = (uint32)ARKRTOS_CFG_RECLAIM_MINIMAL;
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = base;
    g_pt.slot_size[0]  = (uint32)sizeof(pool);
    g_pt.slot_task_id[0] = 0u;
    CHECK(arkrtos_loader_uninstall(0u) >= 0);

    g_pt_live = 0;
    g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* 启停里两条「查表失败」：读不到记录就报 PARAM（不是 STATE），
 * RAM 窗口查不到则报 STATE —— 这两种失败对上层是不同的处置。 */
static void test_start_stop_edges(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x2000] __attribute__((aligned(8)));
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    g_pt_live = 1;
    g_flash_on = 1;
    g_flash_fail = 0;
    g_task_reg_ret = 1;
    arkrtos_task_count = 1;
    g_crash_disabled = 0u;

    memset(pool, 0xFF, sizeof(pool));
    scan_put_image(base, ARKRTOS_APP_TYPE_APP, 4096u, 0u, 0u);

    g_pt.slot_type[0]     = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0]    = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]     = base;
    g_pt.slot_size[0]     = (uint32)sizeof(pool);
    g_pt.slot_entry[0]    = base + ARKRTOS_APP_HEADER_SIZE;
    g_pt.slot_ram_base[0] = ram_base;
    g_pt.slot_ram_size[0] = (uint32)sizeof(ram);

    g_get_slot_fail = 1;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_PARAM);
    CHECK(arkrtos_loader_stop(0u) == ARKRTOS_LOADER_ERR_PARAM);
    g_get_slot_fail = 0;

    g_ram_info_fail = 1;
    CHECK(arkrtos_loader_start(0u) == ARKRTOS_LOADER_ERR_STATE);
    g_ram_info_fail = 0;

    g_task_reg_ret = -1;
    g_pt_live = 0;
    g_flash_on = 0;
    arkrtos_task_count = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}


/* 提交点的两种坏法：落盘字节与「以为写下去的那份」不一致（回读复核要拒），
 * 以及提交那一个字写不进去（镜像停在 UNCOMMITTED，必须作废而不是当装好了）。 */
static void test_install_commit_edges(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[1024] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 total;

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_layout_on = 1; g_flash_on = 1; g_flash_fail = 0;
    g_erase_fail = 0; g_scan_on = 1; g_pt_live = 1;

    stream_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 64u);
    total = ARKRTOS_APP_TOTAL_LEN(&h);

    /* 第 3 次写（负载）落盘后被翻了一位：回读复核必须拒，并回滚记录 */
    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0;
    g_flash_write_n = 0; g_flash_fail_at = 0; g_flash_corrupt_at = 3;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_CRC);
    CHECK(g_free_n == 1);
    g_flash_corrupt_at = 0;

    /* 第 4 次写（提交 VALID）失败：镜像停在 UNCOMMITTED，记录必须回滚 */
    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0;
    g_flash_write_n = 0; g_flash_fail_at = 4;
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->state ==
          ARKRTOS_APP_STATE_UNCOMMITTED);   /* 掉电点的真实形态 */
    g_flash_fail_at = 0;

    g_dev_rx = 0;
    g_pt_live = 0;
    g_scan_on = 0;
    g_layout_on = 0;
    g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* ------------------------------------------------------------------ */
/* 固定槽位选址的分支（提示 / 类型 / 尺寸）：多槽配置                  */
/* ------------------------------------------------------------------ */

/* 造一份「内存镜像」形态的镜像（load_buffer 的输入）：头 + nrel 条 ROM 整字
 * 表项（偏移从负载起按 4 递增、升序）+ 负载，crc32 按标称形态算。返回总长。 */
static uint32 buf_put(uint8 *img, arkrtos_app_header_t *h, uint32 nominal_base,
                      uint32 nominal_ram, uint32 type, uint32 nrel, uint32 pay_bytes)
{
    uint32 rel[8];
    uint32 pay_off;
    uint32 i;

    memset(img, 0, 256u + (4u * nrel) + pay_bytes);
    hdr_init(h);
    h->type             = type;
    h->image_size       = pay_bytes;
    h->reloc_count      = nrel;
    h->nominal_base     = nominal_base;
    h->nominal_ram_base = nominal_ram;
    h->payload_offset   = ARKRTOS_APP_PAYLOAD_OFFSET(h);
    pay_off             = h->payload_offset;

    for(i = 0u; i < nrel; i++)
    {
        rel[i] = ARKRTOS_APP_RELOC_ENTRY(pay_off + (4u * i), ARKRTOS_APP_RELOC_KIND_ROM);
    }
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, 4u * nrel);

    for(i = 0u; i < pay_bytes; i++)
    {
        img[pay_off + i] = (uint8)(0x50u + i);
    }
    if(pay_bytes >= 4u)
    {
        *(uint32 *)(void *)(img + pay_off) = nominal_base + 0x40u;
    }

    memcpy(img, h, sizeof(*h));
    ((arkrtos_app_header_t *)(void *)img)->crc32 = stream_nominal_crc(img, h);

    return ARKRTOS_APP_TOTAL_LEN(h);
}

static void test_fixed_reserve_hint(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[2048] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 base0    = (uint32)(uintptr_t)pool;
    uint32 base1    = (uint32)(uintptr_t)pool + 0x2000u;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 total;

    memset(pool, 0xFF, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    /* clear_span 要求槽长是物理扇区的整数倍（擦除粒度就是扇区），
     * 所以这里取 0x400：两条槽 0x1000 / 0x400 都是它的整数倍。 */
    g_pt.pool_sector = 0x400u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    /* 两条配置槽：0 号是驱动槽、1 号才是 App 槽。装 App 时必须跳过 0 号。
     * ram_size 填 0 表示这一槽不额外限制 RAM 窗口。 */
    memset(g_slot_arr, 0, sizeof(g_slot_arr));
    g_slot_arr[0].base     = base0;
    g_slot_arr[0].size     = 0x1000u;
    g_slot_arr[0].type     = (uint32)ARKRTOS_CFG_TYPE_DRIVER;
    g_slot_arr[0].ram_base = ram_base;
    g_slot_arr[1].base     = base1;
    g_slot_arr[1].size     = 0x400u;
    g_slot_arr[1].type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_slot_arr[1].ram_base = ram_base;

    g_slot_count = 2u;
    g_layout_on  = 1;
    g_flash_on   = 1;
    g_flash_fail = 0;
    g_pt_live    = 1;

    total = buf_put(img, &h, base1, ram_base, ARKRTOS_APP_TYPE_APP, 1u, 64u);

    /* 正向：类型不符的 0 号槽被跳过，落在 1 号槽 */
    g_alloc_n = 0; g_free_n = 0;
    arkrtos_loader_slot_hint_set(-1);
    CHECK(arkrtos_loader_load_buffer(img, total) >= 0);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)base1 == (uint32)ARKRTOS_APP_MAGIC);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)base0 == 0xFFFFFFFFu);

    /* 操作者点名 1 号槽：0 号槽连看都不看 */
    g_alloc_n = 0; g_free_n = 0;
    arkrtos_loader_slot_hint_set(1);
    CHECK(arkrtos_loader_load_buffer(img, total) >= 0);

    /* 点名 0 号槽（驱动槽）装 App：类型对不上 → 明确报 NO_SLOT，
     * 不能默默换到别的槽去装 */
    arkrtos_loader_slot_hint_set(0);
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_NO_SLOT);

    /* 点名 1 号槽但装不下：要说 SIZE（点名的槽装不下），不是「没有空槽」 */
    total = buf_put(img, &h, base1, ram_base, ARKRTOS_APP_TYPE_APP, 1u, 1100u);
    arkrtos_loader_slot_hint_set(1);
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_SIZE);

    /* 没点名、又没有任何 App 槽装得下：报 NOSPACE */
    arkrtos_loader_slot_hint_set(-1);
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_NOSPACE);

    arkrtos_loader_slot_hint_set(-1);
    g_slot_count = 1u;
    g_layout_on  = 0;
    g_flash_on   = 0;
    g_pt_live    = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* ------------------------------------------------------------------ */
/* 流式安装：大表分帧 + MOVW 表项跨块                                 */
/* ------------------------------------------------------------------ */

/* 这样一份镜像同时压中三条路径：
 *   - 重定位表 198 条 = 792 字节 > 512，必须分两帧收（表体落盘与滚动 CRC 都
 *     要跨帧续算）；
 *   - 负载 1100 字节，分三帧；
 *   - 其中一条 MOVW 表项落在负载偏移 508 处，正好被 512 字节的块切开：
 *     本块只落盘到它之前，尾部留到下一块拼齐再打补丁。
 * 任何一处偏移算错，装完的镜像都过不了回读 CRC。 */
static void test_stream_big_image(void)
{
    static uint8 pool[0x8000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[4096] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 rel[200];
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 nrel     = 198u;
    uint32 pay_off;
    uint32 total;
    uint32 n        = 0u;
    uint32 i;
    uint32 k;

    memset(pool, 0xFF, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_slot_count = 1u;
    g_layout_on  = 1;
    g_flash_on   = 1;
    g_flash_fail = 0;
    g_scan_on    = 1;
    g_pt_live    = 1;

    memset(img, 0, sizeof(img));
    hdr_init(&h);
    h.type             = ARKRTOS_APP_TYPE_APP;
    h.image_size       = 1100u;
    h.reloc_count      = nrel;
    h.nominal_base     = base;
    h.nominal_ram_base = ram_base;
    h.payload_offset   = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off            = h.payload_offset;
    total              = ARKRTOS_APP_TOTAL_LEN(&h);

    /* 197 条整字表项（负载偏移 0/4/8/...，跳过 508）+ 1 条 MOVW（在 508） */
    for(i = 0u; i < 198u; i++)
    {
        uint32 off = 4u * i;

        rel[n++] = (off == 508u)
            ? ARKRTOS_APP_RELOC_ENTRY(pay_off + off, ARKRTOS_APP_RELOC_KIND_ROM_MOVW)
            : ARKRTOS_APP_RELOC_ENTRY(pay_off + off, ARKRTOS_APP_RELOC_KIND_ROM);
    }
    CHECK(n == nrel);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, 4u * nrel);

    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0x70u + i);
    }
    *(uint32 *)(void *)(img + pay_off) = base + 0x40u;
    for(k = 0u; k < 4u; k++)
    {
        img[pay_off + 508u + k] = (uint8)(0x11u * (k + 1u));   /* MOVW/MOVT 两条半字 */
    }

    memcpy(img, &h, sizeof(h));
    ((arkrtos_app_header_t *)(void *)img)->crc32 = stream_nominal_crc(img, &h);

    /* 正向：表体两帧 + 负载三帧，跨块表项打补丁后回读 CRC 必须对上 */
    arkrtos_ptable_clear_slots();
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u; g_dev_rx_dry = 0;
    g_dev_ack = 0u; g_dev_nak = 0u;
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 0u; g_flash_corrupt_at = 0u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == 0);
    CHECK(g_free_n == 0);
    CHECK(g_dev_nak == 0u);
    CHECK(g_dev_ack >= 3u);
    CHECK(g_dev_rx_pos == g_dev_rx_len);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->state == ARKRTOS_APP_STATE_VALID);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->runtime_ram_base == ram_base);
    /* delta_rom = (base + payload_offset) - nominal_base = payload_offset */
    CHECK(*(uint32 *)(void *)(pool + pay_off) == (base + 0x40u + pay_off));
    CHECK(pool[pay_off + 508u] == (uint8)0x11u);   /* 表项本身之外的花样没被动过 */

    /* 表体第二帧写失败：槽位必须还回去（第一帧已经落盘） */
    arkrtos_ptable_clear_slots();
    memset(pool, 0xFF, sizeof(pool));
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 3u;               /* 1=头 2=表第一帧 3=表第二帧 */
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    g_flash_fail_at = 0u;

    /* 负载第一帧写失败 */
    arkrtos_ptable_clear_slots();
    memset(pool, 0xFF, sizeof(pool));
    g_dev_rx = img; g_dev_rx_len = total; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 4u;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    g_flash_fail_at = 0u;

    /* 负载中途断流：读超时按长度错收场，槽位还回去 */
    arkrtos_ptable_clear_slots();
    memset(pool, 0xFF, sizeof(pool));
    g_dev_rx = img; g_dev_rx_len = total - 10u; g_dev_rx_pos = 0u;
    g_alloc_n = 0; g_free_n = 0;
    CHECK(arkrtos_loader_load_dev(7, 0u) == ARKRTOS_LOADER_ERR_SIZE);
    CHECK(g_free_n == 1);

    g_dev_rx = 0; g_dev_rx_len = 0u; g_dev_rx_pos = 0u;
    g_scan_on = 0; g_pt_live = 0; g_layout_on = 0; g_flash_on = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* ------------------------------------------------------------------ */
/* 内存镜像安装（load_buffer）的拒绝与写失败                           */
/* ------------------------------------------------------------------ */

static void test_load_buffer_failures(void)
{
    static uint8 pool[0x4000] __attribute__((aligned(8)));
    static uint8 ram[0x1000] __attribute__((aligned(8)));
    static uint8 img[2048] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    arkrtos_app_header_t saved;
    uint32 base     = (uint32)(uintptr_t)pool;
    uint32 ram_base = (uint32)(uintptr_t)ram;
    uint32 total;

    memset(pool, 0xFF, sizeof(pool));
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.pool_sector = 0x1000u;
    g_pt.slot_max    = (uint32)ARKRTOS_SLOT_ARRAY_MAX;

    memset(&g_fake_slot, 0, sizeof(g_fake_slot));
    g_fake_slot.base     = base;
    g_fake_slot.size     = (uint32)sizeof(pool);
    g_fake_slot.type     = (uint32)ARKRTOS_CFG_TYPE_APP;
    g_fake_slot.ram_base = ram_base;
    g_fake_slot.ram_size = (uint32)sizeof(ram);

    g_slot_count = 1u;
    g_layout_on  = 1;
    g_flash_on   = 1;
    g_flash_fail = 0;
    g_flash_fail_at = 0u;
    g_flash_corrupt_at = 0u;
    g_pt_live    = 1;

    total = buf_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 1u, 64u);

    /* 头本身不合法：在碰槽位之前就被拒（类型 0 不在池的接受范围里） */
    memcpy(&saved, img, sizeof(saved));
    ((arkrtos_app_header_t *)(void *)img)->type = 0u;
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_PARAM);
    memcpy(img, &saved, sizeof(saved));

    /* 长度比头里声明的总长少一个字节：同样在分槽之前被挡 */
    CHECK(arkrtos_loader_load_buffer(img, total - 1u) == ARKRTOS_LOADER_ERR_SIZE);

    /* 没有配置槽表：分不到槽，报负值且一个记录都不占 */
    g_layout_on = 0;
    g_alloc_n = 0; g_free_n = 0;
    CHECK(arkrtos_loader_load_buffer(img, total) < 0);
    CHECK(g_alloc_n == 0);
    g_layout_on = 1;

    /* 重定位表写失败：槽位还回去（只还一次） */
    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 2u;               /* 1=头 2=表 */
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    g_flash_fail_at = 0u;

    /* 大负载：分两帧搬（512 + 88），失败要落在各自的回滚分支上 */
    total = buf_put(img, &h, base, ram_base, ARKRTOS_APP_TYPE_APP, 1u, 600u);

    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 4u;               /* 1=头 2=表 3=负载第一帧 4=负载第二帧 */
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    g_flash_fail_at = 0u;

    /* 回读复核发现落盘字节被改：报 CRC，槽位还回去（提交点之前停下） */
    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_corrupt_at = 4u;
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_CRC);
    CHECK(g_free_n == 1);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->state ==
          ARKRTOS_APP_STATE_UNCOMMITTED);
    g_flash_corrupt_at = 0u;

    /* 提交那一个字写失败：镜像停在 UNCOMMITTED，记录回滚 */
    memset(pool, 0xFF, sizeof(pool));
    arkrtos_ptable_clear_slots();
    g_alloc_n = 0; g_free_n = 0; g_flash_write_n = 0u;
    g_flash_fail_at = 5u;               /* 1=头 2=表 3/4=负载 5=提交 */
    CHECK(arkrtos_loader_load_buffer(img, total) == ARKRTOS_LOADER_ERR_FLASH);
    CHECK(g_free_n == 1);
    CHECK(((const arkrtos_app_header_t *)(const void *)pool)->state ==
          ARKRTOS_APP_STATE_UNCOMMITTED);
    g_flash_fail_from = 0u;

    total = ARKRTOS_APP_TOTAL_LEN(&h);
    (void)total;
    g_layout_on = 0; g_flash_on = 0; g_pt_live = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* ------------------------------------------------------------------ */
/* 停止镜像：只停同一 RAM 窗口里的任务                                 */
/* ------------------------------------------------------------------ */

static void test_halt_image_window(void)
{
    uint32 i;

    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    arkrtos_task_count = 2;

    /* 0 号任务在别的 RAM 窗口里：它可能是别人的、也可能是内核自己的，
     * 停 1 号镜像时不能把它一起停掉 */
    arkrtos_task_table[0].status   = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].ram_start = 0x20000000u;
    arkrtos_task_table[0].ram_size  = 0x1000u;
    arkrtos_task_table[1].status   = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[1].ram_start = (uint32)(uintptr_t)&arkrtos_task_table[1];
    arkrtos_task_table[1].ram_size  = 0x1000u;

    arkrtos_loader_halt_image(2);

    CHECK(arkrtos_task_table[1].status == ARKRTOS_TASK_INVALID);   /* 窗口里的停掉 */
    CHECK(arkrtos_task_table[0].status == ARKRTOS_TASK_RUNNING);   /* 窗口外的不动 */

    arkrtos_task_count = 0;
}

/* ------------------------------------------------------------------ */
/* 控制台重试：被禁用的槽救不回来时要说 state                          */
/* ------------------------------------------------------------------ */

static void test_start_manual_disabled(void)
{
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.slot_max = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt_live     = 1;

    /* 槽因反复故障被禁用，而记录里连落点都没有：显式重试不能把它救回来
     * （没有镜像可验），只能让人重装 */
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_INVALID;
    g_pt.slot_base[0]  = 0u;
    g_crash_disabled   = 7u;            /* 非 0 = 已禁用，值是原因码 */

    CHECK(arkrtos_loader_start_manual(0) == ARKRTOS_LOADER_ERR_STATE);
    CHECK(g_crash_disabled == 7u);      /* 没被悄悄清掉 */

    g_crash_disabled = 0u;
    g_pt_live = 0;
    memset(&g_pt, 0, sizeof(g_pt));
}

/* ------------------------------------------------------------------ */
/* 搬移的失败面：表/负载分块、写失败、复核失败、提交失败、改指失败      */
/* ------------------------------------------------------------------ */

/* 把一份「已落盘」的镜像摆到 old_base，并把分区表置成 AUTO/LOADED。
 * 擦除态与三个写闸门一起复位，保证每个子用例从同一个起点出发。 */
static void move_fault_prep(uint32 pool, uint32 old_base,
                            const uint8 *img, uint32 total)
{
    memset((void *)(uintptr_t)pool, 0xFF, (size_t)IMAGE_POOL_SIZE);
    memcpy((void *)(uintptr_t)old_base, img, (size_t)total);

    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.layout_mode   = (uint32)ARKRTOS_LAYOUT_MODE_AUTO;
    g_pt.pool_base     = pool;
    g_pt.pool_sector   = (uint32)IMAGE_POOL_SECTOR;
    g_pt.pool_units    = (uint32)IMAGE_POOL_UNITS;
    g_pt.pool_size     = (uint32)IMAGE_POOL_USABLE_SIZE;
    g_pt.reclaim_mode  = (uint32)ARKRTOS_CFG_RECLAIM_GLOBAL;
    g_pt.slot_max      = (uint32)ARKRTOS_SLOT_ARRAY_MAX;
    g_pt.slot_type[0]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0]  = old_base;
    g_pt.slot_size[0]  = total;
    /* 1 号槽是一条空记录（非 FREE、区间为 0）：压实的内层循环必须跳过它，
     * 既不能把它当占位者，也不能把它搬走。 */
    g_pt.slot_type[1]  = (uint32)ARKRTOS_SLOT_APP;
    g_pt.slot_state[1] = (uint32)ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[1]  = 0u;
    g_pt.slot_size[1]  = 0u;

    g_flash_on         = 1;
    g_flash_fail       = 0;
    g_flash_fail_at    = 0u;
    g_flash_corrupt_at = 0u;
    g_flash_fail_from  = 0u;
    g_flash_corrupt_from = 0u;
    g_flash_write_n    = 0u;
    g_pt_move_fail     = 0;
}

static void test_move_fault_paths(void)
{
    static uint8 img[4096] __attribute__((aligned(8)));
    arkrtos_app_header_t h;
    uint32 rel[198];
    uint32 pool = (uint32)IMAGE_POOL_BASE;
    uint32 old_base;
    uint32 total;
    uint32 pay_off;
    uint32 i;

    if(map_pool_at(pool, (uint32)IMAGE_POOL_SIZE) == (void *)0)
    {
        printf("SKIP test_move_fault_paths: cannot map image pool\n");
        return;
    }

    /* 表 198 条 = 792 字节（两帧）、负载 1100 字节（三帧）：
     * 搬移里那两条 512 字节分块循环都会被切成多段。 */
    hdr_init(&h);
    h.type           = ARKRTOS_APP_TYPE_APP;
    h.state          = ARKRTOS_APP_STATE_VALID;
    h.image_size     = 1100u;
    h.reloc_count    = 198u;
    h.payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(&h);
    pay_off = h.payload_offset;
    total   = ARKRTOS_APP_TOTAL_LEN(&h);

    memset(img, 0xEE, sizeof(img));
    for(i = 0u; i < 198u; i++)
    {
        rel[i] = ARKRTOS_APP_RELOC_ENTRY(pay_off + (4u * i), ARKRTOS_APP_RELOC_KIND_ROM);
    }
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (uint8)(0x30u + i);
    }
    memcpy(img, &h, sizeof(h));

    old_base = pool + (uint32)IMAGE_POOL_SECTOR;

    /* 正向：分块搬完，落到池底并提交成 VALID */
    move_fault_prep(pool, old_base, img, total);
    CHECK(arkrtos_loader_reclaim() != 0u);
    CHECK(g_pt.slot_base[0] == pool);
    CHECK(((const arkrtos_app_header_t *)(const void *)(uintptr_t)pool)->state ==
          ARKRTOS_APP_STATE_VALID);

    /* 表体第一帧写不下去：整次搬移放弃，旧镜像原样留着 */
    /* 一次搬移的写入次序是固定的：1 头、2/3 表体两帧、4/5/6 负载三帧、
     * 7 写 VALID。回收会重试，所以失败必须「从第 n 次写起一直失败」，
     * 否则第二次尝试就把活干完了（只失败一次的旋钮测不出分支）。 */
    move_fault_prep(pool, old_base, img, total);
    g_flash_fail_from = 2u;
    /* 返回值是「擦掉的扇区数」，不是成败：搬移失败时逐扇区收尾照样会擦掉
     * 已经没人占的扇区，所以这里只看唯一的不变式 —— 记录仍指旧落点。 */
    (void)arkrtos_loader_reclaim();
    CHECK(g_pt.slot_base[0] == old_base);
    CHECK(*(const uint32 *)(const void *)(uintptr_t)old_base == (uint32)ARKRTOS_APP_MAGIC);
    g_flash_fail_from = 0u;

    /* 负载第一帧写不下去 */
    move_fault_prep(pool, old_base, img, total);
    g_flash_fail_from = 4u;
    /* 返回值是「擦掉的扇区数」，不是成败：搬移失败时逐扇区收尾照样会擦掉
     * 已经没人占的扇区，所以这里只看唯一的不变式 —— 记录仍指旧落点。 */
    (void)arkrtos_loader_reclaim();
    CHECK(g_pt.slot_base[0] == old_base);
    g_flash_fail_from = 0u;

    /* 提交（写 VALID）失败：新副本停在 UNCOMMITTED，记录仍指旧落点 */
    move_fault_prep(pool, old_base, img, total);
    g_flash_fail_from = 7u;
    /* 返回值是「擦掉的扇区数」，不是成败：搬移失败时逐扇区收尾照样会擦掉
     * 已经没人占的扇区，所以这里只看唯一的不变式 —— 记录仍指旧落点。 */
    (void)arkrtos_loader_reclaim();
    CHECK(g_pt.slot_base[0] == old_base);
    g_flash_fail_from = 0u;

    /* 落盘字节被改坏：整段复核不通过，同样放弃，不把半份副本当装好 */
    move_fault_prep(pool, old_base, img, total);
    g_flash_corrupt_from = 5u;
    /* 返回值是「擦掉的扇区数」，不是成败：搬移失败时逐扇区收尾照样会擦掉
     * 已经没人占的扇区，所以这里只看唯一的不变式 —— 记录仍指旧落点。 */
    (void)arkrtos_loader_reclaim();
    CHECK(g_pt.slot_base[0] == old_base);
    g_flash_corrupt_from = 0u;

    /* 分区表改指失败：不能留下「新副本已写好、记录却没挪」的半吊子状态 */
    move_fault_prep(pool, old_base, img, total);
    g_pt_move_fail = 1;
    /* 返回值是「擦掉的扇区数」，不是成败：搬移失败时逐扇区收尾照样会擦掉
     * 已经没人占的扇区，所以这里只看唯一的不变式 —— 记录仍指旧落点。 */
    (void)arkrtos_loader_reclaim();
    CHECK(g_pt.slot_base[0] == old_base);
    g_pt_move_fail = 0;

    /* 旧副本的头已经坏了：前置校验直接拒（MAGIC），一个字节都不写 */
    move_fault_prep(pool, old_base, img, total);
    memset((void *)(uintptr_t)old_base, 0x00, (size_t)total);
    g_flash_write_n = 0u;
    CHECK(arkrtos_loader_reclaim() == 0u);
    CHECK(g_flash_write_n == 0u);

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
    test_move_fault_paths();
    test_stream_install_e2e();
    test_check_install();
    test_uninstall_e2e();
    test_start_stop_e2e();
    test_pool_free_e2e();
    test_auto_install_e2e();
    test_fixed_overwrite_e2e();
    test_driver_wrappers();
    test_hint_and_params();
    test_fixed_clear_edges();
    test_fixed_claim_edges();
    test_uninstall_special();
    test_start_stop_edges();
    test_install_commit_edges();
    test_fixed_reserve_hint();
    test_stream_big_image();
    test_load_buffer_failures();
    test_halt_image_window();
    test_start_manual_disabled();

    printf("loader-host: %d passed, %d failed\n", g_pass, g_fail);
    if(g_fail == 0)
    {
        printf("LOADER-HOST OK\n");
    }
    return (g_fail == 0) ? 0 : 1;
}
