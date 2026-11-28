/**
* @file arkrtos_devid.c
* @brief Per-device identity and challenge-response proof (SEC-G13)
* @details See arkrtos_devid.h for the claim and its limits. The whole file is
*          compiled out when ARKRTOS_USE_DEVICE_ID is 0, so the default build
*          keeps every byte of the boot and sec paths exactly as they were.
*/

#include "arkrtos_types.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_DEVICE_ID == 1)

#include "arkrtos_devid.h"
#include "arkrtos_sha256.h"

/* 芯片唯一号的读回上限。96 位（12 字节）是常见形态，留到 32 字节是为了
 * 不因为某颗片子多几位就把人挡在门外；再长就不是"唯一号"而是资产标签了。 */
#define DEVID_UID_MAX       (32u)
/* 每机密钥的长度区间。低于 16 字节的"密钥"挡不住穷举；超过 64 字节没有
 * 收益（HMAC 会把它压成一个块），只会让 OTP 槽规划更贵。 */
#define DEVID_SECRET_MIN    (16u)
#define DEVID_SECRET_MAX    (64u)

/* 两个域分隔标签：同一个密钥既算 ID 又算证明时，输入不分离会让两类输出
 * 之间存在可推导关系。标签进哈希，长度写死（编译期常量，不 strlen）。 */
static const char DEVID_ID_TAG[]    = "ark-devid-v1";
static const char DEVID_PROOF_TAG[] = "ark-devid-proof-v1";
#define DEVID_ID_TAG_LEN     (12u)
#define DEVID_PROOF_TAG_LEN  (18u)

/* HMAC 内层一次性拼在栈上：ipad 64 + 标签 18 + 挑战 256 = 338 字节，
 * 取 352 对齐。调用者（shell / 认证路径）的栈至少 8 KB，这一块不会成为
 * 水位最高的地方；换成流式两遍哈希只是省 300 字节，却多一个必须对齐的
 * 临时状态，不值得。 */
#define DEVID_HMAC_INNER_MAX (64u + DEVID_PROOF_TAG_LEN + ARKRTOS_DEVID_CHALLENGE_MAX + 16u)

static arkrtos_devid_status_t g_status;

/* 板级没实现时的兜底：明确回答"没有这条路"，而不是给一个零密钥/零 ID。
 * 零密钥会让"证明"变成人人可算的值 —— 那是最糟的假绿：看起来有身份。
 *
 * MinGW(PE) 上同一个 TU 里出现两个 __attribute__((weak)) 定义时，两个弱别名
 * 会在链接期错位合并（实测调用被解析到另一个钩子的函数体中间，直接执行到
 * 非法指令），所以每个兜底各开独立 section。与 arkrtos_trust.c 同因同治。 */
#if defined(_WIN32) && defined(__GNUC__)
#define DEVID_HOOK_WEAK(sec) __attribute__((weak, section(".text.devid_hook_" sec)))
#else
#define DEVID_HOOK_WEAK(sec) __attribute__((weak))
#endif

DEVID_HOOK_WEAK("uid") int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    (void)p_out;
    (void)cap;
    (void)p_len;
    return -1;
}

DEVID_HOOK_WEAK("secret") int32 arkrtos_port_devid_secret(uint8 *p_out, uint32 cap)
{
    (void)p_out;
    (void)cap;
    return -1;
}

/* 编进固件的那份密钥。它**不是身份**：同一份构建出来的每一台设备都长一样，
 * 换固件就能换掉它。存在的理由是让证明路径在模拟器与开发板上能被真正跑一遍，
 * 而不是让"没有 OTP 的板子"看起来像有身份。所以来源必须如实报 BUILTIN。 */
#if defined(ARKRTOS_DEVICE_SECRET)
static const uint8 g_builtin_secret[] = ARKRTOS_DEVICE_SECRET;
#endif

static void devid_zero(uint8 *p, uint32 n)
{
    uint32 i;

    for(i = 0u; i < n; i++)
    {
        p[i] = 0u;
    }
}

/* HMAC-SHA256（RFC 2104 口径，密钥上限 64 字节）。密钥右侧补 0 到块长；
 * 超过块长的密钥由调用方在上层拒绝，不在这里先哈希一次。 */
static void devid_hmac(const uint8 *p_key, uint32 klen,
                       const uint8 *p_msg1, uint32 len1,
                       const uint8 *p_msg2, uint32 len2,
                       uint8 out[32])
{
    uint8  blk[DEVID_HMAC_INNER_MAX];
    uint8  key[64];
    uint8  inner[32];
    uint8  outer[96];
    uint32 i;

    devid_zero(key, 64u);
    /* 密钥长度两个调用点都 <= 64：boot() 与 prove() 都把 sizeof(secret)=64 交给
     * 板级钩子，读回更长就回 ERR_RANGE，内建密钥也先夹到 64。所以 RFC 2104 里
     * "长于块长先哈希一次"那条分支永远走不到，删掉；真出现长密钥应当由上层
     * 拒绝（报配置错），而不是在这里悄悄改写它。 */
    for(i = 0u; i < klen; i++)
    {
        key[i] = p_key[i];
    }

    /* inner = SHA256((key ^ ipad) || msg1 || msg2) */
    for(i = 0u; i < 64u; i++)
    {
        blk[i] = (uint8)(key[i] ^ 0x36u);
    }
    for(i = 0u; i < len1; i++)
    {
        blk[64u + i] = p_msg1[i];
    }
    for(i = 0u; i < len2; i++)
    {
        blk[64u + len1 + i] = p_msg2[i];
    }
    arkrtos_sha256_hash(blk, 64u + len1 + len2, inner);

    /* outer = SHA256((key ^ opad) || inner) */
    for(i = 0u; i < 64u; i++)
    {
        outer[i] = (uint8)(key[i] ^ 0x5cu);
    }
    for(i = 0u; i < 32u; i++)
    {
        outer[64u + i] = inner[i];
    }
    arkrtos_sha256_hash(outer, 96u, out);

    /* 栈上留过 ipad/opad 与密钥：擦掉不是对抗"能读内存的人"（那要硬件），
     * 只是不让一次崩溃转储顺手带走一份可直接复用的副本。 */
    devid_zero(blk, sizeof(blk));
    devid_zero(key, sizeof(key));
    devid_zero(inner, sizeof(inner));
    devid_zero(outer, sizeof(outer));
}

/* 公开 ID = SHA256("ark-devid-v1" || uid) 的前 ARKRTOS_DEVICE_ID_LEN 字节。
 * 取前缀而不是整份 32 字节：ID 是给人看、给协议当名字用的，长度由配置定。 */
static void devid_derive_id(const uint8 *p_uid, uint32 ulen, uint8 out[32])
{
    arkrtos_sha256_t ctx;

    arkrtos_sha256_init(&ctx);
    arkrtos_sha256_update(&ctx, (const uint8 *)DEVID_ID_TAG, DEVID_ID_TAG_LEN);
    arkrtos_sha256_update(&ctx, p_uid, ulen);
    arkrtos_sha256_final(&ctx, out);
}

static void devid_status_clear(void)
{
    g_status.source     = (uint32)ARKRTOS_DEVID_SRC_NONE;
    g_status.ready      = 0u;
    g_status.uid_len    = 0u;
    g_status.secret_len = 0u;
    g_status.id_len     = 0u;
    devid_zero(g_status.id, (uint32)sizeof(g_status.id));
}

int32 arkrtos_devid_boot(void)
{
    uint8  uid[DEVID_UID_MAX];
    uint8  secret[DEVID_SECRET_MAX];
    uint8  digest[32];
    uint32 ulen = 0u;
    int32  rc;

    devid_status_clear();
    devid_zero(uid, (uint32)sizeof(uid));
    devid_zero(secret, (uint32)sizeof(secret));

    /* 1. 芯片唯一号。没有它就没有名字，整条身份都不成立 —— 但这不是错误，
     *    只是"这块板没接线"，所以不锁任何东西、也不报 PASS。 */
    rc = arkrtos_port_devid_uid(uid, (uint32)sizeof(uid), &ulen);
    if((rc == 0) && (ulen > 0u))
    {
        if(ulen > (uint32)sizeof(uid))
        {
            /* 板级说写进来的比缓冲区大：宁可不认，也不截断成一个可能撞车的名字。 */
            devid_status_clear();
            devid_zero(uid, (uint32)sizeof(uid));
            return ARKRTOS_DEVID_ERR_RANGE;
        }

        g_status.uid_len = ulen;
        devid_derive_id(uid, ulen, digest);
        /* features.h 用 #error 把 ARKRTOS_DEVICE_ID_LEN 钉在 8..32，所以这里
         * 不需要再夹一次；多一条恒不成立的判断只会让读者以为配置可能超限。 */
        g_status.id_len = (uint32)ARKRTOS_DEVICE_ID_LEN;
        {
            uint32 i;

            for(i = 0u; i < g_status.id_len; i++)
            {
                g_status.id[i] = digest[i];
            }
        }
        g_status.source = (uint32)ARKRTOS_DEVID_SRC_UID_ONLY;
    }

    /* 没有名字就没有身份：即使密钥读得出来，也不往下走。否则会出现
     * "report 说 OTP + ready、id_len 却是 0"的假绿——一块答得出挑战却叫不出
     * 自己名字的设备，不构成一个可以拿去对账的身份。这里清干净再回 ERR_STATE，
     * 让 prove() 也一起失败关闭。 */
    if(g_status.id_len == 0u)
    {
        devid_status_clear();
        devid_zero(uid, (uint32)sizeof(uid));
        devid_zero(secret, (uint32)sizeof(secret));
        devid_zero(digest, (uint32)sizeof(digest));
        return ARKRTOS_DEVID_ERR_STATE;
    }

    /* 2. 每机密钥：板级优先。读得出来才叫身份；读不出来就退到编进固件的那份，
     *    来源如实写 BUILTIN（它不是身份，只是让证明路径能被跑起来）。 */
    rc = arkrtos_port_devid_secret(secret, (uint32)sizeof(secret));
    if((rc > 0) && ((uint32)rc <= (uint32)sizeof(secret)))
    {
        g_status.secret_len = (uint32)rc;
        g_status.source     = (uint32)ARKRTOS_DEVID_SRC_OTP;
        /* 短于 DEVID_SECRET_MIN 的"密钥"挡不住穷举：来源照样如实报 OTP，
         * 但不给 ready —— 让报告读出「这块板的密钥槽是配错的」，而不是假装
         * 身份成立，也不是把它混进"没有槽"那一档。 */
        g_status.ready = ((uint32)rc >= DEVID_SECRET_MIN) ? 1u : 0u;
    }
    else if(rc > (int32)sizeof(secret))
    {
        devid_zero(secret, (uint32)sizeof(secret));
        devid_status_clear();
        return ARKRTOS_DEVID_ERR_RANGE;
    }
    else
    {
#if defined(ARKRTOS_DEVICE_SECRET)
        uint32 i;
        uint32 n = (uint32)(sizeof(g_builtin_secret) / sizeof(g_builtin_secret[0]));

        if(n > (uint32)sizeof(secret))
        {
            n = (uint32)sizeof(secret);
        }
        for(i = 0u; i < n; i++)
        {
            secret[i] = g_builtin_secret[i];
        }
        if((n >= DEVID_SECRET_MIN) && (n <= DEVID_SECRET_MAX))
        {
            g_status.secret_len = n;
            g_status.ready      = 1u;
            /* 有芯片 ID 时不算"纯内建"：名字是真的，只有证明的钥匙是共用的。
             * 但来源只报一件最重要的事 —— 这把钥匙不是这一台独有的，所以
             * 报 BUILTIN，让报告层面不会把它读成"身份成立"。 */
            g_status.source = (uint32)ARKRTOS_DEVID_SRC_BUILTIN;
        }
#endif
    }

    devid_zero(secret, (uint32)sizeof(secret));
    devid_zero(uid, (uint32)sizeof(uid));
    devid_zero(digest, (uint32)sizeof(digest));

    return ARKRTOS_DEVID_OK;
}

uint32 arkrtos_devid_ready(void)
{
    return (g_status.ready != 0u) ? 1u : 0u;
}

int32 arkrtos_devid_id(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    uint32 i;

    if((p_out == 0) || (p_len == 0))
    {
        return ARKRTOS_DEVID_ERR_PARAM;
    }
    if(g_status.id_len == 0u)
    {
        return ARKRTOS_DEVID_ERR_STATE;
    }
    if(cap < g_status.id_len)
    {
        return ARKRTOS_DEVID_ERR_RANGE;
    }

    for(i = 0u; i < g_status.id_len; i++)
    {
        p_out[i] = g_status.id[i];
    }
    *p_len = g_status.id_len;

    return ARKRTOS_DEVID_OK;
}

int32 arkrtos_devid_prove(const uint8 *p_challenge, uint32 len, uint8 mac[ARKRTOS_DEVID_MAC_LEN])
{
    uint8  secret[DEVID_SECRET_MAX];
    uint32 slen;
    int32  rc;

    if((p_challenge == 0) || (mac == 0))
    {
        return ARKRTOS_DEVID_ERR_PARAM;
    }
    if((len == 0u) || (len > (uint32)ARKRTOS_DEVID_CHALLENGE_MAX))
    {
        return ARKRTOS_DEVID_ERR_PARAM;
    }
    /* 失败关闭：没有每机密钥就不给"证明"。返回一个全 0 的 MAC 等于告诉对方
     * "答案是 0"，那是把"没有身份"伪装成"身份是 0"。 */
    if(g_status.ready == 0u)
    {
        return ARKRTOS_DEVID_ERR_NOKEY;
    }

    /* 密钥每次现取：常驻一份明文密钥等于把它多暴露一倍时间，而这条路径
     * 一次认证只走一次，读一次 OTP 副本的代价可以接受。 */
    devid_zero(secret, (uint32)sizeof(secret));
    rc = arkrtos_port_devid_secret(secret, (uint32)sizeof(secret));
    if((rc > 0) && ((uint32)rc <= (uint32)sizeof(secret)))
    {
        slen = (uint32)rc;
    }
    else
    {
#if defined(ARKRTOS_DEVICE_SECRET)
        uint32 i;
        uint32 n = (uint32)(sizeof(g_builtin_secret) / sizeof(g_builtin_secret[0]));

        if(n > (uint32)sizeof(secret))
        {
            n = (uint32)sizeof(secret);
        }
        for(i = 0u; i < n; i++)
        {
            secret[i] = g_builtin_secret[i];
        }
        slen = n;
#else
        devid_zero(secret, (uint32)sizeof(secret));
        return ARKRTOS_DEVID_ERR_NOKEY;
#endif
    }

    devid_hmac(secret, slen,
               (const uint8 *)DEVID_PROOF_TAG, DEVID_PROOF_TAG_LEN,
               p_challenge, len, mac);

    devid_zero(secret, (uint32)sizeof(secret));

    return ARKRTOS_DEVID_OK;
}

int32 arkrtos_devid_check(const uint8 *p_challenge, uint32 len,
                          const uint8 mac[ARKRTOS_DEVID_MAC_LEN])
{
    uint8  want[ARKRTOS_DEVID_MAC_LEN];
    uint8  diff = 0u;
    uint32 i;
    int32  rc;

    if(mac == 0)
    {
        return ARKRTOS_DEVID_ERR_PARAM;
    }

    devid_zero(want, (uint32)sizeof(want));
    rc = arkrtos_devid_prove(p_challenge, len, want);
    if(rc != ARKRTOS_DEVID_OK)
    {
        return rc;
    }

    /* 常数时间比较：不提前 return。提前返回会告诉伪造者"前几个字节对了"。 */
    for(i = 0u; i < (uint32)ARKRTOS_DEVID_MAC_LEN; i++)
    {
        diff |= (uint8)(want[i] ^ mac[i]);
    }
    devid_zero(want, (uint32)sizeof(want));

    return (diff == 0u) ? ARKRTOS_DEVID_OK : ARKRTOS_DEVID_ERR_MISMATCH;
}

int32 arkrtos_devid_status_get(arkrtos_devid_status_t *p_out)
{
    uint32 i;

    if(p_out == 0)
    {
        return ARKRTOS_DEVID_ERR_PARAM;
    }

    p_out->source     = g_status.source;
    p_out->ready      = g_status.ready;
    p_out->uid_len    = g_status.uid_len;
    p_out->secret_len = g_status.secret_len;
    p_out->id_len     = g_status.id_len;
    for(i = 0u; i < (uint32)sizeof(p_out->id); i++)
    {
        p_out->id[i] = g_status.id[i];
    }

    return ARKRTOS_DEVID_OK;
}

const char *arkrtos_devid_src_name(uint32 source)
{
    switch(source)
    {
        case ARKRTOS_DEVID_SRC_UID_ONLY: return "uid-only";
        case ARKRTOS_DEVID_SRC_OTP:      return "otp";
        case ARKRTOS_DEVID_SRC_BUILTIN:  return "builtin";
        default:                         return "none";
    }
}

#endif /* ARKRTOS_USE_DEVICE_ID */
