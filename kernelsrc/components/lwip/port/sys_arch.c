/**
* @file sys_arch.c
* @brief lwIP 的 OS 面实现（Ark RTOS）。见 arch/sys_arch.h 的映射表。
* @details 两条贯穿全文件的口径：
*            - **超时语义要换算**。lwIP 说 timeout==0 是"永久等待"，
*              Ark RTOS 说 0 是"不等待"、负值才是永久。所有入口都过
*              lwip_to_arkrtos_timeout()，谁也别自己写死参数。
*            - **失败不装成功**。内核对象创建失败、句柄非法、超时以外的错误码，
*              一律回 lwIP 认的失败值，并且往内核日志里留一条带错误码的记录；
*              不把"参数错了"说成"超时了"还不吭声。
*
*          线程只有一个来源：tcpip 线程（sockets API 必须有它）。栈是这里的
*          静态数组——内核任务只接受"调用者自己的"栈，理由见 arkrtos_cfg.h。
*
* @author xw
* @date 2026.09.23
*/
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"

#include "arkrtos_cfg.h"       /* arkrtos_task_register()：线程只能从内核注册 */
#include "arkrtos_config.h"    /* ARKRTOS_MQ_DEPTH：邮箱深度就是它的上限 */
#include "arkrtos_task.h"      /* wait_period_internal / kernel_get_time / task_wait_internal */
#include "arkrtos_sync.h"      /* *_internal：内核侧没有 SVC 可走，只能用内部原语 */
#include "arkrtos_mq.h"
#include "arkrtos_log.h"

/* lwIP 的 errno 变量。LWIP_PROVIDE_ERRNO=1 时由端口提供定义
 * （见 lwip/errno.h 末尾的 extern int errno）。
 * 这里只有一个全局量：多任务共享 errno 是 lwIP sockets 层自己的已知限制，
 * 我们没有给它做每任务副本——所以判断 socket 调用是否失败要看返回值，
 * 不要跨任务依赖 errno 的值。 */
int errno;

#include "lwip_port.h"

/* ============================================================
 * 超时换算
 * ============================================================ */

/** @brief lwIP 的毫秒超时 → Ark RTOS 的毫秒超时（0 = 永久）。 */
static int32 lwip_to_arkrtos_timeout(u32_t timeout)
{
    return (timeout == 0u) ? -1 : (int32)timeout;
}

/* ============================================================
 * 信号量
 * ============================================================ */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    int32 h;

    if(sem == NULL)
    {
        return ERR_ARG;
    }
    h = arkrtos_sem_create_internal("lwip.sem", (int32)count);
    if(h < 0)
    {
        ARKRTOS_LOGE("LWIP", "sem_create failed rc=%d", (int)h);
        *sem = SYS_SEM_NULL;
        return ERR_MEM;
    }
    *sem = h;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    if(!sys_sem_valid(sem))
    {
        return;
    }
    (void)arkrtos_sem_delete_internal(*sem);
    sys_sem_set_invalid(sem);
}

void sys_sem_signal(sys_sem_t *sem)
{
    if(!sys_sem_valid(sem))
    {
        ARKRTOS_LOGE("LWIP", "sem_signal on invalid handle");
        return;
    }
    if(arkrtos_sem_post_internal(*sem) != 0)
    {
        ARKRTOS_LOGE("LWIP", "sem_post failed h=%d", (int)*sem);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    u32_t start;
    int32 rc;

    if(!sys_sem_valid(sem))
    {
        ARKRTOS_LOGE("LWIP", "sem_wait on invalid handle");
        return SYS_ARCH_TIMEOUT;
    }
    start = (u32_t)arkrtos_kernel_get_time();
    rc = arkrtos_sem_wait_internal(*sem, lwip_to_arkrtos_timeout(timeout));
    if(rc != 0)
    {
        /* -2 是超时（正常路径）；别的负值是参数/句柄错误，也按超时回给 lwIP，
         * 但必须留痕，不能让"配置写错了"伪装成"等超时了"。 */
        if(rc != ARKRTOS_SYNC_ERR_TIMEOUT)
        {
            ARKRTOS_LOGE("LWIP", "sem_wait error rc=%d h=%d", (int)rc, (int)*sem);
        }
        return SYS_ARCH_TIMEOUT;
    }
    return (u32_t)arkrtos_kernel_get_time() - start;
}

/* ============================================================
 * 互斥量
 * ============================================================ */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    int32 h;

    if(mutex == NULL)
    {
        return ERR_ARG;
    }
    h = arkrtos_mtx_create_internal("lwip.mtx");
    if(h < 0)
    {
        ARKRTOS_LOGE("LWIP", "mutex_create failed rc=%d", (int)h);
        *mutex = SYS_MUTEX_NULL;
        return ERR_MEM;
    }
    *mutex = h;
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if((mutex == NULL) || (*mutex < 0))
    {
        return;
    }
    (void)arkrtos_mtx_delete_internal(*mutex);
    *mutex = SYS_MUTEX_NULL;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    /* 负值 = 永久等待：lwIP 的互斥量没有超时语义。 */
    if((mutex == NULL) || (arkrtos_mtx_lock_internal(*mutex, -1) != 0))
    {
        ARKRTOS_LOGE("LWIP", "mutex_lock failed");
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if(mutex == NULL)
    {
        return;
    }
    if(arkrtos_mtx_unlock_internal(*mutex) != 0)
    {
        ARKRTOS_LOGE("LWIP", "mutex_unlock failed h=%d", (int)*mutex);
    }
}

/* ============================================================
 * 消息队列（mbox：一条消息 = 一个指针）
 * ============================================================ */

/* lwIP 的邮箱装的是 void*，Ark RTOS 的消息队列按 32 位字搬运：这里固定搬
 * 一个字（指针本身），所以下面全部写成 (uint32 *)&ptr。队列的"深度"是
 * 消息条数（<= ARKRTOS_MQ_DEPTH），不是字节数。 */
err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    int32 h;

    if(mbox == NULL)
    {
        return ERR_ARG;
    }
    /* 内核队列的深度是编译期定值（ARKRTOS_MQ_DEPTH）。要得比它大就直说，
     * 不截短——截短的后果是丢包，而且丢在看不见的地方。 */
    if((size <= 0) || (size > (int)ARKRTOS_MQ_DEPTH))
    {
        ARKRTOS_LOGE("LWIP", "mbox size %d outside 1..%d", size, (int)ARKRTOS_MQ_DEPTH);
        *mbox = SYS_MBOX_NULL;
        return ERR_MEM;
    }
    h = arkrtos_mq_create_internal("lwip.mbx");
    if(h < 0)
    {
        ARKRTOS_LOGE("LWIP", "mq_create failed rc=%d", (int)h);
        *mbox = SYS_MBOX_NULL;
        return ERR_MEM;
    }
    *mbox = h;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if(!sys_mbox_valid(mbox))
    {
        return;
    }
    (void)arkrtos_mq_delete_internal(*mbox);
    sys_mbox_set_invalid(mbox);
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        ARKRTOS_LOGE("LWIP", "mbox_post on invalid handle");
        return;
    }
    /* 负值 = 等到有位为止：lwIP 认为 post 不该丢消息。 */
    if(arkrtos_mq_send_internal(*mbox, (uint32 *)&msg, 1, -1) != 0)
    {
        ARKRTOS_LOGE("LWIP", "mbox_post failed h=%d", (int)*mbox);
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        return ERR_ARG;
    }
    if(arkrtos_mq_send_internal(*mbox, (uint32 *)&msg, 1, 0) != 0)
    {
        return ERR_MEM;     /* 满：lwIP 会按"这次没投进去"处理 */
    }
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        return ERR_ARG;
    }
    if(arkrtos_mq_send_from_isr_internal(*mbox, (uint32 *)&msg, 1) != 0)
    {
        return ERR_MEM;
    }
    return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    u32_t start;
    void *got = NULL;
    int32 rc;

    if(!sys_mbox_valid(mbox))
    {
        ARKRTOS_LOGE("LWIP", "mbox_fetch on invalid handle");
        return SYS_ARCH_TIMEOUT;
    }
    start = (u32_t)arkrtos_kernel_get_time();
    /* 收进本地变量再交出去：超时那一轮 mbox 里什么都没有，绝不能让
     * 调用者拿到一个没被写过的指针。 */
    rc = arkrtos_mq_recv_internal(*mbox, (uint32 *)&got, 1, lwip_to_arkrtos_timeout(timeout));
    if(rc < 0)
    {
        if(rc != ARKRTOS_SYNC_ERR_TIMEOUT)
        {
            ARKRTOS_LOGE("LWIP", "mq_recv error rc=%d h=%d", (int)rc, (int)*mbox);
        }
        return SYS_ARCH_TIMEOUT;
    }
    if(msg != NULL)
    {
        *msg = got;
    }
    return (u32_t)arkrtos_kernel_get_time() - start;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    void *got = NULL;

    if(!sys_mbox_valid(mbox))
    {
        return SYS_MBOX_EMPTY;
    }
    if(arkrtos_mq_recv_internal(*mbox, (uint32 *)&got, 1, 0) < 0)
    {
        return SYS_MBOX_EMPTY;
    }
    if(msg != NULL)
    {
        *msg = got;
    }
    return 0;
}

/* ============================================================
 * 线程
 * ============================================================ */

/* lwIP 只会为 tcpip 线程调这里（sockets/netconn 都由它服务，App 不直接建
 * lwIP 线程）。两个槽足够，多出来的请求会拿到 SYS_THREAD_NULL 而不是
 * 悄悄复用别人的栈。 */
#define LWIP_THREAD_MAX      2
/* 栈按字算，实际给的是 LWIP_THREAD_STACK_W * 4 字节。
 *
 * 开真实网卡时给 8 KB：这一轮 tcpip 线程要跑 tcpip_callback(netif_add)、
 * ethernet_input 与 pbuf 链，实测 1 KB / 2 KB 都会被打穿——读数就是
 * "[STK] task N: stack bottom flag overwritten"，随后所有 tcpip_callback
 * 静默不执行（协议栈本身一句错都不报）。8 KB 是 lwIP 官方对 32 位平台
 * tcpip 线程的推荐下限。
 *
 * 关档保持 512 字（2 KB）：两个槽共 4 KB。这不是随手写的小数——Keil 的
 * F427 / F401 工程把 lwip_port.c 一起链进去，而 F427 的内核 RAM 区只有
 * 56 KB（build/kernel.sct 的 RW_KERNEL 0xE000）。两个槽按 8 KB 给会多占
 * 12 KB，实测直接 L6406E "No space in execution regions"（停在链接期，
 * 不是编译期），并且这 12 KB 换不来任何东西：ARM 这一档没有真网卡，
 * tcpip 线程仍然只调一次 tcpip_init。
 *
 * 2026-11-25 修正（LA64 十档低频 INE 停机的根因）：判断条件不能只看
 * NET_VIRTIO。**只要 LINUX_SOCKET 打开**，tcpip 线程就会跑 netconn /
 * socket 的收发与 select 唤醒，深度远超「只调一次 tcpip_init」——
 * 实测（QEMU LA64、-icount shift=5 确定性复现）2 KB 槽被 100% 用满
 * （0xcd 填充图案在整段 2044 字节里一个不剩），528 字节的陷阱帧溢出到
 * 槽下方的 g_lwip_task_stack，把相邻任务（task 11 arkrtos_lwip_task）
 * 保存的 ra 改写成 0x800，恢复时 jr $ra 落到零页 → INE 停机。
 * 因此：真网卡 或 socket 档一律 8 KB；ARM 那两档不开 socket，仍是 2 KB。 */
#if (ARKRTOS_USE_NET_VIRTIO == 1) || (ARKRTOS_USE_LINUX_SOCKET == 1)
#define LWIP_THREAD_STACK_W  2048u  /* 8 KB：真网卡档 / socket 档的 tcpip 线程 */
#else
#define LWIP_THREAD_STACK_W  512u   /* 2 KB，与 TCPIP_THREAD_STACKSIZE 对齐 */
#endif

/* lwipopts 里的 stacksize 只是"至少这么多"，真正的栈是下面的槽数组。
 * 两者一致时按槽给，不一致就报错——不截短、不悄悄放大。 */

typedef struct
{
    uint32          used;
    uint32          stack[LWIP_THREAD_STACK_W];
    lwip_thread_fn  entry;
    void           *arg;
} lwip_thread_slot_t;

static lwip_thread_slot_t g_thread_slot[LWIP_THREAD_MAX];

/* 每个槽一个入口：内核任务注册不收参数，靠"槽位专属入口"把 arg 带过去，
 * 免得去猜当前是哪个槽。 */
static void lwip_thread_tramp0(void);
static void lwip_thread_tramp1(void);

static void (* const g_thread_tramp[LWIP_THREAD_MAX])(void) =
{
    lwip_thread_tramp0,
    lwip_thread_tramp1
};

/** @brief 线程体跑完不该返回：内核任务返回意味着任务结束，lwIP 没这个预期。 */
static void lwip_thread_run(uint32 idx)
{
    g_thread_slot[idx].entry(g_thread_slot[idx].arg);
    ARKRTOS_LOGE("LWIP", "thread %u returned", (unsigned)idx);
    for(;;)
    {
        arkrtos_task_wait_internal(1000u);
    }
}

static void lwip_thread_tramp0(void)
{
    lwip_thread_run(0u);
}

static void lwip_thread_tramp1(void)
{
    lwip_thread_run(1u);
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    uint32 i;

    (void)name;
    if((thread == NULL) || (prio <= 0) || (prio > 254))
    {
        ARKRTOS_LOGE("LWIP", "thread_new bad args prio=%d", prio);
        return SYS_THREAD_NULL;
    }
    if(((uint32)stacksize > (LWIP_THREAD_STACK_W * 4u)) || (stacksize < 128))
    {
        /* 要的栈比这里有的还大：回失败。悄悄给一半的后果是随机踩内存。 */
        ARKRTOS_LOGE("LWIP", "thread_new stack %d out of range", stacksize);
        return SYS_THREAD_NULL;
    }
    for(i = 0u; i < (uint32)LWIP_THREAD_MAX; i++)
    {
        if(g_thread_slot[i].used == 0u)
        {
            int32 id;

            g_thread_slot[i].used  = 1u;
            g_thread_slot[i].entry = thread;
            g_thread_slot[i].arg   = arg;
            id = arkrtos_task_register(g_thread_tramp[i],
                                       g_thread_slot[i].stack,
                                       (uint32)sizeof(g_thread_slot[i].stack),
                                       (uint8)prio, 0u);
            if(id <= 0)
            {
                /* 注册失败必须留痕：tcpip 线程没起来时，协议栈不会有任何
                 * 报错，症状只是"所有 tcpip_callback 都不执行"。 */
                ARKRTOS_LOGE("LWIP", "thread_new: register failed id=%d stack=%d prio=%d",
                             (int)id, stacksize, prio);
                g_thread_slot[i].used = 0u;
                return SYS_THREAD_NULL;
            }
            ARKRTOS_LOGI("LWIP", "thread %u up id=%d stack=%u prio=%d",
                         (unsigned)i, (int)id,
                         (unsigned)sizeof(g_thread_slot[i].stack), prio);
            return (sys_thread_t)id;
        }
    }
    ARKRTOS_LOGE("LWIP", "thread_new: no slot");
    return SYS_THREAD_NULL;
}

/* ============================================================
 * 时间与初始化
 * ============================================================ */

u32_t sys_now(void)
{
    return (u32_t)arkrtos_kernel_get_time();
}

void sys_init(void)
{
    /* Ark RTOS 没有需要在这里初始化的 OS 侧资源：内核对象按需创建。 */
}

/* lwIP 的断言出口（lwipopts.h 里把 LWIP_PLATFORM_ASSERT 指到这里）。
 * 不调用 abort：内核里 abort 只会把整块板子带下去，而调用者想知道的是
 * "哪一条断言、在哪个文件"。 */
void arkrtos_lwip_assert_hook(const char *msg, int line, const char *file)
{
    ARKRTOS_LOGE("LWIP", "assert %s @%s:%d", (msg != NULL) ? msg : "?", file, line);
}
