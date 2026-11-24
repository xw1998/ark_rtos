/**
* @file arkrtos_net.h
* @brief 内核侧网络服务（SVC 0x1E）的入口。
* @details 为什么不是"SVC 里直接调 lwIP"：lwIP 的 socket API 会把请求投给
*          tcpip 线程并等它的完成信号量，也就是要阻塞；而 SVC 处理程序里
*          不能阻塞（PendSV 不会抢占 SVC，见 arkrtos_hal.h 的
*          arkrtos_sync_in_handler 说明）。所以拆成两边：
*            - SVC 处理程序只登记请求、立刻返回 ARKRTOS_NET_EPENDING；
*            - 一个内核服务任务用 thread mode 执行真正会阻塞的 lwIP 调用，
*              把结果放进请求槽；调用方（App SDK）轮询同一个请求拿结果。
*          真正的等待发生在 App 侧，和信号量/互斥锁的"登记 + WOULDBLOCK"
*          是同一套思路，只是这里多了一个执行者。
*
*          服务任务只调**非阻塞**的 lwIP 调用（socket 建好即设 O_NONBLOCK），
*          所以它自己几乎不占时间，一个请求槽就够。
*
* @note 只有内核包含。协议常量在 arkrtos_net_abi.h（内核与 App 共用）。
* @author xw
*/
#ifndef __ARKRTOS_NET_H__
#define __ARKRTOS_NET_H__

#include "arkrtos_types.h"
#include "arkrtos_net_abi.h"

/** @brief 网络服务任务的优先级。tcpip 线程是 12（见 lwipopts.h），
 *         服务任务要低一些，让 tcpip 能先跑完它投递的消息。 */
#define ARKRTOS_NET_TASK_PRIO        (14u)
/** @brief 服务任务栈（字）。只跑 lwIP 调用与一次 4 KB 以内的小拷贝。 */
#ifndef ARKRTOS_NET_TASK_STACK_WORDS
#define ARKRTOS_NET_TASK_STACK_WORDS (384u)
#endif

/**
* @brief 起网络服务任务（幂等）。由 lwIP 任务在 tcpip_init() 之后调用，
*        这样服务任务开始受理请求时 tcpip 线程一定已经在跑。
* @return 0=成功，负值=服务未编入（ARKRTOS_USE_LWIP=0）/任务表满
*/
int32 arkrtos_net_start(void);

/**
* @brief 网络服务是否已经起来。App 侧用它区分"服务没编进去"和"请求失败"。
* @return 1=可用，0=不可用
*/
uint8 arkrtos_net_ready(void);

/**
* @brief SVC 0x1E 的分发入口。**不阻塞**：登记请求或取回结果，随即返回。
* @param p_svc_ctx 系统调用上下文（ARKRTOS_SVC_ARG/RET 用）
* @return 见 arkrtos_net_abi.h 的返回约定
*/
int32 arkrtos_net_svc(void *p_svc_ctx);

/**
* @brief 归还某个任务名下的所有 socket。任务被卸载/故障回收时调用，
*        否则泄漏的是内核侧 socket 槽 —— 一个 App 反复崩溃会把它们耗干。
* @param task_id 目标任务号（1 起）
*/
void arkrtos_net_release_task(int32 task_id);

#if (defined(ARKRTOS_USE_LINUX_SOCKET) && (ARKRTOS_USE_LINUX_SOCKET == 1))
/**
* @brief 内核内的网络调用口（Linux ABI 层用，U-23）。
* @details 与 arkrtos_net_svc 同一条请求状态机，区别只在参数块来自内核栈：
*          不需要用户指针校验。返回约定与 SVC 完全一致，包括
*          ARKRTOS_NET_EPENDING（请求已登记，稍后用同样的参数再问）与
*          ARKRTOS_NET_EBUSY（别人的请求在飞）。
* @param op 子命令号（ARKRTOS_NET_SUB_*）
* @param a1..a5 子命令参数，含义见 arkrtos_net_abi.h
* @return 见 arkrtos_net_abi.h
*/
int32 arkrtos_net_call(uint32 op, uint32 a1, uint32 a2, uint32 a3,
                       uint32 a4, uint32 a5);
#endif

#if defined(ARKRTOS_NET_HOST_TEST)
/** @brief 宿主用例专用：只承接队列里的一条消息并执行（生产构建里不存在）。 */
void   arkrtos_net_host_step(void);
/** @brief 宿主用例专用：请求槽当前状态（IDLE / PENDING / DONE）。 */
uint32 arkrtos_net_host_req_state(void);
/** @brief 宿主用例专用：socket 表第 index 行的 lwIP fd（-1 = 空行）。 */
int32  arkrtos_net_host_sock_fd(uint32 index);
/** @brief 宿主用例专用：socket 表第 index 行的 owner（-1 = 孤儿）。 */
int32  arkrtos_net_host_sock_owner(uint32 index);
/** @brief 宿主用例专用：请求槽里最后一次执行的原样结果（主人已死时也只能经此读到）。 */
int32  arkrtos_net_host_req_result(void);
/** @brief 宿主用例专用：把请求槽强制恢复成空闲，用例之间互不继承。 */
void   arkrtos_net_host_reset(void);
#endif /* ARKRTOS_NET_HOST_TEST */

#endif /* __ARKRTOS_NET_H__ */
