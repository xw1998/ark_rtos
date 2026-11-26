/**
* @file test_sec_host.c
* @brief 安全基线自检（arkrtos_sec.c）的宿主用例
* @details 被测对象是一个**只读的汇总器**：它把编译期开关与运行期结构读一遍，
*          给十五条基线项各下一个结论（PASS / FAIL / FINDING / SKIP），
*          一条被检查的结构都不改。所以宿主用例把被汇总的那几样东西全部桩掉，
*          钉住的是**判据与账目**本身：
*
*            1. 开关位图（gates.kernel）逐位对应哪条机制：隔离后端报的是
*               「本构建用哪个后端」（iso=1 窗口 / 2 页表），不是「这颗核有没有
*               TLB」；前六位缺一位就必须是 FAIL，第七、八位（签名 / 遗言）
*               只作读数、不重复计分；
*            2. 槽位窗口：非空槽位没有 RAM 窗口就是 FAIL，first 报第一个违例的
*               槽号；全都有窗口才 PASS；
*            3. 结构审计：mask 非零即 FAIL，且 first_index 只在 FAIL 时有意义；
*            4. 镜像头：魔数不对报自己的错误码（0x100），不冒充 loader 的结论；
*               只有魔数对的那些才交给 loader_check_header；
*            5. CRC 复核三档分开：0 = PASS、ERR_CRC = FAIL、其他负值 = 「这一份
*               算不出来」，既不算 PASS 也不算 FAIL；一份都算不出来时报 SKIP
*               而不是 PASS（那是假绿）；池里没有镜像同样是 SKIP；
*            6. 签名 / 遗言 / 看门狗 / 崩溃台账 / 实时读数：编进来了报读数，
*               没编进来报 FINDING 或 SKIP 并给出原因，不猜一个好看的结果；
*               看门狗还有第三档——守护编了但板子不咬人，也是 FINDING；
*            7. 控制台认证这一条永远 FINDING：串口可达即可 install / reboot；
*            8. Flash 保护：没有板级通路是 SKIP，读到了就报读数（RDP 0 级是
*               FINDING —— 「没读过」和「读出来是没保护」是两件事）；
*            9. 报告账目：count / pass / fail / finding / skip 与逐项状态
*               互相吻合，arkrtos_sec_run 的返回值就是 FAIL 条数；
*           10. 信任根（SEC-G12，第 14 项 trust.root）：公钥从板级槽位来
*               且一致 / 首启写入才 PASS，不一致 FAIL，槽读不出或写不进只
*               SKIP（不准把「零件没这功能」记成 PASS），只回落内建公钥
*               报 FINDING，开机还没跑过信任根报 SKIP；
*           11. 设备身份（SEC-G13，第 15 项 device.identity）：板级出号 + 出够长
*               的密钥才 PASS，只有号或密钥槽配短了都只 SKIP（不准读成 PASS），
*               只有编进固件的密钥报 FINDING，档关报 SKIP。
*
*          本文件被七种配置各编一次：
*            sec      全部基线机制打开 + 板级 Flash 保护桩（读得到）
*            sec-min  全部关掉 + 没有板级 Flash 保护（弱符号兜底）
*            sec-page 页表后端（LA64，MPU 关、权限分开）+ 守护开着但狗不咬
*            sec-trust 再叠上信任根（SEC-G12）：公钥钩子与钉存槽都是桩，
*                      专门走 trust.root 那一项的 OTP 四态
*            sec-devid 再叠上设备身份（SEC-G13）：号与密钥钩子是桩，
*                      走 device.identity 的「无钥 / 短钥 / 成对」三档
*            sec-devid-builtin SEC-G13 + ARKRTOS_DEVICE_SECRET：走内建
*                      密钥那一条（FINDING），并验证板级密钥会盖过它
*            sec-disabled SECCHECK=0：整模块不编，关档零符号
*          覆盖率按七档并集算，所以每个 #if 分支都有自己的配置去走它。
*
*          未验证（如实说明）：真实板子上这些结构的取值是否真的如此，以及
*          芯片选项字节的读数。宿主用例只能证明**判定逻辑**没写反。
* @note 用法：由 tools/coverage_gate.py 的七条 case 编译并运行，输出 SEC-HOST OK /
*       SEC-HOST MIN OK / SEC-HOST PAGE OK / SEC-HOST DISABLED OK；sec-trust /
*       sec-devid / sec-devid-builtin 三条只多叠一项的钩子桩与开关，
*       仍走默认的 SEC-HOST OK 标示。
*/

#include "arkrtos_sec.h"

#if (ARKRTOS_USE_SECCHECK == 1)

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_config.h"
#include "arkrtos_partition.h"
#include "arkrtos_ptable.h"
#include "arkrtos_share.h"
#include "arkrtos_loader.h"
#include "arkrtos_app_image.h"
#include "arkrtos_audit.h"
#include "arkrtos_crash.h"
#include "arkrtos_hwsafe.h"
#include "arkrtos_rt.h"
#include "arkrtos_hal.h"
#if (ARKRTOS_USE_TRUST_ROOT == 1)
#include "arkrtos_trust.h"
#endif
#if (ARKRTOS_USE_DEVICE_ID == 1)
#include "arkrtos_devid.h"
#endif

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

/* ---------------- 低地址映射 ----------------
 * 分区表里的槽位基址是 32 位地址，而宿主是 64 位进程：直接取本地数组地址
 * 会被截断成非法指针。所以像内核那样把镜像放在低 4 GB 内的一页上。 */

#define IMG_BASE_ADDR   (0x40000000u)
#define IMG_MAP_SIZE    (0x10000u)

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

static uint8 *g_img;

/* ---------------- 分区表桩 ---------------- */

static arkrtos_partition_table_t g_pt;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

/* ---------------- 邻居桩：可调状态 ---------------- */

static arkrtos_audit_result_t g_audit;

void arkrtos_audit_run(arkrtos_audit_result_t *p_out)
{
    *p_out = g_audit;
}

static int32 g_hdr_ret;
static uint32 g_hdr_calls;
/* 只对"这一份"镜像的重定位表指针返回 g_hdr_ret，其余镜像一律 0：
 * 这样同一个用例里可以同时摆"合格的"和"被 loader 拒的"两份镜像。
 * 用 p_rel 而不是 p_hdr 当键：模块把头部拷到自己栈上再交给 loader，
 * 头部地址每次调用都不一样，只有镜像内的重定位表地址是稳定的。 */
static const uint32 *g_hdr_bad_rel;

int32 arkrtos_loader_check_header(const arkrtos_app_header_t *p_hdr,
                                  const uint32 *p_rel, uint32 types_mask)
{
    (void)p_hdr;
    (void)types_mask;
    g_hdr_calls++;
    if((g_hdr_bad_rel != 0) && (p_rel != g_hdr_bad_rel))
    {
        return 0;
    }
    return g_hdr_ret;
}

#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
static int32  g_crc_ret;
static uint32 g_crc_got;

int32 arkrtos_loader_crc_recheck(const uint8 *p_img, uint32 base, uint32 cap,
                                 uint32 *p_got)
{
    (void)p_img;
    (void)base;
    (void)cap;
    if(p_got != 0)
    {
        *p_got = g_crc_got;
    }
    return g_crc_ret;
}
#endif

static uint32 g_crash_boots;
static uint32 g_crash_seq;
static uint32 g_crash_held_slot;

uint32 arkrtos_crash_boots(void)
{
    return g_crash_boots;
}

uint32 arkrtos_crash_seq(void)
{
    return g_crash_seq;
}

uint32 arkrtos_crash_disabled(uint32 slot)
{
    return (slot == g_crash_held_slot) ? 1u : 0u;
}

static uint32 g_hw_will;
static uint32 g_hw_fail;
static uint32 g_hw_cb;
static int32  g_hw_devs;
static int32  g_hw_owners;

void arkrtos_hwsafe_path_counts(uint32 *will_out, uint32 *fail_out,
                                uint32 *cb_out, uint32 *none_out,
                                uint32 *handover_out)
{
    *will_out = g_hw_will;
    *fail_out = g_hw_fail;
    *cb_out = g_hw_cb;
    *none_out = 7u;
    *handover_out = 8u;
}

int32 arkrtos_hwsafe_dev_count(void)
{
    return g_hw_devs;
}

int32 arkrtos_hwsafe_owner_count(void)
{
    return g_hw_owners;
}

#if (ARKRTOS_USE_RTSTAT == 1)
static int32  g_rt_alive;
static uint32 g_rt_rows;
static uint32 g_rt_max_us;
static uint32 g_rt_row_n;
static uint32 g_rt_cpt;

int32 arkrtos_rt_counter_alive(void)
{
    return g_rt_alive;
}

uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows)
{
    uint32 n = g_rt_rows;

    if(n > max_rows)
    {
        n = max_rows;
    }

    if(n > 0u)
    {
        memset(&rows[0], 0, sizeof(rows[0]));
        rows[0].name = "tick.isr";
        rows[0].n = g_rt_row_n;
        rows[0].max_us = g_rt_max_us;
    }

    return n;
}

uint32 arkrtos_rt_cycles_per_tick(void)
{
    return g_rt_cpt;
}
#endif

/* 板级 Flash 保护桩：默认（sec / sec-page 档）给一条能读到读数的通路；
 * sec-min 档用 -DSEC_HOST_NO_PORT 把它去掉，让模块自己的弱符号兜底。 */
#ifndef SEC_HOST_NO_PORT
static int32 g_fp_ret;
static uint32 g_fp_rdp;
static uint32 g_fp_lo;
static uint32 g_fp_hi;

int32 arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out)
{
    if(g_fp_ret != 0)
    {
        return g_fp_ret;
    }

    p_out->rdp_level = g_fp_rdp;
    p_out->wrp_mask_lo = g_fp_lo;
    p_out->wrp_mask_hi = g_fp_hi;
    return 0;
}
#endif

/* ---------------- 报告查询助手 ---------------- */

static const arkrtos_sec_item_t *item_by_name(const arkrtos_sec_report_t *p_rep,
                                              const char *name)
{
    uint32 i;

    for(i = 0u; i < p_rep->count; i++)
    {
        if(strcmp(p_rep->item[i].name, name) == 0)
        {
            return &p_rep->item[i];
        }
    }

    return 0;
}

#if (ARKRTOS_USE_TRUST_ROOT == 1)
/* 信任根的板级钩子替身（SEC-G12）：内核里是 weak，这里给强符号，
 * 于是"板上有没有 OTP 公钥"、"钉存槽读不读得出来、写不写得进去"都成了
 * 用例能直接摆的开关。 */
static uint8  g_tk[32];
static int    g_tk_ok;
static uint8  g_tpin[32];
static int    g_tpin_rd;
static int    g_tpin_wr;

int32 arkrtos_port_trust_pubkey(const uint8 **pp_key, uint32 *p_len)
{
    if(g_tk_ok == 0)
    {
        return -1;
    }
    *pp_key = g_tk;
    *p_len  = 32u;
    return 0;
}

int32 arkrtos_port_trust_pin(uint8 *p_digest, uint32 write)
{
    uint32 i;

    if(write == 0u)
    {
        if(g_tpin_rd == 0)
        {
            return -1;
        }
        for(i = 0u; i < 32u; i++)
        {
            p_digest[i] = g_tpin[i];
        }
        return 0;
    }

    if(g_tpin_wr == 0)
    {
        return -1;
    }
    for(i = 0u; i < 32u; i++)
    {
        g_tpin[i] = p_digest[i];
    }
    return 0;
}
#endif /* ARKRTOS_USE_TRUST_ROOT */

#if (ARKRTOS_USE_DEVICE_ID == 1)
/* 设备身份的板级钩子替身（SEC-G13）：内核里是 weak，这里给强符号，
 * 于是"这块板有没有唯一号 / 密钥槽有多长"成了用例能直接摆的开关。
 * g_dv_sec_len <= 0 表示没有密钥槽。 */
static uint8 g_dv_uid[12];
static int   g_dv_uid_ok;
static uint8 g_dv_sec[32];
static int   g_dv_sec_len;

int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    uint32 i;

    if(g_dv_uid_ok == 0)
    {
        return -1;
    }
    if(cap < 12u)
    {
        *p_len = 12u;
        return 0;
    }
    for(i = 0u; i < 12u; i++)
    {
        p_out[i] = g_dv_uid[i];
    }
    *p_len = 12u;
    return 0;
}

int32 arkrtos_port_devid_secret(uint8 *p_out, uint32 cap)
{
    int i;

    if(g_dv_sec_len <= 0)
    {
        return -1;
    }
    if((uint32)g_dv_sec_len > cap)
    {
        return g_dv_sec_len;
    }
    for(i = 0; i < g_dv_sec_len; i++)
    {
        p_out[i] = g_dv_sec[i];
    }
    return g_dv_sec_len;
}
#endif /* ARKRTOS_USE_DEVICE_ID */

static void table_clear(void)
{
    memset(&g_pt, 0, sizeof(g_pt));
    memset(&g_audit, 0, sizeof(g_audit));
    g_audit.index = (uint32)ARKRTOS_AUDIT_INDEX_NONE;
    g_hdr_ret = 0;
    g_hdr_calls = 0u;
    g_hdr_bad_rel = 0;
#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
    g_crc_ret = 0;
    g_crc_got = 0u;
#endif
    g_crash_boots = 0u;
    g_crash_seq = 0u;
    g_crash_held_slot = 0xFFFFFFFFu;
    g_hw_will = 0u;
    g_hw_fail = 0u;
    g_hw_cb = 0u;
    g_hw_devs = 0;
    g_hw_owners = 0;
#if (ARKRTOS_USE_RTSTAT == 1)
    g_rt_alive = 0;
    g_rt_rows = 0u;
    g_rt_max_us = 0u;
    g_rt_row_n = 0u;
    g_rt_cpt = 0u;
#endif
#ifndef SEC_HOST_NO_PORT
    g_fp_ret = 0;
    g_fp_rdp = 0u;
    g_fp_lo = 0u;
    g_fp_hi = 0u;
#endif
}

/* ---------------- 用例 ---------------- */

/* 1. 报告账目与三个查询接口的边界 */
static void test_api(void)
{
    arkrtos_sec_report_t rep;

    table_clear();
    ck_u((unsigned long)arkrtos_sec_run(0), 0u, "run(NULL) returns 0");

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);

    ck_u(rep.count, 15u, "fifteen baseline items");
    ck_u((unsigned long)(rep.pass + rep.fail + rep.finding + rep.skip),
         (unsigned long)rep.count, "per-state counts add up to count");

    ck_u((unsigned long)arkrtos_sec_count(&rep, ARKRTOS_SEC_PASS), rep.pass,
         "count(PASS)");
    ck_u((unsigned long)arkrtos_sec_count(&rep, ARKRTOS_SEC_FAIL), rep.fail,
         "count(FAIL)");
    ck_u((unsigned long)arkrtos_sec_count(&rep, ARKRTOS_SEC_FINDING), rep.finding,
         "count(FINDING)");
    ck_u((unsigned long)arkrtos_sec_count(&rep, ARKRTOS_SEC_SKIP), rep.skip,
         "count(SKIP)");
    ck_u((unsigned long)arkrtos_sec_count(&rep, 99u), 0u, "count(invalid) is 0");
    ck_u((unsigned long)arkrtos_sec_count(0, ARKRTOS_SEC_PASS), 0u,
         "count(NULL) is 0");

    ck(strcmp(arkrtos_sec_state_name(ARKRTOS_SEC_PASS), "PASS") == 0, "name PASS");
    ck(strcmp(arkrtos_sec_state_name(ARKRTOS_SEC_FAIL), "FAIL") == 0, "name FAIL");
    ck(strcmp(arkrtos_sec_state_name(ARKRTOS_SEC_FINDING), "FIND") == 0, "name FIND");
    ck(strcmp(arkrtos_sec_state_name(ARKRTOS_SEC_SKIP), "SKIP") == 0, "name SKIP");
    ck(strcmp(arkrtos_sec_state_name(99u), "?") == 0, "name invalid is ?");
}

/* 2. 开关位图：位与档位必须与「本构建启用了哪条机制」一一对应 */
static void test_gates(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;
    uint32 m = 0u;
    uint32 iso = 0u;
    uint32 need = 63u;

    /* 期望值由本档实际打开的开关算出来：这不是抄模块的实现，而是把
     * 「哪一位对应哪条机制」这份契约独立写一遍，两边对不上就红。 */
#if (ARKRTOS_USE_MPU == 1)
    m |= 1u;
    iso = 1u;
#elif (ARKRTOS_USE_PAGING == 1)
    m |= 1u;
    iso = 2u;
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

    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);

    p_it = item_by_name(&rep, "gates.kernel");
    ck(p_it != 0, "gates.kernel present");
    if(p_it == 0)
    {
        return;
    }

    ck_u(p_it->v[0], m, "gates bitmask");
    ck_u(p_it->v[1], need, "need = low six bits");
    ck_u(p_it->v[2], iso, "iso backend code");
    ck_u(p_it->v[3], (m >> 6) & 3u, "sign/hwsafe bits reported separately");
    ck_u((unsigned long)p_it->state,
         ((m & need) == need) ? (unsigned long)ARKRTOS_SEC_PASS
                              : (unsigned long)ARKRTOS_SEC_FAIL,
         "gates verdict follows the low six bits");
}

/* 3. 槽位 RAM 窗口 */
static void test_windows(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    g_pt.slot_max = 4u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_ram_base[0] = 0x20001000u;
    g_pt.slot_ram_size[0] = 0x1000u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_DRIVER;
    g_pt.slot_ram_base[1] = 0u;              /* 缺窗口 */
    g_pt.slot_ram_size[1] = 0x1000u;
    g_pt.slot_type[2] = ARKRTOS_SLOT_FREE;   /* 空槽位不算 */
    g_pt.slot_type[3] = ARKRTOS_SLOT_APP;
    g_pt.slot_ram_base[3] = 0x20003000u;
    g_pt.slot_ram_size[3] = 0x1000u;

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "mpu.slot_windows");
    ck(p_it != 0, "mpu.slot_windows present");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FAIL, "window missing -> FAIL");
        ck_u(p_it->v[0], 3u, "three non-empty slots");
        ck_u(p_it->v[1], 1u, "one without window");
        ck_u(p_it->v[2], 1u, "first offender index");
    }

    g_pt.slot_ram_base[1] = 0x20002000u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "mpu.slot_windows");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "all windows -> PASS");
        ck_u(p_it->v[1], 0u, "no missing window");
    }
}

/* 4. 结构审计的转述 */
static void test_audit(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    g_audit.checks = 42u;
    g_audit.mask = 0u;
    g_audit.index = (uint32)ARKRTOS_AUDIT_INDEX_NONE;

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "structures.audit");
    ck(p_it != 0, "structures.audit present");
    if(p_it == 0)
    {
        return;
    }

    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "clean audit -> PASS");
    ck_u(p_it->v[0], 42u, "checks passed through");
    ck_u(p_it->v[1], 0u, "mask 0");
    ck_u(p_it->v[2], 0u, "no index when clean");

    g_audit.mask = 0x4u;
    g_audit.index = 7u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "structures.audit");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FAIL, "audit mask -> FAIL");
        ck_u(p_it->v[1], 4u, "mask reported");
        ck_u(p_it->v[2], 7u, "first index reported");
    }
}

/* 5. 镜像头：魔数自己判，其余交给 loader */
static void test_headers(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;
    arkrtos_app_header_t hdr;
    uint32 base = (uint32)(uintptr_t)g_img;

    table_clear();
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = (uint32)ARKRTOS_APP_MAGIC;

    /* 一份好镜像 */
    memcpy(g_img, &hdr, sizeof(hdr));
    g_pt.slot_max = 1u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[0] = base;
    g_pt.slot_size[0] = 512u;
    g_hdr_ret = 0;

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.headers");
    ck(p_it != 0, "images.headers present");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "good header -> PASS");
        ck_u(p_it->v[0], 1u, "one slot checked");
        ck_u((unsigned long)g_hdr_calls, 1u, "loader asked once");
    }

    /* 第二个槽位魔数坏：必须报 0x100，且不惊动 loader；第三个槽位魔数对
     * 但 loader 拒绝：错误码原样透出。 */
    g_pt.slot_max = 3u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[1] = base + 512u;
    g_pt.slot_size[1] = 512u;
    memset(g_img + 512u, 0, sizeof(hdr));

    g_pt.slot_type[2] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[2] = base + 1024u;
    g_pt.slot_size[2] = 512u;
    memcpy(g_img + 1024u, &hdr, sizeof(hdr));
    g_hdr_ret = -11;
    g_hdr_bad_rel = (const uint32 *)(uintptr_t)(base + 1024u + ARKRTOS_APP_RELOC_OFFSET);
    g_hdr_calls = 0u;

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.headers");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FAIL, "bad header -> FAIL");
        ck_u(p_it->v[0], 3u, "three slots checked");
        ck_u(p_it->v[1], 2u, "two bad");
        ck_u(p_it->v[2], 1u, "first bad slot index");
        ck_u(p_it->v[3], 0x100u, "magic error reported as 0x100");
        ck_u((unsigned long)g_hdr_calls, 2u, "loader sees exactly the two good-magic slots");
    }

    /* 只有魔数对的那一个被 loader 拒绝时，错误码是 loader 的 */
    g_hdr_calls = 0u;
    g_hdr_bad_rel = 0;
    g_pt.slot_max = 1u;
    g_pt.slot_base[1] = 0u;
    g_pt.slot_base[2] = 0u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.headers");
    if(p_it != 0)
    {
        ck_u(p_it->v[3], (uint32)-11, "loader error code passed through");
    }
    g_hdr_ret = 0;
}

/* 6. CRC 复核：三档分开，一份都算不出来时不报 PASS */
static void test_crc(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;
    uint32 base = (uint32)(uintptr_t)g_img;

    (void)base;                 /* CRC 关档时这一档不用基址 */
    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.crc_recheck");
    ck(p_it != 0, "images.crc_recheck present");
    if(p_it == 0)
    {
        return;
    }

#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
    /* 池里没有镜像 -> SKIP */
    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP, "no image -> SKIP");

    /* 有镜像但一份都算不出来 -> SKIP，v0 = unknown */
    g_pt.slot_max = 1u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[0] = base;
    g_pt.slot_size[0] = 512u;
    g_crc_ret = -6;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.crc_recheck");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
             "nothing computable -> SKIP not PASS");
        ck_u(p_it->v[0], 1u, "unknown count reported");
    }

    /* 一致 -> PASS */
    g_crc_ret = 0;
    g_crc_got = 0x1234u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.crc_recheck");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "crc match -> PASS");
        ck_u(p_it->v[0], 1u, "checked 1");
        ck_u(p_it->v[1], 0u, "bad 0");
        ck_u(p_it->v[2], 0u, "unknown 0");
    }

    /* 不一致 -> FAIL */
    g_crc_ret = (int32)ARKRTOS_LOADER_ERR_CRC;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.crc_recheck");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FAIL, "crc mismatch -> FAIL");
        ck_u(p_it->v[1], 1u, "bad 1");
        ck_u(p_it->v[3], 0u, "first bad slot");
    }

    /* 两槽位、第二个空：只有第一个被算，仍是一份 */
    g_pt.slot_max = 2u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[1] = base + 512u;
    g_pt.slot_size[1] = 512u;
    g_crc_ret = 0;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "images.crc_recheck");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "two images -> PASS");
        ck_u(p_it->v[0], 2u, "checked 2");
        ck_u(p_it->v[2], 0u, "unknown 0");
    }

    g_pt.slot_type[1] = ARKRTOS_SLOT_FREE;
    g_crc_ret = -6;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
#else
    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP, "CRC recheck off -> SKIP");
    ck_u(p_it->v[0], 0u, "no reading when off");
#endif
}

/* 7. 编译期机制：签名 / 遗言 / 看门狗 / 崩溃台账 / 控制台 */
static void test_switches(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    g_hw_devs = 2;
    g_hw_owners = 3;
    g_hw_will = 4u;
    g_hw_fail = 5u;
    g_hw_cb = 6u;
    g_crash_boots = 9u;
    g_crash_seq = 11u;
    g_crash_held_slot = 2u;
    g_pt.slot_max = 4u;

    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);

    p_it = item_by_name(&rep, "images.signature");
    ck(p_it != 0, "images.signature present");
    if(p_it != 0)
    {
#if (ARKRTOS_USE_IMAGE_SIGN == 1)
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "sign on -> PASS");
#else
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "sign off -> FINDING (unsigned images accepted)");
#endif
    }

    p_it = item_by_name(&rep, "hwsafe.deadman");
    ck(p_it != 0, "hwsafe.deadman present");
    if(p_it != 0)
    {
#if (ARKRTOS_HW_SAFE == 1)
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "hwsafe on -> PASS");
        ck_u(p_it->v[0], 2u, "device count");
        ck_u(p_it->v[1], 3u, "owner count");
        ck_u(p_it->v[2], 4u, "will count");
        ck_u(p_it->v[3], 6u, "callback count");
#else
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "hwsafe off -> FINDING");
#endif
    }

    p_it = item_by_name(&rep, "guard.watchdog");
    ck(p_it != 0, "guard.watchdog present");
    if(p_it != 0)
    {
#if (ARKRTOS_USE_KERNEL_GUARD == 1) && (ARKRTOS_WDG_ENABLE == 1)
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS,
             "guard + watchdog -> PASS");
#else
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "no live watchdog -> FINDING");
#endif
    }

    p_it = item_by_name(&rep, "crash.journal");
    ck(p_it != 0, "crash.journal present");
    if(p_it != 0)
    {
#if (ARKRTOS_USE_CRASH_LOG == 1)
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "journal valid -> PASS");
        ck_u(p_it->v[0], 9u, "boots");
        ck_u(p_it->v[1], 11u, "seq");
        ck_u(p_it->v[2], 1u, "held slots");
#else
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "crash log off -> FINDING");
#endif
    }

    p_it = item_by_name(&rep, "console.auth");
    ck(p_it != 0, "console.auth present");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "console auth is a permanent FINDING");
    }

#if (ARKRTOS_USE_CRASH_LOG == 1)
    /* 台账没活过复位（boots = 0）时不许算通过 */
    g_crash_boots = 0u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "crash.journal");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FAIL,
             "invalid journal -> FAIL");
    }
#endif
}

/* 8. Flash 保护：没通路是 SKIP，读到了就报读数 */
static void test_flash(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "flash.protection");
    ck(p_it != 0, "flash.protection present");
    if(p_it == 0)
    {
        return;
    }

#ifdef SEC_HOST_NO_PORT
    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
         "no board path -> SKIP");
#else
    /* RDP 还是 0 级：读得到，但这不是 PASS */
    g_fp_rdp = 0u;
    g_fp_lo = 0x0FFFu;
    g_fp_hi = 0x0001u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "flash.protection");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_FINDING,
             "rdp level 0 -> FINDING");
        ck_u(p_it->v[0], 0u, "rdp level reported");
        ck_u(p_it->v[1], 0x0FFFu, "wrp lo masked to 12 bits");
        ck_u(p_it->v[2], 0x1u, "wrp hi masked to 12 bits");
    }

    /* RDP 生效 -> PASS */
    g_fp_rdp = 1u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "flash.protection");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS,
             "rdp active -> PASS");
    }

    /* 板级读失败 -> SKIP（不是 PASS，也不是 FAIL） */
    g_fp_ret = -3;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "flash.protection");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
             "read failure -> SKIP");
    }
    g_fp_ret = 0;
#endif
}

/* 9. 实时读数：计数器不在 / 没样本 / 有样本 */
static void test_rt(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "rt.measured");
    ck(p_it != 0, "rt.measured present");
    if(p_it == 0)
    {
        return;
    }

#if (ARKRTOS_USE_RTSTAT == 1)
    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
         "counter not alive -> SKIP");

    g_rt_alive = 1;
    g_rt_rows = 0u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "rt.measured");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
             "no sample yet -> SKIP not PASS");
    }

    g_rt_rows = 5u;
    g_rt_row_n = 12u;
    g_rt_max_us = 120u;
    g_rt_cpt = 1000000u;
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "rt.measured");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, ARKRTOS_SEC_PASS, "samples -> PASS");
        ck_u(p_it->v[0], 120u, "max us");
        ck_u(p_it->v[1], 12u, "sample count");
        ck_u(p_it->v[2], 1000000u, "cycles per tick");
    }
#else
    ck_u((unsigned long)p_it->state, ARKRTOS_SEC_SKIP,
         "rtstat off -> SKIP");
#endif
}

/* 14. 信任根（SEC-G12）：四种读数必须分开，尤其"没得看"不能报成 PASS */
static void test_trust(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    ck(p_it != 0, "trust.root present");
    if(p_it == 0)
    {
        return;
    }

#if (ARKRTOS_USE_TRUST_ROOT == 1)
    /* 开机还没跑过信任根：没有公钥被解析出来 -> SKIP，不是 PASS */
    ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
         "trust boot not run -> SKIP");

    /* 板级公钥 + 空白槽：首次开机把摘要钉下去 -> PASS */
    memset(g_tk, 0x11, sizeof(g_tk));
    memset(g_tpin, 0, sizeof(g_tpin));
    g_tk_ok = 1;
    g_tpin_rd = 1;
    g_tpin_wr = 1;
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "first boot pins the digest");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_PASS,
             "OTP key pinned now -> PASS");
        ck_u(p_it->v[0], 32u, "key length reported");
    }

    /* 同一把钥再开机：相符 -> 仍是 PASS */
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "second boot matches");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_PASS,
             "pinned match -> PASS");
    }

    /* 板级没有公钥槽：只能用编进固件的那份 -> FINDING（不是 PASS）。
     * 槽要先抹回空白，否则那把固件公钥的摘要与刚才钉下的对不上，
     * 这一档就变成"MISMATCH"而不是我们要看的"BUILTIN"。 */
    g_tk_ok = 0;
    memset(g_tpin, 0, sizeof(g_tpin));
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "builtin key boots");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_FINDING,
             "builtin key -> FINDING, not PASS");
    }

    /* 空白槽写不进去 -> SKIP（没建立起根，也没什么可看的） */
    g_tk_ok = 1;
    memset(g_tk, 0x11, sizeof(g_tk));
    memset(g_tpin, 0, sizeof(g_tpin));
    g_tpin_wr = 0;
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_ERR_PIN, "pin write refused");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
             "write failure -> SKIP");
    }

    /* 板上没有钉存槽 -> SKIP，且不锁存 */
    g_tpin_wr = 1;
    g_tpin_rd = 0;
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "missing pin store is not an error");
    ck(arkrtos_trust_locked() == 0u, "missing pin store does not latch");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
             "no pin store -> SKIP");
    }

    /* 换钥 -> FAIL。放在最后：锁存是一次性的，后面的用例不能指望它复位。 */
    g_tpin_rd = 1;
    memset(g_tpin, 0x22, sizeof(g_tpin));
    memset(g_tk, 0x33, sizeof(g_tk));
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_ERR_MISMATCH, "new key is refused");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "trust.root");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_FAIL,
             "mismatch -> FAIL");
    }
#else
    ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
         "trust root off -> SKIP");
#endif
}

/* 设备身份（SEC-G13，第 15 项 device.identity）：
 * 只有"板上出号 + 板上出够长的密钥"才算 PASS；只有号是 SKIP（证不了），
 * 密钥槽配短了也是 SKIP（不能读成 PASS）；只有编进固件的那把报 FINDING
 * （能应答，但人人都能顶替）。档关时一律 SKIP。 */
static void test_devid(void)
{
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *p_it;

    table_clear();
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "device.identity");
    ck(p_it != 0, "device.identity present");
    if(p_it == 0)
    {
        return;
    }

#if (ARKRTOS_USE_DEVICE_ID == 1)
    /* 开机还没跑过 devid：无号无钥 -> SKIP */
    ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
         "devid boot not run -> SKIP");

    g_dv_uid_ok = 1;
    memset(g_dv_uid, 0x5A, sizeof(g_dv_uid));

#if !defined(ARKRTOS_DEVICE_SECRET)
    /* 有号、没有密钥槽 -> SKIP（有名字，证不了） */
    g_dv_sec_len = -1;
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "uid-only devid boot");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "device.identity");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
             "uid without a secret -> SKIP");
    }

    /* 密钥槽配短了（8 字节）-> 仍是 SKIP，但不能说成 PASS */
    memset(g_dv_sec, 0x11, sizeof(g_dv_sec));
    g_dv_sec_len = 8;
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "short secret still boots");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "device.identity");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
             "too-short secret -> SKIP");
    }
#else
    /* 内建密钥（板级没有槽）-> FINDING：能应答，但不是身份 */
    g_dv_sec_len = -1;
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "builtin devid boot");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "device.identity");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_FINDING,
             "builtin secret -> FINDING, not PASS");
    }
#endif

    /* 号 + 32 字节板级密钥 -> PASS，并报出 ID 长度 */
    memset(g_dv_sec, 0x22, sizeof(g_dv_sec));
    g_dv_sec_len = 32;
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "otp devid boot");
    memset(&rep, 0, sizeof(rep));
    (void)arkrtos_sec_run(&rep);
    p_it = item_by_name(&rep, "device.identity");
    if(p_it != 0)
    {
        ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_PASS,
             "otp uid + otp secret -> PASS");
        ck_u((unsigned long)p_it->v[0], (unsigned long)ARKRTOS_DEVICE_ID_LEN,
             "id length reported");
    }
#else
    ck_u((unsigned long)p_it->state, (unsigned long)ARKRTOS_SEC_SKIP,
         "devid off -> SKIP");
#endif
}

int main(void)
{
    int rc;

    setvbuf(stdout, 0, _IONBF, 0);

    g_img = (uint8 *)(uintptr_t)IMG_BASE_ADDR;
    if(map_at(IMG_BASE_ADDR, IMG_MAP_SIZE) != 0)
    {
        printf("SEC-HOST FAIL: cannot map image window\n");
        return 1;
    }

    test_api();
    test_gates();
    test_windows();
    test_audit();
    test_headers();
    test_crc();
    test_switches();
    test_flash();
    test_rt();
    test_trust();
    test_devid();

    /* 全清的板子跑一遍：项数固定 13，run 返回 FAIL 条数 */
    {
        arkrtos_sec_report_t rep;
        uint32 fails;

        table_clear();
        memset(&rep, 0, sizeof(rep));
        fails = arkrtos_sec_run(&rep);
        ck_u(rep.count, 15u, "final run has 15 items");
        ck_u((unsigned long)fails, (unsigned long)rep.fail,
             "run() returns the FAIL count");
    }

    printf("checks=%d fails=%d\n", g_checks, g_fails);

    rc = (g_fails == 0) ? 0 : 1;
    if(rc == 0)
    {
#if defined(SEC_HOST_EXPECT_MIN)
        printf("SEC-HOST MIN OK  %d checks, all baseline mechanisms off\n", g_checks);
#elif defined(SEC_HOST_EXPECT_PAGE)
        printf("SEC-HOST PAGE OK  %d checks, paging backend, watchdog not biting\n",
               g_checks);
#else
        printf("SEC-HOST OK  %d checks, baseline mechanisms and break-glass paths\n",
               g_checks);
#endif
    }

    return rc;
}

#else /* ARKRTOS_USE_SECCHECK == 0 */

#include <stdio.h>

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    printf("SEC-HOST DISABLED OK  switch=0, no symbols\n");
    return 0;
}

#endif /* ARKRTOS_USE_SECCHECK */
