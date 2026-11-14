/**
* @file arkrtos_waitmulti.h
* @brief 多对象同时就绪探测（内核内部头文件）
* @details 一次传进一组句柄，报回「哪几个已经就绪」，**不消费任何令牌**。
*          这样做的理由是：非破坏性探测没有回滚问题——如果探测时顺手把
*          令牌取走，ALL 模式下取得一半就失败时没法把已取的那几个放回去，
*          调用方还得处理「顺序污染」。所以这里只回答就绪状态，真正的取用
*          仍由调用方随后用各自 timeout=0 的接口完成。
*
*          支持的句柄类型：信号量（count>0）、消息队列（count>0）、
*          事件（flag 已置位）、互斥锁（owner==0，即可加锁）。
*          32 位事件集需要每条目带掩码，本模块不做（它有自己的等待接口）。
*
*          本模块只做一次探测，不登记等待者、不阻塞。带超时的等待由
*          用户态包装轮询完成（见 arkrtos.h 的 arkrtos_wait_multi）。
*          这是如实的能力边界：唤醒延迟上限是轮询周期，不是零。
* @author xw
* @date 2026.11.14
*/

#ifndef __ARKRTOS_WAITMULTI_H__
#define __ARKRTOS_WAITMULTI_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"

/* 就绪判定：ANY = 任意一个就绪，ALL = 全部就绪（值不得单独修改）。 */
#define ARKRTOS_WM_OPT_ANY          (1u)
#define ARKRTOS_WM_OPT_ALL          (2u)

#if (ARKRTOS_USE_WAITMULTI == 1)

/* 一次探测：handles[0..n-1] 的句柄，结果按位放进 ready_mask（bit i 对应
 * handles[i]）。返回 0=满足 opt 条件；-1=参数/句柄非法（整组拒绝）；
 * -2=暂不满足（ready_mask 里是当前快照，可用于诊断）。 */
int32 arkrtos_wm_probe_internal(const uint32 *handles, uint32 n,
                                uint32 opt, uint32 *ready_mask);

#endif /* ARKRTOS_USE_WAITMULTI == 1 */

#endif /* __ARKRTOS_WAITMULTI_H__ */
