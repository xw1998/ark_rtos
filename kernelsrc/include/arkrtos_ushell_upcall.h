/**
* @file arkrtos_ushell_upcall.h
* @brief SEC-G08 隔离执行：App 命令的 upcall 线程（ready / call / ret）
* @details 这份头是内核内部契约，**不是**给 App 用的 API：
*            - 注册路径（arkrtos_shell_ext_register）用 ready() 判断「这个空间
*              能不能把 App 回调放到自己的地址空间里以 PLV3 跑」；
*            - 跳板（arkrtos_shell_ext_trampoline）用 call() 起一次隔离执行并
*              等结果；
*            - 内核 SVC 0x23 分派用 ret() 收账并终止那条 upcall 线程。
*
*          设计见 docs/SEC-G08-隔离执行-设计.md。实现与注册表本体在同一个
*          翻译单元（kernelsrc/shell/arkrtos_shell_ushell.c）：仓库里除 QEMU
*          板级以外的四个工程（F427 GCC sources.mk + F427/F401 .uvprojx +
*          H743 sources.mk）都是**显式文件清单**，新增一个独立 .c 必须同时改
*          这四处，否则那些平台链接失败；而 arkrtos_shell_ushell.c 已经被
*          所有工程包含。取舍写在设计文档 §7。
*
* @note 只在特权级分离 + 分页 + ELF 的构建上编出实体；其它构建上每个入口都
*       回答「没有这件事」，注册路径因此继续 fail-closed（行为逐字不变）。
*/

#ifndef ARKRTOS_USHELL_UPCALL_H
#define ARKRTOS_USHELL_UPCALL_H

#include "arkrtos_config.h"
#include "arkrtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本模块是否编出实体：四个前提缺一不可。显式定义可强制打开/关闭（宿主用例
 * 用后者构造「没有这块区」的读数）。
 *
 * 这里比 M-08-2 时多要了一条 `ARKRTOS_USE_PAGING_PAGES == 1`：隔离执行要的是
 * 「按空间的那张页表」——桩与初始帧都是往**某个空间的窗口 VA** 里写，静态
 * PS=20 窗口档（PAGES=0）只有一张共享窗口，没有「按空间写」的通路，所以那
 * 一档必须继续 fail-closed，而不是编出一个写不进去的实体。 */
#ifndef ARKRTOS_USE_USHELL_ISO
#  if (ARKRTOS_USE_PRIV == 1) && (ARKRTOS_USE_PAGING == 1) && \
      (ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_ELF == 1)
#    define ARKRTOS_USE_USHELL_ISO 1
#  else
#    define ARKRTOS_USE_USHELL_ISO 0
#  endif
#endif

/* 返回码：调用方按值区分「没开隔离」「忙」「建不起任务」「超时」，
 * 每种都要落到一条不同的日志行上（设计 §4.5）。 */
#define ARKRTOS_USHELL_UP_OK           (0)    /**< rc 已取回，值在 *p_rc */
#define ARKRTOS_USHELL_UP_ERR_PARAM    (-1)   /**< 参数非法 / owner 任务号非法 */
#define ARKRTOS_USHELL_UP_ERR_NOAREA   (-2)   /**< 该空间没有 upcall 区，或 port 不给桩 */
#define ARKRTOS_USHELL_UP_ERR_BUSY     (-3)   /**< 同一空间已有 upcall 在跑 */
#define ARKRTOS_USHELL_UP_ERR_TASK     (-4)   /**< 建任务 / 信号量失败 */
#define ARKRTOS_USHELL_UP_ERR_EXEC     (-5)   /**< 写桩页或初始帧失败 */
#define ARKRTOS_USHELL_UP_ERR_TIMEOUT  (-6)   /**< 等结果超时（回调没返回） */
#define ARKRTOS_USHELL_UP_ERR_CALLER   (-7)   /**< ret() 的调用者不是 upcall 线程 */

/* 等一次回调返回的上限：App 死循环时跳板不能一起挂死。超时不强杀任务
 * （与「内核不擅自杀任务」的口径一致），只把 busy 位清掉并由调用方报错。 */
#ifndef ARKRTOS_USHELL_UP_TIMEOUT_MS
#define ARKRTOS_USHELL_UP_TIMEOUT_MS   (10000u)
#endif

/* upcall 线程的优先级：与 ELF 主任务同档，避免回调把内核任务饿死，
 * 也避免它排在 App 本体后面迟迟跑不上 CPU。 */
#ifndef ARKRTOS_USHELL_UP_TASK_PRIORITY
#define ARKRTOS_USHELL_UP_TASK_PRIORITY (12u)
#endif

/* 桩页大小 = 一个页：桩只有几条指令，但页是 PTE 的粒度。 */
#define ARKRTOS_USHELL_UP_PAGE_SIZE    (4096u)

/* 并发记账数组的上限：与分页空间数同量级即可（多出来的空间号直接拒绝）。 */
#ifndef ARKRTOS_USHELL_UP_MAX_SPACES
#  ifdef ARKRTOS_PAGING_APPS_MAX
#    define ARKRTOS_USHELL_UP_MAX_SPACES ((uint32)ARKRTOS_PAGING_APPS_MAX)
#  else
#    define ARKRTOS_USHELL_UP_MAX_SPACES (8u)
#  endif
#endif

#if (ARKRTOS_USE_USHELL_ISO == 1)

/**
* @brief 这个空间现在能不能把 App 回调隔离执行
* @param space 地址空间号
* @return 1 = 能（有 upcall 区 + port 提供桩），0 = 不能
* @note 只看「静态前提」，不看忙闲：注册那一刻恰好有一次回调在跑，不该把
*       注册拒掉。忙闲由 call() 判。
*/
int32 arkrtos_ushell_upcall_ready(uint32 space);

/**
* @brief 在本 App 的空间里起一条 upcall 线程执行 func，并等它返回
* @param space      地址空间号（= 注册者装载时所在的空间）
* @param owner_task 注册这条命令的任务号（用来继承 App 配额身份）
* @param func       回调入口（VA 必须落在该空间的 ELF 段内，由跳板先校验）
* @param argc/argv  参数，原样交给桩
* @param p_rc       成功时写回回调的返回值
* @return ARKRTOS_USHELL_UP_*
*/
int32 arkrtos_ushell_upcall_call(uint32 space, uint32 owner_task,
                                 int (*func)(int, char **argv),
                                 int argc, char **argv, int32 *p_rc);

/**
* @brief upcall 线程的返回通道（SVC 0x23）：记账后终止自己
* @param rc 回调的返回值
* @return 正常路径不返回（线程已退出）；调用者不是 upcall 线程时回
*         ARKRTOS_USHELL_UP_ERR_CALLER，且不改动任何状态
*/
int32 arkrtos_ushell_upcall_ret(int32 rc);

/* ---- port 侧契约（只有 LA64 这一档提供实体；其它 port 不编出隔离执行） ----
 * stub_supported：这一档能不能写桩（0 = 不能，注册继续 fail-closed）
 * stub：把桩指令写进 space 的 stub_va（页内），0 = 成功
 * start：在 space 里建 upcall 线程的初始帧（寄存器 a0=argc / a1=argv /
 *        a2=func / sp=upcall_sp），返回帧所在的 VA（>0），0 = 失败。
 *        帧必须在 stub_va 之上、窗口之内；space 显式传，不读「当前空间」
 *        这条会随调用时机漂移的侧信道。 */
int32 arkrtos_port_ushell_stub_supported(void);
int32 arkrtos_port_ushell_stub(uint32 space, unsigned long stub_va);
unsigned long arkrtos_port_ushell_start(uint32 space, unsigned long stub_va,
                                        unsigned long upcall_sp,
                                        int (*func)(int, char **argv),
                                        int argc, char **argv);

#else  /* ARKRTOS_USE_USHELL_ISO == 0 */

/* 关档：每个入口都回答「没有这件事」。用逗号表达式把参数吃掉，
 * 避免调用点的局部变量在关档构建里变成「定义了没用」的告警。 */
#define arkrtos_ushell_upcall_ready(space)              ((void)(space), (int32)0)
#define arkrtos_ushell_upcall_call(space, owner, fn, argc, argv, p_rc) \
    ((void)(space), (void)(owner), (void)(fn), (void)(argc), (void)(argv), \
     (void)(p_rc), (int32)ARKRTOS_USHELL_UP_ERR_NOAREA)
#define arkrtos_ushell_upcall_ret(rc)                   \
    ((void)(rc), (int32)ARKRTOS_USHELL_UP_ERR_CALLER)

#endif /* ARKRTOS_USE_USHELL_ISO */

#ifdef __cplusplus
}
#endif

#endif /* ARKRTOS_USHELL_UPCALL_H */
