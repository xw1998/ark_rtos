/**
* @file arkrtos_svc_abi.h
* @brief SVC sub-command numbers: one table per service, shared by both sides.
* @details An SVC call carries a service number in the instruction immediate
*          (ARKRTOS_SVC_* in arkrtos_def.h) and, for every service that has
*          more than one operation, a sub-command in p[0] / parameters[0].
*          The kernel dispatch (svc_*() in arkrtos_task.c) and the SDK
*          wrappers (arkrtos_oslib.c, arkrtos_driver_bridge.c) each used to
*          spell those sub-commands as bare numbers, so adding or reordering
*          one meant editing two files and silently misrouting the call if
*          either side was missed. Both sides now use the names below.
*
*          Rules:
*            1. The numbers are part of the ABI. Add, never repurpose:
*               an App image built against an older SDK keeps sending the
*               old number, and a reused number would make it call the
*               wrong operation instead of failing.
*            2. A named constant never changes value; renaming one is a
*               source change, changing its number is an ABI change.
*            3. The argument layout of each sub-command lives next to the
*               wrapper that fills it (p[1..] in the SDK) and is validated
*               by the kernel handler (arkrtos_kernel_*_ok) before any
*               pointer is dereferenced.
*
*          The network service keeps its own table in arkrtos_net_abi.h,
*          because that file also carries the sockaddr-free argument rules
*          and the socket option numbers of the same contract.
*
* @note  Shared by kernel and SDK on purpose. Plain C, no include, no
*        platform dependency - the SDK compiles it with the App's own flags.
* @author xw
*/
#ifndef __ARKRTOS_SVC_ABI_H__
#define __ARKRTOS_SVC_ABI_H__

/* ------------------------------------------------------------------
 * 参数块字宽（U-18 剩余项：native SVC 的参数块）
 * @brief 历史上 native SVC 的参数块固定是 32 位字数组（内核按 uint32*
 *        解引用，SDK 用 uint32 parameters[]），于是窗口 VA 高于 4 GB 时
 *        App 放进块里的指针必被截断，内核的 user_ro_ok 一校验就拒——
 *        宽 VA 只通到了装载器与 Linux ABI，native SVC 这一半还卡在 32 位。
 *
 *        打开 ARKRTOS_USE_WIDE_SVC_ARGS 后参数块改成「原生字宽」数组：
 *          - 64 位目标：每个字 8 字节，指针字段不再截断；
 *          - 32 位目标：unsigned long 本身就是 4 字节，与原来逐字节相同
 *            （所以这个开关在 32 位目标上是空的）。
 *
 *        字宽变了必须能被认出来，否则窄档内核会把 8 字节块按 4 字节解，
 *        读出一组「看着合理」的错参数。因此宽档把子命令字置上 bit31：
 *          - 宽档内核读到没有标记的块 -> 拒绝并打一行一次性日志；
 *          - 窄档内核读到带标记的块 -> 子命令号不匹配任何 case，落到
 *            default，调用失败而不是碰巧成功。
 *        两种构建混用会失败，不会给出看似权威的错答案；但混用仍然是
 *        配置错误：内核与 App 必须用同一个开关编译（SDK 头跟着变）。
 *
 *        默认 0：默认档的代码、体积与行为零变化。
 * ------------------------------------------------------------------ */
#ifndef ARKRTOS_USE_WIDE_SVC_ARGS
#define ARKRTOS_USE_WIDE_SVC_ARGS       0
#endif

#if (ARKRTOS_USE_WIDE_SVC_ARGS == 1) && defined(__SIZEOF_LONG__) && (__SIZEOF_LONG__ > 4)
#define ARKRTOS_SVC_WIDE_ARGS           1
#else
#define ARKRTOS_SVC_WIDE_ARGS           0
#endif

#if (ARKRTOS_SVC_WIDE_ARGS == 1)
/** @brief 参数块里的一个字：原生指针宽度（宽档 = 8 字节）。 */
typedef unsigned long arkrtos_svc_arg_t;
#define ARKRTOS_SVC_SUB_WORD            (0x80000000UL)
/** @brief 宽档写子命令字：置位标记，窄档内核据此拒绝而不是误读。 */
#define ARKRTOS_SVC_SUB(sub)            ((arkrtos_svc_arg_t)((unsigned long)(sub) | ARKRTOS_SVC_SUB_WORD))
/** @brief 从子命令字取回子命令号（两档都能用）。 */
#define ARKRTOS_SVC_SUB_OF(w)           ((unsigned long)(w) & ~ARKRTOS_SVC_SUB_WORD)
/** @brief 参数块里 n 个 32 位字对应的字节数：窄档 n，宽档 2n。 */
#define ARKRTOS_SVC_ARG_BYTES(n)        ((unsigned long)(n) * (sizeof(arkrtos_svc_arg_t) / 4u))
#else
typedef unsigned int arkrtos_svc_arg_t;
#define ARKRTOS_SVC_SUB(sub)            ((arkrtos_svc_arg_t)(sub))
#define ARKRTOS_SVC_SUB_OF(w)           ((unsigned long)(w))
#define ARKRTOS_SVC_ARG_BYTES(n)        ((unsigned long)(n))
#endif

/* ARKRTOS_SVC_DEV_IO (0x10) - device open / read / write / ctrl / close */
#define ARKRTOS_DEV_SUB_OPEN                 (1u)
#define ARKRTOS_DEV_SUB_READ                 (2u)
#define ARKRTOS_DEV_SUB_WRITE                (3u)
#define ARKRTOS_DEV_SUB_CTRL                 (4u)
#define ARKRTOS_DEV_SUB_CLOSE                (5u)
#define ARKRTOS_DEV_SUB_SET_WILL             (6u)

/* ARKRTOS_SVC_TASK_CTRL (0x11) - task wait, delay, kill, status, scheduler lock */
#define ARKRTOS_TASK_SUB_WAIT                (1u)
#define ARKRTOS_TASK_SUB_WAIT_PERIOD         (2u)
#define ARKRTOS_TASK_SUB_DELAY               (3u)
#define ARKRTOS_TASK_SUB_KILL                (4u)
#define ARKRTOS_TASK_SUB_STATUS              (5u)
#define ARKRTOS_TASK_SUB_RECOVER             (6u)
#define ARKRTOS_TASK_SUB_SCHED_LOCK          (7u)
#define ARKRTOS_TASK_SUB_SCHED_UNLOCK        (8u)
#define ARKRTOS_TASK_SUB_SCHED_LOCK_COUNT    (9u)
#define ARKRTOS_TASK_SUB_STACK_INFO          (10u)

/* ARKRTOS_SVC_SYS_INFO (0x12) - time, cpu bus, fault log, loader pool */
#define ARKRTOS_SYS_SUB_TIME                 (1u)
#define ARKRTOS_SYS_SUB_CPU_BUSY             (2u)
#define ARKRTOS_SYS_SUB_FAULT_COUNT          (3u)
#define ARKRTOS_SYS_SUB_FAULT_GET            (4u)
#define ARKRTOS_SYS_SUB_POOL_FREE            (5u)

/* ARKRTOS_SVC_EVENT_CTRL (0x13) - event object create / wait / set */
#define ARKRTOS_EVENT_SUB_CREATE             (1u)
#define ARKRTOS_EVENT_SUB_WAIT               (2u)
#define ARKRTOS_EVENT_SUB_SET                (3u)

/* ARKRTOS_SVC_DRV_MGR (0x14) - driver register / count and hardware-safe region */
#define ARKRTOS_DRV_SUB_REGISTER             (1u)
#define ARKRTOS_DRV_SUB_UNREGISTER           (2u)
#define ARKRTOS_DRV_SUB_COUNT                (3u)
#define ARKRTOS_DRV_SUB_HSAFE_REGISTER       (4u)
#define ARKRTOS_DRV_SUB_HSAFE_QUERY          (5u)
#define ARKRTOS_DRV_SUB_HSAFE_UNREGISTER     (6u)

/* ARKRTOS_SVC_SYNC_CTRL (0x15) - semaphore / mutex / condition variable */
#define ARKRTOS_SYNC_SUB_SEM_CREATE          (1u)
#define ARKRTOS_SYNC_SUB_SEM_WAIT            (2u)
#define ARKRTOS_SYNC_SUB_SEM_POST            (3u)
#define ARKRTOS_SYNC_SUB_SEM_DELETE          (4u)
#define ARKRTOS_SYNC_SUB_MTX_CREATE          (5u)
#define ARKRTOS_SYNC_SUB_MTX_LOCK            (6u)
#define ARKRTOS_SYNC_SUB_MTX_UNLOCK          (7u)
#define ARKRTOS_SYNC_SUB_MTX_DELETE          (8u)
/* 递归互斥锁创建（ARKRTOS_USE_MTX_RECURSIVE=1 才受理，关档返回 -1）。
 * 加锁/解锁仍走 MTX_LOCK / MTX_UNLOCK：递归与否是对象的属性，
 * 不是每次调用另开一套入口。 */
#define ARKRTOS_SYNC_SUB_MTX_CREATE_RECURSIVE (17u)
/* ARKRTOS_SVC_WAITMULTI_CTRL：多对象就绪探测。
 * 参数块：p[0]=子命令，p[1]=句柄数组指针，p[2]=数量，p[3]=ANY/ALL，
 * p[4]=结果掩码指针。只做一次快照，不阻塞、不消费令牌。 */
#define ARKRTOS_WM_SUB_PROBE                 (1u)
#define ARKRTOS_SYNC_SUB_COND_CREATE         (9u)
#define ARKRTOS_SYNC_SUB_COND_WAIT           (10u)
#define ARKRTOS_SYNC_SUB_COND_SIGNAL         (11u)
#define ARKRTOS_SYNC_SUB_COND_BROADCAST      (12u)
#define ARKRTOS_SYNC_SUB_COND_DELETE         (13u)
#define ARKRTOS_SYNC_SUB_COND_ENQUEUE        (14u)
#define ARKRTOS_SYNC_SUB_COND_POLL           (15u)
#define ARKRTOS_SYNC_SUB_COND_ABORT          (16u)

/* ARKRTOS_SVC_MQ_CTRL (0x16) - message queue create / send / recv / delete */
#define ARKRTOS_MQ_SUB_CREATE                (1u)
#define ARKRTOS_MQ_SUB_SEND                  (2u)
#define ARKRTOS_MQ_SUB_RECV                  (3u)
#define ARKRTOS_MQ_SUB_DELETE                (4u)

/* ARKRTOS_SVC_TIMER_CTRL (0x17) - software timer create / start / stop / delete */
#define ARKRTOS_TIMER_SUB_CREATE             (1u)
#define ARKRTOS_TIMER_SUB_START              (2u)
#define ARKRTOS_TIMER_SUB_STOP               (3u)
#define ARKRTOS_TIMER_SUB_DELETE             (4u)

/* ARKRTOS_SVC_APP_MGR (0x18) - app image load / start / stop / status, mini program */
#define ARKRTOS_APP_SUB_LOAD_DEV             (2u)
#define ARKRTOS_APP_SUB_START                (3u)
#define ARKRTOS_APP_SUB_STOP                 (4u)
#define ARKRTOS_APP_SUB_STATUS               (5u)
#define ARKRTOS_APP_SUB_DRIVER_LOAD          (6u)
#define ARKRTOS_APP_SUB_MINI_RUN             (7u)
#define ARKRTOS_APP_SUB_MINI_STOP            (8u)

/* ARKRTOS_SVC_SHELL (0x1A) - extended shell command register / print */
#define ARKRTOS_SHELL_SUB_REGISTER           (1u)
#define ARKRTOS_SHELL_SUB_UNREGISTER         (2u)
#define ARKRTOS_SHELL_SUB_PRINT              (3u)

/* ARKRTOS_SVC_THREAD_CTRL (0x1B) - user thread create / self / exit */
#define ARKRTOS_THREAD_SUB_CREATE            (1u)
#define ARKRTOS_THREAD_SUB_SELF              (2u)
#define ARKRTOS_THREAD_SUB_EXIT              (3u)
/* 子 4（U-8）：p[1] = 退出码（int32，负数原样传给静待方）。
 * 与子 3 的差别只在「有没有一个可读的退出码」，两者都结束调用任务。 */
#define ARKRTOS_THREAD_SUB_EXIT_CODE         (4u)

/* ARKRTOS_SVC_FILE_SYS (0x1C) - volume relative file ops (1..10) and namespace path ops (11..23) */
#define ARKRTOS_FILE_SUB_WRITE               (1u)
#define ARKRTOS_FILE_SUB_READ                (2u)
#define ARKRTOS_FILE_SUB_REMOVE              (3u)
#define ARKRTOS_FILE_SUB_STAT                (4u)
#define ARKRTOS_FILE_SUB_INFO                (5u)
#define ARKRTOS_FILE_SUB_MOUNT               (6u)
#define ARKRTOS_FILE_SUB_UNMOUNT             (7u)
#define ARKRTOS_FILE_SUB_READ_AT             (8u)
#define ARKRTOS_FILE_SUB_RENAME              (9u)
#define ARKRTOS_FILE_SUB_LIST                (10u)
#define ARKRTOS_PATH_SUB_OPEN                (11u)
#define ARKRTOS_PATH_SUB_READ                (12u)
#define ARKRTOS_PATH_SUB_WRITE               (13u)
#define ARKRTOS_PATH_SUB_CLOSE               (14u)
#define ARKRTOS_PATH_SUB_STAT                (15u)
#define ARKRTOS_PATH_SUB_SEEK                (16u)
#define ARKRTOS_PATH_SUB_READDIR             (17u)
#define ARKRTOS_PATH_SUB_UNLINK              (18u)
#define ARKRTOS_PATH_SUB_MKDIR               (19u)
#define ARKRTOS_PATH_SUB_RMDIR               (20u)
#define ARKRTOS_PATH_SUB_RENAME              (21u)
#define ARKRTOS_PATH_SUB_TRUNCATE            (22u)
#define ARKRTOS_PATH_SUB_FTRUNCATE           (23u)

/* ARKRTOS_SVC_HEARTBEAT (0x1D) - supervision heartbeat report */
#define ARKRTOS_HB_SUB_REPORT                (1u)

/* ARKRTOS_SVC_SBUF_CTRL (0x1F) - stream buffer and message buffer */
#define ARKRTOS_SBUF_SUB_CREATE              (1u)
#define ARKRTOS_SBUF_SUB_SEND                (2u)
#define ARKRTOS_SBUF_SUB_RECV                (3u)
#define ARKRTOS_SBUF_SUB_SEND_ISR            (4u)
#define ARKRTOS_SBUF_SUB_USED                (5u)
#define ARKRTOS_SBUF_SUB_FREE                (6u)
#define ARKRTOS_SBUF_SUB_RESET               (7u)
#define ARKRTOS_SBUF_SUB_DELETE              (8u)
#define ARKRTOS_SBUF_SUB_MBUF_SEND           (9u)
#define ARKRTOS_SBUF_SUB_MBUF_RECV           (10u)
#define ARKRTOS_SBUF_SUB_MBUF_SEND_ISR       (11u)
#define ARKRTOS_SBUF_SUB_MBUF_PEEK_LEN       (12u)

/* ARKRTOS_SVC_MEMPOOL_CTRL (0x20) - fixed block memory pool */
#define ARKRTOS_MPOOL_SUB_CREATE             (1u)
#define ARKRTOS_MPOOL_SUB_ALLOC              (2u)
#define ARKRTOS_MPOOL_SUB_FREE               (3u)
#define ARKRTOS_MPOOL_SUB_QUERY              (4u)
#define ARKRTOS_MPOOL_SUB_DELETE             (5u)

/* ARKRTOS_SVC_ESET_CTRL (0x21) - 32-bit event set (ARKRTOS_USE_ESET=1) */
#define ARKRTOS_ESET_SUB_CREATE             (1u)
#define ARKRTOS_ESET_SUB_WAIT               (2u)
#define ARKRTOS_ESET_SUB_SET                (3u)
#define ARKRTOS_ESET_SUB_CLEAR              (4u)
#define ARKRTOS_ESET_SUB_GET                (5u)
#define ARKRTOS_ESET_SUB_DELETE             (6u)

#endif /* __ARKRTOS_SVC_ABI_H__ */
