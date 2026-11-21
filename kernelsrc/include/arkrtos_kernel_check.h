/**
* @file arkrtos_kernel_check.h
* @brief Kernel side checks on what a caller handed over through an SVC.
* @details These four helpers are the only place that decides whether a
*          pointer that arrived from user mode may be dereferenced by the
*          kernel. They used to be file static inside arkrtos_task.c; a second
*          kernel service module (arkrtos_net.c) needs the same answers, and a
*          second copy of this logic is exactly the kind of thing that drifts
*          apart and stops guarding.
*
*          Rules of use:
*            - arkrtos_kernel_svc_args_ok   the p[] argument block itself
*            - arkrtos_kernel_dev_buf_ok    memory the kernel WRITES into
*            - arkrtos_kernel_user_ro_ok    memory the kernel only READS
*            - arkrtos_kernel_user_name_ok  a NUL terminated name / path
*          Every check answers 1 = usable, 0 = refuse. A refusal is not an
*          error to be worked around: the caller gets a negative return.
*
* @note  Kernel internal. Apps and drivers must not include this file.
* @author xw
*/
#ifndef __ARKRTOS_KERNEL_CHECK_H__
#define __ARKRTOS_KERNEL_CHECK_H__

#include "arkrtos_types.h"

/* The p[] block of a multi argument SVC: sub command plus its arguments. */
uint8 arkrtos_kernel_svc_args_ok(const void *p, uint32 len);

/* A buffer the kernel writes into (dev_read, socket recv, ...). */
uint8 arkrtos_kernel_dev_buf_ok(const void *p, int32 len);

/* A buffer the kernel only reads (log text, socket send, ...). */
uint8 arkrtos_kernel_user_ro_ok(const void *p, uint32 len);

/* A name / path: read up to len bytes including the NUL, may live in the
 * caller's own firmware window (string literals). */
uint8 arkrtos_kernel_user_name_ok(const void *p, uint32 len);

/* 参数块字宽（U-18 剩余项，协议形状见 arkrtos_svc_abi.h）。
 *
 * 宽档（ARKRTOS_SVC_WIDE_ARGS == 1）内核只认带 ARKRTOS_SVC_SUB_WORD 标记的
 * 参数块：窄档 App 的 32 位块若按 8 字节解，每个参数都会混进下一个参数的
 * 高半截，读出一组「看着合理」的错参数。第一次拒绝打一行日志，
 * 1 = 块可用，0 = 拒绝（调用方回负值，不做任何解引用）。 */
#if (ARKRTOS_SVC_WIDE_ARGS == 1)
uint8 arkrtos_kernel_svc_wide_ok(const void *p_blk);

/* 取到参数块之后立刻过一遍字宽守卫：拒绝就回 -1 并返回。 */
#define SVC_WIDE_GUARD(p_svc_ctx, p)                                       \
    do                                                                     \
    {                                                                      \
        if(arkrtos_kernel_svc_wide_ok(p) == 0u)                            \
        {                                                                  \
            ARKRTOS_SVC_RET(p_svc_ctx, (uint32)(-1));                      \
            return;                                                        \
        }                                                                  \
    } while(0)
#else
/* 窄档没有字宽可判：块本来就是 32 位字，这一句展开成空语句。 */
#define SVC_WIDE_GUARD(p_svc_ctx, p)   do { } while(0)
#endif

#endif /* __ARKRTOS_KERNEL_CHECK_H__ */
