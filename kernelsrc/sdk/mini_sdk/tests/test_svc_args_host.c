/**
* @file test_svc_args_host.c
* @brief 迷你运行时参数块字宽的宿主门禁：同一份源码在窄档 / 宽档各编一遍，逐字核对块
* @details 为什么要有它：`arkrtos_mini_rt_arkc.c` 里的参数块在 32 位槽与 64 位槽
*          两档下都要与内核的 `ARKRTOS_SVC_ARG_BYTES(3)` 对得上。这档错法的形态
*          全都很安静——块长了一倍但内核按 4 字节解、子命令字没带 bit31、指针被
*          截成低 32 位——在板子上表现为「控制台什么都不出」或者「打印了一句别的
*          话」，跟「内核没收到调用」不可区分。所以把它拿到宿主上，用桩接住 SVC，
*          直接看块里的字节。
*
*          【这里测什么、不测什么】
*          测：块字宽（4 / 8 字节）、子命令字是否带 `0x80000000` 标记、指针字段
*              是否原值落进块、分块写入的 SVC 次数（95 字节/次）。
*          不测：`_exit()` 的块——它最后是一个死循环（内核不会再调度这个任务），
*              宿主上调不回来。那一条由门禁的源码断言钉住（`_exit` 必须用
*              `MINI_SVC_SUB(ARKRTOS_THREAD_SUB_EXIT)` 填 `q[0]`）。
*          窄档必须用 `-m32` 编（与板上一致）；宽档用 64 位宿主（`__SIZEOF_LONG__`
*          为 8）。跑法见 `tools/mini_svc_args_gate.py`。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 与 arkrtos_mini_rt_arkc.c 同一条字宽规则，但必须先于它写出来：
 * 桩的签名要与被测文件里的声明同型，而声明就依赖这个 typedef。 */
#ifndef ARKRTOS_USE_WIDE_SVC_ARGS
#define ARKRTOS_USE_WIDE_SVC_ARGS 0
#endif

#if (ARKRTOS_USE_WIDE_SVC_ARGS == 1) && defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ > 4)
typedef unsigned long long mini_svc_arg_t;
#else
typedef unsigned int mini_svc_arg_t;
#endif

/* ---- SVC 桩：把块原样接住 ---- */
static unsigned char g_block[3 * sizeof(mini_svc_arg_t)];
static int  g_block_bytes;
static int  g_nblocks;

int arkrtos_call_shell_svc(mini_svc_arg_t *p)
{
    if (g_nblocks == 0)
    {
        g_block_bytes = (int)(3 * sizeof(mini_svc_arg_t));
        memcpy(g_block, (const void *)p, (size_t)g_block_bytes);
    }
    g_nblocks++;
    return 0;
}

int arkrtos_call_thread_ctrl(mini_svc_arg_t *p)
{
    (void)p;
    g_nblocks++;
    return 0;
}

/* 被测文件：SDK 的 rt.c 原样，不是副本。 */
#include "../arkc/arkrtos_mini_rt_arkc.c"

static int g_fail;
static int g_case;

static void chk(int cond, const char *what)
{
    g_case++;
    if (cond)
    {
        printf("PASS %s\n", what);
    }
    else
    {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static mini_svc_arg_t blk_word(int i)
{
    mini_svc_arg_t w;

    memcpy(&w, g_block + (size_t)i * sizeof(mini_svc_arg_t), sizeof(w));
    return w;
}

int main(void)
{
    static char big[200];
    int i;

    printf("mode=%s block_bytes=%d\n",
           (MINI_SVC_WIDE != 0) ? "wide" : "narrow",
           (int)(3 * sizeof(mini_svc_arg_t)));

    for (i = 0; i < 200; i++)
    {
        big[i] = 'x';
    }

    /* 1) 一次 write：块里必须是 [子命令, 指针, 0] */
    g_nblocks = 0;
    g_block_bytes = 0;
    memset(g_block, 0xA5, sizeof(g_block));
    chk(write(1, "hi", 2u) == 2, "write() returns len");

    chk(g_nblocks == 1, "write() issues exactly one SVC");
    chk(g_block_bytes == (int)(3 * sizeof(mini_svc_arg_t)), "block is exactly 3 words wide");
    /* 这里写字面量、不写 MINI_SVC_SUB(...)：若两边都跟着宏变，宏本身被改坏
     * 也照样 PASS（变异自检实测过：去掉标记这条原来是绿的）。 */
#if (MINI_SVC_WIDE != 0)
    chk((unsigned long long)blk_word(0) == 0x80000003ULL,
        "block[0] is 3 with the bit31 marker (wide)");
#else
    chk((unsigned long)blk_word(0) == 3UL, "block[0] is plain 3 (narrow)");
#endif
    chk(blk_word(2) == (mini_svc_arg_t)0u, "block[2] is 0");

#if (MINI_SVC_WIDE != 0)
    chk((unsigned long)blk_word(1) == (unsigned long)(uintptr_t)g_chunk,
        "block[1] carries the full 64-bit chunk address");
#else
    chk((unsigned long)blk_word(1)
        == ((unsigned long)(uintptr_t)g_chunk & 0xFFFFFFFFUL),
        "block[1] carries the 32-bit chunk address");
#endif

    /* 2) 长负载：MINI_RT_CHUNK=96，每次最多 95 字节负载 => 200 字节分三次 */
    g_nblocks = 0;
    chk(write(1, big, 200u) == 200, "long write() returns len");
    chk(g_nblocks == 3, "200 bytes are split into 3 SVC calls (95 per call)");

    printf("---- %d case(s), %d failure(s) ----\n", g_case, g_fail);
    if (g_fail == 0)
    {
        printf("SVC-ARGS-HOST OK mode=%s cases=%d\n",
               (MINI_SVC_WIDE != 0) ? "wide" : "narrow", g_case);
    }
    return (g_fail == 0) ? 0 : 1;
}
