/**
* @file test_apppack_host.c
* @brief 四遍差分打包（arkrtos_apppack.c）的宿主用例（不需要板子）
*
* 覆盖的唯一公开入口 arkrtos_apppack_build()：
*   - 四类重定位（整字 ROM / 整字 RAM / MOVW+MOVT ROM / MOVW+MOVT RAM）
*     的识别、表项编码（相对负载偏移 + 类型位 + 表长平移）；
*   - 把表应用回 A 之后必须逐字节等于 D 的复核（含复核完还原成 A）；
*   - 镜像头逐字段（含 image_id 在算头 CRC 之前填好）与 CRC 覆盖面；
*   - 入口解析两条合法形态（向量表 / 平铺小程序 PUSH 序言）与拒绝；
*   - 每一条参数闸门与两个「宁可拒绝也不猜」的出口（DIFF / ENTRY）。
*
* 为什么值得在宿主上钉死：装载器只认「四遍差分证明过可重定位」的镜像，
* 一旦表项偏移错 2 字节或 CRC 覆盖面漂了，镜像在板上的表现是「偶发 HardFault」
* 或「装上先报 -5」，都不可能靠读代码定位。这里把打包器的判定与账目钉成读数，
* 并且**产出的镜像必须真的通过装载器的 check_header**（链接真 arkrtos_loader.c，
* 不用自己抄一份校验规则）。
*
* 明确没验证：板上 cc --pack 的四遍真实链接、真实编译器产出的指令流、
* 装载期按落点打补丁、CRC 在 Flash 上的回读复核。那些只能上板。
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "arkrtos_apppack.h"
#include "arkrtos_app_image.h"
#include "arkrtos_loader.h"
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
/* 内核桩：loader.c 里用得到、但本用例根本不走到的那些                */
/* （本文件只调 crc / crc_header / check_header 三个纯函数）          */
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
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr; (void)data; (void)len;
    return 0;
}

uint32 arkrtos_log_get_level(void) { return 0u; }

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

uint32 arkrtos_crash_fault(uint32 slot, uint32 reason)
{
    (void)slot; (void)reason;
    return 0u;
}

uint32 arkrtos_crash_disabled(uint32 slot)            { (void)slot; return 0u; }
void   arkrtos_crash_forget(uint32 slot)              { (void)slot; }
const char *arkrtos_crash_reason_name(uint32 reason)  { (void)reason; return "stub"; }

void arkrtos_fault_record(uint32 type, int32 task_id);
void arkrtos_fault_record(uint32 type, int32 task_id) { (void)type; (void)task_id; }

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

/* 本文件不安装镜像，分区表只作为 loader.c 的链接依赖存在。 */
static arkrtos_partition_table_t g_pt;

arkrtos_partition_table_t *arkrtos_ptable_get(void) { return &g_pt; }
void arkrtos_ptable_clear_slots(void) {}
int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size)
{
    (void)type; (void)base; (void)size;
    return -1;
}
void arkrtos_ptable_free(uint32 slot) { (void)slot; }
int32 arkrtos_ptable_move(uint32 slot, uint32 new_base) { (void)slot; (void)new_base; return -1; }
int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    (void)addr; (void)p_base; (void)p_size;
    return -1;
}
int32 arkrtos_ptable_find_task(uint32 task_id) { (void)task_id; return -1; }
int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    (void)slot; (void)state; (void)entry; (void)task_id;
    return -1;
}
int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    (void)slot; (void)p_state; (void)p_entry; (void)p_task_id;
    return -1;
}
int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size)
{
    (void)bytes; (void)p_base; (void)p_size;
    return -1;
}
int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size) { (void)base; (void)size; return -1; }
int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size)
{
    (void)slot; (void)base; (void)size;
    return -1;
}
int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size)
{
    (void)slot; (void)p_base; (void)p_size;
    return -1;
}

uint32 arkrtos_layout_slot_count(void) { return 0u; }
uint32 arkrtos_layout_raw_allow(void)  { return 1u; }
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void) { return (const arkrtos_cfg_slot_t *)0; }
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    (void)index;
    return (const arkrtos_cfg_slot_t *)0;
}

/* ------------------------------------------------------------------ */
/* 断言                                                               */
/* ------------------------------------------------------------------ */

static uint32 g_checks = 0u;
static uint32 g_fails  = 0u;

static void ck_u(uint32 got, uint32 want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got 0x%08X want 0x%08X\n", what,
               (unsigned)got, (unsigned)want);
    }
}

static void ck_i(int32 got, int32 want, const char *what)
{
    ck_u((uint32)got, (uint32)want, what);
}

static void ck_true(uint32 cond, const char *what)
{
    g_checks++;
    if(cond == 0u)
    {
        g_fails++;
        printf("FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* 负载构造                                                           */
/* ------------------------------------------------------------------ */

/* 两个窗口都取 2 的幂；基址来自分区头，不在本文件里写死地址字面量。 */
#define ROM_SIZE   (0x1000u)
#define RAM_SIZE   (0x1000u)
#define DELTA      (0x1000u)

static uint32 g_rom_base = CHIP_FLASH_BASE + 0x4000u;
static uint32 g_ram_base = CHIP_RAM_BASE + 0x8000u;

static void put32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

static void put16(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
}

/* 与 ARM ARM 的 MOVW/MOVT 编码一致；测试侧独立实现，不复用被测代码。 */
static void put_mov(uint8 *p, uint32 rd, uint32 imm16, int is_movt)
{
    uint32 imm4 = (imm16 >> 12) & 0xFu;
    uint32 i    = (imm16 >> 11) & 1u;
    uint32 imm3 = (imm16 >> 8) & 7u;
    uint32 imm8 = imm16 & 0xFFu;
    uint32 h1   = (is_movt != 0) ? 0xF2C0u : 0xF240u;
    uint32 h2;

    h1 |= (i << 10) | imm4;
    h2  = (imm3 << 12) | (rd << 8) | imm8;

    put16(p, h1);
    put16(p + 2u, h2);
}

/* 四个负载同时写：a=标称、b=ROM+delta、c=RAM+delta、d=ROM+2*delta、RAM+delta */
typedef struct {
    uint8 *a;
    uint8 *b;
    uint8 *c;
    uint8 *d;
    uint32 len;
} quad_t;

static void q_word(quad_t *q, uint32 off, uint32 va, uint32 vb, uint32 vc, uint32 vd)
{
    put32(q->a + off, va);
    put32(q->b + off, vb);
    put32(q->c + off, vc);
    put32(q->d + off, vd);
}

static void q_rom_word(quad_t *q, uint32 off, uint32 value)
{
    q_word(q, off, value, value + DELTA, value, value + (2u * DELTA));
}

static void q_ram_word(quad_t *q, uint32 off, uint32 value)
{
    q_word(q, off, value, value, value + DELTA, value + DELTA);
}

static void q_same_word(quad_t *q, uint32 off, uint32 value)
{
    q_word(q, off, value, value, value, value);
}

static void q_same16(quad_t *q, uint32 off, uint32 value)
{
    put16(q->a + off, value);
    put16(q->b + off, value);
    put16(q->c + off, value);
    put16(q->d + off, value);
}

static void q_pair(quad_t *q, uint32 off, uint32 rd, uint32 va, uint32 vb,
                   uint32 vc, uint32 vd)
{
    put_mov(q->a + off, rd, va & 0xFFFFu, 0);
    put_mov(q->a + off + 4u, rd, (va >> 16) & 0xFFFFu, 1);

    put_mov(q->b + off, rd, vb & 0xFFFFu, 0);
    put_mov(q->b + off + 4u, rd, (vb >> 16) & 0xFFFFu, 1);

    put_mov(q->c + off, rd, vc & 0xFFFFu, 0);
    put_mov(q->c + off + 4u, rd, (vc >> 16) & 0xFFFFu, 1);

    put_mov(q->d + off, rd, vd & 0xFFFFu, 0);
    put_mov(q->d + off + 4u, rd, (vd >> 16) & 0xFFFFu, 1);
}

static void q_rom_pair(quad_t *q, uint32 off, uint32 rd, uint32 value)
{
    q_pair(q, off, rd, value, value + DELTA, value, value + (2u * DELTA));
}

static void q_ram_pair(quad_t *q, uint32 off, uint32 rd, uint32 value)
{
    q_pair(q, off, rd, value, value, value + DELTA, value + DELTA);
}

/* 清成 0（0x0000 是「两条 16 位指令」的形态，差分看起来是常量）。 */
static void q_zero(quad_t *q)
{
    memset(q->a, 0, q->len);
    memset(q->b, 0, q->len);
    memset(q->c, 0, q->len);
    memset(q->d, 0, q->len);
}

/* 标准向量表：SP 落在 RAM 窗口内，入口 rom_base + entry_off（Thumb 位为 1）。 */
static void q_vector(quad_t *q, uint32 entry_off)
{
    q_ram_word(q, 0u, g_ram_base + (RAM_SIZE / 2u));
    q_rom_word(q, 4u, g_rom_base + entry_off + 1u);
}

static void win_init(arkrtos_apppack_win_t *win)
{
    win->rom_base = g_rom_base;
    win->rom_size = ROM_SIZE;
    win->ram_base = g_ram_base;
    win->ram_size = RAM_SIZE;
}

/* ------------------------------------------------------------------ */
/* 用例 1：四类表项 + 头 + 复核 + 装载器认账                          */
/* ------------------------------------------------------------------ */

static uint8 g_a[0x2000];
static uint8 g_b[0x2000];
static uint8 g_c[0x2000];
static uint8 g_d[0x2000];
static uint8 g_out[0x4000];

static void fill_scene(quad_t *q)
{
    q_zero(q);

    q_vector(q, 0x20u);                       /* 0x00 RAM / 0x04 ROM         */
    q_rom_pair(q, 0x08u, 3u, g_rom_base + 0x48u);  /* ROM_MOVW              */
    q_ram_pair(q, 0x10u, 4u, g_ram_base + 0x100u); /* RAM_MOVW              */
    q_same16(q, 0x18u, 0xBF00u);              /* nop                          */
    q_same16(q, 0x1Au, 0xBF00u);              /* nop                          */
    q_same_word(q, 0x1Cu, 0x0000F8DFu);       /* 32 位指令（h1=0xF8DF）       */
    q_rom_word(q, 0x20u, g_rom_base + 0x60u); /* 入口落点处的 ROM 常量        */
    q_ram_word(q, 0x24u, g_ram_base + 0x200u);
    q_same_word(q, 0x28u, 0x0000E800u);       /* 32 位指令（h1=0xE800）       */
    q_pair(q, 0x2Cu, 5u, 0x12345678u,
           0x12345678u, 0x12345678u, 0x12345678u);  /* 三份一致的立即数对   */

    q->len = 0x40u;
}

static void test_build_full(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    arkrtos_apppack_stats_t st;
    const arkrtos_app_header_t *hdr;
    const uint32 *rel;
    uint32 out_len = 0u;
    uint32 i;
    int32 rc;

    memset(g_out, 0xAA, sizeof(g_out));
    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0u;
    fill_scene(&q);
    win_init(&win);

    memset(&st, 0, sizeof(st));
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 7u,
                               g_out, sizeof(g_out), &out_len, &st);
    ck_i(rc, 0, "full: build ok");

    /* 6 条表项：RAM 0x00 / ROM 0x04 / ROM_MOVW 0x08 / RAM_MOVW 0x10 /
     * ROM 0x20 / RAM 0x24（一致的立即数对与 nop 不记账）。 */
    ck_u(st.n_rom, 2u, "full: stats n_rom");
    ck_u(st.n_ram, 2u, "full: stats n_ram");
    ck_u(st.n_rom_movw, 1u, "full: stats n_rom_movw");
    ck_u(st.n_ram_movw, 1u, "full: stats n_ram_movw");
    ck_u(st.entry_offset, 0x20u, "full: stats entry_offset");
    ck_u(st.image_size, q.len, "full: stats image_size");

    ck_u(out_len, ARKRTOS_APP_HEADER_SIZE + 6u * ARKRTOS_APP_RELOC_SIZE + q.len,
         "full: out_len");

    hdr = (const arkrtos_app_header_t *)(const void *)g_out;
    ck_u(hdr->magic, ARKRTOS_APP_MAGIC, "full: magic");
    ck_u(hdr->type, ARKRTOS_APP_TYPE_APP, "full: type");
    ck_u(hdr->hw_compat_id, ARKRTOS_HW_COMPAT_ID, "full: hw_compat_id");
    ck_u(hdr->version, 7u, "full: version");
    ck_u(hdr->image_size, q.len, "full: header image_size");
    ck_u(hdr->entry_offset, 0x20u, "full: header entry_offset");
    ck_u(hdr->nominal_base, g_rom_base, "full: nominal_base");
    ck_u(hdr->flags, ARKRTOS_APP_FLAG_NONE, "full: flags");
    ck_u(hdr->state, ARKRTOS_APP_STATE_VALID, "full: state");
    ck_u(hdr->ram_size, RAM_SIZE, "full: ram_size");
    ck_u(hdr->reloc_offset, ARKRTOS_APP_RELOC_OFFSET, "full: reloc_offset");
    ck_u(hdr->reloc_count, 6u, "full: reloc_count");
    ck_u(hdr->reloc_kind, ARKRTOS_APP_RELOC_KIND_CUR, "full: reloc_kind");
    ck_u(hdr->payload_offset, ARKRTOS_APP_HEADER_SIZE + 6u * ARKRTOS_APP_RELOC_SIZE,
         "full: payload_offset");
    ck_u(hdr->nominal_ram_base, g_ram_base, "full: nominal_ram_base");
    ck_u(hdr->runtime_ram_base, 0u, "full: runtime_ram_base left 0");

    /* image_id 是负载 CRC，且必须在算头 CRC 之前填好。 */
    ck_u(hdr->image_id, arkrtos_crc32(g_a, q.len, 0u), "full: image_id over payload");

    /* 表项：偏移相对镜像、类型位正确、按升序。 */
    rel = (const uint32 *)(const void *)(g_out + ARKRTOS_APP_RELOC_OFFSET);
    {
        static const uint32 want_off[6] = { 0x00u, 0x04u, 0x08u, 0x10u, 0x20u, 0x24u };
        static const uint32 want_kind[6] = {
            ARKRTOS_APP_RELOC_KIND_RAM, ARKRTOS_APP_RELOC_KIND_ROM,
            ARKRTOS_APP_RELOC_KIND_ROM_MOVW, ARKRTOS_APP_RELOC_KIND_RAM_MOVW,
            ARKRTOS_APP_RELOC_KIND_ROM, ARKRTOS_APP_RELOC_KIND_RAM
        };
        uint32 base_off = ARKRTOS_APP_HEADER_SIZE + 6u * ARKRTOS_APP_RELOC_SIZE;

        for(i = 0u; i < 6u; i++)
        {
            ck_u(ARKRTOS_APP_RELOC_OFF(rel[i]), base_off + want_off[i],
                 "full: table offset");
            ck_u(ARKRTOS_APP_RELOC_KIND(rel[i]), want_kind[i], "full: table kind");
        }
    }

    /* 负载必须是标称形态 A（复核后还原），不是 D。 */
    ck_true(memcmp(g_out + hdr->payload_offset, g_a, q.len) == 0,
            "full: payload restored to nominal A");

    /* CRC 覆盖：头（三个字段按 0）+ 表 + 负载。 */
    {
        uint32 crc = arkrtos_loader_crc_header(hdr);
        crc = arkrtos_crc32(rel, hdr->reloc_count * ARKRTOS_APP_RELOC_SIZE, crc);
        crc = arkrtos_crc32(g_out + hdr->payload_offset, q.len, crc);
        ck_u(hdr->crc32, crc, "full: crc32 covers header+table+payload");
    }

    /* 装载器认账：真 arkrtos_loader_check_header（同一套规则，不抄副本）。 */
    ck_i(arkrtos_loader_check_header(hdr, rel, ARKRTOS_APP_TYPE_MASK_POOL),
         0, "full: loader accepts the image");

    /* stats 可空。 */
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 7u,
                               g_out, sizeof(g_out), &out_len, (arkrtos_apppack_stats_t *)0);
    ck_i(rc, 0, "full: stats may be null");
}

/* ------------------------------------------------------------------ */
/* 用例 2：平铺小程序（+0 是 PUSH 序言，入口 0）                      */
/* ------------------------------------------------------------------ */

static void test_build_mini(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    arkrtos_apppack_stats_t st;
    const arkrtos_app_header_t *hdr;
    uint32 out_len = 0u;
    int32 rc;

    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0x40u;
    q_zero(&q);
    q_same16(&q, 0x00u, 0xB5E0u);             /* PUSH {r5-r7,lr}：小程序启动桩 */
    q_same16(&q, 0x02u, 0xBF00u);
    q_ram_word(&q, 0x08u, g_ram_base + 0x40u);
    win_init(&win);

    memset(&st, 0, sizeof(st));
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_MINIAPP, 1u,
                               g_out, sizeof(g_out), &out_len, &st);
    ck_i(rc, 0, "mini: build ok");
    ck_u(st.entry_offset, 0u, "mini: entry offset is 0");
    ck_u(st.n_ram, 1u, "mini: one RAM word");
    ck_u(st.n_rom, 0u, "mini: no ROM word");

    hdr = (const arkrtos_app_header_t *)(const void *)g_out;
    ck_u(hdr->entry_offset, 0u, "mini: header entry 0");
    ck_u(hdr->type, ARKRTOS_APP_TYPE_MINIAPP, "mini: type");
    ck_i(arkrtos_loader_check_header(hdr, (const uint32 *)(const void *)(g_out + ARKRTOS_APP_RELOC_OFFSET),
                                     ARKRTOS_APP_TYPE_MASK_MINI),
         0, "mini: loader accepts mini image");
}

/* ------------------------------------------------------------------ */
/* 用例 3：入口解析的三条拒绝                                         */
/* ------------------------------------------------------------------ */

static void test_entry_refused(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    uint32 out_len = 0u;
    int32 rc;

    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0x40u;
    win_init(&win);

    /* (a) 什么都没有：+0 不是窗口内的栈、也不是 PUSH。 */
    q_zero(&q);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_ENTRY, "entry: garbage refused");

    /* (b) 入口落在负载起始（偏移 0）：必须 >= 8，否则会把向量表当代码。 */
    q_zero(&q);
    q_ram_word(&q, 0u, g_ram_base + 0x100u);
    q_rom_word(&q, 4u, g_rom_base + 1u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_ENTRY, "entry: offset 0 refused");

    /* (c) 入口越出负载末尾。 */
    q_zero(&q);
    q_ram_word(&q, 0u, g_ram_base + 0x100u);
    q_rom_word(&q, 4u, g_rom_base + q.len + 1u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_ENTRY, "entry: past payload refused");

    /* (d) 栈指针不在 RAM 窗口内，且 +0 不是 PUSH。 */
    q_zero(&q);
    q_ram_word(&q, 0u, g_ram_base + RAM_SIZE + 4u);
    q_rom_word(&q, 4u, g_rom_base + 0x10u + 1u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_ENTRY, "entry: SP outside RAM window refused");
}

/* ------------------------------------------------------------------ */
/* 用例 4：参数闸门                                                   */
/* ------------------------------------------------------------------ */

static void test_params(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    uint32 out_len = 0u;
    int32 rc;

    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0x40u;
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    win_init(&win);

    rc = arkrtos_apppack_build(0, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: a null");

    rc = arkrtos_apppack_build(g_a, 0, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: b null");

    rc = arkrtos_apppack_build(g_a, g_b, 0, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: c null");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, 0, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: d null");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, 0,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: win null");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, 0, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: out null");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), 0, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: out_len null");

    /* 门前的驻入报错不写出参：
     * 只有参数齐齐过了那一道才会先清零。 */
    out_len = 123u;
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, 0, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: null out still reports");
    ck_u(out_len, 123u, "param: out_len untouched before the gate is passed");

    /* 长度：0 与不是 4 的倍数。 */
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, 0u, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: len 0");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, 0x3Eu, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: len not multiple of 4");

    /* 窗口：非 2 的幂、小于 8。 */
    win.rom_size = 0x1800u;
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: rom_size not pow2");
    win_init(&win);

    win.ram_size = 0x3000u;
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: ram_size not pow2");
    win_init(&win);

    win.rom_size = 4u;
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: rom_size < 8");
    win_init(&win);

    win.ram_size = 0u;
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: ram_size 0 (not pow2 either)");
    win_init(&win);

    /* delta：0、小于 ROM 窗口、小于 RAM 窗口。 */
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, 0u, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: delta 0");

    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, ROM_SIZE - 4u, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: delta below rom window");

    win.ram_size = ROM_SIZE * 4u;   /* 让 delta 只小于 RAM 窗口 */
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u, g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_PARAM, "param: delta below ram window");
    win_init(&win);

    /* 负载比 ROM 窗口还大。 */
    {
        uint8 big[0x200];
        memset(big, 0, sizeof(big));
        win.rom_size = 0x80u;
        win.ram_size = 0x80u;
        rc = arkrtos_apppack_build(big, big, big, big, sizeof(big), 0x80u, &win,
                                   ARKRTOS_APP_TYPE_APP, 1u,
                                   g_out, sizeof(g_out), &out_len, 0);
        ck_i(rc, ARKRTOS_APPPACK_ERR_SIZE, "param: payload bigger than ROM window");
        win_init(&win);
    }
}

/* ------------------------------------------------------------------ */
/* 用例 5：三个「宁可拒绝也不猜」的出口                               */
/* ------------------------------------------------------------------ */

static void test_scan_refusals(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    uint32 out_len = 0u;
    int32 rc;

    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0x40u;
    win_init(&win);

    /* (a) 一个位置「差了一个非 delta 的量」且又不是 MOVW 对 ⇒ DIFF。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_word(&q, 0x08u, 0x00010000u, 0x00011004u, 0x00010000u, 0x00010000u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF, "diff: unexplained delta refused");

    /* (b) 立即数对里 B 与 C 都变了 ⇒ 一个增量解释不了两处 ⇒ DIFF。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    q_pair(&q, 0x08u, 2u, g_rom_base + 0x48u,
           g_rom_base + 0x48u + DELTA,
           g_rom_base + 0x48u + (2u * DELTA),
           g_rom_base + 0x48u + (2u * DELTA));
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF, "diff: both base deltas in one pair refused");

    /* (c) MOVW 与 MOVT 的 Rd 不同 ⇒ 不是一对，且 B 变了 ⇒ DIFF。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    put_mov(g_a + 0x08u, 0u, (g_rom_base + 0x48u) & 0xFFFFu, 0);
    put_mov(g_a + 0x0Cu, 1u, ((g_rom_base + 0x48u) >> 16) & 0xFFFFu, 1);
    put_mov(g_b + 0x08u, 0u, (g_rom_base + 0x48u + DELTA) & 0xFFFFu, 0);
    put_mov(g_b + 0x0Cu, 1u, ((g_rom_base + 0x48u) >> 16) & 0xFFFFu, 1);
    memcpy(g_c, g_a, q.len);
    memcpy(g_d, g_a, q.len);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF, "diff: MOVW/MOVT with different Rd refused");

    /* (d) 未对齐位置上的「恰好差 delta」不是地址槽 ⇒ DIFF（不是误收）。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    q_same16(&q, 0x08u, 0xBF00u);  /* 先把扫描器带到 2 mod 4 */
    put16(g_a + 0x0Au, 0x0000u);   /* 0x0A 起的一个字：低半单词变 */
    put16(g_a + 0x0Cu, 0x0001u);
    put16(g_b + 0x0Au, 0x1000u);
    put16(g_b + 0x0Cu, 0x0001u);
    memcpy(g_c, g_a, q.len);
    memcpy(g_d, g_a, q.len);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF, "diff: unaligned delta is not a slot");

    /* (e) 表应用回 A 后与 D 不同 ⇒ VERIFY（D 必须是同一个 delta 链出来的）。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    memcpy(g_d, g_a, q.len);   /* D 没被平移 */
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_VERIFY, "verify: D mismatch refused");

    /* (f) 输出缓冲差一个字节 ⇒ SIZE（不截断）。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    q_ram_word(&q, 0x20u, g_ram_base + 0x200u);
    {
        uint32 need = ARKRTOS_APP_HEADER_SIZE + 3u * ARKRTOS_APP_RELOC_SIZE + q.len;
        rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                                   ARKRTOS_APP_TYPE_APP, 1u,
                                   g_out, need - 1u, &out_len, 0);
        ck_i(rc, ARKRTOS_APPPACK_ERR_SIZE, "size: one byte short refused");

        rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                                   ARKRTOS_APP_TYPE_APP, 1u,
                                   g_out, need, &out_len, 0);
        ck_i(rc, 0, "size: exactly enough accepted");
        ck_u(out_len, need, "size: exact length reported");
    }

    /* (g) 一条绝对地址都没有 ⇒ SIZE（判据失效/不该打包，不产出空表镜像）。 */
    q_zero(&q);
    put16(g_a, 0xB5E0u); put16(g_b, 0xB5E0u); put16(g_c, 0xB5E0u); put16(g_d, 0xB5E0u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_MINIAPP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_SIZE, "size: zero entries refused");

    /* (h) 表项超过 ARKRTOS_APP_RELOC_MAX ⇒ SIZE。 */
    {
        static uint8 ba[0x2000];
        static uint8 bb[0x2000];
        static uint8 bc[0x2000];
        static uint8 bd[0x2000];
        arkrtos_apppack_win_t w2;
        uint32 i;

        memset(ba, 0, sizeof(ba));
        memset(bb, 0, sizeof(bb));
        memset(bc, 0, sizeof(bc));
        memset(bd, 0, sizeof(bd));

        put32(ba, g_ram_base + 0x100u); put32(bb, g_ram_base + 0x100u);
        put32(bc, g_ram_base + 0x100u + 0x2000u); put32(bd, g_ram_base + 0x100u + 0x2000u);
        put32(ba + 4u, g_rom_base + 0x20u + 1u); put32(bb + 4u, g_rom_base + 0x20u + 1u + 0x2000u);
        put32(bc + 4u, g_rom_base + 0x20u + 1u); put32(bd + 4u, g_rom_base + 0x20u + 1u + 0x4000u);

        /* 每个 4 字节字都是 RAM 类：2048 条 > 1024。 */
        for(i = 8u; i < sizeof(ba); i += 4u)
        {
            put32(ba + i, g_ram_base + 0x400u + i);
            put32(bb + i, g_ram_base + 0x400u + i);
            put32(bc + i, g_ram_base + 0x400u + i + 0x2000u);
            put32(bd + i, g_ram_base + 0x400u + i + 0x2000u);
        }

        w2.rom_base = g_rom_base;
        w2.rom_size = 0x2000u;
        w2.ram_base = g_ram_base;
        w2.ram_size = 0x2000u;

        rc = arkrtos_apppack_build(ba, bb, bc, bd, sizeof(ba), 0x2000u, &w2,
                                   ARKRTOS_APP_TYPE_APP, 1u,
                                   g_out, sizeof(g_out), &out_len, 0);
        ck_i(rc, ARKRTOS_APPPACK_ERR_SIZE, "size: more than RELOC_MAX entries refused");
    }
}

/* ------------------------------------------------------------------ */
/* 用例 6：扫描器的两条『深水区』拒绝                                */
/* ------------------------------------------------------------------ */

/* 这两条不在四类表项的正常路径上，但它们是扫描器自己的判断边界，钉住的
 * 是「遇到不认识的形态就不往下猜」这条产业要求：
 *   (i)  位置上是 32 位 Thumb（0xF0xx 这一组）却不是 MOVW/MOVT：读不出
 *        立即数对，且整字差又不是一个 delta ⇒ DIFF；
 *   (ii) 位置离负载尾部不足 8 字节（放不下一条 MOVW+MOVT）：读立即数对
 *        被长度闸门挡住 ⇒ DIFF。
 * 两条都必须是在「整字差分、指令一致性」两步都已经否掉之后才落下来。 */
static void test_scan_deep(void)
{
    quad_t q;
    arkrtos_apppack_win_t win;
    uint32 out_len = 0u;
    int32 rc;

    q.a = g_a; q.b = g_b; q.c = g_c; q.d = g_d; q.len = 0x40u;
    win_init(&win);

    /* (i) 0xF040 是 32 位 Thumb 前缀（0xF000 组），(h>>4)&0x3F = 0x04，
     * 既不是 MOVW(0x24) 也不是 MOVT(0x2C)：读立即数对必须回「不是」。
     * 同时 B 在这个半字上差 0x10（不是 delta），整字差分也否掉。 */
    q_zero(&q);
    q_vector(&q, 0x10u);
    put16(g_a + 0x08u, 0xF040u);
    put16(g_b + 0x08u, 0xF050u);
    put16(g_c + 0x08u, 0xF040u);
    put16(g_d + 0x08u, 0xF040u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF,
         "diff: 32-bit non-MOVW/MOVT halfword refused");

    /* (ii) 差异落在最后 4 个字节：pos + 8 > len，立即数对读到一半就得停。
     * 前面都是三份一致的 0，扫描器按 16 位一步一步走到 0x3C。 */
    q_zero(&q);
    q_vector(&q, 0x08u);
    q_word(&q, 0x3Cu, 0x00010000u, 0x00011004u, 0x00010000u, 0x00010000u);
    rc = arkrtos_apppack_build(g_a, g_b, g_c, g_d, q.len, DELTA, &win,
                               ARKRTOS_APP_TYPE_APP, 1u,
                               g_out, sizeof(g_out), &out_len, 0);
    ck_i(rc, ARKRTOS_APPPACK_ERR_DIFF,
         "diff: truncated MOVW/MOVT pair at tail refused");
}

/* ------------------------------------------------------------------ */
/* 用例 7：错误码名字（未知码如实回 unknown）                         */
/* ------------------------------------------------------------------ */

static void test_error_names(void)
{
    ck_true(strcmp(arkrtos_apppack_error_name(0), "ok") == 0, "name: ok");
    ck_true(strcmp(arkrtos_apppack_error_name(ARKRTOS_APPPACK_ERR_PARAM),
                   "invalid arguments (pointer / window / length)") == 0,
            "name: PARAM");
    ck_true(strcmp(arkrtos_apppack_error_name(ARKRTOS_APPPACK_ERR_DIFF),
                   "a payload offset cannot be explained by one base delta (not relocatable)") == 0,
            "name: DIFF");
    ck_true(strcmp(arkrtos_apppack_error_name(ARKRTOS_APPPACK_ERR_VERIFY),
                   "relocating A with the built table does not reproduce D") == 0,
            "name: VERIFY");
    ck_true(strcmp(arkrtos_apppack_error_name(ARKRTOS_APPPACK_ERR_SIZE),
                   "relocation table or output buffer too small") == 0,
            "name: SIZE");
    ck_true(strcmp(arkrtos_apppack_error_name(ARKRTOS_APPPACK_ERR_ENTRY),
                   "no usable entry: +0/+4 is not a vector table and "
                   "+0 is not a mini start-stub prologue (0xB5E0)") == 0,
            "name: ENTRY");
    ck_true(strcmp(arkrtos_apppack_error_name(-99), "unknown") == 0,
            "name: unknown code stays unknown");
}

int main(void)
{
    test_build_full();
    test_build_mini();
    test_entry_refused();
    test_params();
    test_scan_refusals();
    test_scan_deep();
    test_error_names();

    if(g_fails != 0u)
    {
        printf("APPPACK-HOST FAIL  %u/%u checks failed\n",
               (unsigned)g_fails, (unsigned)g_checks);
        return 1;
    }

    printf("APPPACK-HOST OK  %u checks\n", (unsigned)g_checks);
    return 0;
}
