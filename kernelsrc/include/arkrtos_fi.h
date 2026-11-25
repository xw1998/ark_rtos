/**
* @file arkrtos_fi.h
* @brief Fault injection and tamper watch (SEC-G16, the software-reachable half)
* @details IEC 61508 的故障注入问的是「诊断项声称能发现的那类失效，注入进去
*          之后真的被发现了吗」。没有这一步，诊断覆盖率只是一张清单。本模块
*          给两件东西，各自的能力边界写死在下面：
*
*          一、定点故障注入（三个点，都是内核里的一条**决定**，不是内存里的
*              一个字节）：
*                - ARKRTOS_FI_KIND_ALLOC：固定块内存池这一次分配按失败返回；
*                - ARKRTOS_FI_KIND_TASK ：线程创建这一次按失败返回（不占槽位）；
*                - ARKRTOS_FI_KIND_TICK ：这一次节拍不前进（时间少一拍）。
*              注入有预算：武装 n 次，用满自动撤防，不会「忘记关掉」把系统
*              留在坏状态里。预算用完之前的每一次注入都记 fired 与 tick。
*
*              这三个点覆盖的是**错误路径**：分配失败后调用方是否按错误处理、
*              创建失败后有没有泄漏槽位、少一拍之后第二路时钟是否看得出来。
*              它们**不是**位翻转：往别人的内存里写一位属于破坏而不是注入，
*              那种失效要在硬件层做（电压/时钟毛刺、Rowhammer 一类），
*              本模块不声称覆盖 —— 见 docs/已知限制.md。
*
*          二、篡改监视（watch）：调用方登记一段内存（最多 ARKRTOS_FI_WATCH_MAX
*              段），本模块记下 crc32 基线，之后 scan 重新算并比对。它发现的是
*              「这段内存在登记之后被改过」；发现不了「登记的那一刻就已经是
*              伪造的」—— 后者要签名链（SEC-G12 / SEC-G09）。命中一次写一条
*              持久审计记录（kind ARKRTOS_AUDIT_EV_TAMPER），开审计才有落点。
*
*          两半都默认关（ARKRTOS_USE_FAULT_INJECT = 0）：关档编成空 TU，
*          三个 hook 站点整段不参与编译，镜像逐字节不变。开关打开也只在内核
*          里记账，唯一会改变行为的是「武装之后的那几次调用」。
*
* @note 内核内部 + 控制台用。App 不能通过 SVC 注入 —— 那是给自己开后门。
* @author xw
*/

#ifndef ARKRTOS_FI_H
#define ARKRTOS_FI_H

#include "arkrtos_types.h"
#include "arkrtos_features.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 注入点（位掩码，可以一次武装多个）
 * ------------------------------------------------------------------ */
#define ARKRTOS_FI_KIND_ALLOC   (1u << 0)   /**< 固定块池分配失败 */
#define ARKRTOS_FI_KIND_TASK    (1u << 1)   /**< 线程创建失败 */
#define ARKRTOS_FI_KIND_TICK    (1u << 2)   /**< 本次节拍不前进 */
#define ARKRTOS_FI_KIND_ALL     (0x07u)

/** @brief arm 时预算传 0 表示用这个默认值（1 次） */
#define ARKRTOS_FI_DEFAULT_BUDGET   (1u)

/** @brief 篡改监视同时能登记的段数 */
#define ARKRTOS_FI_WATCH_MAX        (4u)

/** @brief 自检返回值：三个注入点之外，篡改监视那一条也过了 */
#define ARKRTOS_FI_SELFTEST_WATCH   (1u << 3)
#define ARKRTOS_FI_SELFTEST_ALL     (ARKRTOS_FI_KIND_ALL | ARKRTOS_FI_SELFTEST_WATCH)

/* ------------------------------------------------------------------
 * 返回码
 * ------------------------------------------------------------------ */
#define ARKRTOS_FI_OK           (0)
#define ARKRTOS_FI_ERR_PARAM    (-1)   /**< kind / index / 地址 / 长度 非法 */

/** @brief 单个注入点的读数 */
typedef struct
{
    uint32 armed;       /**< 剩余预算（0 = 已撤防） */
    uint32 fired;       /**< 累计注入次数（clear 之前一直累加） */
    uint32 last_tick;   /**< 最后一次注入时的内核节拍 */
} arkrtos_fi_slot_t;

/** @brief 一段篡改监视的读数 */
typedef struct
{
    uint32 addr;        /**< 登记的起始地址 */
    uint32 len;         /**< 长度（字节） */
    uint32 baseline;    /**< 登记时算出的 crc32 */
    uint32 scans;       /**< 比对次数 */
    uint32 hits;        /**< 检出次数（内容与基线不符） */
    uint32 last_crc;    /**< 最后一次比对算出的 crc32 */
} arkrtos_fi_watch_t;

/* ------------------------------------------------------------------
 * 注入
 * ------------------------------------------------------------------ */

/**
 * @brief 给一个注入点武装预算。
 * @param kind    ARKRTOS_FI_KIND_xxx；一次只能一个位（用 ALL 逐个武装）。
 * @param budget  注入几次；0 表示 ARKRTOS_FI_DEFAULT_BUDGET。
 * @return ARKRTOS_FI_OK / ARKRTOS_FI_ERR_PARAM
 */
int32 arkrtos_fi_arm(uint32 kind, uint32 budget);

/**
 * @brief 撤防（预算清零）。已注入的次数保留在 fired 里。
 * @param kind  ARKRTOS_FI_KIND_xxx，或 ARKRTOS_FI_KIND_ALL 表示全部撤防并清计数。
 * @return ARKRTOS_FI_OK / ARKRTOS_FI_ERR_PARAM
 */
int32 arkrtos_fi_disarm(uint32 kind);

/** @brief 剩余预算（0 = 没武装） */
uint32 arkrtos_fi_armed(uint32 kind);

/** @brief 累计注入次数 */
uint32 arkrtos_fi_fired(uint32 kind);

/**
 * @brief 读一个注入点的完整读数（含 last_tick）。
 * @return ARKRTOS_FI_OK / ARKRTOS_FI_ERR_PARAM（kind 不是单个位或 out 为空）
 */
int32 arkrtos_fi_stat(uint32 kind, arkrtos_fi_slot_t *out);

/**
 * @brief 注入点的实现侧入口：返回 1 表示「这一次按失败处理」。
 * @details 只在 hook 站点调用。武装且预算未用完时返回 1 并扣减预算；
 *          预算用满的那一次之后自动撤防，之后恒返回 0。
 */
uint32 arkrtos_fi_inject(uint32 kind);

/**
 * @brief 框架自检：三个注入点各走一遍「武装 → 注入 → 预算用满 → 自动撤防」，
 *        再用模块私有的静态缓冲走一遍「登记 → 改一个字节 → 检出 → 还原」。
 * @return 通过的位掩码（期望 ARKRTOS_FI_SELFTEST_ALL）。结束时注入点全部
 *         撤防、临时监视段被摘掉，但 fired 计数与检出的日志/审计会留下 ——
 *         这就是它想留下的证据。
 * @note 篡改监视那一条用 index 0，会覆盖调用方在 0 号槽登记的段。
 */
uint32 arkrtos_fi_selftest(void);

/* ------------------------------------------------------------------
 * 篡改监视
 * ------------------------------------------------------------------ */

/**
 * @brief 登记一段要监视的内存，并把当前内容记为基线。
 * @param index  0 .. ARKRTOS_FI_WATCH_MAX-1
 * @param addr   起始地址（非 0）
 * @param len    长度（非 0，addr+len 不得回绕）
 * @return ARKRTOS_FI_OK / ARKRTOS_FI_ERR_PARAM
 */
int32 arkrtos_fi_watch(uint32 index, uint32 addr, uint32 len);

/**
 * @brief 重新算一段的 crc32 并与基线比。不符时 hits++、写审计。
 * @return 0 = 与基线一致；1 = 检出改动；-1 = 参数错 / 该段没登记。
 */
int32 arkrtos_fi_scan(uint32 index);

/** @brief 扫全部已登记的段，返回检出改动的段数（0/1 以外说明不止一段）。 */
int32 arkrtos_fi_scan_all(void);

/**
 * @brief 读一段监视的读数。
 * @return ARKRTOS_FI_OK / ARKRTOS_FI_ERR_PARAM
 */
int32 arkrtos_fi_watch_stat(uint32 index, arkrtos_fi_watch_t *out);

/** @brief 清掉所有注入点与监视段（开机调用；`fi clear` 也走这里）。 */
void arkrtos_fi_module_init(void);

/* ------------------------------------------------------------------
 * 关档时 hook 站点的替身：常量 0，编译器直接折掉整段判断。
 * 开档才把函数声明出来，免得关档时有人误以为可以调用。
 * ------------------------------------------------------------------ */
#if (ARKRTOS_USE_FAULT_INJECT == 1)
#define ARKRTOS_FI_ENABLED  1
#else
#define ARKRTOS_FI_ENABLED  0
#define arkrtos_fi_inject(kind)   (0u)
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARKRTOS_FI_H */
