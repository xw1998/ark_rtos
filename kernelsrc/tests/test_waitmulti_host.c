/**
* @file test_waitmulti_host.c
* @brief 多对象同时就绪探测（arkrtos_waitmulti）的宿主单元测试
* @details 这个模块的全部判断都在「分派 + 掩码 + ANY/ALL」里，没有调度器
*          参与，也没有令牌交收，所以宿主上就能把边界逐条摆出来。
*
*          被测的是真实的 arkrtos_waitmulti.c；四个 *_ready_internal() 由本
*          文件用可编程的桩代替（每个槽位可以摆成 就绪/未就绪/句柄失效），
*          内核的关中断/开中断也在这里空实现。
*
*          覆盖范围（如实说明）：
*            已验证：全未就绪、ANY 部分就绪（掩码逐位正确）、ALL 部分就绪、
*                    ALL 全就绪、n=0 / n 超过 ARKRTOS_WAITMULTI_MAX_OBJS、
*                    opt 非法、空指针、某个句柄失效时整组拒绝且掩码清 0、
*                    不支持的句柄类型、四种句柄的分派正确性、
*                    探测是只读的（只调用 ready，不调用任何取用接口）。
*            未验证：真实对象的并发时序（「探测到就绪、取用前被别人拿走」
*                    这个窗口只能在上板用例里观察）、用户态 SVC 轮询包装
*                    arkrtos_wait_multi() 的超时与唤醒延迟、真调度器下的
*                    关中断长度。那几条只有上板能证明。
* @note 用法：由 tools/waitmulti_host_gate.py 编译并运行，输出 WM-HOST OK。
*/

#include "arkrtos_waitmulti.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩
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

/* 关中断/开中断：本模块只在探测期间短暂进临界区，宿主上无意义 */
/* 桩也是接口：先声明后定义。 */
void arkrtos_port_disable_irq(void);
void arkrtos_port_enable_irq(void);
void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }

/* ------------------------------------------------------------------
 * 四个可编程的 ready 桩
 *   槽位值: 1 = 就绪, 0 = 未就绪, -1 = 句柄失效
 * ------------------------------------------------------------------ */
#define STUB_N  (8)

typedef struct
{
    int32 sem[STUB_N];
    int32 mtx[STUB_N];
    int32 mq[STUB_N];
    int32 evt[STUB_N];
    int   sem_calls;
    int   mtx_calls;
    int   mq_calls;
    int   evt_calls;
} stub_t;

static stub_t g_stub;

static void stub_reset(void)
{
    int i;

    for(i = 0; i < STUB_N; i++)
    {
        g_stub.sem[i] = -1;   /* 默认：没摆过就是句柄失效 */
        g_stub.mtx[i] = -1;
        g_stub.mq[i]  = -1;
        g_stub.evt[i] = -1;
    }
    g_stub.sem_calls = 0;
    g_stub.mtx_calls = 0;
    g_stub.mq_calls  = 0;
    g_stub.evt_calls = 0;
}

static int stub_lookup(const int32 *tab, int32 handle)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if(idx < 0 || idx >= STUB_N)
    {
        return -1;
    }
    return (int)tab[idx];
}

/* 四条 ready 桩同样是接口：先声明后定义。 */
int32 arkrtos_sem_ready_internal(int32 handle);
int32 arkrtos_mtx_ready_internal(int32 handle);
int32 arkrtos_mq_ready_internal(int32 handle);
int32 arkrtos_event_ready_internal(int32 handle);

int32 arkrtos_sem_ready_internal(int32 handle)
{
    g_stub.sem_calls++;
    return stub_lookup(g_stub.sem, handle);
}

int32 arkrtos_mtx_ready_internal(int32 handle)
{
    g_stub.mtx_calls++;
    return stub_lookup(g_stub.mtx, handle);
}

int32 arkrtos_mq_ready_internal(int32 handle)
{
    g_stub.mq_calls++;
    return stub_lookup(g_stub.mq, handle);
}

int32 arkrtos_event_ready_internal(int32 handle)
{
    g_stub.evt_calls++;
    return stub_lookup(g_stub.evt, handle);
}

int main(void)
{
    uint32 hs[4];
    uint32 mask;

    printf("== waitmulti host test ==\n");

    /* ---------------- 1. 参数校验 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG | 0u;
    g_stub.sem[0] = 0;

    mask = 0xFFFFFFFFu;
    ck(arkrtos_wm_probe_internal(hs, 0u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "n=0 必须拒绝");
    ck(arkrtos_wm_probe_internal(hs, (uint32)ARKRTOS_WAITMULTI_MAX_OBJS + 1u,
                                 ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "n 超过 ARKRTOS_WAITMULTI_MAX_OBJS 必须拒绝");
    ck(arkrtos_wm_probe_internal(0, 1u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "handles 空指针必须拒绝");
    ck(arkrtos_wm_probe_internal(hs, 1u, ARKRTOS_WM_OPT_ANY, 0) == -1,
       "ready_mask 空指针必须拒绝");
    ck(arkrtos_wm_probe_internal(hs, 1u, 0u, &mask) == -1,
       "opt=0 必须拒绝");
    ck(arkrtos_wm_probe_internal(hs, 1u, 3u, &mask) == -1,
       "opt=3（ANY|ALL）必须拒绝");

    /* ---------------- 2. 分派：四种句柄各一次 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG   | 1u;
    hs[1] = ARKRTOS_MTX_HANDLE_FLAG   | 2u;
    hs[2] = ARKRTOS_MQ_HANDLE_FLAG    | 3u;
    hs[3] = ARKRTOS_EVENT_HANDLE_FLAG | 4u;
    g_stub.sem[1] = 1;
    g_stub.mtx[2] = 1;
    g_stub.mq[3]  = 1;
    g_stub.evt[4] = 1;

    mask = 0u;
    ck(arkrtos_wm_probe_internal(hs, 4u, ARKRTOS_WM_OPT_ALL, &mask) == 0,
       "四种句柄全部就绪时 ALL 应满足");
    ck(mask == 0x0Fu, "四种句柄全部就绪时掩码应为 0x0F");
    ck(g_stub.sem_calls == 1 && g_stub.mtx_calls == 1 &&
       g_stub.mq_calls == 1  && g_stub.evt_calls == 1,
       "每种句柄应恰好分派一次");

    /* ---------------- 3. 全未就绪 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG | 0u;
    hs[1] = ARKRTOS_MQ_HANDLE_FLAG  | 1u;
    g_stub.sem[0] = 0;
    g_stub.mq[1]  = 0;
    mask = 0xFFFFFFFFu;
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ANY, &mask) == -2,
       "全未就绪时 ANY 应返回 -2");
    ck(mask == 0u, "全未就绪时掩码应清 0");
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ALL, &mask) == -2,
       "全未就绪时 ALL 应返回 -2");

    /* ---------------- 4. ANY 部分就绪 ---------------- */
    g_stub.sem[0] = 0;
    g_stub.mq[1]  = 1;
    mask = 0u;
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ANY, &mask) == 0,
       "ANY 部分就绪应满足");
    ck(mask == 0x2u, "ANY 部分就绪时掩码应只置 bit1");

    /* ---------------- 5. ALL 部分就绪 ---------------- */
    mask = 0u;
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ALL, &mask) == -2,
       "ALL 只有一半就绪时应返回 -2");
    ck(mask == 0x2u, "ALL 部分就绪时应报出当前快照（bit1）");

    /* ---------------- 6. ALL 全就绪 ---------------- */
    g_stub.sem[0] = 1;
    mask = 0u;
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ALL, &mask) == 0,
       "ALL 全就绪应满足");
    ck(mask == 0x3u, "ALL 全就绪时掩码应为 0x3");

    /* ---------------- 7. 单个句柄失效 ⇒ 整组拒绝 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG | 0u;
    hs[1] = ARKRTOS_MQ_HANDLE_FLAG  | 1u;
    g_stub.sem[0] = 1;   /* 前面这个是好的 */
    g_stub.mq[1]  = -1;  /* 后面这个失效 */
    mask = 0xFFFFFFFFu;
    ck(arkrtos_wm_probe_internal(hs, 2u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "组里有失效句柄时应整组返回 -1");
    ck(mask == 0u, "整组拒绝时掩码必须清 0（不能留半份答案）");

    /* ---------------- 8. 不支持的句柄类型 ---------------- */
    hs[0] = ARKRTOS_COND_HANDLE_FLAG | 0u;
    mask = 0u;
    ck(arkrtos_wm_probe_internal(hs, 1u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "条件变量句柄不在支持列表里，应拒绝");
    hs[0] = 0x00000000u;
    ck(arkrtos_wm_probe_internal(hs, 1u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "裸下标（无类型标志）应拒绝");

    /* ---------------- 9. 探测是只读的 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG | 0u;
    g_stub.sem[0] = 1;
    (void)arkrtos_wm_probe_internal(hs, 1u, ARKRTOS_WM_OPT_ALL, &mask);
    ck(g_stub.sem[0] == 1, "探测后桩状态应原样不变（没有取用）");

    /* ---------------- 10. 负数 idx 不越界 ---------------- */
    stub_reset();
    hs[0] = ARKRTOS_SEM_HANDLE_FLAG | 0x000FF000u;   /* idx 落在表外 */
    ck(arkrtos_wm_probe_internal(hs, 1u, ARKRTOS_WM_OPT_ANY, &mask) == -1,
       "越界槽位应拒绝");

    printf("%s (%d checks)\n", (g_fails == 0) ? "WM-HOST OK" : "WM-HOST FAIL",
           g_checks);
    return (g_fails == 0) ? 0 : 1;
}
