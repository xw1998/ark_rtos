/**
* @file arkrtos_trust.c
* @brief Trust root for the image verification key (SEC-G12)
* @details See arkrtos_trust.h for the claim and its limits. The whole file is
*          compiled out when ARKRTOS_USE_TRUST_ROOT is 0, so the default build
*          keeps the accept path byte for byte.
*/

#include "arkrtos_types.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_TRUST_ROOT == 1)

#include "arkrtos_trust.h"
#include "arkrtos_sha256.h"
#include "arkrtos_hal.h"

/* 32 字节：Ed25519 公钥的长度。本模块只认这个档，因为只有非对称档的"公钥"
 * 才是可以公开暴露、又能被钉存的东西；HMAC 档的密钥是秘密，钉它的摘要不解决
 * "密钥从哪来"的问题（换固件仍可换密钥）。 */
#define TRUST_KEY_LEN   (32u)

static arkrtos_trust_status_t g_status;
static uint32                 g_locked;      /* 1 = 钉存摘要与本机公钥不一致 */

/* 板级没实现时的兜底：明确回答"没有这条路"，而不是给一个零密钥。
 * 零密钥会让摘要变成一个确定的、看起来"验过了"的值——那是最糟的假绿。
 *
 * MinGW(PE) 上同一个 TU 里出现两个 __attribute__((weak)) 定义时，两个弱别名
 * (.weak.<sym>.<containing>) 会在链接期错位合并 —— 实测调用被解析到另一个钩子
 * 的函数体中间，直接执行到非法指令（宿主用例 0xC0000096）。给每个兜底各开
 * 独立 section 可规避，这与 arkrtos_selftest.c 里那对钩子用同一个办法；
 * 非 MinGW 工具链保持原有的弱符号写法。 */
#if defined(_WIN32) && defined(__GNUC__)
#define TRUST_HOOK_WEAK(sec) __attribute__((weak, section(".text.trust_hook_" sec)))
#else
#define TRUST_HOOK_WEAK(sec) __attribute__((weak))
#endif

TRUST_HOOK_WEAK("pubkey") int32 arkrtos_port_trust_pubkey(const uint8 **pp_key, uint32 *p_len)
{
    (void)pp_key;
    (void)p_len;
    return -1;
}

TRUST_HOOK_WEAK("pin") int32 arkrtos_port_trust_pin(uint8 *p_digest, uint32 write)
{
    (void)p_digest;
    (void)write;
    return -1;
}

/* 编进固件的那份公钥。它不是信任根：换固件就能换它，这是本模块存在的理由，
 * 不是它的答案。所以用它的时候状态里 source 明确写成 BUILTIN。
 * 不需要 #if 守着：features.h 把 TRUST_ROOT 钉在 IMAGE_SIGN_ED25519 上，
 * 而后者已经要求 ARKRTOS_IMAGE_SIGN_PUBKEY 必须定义 —— 这一档永远有公钥，
 * "没有公钥" 不是一个可能的状态（也就不该有一段永远走不到的代码）。 */
static const uint8 g_builtin_key[TRUST_KEY_LEN] = ARKRTOS_IMAGE_SIGN_PUBKEY;

/* 空白槽：全 0x00 或全 0xFF。别的值一律当成"已经钉过的东西"，
 * 不能猜它是垃圾还是半个写坏的公钥——写坏的公钥只能由产线重刷，不能由这里
 * 静默覆盖。 */
static int32 trust_is_blank(const uint8 *p)
{
    uint32 i;
    uint8  zero = 1u;
    uint8  ones = 1u;

    for(i = 0u; i < TRUST_KEY_LEN; i++)
    {
        if(p[i] != 0x00u)
        {
            zero = 0u;
        }
        if(p[i] != 0xFFu)
        {
            ones = 0u;
        }
    }

    return ((zero != 0u) || (ones != 0u)) ? 1 : 0;
}

static void trust_digest(const uint8 *p_key, uint32 len, uint8 out[32])
{
    arkrtos_sha256_t c;

    arkrtos_sha256_init(&c);
    arkrtos_sha256_update(&c, p_key, len);
    arkrtos_sha256_final(&c, out);
}

int32 arkrtos_trust_boot(void)
{
    const uint8  *p_key = 0;
    uint32        len = 0u;
    uint8         pinned[TRUST_KEY_LEN];
    int32         rc;

    g_status.source  = ARKRTOS_TRUST_SRC_NONE;
    g_status.pin     = ARKRTOS_TRUST_PIN_NONE;
    g_status.key_len = 0u;
    {
        uint32 i;

        for(i = 0u; i < 32u; i++)
        {
            g_status.digest[i] = 0u;
        }
    }

    /* 1. 取钥：板级优先。板级给得出来才叫根，固件里那份只是"已知公钥"。 */
    rc = arkrtos_port_trust_pubkey(&p_key, &len);
    if((rc == 0) && (p_key != 0) && (len == TRUST_KEY_LEN))
    {
        g_status.source = ARKRTOS_TRUST_SRC_OTP;
    }
    else
    {
        /* 板级没有 OTP 公钥槽：退回编进固件的那份。来源报 BUILTIN，
         * 它验得了签名，但验不了"这把钥是谁放进来的"。 */
        p_key = g_builtin_key;
        len   = TRUST_KEY_LEN;
        g_status.source = ARKRTOS_TRUST_SRC_BUILTIN;
    }

    g_status.key_len = len;
    trust_digest(p_key, len, g_status.digest);

    /* 2. 读钉存槽。读不到 = 这块片子没有一次性存储（或板级没接线）。
     * 这时不锁、也不报 PASS：状态是 UNAVAILABLE，sec 那边报 SKIP。 */
    {
        uint32 i;

        for(i = 0u; i < TRUST_KEY_LEN; i++)
        {
            pinned[i] = 0u;
        }
    }

    if(arkrtos_port_trust_pin(pinned, 0u) != 0)
    {
        g_status.pin = ARKRTOS_TRUST_PIN_UNAVAILABLE;
        return ARKRTOS_TRUST_OK;
    }

    if(trust_is_blank(pinned) != 0)
    {
        /* 3. 空白槽：首次开机把摘要写进去（TOFU）。写不进去要如实报错——
         *    报成 "pinned" 等于宣称一个不存在的钉存。 */
        uint32 i;

        for(i = 0u; i < TRUST_KEY_LEN; i++)
        {
            pinned[i] = g_status.digest[i];
        }

        if(arkrtos_port_trust_pin(pinned, 1u) != 0)
        {
            g_status.pin = ARKRTOS_TRUST_PIN_WRITEFAIL;
            return ARKRTOS_TRUST_ERR_PIN;
        }

        g_status.pin = ARKRTOS_TRUST_PIN_NEW;
        return ARKRTOS_TRUST_OK;
    }

    /* 4. 已经有摘要：必须逐字节相同。不一致锁存，且**不覆盖**槽里的值——
     *    覆盖等于让一次换钥把钉子拔了再钉回去。 */
    {
        uint32 i;
        uint8  diff = 0u;

        for(i = 0u; i < TRUST_KEY_LEN; i++)
        {
            diff |= (uint8)(pinned[i] ^ g_status.digest[i]);
        }

        if(diff != 0u)
        {
            g_status.pin = ARKRTOS_TRUST_PIN_MISMATCH;
            g_locked = 1u;
            return ARKRTOS_TRUST_ERR_MISMATCH;
        }
    }

    g_status.pin = ARKRTOS_TRUST_PIN_MATCH;
    return ARKRTOS_TRUST_OK;
}

uint32 arkrtos_trust_locked(void)
{
    return g_locked;
}

int32 arkrtos_trust_status_get(arkrtos_trust_status_t *p_out)
{
    uint32 i;

    if(p_out == 0)
    {
        return ARKRTOS_TRUST_ERR_PARAM;
    }

    p_out->source  = g_status.source;
    p_out->pin     = g_status.pin;
    p_out->key_len = g_status.key_len;
    for(i = 0u; i < 32u; i++)
    {
        p_out->digest[i] = g_status.digest[i];
    }

    return ARKRTOS_TRUST_OK;
}

const char *arkrtos_trust_src_name(uint32 source)
{
    switch(source)
    {
        case ARKRTOS_TRUST_SRC_OTP:     return "otp";
        case ARKRTOS_TRUST_SRC_BUILTIN: return "builtin";
        default:                        return "none";
    }
}

const char *arkrtos_trust_pin_name(uint32 pin)
{
    switch(pin)
    {
        case ARKRTOS_TRUST_PIN_NEW:         return "pinned-now";
        case ARKRTOS_TRUST_PIN_MATCH:       return "match";
        case ARKRTOS_TRUST_PIN_MISMATCH:    return "mismatch";
        case ARKRTOS_TRUST_PIN_UNAVAILABLE: return "unavailable";
        case ARKRTOS_TRUST_PIN_WRITEFAIL:   return "write-failed";
        default:                            return "none";
    }
}

#endif /* ARKRTOS_USE_TRUST_ROOT */
