/**
* @file test_init_host.c
* @brief arkrtos_init 的宿主测试（内核模块初始化顺序）
*
* @details 本文件回答的是「内核启动时各模块以什么顺序初始化」。这个函数自己不做
*          任何事，它的全部价值就是**顺序**：调用者把它当成唯一权威实现，某个工程
*          的 main 一旦自己按别的顺序逐个调 init，就是两份启动流程各自演化。所以
*          用例的不是「各模块被调了」，而是「被调的那一串名字逐项相等」——把顺序
*          本身钉成断言。
*
*          两处顺序的后果值得写下来：
*          - SMP 必须在调度启动前、且排在其它模块之后（次核晚启动会看到一个不是
*            自己拥有的正在跑的调度器）；
*          - 软定时器服务任务用 arkrtos_task_register 注册，必须在 arkrtos_cfg_load()
*            之后、调度启动之前；本文件里它是排在设备初始化之后、MPU 之前的那一步。
*
*          已验证：
*          - 默认档（SBUF / ESET / SMP 关，MPU / RTSTAT 开）的完整调用序列；
*          - 开档（SBUF / ESET / SMP 全开）的完整调用序列，三个新模块插在文档写的
*            位置（sbuf / eset 在 mq 之后、mempool 之前；smp 在 eset 之后）；
*          - 每个模块在整串里恰好出现一次（逐项比较本身就排除了重复与遗漏）；
*          - 连调两次时序列是「原序列重复一遍」——本函数**没有**重入保护，它在
*            如实反映「调用者只该调一次」，若将来加了 guard，这里必须先红一次再改。
*
*          刻意没验证：
*          - 各模块 init 的真实行为（各自有自己的用例）；
*          - MPU 区域内容、次核真的起来、真 Flash / 真外设（要板子）；
*          - arkrtos_cfg_load 与 arkrtos_port_irq_init 的先后——那两步在工程 main 里，
*            内核不拥有它们，本文件只钉内核拥有的这一段。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_config.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_event.h"
#include "arkrtos_hal.h"
#include "arkrtos_dev.h"
#include "arkrtos_sync.h"
#include "arkrtos_mq.h"
#include "arkrtos_mempool.h"
#include "arkrtos_timer.h"
#include "arkrtos_fault.h"
#include "arkrtos_mpu.h"
#include "arkrtos_sbuf.h"
#include "arkrtos_rt.h"
#include "arkrtos_eset.h"
#include "arkrtos_smp.h"
#include "arkrtos_init.h"

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

/* ------------------------------------------------------------------
 * 调用记录
 * ------------------------------------------------------------------ */
#define MAX_REC 64

static const char *g_seq[MAX_REC];
static int g_n = 0;

static void rec(const char *name)
{
    if(g_n < (int)MAX_REC)
    {
        g_seq[g_n] = name;
    }
    g_n++;
}

/* ------------------------------------------------------------------
 * 桩：十四路模块初始化，只记名字
 * ------------------------------------------------------------------ */
/* 接口暴露那一档（-Wmissing-prototypes）要求定义点先看得见原型；这些函数在各自
 * 模块头里本来就有声明，下面重复写一遍是为了让「桩」这件事在文件里显式可见。 */
void arkrtos_event_module_init(void);
void arkrtos_sync_module_init(void);
void arkrtos_mq_module_init(void);
#if (ARKRTOS_USE_SBUF == 1)
void arkrtos_sbuf_module_init(void);
#endif
#if (ARKRTOS_USE_ESET == 1)
void arkrtos_eset_module_init(void);
#endif
#if (ARKRTOS_USE_SMP == 1)
int32 arkrtos_smp_init(void);
#endif
void arkrtos_mempool_module_init(void);
void arkrtos_timer_module_init(void);
void arkrtos_fault_module_init(void);
void arkrtos_dev_module_init(void);
void arkrtos_dev_board_init(void);
void arkrtos_timer_task_install(void);
#if (ARKRTOS_USE_MPU == 1)
void arkrtos_mpu_module_init(void);
#endif
#if (ARKRTOS_USE_RTSTAT == 1)
void arkrtos_rt_init(void);
#endif

void arkrtos_event_module_init(void)    { rec("event"); }
void arkrtos_sync_module_init(void)     { rec("sync"); }
void arkrtos_mq_module_init(void)       { rec("mq"); }
#if (ARKRTOS_USE_SBUF == 1)
void arkrtos_sbuf_module_init(void)     { rec("sbuf"); }
#endif
#if (ARKRTOS_USE_ESET == 1)
void arkrtos_eset_module_init(void)     { rec("eset"); }
#endif
#if (ARKRTOS_USE_SMP == 1)
int32 arkrtos_smp_init(void)             { rec("smp"); return 0; }
#endif
void arkrtos_mempool_module_init(void)  { rec("mempool"); }
void arkrtos_timer_module_init(void)    { rec("timer"); }
void arkrtos_fault_module_init(void)    { rec("fault"); }
void arkrtos_dev_module_init(void)      { rec("dev"); }
void arkrtos_dev_board_init(void)       { rec("dev_board"); }
void arkrtos_timer_task_install(void)   { rec("timer_task"); }
#if (ARKRTOS_USE_MPU == 1)
void arkrtos_mpu_module_init(void)      { rec("mpu"); }
#endif
#if (ARKRTOS_USE_RTSTAT == 1)
void arkrtos_rt_init(void)              { rec("rt"); }
#endif

/* 文档里写死的那一串。开档把 sbuf / eset / smp 插进它们该在的位置。 */
static const char *const s_want[] =
{
    "event",
    "sync",
    "mq",
#if (ARKRTOS_USE_SBUF == 1)
    "sbuf",
#endif
#if (ARKRTOS_USE_ESET == 1)
    "eset",
#endif
#if (ARKRTOS_USE_SMP == 1)
    "smp",
#endif
    "mempool",
    "timer",
    "fault",
    "dev",
    "dev_board",
    "timer_task",
#if (ARKRTOS_USE_MPU == 1)
    "mpu",
#endif
#if (ARKRTOS_USE_RTSTAT == 1)
    "rt",
#endif
};

#define WANT_NUM ((int)(sizeof(s_want) / sizeof(s_want[0])))

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */

/* 一次调用的完整序列：逐项比名字，外加总数（总数独立挡住「多调了谁」）。 */
static void test_sequence(void)
{
    int i;

    g_n = 0;
    arkrtos_kernel_module_init();

    ck(g_n == WANT_NUM, "one call makes exactly the documented number of calls");
    for(i = 0; (i < WANT_NUM) && (i < g_n); i++)
    {
        if(g_seq[i] != s_want[i])
        {
            printf("  step %d: got %s, want %s\n", i,
                   (g_seq[i] != 0) ? g_seq[i] : "(none)", s_want[i]);
        }
        ck(g_seq[i] == s_want[i], "step order matches the documented sequence");
    }
}

/* 连调两次：没有重入保护，第二次原样再跑一遍。这不是「应该」，是当前事实。 */
static void test_no_reentry_guard(void)
{
    int i;

    g_n = 0;
    arkrtos_kernel_module_init();
    arkrtos_kernel_module_init();

    ck(g_n == (2 * WANT_NUM), "two calls make two full sequences (no internal guard)");
    for(i = 0; (i < WANT_NUM) && ((i + WANT_NUM) < g_n); i++)
    {
        ck(g_seq[i + WANT_NUM] == s_want[i],
           "the second run repeats the same order (no internal guard)");
    }
}

int main(void)
{
    test_sequence();
    test_no_reentry_guard();

#if (ARKRTOS_USE_SBUF == 1) && (ARKRTOS_USE_ESET == 1) && (ARKRTOS_USE_SMP == 1)
    printf("INIT-HOST ON OK  %d checks\n", g_checks);
#else
    printf("INIT-HOST OK  %d checks\n", g_checks);
#endif

    if(g_fails != 0)
    {
        printf("FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    return 0;
}
