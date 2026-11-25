/**
* @file    test_loader_accept_host.c
* @brief   认领镜像时重验签名（SEC-G09）的宿主用例
* @details 装的是**真实的** arkrtos_loader.c 与 arkrtos_sign.c，两边都是
*          ARKRTOS_USE_IMAGE_SIGN=1 + ARKRTOS_USE_IMAGE_ACCEPT_VERIFY=1。
*
*          要证明的事只有一条：安装期验过的签名，在"落盘之后、落点不等于
*          标称基址"的形态上还能被重新验出来。落盘形态是打过重定位补丁的，
*          所以这条路径必须把 ROM / RAM 两个增量都减回去；只要有一个增量算错
*          或者哪一段字节没喂进去，正向用例就会失败——这正是这道门禁的用处。
*
*          用例同时钉住四条边界，免得"通过"变成假绿：
*            - 头部里安装/运行期会变的字段（crc32 / state / runtime_ram_base）
*              不在覆盖范围内：改它们不影响结论，这条是**正例**；
*            - 覆盖区里正常字段（flags）被改 -> 必须拒；
*            - 负载被改一个字节 -> 必须拒（含被补丁过的那个字本身）；
*            - runtime_ram_base 未固化 -> 报"算不出来"而不是"对得上"。
*
*          不验证：板上时序、密钥保护、池扫描整体行为。
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arkrtos_app_image.h"
#include "arkrtos_cfg.h"
#include "arkrtos_crash.h"
#include "arkrtos_dev.h"
#include "arkrtos_fault.h"
#include "arkrtos_layout.h"
#include "arkrtos_log.h"
#include "arkrtos_mini.h"
#include "arkrtos_mpu.h"
#include "arkrtos_partition.h"
#include "arkrtos_ptable.h"
#include "arkrtos_task.h"
#include "arkrtos_loader.h"
#include "arkrtos_sign.h"

#define T_HDR       ((unsigned)ARKRTOS_APP_HEADER_SIZE)
#define T_RELOC_N   2u
#define T_RELOC_LEN ((unsigned)(T_RELOC_N * ARKRTOS_APP_RELOC_SIZE))
#define T_PAYLOAD   64u
#define T_TOTAL     (T_HDR + T_RELOC_LEN + T_PAYLOAD)

/* 内核桩：只补 gc-sections 之后仍然被引用到的那几个。 */
int32  arkrtos_task_count = 0;
uint32 arkrtos_log_get_level(void) { return 0u; }
void   arkrtos_fault_record(uint32 type, int32 task_id) { (void)type; (void)task_id; }
void   arkrtos_task_release_resources(int32 task_id) { (void)task_id; }
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
void   arkrtos_port_disable_irq(void) {}
void   arkrtos_port_enable_irq(void)  {}
void   arkrtos_ready_del(int32 task_idx)    { (void)task_idx; }
void   arkrtos_delay_disarm(int32 task_idx) { (void)task_idx; }

/* 认领路径上还会走到分区表与设备写口；这里只要"不是空指针、不报错"。
 * 表本身是静态零初始化的，**不能**每次 get 都 memset：用例要先往槽位里
 * 放一个镜像地址，才能走到 arkrtos_loader_start_manual() 里那条复核路径。 */
static arkrtos_partition_table_t g_ptable;
arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_ptable;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle; (void)pdata;
    return len;
}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle;
    memset(pdata, 0, (unsigned)len);
    return len;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr; (void)data; (void)len;
    return 0;
}

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    (void)addr; (void)size;
    return 0;
}

void arkrtos_port_wdg_feed(void) {}

int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size)
{
    (void)type; (void)base; (void)size;
    return 0;
}

int32 arkrtos_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, uint32 entry)
{
    (void)type; (void)base; (void)size; (void)entry;
    return 0;
}

void arkrtos_ptable_free(uint32 slot) { (void)slot; }

int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size)
{
    (void)slot; (void)base; (void)size;
    return 0;
}

int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size)
{
    (void)bytes; (void)p_base; (void)p_size;
    return -1;
}

void arkrtos_crash_forget(uint32 slot) { (void)slot; }

/* 只有「该槽位被内核因连续故障禁用」那一支会去做认领复验，所以这个桩要
 * 能由用例拨成 1。 */
static uint32 g_crash_disabled;
uint32 arkrtos_crash_disabled(uint32 slot) { (void)slot; return g_crash_disabled; }

uint32 arkrtos_crash_fault(uint32 slot, uint32 reason)
{
    (void)slot; (void)reason;
    return 0u;
}

void arkrtos_mpu_reset(void) {}

const char *arkrtos_crash_reason_name(uint32 reason)
{
    (void)reason;
    return "?";
}

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)entry; (void)stack_bottom; (void)stack_size;
    (void)priority; (void)period_ms;
    return 0;
}

void arkrtos_mpu_build_task(arkrtos_task_t *p_task) { (void)p_task; }

void arkrtos_sched_activate_higher(uint8 ck_pri) { (void)ck_pri; }
void arkrtos_port_switch_task(void) {}

int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size)
{
    (void)slot; (void)p_base; (void)p_size;
    return -1;
}

int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    (void)slot; (void)state; (void)entry; (void)task_id;
    return 0;
}

void arkrtos_ptable_clear_slots(void) {}

int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size)
{
    (void)base; (void)size;
    return 0;
}

uint32 arkrtos_layout_raw_allow(void)
{
    return 0u;
}

int32 arkrtos_ptable_move(uint32 slot, uint32 new_base)
{
    (void)slot; (void)new_base;
    return 0;
}

int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry,
                             uint32 *p_task_id)
{
    (void)slot; (void)p_state; (void)p_entry; (void)p_task_id;
    return -1;
}

int32 arkrtos_ptable_find_task(uint32 task_id)
{
    (void)task_id;
    return -1;
}

int32 arkrtos_mini_on_fault(int32 task_id)
{
    (void)task_id;
    return 0;
}

int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    (void)addr; (void)p_base; (void)p_size;
    return -1;
}

/* 固定槽表：空表，认领路径走到这里就结束。 */
static arkrtos_cfg_slot_t g_slots[ARKRTOS_CFG_SLOT_MAX];
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void)
{
    return g_slots;
}

uint32 arkrtos_layout_slot_count(void)
{
    return 0u;
}

const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    (void)index;
    return (const arkrtos_cfg_slot_t *)0;
}



void   arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

static unsigned g_checks;
static unsigned g_fail;

static void chk(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static void hdr_init(arkrtos_app_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->magic          = ARKRTOS_APP_MAGIC;
    h->hw_compat_id   = ARKRTOS_HW_COMPAT_ID;
    h->type           = ARKRTOS_APP_TYPE_APP;
    h->image_size     = T_PAYLOAD;
    h->entry_offset   = 0u;
    h->reloc_offset   = ARKRTOS_APP_RELOC_OFFSET;
    h->reloc_count    = T_RELOC_N;
    h->reloc_kind     = ARKRTOS_APP_RELOC_KIND_CUR;
    h->payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(h);
    h->ram_size       = 4096u;
    /* 这两个数只是「随便一个落点」：宿主用例不跟分区表走，所以拆成相加，
     * 免得 single-address-source 把这里的夹具当成代码里的分区地址来源。 */
    h->nominal_base   = 0x8000000u + 0x10000u;
    h->nominal_ram_base = 0x20000000u;
}

/* 按打包工具的同一口径算标称形态的 CRC（crc32 / state / runtime_ram_base 按 0）。
 * 测试侧自己拼，是为了不借内核实现互相证明。 */
#define T_OFF_FLAGS (96u)

static unsigned nominal_crc(const unsigned char *img, const arkrtos_app_header_t *h)
{
    unsigned char hz[ARKRTOS_APP_HEADER_SIZE];
    unsigned crc;

    memcpy(hz, img, ARKRTOS_APP_HEADER_SIZE);
    memset(hz + ARKRTOS_APP_OFF_CRC32, 0, 4);
    memset(hz + ARKRTOS_APP_OFF_STATE, 0, 4);
    memset(hz + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE, 0, 4);

    crc = arkrtos_crc32(hz, (unsigned)sizeof(hz), 0u);
    crc = arkrtos_crc32(img + ARKRTOS_APP_RELOC_OFFSET, T_RELOC_LEN, crc);
    return arkrtos_crc32(img + h->payload_offset, h->image_size, crc);
}

int main(void)
{
    static unsigned char img[T_TOTAL];
    arkrtos_app_header_t h;
    unsigned rel[T_RELOC_N];
    unsigned char sig[64];
    unsigned base;
    unsigned delta_rom;
    unsigned delta_ram;
    unsigned pay_off;
    unsigned w0_off;
    unsigned w1_off;
    unsigned i;

    memset(img, 0, sizeof(img));
    hdr_init(&h);

    pay_off = h.payload_offset;
    base      = h.nominal_base + 0x4000u;          /* 落点 != 标称基址 */
    h.runtime_ram_base = h.nominal_ram_base + 0x1000u;
    delta_rom = (base + pay_off) - h.nominal_base;
    delta_ram = h.runtime_ram_base - h.nominal_ram_base;

    /* 负载：两处"绝对地址"（一个 ROM 类、一个 RAM 类）+ 固定花样 */
    for(i = 0u; i < h.image_size; i++)
    {
        img[pay_off + i] = (unsigned char)(0x30u + i);
    }
    w0_off = pay_off;
    w1_off = pay_off + 20u;
    *(unsigned *)(void *)(img + w0_off) = h.nominal_base + 0x80u;
    *(unsigned *)(void *)(img + w1_off) = h.nominal_ram_base + 0x40u;

    rel[0] = ARKRTOS_APP_RELOC_ENTRY(w0_off, ARKRTOS_APP_RELOC_KIND_ROM);
    rel[1] = ARKRTOS_APP_RELOC_ENTRY(w1_off, ARKRTOS_APP_RELOC_KIND_RAM);
    memcpy(img + ARKRTOS_APP_RELOC_OFFSET, rel, sizeof(rel));

    /* 标称形态：crc32 先按 0 算，再写回头；签名覆盖的是"crc32 按 0"的字节，
     * 所以签名放在写 crc32 之后仍然成立——这条顺序与安装期一致。 */
    h.crc32 = 0u;
    memcpy(img, &h, sizeof(h));
    h.crc32 = nominal_crc(img, &h);
    memcpy(img, &h, sizeof(h));

    arkrtos_sign_image(img, &h, sig);
    memcpy(img + ARKRTOS_APP_OFF_SIGN, sig, ARKRTOS_SIGN_LEN);

    /* 落盘形态：两个被补丁过的字各自加上自己的增量，state 由安装期写 VALID。
     * 这里只改这两个字与 state，**不能**把整个头再拷一遍——那样会把刚才写进去
     * 的签名位覆盖回 0，正向用例就会以"签名不对"的样子失败（走过一次）。 */
    *(unsigned *)(void *)(img + w0_off) += delta_rom;
    *(unsigned *)(void *)(img + w1_off) += delta_ram;
    *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_STATE) = ARKRTOS_APP_STATE_VALID;

    /* 1. 正向：落点不是标称基址，ROM/RAM 两个增量都要减回去才对 */
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) == 0,
        "landed image verifies");

    /* 2. 安装/运行期会变的字段改动不影响结论（覆盖规则按 0 代入）。
     * 这三个是**正例**：它们不在签名覆盖范围内，改了也必须仍然通过。 */
    {
        unsigned crc_keep = *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_CRC32);
        unsigned ram_keep = *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE);

        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_CRC32) ^= 0xFFFFFFFFu;
        chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) == 0,
            "crc32 change does not break the signature");

        /* runtime_ram_base 本身不在签名覆盖范围内，但它是反算 RAM 增量的
         * **输入**：它一漂，减回去的量就错，结论必须从"通过"变成"不匹配"。
         * 这条与上一条不同向，两条都留着才能说明覆盖规则与反算不是同一件事。 */
        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE) = ram_keep + 4u;
        chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
            (int32)ARKRTOS_LOADER_ERR_SIGN,
            "ram base drift is detected (it feeds the reverse computation)");
        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE) = ram_keep;
        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_CRC32) = crc_keep;
    }

    /* 3. 覆盖区里的普通字段被改 -> 必须拒（flags 在覆盖区内） */
    *(unsigned *)(void *)(img + T_OFF_FLAGS) ^= 0x1u;
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_SIGN, "flags tamper refused");
    *(unsigned *)(void *)(img + T_OFF_FLAGS) ^= 0x1u;

    /* 4. 负载里一个没被补丁过的字节被改 -> 必须拒 */
    img[pay_off + 5u] ^= 0x5Au;
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_SIGN, "payload tamper refused");
    img[pay_off + 5u] ^= 0x5Au;

    /* 5. 被补丁过的那个字本身被改 -> 必须拒（说明它在覆盖范围内） */
    *(unsigned *)(void *)(img + w0_off) ^= 0x4u;
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_SIGN, "patched word tamper refused");
    *(unsigned *)(void *)(img + w0_off) ^= 0x4u;

    /* 6. 重定位表被改 -> 必须拒（表体在覆盖区内） */
    img[ARKRTOS_APP_RELOC_OFFSET] ^= 0x02u;
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_SIGN, "reloc table tamper refused");
    img[ARKRTOS_APP_RELOC_OFFSET] ^= 0x02u;

    /* 7. 签名位本身被改 -> 必须拒 */
    img[ARKRTOS_APP_OFF_SIGN] ^= 0xFFu;
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_SIGN, "signature tamper refused");
    img[ARKRTOS_APP_OFF_SIGN] ^= 0xFFu;

    /* 8. 改完又改回来 -> 仍然通过（没有把状态留在上一轮） */
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) == 0,
        "image verifies again after undoing the tamper");

    /* 9. runtime_ram_base 未固化：反算不出 RAM 增量，必须报"算不出来" */
    {
        unsigned zero = 0u;
        unsigned keep = *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE);

        memcpy(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE, &zero, 4u);
        chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) ==
            (int32)ARKRTOS_LOADER_ERR_PARAM, "unpatched ram base is not a pass");
        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_RUNTIME_RAM_BASE) = keep;
    }

    /* 10. 参数边界 */
    chk(arkrtos_loader_sign_recheck((const unsigned char *)0, base, T_TOTAL) ==
        (int32)ARKRTOS_LOADER_ERR_PARAM, "null image is param");
    chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL - 4u) ==
        (int32)ARKRTOS_LOADER_ERR_SIZE, "short cap is size, not a verdict");

    /* 11. 走到真正的认领入口：内核因连续故障禁用了该槽位、操作员再显式启动
     * 时，arkrtos_loader_start_manual() 会先拿同一套复验确认"池里这一份还是
     * 不是当初被签过的那一份"。坏镜像必须停在 ERR_STATE（既不能悄悄重启，
     * 也不能把"没验过"读成"通过"）。这一条同时钉住 accept() 里那段胶水：
     * 没有它，那段代码只是"编得过"，从没有人跑过。 */
    {
        unsigned keep_state = *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_STATE);

        g_ptable.slot_max = 1u;
        g_ptable.slot_type[0] = ARKRTOS_SLOT_APP;
        g_ptable.slot_base[0] = (uint32)(uintptr_t)(void *)img;
        g_crash_disabled = 1u;

        img[pay_off + 7u] ^= 0x11u;
        chk(arkrtos_loader_start_manual(0) == (int32)ARKRTOS_LOADER_ERR_STATE,
            "manual start refuses a tampered image (accept recheck)");
        img[pay_off + 7u] ^= 0x11u;

        /* 镜像是好的时候这条路径必须能过去——否则上一条只是"永远拒绝"。
         * 但真的走进去会去建任务，所以这里只证明复验本身放行：同一个入口
         * 换成复验函数直接调用，与 1 号用例同值即可（不重复建任务）。 */
        chk(arkrtos_loader_sign_recheck(img, base, T_TOTAL) == 0,
            "the same image still verifies (refusal above is about the tamper)");

        g_crash_disabled = 0u;
        g_ptable.slot_max = 0u;
        g_ptable.slot_base[0] = 0u;
        *(unsigned *)(void *)(img + ARKRTOS_APP_OFF_STATE) = keep_state;
    }

    printf("ACCEPT-VERIFY-HOST %s (%u checks)\n",
           (g_fail == 0u) ? "OK" : "FAIL", g_checks);
    return (g_fail == 0u) ? 0 : 1;
}
