/**
* @file arkrtos_sec.c
* @brief 安全基线自检：只读地把"该在的机制在不在"问一遍。见 arkrtos_sec.h。
*/

#include "arkrtos_sec.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"
#include "arkrtos_ptable.h"
#include "arkrtos_loader.h"
#include "arkrtos_app_image.h"
#include "arkrtos_audit.h"
#include "arkrtos_crash.h"
#include "arkrtos_hwsafe.h"
#include "arkrtos_rt.h"
#include "arkrtos_hal.h"

#if (ARKRTOS_USE_SECCHECK == 1)

/* 单个槽位镜像头自检：能读到的字段先看魔数，再交给 loader 的正式判定，
 * 这样"头都读不出来"和"头读出来了但内容不合法"报的是两件事。 */
#define SEC_HDR_ERR_MAGIC   (0x100)

/* 内核不链接 libc 的 memcpy：这里自己按字节拷，语义就是逐个赋值。 */
static void sec_copy(void *p_dst, const void *p_src, uint32 len)
{
    uint8 *d = (uint8 *)p_dst;
    const uint8 *s = (const uint8 *)p_src;
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        d[i] = s[i];
    }
}

static void sec_add(arkrtos_sec_report_t *p_rep, const char *name, uint32 state,
                    const char *fmt, uint32 v0, uint32 v1, uint32 v2, uint32 v3)
{
    arkrtos_sec_item_t *p_it;

    if(p_rep->count >= (uint32)ARKRTOS_SEC_ITEM_MAX)
    {
        return;
    }

    p_it = &p_rep->item[p_rep->count];
    p_it->name  = name;
    p_it->state = state;
    p_it->fmt   = fmt;
    p_it->v[0]  = v0;
    p_it->v[1]  = v1;
    p_it->v[2]  = v2;
    p_it->v[3]  = v3;
    p_rep->count++;

    if(state == (uint32)ARKRTOS_SEC_PASS)
    {
        p_rep->pass++;
    }
    else if(state == (uint32)ARKRTOS_SEC_FAIL)
    {
        p_rep->fail++;
    }
    else if(state == (uint32)ARKRTOS_SEC_FINDING)
    {
        p_rep->finding++;
    }
    else
    {
        p_rep->skip++;
    }
}

/* 1. 编译期基线：这几项是"保护"这件事本身，缺一项就不是参数调优的问题 */
static void sec_check_gates(arkrtos_sec_report_t *p_rep)
{
    uint32 m = 0u;
    uint32 need;

#if (ARKRTOS_USE_MPU == 1)
    m |= 1u;
#endif
#if (ARKRTOS_USE_PRIV == 1)
    m |= 2u;
#endif
#if (ARKRTOS_USE_STACK_CHECK == 1)
    m |= 4u;
#endif
#if (ARKRTOS_USE_FAULT_RECOVER == 1)
    m |= 8u;
#endif
#if (ARKRTOS_USE_CRASH_LOG == 1)
    m |= 16u;
#endif
#if (ARKRTOS_USE_AUDIT == 1)
    m |= 32u;
#endif
#if (ARKRTOS_USE_IMAGE_SIGN == 1)
    m |= 64u;
#endif
#if (ARKRTOS_HW_SAFE == 1)
    m |= 128u;
#endif

    /* 前六位必须全在；签名与遗言允许缺，但它们各自有独立的 FINDING 项，
     * 这里只把值报出来，不重复计分。 */
    need = 63u;

    sec_add(p_rep, "gates.kernel",
            ((m & need) == need) ? (uint32)ARKRTOS_SEC_PASS : (uint32)ARKRTOS_SEC_FAIL,
            "mpu/priv/stk/fault/crash/audit bits=0x%X (need 0x%X), sign/hwsafe=0x%X",
            m, need, (m >> 6) & 3u, 0u);
}

/* 2. 每个非空槽位都必须有 RAM 窗口：没有窗口的 App 任务跑起来就没有可写的
 *    私有内存，MPU 区域也就无从建立——那是"隔离"这句话的空转。 */
static void sec_check_windows(arkrtos_sec_report_t *p_rep)
{
    const arkrtos_partition_table_t *p_pt = arkrtos_ptable_get();
    uint32 i;
    uint32 total = 0u;
    uint32 missing = 0u;
    uint32 first = 0u;

    for(i = 0u; (i < p_pt->slot_max) && (i < (uint32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(p_pt->slot_type[i] == (uint32)ARKRTOS_SLOT_FREE)
        {
            continue;
        }

        total++;

        if((p_pt->slot_ram_base[i] == 0u) || (p_pt->slot_ram_size[i] == 0u))
        {
            if(missing == 0u)
            {
                first = i;
            }
            missing++;
        }
    }

    sec_add(p_rep, "mpu.slot_windows",
            (missing == 0u) ? (uint32)ARKRTOS_SEC_PASS : (uint32)ARKRTOS_SEC_FAIL,
            "slots=%u without_window=%u first=%u",
            total, missing, first, 0u);
}

/* 3. 结构不变式：分区表 / 槽位 / 任务表 / 调度链是否还自洽。
 *    这一项不修任何东西，只报"第一个自相矛盾的地方在哪"。 */
static void sec_check_audit(arkrtos_sec_report_t *p_rep)
{
    arkrtos_audit_result_t res;

    res.mask   = 0u;
    res.index  = ARKRTOS_AUDIT_INDEX_NONE;
    res.checks = 0u;

    arkrtos_audit_run(&res);

    sec_add(p_rep, "structures.audit",
            (res.mask == 0u) ? (uint32)ARKRTOS_SEC_PASS : (uint32)ARKRTOS_SEC_FAIL,
            "checks=%u mask=0x%X first_index=%u",
            (unsigned)res.checks, (unsigned)res.mask,
            (res.mask == 0u) ? 0u : (unsigned)res.index, 0u);
}

/* 4. 池里每个镜像的头部是否仍然成立：魔数、兼容号、类型、尺寸、入口、
 *    重定位表编码与对齐。装进来的镜像在开机时被信一次，`sec` 是在那之后
 *    唯一会把它重新判一遍的地方（CRC 不在这一项，见下面的 SKIP）。 */
static void sec_check_headers(arkrtos_sec_report_t *p_rep)
{
    const arkrtos_partition_table_t *p_pt = arkrtos_ptable_get();
    uint32 i;
    uint32 total = 0u;
    uint32 bad = 0u;
    uint32 first = 0u;
    uint32 first_err = 0u;

    for(i = 0u; (i < p_pt->slot_max) && (i < (uint32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        const uint8 *p_img;
        arkrtos_app_header_t hdr;
        int32 chk;

        if((p_pt->slot_type[i] == (uint32)ARKRTOS_SLOT_FREE) || (p_pt->slot_base[i] == 0u))
        {
            continue;
        }

        total++;
        p_img = (const uint8 *)p_pt->slot_base[i];

        /* Flash 是内存映射的，但记录可能不是 4 字节对齐地指向头；
         * 先按字节拷一份，避免对不齐指针解引用（M0/M3 上会直接 HardFault）。 */
        sec_copy(&hdr, p_img, (uint32)sizeof(hdr));

        if(hdr.magic != (uint32)ARKRTOS_APP_MAGIC)
        {
            chk = (int32)SEC_HDR_ERR_MAGIC;
        }
        else
        {
            chk = arkrtos_loader_check_header(&hdr,
                        (const uint32 *)(p_img + ARKRTOS_APP_RELOC_OFFSET),
                        (uint32)ARKRTOS_APP_TYPE_MASK_POOL);
        }

        if(chk != 0)
        {
            if(bad == 0u)
            {
                first = i;
                first_err = (uint32)chk;
            }
            bad++;
        }
    }

    sec_add(p_rep, "images.headers",
            (bad == 0u) ? (uint32)ARKRTOS_SEC_PASS : (uint32)ARKRTOS_SEC_FAIL,
            "slots=%u bad=%u first=%u err=%d",
            total, bad, first, first_err);
}

/* 5. 开机复核镜像 CRC：做不到，且原因必须写清楚，不能报个 PASS 蒙过去。
 *    Flash 上躺的是**打过重定位补丁**的字节，而头里的 crc32 是对标称形态算的；
 *    要复核就得先把 delta 反算回去，那是另一套工作量（当前只在安装时做回读复核）。*/
static void sec_check_crc(arkrtos_sec_report_t *p_rep)
{
    sec_add(p_rep, "images.crc_recheck", (uint32)ARKRTOS_SEC_SKIP,
            "flash holds relocated bytes; header crc32 covers the nominal form",
            0u, 0u, 0u, 0u);
}

/* 6. 镜像签名：编进来就是安装期强制，没编就是"任何拿到串口的人都能装自己的代码" */
static void sec_check_sign(arkrtos_sec_report_t *p_rep)
{
#if (ARKRTOS_USE_IMAGE_SIGN == 1)
    sec_add(p_rep, "images.signature", (uint32)ARKRTOS_SEC_PASS,
            "hmac-sha256, verified at install (symmetric pre-shared key)",
            0u, 0u, 0u, 0u);
#else
    sec_add(p_rep, "images.signature", (uint32)ARKRTOS_SEC_FINDING,
            "not compiled (ARKRTOS_USE_IMAGE_SIGN=0): unsigned images are accepted",
            0u, 0u, 0u, 0u);
#endif
}

/* 7. 硬件安全态（遗言）：崩溃/被杀之后设备有没有人负责带回安全态 */
static void sec_check_hwsafe(arkrtos_sec_report_t *p_rep)
{
#if (ARKRTOS_HW_SAFE == 1)
    uint32 will = 0u;
    uint32 fail = 0u;
    uint32 cb = 0u;
    uint32 none = 0u;
    uint32 handover = 0u;

    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &handover);
    /* none / handover 不进报告：完整分归在 hwsafe 命令里，这里只看两条通道。 */
    (void)none;
    (void)handover;

    sec_add(p_rep, "hwsafe.deadman", (uint32)ARKRTOS_SEC_PASS,
            "devs=%u owners=%u will/cb=%u/%u",
            (unsigned)arkrtos_hwsafe_dev_count(),
            (unsigned)arkrtos_hwsafe_owner_count(),
            will, cb);
#else
    sec_add(p_rep, "hwsafe.deadman", (uint32)ARKRTOS_SEC_FINDING,
            "not compiled (ARKRTOS_HW_SAFE=0): a dead App leaves its devices as they were",
            0u, 0u, 0u, 0u);
#endif
}

/* 8. 看门狗：内核守护编进来了不等于板子在咬人 */
static void sec_check_guard(arkrtos_sec_report_t *p_rep)
{
#if (ARKRTOS_USE_KERNEL_GUARD == 1) && (ARKRTOS_WDG_ENABLE == 1)
    sec_add(p_rep, "guard.watchdog", (uint32)ARKRTOS_SEC_PASS,
            "kernel guard + board watchdog both active", 0u, 0u, 0u, 0u);
#elif (ARKRTOS_USE_KERNEL_GUARD == 1)
    sec_add(p_rep, "guard.watchdog", (uint32)ARKRTOS_SEC_FINDING,
            "guard compiled but ARKRTOS_WDG_ENABLE=0: a hung task is not reset",
            0u, 0u, 0u, 0u);
#else
    sec_add(p_rep, "guard.watchdog", (uint32)ARKRTOS_SEC_FINDING,
            "not compiled (ARKRTOS_USE_KERNEL_GUARD=0)", 0u, 0u, 0u, 0u);
#endif
}

/* 9. 崩溃台账：它存在与否决定了"崩 3 次禁用"能不能跨复位保持 */
static void sec_check_crash(arkrtos_sec_report_t *p_rep)
{
#if (ARKRTOS_USE_CRASH_LOG == 1)
    const arkrtos_partition_table_t *p_pt = arkrtos_ptable_get();
    uint32 boots = arkrtos_crash_boots();
    uint32 held = 0u;
    uint32 i;

    for(i = 0u; (i < p_pt->slot_max) && (i < (uint32)ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(arkrtos_crash_disabled(i) != 0u)
        {
            held++;
        }
    }

    if(boots == 0u)
    {
        /* magic / version / checksum 任一不过时整本重开：说明这块 RAM 没活过复位，
         * 或者被写坏了。两种都不该算通过。 */
        sec_add(p_rep, "crash.journal", (uint32)ARKRTOS_SEC_FAIL,
                "journal not valid (boots=0): cross-reset history is not kept",
                0u, 0u, 0u, 0u);
    }
    else
    {
        sec_add(p_rep, "crash.journal", (uint32)ARKRTOS_SEC_PASS,
                "boots=%u seq=%u held_slots=%u",
                boots, (unsigned)arkrtos_crash_seq(), held, 0u);
    }
#else
    sec_add(p_rep, "crash.journal", (uint32)ARKRTOS_SEC_FINDING,
            "not compiled (ARKRTOS_USE_CRASH_LOG=0)", 0u, 0u, 0u, 0u);
#endif
}

/* 10. 控制台认证：没有。串口可达即可 install / reboot，这是认证清单上最硬的一条缺口，
 *     所以它固定出现，且不计 PASS。 */
static void sec_check_console(arkrtos_sec_report_t *p_rep)
{
    sec_add(p_rep, "console.auth", (uint32)ARKRTOS_SEC_FINDING,
            "shell has no authentication: serial access = install/reboot/stop",
            0u, 0u, 0u, 0u);
}

/* 11. Flash 读保护 / 写保护：读选项字节（只读、不解锁、不改任何保护位）。
 *     板级实现 arkrtos_port_flash_protection()；没实现的板子由下面的弱符号兜住，
 *     输出与"没有这条通路"时逐字节相同（仍是 SKIP）。
 *     读到了就报读数：RDP 生效算 PASS；RDP 还是 0 级（谁拿到片子都能读走固件）
 *     算 FINDING —— "没读过"和"读出来是 0 级"始终是两件事。 */
__attribute__((weak)) int32 arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out)
{
    (void)p_out;
    return ARKRTOS_SEC_ERR_NO_PORT;
}

static void sec_check_flash_protection(arkrtos_sec_report_t *p_rep)
{
    arkrtos_flash_prot_t fp;
    int32 rc;

    fp.rdp_level   = 0u;
    fp.wrp_mask_lo = 0u;
    fp.wrp_mask_hi = 0u;

    rc = arkrtos_port_flash_protection(&fp);
    if(rc != 0)
    {
        sec_add(p_rep, "flash.protection", (uint32)ARKRTOS_SEC_SKIP,
                "no HAL path to option bytes: read RDP/WRP with a debug probe",
                0u, 0u, 0u, 0u);
        return;
    }

    sec_add(p_rep, "flash.protection",
            (fp.rdp_level != 0u) ? (uint32)ARKRTOS_SEC_PASS : (uint32)ARKRTOS_SEC_FINDING,
            "rdp=%u (0=none), wrp lo=0x%04x hi=0x%04x, bit=protected",
            fp.rdp_level, fp.wrp_mask_lo & 0x0FFFu, fp.wrp_mask_hi & 0x0FFFu, 0u);
}

/* 12. 实时性读数：既然"响应时间可度量"是安全功能（看门狗喂狗窗口、急停）的前提，
 *     就把实测的最大节拍耗时摆出来，而不是写一句"实时性良好"。 */
static void sec_check_rt(arkrtos_sec_report_t *p_rep)
{
#if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_row_t rows[ARKRTOS_RT_ROWS];
    uint32 n;

    if(arkrtos_rt_counter_alive() == 0)
    {
        sec_add(p_rep, "rt.measured", (uint32)ARKRTOS_SEC_SKIP,
                "cycle counter is not running (debugger detached?); no reading",
                0u, 0u, 0u, 0u);
        return;
    }

    n = arkrtos_rt_snapshot(rows, (uint32)ARKRTOS_RT_ROWS);

    sec_add(p_rep, "rt.measured",
            (n == 0u) ? (uint32)ARKRTOS_SEC_SKIP : (uint32)ARKRTOS_SEC_PASS,
            "tick.isr max=%u us over %u samples, %u cyc/tick",
            (n == 0u) ? 0u : (unsigned)rows[0].max_us,
            (n == 0u) ? 0u : (unsigned)rows[0].n,
            (unsigned)arkrtos_rt_cycles_per_tick(), 0u);
#else
    sec_add(p_rep, "rt.measured", (uint32)ARKRTOS_SEC_SKIP,
            "not compiled (ARKRTOS_USE_RTSTAT=0)", 0u, 0u, 0u, 0u);
#endif
}

uint32 arkrtos_sec_run(arkrtos_sec_report_t *p_out)
{
    if(p_out == 0)
    {
        return 0u;
    }

    p_out->count   = 0u;
    p_out->pass    = 0u;
    p_out->fail    = 0u;
    p_out->finding = 0u;
    p_out->skip    = 0u;

    sec_check_gates(p_out);
    sec_check_windows(p_out);
    sec_check_audit(p_out);
    sec_check_headers(p_out);
    sec_check_crc(p_out);
    sec_check_sign(p_out);
    sec_check_hwsafe(p_out);
    sec_check_guard(p_out);
    sec_check_crash(p_out);
    sec_check_console(p_out);
    sec_check_flash_protection(p_out);
    sec_check_rt(p_out);

    return p_out->fail;
}

uint32 arkrtos_sec_count(const arkrtos_sec_report_t *p_rep, uint32 state)
{
    if(p_rep == 0)
    {
        return 0u;
    }

    if(state == (uint32)ARKRTOS_SEC_PASS)
    {
        return p_rep->pass;
    }
    if(state == (uint32)ARKRTOS_SEC_FAIL)
    {
        return p_rep->fail;
    }
    if(state == (uint32)ARKRTOS_SEC_FINDING)
    {
        return p_rep->finding;
    }
    if(state == (uint32)ARKRTOS_SEC_SKIP)
    {
        return p_rep->skip;
    }

    return 0u;
}

const char *arkrtos_sec_state_name(uint32 state)
{
    if(state == (uint32)ARKRTOS_SEC_PASS)
    {
        return "PASS";
    }
    if(state == (uint32)ARKRTOS_SEC_FAIL)
    {
        return "FAIL";
    }
    if(state == (uint32)ARKRTOS_SEC_FINDING)
    {
        return "FIND";
    }
    if(state == (uint32)ARKRTOS_SEC_SKIP)
    {
        return "SKIP";
    }

    return "?";
}

#endif /* ARKRTOS_USE_SECCHECK */
