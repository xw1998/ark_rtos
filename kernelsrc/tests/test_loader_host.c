/**
* @file test_loader_host.c
* @brief 镜像头校验与重定位打补丁的宿主用例（不需要板子）
*
* 只覆盖两个纯函数：
*   - arkrtos_loader_check_header()：镜像头的每一条拒绝理由；
*   - arkrtos_loader_reloc_apply()：整字 / MOVW+MOVT 两类表项的补丁、跨块切断时
*     的可提交长度与前进指针。
*
* 为什么这两个值得在宿主上钉死：重定位错一位不会报错，只会让镜像在板上跑到
* 别的地址上，现象是"偶发 HardFault"或"读到错常量"。四遍差分的产物是否正确
* 落进池里，全靠这两个函数。
*
* 明确没验证：装载/擦除/搬移/卸载（要设备与 Flash 端口）、签名、崩溃计数、
* 任务真正启动。那些只能上板，不要把本文件读成"loader 已验证"。
*/

#include <stdio.h>
#include <string.h>

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

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    (void)addr; (void)size;
    return -1;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr; (void)data; (void)len;
    return -1;
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

arkrtos_partition_table_t *arkrtos_ptable_get(void) { return &g_pt; }
void arkrtos_ptable_clear_slots(void) {}
int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size)
{
    (void)type; (void)base; (void)size; return -1;
}
void arkrtos_ptable_free(uint32 slot) { (void)slot; }
int32 arkrtos_ptable_move(uint32 slot, uint32 new_base) { (void)slot; (void)new_base; return -1; }
int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    (void)addr; (void)p_base; (void)p_size; return -1;
}
int32 arkrtos_ptable_find_task(uint32 task_id) { (void)task_id; return -1; }
int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    (void)slot; (void)state; (void)entry; (void)task_id; return -1;
}
int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    (void)slot; (void)p_state; (void)p_entry; (void)p_task_id; return -1;
}
int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size)
{
    (void)bytes; (void)p_base; (void)p_size; return -1;
}
int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size) { (void)base; (void)size; return -1; }
int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size)
{
    (void)slot; (void)base; (void)size; return -1;
}
int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size)
{
    (void)slot; (void)p_base; (void)p_size; return -1;
}

uint32 arkrtos_layout_slot_count(void) { return 0u; }
uint32 arkrtos_layout_raw_allow(void)  { return 0u; }
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void) { return (const arkrtos_cfg_slot_t *)0; }
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index) { (void)index; return (const arkrtos_cfg_slot_t *)0; }

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

int main(void)
{
    test_check_header();
    test_reloc_words();
    test_reloc_movw();
    test_reloc_offsets();

    printf("loader-host: %d passed, %d failed\n", g_pass, g_fail);
    if(g_fail == 0)
    {
        printf("LOADER-HOST OK\n");
    }
    return (g_fail == 0) ? 0 : 1;
}
