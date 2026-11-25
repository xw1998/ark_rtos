/**
* @file test_trust_host.c
* @brief 信任根（arkrtos_trust.c，SEC-G12）的宿主用例
* @details 这一层只做一件事：决定"这次开机用来验镜像的那把公钥，与上次钉在板上
*          的那把是不是同一把"。它不碰 Flash、不碰 OTP，两个板级钩子在用例里被
*          强符号替换成内存数组，所以被测的是**判定与锁存**，不是某颗芯片的写入
*          时序。钉住的是下面这些语义，每一条都是"读错一个字节就会变味"的地方：
*
*            1. 首次开机（槽全 0x00）→ PIN_NEW，摘要真的写进槽里；
*            2. 再次开机同一把钥 → PIN_MATCH，不重复写；
*            3. 槽全 0xFF 也算空白（擦干净的 Flash 与出厂的全 0 都是"没钉过"）；
*            4. 换钥 → PIN_MISMATCH + 锁存 + 返回 ERR_MISMATCH，并且
*               **不覆盖槽里的旧摘要**（覆盖等于让一次换钥把钉子拔了再钉回去）；
*            5. 板级没有 OTP 公钥槽 → 退回落编译进来的公钥，来源报 BUILTIN
*               （能验签，但验不了"这把钥是谁放进来的"，所以它不是根）；
*            6. 读不到钉存槽 → PIN_UNAVAILABLE 且**不锁存**：这块片子没有 OTP
*               不等于有人动过手脚；
*            7. 槽是空白但写不进去 → ERR_PIN + PIN_WRITEFAIL，不锁存也不谎称
*               "已钉住"；
*            8. 锁存只置不清：把钥换回来再开机，锁存仍然在（否则攻击者只要
*               换回原钥就能把钉子复位）；
*            9. 状态查询与两个名字函数的边界。
*
*          未验证（如实说明）：真实 OTP/熔丝的一次性语义、写入时序、掉电中断；
*          用例里的"一次性"只是测试自己把写口关掉。宿主能证明的是判定逻辑，
*          板端能证明的是那颗片子的槽到底可不可写。
* @note 用法：由 tools/coverage_gate.py 的 trust 两条 case 编译并运行，分别输出
*       TRUST-HOST OK / TRUST-HOST OFF OK。
*/

#include "arkrtos_features.h"
#include "arkrtos_trust.h"

#include <stdio.h>
#include <string.h>

#if (ARKRTOS_USE_TRUST_ROOT == 1)

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
 * 内核里的两个钩子是 weak，这里给强符号：同一份源码、两个数组，
 * 于是"板上有没有槽"、"写不写得进去"都成了用例可以直接摆的开关。 */

#define KEY_LEN     (32u)

static uint8 g_key[KEY_LEN];        /* 板级 OTP 里的公钥 */
static int   g_key_ok;              /* 0 = 这块板没有 OTP 公钥槽 */
static uint32 g_key_len;

static uint8 g_slot[KEY_LEN];       /* 钉存槽 */
static int   g_slot_rd;             /* 0 = 读不出来（没有这个槽） */
static int   g_slot_wr;             /* 0 = 写不进去（一次性已烧 / 保护打开） */
static int   g_slot_writes;         /* 实际写了几次 */

int32 arkrtos_port_trust_pubkey(const uint8 **pp_key, uint32 *p_len)
{
    if(g_key_ok == 0)
    {
        return -1;
    }
    *pp_key = g_key;
    *p_len  = g_key_len;
    return 0;
}

int32 arkrtos_port_trust_pin(uint8 *p_digest, uint32 write)
{
    if(write == 0u)
    {
        if(g_slot_rd == 0)
        {
            return -1;
        }
        memcpy(p_digest, g_slot, KEY_LEN);
        return 0;
    }

    if(g_slot_wr == 0)
    {
        return -1;
    }
    memcpy(g_slot, p_digest, KEY_LEN);
    g_slot_writes++;
    return 0;
}

/* ---------------- 辅助 ---------------- */

static void key_fill(uint8 base)
{
    uint32 i;

    for(i = 0u; i < KEY_LEN; i++)
    {
        g_key[i] = (uint8)(base + (uint8)i);
    }
    g_key_len = KEY_LEN;
    g_key_ok  = 1;
}

static void slot_fill(uint8 v)
{
    memset(g_slot, (int)v, KEY_LEN);
}

static void state_reset(void)
{
    /* 注意：g_locked 在模块里是"只置不清"的一次性锁存，用例不能替它复位；
     * 所以锁存那一段必须放在最后跑（见 main 的顺序）。 */
    g_key_ok = 0;
    g_key_len = 0u;
    memset(g_key, 0, KEY_LEN);
    slot_fill(0x00u);
    g_slot_rd = 1;
    g_slot_wr = 1;
    g_slot_writes = 0;
}

/* ---------------- 用例 ---------------- */

/* 1. 首次开机：空白槽 + 板级公钥 -> 钉下去，摘要对得上 */
static void test_first_boot(void)
{
    arkrtos_trust_status_t st;
    uint8 expect[32];

    state_reset();
    key_fill(0x10u);

    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "first boot returns OK");
    ck(arkrtos_trust_locked() == 0u, "first boot does not latch");
    ck_u((unsigned long)g_slot_writes, 1u, "blank slot written exactly once");

    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_TRUST_SRC_OTP, "source is OTP");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_NEW, "pin is NEW");
    ck_u((unsigned long)st.key_len, KEY_LEN, "key length reported");

    arkrtos_sha256_hash(g_key, KEY_LEN, expect);
    ck(memcmp(st.digest, expect, 32) == 0, "digest is sha256 of the board key");
    ck(memcmp(g_slot, expect, 32) == 0, "the same digest landed in the slot");
}

/* 2. 同一把钥再开机：相符，且不再写槽 */
static void test_second_boot(void)
{
    arkrtos_trust_status_t st;
    int writes_before;

    writes_before = g_slot_writes;
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "second boot returns OK");
    ck(arkrtos_trust_locked() == 0u, "second boot does not latch");
    ck_u((unsigned long)g_slot_writes, (unsigned long)writes_before,
         "a matching pin is not rewritten");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_MATCH, "pin is MATCH");
}

/* 3. 全 0xFF 也是空白（擦干净的 Flash） */
static void test_blank_ff(void)
{
    arkrtos_trust_status_t st;

    state_reset();
    slot_fill(0xFFu);
    key_fill(0x20u);

    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "0xFF slot is blank -> OK");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_NEW, "0xFF slot pinned now");
    ck_u((unsigned long)g_slot_writes, 1u, "0xFF slot written once");
}

/* 4. 读不到钉存槽：不锁存，也不谎称已钉 */
static void test_slot_unavailable(void)
{
    arkrtos_trust_status_t st;

    state_reset();
    key_fill(0x30u);
    g_slot_rd = 0;

    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "unreadable slot is not an error");
    ck(arkrtos_trust_locked() == 0u,
       "a part without a pin store is not evidence of tampering");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_UNAVAILABLE,
         "pin is UNAVAILABLE");
    ck_u((unsigned long)g_slot_writes, 0u, "nothing was written");
}

/* 5. 空白槽但写不进去：报错，且不锁存、不假装钉住 */
static void test_slot_write_fails(void)
{
    arkrtos_trust_status_t st;

    state_reset();
    key_fill(0x40u);
    g_slot_wr = 0;

    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_ERR_PIN, "write failure is reported");
    ck(arkrtos_trust_locked() == 0u, "write failure does not latch");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_WRITEFAIL,
         "pin is WRITEFAIL");
}

/* 6. 板级没有公钥槽：退回编进固件的那份，来源必须报 BUILTIN */
static void test_builtin_fallback(void)
{
    arkrtos_trust_status_t st;
    uint8 expect[32];
    static const uint8 pub[KEY_LEN] = ARKRTOS_IMAGE_SIGN_PUBKEY;

    state_reset();
    g_key_ok = 0;                       /* 这块板没有 OTP 公钥 */

    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "builtin key boots");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.source, (unsigned long)ARKRTOS_TRUST_SRC_BUILTIN,
         "source says builtin, not otp");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_NEW,
         "builtin key pins like any other key");
    arkrtos_sha256_hash(pub, KEY_LEN, expect);
    ck(memcmp(st.digest, expect, 32) == 0, "digest is sha256 of the builtin key");
    ck(memcmp(g_slot, expect, 32) == 0, "builtin digest landed in the slot");
}

/* 7. 换钥：锁存、不覆盖旧摘要、换回来也不解锁 */
static void test_mismatch_latches(void)
{
    arkrtos_trust_status_t st;
    uint8 old_digest[32];

    state_reset();
    key_fill(0x50u);
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "pin the first key");
    memcpy(old_digest, g_slot, 32);

    key_fill(0x90u);                    /* 换一把钥 */
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_ERR_MISMATCH, "new key is refused");
    ck(arkrtos_trust_locked() == 1u, "mismatch latches");
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status readable");
    ck_u((unsigned long)st.pin, (unsigned long)ARKRTOS_TRUST_PIN_MISMATCH,
         "pin is MISMATCH");
    ck(memcmp(g_slot, old_digest, 32) == 0, "the slot keeps the old digest");
    ck_u((unsigned long)g_slot_writes, 1u, "the slot was never rewritten");

    key_fill(0x50u);                    /* 把原钥换回来 */
    ck(arkrtos_trust_boot() == ARKRTOS_TRUST_OK, "the original key boots again");
    ck(arkrtos_trust_locked() == 1u, "the latch survives a matching boot");
}

/* 8. 接口边界与名字 */
static void test_api_edges(void)
{
    arkrtos_trust_status_t st;

    ck(arkrtos_trust_status_get(0) == ARKRTOS_TRUST_ERR_PARAM, "NULL status rejected");

    memset(&st, 0xAA, sizeof(st));
    ck(arkrtos_trust_status_get(&st) == ARKRTOS_TRUST_OK, "status_get works");
    ck(st.source <= (uint32)ARKRTOS_TRUST_SRC_BUILTIN, "source is a known code");

    ck(strcmp(arkrtos_trust_src_name(ARKRTOS_TRUST_SRC_OTP), "otp") == 0, "name otp");
    ck(strcmp(arkrtos_trust_src_name(ARKRTOS_TRUST_SRC_BUILTIN), "builtin") == 0,
       "name builtin");
    ck(strcmp(arkrtos_trust_src_name(ARKRTOS_TRUST_SRC_NONE), "none") == 0, "name none");
    ck(strcmp(arkrtos_trust_src_name(99u), "none") == 0, "unknown source is none");

    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_NEW), "pinned-now") == 0,
       "name pinned-now");
    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_MATCH), "match") == 0,
       "name match");
    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_MISMATCH), "mismatch") == 0,
       "name mismatch");
    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_UNAVAILABLE), "unavailable") == 0,
       "name unavailable");
    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_WRITEFAIL), "write-failed") == 0,
       "name write-failed");
    ck(strcmp(arkrtos_trust_pin_name(ARKRTOS_TRUST_PIN_NONE), "none") == 0, "name none pin");
    ck(strcmp(arkrtos_trust_pin_name(99u), "none") == 0, "unknown pin is none");
}

int main(void)
{
    int rc;

    setvbuf(stdout, 0, _IONBF, 0);

    test_first_boot();
    test_second_boot();
    test_blank_ff();
    test_slot_unavailable();
    test_slot_write_fails();
    test_builtin_fallback();
    /* 锁存是不可逆的，所以这一段必须最后跑 */
    test_mismatch_latches();
    test_api_edges();

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    rc = (g_fails == 0) ? 0 : 1;
    if(rc == 0)
    {
        printf("TRUST-HOST OK %d checks\n", g_checks);
    }
    return rc;
}

#else /* ARKRTOS_USE_TRUST_ROOT == 0 */

int main(void)
{
    /* 关档时整个模块不参与编译：这一档要证明的就是"零符号、零行为"。 */
    printf("TRUST-HOST OFF OK\n");
    return 0;
}

#endif /* ARKRTOS_USE_TRUST_ROOT */
