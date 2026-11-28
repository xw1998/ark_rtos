/**
* @file test_devid_weak_host.c
* @brief 板级钩子缺失时的 devid 行为（arkrtos_devid.c 的 weak 兜底）
* @details test_devid_host.c 用强符号替换了两只板级钩子，所以模块里的
*          weak 兜底（"这块板没有这条路"）一行都走不到。这里反过来：
*          只给一只钩子，另一只留给模块自己的 weak 定义，把"没有 OTP"的
*          两种形态各钉一遍——
*
*            A. 没有 UID 钩子：boot() 回 ERR_STATE、ready=0、prove() 回
*               ERR_NOKEY（绝不回一个看起来像身份的结果）；
*            B. 有 UID、没有密钥钩子（也没有内建密钥）：boot() 回 OK、
*               ready=0、来源 UID_ONLY、prove() 回 ERR_NOKEY。
*
*          两种形态都由命令行 -D 选择，同一份源码编两次：
*            -DDEVID_TEST_HAVE_UID      -> B
*            （什么都不定义）            -> A
*
*          未验证：真实板子上 weak 是否真的被板级强符号覆盖（那是链接器
*          的事，宿主上由这两个档间接证明弱定义本身可用）。
* @note 用法：由 tools/coverage_gate.py 的 devid-weak 两条 case 编译运行，
*       输出 DEVID-WEAK OK。
*/

#include "arkrtos_features.h"
#include "arkrtos_devid.h"

#include <stdio.h>
#include <string.h>

#if (ARKRTOS_USE_DEVICE_ID == 1)

static int g_fails;

static void ck(int cond, const char *what)
{
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

#if defined(DEVID_TEST_HAVE_UID)
/* 只补 UID 这一只；密钥钩子留给模块的 weak 兜底 */
int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    uint32 i;

    if(cap < 8u)
    {
        return -1;
    }
    for(i = 0u; i < 8u; i++)
    {
        p_out[i] = (uint8)(0x10u + i);
    }
    *p_len = 8u;
    return 0;
}
#endif

int main(void)
{
    uint8 mac[ARKRTOS_DEVID_MAC_LEN];
    uint8 ch[4] = { 1u, 2u, 3u, 4u };
    arkrtos_devid_status_t st;
    int32 rc;

    setvbuf(stdout, 0, _IONBF, 0);

    rc = arkrtos_devid_boot();

#if defined(DEVID_TEST_HAVE_UID)
    /* B：有号、没密钥 —— 名字成立，证明不成立 */
    ck(rc == ARKRTOS_DEVID_OK, "boot without a secret hook is OK");
    ck(arkrtos_devid_ready() == 0u, "no secret hook -> not ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck(st.source == (uint32)ARKRTOS_DEVID_SRC_UID_ONLY, "source is uid-only");
    ck(st.id_len == (uint32)ARKRTOS_DEVICE_ID_LEN, "id derived from uid");
#else
    /* A：连号都没有 —— 没有名字就没有身份 */
    ck(rc == ARKRTOS_DEVID_ERR_STATE, "boot without a uid hook is ERR_STATE");
    ck(arkrtos_devid_ready() == 0u, "no uid hook -> not ready");
    ck(arkrtos_devid_status_get(&st) == ARKRTOS_DEVID_OK, "status readable");
    ck(st.source == (uint32)ARKRTOS_DEVID_SRC_NONE, "source is none");
    ck(st.id_len == 0u, "no id without a uid");
#endif

    memset(mac, 0x5Au, sizeof(mac));
    ck(arkrtos_devid_prove(ch, sizeof(ch), mac) == ARKRTOS_DEVID_ERR_NOKEY,
       "prove() fails closed when a hook is missing");
    ck(mac[0] == 0x5Au && mac[ARKRTOS_DEVID_MAC_LEN - 1] == 0x5Au,
       "MAC untouched on failure");

    if(g_fails == 0)
    {
        printf("DEVID-WEAK OK\n");
    }
    return (g_fails == 0) ? 0 : 1;
}

#else /* ARKRTOS_USE_DEVICE_ID == 0 */

int main(void)
{
    printf("DEVID-WEAK OFF OK\n");
    return 0;
}

#endif /* ARKRTOS_USE_DEVICE_ID */
