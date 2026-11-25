/**
* @file    test_sign_ed25519_host.c
* @brief   Ed25519 镜像签名的宿主门禁：内核摘要 == 打包器摘要，且验签/拒签都成立
* @details 为什么单独一份而不并进 test_sign_host.c：那一份测的是 HMAC 档，
*          两档的公开符号集合不同（Ed25519 档没有 arkrtos_sign_image），
*          混在一个文件里只能靠一堆 #if，读起来比两份更糟。
*
*          这一份固定三件事：
*            1. 覆盖范围一致 —— 内核 arkrtos_sign_digest() 算出的 32 字节，
*               必须等于 tools/pack_app.py 的 calc_digest()（由
*               tools/ed25519_image_gate.py 比对）。这条错的话，
*               打包侧签的镜像在设备上一定验不过。
*            2. 验签通路 —— 用固定的 64 字节签名（T_SIG）走完
*               signature -> 公钥 -> 摘要 的整条路，正例必须过。
*            3. 拒签 —— 改负载 / 改被覆盖的头字段 / 改签名本身都必须拒；
*               改 crc32 必须**照旧通过**，因为覆盖规则把该字段按 0
*               （这条是"覆盖规则真的生效"的正面证据，不是漏洞）。
*
*          T_SIG 是对本文件里那条确定性缓冲区算出的摘要的签名，私钥种子是
*          0x00..0x1f，公钥见 ARKRTOS_IMAGE_SIGN_PUBKEY（构建时由门禁给出）。
*          它是**黄金值**：改了 T_HDR/T_RELOC_N/T_PAYLOAD 或覆盖规则，它就会
*          失效，必须重签。重签命令写在 tools/ed25519_image_gate.py 头部注释里。
*
*          不验证：真机安装期拒收、公钥真实性（信任根属 SEC-G12）、
*          侧信道、打包端私钥保管。宿主全绿不等于板端已验证。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_sign.h"
#include "arkrtos_app_image.h"
#include "arkrtos_ed25519.h"

#define T_HDR       ((unsigned)ARKRTOS_APP_HEADER_SIZE)
#define T_RELOC_N   4u
#define T_RELOC_LEN ((unsigned)(T_RELOC_N * ARKRTOS_APP_RELOC_SIZE))
#define T_PAYLOAD   100u
#define T_TOTAL     (T_HDR + T_RELOC_LEN + T_PAYLOAD)

/* 对 T_TOTAL 字节确定性缓冲区的 SHA-256 摘要所做的 Ed25519 签名。
 * 私钥种子 = 0x00,0x01,...,0x1f（仅测试用，公开且一次性）。 */
static const unsigned char T_SIG[ARKRTOS_ED25519_SIG_LEN] = {
    0xfd, 0xa6, 0xec, 0x38, 0x0d, 0xe7, 0x7d, 0x4f,
    0x93, 0x8d, 0xc6, 0x6a, 0xae, 0xd5, 0xec, 0x0a,
    0xa3, 0x35, 0xe7, 0x1e, 0x0c, 0x73, 0x54, 0xb1,
    0x73, 0x5c, 0x5d, 0x96, 0x54, 0x55, 0x40, 0xc9,
    0x2a, 0x58, 0xd6, 0xfa, 0x65, 0xae, 0x60, 0xf4,
    0xcd, 0x0a, 0x3b, 0xe5, 0xf8, 0xf8, 0xd3, 0x1d,
    0xbf, 0x5d, 0x01, 0xee, 0x2d, 0xd7, 0x7d, 0x1c,
    0x17, 0x55, 0x49, 0xfa, 0x17, 0xd0, 0x03, 0x00
};

static unsigned g_checks = 0u;

static unsigned char lcg_next(unsigned *p_x)
{
    *p_x = (*p_x * 1103515245u) + 12345u;
    return (unsigned char)((*p_x >> 16) & 0xFFu);
}

static int expect(const char *what, int got, int want)
{
    g_checks++;
    if(got != want)
    {
        fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    static unsigned char buf[T_TOTAL];
    arkrtos_app_header_t *p_hdr = (arkrtos_app_header_t *)(void *)buf;
    unsigned char digest[32];
    unsigned x = 1u;
    unsigned i;
    FILE *f;
    int ok = 1;

    for(i = 0u; i < (unsigned)sizeof(buf); i++)
    {
        buf[i] = lcg_next(&x);
    }

    /* 四个"必须被按 0"的字段与两个长度字段；与 test_sign_host.c 同形。 */
    p_hdr->reloc_count      = T_RELOC_N;
    p_hdr->image_size       = T_PAYLOAD;
    p_hdr->crc32            = 0xDEADBEEFu;
    p_hdr->state            = 0x00000007u;
    p_hdr->runtime_ram_base = 0x20001000u;

    memset(p_hdr->signature, 0, sizeof(p_hdr->signature));
    memset(digest, 0, sizeof(digest));
    arkrtos_sign_digest(buf, p_hdr, digest);

    if(argc >= 2)
    {
        f = fopen(argv[1], "wb");
        if(f == NULL)
        {
            fprintf(stderr, "cannot write %s\n", argv[1]);
            return 2;
        }
        if(fwrite(buf, 1u, sizeof(buf), f) != sizeof(buf))
        {
            fclose(f);
            fprintf(stderr, "short write\n");
            return 2;
        }
        fclose(f);
    }

    printf("DIGEST ");
    for(i = 0u; i < (unsigned)sizeof(digest); i++)
    {
        printf("%02x", (unsigned)digest[i]);
    }
    printf("\n");
    printf("IMAGE %u %u %u %u\n", T_HDR, T_RELOC_N, T_RELOC_LEN, T_PAYLOAD);

    memcpy(p_hdr->signature, T_SIG, sizeof(T_SIG));

    /* --- 1. 黄金签名必须被接受 ---------------------------------- */
    ok &= expect("verify rejected the golden signature", arkrtos_sign_verify(buf, p_hdr), 0);

    /* --- 2. 流式入口必须与整幅入口同源 -------------------------- */
    arkrtos_sign_stream_begin(buf);
    arkrtos_sign_stream_update(buf + T_HDR, T_RELOC_LEN + T_PAYLOAD);
    ok &= expect("streaming verify rejected the golden signature",
                 arkrtos_sign_stream_end(p_hdr), 0);

    /* --- 3. 未 begin 就 update/end、重复 end 必须拒 -------------- */
    ok &= expect("stream end without begin was accepted", arkrtos_sign_stream_end(p_hdr), -1);
    arkrtos_sign_stream_begin(buf);
    arkrtos_sign_stream_update(buf + T_HDR, T_RELOC_LEN + T_PAYLOAD);
    ok &= expect("first stream end rejected the golden signature",
                 arkrtos_sign_stream_end(p_hdr), 0);
    ok &= expect("second stream end was accepted", arkrtos_sign_stream_end(p_hdr), -1);

    /* --- 4. 篡改负载一个比特必须拒 ------------------------------ */
    buf[T_TOTAL - 1u] = (unsigned char)(buf[T_TOTAL - 1u] ^ 0x01u);
    ok &= expect("verify accepted a flipped payload bit", arkrtos_sign_verify(buf, p_hdr), -1);
    buf[T_TOTAL - 1u] = (unsigned char)(buf[T_TOTAL - 1u] ^ 0x01u);
    ok &= expect("verify broke after the tamper was undone", arkrtos_sign_verify(buf, p_hdr), 0);

    /* --- 5. 篡改被覆盖的头字段必须拒 ---------------------------- */
    p_hdr->version ^= 0x00000001u;
    ok &= expect("verify accepted a flipped covered header field",
                 arkrtos_sign_verify(buf, p_hdr), -1);
    p_hdr->version ^= 0x00000001u;

    /* --- 6. crc32 在覆盖规则里按 0：改它必须**照旧通过** -------- */
    p_hdr->crc32 ^= 0x00000001u;
    ok &= expect("crc32 is not zeroed by the coverage rule (should still verify)",
                 arkrtos_sign_verify(buf, p_hdr), 0);
    p_hdr->crc32 ^= 0x00000001u;

    /* --- 7. 篡改签名本身必须拒 ---------------------------------- */
    p_hdr->signature[0] ^= 0x01u;
    ok &= expect("verify accepted a flipped signature byte", arkrtos_sign_verify(buf, p_hdr), -1);
    p_hdr->signature[0] ^= 0x01u;
    ok &= expect("verify broke after the signature was restored",
                 arkrtos_sign_verify(buf, p_hdr), 0);

    if(!ok)
    {
        return 1;
    }

    printf("SIGN-ED25519-HOST OK (%u checks)\n", g_checks);
    return 0;
}
