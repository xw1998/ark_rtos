/**
* @file test_devid_host.c
* @brief 设备身份（arkrtos_devid.c，SEC-G13）的宿主用例
* @details 这一个模块只回答两件事：这台设备叫什么，以及它能不能证明自己就是
*          那台设备。名字是指纹（派生摘要），证明是挑战-应答（HMAC）。板级的两只
*          钩子在用例里被强符号替换成内存数组，所以被测的是**判据与失败关闭**，
*          不是某颗片子 OTP 的读时序。钉住的是下面这些语义：
*
*            1. 只有芯片号、没有每机密钥 -> 有名字（来源 UID_ONLY），
*               但 ready=0，prove() 返回 ERR_NOKEY —— 失败关闭，绝不吐全 0 MAC；
*            2. 板级出号 + 出密钥 -> 来源 OTP、ready=1，ID 等于
*               SHA256("ark-devid-v1" || uid) 的前 ARKRTOS_DEVICE_ID_LEN 字节；
*            3. 同一挑战两次应答相同（确定性），不同挑战应答不同（不重放）；
*            4. check() 自校验：正确 MAC -> OK，改任意一个字节 -> ERR_MISMATCH，
*               且比较是常数时间的（不因前几个字节对就提前返回）；
*            5. 只有编进固件的密钥（ARKRTOS_DEVICE_SECRET）-> 来源必须报
*               BUILTIN，不能报 OTP：同一份构建的每台设备都长一样，那不是身份；
*            6. 拿不到芯片号 -> ERR_STATE，id() 也回 ERR_STATE（没名字可给）；
*            7. 钩子要求超过缓冲区的长度 -> ERR_RANGE，不截断成一个会撞车的名字；
*            8. 接口边界：NULL、空挑战、超长挑战、cap 不够、名字函数。
*
*          未验证（如实说明）：真实 OTP/熔丝的可读性、读取失败与掉电；用例里的
*          "OTP" 只是一个内存数组。宿主能证明的是判定逻辑与失败关闭，板端才能
*          证明那颗片子的槽到底读不读得出来。
* @note 用法：由 tools/coverage_gate.py 的 devid 三条 case 编译并运行，分别输出
*       DEVID-HOST OK / DEVID-HOST OFF OK / DEVID-HOST BUILTIN OK。
*/

#include "arkrtos_features.h"
#include "arkrtos_devid.h"

#include <stdio.h>
#include <string.h>

#if (ARKRTOS_USE_DEVICE_ID == 1)

#include "arkrtos_sha256.h"

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

/* ---------------- 板级钩子的强符号替身 ----------------
 * 内核里的两只钩子是 weak，这里给强符号：同一份源码、两个数组，
 * 于是"这块板有没有号 / 有没有密钥"成了用例可以直接摆的开关。 */

#define UID_BYTES   (12u)
#define SEC_BYTES   (32u)

static uint8  g_uid[UID_BYTES];
static int    g_uid_ok;          /* 0 = 这块板没有唯一号 */
static uint32 g_uid_len;         /* 板级声称写了多少字节 */

static uint8  g_sec[SEC_BYTES];
static int    g_sec_ok;          /* 0 = 这块板没有密钥槽 */
static int    g_sec_len;

int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    uint32 i;

    if(g_uid_ok == 0)
    {
        return -1;
    }
    if(g_uid_len > cap)
    {
        /* 板级说"我要写这么多"，但它比调用者的缓冲区大：如实报长度，
         * 让模块自己判断要不要认（模块回 ERR_RANGE）。 */
        *p_len = g_uid_len;
        return 0;
    }
    for(i = 0u; i < g_uid_len; i++)
    {
        p_out[i] = g_uid[i];
    }
    *p_len = g_uid_len;
    return 0;
}

int32 arkrtos_port_devid_secret(uint8 *p_out, uint32 cap)
{
    int i;

    if(g_sec_ok == 0)
    {
        return -1;
    }
    if((uint32)g_sec_len > cap)
    {
        return g_sec_len;
    }
    for(i = 0; i < g_sec_len; i++)
    {
        p_out[i] = g_sec[i];
    }
    return g_sec_len;
}

/* ---------------- 辅助 ---------------- */

static void uid_fill(uint8 base)
{
    uint32 i;

    for(i = 0u; i < UID_BYTES; i++)
    {
        g_uid[i] = (uint8)(base + (uint8)i);
    }
    g_uid_len = UID_BYTES;
    g_uid_ok  = 1;
}

static void sec_fill(uint8 base)
{
    int i;

    for(i = 0; i < (int)SEC_BYTES; i++)
    {
        g_sec[i] = (uint8)(base + (uint8)i);
    }
    g_sec_len = (int)SEC_BYTES;
    g_sec_ok  = 1;
}

static void state_reset(void)
{
    g_uid_ok  = 0;
    g_uid_len = 0u;
    memset(g_uid, 0, UID_BYTES);
    g_sec_ok  = 0;
    g_sec_len = 0;
    memset(g_sec, 0, SEC_BYTES);
}

/* ---------------- 用例 ---------------- */

#if !defined(ARKRTOS_DEVICE_SECRET)
/* 1. 只有芯片号：有名字，但没有证明能力。
 *    这一档只在"没有编进固件密钥"时才成立：一旦定义了 ARKRTOS_DEVICE_SECRET，
 *    每台设备都会拿到一把内建密钥，来源就变成 BUILTIN 了。 */
static void test_uid_only(void)
{
    arkrtos_devid_status_t st;
    uint8 uid_local[UID_BYTES];
    uint8 digest[32];
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];
    uint8 ch[4] = { 1u, 2u, 3u, 4u };
    arkrtos_sha256_t ctx;
    uint32 i;

    state_reset();
    uid_fill(0x40u);

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "uid-only boot is OK");
    ck(arkrtos_devid_ready() == 0u, "no secret -> not ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_UID_ONLY,
         "source is uid-only");
    ck_u((unsigned long)st.uid_len, UID_BYTES, "uid length reported");
    ck_u((unsigned long)st.id_len, (unsigned long)ARKRTOS_DEVICE_ID_LEN,
         "id length equals the configured length");

    /* ID 必须等于 SHA256("ark-devid-v1" || uid) 的前 id_len 字节 */
    for(i = 0u; i < UID_BYTES; i++)
    {
        uid_local[i] = (uint8)(0x40u + (uint8)i);
    }
    arkrtos_sha256_init(&ctx);
    arkrtos_sha256_update(&ctx, (const uint8 *)"ark-devid-v1", 12u);
    arkrtos_sha256_update(&ctx, uid_local, UID_BYTES);
    arkrtos_sha256_final(&ctx, digest);
    ck(memcmp(st.id, digest, (size_t)st.id_len) == 0, "id is sha256(tag||uid) prefix");

    /* 失败关闭：没有密钥就拒绝，且不把 MAC 写成 0 */
    memset(mac, 0xAA, sizeof(mac));
    ck(arkrtos_devid_prove(ch, sizeof(ch), mac) == ARKRTOS_DEVID_ERR_NOKEY,
       "prove() fails closed without a secret");
    ck(mac[0] == 0xAAu && mac[31] == 0xAAu, "prove() does not zero-fill the MAC");
    ck(arkrtos_devid_check(ch, sizeof(ch), mac) == ARKRTOS_DEVID_ERR_NOKEY,
       "check() propagates the no-key failure");
}
#endif /* !ARKRTOS_DEVICE_SECRET */

/* 2. OTP 出号 + 出密钥：身份成立，挑战-应答可用 */
static void test_otp_identity(void)
{
    arkrtos_devid_status_t st;
    uint8 id1[ARKRTOS_DEVICE_ID_LEN];
    uint8 mac_a[ARKRTOS_DEVID_MAC_LEN];
    uint8 mac_b[ARKRTOS_DEVID_MAC_LEN];
    uint8 mac_c[ARKRTOS_DEVID_MAC_LEN];
    uint8 ch1[8] = { 0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u };
    uint8 ch2[8] = { 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u };
    uint32 out_len = 0u;

    state_reset();
    uid_fill(0x00u);
    sec_fill(0x80u);

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "otp boot is OK");
    ck(arkrtos_devid_ready() == 1u, "otp -> ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_OTP, "source is otp");
    ck_u((unsigned long)st.secret_len, SEC_BYTES, "secret length reported");

    /* id() 与 status 里的 id 一致 */
    memset(id1, 0, sizeof(id1));
    ck(arkrtos_devid_id(id1, sizeof(id1), &out_len) == ARKRTOS_DEVID_OK, "id() works");
    ck_u((unsigned long)out_len, (unsigned long)st.id_len, "id() reports id_len");
    ck(memcmp(id1, st.id, (size_t)st.id_len) == 0, "id() matches status");

    /* 同一挑战两次一致（确定性） */
    ck(arkrtos_devid_prove(ch1, sizeof(ch1), mac_a) == ARKRTOS_DEVID_OK, "prove #1");
    ck(arkrtos_devid_prove(ch1, sizeof(ch1), mac_b) == ARKRTOS_DEVID_OK, "prove #2");
    ck(memcmp(mac_a, mac_b, sizeof(mac_a)) == 0, "same challenge -> same answer");

    /* 不同挑战不同（不重放） */
    ck(arkrtos_devid_prove(ch2, sizeof(ch2), mac_c) == ARKRTOS_DEVID_OK, "prove #3");
    ck(memcmp(mac_a, mac_c, sizeof(mac_a)) != 0, "different challenge -> different answer");

    /* check() 自校验 */
    ck(arkrtos_devid_check(ch1, sizeof(ch1), mac_a) == ARKRTOS_DEVID_OK, "check accepts");
    mac_a[31] ^= 0x01u;
    ck(arkrtos_devid_check(ch1, sizeof(ch1), mac_a) == ARKRTOS_DEVID_ERR_MISMATCH,
       "check rejects a flipped tail byte");
    mac_a[31] ^= 0x01u;
    mac_a[0] ^= 0x80u;
    ck(arkrtos_devid_check(ch1, sizeof(ch1), mac_a) == ARKRTOS_DEVID_ERR_MISMATCH,
       "check rejects a flipped head byte");

    /* 名义上的常数时间：比较循环里没有 early return（源码层断言由门禁钉），
     * 这里只钉语义——最后一个字节不同也必须报 MISMATCH，而不是被提前放过。 */
}

/* 3. 拿不到芯片号：ERR_STATE，没有名字可给 */
static void test_no_uid(void)
{
    arkrtos_devid_status_t st;
    uint8 buf[8];
    uint32 out_len = 0u;
    uint8 ch[1] = { 0x5Au };
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];

    state_reset();
    sec_fill(0x11u);   /* 有密钥也不行：没有名字就不成立 */

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_ERR_STATE, "no uid -> ERR_STATE");
    ck(arkrtos_devid_ready() == 0u, "no uid -> not ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_NONE, "source is none");
    ck_u((unsigned long)st.id_len, 0u, "no id derived");

    ck(arkrtos_devid_id(buf, sizeof(buf), &out_len) == ARKRTOS_DEVID_ERR_STATE,
       "id() refuses before a name exists");
    ck(arkrtos_devid_prove(ch, sizeof(ch), mac) == ARKRTOS_DEVID_ERR_NOKEY,
       "prove() refuses without an identity");
}

/* 3b. 密钥槽配错（短于下限）：来源报 OTP，但 ready=0、证明被拒 */
static void test_short_secret(void)
{
    arkrtos_devid_status_t st;
    uint8 ch[3] = { 0x01u, 0x02u, 0x03u };
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];

    state_reset();
    uid_fill(0x30u);
    sec_fill(0xB0u);
    g_sec_len = 8;        /* 短于 DEVID_SECRET_MIN(16) */

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "short secret does not fail boot");
    ck(arkrtos_devid_ready() == 0u, "short secret -> not ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_OTP,
         "source still says otp so the report can flag a bad slot");
    ck(arkrtos_devid_prove(ch, sizeof(ch), mac) == ARKRTOS_DEVID_ERR_NOKEY,
       "short secret cannot answer a challenge");
}

/* 4. 钩子要求超过缓冲区：ERR_RANGE，不截断 */
static void test_range(void)
{
    state_reset();
    uid_fill(0x00u);
    g_uid_len = 64u;      /* 比内部 32 字节缓冲大 */

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_ERR_RANGE, "oversized uid -> ERR_RANGE");
    ck(arkrtos_devid_ready() == 0u, "oversized uid -> not ready");
}

/* 5. 接口边界 */
static void test_api_edges(void)
{
    arkrtos_devid_status_t st;
    uint8 buf[ARKRTOS_DEVICE_ID_LEN];
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];
    uint32 out_len = 0u;
    uint8 ch[ARKRTOS_DEVID_CHALLENGE_MAX + 1u];

    state_reset();
    uid_fill(0x70u);
    sec_fill(0x30u);
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "boot for edge tests");

    ck(arkrtos_devid_status_get(0) == ARKRTOS_DEVID_ERR_PARAM, "NULL status rejected");

    ck(arkrtos_devid_id(0, sizeof(buf), &out_len) == ARKRTOS_DEVID_ERR_PARAM,
       "id(NULL out) rejected");
    ck(arkrtos_devid_id(buf, sizeof(buf), 0) == ARKRTOS_DEVID_ERR_PARAM,
       "id(NULL len) rejected");
    ck(arkrtos_devid_id(buf, 1u, &out_len) == ARKRTOS_DEVID_ERR_RANGE,
       "id() with a tiny cap is ERR_RANGE");

    memset(ch, 0x42, sizeof(ch));
    ck(arkrtos_devid_prove(0, 4u, mac) == ARKRTOS_DEVID_ERR_PARAM, "NULL challenge rejected");
    ck(arkrtos_devid_prove(ch, 0u, mac) == ARKRTOS_DEVID_ERR_PARAM, "empty challenge rejected");
    ck(arkrtos_devid_prove(ch, (uint32)sizeof(ch), mac) == ARKRTOS_DEVID_ERR_PARAM,
       "oversized challenge rejected");
    ck(arkrtos_devid_prove(ch, 4u, 0) == ARKRTOS_DEVID_ERR_PARAM, "NULL mac rejected");
    ck(arkrtos_devid_check(ch, 4u, 0) == ARKRTOS_DEVID_ERR_PARAM, "check(NULL mac) rejected");
    ck(arkrtos_devid_check(0, 4u, mac) == ARKRTOS_DEVID_ERR_PARAM,
       "check(NULL challenge) rejected");

    memset(&st, 0xAA, sizeof(st));
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status_get works");

    ck(strcmp(arkrtos_devid_src_name(ARKRTOS_DEVID_SRC_OTP), "otp") == 0, "name otp");
    ck(strcmp(arkrtos_devid_src_name(ARKRTOS_DEVID_SRC_UID_ONLY), "uid-only") == 0,
       "name uid-only");
    ck(strcmp(arkrtos_devid_src_name(ARKRTOS_DEVID_SRC_BUILTIN), "builtin") == 0,
       "name builtin");
    ck(strcmp(arkrtos_devid_src_name(ARKRTOS_DEVID_SRC_NONE), "none") == 0, "name none");
    ck(strcmp(arkrtos_devid_src_name(99u), "none") == 0, "unknown source is none");
}

#if defined(ARKRTOS_DEVICE_SECRET)
/* 6. 只有编进固件的密钥：来源必须报 BUILTIN，不能报 OTP */
static void test_builtin(void)
{
    arkrtos_devid_status_t st;
    uint8 ch[2] = { 0x01u, 0x02u };
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];

    state_reset();
    uid_fill(0x20u);
    /* g_sec_ok 仍为 0：板级没有 OTP 密钥 */

    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "builtin secret boots");
    ck(arkrtos_devid_ready() == 1u, "builtin secret -> ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_BUILTIN,
         "source says builtin, not otp");
    ck(arkrtos_devid_prove(ch, sizeof(ch), mac) == ARKRTOS_DEVID_OK,
       "builtin secret can answer");
    ck(arkrtos_devid_check(ch, sizeof(ch), mac) == ARKRTOS_DEVID_OK,
       "builtin answer checks out");

    /* 板级密钥一出现就要盖过内建的：来源必须变回 OTP */
    sec_fill(0xC0u);
    ck(arkrtos_devid_boot() == ARKRTOS_DEVID_OK, "board secret boots");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_DEVID_SRC_OTP,
         "board secret wins over builtin");
}
#endif /* ARKRTOS_DEVICE_SECRET */

int main(void)
{
    int rc;

    setvbuf(stdout, 0, _IONBF, 0);

#if !defined(ARKRTOS_DEVICE_SECRET)
    test_uid_only();
#endif
    test_otp_identity();
    test_no_uid();
    test_short_secret();
    test_range();
    test_api_edges();
#if defined(ARKRTOS_DEVICE_SECRET)
    test_builtin();
#endif

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    rc = (g_fails == 0) ? 0 : 1;
    if(rc == 0)
    {
#if defined(ARKRTOS_DEVICE_SECRET)
        printf("DEVID-HOST BUILTIN OK %d checks\n", g_checks);
#else
        printf("DEVID-HOST OK %d checks\n", g_checks);
#endif
    }
    return rc;
}

#else /* ARKRTOS_USE_DEVICE_ID == 0 */

int main(void)
{
    /* 关档时整个模块不参与编译：这一档要证明的就是"零符号、零行为"。 */
    printf("DEVID-HOST OFF OK\n");
    return 0;
}

#endif /* ARKRTOS_USE_DEVICE_ID */
