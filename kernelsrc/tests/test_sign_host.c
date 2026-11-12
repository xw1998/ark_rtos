/**
* @file    test_sign_host.c
* @brief   镜像签名（HMAC-SHA256）的宿主门禁：内核算出来的 MAC 必须与打包器逐字节相同
* @details 为什么需要这一道：`ARKRTOS_USE_IMAGE_SIGN` 默认 0，`arkrtos_sign.c`
*          整个文件平时一个编译器都不经过，更没有任何东西跑过它。
*          一条镜像签名链只有两头对上才有意义——打包器按 `tools/pack_app.py`
*          的 `calc_sign()` 写 signature，内核按 `arkrtos_sign_image()` 复算。
*          两头若对"哪些字段按 0、哪段算进 MAC"有一点点不同，结果就是
*          "自己签的镜像被自己拒收"，而这种错只会在打开开关那天以
*          "安装报 -6 签名不符"的形式出现，且看起来像密钥问题。
*
*          这里让**真实的 arkrtos_sign.c** 在宿主上对一个确定性缓冲区算 MAC，
*          把缓冲区与 MAC 一起交给 `tools/sign_host_gate.py`；
*          由它调打包器自己的 `calc_sign()` 复算并逐字节比对。
*          缓冲区里故意把 crc32 / state / runtime_ram_base 填成非 0 垃圾值，
*          这样"内核有没有把它们按 0"是被证明的，不是被假设的。
*
*          不验证：真机安装期拒收、密钥保护、公钥方案（未实现）。
*          宿主全绿不等于板端已验证。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_sign.h"
#include "arkrtos_app_image.h"

#define T_HDR       ((unsigned)ARKRTOS_APP_HEADER_SIZE)
#define T_RELOC_N   4u
#define T_RELOC_LEN ((unsigned)(T_RELOC_N * ARKRTOS_APP_RELOC_SIZE))
#define T_PAYLOAD   100u
#define T_TOTAL     (T_HDR + T_RELOC_LEN + T_PAYLOAD)

/* 确定性字节：同一序列在 Python 侧按同样算式复算，不依赖任何库。 */
static unsigned char lcg_next(unsigned *p_x)
{
    *p_x = (*p_x * 1103515245u) + 12345u;
    return (unsigned char)((*p_x >> 16) & 0xFFu);
}

int main(int argc, char **argv)
{
    static unsigned char buf[T_TOTAL];
    arkrtos_app_header_t *p_hdr = (arkrtos_app_header_t *)(void *)buf;
    unsigned char mac[ARKRTOS_SIGN_LEN];
    unsigned x = 1u;
    unsigned i;
    FILE *f;

    if(argc < 2)
    {
        fprintf(stderr, "usage: %s <image.bin>\n", argv[0]);
        return 2;
    }

    for(i = 0u; i < (unsigned)sizeof(buf); i++)
    {
        buf[i] = lcg_next(&x);
    }

    /* 长度字段决定内核算哪一段；另外三个字段是"必须被按 0"的证明点。 */
    p_hdr->reloc_count      = T_RELOC_N;
    p_hdr->image_size       = T_PAYLOAD;
    p_hdr->crc32            = 0xDEADBEEFu;
    p_hdr->state            = 0x00000007u;
    p_hdr->runtime_ram_base = 0x20001000u;

    memset(mac, 0, sizeof(mac));
    arkrtos_sign_image(buf, p_hdr, mac);

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

    printf("MAC ");
    for(i = 0u; i < (unsigned)ARKRTOS_SIGN_LEN; i++)
    {
        printf("%02x", (unsigned)mac[i]);
    }
    printf("\n");
    printf("IMAGE %u %u %u %u\n", T_HDR, T_RELOC_N, T_RELOC_LEN, T_PAYLOAD);
    return 0;
}
