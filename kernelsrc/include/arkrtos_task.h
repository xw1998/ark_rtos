/**
* @brief Ark RTOS 任务与调度相关定义（内核内部头文件）
* @details 定义任务控制块、任务状态、SVC 上下文结构，以及任务管理和调度器函数。
*          这些是内核内部接口，应用/驱动一般不直接包含本文件，而是通过 SDK 或 arkrtos.h 使用。
*          其中 stack_ptr 字段用于保存/恢复任务的栈指针，是上下文切换的关键。
*/

#ifndef __ARKRTOS_TASK_H__
#define __ARKRTOS_TASK_H__

#include "arkrtos_def.h"
#include "arkrtos_hal.h"
#include "arkrtos_config.h"

/* 返回码：值不得单独修改（多个名字共用 -1 是有意的，分名只为表意）。 */
#define ARKRTOS_TASK_ERR_PARAM     (-1)   /* 任务号 / 输出指针 / 优先级非法 */
#define ARKRTOS_TASK_ERR_ENTRY     (-1)   /* 入口不在调用方自己的代码区 */
#define ARKRTOS_TASK_ERR_STACK     (-1)   /* 栈不在调用方自己的 RAM 区 */
#define ARKRTOS_TASK_ERR_PRIORITY  (-1)   /* 255 是调度器的"无候选"哨兵，不能给任务 */
#define ARKRTOS_TASK_ERR_BUSY      (-2)   /* deadline 准入失败（带宽不足）；唯一与 -1 分开的错误码 */
#define ARKRTOS_TASK_ERR_CONTEXT   (-1)   /* 内核上下文 / 调度器锁定期间不允许阻塞 */
#define ARKRTOS_TASK_ERR_NOSTATE   (-1)   /* 任务没有栈信息（已结束 / 未创建） */
#define ARKRTOS_TASK_NONE          (-1)   /* 哨兵：没有就绪任务 / 不需要切换（不是错误码） */


typedef enum {
    ARKRTOS_TASK_INVALID,
    ARKRTOS_TASK_READY,
    ARKRTOS_TASK_WAIT,
    ARKRTOS_TASK_RUNNING
} arkrtos_task_status_t;

/* ============================================================
 * 系统调用上下文（架构无关）
 * @brief SVC_Server 只接收不透明的上下文指针，参数与返回值统一通过
 *        ARKRTOS_SVC_ARG / ARKRTOS_SVC_RET 宏访问（见 arkrtos_hal.h）。
 *        各架构的栈帧布局定义在 kernelsrc/port/<族>/<核心>/，
 *        内核不再包含任何寄存器排布，移植新架构无需改动本文件。
 * ============================================================ */
void SVC_Server(void *p_svc_ctx);


/**
* @brief 任务控制块（TCB）
* @details 保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
*          所有任务的 TCB 排成 arkrtos_task_table 数组。其中 stack_ptr 在任务被切走时
*          保存其 PSP，切回时据此恢复现场；mpu 是该任务的 MPU 区域快照。
*/
typedef struct {
    /* 地址字段用 unsigned long：32 位端口上它就是 uint32（逐字节不变），
     * LA64 上才装得下 32 位以上的窗口 VA（真实发行版静态二进制按
     * 0x120000000 链接）。size 仍然是 uint32 —— 2 MB 窗口塞不进别的类型
     * 才是问题。 */
    unsigned long ram_start;
    uint32 ram_size;
    uint32 stack_size;
    unsigned long rom_start;
    uint32 rom_size;
    int32  period;
    uint8  priority;                                /* 当前有效优先级（可被继承临时提升） */
    uint8  base_priority;                           /* 基准优先级：创建后不变，撤销继承用 */
    uint8  shm_attri;

    unsigned long stack_top;
    uint32 *stack_bottom;
    #if (ARKRTOS_USE_MPU == 1)
    arkrtos_arch_mpu_t mpu;           /* 架构无关的 MPU 区域上下文（见 arkrtos_arch.h） */
    #endif
    arkrtos_task_status_t status;
    int32  period_time;
    int32  wait_time;
    uint32 tim_tick;
    uint32 touch_tick;
    unsigned long stack_ptr;
    uint8  ready_next;                              /* 就绪链表后继（任务下标；ARKRTOS_TASK_NIL=无） */
    uint8  ready_prev;                              /* 就绪链表前驱（任务下标；ARKRTOS_TASK_NIL=无） */
    uint8  delay_next;                              /* 延时链表后继（任务下标；ARKRTOS_TASK_NIL=无） */
    uint8  delay_prev;                              /* 延时链表前驱（任务下标；ARKRTOS_TASK_NIL=无） */
    uint32 delay_tick;                              /* 延时链上的绝对到期节拍 */
    int32  slice_tick;                              /* 时间片剩余节拍，<=0 时让出同优先级队首 */
    uint8  recover_pending;                         /* 故障恢复待处理标记（两阶段恢复） */
    uint8  delay_queued;                            /* 1=当前挂在延时链上（显式标记，避免单节点自环歧义） */
    void (*entry)(void);                            /* 任务入口（恢复重建用） */
    uint8  wake_reason;                             /* 0=被唤醒 1=超时唤醒 */
    #if (ARKRTOS_USE_STACK_USAGE == 1)
    unsigned long stack_peak_low;                   /* 历史最低栈指针，用于峰值栈用量统计 */
    #endif
    #if (ARKRTOS_USE_STACK_GUARD == 1)
    uint8  stack_guard_armed;                       /* 1 = 栈底保护字已由注册流程布防 */
    uint8  stack_guard_reported;                    /* 1 = 已报过一次，避免每次切换刷同一条记录 */
    #endif
#if (ARKRTOS_USE_EXIT_CODE == 1)
    int32  exit_code;                               /* U-8：任务自己上报的退出码（exit_code_set=0 时无意义） */
    uint8  exit_code_set;                           /* 1 = exit_code 已由 EXIT_CODE 子命令写入 */
    uint8  exit_by_signal;                          /* 1 = 该任务是被信号杀死的（exit_code 存信号号）。
                                                     * wait4 的状态字按 Linux 的「信号终止」编码
                                                     * （低 7 位 = 信号号），而不是退出码的高字节。
                                                     * 两者混用会让 WIFEXITED / WIFSIGNALED
                                                     * 同时判错：一个越界写看起来像正常 exit(11)。 */
#endif
#if (ARKRTOS_USE_LINUX_FORK == 1)
    uint32 generation;                              /* U-19：每次 register 自增的注册代次。
                                                     * 任务槽位会被复用，单看 pid 认不出
                                                     * 「同一个号上的新旧两个实例」；fork 的
                                                     * wait4 拿它区分，晚到的 reap 如实报
                                                     * -ECHILD 而不是认错人。 */
#endif
#if (ARKRTOS_USE_APP_QUOTA == 1)
    uint32 app_owner;                               /* SEC-G07：这条任务属于哪个 App。
                                                     * 装载时分配一个非零号，fork 与
                                                     * clone(CLONE_THREAD) 继承；0 表示
                                                     * 没有身份（内核任务 / 不参与配额）。 */
#endif
    #if (ARKRTOS_USE_SCHED_DEADLINE == 1)
    /* U-28 dead线类（EDF）。单位全是节拍：换算只在设置入口做一次，
     * 调度路径上不再乘除，也不碰浮点。 */
    uint32 dl_runtime_tick;   /* 每个周期的运行预算 */
    uint32 dl_deadline_tick;  /* 相对截止时间（= 周期内的死线） */
    uint32 dl_period_tick;    /* 周期 */
    uint32 dl_abs_tick;       /* 本周期的绝对截止时刻 */
    uint32 dl_runtime_ns;     /* 原参数（ns）：准入按它算，不做节拍反推 */
    uint32 dl_deadline_ns;    /* 原参数（ns） */
    uint32 dl_period_ns;      /* 原参数（ns） */
    uint32 dl_budget_tick;    /* 本轮剩余预算 */
    uint8  dl_active;         /* 1 = 属于 deadline 类 */
    uint8  dl_throttled;      /* 1 = 预算耗尽、已从就绪集摘下等补回 */
    #endif
#if (ARKRTOS_USE_LINUX_SCHED == 1)
    uint8  sched_policy;                            /* U-28：Linux 策略镜像
                                                     * （0=OTHER/1=FIFO/2=RR/
                                                     * 6=DEADLINE）。注册时清零，
                                                     * 不随槽位复用继承旧策略。 */
#endif
    uint8  is_priv;                                 /* 1 = Thread mode privileged, 0 = unprivileged.
                                                     * Honoured only when ARKRTOS_USE_PRIV == 1;
                                                     * the switch path writes CONTROL.nPRIV from
                                                     * it, so no task can change it for itself. */
} arkrtos_task_t;

#if (ARKRTOS_USE_STACK_CHECK == 1)
#define ARKRTOS_STACK_END_FLAG_VAL  ARKRTOS_STACK_END_FLAG
#else
#define ARKRTOS_STACK_END_FLAG_VAL  (0xed01)
#endif

#if (ARKRTOS_USE_CPU_LOAD == 1)
extern uint16 arkrtos_cpu_busy_ticks;
/* Cumulative ticks the idle task (task id 0) was on the CPU since boot.
 * Added for /proc/uptime's idle field, which must report a real measurement
 * rather than a fabricated zero.  Gated with CPU_LOAD: idle time is the
 * complement of the busy sample this feature already takes. */
extern uint32 arkrtos_idle_ticks;
#endif

extern volatile uint32 arkrtos_interrupt_nest;
extern uint32 arkrtos_kernel_tick;
extern int32  arkrtos_current_task_id;
#if (ARKRTOS_USE_SCHED_LOCK == 1)
/* 调度器锁：>0 表示当前禁止任务切换（用户任务经 SVC 调用 arkrtos_sched_lock） */
extern volatile uint32 arkrtos_sched_lock_nest;

void   arkrtos_sched_lock_internal(void);
uint32 arkrtos_sched_unlock_internal(void);
int32  arkrtos_sched_lock_count_internal(void);
#endif

#if (ARKRTOS_USE_STACK_USAGE == 1)
/* 查询任务栈信息：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节 */
int32  arkrtos_task_stack_info_internal(int32 task_id, uint32 *out3);
/* 分项查询（诊断用）：out4[0]=总字节 out4[1]=合成已用 out4[2]=扫描已用
 * out4[3]=最低 PSP 已用。合成值取两路较大者，分开报才能判断是哪一路触顶 */
int32  arkrtos_task_stack_info_raw(int32 task_id, uint32 *out4);
#endif


int32  arkrtos_sched_next(void);

/* ============================================================
 * 就绪队列与延时链（O(1) 调度核心）
 *
 * 就绪队列：按优先级分桶的双向循环链表 + 256 位优先级位图。
 *   选任务 = 位图取最低置位得优先级，取该桶链表头 -> O(1)；
 *   同优先级轮转 = 把队首移到队尾 -> O(1)。
 * 就绪集 = status 为 READY 或 RUNNING 的任务（RUNNING 也是可运行）。
 *
 * 延时链：按绝对到期节拍升序的双向链表，只放“有到期时间的非就绪任务”。
 *   每拍只处理链头到期的那几个 -> 均摊 O(1)，不再遍历全表。
 * ============================================================ */
#define ARKRTOS_TASK_NIL        (0xFFu)

/* 任务控制块指针 -> 任务下标（0 起） */
#define ARKRTOS_TASK_IDX(p)     ((int32)((p) - arkrtos_task_table))

int32  arkrtos_ready_top(void);                      /* O(1) 取最高优先级就绪任务下标，-1=无 */
void   arkrtos_ready_add(int32 task_idx);            /* 加入就绪队列（幂等）并从延时链摘除 */
void   arkrtos_ready_del(int32 task_idx);            /* 移出就绪队列（幂等） */
void   arkrtos_ready_reprio(int32 task_idx, uint8 old_prio);  /* 优先级变了，换桶 */
void   arkrtos_ready_rotate(int32 task_idx);         /* 同优先级轮转：移到该桶队尾 */
void   arkrtos_delay_arm(int32 task_idx, uint32 ticks);   /* 挂入延时链（到期=当前节拍+ticks） */
void   arkrtos_delay_disarm(int32 task_idx);         /* 从延时链摘除（幂等） */
void   arkrtos_delay_tick(void);                     /* 每拍调用：只处理链头已到期的节点 */
void   arkrtos_ready_reset(void);                    /* 清空就绪队列与延时链（装载/重置时调用） */
void   arkrtos_ready_rebuild(void);                  /* 从任务表 status 重新派生就绪集（首次调度前兜底） */
int32  arkrtos_sched_check(void);                    /* 一致性自检：返回不一致条目数，0=一致 */
uint32 arkrtos_sched_ready_count(void);              /* 当前就绪队列上的任务数（仅自检/调试） */
uint8  arkrtos_ready_contains(int32 task_idx);       /* 1=该任务当前挂在就绪链上（仅自检/调试） */

arkrtos_task_t *arkrtos_task_get_current(void);
uint32 arkrtos_kernel_get_time(void);
uint32 arkrtos_kernel_get_tick(void);
uint16 arkrtos_kernel_get_cpu_busy_ticks(void);

void arkrtos_task_wait_internal(uint32 ms);
void arkrtos_task_wait_period_internal(void);
void arkrtos_task_block_internal(void);

/**
* @brief 在调用方已持有的临界区内阻塞当前任务（供信号量/互斥锁/消息队列使用）
* @param timeout_ms >0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
* @note 这是内核内部原语，不直接对外。调用方（sem/mtx/mq/event）必须先把
*       对外 API 的“0=不等待、负值=永久等待”约定归一化后再调用，
*       不能把对外的 0 直接传进来（那会被本原语解释成无限等待）。
* @return 0=被显式唤醒（已获得资源），1=等待超时，-1=未能进入阻塞（调用方需自行摘除队列）
* @details 调用约定：进入时中断已关，返回时中断仍关（调用方在同一临界区内继续处理等待队列）。
*          与 arkrtos_task_wait_internal 的区别：后者会自行开关中断，
*          对“先把自己挂进等待队列、再睡下”的原语来说，那中间存在唤醒丢失窗口
*          （ISR 的唤醒会投给一个还没睡下的任务）。本函数把置状态与切走放在同一临界区内。
*/
int32 arkrtos_task_block_in_critical(uint32 timeout_ms);

/**
* @brief 唤醒一个正在等待的任务（可在别的任务上下文里调用）
* @param task_id 目标任务号（1 起）
* @return 0 = 已从 WAIT 置为 READY；-1 = 目标任务不存在或不处于等待态
* @details 与各同步对象里手写的唤醒三件套同形（wake_reason / status /
*          arkrtos_ready_add），但收在一处、自带保存-恢复临界区。
*          只动 status == ARKRTOS_TASK_WAIT 的任务：对就绪/运行/无效的任务
*          调用不产生任何状态变化，因此不会把一个已经死掉的任务从旧栈
*          "复活"（收尸清掉等待登记之后仍可能有迟到的唤醒）。
*          当前唯一使用者是 Linux ABI 兼容层（子进程退出唤醒等它的父进程、
*          跨任务信号唤醒目标进程）；默认档没有调用点，符号也不产生。
*/
int32 arkrtos_task_wakeup_internal(int32 task_id);

/**
* @brief 任务下线收尸：清理该任务在同步对象/消息队列中的等待登记与锁持有关系
* @param task_id 目标任务号（从 1 开始）
* @details 用于任务自杀（kill）、故障恢复、覆盖安装前硬停任务等场景。
*          不做收尸的后果：post/unlock 会把一个已经不在等待的任务置为 READY
*          （从旧栈“复活”）；它持有的互斥锁永久锁死，等待者全部饿死。
*          本函数自带保存-恢复语义的临界区，可在已有临界区内安全调用。
*/
void arkrtos_task_release_resources(int32 task_id);
/* Planned hand-over variant: the device ledger is dropped, but no will and
 * no safe-state callback is run - the successor re-opens the same devices
 * right after, so driving them to safe state here would be a glitch.
 * Used by the MiniApp graceful-exit path (ARKRTOS_MINI_EXIT_IS_HANDOVER). */
void arkrtos_task_release_resources_handover(int32 task_id);
void arkrtos_task_delay_internal(uint32 us);
void arkrtos_task_kill_internal(void);
/* 任务入口返回时的回收入口：移植层建初始栈帧时把 ra 指向这里。
 * 内核不承认「入口返回」是正常结束，但也绝不让 PC 跳到 0。 */
void arkrtos_task_entry_return(void);
int32  arkrtos_task_status_get_internal(int32 task_id);

/* ============================================================
 * User thread service (SVC 0x1B) - kernel side
 *
 * Every entry point below is reachable from user mode through the SVC
 * boundary. The create wrapper validates the entry point and the stack
 * window against the caller's own firmware / RAM regions before a TCB is
 * allocated, so a thread can never be handed a stack that overlaps the
 * kernel or a neighbour App.
 * ============================================================ */
int32  arkrtos_thread_create_internal(void (*entry)(void), uint32 *stack_bottom,
                                    uint32 stack_size, uint8 priority, uint32 period_ms);
int32  arkrtos_thread_self_internal(void);
void   arkrtos_thread_exit_internal(void);
#if (ARKRTOS_USE_EXIT_CODE == 1)
/**
* @brief 带退出码结束当前任务（U-8）。
* @param code 退出码，原样记进 TCB 的 exit_code 并置 exit_code_set=1。
* @note 只对「当前任务」有意义；内核线程调用等价于 arkrtos_thread_exit_internal()。
*       ARKRTOS_USE_EXIT_CODE=0 时不提供这两个入口（不占一个字节）。
*/
void   arkrtos_thread_exit_code_internal(int32 code);
/**
* @brief 读某个任务上报的退出码。
* @param task_id 任务号（1 起）
* @param p_code 可为 0；成功时写回退出码
* @return 1 = 该任务上报过退出码，0 = 没上报过 / 任务号非法
*/
int32  arkrtos_task_exit_code(int32 task_id, int32 *p_code);
#endif
int32  arkrtos_task_recover(int32 task_id);

void arkrtos_sched_activate_higher(uint8 ck_pri);

#if (ARKRTOS_USE_LINUX_SCHED == 1)
/* sched_yield(124)：把当前任务挪到同优先级队尾并请求一次调度。
 * 内核原地让出，不经过 ABI 层自己拼栈帧。 */
void   arkrtos_task_yield_internal(void);
#endif


/* ============================================================
 * U-28 deadline 类（EDF + CBS 节流）
 *
 * 为什么是内核级的调度类，而不是 ABI 层自己记一笔：选中必须在调度决策
 * 里做（ready_top），预算必须按「真正在跑的那一拍」扣（节拍中断里），
 * 两件事都只有调度器自己知道。ABI 层只负责把 Linux 的 ns 参数翻译过来。
 * ============================================================ */
#if (ARKRTOS_USE_SCHED_DEADLINE == 1)
/**
* @brief 把任务设成 deadline 类（成立则替换它原来的 deadline 参数）
* @param task_id 任务号（1 起）
* @param runtime_ns 每周期预算（ns，> 0）
* @param deadline_ns 相对截止时间（ns，>= runtime，<= period）
* @param period_ns 周期（ns，> 0）
* @return 0 成功；-1 参数非法或任务号不存在；-2 准入失败（带宽不足）；
*         -3 该任务正持有互斥锁/在等待队列上（不能中途换类）
* @note 带宽已满时回 -2 且不改动任何字段：拒绝比改一半好。
*       任务原来就是 deadline 类的，先释放它的旧带宽再按新参数准入。
*/
int32  arkrtos_task_dl_set(int32 task_id, uint32 runtime_ns, uint32 deadline_ns,
                           uint32 period_ns);
/**
* @brief 退出 deadline 类（释放带宽；幂等）
* @note 节流中的任务会被重新挂回就绪集，否则它会永远醒不过来。
*/
void   arkrtos_task_dl_clear(int32 task_id);
/**
* @brief 读回某任务的 deadline 参数
* @return 1 = 该任务是 deadline 类（三个输出都写）；0 = 不是（不写）
*/
int32  arkrtos_task_dl_get(int32 task_id, uint32 *p_runtime_ns,
                           uint32 *p_deadline_ns, uint32 *p_period_ns);
/** 已用带宽（ns / s），准入与读数用。用 unsigned long long 而不是自造
 *  uint64：内核里没有这个 typedef，32 位端口也要能算 64 位乘积。 */
unsigned long long arkrtos_task_dl_bw_ns(void);
#endif


/* ============================================================
 * 任务热切换（A/B 双缓冲无缝升级的内核一侧）
 *
 * 用法是两段式：先 arm（把「换到哪份镜像的哪个入口与哪块 RAM」挂上去），
 * 再到目标任务下一次 SVC 的入口处由 SVC_Server 调 take 完成切换。
 * 之所以不能一口气做完：改的是「异常返回到哪里」，只有站在 SVC 的硬件栈帧上
 * 才有一份可以安全改写的栈帧。arm 只记账、不动任何正在跑的东西，因此随时可撤。
 * ============================================================ */
#define ARKRTOS_SWAP_ERR_PARAM   (-1)   /* 参数非法 / 任务号非法 */
#define ARKRTOS_SWAP_ERR_BUSY    (-2)   /* 已经有一次切换在路上（同时只允许一次） */
#define ARKRTOS_SWAP_ERR_LAYOUT  (-3)   /* 新旧 RAM 块大小不同：数据布局不兼容，拒绝 */
#define ARKRTOS_SWAP_ERR_THREAD  (-4)   /* 还有同窗口的线程：它们会指着旧块，拒绝 */
#define ARKRTOS_SWAP_ERR_FRAME   (-5)   /* 当前栈帧不是基本帧（用过 FPU），拒绝 */

int32 arkrtos_task_swap_arm(int32 task_id, unsigned long rom_start, uint32 rom_size,
                            unsigned long ram_start, uint32 ram_size,
                            unsigned long entry);
int32 arkrtos_task_swap_take(void *p_exc_ctx, int32 task_id);
int32 arkrtos_task_swap_cancel(int32 task_id);
int32 arkrtos_task_swap_pending(void);

#if (ARKRTOS_USE_TASK_SWAP == 1)
/* 内核自检（swap 命令）的直连钩子：arm 之后，目标任务的下一次 SVC 由
 * SVC_Server 直接走内核这套 take，不经过 mini/loader 的在途账。
 * 不 arm 时两个函数一个字都不做。 */
void  arkrtos_task_swap_selftest_arm(int32 task_id);
int32 arkrtos_task_swap_selftest_take(void *p_exc_ctx);
#endif

#if (ARKRTOS_USE_STACK_GUARD == 1)
/* 栈底守卫字检查：slot 是 0 基槽位。0 = 正常或未布防，1 = 被踩。
 * 首次发现时记一条 ARKRTOS_FAULT_STACKOVF 并打一行控制台，之后只返回 1。 */
int32 arkrtos_stack_guard_check(int32 slot);
void  arkrtos_stack_guard_check_current(void);

/* 启动/idle 上下文的栈守卫（这块栈不属于任何任务槽位）。
 * 对象：arkrtos_current_task_id == 0 时用的那块栈 —— 示例里是
 *       arkrtos_port_enter_idle() 之前由启动代码装的 idle 栈。
 * 为什么它也要守：eentry 在换到静态陷阱栈之前就用被打断者的 sp 建帧并
 *       直接调 la_trap_c，所以节拍链（守卫 + 软定时器 + 调度扫描）整段跑在
 *       这块栈上；它和任务栈一样会被踩穿，而它下面紧邻的往往是另一块栈或
 *       .bss（实测：踩穿后写坏相邻任务的初始帧，ertn 到垃圾 PC）。
 * 做不到什么：只在这块栈**被用到更低**之后才看得见，每拍从已知最低水位
 *       往下探几格，代价 O(新增深度)，不是硬件保护。
 * 布防时机：必须在 arkrtos_port_enter_idle() 之前（此时这块栈还没被当作
 *       栈用）；stack_top 传马上要交给 enter_idle 的 sp，函数只填
 *       [stack_bottom, stack_top)，不会碰调用者的活帧。 */
void   arkrtos_stack_guard_watch_context(uint32 stack_bottom, uint32 stack_size,
                                        uint32 stack_top);
void   arkrtos_stack_guard_check_context(void);
/* out = { bottom, top, low, margin }，四格都写；未布防返回 -1 且不写。 */
int32  arkrtos_stack_guard_context_info(uint32 out[4]);
#endif

int32 arkrtos_sched_is_switching(void);
unsigned long arkrtos_sched_activate(int32 new_task, unsigned long old_psp);

void arkrtos_kernel_tick_handler(void);
#if (ARKRTOS_USE_TICKLESS == 1)
/* tickless idle 用：延时链里最近一次到期还有几拍。
 * 没有等待中的任务时返回 ARKRTOS_TICKLESS_NO_EVENT（见 arkrtos_tickless.h）。 */
uint32 arkrtos_task_next_delay_ticks(void);
/* tickless 补发整拍：只跑节拍体，不结账 */
void   arkrtos_kernel_tick_ticks(uint32 n);
#endif
/* 各 CPU 异常共用入口：fault_type 取 arkrtos_fault_type_t 中的 HARDFAULT/MEMFAULT/BUSFAULT/USGFAULT
 * 返回值：可用于恢复的任务栈指针（非 0 时由板级层调用 arkrtos_port_resume_task 完成恢复）；
 *         0 表示不可恢复（内核/中断上下文故障），板级层应停机等待调试器。 */
unsigned long arkrtos_cpu_fault_handler(uint32 fault_type);
unsigned long arkrtos_hardfault_handler(void);
#if (ARKRTOS_USE_PAGING_MPROTECT == 1)
/**
* @brief 摘掉当前任务并切走，不重启（U-22：mprotect 违规写的处置）
* @param fault_type 故障类型（只用于记录）
* @return 可恢复的任务栈指针（非 0 时由板级层 resume_task 完成切换）；0 = 无可切换任务
* @note 与 arkrtos_cpu_fault_handler 的唯一差别是**不重启**：重启会回到同一条
*       违规指令上，形成无限重启。fork 出来的子任务被摘掉后，父进程 wait4 照常
*       收尸（收尾里调了 arkrtos_linux_abi_on_task_exit）。只有
*       ARKRTOS_USE_PAGING_MPROTECT=1 时才存在这个符号。
*/
unsigned long arkrtos_cpu_fault_kill(uint32 fault_type);
#endif

#endif
