/**
* @file test_bus_crypto_host.c
* @brief 软总线加密档的宿主用例：原语向量 + 双节点端到端
* @details SEC-G06 的证据件。被验的不是"加密算法对不对"（那是组件的
*          责任，且已经在组件自己的用例里对过 RFC 向量），而是这三件事：
*
*            1. 原语仍然是出版向量：FIPS 180-4 的 SHA-256、RFC 4231 的
*               HMAC-SHA256、RFC 8439 的 ChaCha20。档位把开关拨到 1 之后
*               这些实现必须还是原来那份，不能因为接线而改坏；
*            2. 装了钥的节点发出去的帧**线上看不到明文**（加密档），标签
*               覆盖载荷与帧头，篡改任一字节都被拒（E_AUTH，计
*               stats.rx_auth_fail），且回调一次都不会被调用；
*            3. fail-closed 双向成立：装钥的拒收无标签帧，未装钥的拒收带
*               标签帧。错钥、重放（含改号重放）同理被拒，重放计
*               stats.rx_replay。
*
*          传输是板内 loopback，但用例不调用 ark_bus_poll：它把帧直接从环形
*          缓冲里取出来（这正是"线上"的字节），先看字节、再手工喂给对端。
*          这样"线上是什么"与"对端怎么判"是两次独立的读数，不会因为 poll
*          一次走完而互相掩盖。
*
*          明确没验证：真实串口/以太网上的窃听、时序侧信道、密钥分发本身、
*          多节点并发。那些只能上板或属于别的层。
*
* @author xw
* @date 2026.11.23
*/
#include <stdio.h>
#include <string.h>

#include "ark_bus.h"
#include "ark_bus_crypto.h"
#include "ark_bus_chacha20.h"
#include "ark_bus_hal.h"
#include "ark_bus_transport.h"

#if !ARK_BUS_CRYPTO_ENABLED

/* 关档：组件里这条路径整段编掉（ark_bus_crypto.c 是空翻译单元）。用例只
 * 证明"关档时仍可编可跑"；真正的断言在门禁里——关档目标不许剩 crypto
 * 符号，否则就是"开关拨到 0 了、代码还在里面"。 */
int main(void)
{
    printf("BUS-CRYPTO-HOST OK  off\n");
    return 0;
}

#else

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* 夹具：两个节点 + 板内 loopback + 手工时钟                             */
/* ------------------------------------------------------------------ */

static ark_bus_loopback_t g_lb;
static uint8_t            g_storage[2 * 1024];
static ark_bus_t          g_a;
static ark_bus_t          g_b;

static uint32_t g_now;

static uint32_t hal_tick(void)
{
    return g_now;
}

static const ark_bus_hal_t g_hal = { hal_tick, NULL, NULL, NULL };

static int      g_cb_count;
static uint8_t  g_cb_payload[64];
static uint16_t g_cb_len;

static void on_msg(const char *topic, const uint8_t *payload, uint16_t len,
                   void *user)
{
    (void)topic;
    (void)user;
    g_cb_count++;
    g_cb_len = len;
    if (len > (uint16_t)sizeof(g_cb_payload)) {
        len = (uint16_t)sizeof(g_cb_payload);
    }
    memcpy(g_cb_payload, payload, len);
}

static void reset_pair(void)
{
    ark_bus_hal_set(&g_hal);
    g_now = 1000u;
    g_cb_count = 0;
    g_cb_len   = 0;
    memset(g_storage, 0, sizeof(g_storage));
    ark_bus_loopback_init(&g_lb, g_storage,
                          (uint16_t)(sizeof(g_storage) / 2u));
    ark_bus_init(&g_a, ark_bus_loopback_a(&g_lb));
    ark_bus_init(&g_b, ark_bus_loopback_b(&g_lb));

    /* 必须 start：重放窗口按 src_id 分槽，而 src_id 来自 self_id——没 start
     * 的节点发出去的帧 src_id 是 0，窗口会整段跳过（"没有身份可记"）。发现
     * 帧先落进两个环，随后整段丢掉，用例只看被测帧。 */
    ark_bus_start(&g_a, 0x00001101u, 1u, 0x01u);
    ark_bus_start(&g_b, 0x00001102u, 2u, 0x02u);
    g_lb.a2b.tail = g_lb.a2b.head;
    g_lb.b2a.tail = g_lb.b2a.head;
}

/* 把 A->B 环上的字节整段取出（"线上的一帧"），并清空环。 */
static uint16_t wire_take(uint8_t *out, uint16_t max)
{
    ark_bus_ring_t *r = &g_lb.a2b;
    uint16_t        n = 0;

    while (r->tail != r->head && n < max) {
        out[n] = r->buf[r->tail];
        n++;
        r->tail = (uint16_t)((r->tail + 1u) % r->cap);
    }
    return n;
}

/* 帧里载荷长度（帧头偏移 9..10，大端）。 */
static uint16_t frame_plen(const uint8_t *f)
{
    return (uint16_t)(((uint16_t)f[9] << 8) | (uint16_t)f[10]);
}

/* 改完载荷/帧头之后重算 CRC——攻击者当然会这么做，取证也必须这么做，否则
 * 被拒的理由会变成 CRC 错，而不是"标签校验失败"，那就没验到该验的地方。 */
static void refix_crc(uint8_t *f)
{
    uint16_t total = ark_bus_frame_size(frame_plen(f));
    uint16_t crc   = ark_bus_crc16(f, (uint16_t)(total - ARK_BUS_CRC_LEN));

    f[total - 2u] = (uint8_t)(crc >> 8);
    f[total - 1u] = (uint8_t)(crc & 0xFFu);
}

static int wire_has(const uint8_t *buf, uint16_t len, const char *needle)
{
    uint16_t n = (uint16_t)strlen(needle);
    uint16_t i;

    if (n == 0u || len < n) {
        return 0;
    }
    for (i = 0; i <= (uint16_t)(len - n); i++) {
        if (memcmp(buf + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int bytes_eq_hex(const uint8_t *got, const char *hex, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return 0;
        }
        if (got[i] != (uint8_t)v) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 1. 原语向量：开关拨到 1 之后，实现必须还是出版的那份                   */
/* ------------------------------------------------------------------ */

static void test_sha256_vectors(void)
{
    static uint8_t million[1000000];
    uint8_t d[32];
    const char *longmsg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    int i;

    ark_bus_sha256((const uint8_t *)"", 0u, d);
    CK(bytes_eq_hex(d,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32));

    ark_bus_sha256((const uint8_t *)"abc", 3u, d);
    CK(bytes_eq_hex(d,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32));

    /* 56 字节：单块填充的边界，补齐 0x80 与长度域正好跨块。 */
    ark_bus_sha256((const uint8_t *)longmsg, 56u, d);
    CK(bytes_eq_hex(d,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32));

    ark_bus_sha256((const uint8_t *)
        "The quick brown fox jumps over the lazy dog", 43u, d);
    CK(bytes_eq_hex(d,
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592", 32));

    /* 多块路径：100 万个 'a'。压缩函数每 64 字节一轮，短向量碰不到。 */
    for (i = 0; i < (int)sizeof(million); i++) {
        million[i] = (uint8_t)'a';
    }
    ark_bus_sha256(million, (uint32_t)sizeof(million), d);
    CK(bytes_eq_hex(d,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32));
}

static void test_hmac_vectors(void)
{
    uint8_t d[32];
    uint8_t k1[20];
    uint8_t k3[20];
    uint8_t dd[50];
    uint8_t longkey[131];
    uint8_t khashed[32];
    uint8_t a[32];
    uint8_t b[32];

    /* RFC 4231 case 1：20 x 0x0b，"Hi There"。 */
    memset(k1, 0x0b, sizeof(k1));
    ark_bus_hmac_sha256(k1, (uint16_t)sizeof(k1),
                        (const uint8_t *)"Hi There", 8u, d);
    CK(bytes_eq_hex(d,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32));

    /* RFC 4231 case 2：短于分组的钥。 */
    ark_bus_hmac_sha256((const uint8_t *)"Jefe", 4u,
                        (const uint8_t *)"what do ya want for nothing?", 28u, d);
    CK(bytes_eq_hex(d,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32));

    /* RFC 4231 case 3：20 x 0xaa 钥，50 x 0xdd 数据。 */
    memset(k3, 0xaa, sizeof(k3));
    memset(dd, 0xdd, sizeof(dd));
    ark_bus_hmac_sha256(k3, (uint16_t)sizeof(k3), dd, (uint32_t)sizeof(dd), d);
    CK(bytes_eq_hex(d,
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32));

    /* 超过 64 字节的钥要先哈希（RFC 2104）。短钥向量碰不到这条分支，而
     * 32 字节的预共享钥正好在边界上——这条分支错了，长钥部署会静默失配。 */
    memset(longkey, 0xaa, sizeof(longkey));
    ark_bus_sha256(longkey, (uint32_t)sizeof(longkey), khashed);
    ark_bus_hmac_sha256(longkey, (uint16_t)sizeof(longkey),
                        (const uint8_t *)"Test Using Larger Than Block-Size Key",
                        40u, a);
    ark_bus_hmac_sha256(khashed, (uint16_t)sizeof(khashed),
                        (const uint8_t *)"Test Using Larger Than Block-Size Key",
                        40u, b);
    CK(memcmp(a, b, 32) == 0);
}

#if ARK_BUS_ENCRYPT_ENABLED
static void test_chacha20_vectors(void)
{
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t out[64];
    int i;

    for (i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }

    /* RFC 8439 2.3.2：nonce 00:00:00:09:00:00:00:4a:00:00:00:00，counter 1。 */
    memset(nonce, 0, sizeof(nonce));
    nonce[3] = 0x09u;
    nonce[7] = 0x4au;
    ark_bus_chacha20_block(key, 1u, nonce, out);
    CK(bytes_eq_hex(out,
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64));

    /* RFC 8439 2.4.2：同一段的流密码，加密再解密必须回到明文。 */
    memset(nonce, 0, sizeof(nonce));
    nonce[7] = 0x4au;
    {
        static const char pt[] =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        uint8_t ct[128];
        uint8_t back[128];
        uint32_t n = (uint32_t)(sizeof(pt) - 1u);

        CK(n == 114u);
        ark_bus_chacha20_xor(key, 1u, nonce, (const uint8_t *)pt, ct, n);
        CK(bytes_eq_hex(ct,
            "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
            "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
            "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
            "5af90bbf74a35be6b40b8eedf2785e42874d", 114));
        ark_bus_chacha20_xor(key, 1u, nonce, ct, back, n);
        CK(memcmp(back, pt, n) == 0);
    }
}
#endif /* ARK_BUS_ENCRYPT_ENABLED */

/* ------------------------------------------------------------------ */
/* 2. 密钥管理：入参边界与状态位                                           */
/* ------------------------------------------------------------------ */

static void test_key_management(void)
{
    uint8_t key[ARK_BUS_CRYPTO_KEY_MAX + 1u];
    int i;

    for (i = 0; i < (int)sizeof(key); i++) {
        key[i] = (uint8_t)(0x10 + i);
    }

    reset_pair();

    CK(ark_bus_crypto_key_set(&g_a) == 0);
    CK(ark_bus_crypto_set_key(NULL, key, 16u) == ARK_BUS_E_INVAL);
    CK(ark_bus_crypto_set_key(&g_a, NULL, 16u) == ARK_BUS_E_INVAL);
    CK(ark_bus_crypto_set_key(&g_a, key, 0u) == ARK_BUS_E_INVAL);
    CK(ark_bus_crypto_set_key(&g_a, key, (uint16_t)(ARK_BUS_CRYPTO_KEY_MAX + 1u))
       == ARK_BUS_E_INVAL);
    CK(ark_bus_crypto_key_set(&g_a) == 0);      /* 失败的调用没留下状态 */

    CK(ark_bus_crypto_set_key(&g_a, key, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_key_set(&g_a) == 1);

    /* 换钥覆盖旧钥：先装一把、再装另一把，状态仍是"有钥"。 */
    CK(ark_bus_crypto_set_key(&g_a, key + 1, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_key_set(&g_a) == 1);

    ark_bus_crypto_clear_key(&g_a);
    CK(ark_bus_crypto_key_set(&g_a) == 0);
    ark_bus_crypto_clear_key(NULL);             /* 空总线上清钥是 no-op */
}

/* ------------------------------------------------------------------ */
/* 3. 没装钥：行为必须和加加密档之前逐字节一致                              */
/* ------------------------------------------------------------------ */

static void test_plain_unchanged(void)
{
    uint8_t frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;

    reset_pair();
    CK(ark_bus_subscribe(&g_b, "demo/data", on_msg, NULL) > 0);

    CK(ark_bus_publish(&g_a, "demo/data", "plain-text", 10u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));
    CK(n == (uint16_t)(ARK_BUS_HDR_LEN + 10u + ARK_BUS_CRC_LEN));
    CK((frame[4] & ARK_BUS_FLAG_AUTHENTICATED) == 0u);
    CK(wire_has(frame, n, "plain-text"));       /* 没装钥就别假装加密了 */

    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_OK);
    CK(g_cb_count == 1 && g_cb_len == 10u);
    CK(memcmp(g_cb_payload, "plain-text", 10u) == 0);
    CK(g_b.stats.rx_auth_fail == 0u);
}

/* ------------------------------------------------------------------ */
/* 4. 只有发送端装钥：对端必须拒收，而不是把标签当载荷派发                  */
/* ------------------------------------------------------------------ */

static uint8_t g_key[16];
static uint8_t g_other[16];

static void init_keys(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        g_key[i]   = (uint8_t)(0x30 + i);
        g_other[i] = (uint8_t)(0xC0 + i);
    }
}

static void test_sender_keyed_receiver_not(void)
{
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;

    reset_pair();
    init_keys();
    CK(ark_bus_subscribe(&g_b, "sealed/data", on_msg, NULL) > 0);
    CK(ark_bus_crypto_set_key(&g_a, g_key, 16u) == ARK_BUS_OK);

    CK(ark_bus_publish(&g_a, "sealed/data", "secret", 6u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));
    CK(n == (uint16_t)(ARK_BUS_HDR_LEN + 6u + ARK_BUS_CRYPTO_WIRE_OVERHEAD
                       + ARK_BUS_CRC_LEN));
    CK((frame[4] & ARK_BUS_FLAG_AUTHENTICATED) != 0u);

#if ARK_BUS_ENCRYPT_ENABLED
    CK(!wire_has(frame, n, "secret"));          /* 线上就该是密文 */
#else
    CK(wire_has(frame, n, "secret"));           /* 只认证：明文仍在线上 */
#endif

    /* fail-closed：未装钥的节点不认带标签帧。 */
    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_E_AUTH);
    CK(g_b.stats.rx_auth_fail == 1u);
    CK(g_cb_count == 0);
}

/* ------------------------------------------------------------------ */
/* 5. 两端同钥：验签通过，载荷逐字节还原                                   */
/* ------------------------------------------------------------------ */

static void test_both_keyed(void)
{
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;

    reset_pair();
    init_keys();
    CK(ark_bus_subscribe(&g_b, "sealed/data", on_msg, NULL) > 0);
    CK(ark_bus_crypto_set_key(&g_a, g_key, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_set_key(&g_b, g_key, 16u) == ARK_BUS_OK);

    CK(ark_bus_publish(&g_a, "sealed/data", "payload-42", 10u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));

    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_OK);
    CK(g_cb_count == 1 && g_cb_len == 10u);
    CK(memcmp(g_cb_payload, "payload-42", 10u) == 0);
    CK(g_b.stats.rx_auth_fail == 0u);

    /* 同一个明文发两次，线上字节必须不同（加密档的 nonce 计数在走）。 */
#if ARK_BUS_ENCRYPT_ENABLED
    {
        uint8_t  f2[ARK_BUS_FRAME_MAX_SIZE];
        uint16_t n2;

        CK(ark_bus_publish(&g_a, "sealed/data", "payload-42", 10u) > 0);
        n2 = wire_take(f2, (uint16_t)sizeof(f2));
        CK(n2 == n);
        CK(memcmp(f2, frame, n) != 0);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* 6. 篡改：标签、载荷、帧头，各自都要被拒                                 */
/* ------------------------------------------------------------------ */

static void test_tamper(void)
{
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;
    uint16_t plen;
    uint32_t fails;

    reset_pair();
    init_keys();
    CK(ark_bus_subscribe(&g_b, "tamper/data", on_msg, NULL) > 0);
    CK(ark_bus_crypto_set_key(&g_a, g_key, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_set_key(&g_b, g_key, 16u) == ARK_BUS_OK);
    CK(ark_bus_publish(&g_a, "tamper/data", "0123456789", 10u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));
    plen = frame_plen(frame);

    /* 6a. 标签最后一字节。这是"只改认证域"的最小改动：载荷一个字节没动。 */
    {
        uint8_t  copy[ARK_BUS_FRAME_MAX_SIZE];
        uint32_t before = g_b.stats.rx_auth_fail;

        memcpy(copy, frame, n);
        copy[ARK_BUS_HDR_LEN + plen - 1u] ^= 0x01u;
        refix_crc(copy);
        g_cb_count = 0;
        CK(ark_bus_feed(&g_b, copy, n) == ARK_BUS_E_AUTH);
        CK(g_b.stats.rx_auth_fail == before + 1u);
        CK(g_cb_count == 0);
    }

    /* 6b. 载荷首字节（加密档这是密文首字节，非加密档是明文首字节）。 */
    {
        uint8_t copy[ARK_BUS_FRAME_MAX_SIZE];

        memcpy(copy, frame, n);
        copy[ARK_BUS_HDR_LEN] ^= 0xFFu;
        refix_crc(copy);
        g_cb_count = 0;
        CK(ark_bus_feed(&g_b, copy, n) == ARK_BUS_E_AUTH);
        CK(g_cb_count == 0);
    }

    /* 6c. 帧头里的序列号。重放窗口开着时 seq 进 MAC（否则攻击者可以改号绕
     *     过窗口），所以改号必被拒；窗口关着时 seq 本就不是安全相关的量，
     *     组件有意不认证它——这里把两条设计都钉死，而不是只测其中一条。 */
    {
        uint8_t copy[ARK_BUS_FRAME_MAX_SIZE];

        memcpy(copy, frame, n);
        copy[8] ^= 0x01u;                        /* seq 低字节 */
        refix_crc(copy);
        g_cb_count = 0;
#if ARK_BUS_REPLAY_ENABLED
        CK(ark_bus_feed(&g_b, copy, n) == ARK_BUS_E_AUTH);
        CK(g_cb_count == 0);
#else
        CK(ark_bus_feed(&g_b, copy, n) == ARK_BUS_OK);
        CK(g_cb_count == 1);
        CK(memcmp(g_cb_payload, "0123456789", 10u) == 0);
#endif
    }

    /* 6d. 原帧仍然可收：上面的拒绝没有把窗口/状态搞坏。 */
    fails = g_b.stats.rx_auth_fail;
    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_OK);
    CK(g_cb_count == 1);
    CK(g_b.stats.rx_auth_fail == fails);
}

/* ------------------------------------------------------------------ */
/* 7. 错钥：两端都在装钥，但装的不是同一把                                 */
/* ------------------------------------------------------------------ */

static void test_wrong_key(void)
{
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;

    reset_pair();
    init_keys();
    CK(ark_bus_subscribe(&g_b, "wrong/data", on_msg, NULL) > 0);
    CK(ark_bus_crypto_set_key(&g_a, g_key, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_set_key(&g_b, g_other, 16u) == ARK_BUS_OK);

    CK(ark_bus_publish(&g_a, "wrong/data", "x", 1u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));

    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_E_AUTH);
    CK(g_b.stats.rx_auth_fail == 1u);
    CK(g_cb_count == 0);

    /* 装上正确的那把，同一帧就通了——说明拒绝的原因确实是钥，不是别的。 */
    CK(ark_bus_crypto_set_key(&g_b, g_key, 16u) == ARK_BUS_OK);
    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_OK);
    CK(g_cb_count == 1);
}

/* ------------------------------------------------------------------ */
/* 8. 重放窗口：逐字节相同的帧第二次必须被丢，改号也进不来                   */
/* ------------------------------------------------------------------ */

#if ARK_BUS_REPLAY_ENABLED
static void test_replay(void)
{
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
    uint8_t  next[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t n;
    uint16_t n2;

    reset_pair();
    init_keys();
    CK(ark_bus_subscribe(&g_b, "rp/data", on_msg, NULL) > 0);
    CK(ark_bus_crypto_set_key(&g_a, g_key, 16u) == ARK_BUS_OK);
    CK(ark_bus_crypto_set_key(&g_b, g_key, 16u) == ARK_BUS_OK);

    CK(ark_bus_publish(&g_a, "rp/data", "frame-one", 9u) > 0);
    n = wire_take(frame, (uint16_t)sizeof(frame));

    /* 8a. 第一次：接收并派发。 */
    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_OK);
    CK(g_cb_count == 1);
    CK(g_b.stats.rx_replay == 0u);

    /* 8b. 逐字节重放：丢在派发之前，并计数。 */
    {
        uint32_t rp = g_b.stats.rx_replay;

        g_cb_count = 0;
        CK(ark_bus_feed(&g_b, frame, n) == ARK_BUS_E_STATE);
        CK(g_b.stats.rx_replay == rp + 1u);
        CK(g_cb_count == 0);
    }

    /* 8c. 新帧仍然通：窗口没有卡死。 */
    CK(ark_bus_publish(&g_a, "rp/data", "frame-two", 9u) > 0);
    n2 = wire_take(next, (uint16_t)sizeof(next));
    g_cb_count = 0;
    CK(ark_bus_feed(&g_b, next, n2) == ARK_BUS_OK);
    CK(g_cb_count == 1);
    CK(memcmp(g_cb_payload, "frame-two", 9u) == 0);

    /* 8d. 改号重放：seq 在 MAC 里，改了必被标签拦下（E_AUTH 而不是 E_STATE）。 */
    {
        uint8_t  copy[ARK_BUS_FRAME_MAX_SIZE];
        uint32_t fails = g_b.stats.rx_auth_fail;

        memcpy(copy, frame, n);
        copy[8] ^= 0xFFu;
        refix_crc(copy);
        g_cb_count = 0;
        CK(ark_bus_feed(&g_b, copy, n) == ARK_BUS_E_AUTH);
        CK(g_b.stats.rx_auth_fail == fails + 1u);
        CK(g_cb_count == 0);
    }

    /* 8e. 未装钥的节点不看窗口（没认证就没有可记的东西）。 */
    {
        ark_bus_t *c = &g_a;

        ark_bus_crypto_clear_key(c);
        CK(ark_bus_crypto_key_set(c) == 0);
    }
}
#endif /* ARK_BUS_REPLAY_ENABLED */

/* ------------------------------------------------------------------ */

int main(void)
{
    test_sha256_vectors();
    test_hmac_vectors();
#if ARK_BUS_ENCRYPT_ENABLED
    test_chacha20_vectors();
#endif
    test_key_management();
    test_plain_unchanged();
    test_sender_keyed_receiver_not();
    test_both_keyed();
    test_tamper();
    test_wrong_key();
#if ARK_BUS_REPLAY_ENABLED
    test_replay();
#endif

    if (g_fails != 0) {
        printf("BUS-CRYPTO-HOST FAILED  %d/%d checks\n", g_fails, g_checks);
        return 1;
    }
    printf("BUS-CRYPTO-HOST OK  %d checks "
           "(crypto=%d encrypt=%d replay=%d)\n",
           g_checks, ARK_BUS_CRYPTO_ENABLED, ARK_BUS_ENCRYPT_ENABLED,
           ARK_BUS_REPLAY_ENABLED);
    return 0;
}

#endif /* ARK_BUS_CRYPTO_ENABLED */
