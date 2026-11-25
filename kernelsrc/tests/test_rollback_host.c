/**
* @file test_rollback_host.c
* @brief 回滚白名单（arkrtos_rollback.c，SEC-G11）的宿主用例
* @details 被测模块只做一件事：给「版本不许降」这条规则开一个**显式、有界、
*          留痕**的例外。所以这里钉住的不是「能不能放行」，而是那几条会
*          静默放行错误东西的地方：
*
*            - 白名单绑定的是**内容**，不是版本号：同样的 id/version 但
*              crc32 或 image_size 不同的镜像必须被拒。只比版本号等于把
*              「v1.2.3」这个名字当成凭据，谁都能造一个叫 v1.2.3 的镜像；
*            - 配额按「放行」消耗，装失败不退。退了就等于「一次授权可以
*              反复尝试」，而不退的方向是失败关闭；
*            - 条目只在 RAM：clear() 之后同一份镜像必须重新被拒（复位即失效
*              是这套机制的安全前提，不是缺陷）；
*            - arm 的锚点从池里那份**已提交**副本读出来（crc32 / image_size），
*              不由调用方口述；被崩溃策略禁用（INVALID）的副本不是锚点；
*            - 放行与武装都要落到持久审计（SEC-G05 开时）：kind 必须是
*              ROLLBACK_ARM / ROLLBACK_OK，否则审计台账里看不见「为什么
*              又跑回旧版本」。
*
*          桩只有两个：arkrtos_ptable_get() 返回一张假分区表，审计开启时
*          多一个 arkrtos_audit_log_record()（签名取自真头文件，桩一旦与声明
*          漂移，测的就不是模块而是桩）。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_rollback.h"
#include "arkrtos_features.h"
#include "arkrtos_share.h"
#include "arkrtos_ptable.h"

#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

/* ck() 只在关档用例里用：开档那份断言走 ck_u / ck_s 一族，把 ck 也编进去
 * 会让 -Wunused-function 报一条（warn_gate 认这个）。故按开关收窄定义。 */
#if (ARKRTOS_USE_ROLLBACK == 0)
static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}
#endif

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu want %lu)\n", what, got, want);
    }
}

#if (ARKRTOS_USE_ROLLBACK == 1)

/* ---------------- 假分区表与假镜像头 ---------------- */

static arkrtos_partition_table_t g_pt;

/* 三份假镜像：每份都是「256 字节头 + 64 字节负载」，够放一个合法头。 */
#define FAKE_IMAGES  3u
#define FAKE_BYTES   (ARKRTOS_APP_HEADER_SIZE + 64u)

static uint8 g_img[FAKE_IMAGES][FAKE_BYTES];

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

#if (ARKRTOS_USE_AUDIT_LOG == 1)
/* 审计桩：只记账，不落盘。签名来自 arkrtos_audit_log.h。 */
static uint32 g_audit_kinds[8];
static uint32 g_audit_codes[8];
static uint32 g_audit_n;

int32 arkrtos_audit_log_record(uint32 kind, uint32 owner, uint32 code)
{
    (void)owner;
    if(g_audit_n < 8u)
    {
        g_audit_kinds[g_audit_n] = kind;
        g_audit_codes[g_audit_n] = code;
        g_audit_n++;
    }
    return 0;
}
#endif

static arkrtos_app_header_t *fake_hdr(uint32 idx)
{
    return (arkrtos_app_header_t *)g_img[idx];
}

static void fake_reset(void)
{
    uint32 i;

    memset(&g_pt, 0, sizeof(g_pt));
    memset(g_img, 0, sizeof(g_img));
    g_pt.slot_max = ARKRTOS_SLOT_ARRAY_MAX;

    /* 槽位全空：类型 FREE、基址 0、状态 EMPTY。 */
    for(i = 0u; i < ARKRTOS_SLOT_ARRAY_MAX; i++)
    {
        g_pt.slot_type[i]  = 0u;
        g_pt.slot_state[i] = 0u;
        g_pt.slot_base[i]  = 0u;
    }
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    memset(g_audit_kinds, 0, sizeof(g_audit_kinds));
    memset(g_audit_codes, 0, sizeof(g_audit_codes));
    g_audit_n = 0u;
#endif
    (void)arkrtos_rollback_clear();
}

/* 在 0 号槽放一份「已装好」的副本，内容由参数决定。 */
static void fake_install(uint32 slot, uint32 image_id, uint32 version,
                         uint32 image_size, uint32 crc32, uint32 state,
                         uint32 type)
{
    arkrtos_app_header_t *p = fake_hdr(0);

    if(slot >= FAKE_IMAGES)
    {
        return;
    }
    p = fake_hdr(slot);
    p->magic      = ARKRTOS_APP_MAGIC;
    p->type       = type;
    p->version    = version;
    p->image_size = image_size;
    p->crc32      = crc32;
    p->image_id   = image_id;
    p->state      = ARKRTOS_APP_STATE_VALID;

    g_pt.slot_type[slot]  = type;
    g_pt.slot_state[slot] = state;
    g_pt.slot_base[slot]  = (uint32)(uintptr_t)p;
}

static void make_hdr(arkrtos_app_header_t *p, uint32 image_id, uint32 version,
                     uint32 image_size, uint32 crc32)
{
    memset(p, 0, sizeof(*p));
    p->magic      = ARKRTOS_APP_MAGIC;
    p->type       = ARKRTOS_APP_TYPE_APP;
    p->version    = version;
    p->image_size = image_size;
    p->crc32      = crc32;
    p->image_id   = image_id;
}

/* ---------------- A 组：参数与空表 ---------------- */

static void case_params(void)
{
    arkrtos_app_header_t hdr;

    fake_reset();

    ck_u((unsigned long)arkrtos_rollback_arm(0u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "arm: id 0 is refused");
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 0u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "arm: version 0 is refused");
    ck_u((unsigned long)arkrtos_rollback_arm_ex(0u, 1u, 16u, 0x1234u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "arm_ex: id 0 is refused");
    ck_u((unsigned long)arkrtos_rollback_arm_ex(7u, 1u, 0u, 0x1234u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "arm_ex: size 0 is refused");
    ck_u((unsigned long)arkrtos_rollback_arm_ex(7u, 1u, 16u, 0u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "arm_ex: crc 0 is refused");
    ck_u((unsigned long)arkrtos_rollback_allow(0),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_PARAM, "allow: null header is refused");

    make_hdr(&hdr, 7u, 1u, 16u, 0x1234u);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: nothing armed, nothing admitted");
    ck_u((unsigned long)arkrtos_rollback_permits(), 0ul,
         "allow: a refusal is not a permit");
}

/* ---------------- B 组：arm 从池里读锚点 ---------------- */

static void case_arm_from_pool(void)
{
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOTFOUND,
         "arm: nothing installed, no anchor");

    /* 已提交副本：锚点取它的 size / crc，而不是调用方口述的值。 */
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: committed copy is an anchor");
    {
        arkrtos_rollback_entry_t e[4];
        uint32 n = arkrtos_rollback_list(e, 4u);

        ck_u(n, 1ul, "arm: one entry");
        ck_u(e[0].image_id, 7ul, "arm: entry keeps the id");
        ck_u(e[0].version, 1ul, "arm: entry keeps the version");
        ck_u(e[0].image_size, 100ul, "arm: size comes from the pool copy");
        ck_u(e[0].crc32, 0xAAAAul, "arm: crc comes from the pool copy");
        ck_u(e[0].budget, 1ul, "arm: default budget is ARKRTOS_ROLLBACK_USES");
        ck_u(e[0].used, 0ul, "arm: nothing used yet");
    }

    /* 版本对不上 / id 对不上：都不是锚点。 */
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 2u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOTFOUND,
         "arm: wrong version is not an anchor");
    ck_u((unsigned long)arkrtos_rollback_arm(8u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOTFOUND,
         "arm: wrong id is not an anchor");

    /* INVALID（被崩溃策略禁用）的副本不是锚点：它正是要躲开的那份。 */
    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_INVALID,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOTFOUND,
         "arm: a disabled (INVALID) copy is not an anchor");

    /* 驱动也能回滚：槽类型不参与判断。 */
    fake_reset();
    fake_install(0u, 9u, 3u, 40u, 0xBBBBu, ARKRTOS_APP_SLOT_RUNNING,
                 ARKRTOS_SLOT_DRIVER);
    ck_u((unsigned long)arkrtos_rollback_arm(9u, 3u, 2u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: a running driver is an anchor");
}

/* ---------------- C 组：放行只认内容 ---------------- */

static void case_content_bound(void)
{
    arkrtos_app_header_t hdr;

    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: installed copy armed");

    /* 同一份内容：放行。 */
    make_hdr(&hdr, 7u, 1u, 100u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "allow: identical content is admitted");
    ck_u((unsigned long)arkrtos_rollback_permits(), 1ul, "allow: one permit counted");

    /* 配额 1 已用完：同一份内容第二次被拒（有界性就在这里）。 */
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: budget exhausted, second admission refused");
    ck_u((unsigned long)arkrtos_rollback_permits(), 1ul,
         "allow: a spent budget is not a permit");

    /* 版本号相同、内容不同：拒。只比版本号就等于把名字当凭据。 */
    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: re-armed for the content cases");

    make_hdr(&hdr, 7u, 1u, 100u, 0xBBBBu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: same version, different crc is refused");
    make_hdr(&hdr, 7u, 1u, 120u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: same version, different size is refused");
    make_hdr(&hdr, 7u, 2u, 100u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: a different version is refused");
    make_hdr(&hdr, 8u, 1u, 100u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: a different image id is refused");
}

/* ---------------- D 组：预算、容量、复位 ---------------- */

static void case_budget_and_clear(void)
{
    arkrtos_app_header_t hdr;
    arkrtos_rollback_entry_t e[4];
    uint32 i;

    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);

    /* 重复武装同一目标：预算累加，不新增条目。 */
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 2u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: budget 2");
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 3u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: re-arm tops up");
    ck_u(arkrtos_rollback_list(e, 4u), 1ul, "arm: re-arm does not add an entry");
    ck_u(e[0].budget, 5ul, "arm: budgets add up");

    make_hdr(&hdr, 7u, 1u, 100u, 0xAAAAu);
    for(i = 0u; i < 5u; i++)
    {
        ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
             (unsigned long)ARKRTOS_ROLLBACK_OK, "allow: within budget");
    }
    ck_u((unsigned long)arkrtos_rollback_permits(), 5ul, "allow: five permits");
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "allow: sixth admission refused");

    /* 容量：ARKRTOS_ROLLBACK_ENTRIES 条之后没空位。 */
    fake_reset();
    for(i = 0u; i < (uint32)ARKRTOS_ROLLBACK_ENTRIES; i++)
    {
        ck_u((unsigned long)arkrtos_rollback_arm_ex(100u + i, 1u, 16u,
                                                    0x1000u + i, 1u),
             (unsigned long)ARKRTOS_ROLLBACK_OK, "arm_ex: entry fits");
    }
    ck_u((unsigned long)arkrtos_rollback_arm_ex(999u, 1u, 16u, 0x9000u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_FULL,
         "arm_ex: whitelist is full");

    /* 复位语义：clear() 之后同一份镜像重新被拒。 */
    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 1u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "arm: armed before the reset case");
    ck_u((unsigned long)arkrtos_rollback_clear(),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "clear: always OK");
    ck_u(arkrtos_rollback_list(e, 4u), 0ul, "clear: no entries left");
    make_hdr(&hdr, 7u, 1u, 100u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_ERR_NOENTRY,
         "clear: the same image is refused again (an arm does not survive)");
    ck_u((unsigned long)arkrtos_rollback_permits(), 0ul,
         "clear: the permit counter is reset too");
}

#if (ARKRTOS_USE_AUDIT_LOG == 1)
/* ---------------- E 组：审计留痕 ---------------- */

static void case_audit(void)
{
    arkrtos_app_header_t hdr;

    fake_reset();
    fake_install(0u, 7u, 1u, 100u, 0xAAAAu, ARKRTOS_APP_SLOT_LOADED,
                 ARKRTOS_SLOT_APP);
    ck_u((unsigned long)arkrtos_rollback_arm(7u, 1u, 2u),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "audit: arm ok");
    ck_u(g_audit_n, 1ul, "audit: one record for the arm");
    ck_u(g_audit_kinds[0], ARKRTOS_AUDIT_EV_ROLLBACK_ARM,
         "audit: the arm is recorded as ROLLBACK_ARM");
    ck_u(g_audit_codes[0], 2ul, "audit: the arm record carries the granted budget");

    make_hdr(&hdr, 7u, 1u, 100u, 0xAAAAu);
    ck_u((unsigned long)arkrtos_rollback_allow(&hdr),
         (unsigned long)ARKRTOS_ROLLBACK_OK, "audit: permit ok");
    ck_u(g_audit_n, 2ul, "audit: the permit adds a record");
    ck_u(g_audit_kinds[1], ARKRTOS_AUDIT_EV_ROLLBACK_OK,
         "audit: the admission is recorded as ROLLBACK_OK");
    ck_u(g_audit_codes[1], 1ul, "audit: the permit record carries the use count");

    /* 拒绝不写审计：安装路径自己会写 IMAGE_REFUSE，重复记一条只会让
     * 「被拒了几次」变成两倍的读数。 */
    make_hdr(&hdr, 7u, 2u, 100u, 0xAAAAu);
    (void)arkrtos_rollback_allow(&hdr);
    ck_u(g_audit_n, 2ul, "audit: a refusal writes nothing here");
}
#endif

int main(void)
{
    case_params();
    case_arm_from_pool();
    case_content_bound();
    case_budget_and_clear();
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    case_audit();
#endif

    if(g_fails != 0)
    {
        printf("ROLLBACK HOST FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("ROLLBACK HOST OK  %d checks\n", g_checks);
    return 0;
}

#else   /* ARKRTOS_USE_ROLLBACK == 0 */

int main(void)
{
    /* 关档：模块编成空 TU，宿主用例只剩一件事要证明——开关确实是关的。 */
    ck(ARKRTOS_USE_ROLLBACK == 0, "OFF: the switch is 0");
    ck(ARKRTOS_ROLLBACK_ENTRIES >= 1, "OFF: entries default is still sane");

    if(g_fails != 0)
    {
        printf("ROLLBACK OFF FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("ROLLBACK OFF OK  %d checks\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_ROLLBACK */
