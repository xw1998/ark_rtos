/**
* @file sys_arch.h
* @brief lwIP 的"操作系统"面：把 sem/mbox/mutex/thread 映射到 Ark RTOS 内核对象。
* @details 四类对象一一对应，没有自造轮子：
*            sys_sem   → arkrtos_sem_create/wait/post
*            sys_mbox  → arkrtos_mq_create/send/recv
*            sys_mutex → arkrtos_mutex_create/lock/unlock
*            thread    → arkrtos_task_register（静态栈，见 sys_arch.c）
*
*          两个必须记住的点：
*            1) lwIP 的 timeout==0 表示"永久等待"，而 Ark RTOS 的 0 表示"不等待"、
*               负值才是永久。sys_arch.c 里统一做了换算，不要照抄参数。
*            2) 句柄一律 int32，-1 = 无效（SYS_SEM_NULL / SYS_MBOX_NULL ...），
*               这样 sys_sem_valid() 这些宏才有东西可判。
*
* @author xw
* @date 2026.09.23
*/
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

/** @brief 句柄类型：Ark RTOS 的内核对象号，-1 表示无效。 */
typedef int32_t sys_thread_t;

typedef int32_t sys_sem_t;
typedef int32_t sys_mbox_t;
typedef int32_t sys_mutex_t;

#define SYS_SEM_NULL    (-1)
#define SYS_MBOX_NULL   (-1)
#define SYS_MUTEX_NULL  (-1)
#define SYS_THREAD_NULL ((sys_thread_t)0)   /* 0 是内核保留的"不是任务" */

/* lwIP 传进来的是这些类型的**地址**，所以宏里要解引用。 */
#define sys_sem_valid(sem)          (((sem) != NULL) && (*(sem) >= 0))
#define sys_sem_set_invalid(sem)    do { if((sem) != NULL) { *(sem) = SYS_SEM_NULL; } } while(0)
#define sys_mbox_valid(mbox)        (((mbox) != NULL) && (*(mbox) >= 0))
#define sys_mbox_set_invalid(mbox)  do { if((mbox) != NULL) { *(mbox) = SYS_MBOX_NULL; } } while(0)

/* ---- 临界区：关中断并返回"上一次是否开着"，可嵌套 ----
 *
 * 语义与 Cortex-M 的 PRIMASK 存/取一致：进入时返回旧值、退出时按旧值恢复，
 * 所以套两层也不会在内层退出时把中断提前打开。
 *
 * 三套架构各用自己的"全局中断使能位"：
 *   Cortex-M   PRIMASK（bit0 = 关中断）
 *   LoongArch  CRMD.IE（bit2）
 *   RISC-V     mstatus.MIE（bit3）
 * 这三个位都只能在内核态（PLV0 / M）里改，本文件只被内核侧的 lwIP 用，
 * 符合这个前提；App 态代码不 include 它。 */
typedef uint32_t sys_prot_t;

#if defined(__CC_ARM) && !defined(__clang__)
static __inline sys_prot_t lwip_primask_enter(void)
{
    sys_prot_t pm;
    __asm { MRS pm, PRIMASK }
    __asm { CPSID i }
    return pm;
}
static __inline void lwip_primask_exit(sys_prot_t pm)
{
    __asm { MSR PRIMASK, pm }
}
#elif defined(__loongarch__)
/* CRMD 是 CSR 0。csrwr rd, csr 的语义是"交换"：读旧值进 rd、把 rd 写进 csr，
 * 所以一次 csrwr 就能拿到旧值，不必再多读一次。 */
#define LWIP_LA_CRMD_IE     0x4UL
static inline sys_prot_t lwip_primask_enter(void)
{
    unsigned long old;
    unsigned long neu;

    __asm volatile ("csrrd %0, 0x0" : "=r" (old) :: "memory");
    neu = old & ~LWIP_LA_CRMD_IE;
    if(neu != old)
    {
        __asm volatile ("csrwr %0, 0x0" : "+r" (neu) :: "memory");
    }
    return (sys_prot_t)old;
}
static inline void lwip_primask_exit(sys_prot_t pm)
{
    if((pm & LWIP_LA_CRMD_IE) != 0u)
    {
        unsigned long v;

        __asm volatile ("csrrd %0, 0x0" : "=r" (v) :: "memory");
        v |= LWIP_LA_CRMD_IE;
        __asm volatile ("csrwr %0, 0x0" : "+r" (v) :: "memory");
    }
}
#elif defined(__riscv)
#define LWIP_RV_MSTATUS_MIE 0x8UL
static inline sys_prot_t lwip_primask_enter(void)
{
    unsigned long old;

    /* csrrci rd, mstatus, 8：rd = mstatus; mstatus &= ~8 */
    __asm volatile ("csrrci %0, mstatus, 8" : "=r" (old) :: "memory");
    return (sys_prot_t)old;
}
static inline void lwip_primask_exit(sys_prot_t pm)
{
    if((pm & LWIP_RV_MSTATUS_MIE) != 0u)
    {
        __asm volatile ("csrsi mstatus, 8" ::: "memory");
    }
}
#else
static inline sys_prot_t lwip_primask_enter(void)
{
    sys_prot_t pm;
    __asm volatile ("MRS %0, PRIMASK" : "=r" (pm));
    __asm volatile ("CPSID i" ::: "memory");
    return pm;
}
static inline void lwip_primask_exit(sys_prot_t pm)
{
    __asm volatile ("MSR PRIMASK, %0" :: "r" (pm) : "memory");
}
#endif

#define SYS_ARCH_DECL_PROTECT(lev)  sys_prot_t lev
#define SYS_ARCH_PROTECT(lev)       do { (lev) = lwip_primask_enter(); } while(0)
#define SYS_ARCH_UNPROTECT(lev)     lwip_primask_exit(lev)

#endif /* LWIP_ARCH_SYS_ARCH_H */
