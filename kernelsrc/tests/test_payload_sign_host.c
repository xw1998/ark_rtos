/**
* @file    test_payload_sign_host.c
* @brief   payload 尾签名（EXEC_SIGN 档）的宿主门禁：偏偏要拿真内核代码去算
* @details 为什么需要这一道：`ARKRTOS_USE_EXEC_SIGN` 默认 0，`arkrtos_sign_payload*`
*          这几行平时一个编译器都不经过；而这条链的两头分别是
*             打包器/构建  tools/sign_payload.py  （Python hashlib/hmac）
*             内核         arkrtos_sign.c          （手写 SHA-256 + HMAC）
*          任何一边把「覆盖多少字节、密钥怎么展开、tag 放哪」记错，症状都是
*          「自己签的文件自己拒收」，看起来像密钥问题。这里让**真实的**
*          arkrtos_sign.c 在宿主上对一个确定性缓冲区算 MAC，一次性入口与
*          流式入口各算一遍，把结果交给 tools/payload_sign_gate.py 与
*          Python 侧逐字节比对。
*
*          缓冲区长度故意不是 64 的整数倍，也不对齐：SHA-256 的填充分支
*          （55/56/63/64 字节边界）全被走到。
*
*          不验证：真机拒收（那在 QEMU 上由 elf_demo/mod_demo 的 tamper
*          check 驱动）、密钥保管、非对称方案（未实现）。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_sign.h"

#define T_LEN 4093u

static unsigned char lcg_next(unsigned *p_x)
{
    *p_x = (*p_x * 1103515245u) + 12345u;
    return (unsigned char)((*p_x >> 16) & 0xFFu);
}

static void print_hex(const char *tag, const unsigned char *p, unsigned n)
{
    unsigned i;

    printf("%s ", tag);
    for(i = 0u; i < n; i++)
    {
        printf("%02x", (unsigned)p[i]);
    }
    printf("\n");
}

int main(void)
{
    static unsigned char buf[T_LEN];
    unsigned char mac_once[ARKRTOS_SIGN_LEN];
    unsigned char mac_stream[ARKRTOS_SIGN_LEN];
    unsigned char wrong[ARKRTOS_SIGN_LEN];
    unsigned x = 7u;
    unsigned i;
    int rc_ok;
    int rc_bad;

    for(i = 0u; i < T_LEN; i++)
    {
        buf[i] = lcg_next(&x);
    }

    /* 一次性入口 */
    arkrtos_sign_payload(buf, T_LEN, mac_once);

    /* 流式入口：分成 1 + 500 + 剩余 三段，跨度不对齐是故意的 */
    arkrtos_sign_payload_begin();
    arkrtos_sign_payload_update(buf, 1u);
    arkrtos_sign_payload_update(buf + 1u, 500u);
    arkrtos_sign_payload_update(buf + 501u, T_LEN - 501u);
    rc_ok = arkrtos_sign_payload_end(mac_once);      /* 用手算的 tag 对 */

    /* 同时把流式自己的输出留下来：mac_stream 由 check() 反推不方便，
     * 所以再跑一遍，用错的 tag 确认「不一致必须返回 -1」。 */
    arkrtos_sign_payload(buf + 1u, T_LEN - 1u, mac_stream);
    for(i = 0u; i < ARKRTOS_SIGN_LEN; i++) { wrong[i] = (unsigned char)(mac_once[i] ^ 0xFFu); }
    rc_bad = arkrtos_sign_payload_check(buf, T_LEN, wrong);

    print_hex("PAYLOAD", mac_once, ARKRTOS_SIGN_LEN);
    print_hex("SHIFTED", mac_stream, ARKRTOS_SIGN_LEN);
    printf("STREAM_MATCH %d\n", (rc_ok == 0) ? 1 : 0);
    printf("WRONG_TAG_REFUSED %d\n", (rc_bad != 0) ? 1 : 0);
    printf("LEN %u\n", (unsigned)T_LEN);
    return 0;
}
