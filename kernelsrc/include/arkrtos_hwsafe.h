/**
* @brief Ark RTOS 硬件安全态（"遗言"）机制 —— 内核内部头文件
* @details App 控制硬件（电机 / 机床 / 机械臂 / 继电器……）时崩溃，绝不能让硬件
*          带着崩溃前的最后一条指令继续跑。本模块把"设备归谁"和"所有者死掉时
*          怎么把它带回安全态"记在内核侧，由**内核自己**在任务死亡时执行——
*          不依赖崩溃的 App 再跑一行代码。
*
*          三个角色：
*            1. 驱动登记安全态回调：arkrtos_hwsafe_register(设备名, 中和函数)。
*               由**驱动**（受信固件）声明"这台设备的安全态是什么"，内核只负责调用。
*            2. 内核记所有权台账：设备被打开时 arkrtos_hwsafe_claim() 记一条
*               （设备名, 所有者任务号, 实例指针）。这是 App 的"遗言"——
*               它以打开设备这个动作声明了"我活着时归我用，我死了请收走"。
*            3. 任务死亡时由内核中和：arkrtos_hwsafe_on_death()（临界区内只做
*               廉价标记）→ arkrtos_hwsafe_run_pending()（退出/恢复路径里真正
*               执行回调）。
*
*          由 ARKRTOS_HW_SAFE（arkrtos_features.h，默认 0）门控：关掉时本模块
*          整体不进二进制，设备 open/close 与任务终止路径与本机制引入前完全一致。
*
* @note 这不是沙箱，也不替代 MPU：MPU 拦的是"越权访问别人的内存"，本机制处理的是
*       "合法访问过的硬件，在所有者死后必须回到安全态"。两者互补。
*/

#ifndef __ARKRTOS_HWSAFE_H__
#define __ARKRTOS_HWSAFE_H__

#include "arkrtos_def.h"
#include "arkrtos_dev.h"

/**
* @brief 设备安全态回调（"中和"函数）
* @param obj   该设备已被打开的实例（驱动的私有结构，hdr 在开头）
* @param owner 触发本次中和的（已死）任务号，仅作日志/判断用，可为 0
* @return 0 = 已带到安全态；非 0 = 驱动自报失败（内核只记账，不重试）
*
* @warning 调用契约（必须遵守，否则本机制会比不加还危险）：
*          - 运行在**任务退出 / 故障恢复路径**，中断可能处于屏蔽态；
*          - 因此必须**非阻塞**：只做有界的寄存器 / GPIO 写，不得等待、不得
*            延时、不得调用任何 SVC、不得触碰文件系统或 Flash；
*          - 不得依赖"当前运行的是哪个任务"这类上下文（此刻上下文正处在死亡
*            任务的收尾里）；
*          - 允许被并发/重入调用（内核不保证只调一次），应写成幂等。
*/
typedef int32 (*arkrtos_hwsafe_fn)(arkrtos_dev_hdr_t *obj, uint32 owner);

/* 台账与回调表容量（编译期定容，够小系统用） */
#ifndef ARKRTOS_HWSAFE_MAX_DEV
#define ARKRTOS_HWSAFE_MAX_DEV          8u
#endif
#ifndef ARKRTOS_HWSAFE_MAX_OWN
#define ARKRTOS_HWSAFE_MAX_OWN          8u
#endif

/* 模块初始化：清空回调表与所有权台账（内核启动时调用） */
void   arkrtos_hwsafe_reset(void);

/**
* @brief 驱动登记某设备的安全态回调
* @param dev_name 设备名（与 arkrtos_dev_register 用的一致，最多 7 字符）
* @param fn       中和函数，见 arkrtos_hwsafe_fn 契约
* @return 0 = 成功；-1 = 表满；-2 = 参数非法 / 同名设备已登记
*/
int32  arkrtos_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
/**
* @brief 注销某设备的安全态回调
* @param dev_name 设备名
* @return 0 = 已注销；-1 = 该设备没有登记过回调
*
* @note 设备被 arkrtos_dev_unregister() 注销时，内核会自动调用一次，避免留下
*       指向已卸载镜像的回调指针（"回调还挂着，代码已经不在了"）。
*/
int32  arkrtos_hwsafe_unregister(const char *dev_name);


/* 登记一条所有权：设备 dev_name 被任务 owner 打开（obj 为其实例）。
 * 由设备框架在 open 成功后调用；同名同 owner 重复 open 只更新实例指针。 */
void   arkrtos_hwsafe_claim(const char *dev_name, int32 owner, arkrtos_dev_hdr_t *obj);

/* 撤销一条所有权：任务 owner 正常 close 了 dev_name。
 * 由设备框架在 close 时调用；找不到对应条目则什么都不做（幂等）。 */
void   arkrtos_hwsafe_release(const char *dev_name, int32 owner);
/**
* @brief 所有者（App）为某设备留下一句"遗言"：我死后请对它执行这条 ctrl
* @param dev_name 设备名（所有者必须当前正持有该设备的 claim）
* @param owner    任务号；内核在它死亡时执行该遗言
* @param cmd/val  传给设备驱动 drv_ctrl(obj, cmd, val) 的命令与参数
* @return 0 = 已登记；-1 = 该 owner 并未拥有该设备（必须先 open 过）；-2 = 参数非法
*
* @note 这是 **App 侧** 的通道：App 的代码在它死后不可依赖，所以遗言是"一条
*       内核能替它执行的控制命令"，而不是一个回调。执行时调用的仍是**驱动**的
*       drv_ctrl（常驻代码），因此与"驱动注册回调"走的是同一条可信路径。
*       若该设备同时登记过回调，遗言优先（更具体）。
*/
int32  arkrtos_hwsafe_set_will(const char *dev_name, int32 owner,
                               int32 cmd, int32 val);

/* 撤销一句遗言；找不到对应条目则什么都不做（幂等）。返回 0 = 已撤销，-1 = 无此条目。 */
int32  arkrtos_hwsafe_clear_will(const char *dev_name, int32 owner);


/* 阶段一：把 task_id 名下的所有台账条目标记为"待中和"。
 * 只做标记，可安全地在临界区内（中断屏蔽）调用；真正执行在阶段二。 */
void   arkrtos_hwsafe_on_death(int32 task_id);

/* 阶段一（带死亡原因版）：handover != 0 表示这是**计划内交接**（小程序把控制权
 * 交给下一个版本 / 宿主主动让位）。这类死亡会跳过遗言与回调——接手方马上重新
 * open 同一台设备，此刻把硬件打到安全态是抖动而不是安全。非计划死亡(崩/强杀)
 * 一律走完整中和。 */
void   arkrtos_hwsafe_on_death_reason(int32 task_id, int32 handover);

/* 阶段二：对所有"待中和"条目执行驱动的安全态回调，随后移除条目。
 * 在任务退出 / 故障恢复路径、临界区之外调用（可带中断）。返回实际执行的回调数。 */
int32  arkrtos_hwsafe_run_pending(void);

/* 观测接口（shell / 自检用） */
int32  arkrtos_hwsafe_owner_count(void);   /* 当前台账条数 */
int32  arkrtos_hwsafe_dev_count(void);     /* 已登记安全态回调的设备数 */
int32  arkrtos_hwsafe_info(int32 idx, char *name_out, uint32 max,
                           int32 *owner_out, int32 *dead_out);
int32  arkrtos_hwsafe_registered(const char *dev_name);   /* 1=有回调；0=无 */

/* 枚举第 idx 个已登记回调的设备名（供 shell 列表）。
 * 返回 0 = 成功；-1 = idx 越界。 */
int32  arkrtos_hwsafe_dev_at(int32 idx, char *name_out, uint32 max);
/* 读第 idx 条台账上的遗言：*has_will 置 1/0，有则写 *cmd_out, *val_out。
 * 返回 0 = 成功；-1 = idx 越界。idx 口径与 arkrtos_hwsafe_info 一致。 */
int32  arkrtos_hwsafe_will(int32 idx, int32 *has_will,
                           int32 *cmd_out, int32 *val_out);

uint32 arkrtos_hwsafe_neutralized_count(void);            /* 累计中和次数 */
int32  arkrtos_hwsafe_last_owner(void);                   /* 最近被中和的任务号，0=无 */
/*
 * 枚举中和次数按通道拆分：will=执行了遗言，cb=走了设备注册的安全态回调，
 * none=设备没有安全态只删了台账，fail=执行了但驱动返回非 0（尝试过、没成功），
 * handover=计划内交接被跳过（只摘台账、没驱动硬件）。任意项可为 0。
 */
void   arkrtos_hwsafe_path_counts(uint32 *will_out, uint32 *fail_out, uint32 *cb_out,
                                  uint32 *none_out, uint32 *handover_out);

#endif /* __ARKRTOS_HWSAFE_H__ */
