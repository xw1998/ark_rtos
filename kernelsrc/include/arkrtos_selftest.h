/**
* @file arkrtos_selftest.h
* @brief Startup and runtime self-test (SEC-G14): RAM window, CPU core,
*        image CRC, clock cross-check.
* @details IEC 61508 的诊断覆盖率问的不是「有没有测过」，而是「开机与运行期
*          各自能发现哪一类失效」。本模块给四类，每一类都写清覆盖范围与
*          它看不见什么：
*
*          - RAM 窗口（ITEM_RAM）：在一块**专用**静态窗口上跑五种图案
*            （全 0 / 全 1 / 0xAA / 0x55 / 地址相关）。地址相关那一种把
*            「哪个地址」编进数据，能发现地址线粘连（两块地址读出同一份
*            数据）；真要看整片 RAM，得在启动早期、还没人用内存时扫，
*            那种模式不在本模块里 —— 它会毁掉运行中的状态。
*          - CPU 核心（ITEM_CPU）：12 条已知答案的算术/位运算/符号扩展
*            向量。它只证明「数据通路在最基本的操作上还没坏」，不是
*            CPU 覆盖率证据。
*          - 镜像 CRC（ITEM_FLASH）：对板级给出的只读区间算 crc32，与
*            开机算出来的那一份比对。它发现的是「运行期这段只读内存
*            变了」，不是「出厂时写进去的就是对的」（那需要 SEC-G12 的
*            签名链）。
*          - 时钟交叉校验（ITEM_CLOCK）：OS 节拍计数与板级第二路独立
*            计数器互相校。第一次采样学比值，之后每次巡检按容差比。
*            任一路停住即 FAIL。
*
*          板级不实现钩子就是「这板子没有这路读数」——那一项报 SKIP，
*          不报 PASS。诊断项的 SKIP 与 PASS 在 61508 里是两件事。
*
* @note 默认关（ARKRTOS_USE_SELFTEST = 0）。开档才会编进内核；关档编成
*       空 TU，用 nm 查不到任何已定义全局符号。
*/

#ifndef ARKRTOS_SELFTEST_H
#define ARKRTOS_SELFTEST_H

#include "arkrtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 项位（可以多选一起跑）
 * ------------------------------------------------------------------ */
#define ARKRTOS_SELFTEST_ITEM_RAM        (1u << 0)   /**< RAM 图案窗口 */
#define ARKRTOS_SELFTEST_ITEM_CPU        (1u << 1)   /**< CPU 已知答案向量 */
#define ARKRTOS_SELFTEST_ITEM_FLASH      (1u << 2)   /**< 只读区间 crc32 */
#define ARKRTOS_SELFTEST_ITEM_CLOCK      (1u << 3)   /**< 节拍 × 第二路计数器 */
#define ARKRTOS_SELFTEST_ITEM_ALL        (0x0Fu)

/* ------------------------------------------------------------------
 * 判定
 * ------------------------------------------------------------------ */
#define ARKRTOS_SELFTEST_PASS            (0u)   /**< 测了，通过 */
#define ARKRTOS_SELFTEST_FAIL            (1u)   /**< 测了，不通过 */
#define ARKRTOS_SELFTEST_SKIP            (2u)   /**< 没测（板级没给读数），既不是通过也不是失败 */

/* ------------------------------------------------------------------
 * 返回码
 * ------------------------------------------------------------------ */
#define ARKRTOS_SELFTEST_OK              (0)    /**< 全部 PASS 或 SKIP，没有 FAIL */
#define ARKRTOS_SELFTEST_ERR_PARAM       (-1)   /**< 参数非法（mask 越界 / 输出指针为空） */
#define ARKRTOS_SELFTEST_ERR_OFF         (-2)   /**< 开关关着 */
#define ARKRTOS_SELFTEST_ERR_FAIL        (-3)   /**< 有项 FAIL（报告里带明细） */

/** @brief 单项结果 */
typedef struct
{
    uint32 id;          /**< ARKRTOS_SELFTEST_ITEM_xxx */
    uint8  verdict;     /**< PASS / FAIL / SKIP */
    uint32 detail;      /**< RAM: 1 起的图案号；CPU: 1 起的向量号；
                         *   FLASH: 本次 crc32；CLOCK: 漂移 ppm（SKIP 时 0） */
} arkrtos_selftest_item_t;

/** @brief 一次运行的完整报告（固定四项，栈上放得下） */
typedef struct
{
    uint32 items;       /**< item[] 里有效条数 */
    uint32 failed;      /**< FAIL 项的位掩码 */
    uint32 skipped;     /**< SKIP 项的位掩码 */
    arkrtos_selftest_item_t item[4];
} arkrtos_selftest_report_t;

/**
* @brief 跑指定项，结果写进 @p out。
* @param mask ARKRTOS_SELFTEST_ITEM_xxx 的或
* @param out  结果（不可为空）
* @return OK / ERR_PARAM / ERR_OFF / ERR_FAIL
* @note 可在任务上下文与启动早期调用；不分配内存、不碰文件系统。
*/
int32 arkrtos_selftest_run(uint32 mask, arkrtos_selftest_report_t *out);

/**
* @brief 运行期巡检：重算 RAM 窗口、只读区间 crc32 与开机值比对、时钟比值。
* @param out 结果（可为空；为空时只返回码）
* @return 同 arkrtos_selftest_run()
* @note 巡检的 RAM 只测那块专用窗口 —— 运行中的其它内存不能写，写就是
*       制造故障而不是发现故障。
*/
int32 arkrtos_selftest_patrol(arkrtos_selftest_report_t *out);

/**
* @brief 开机那次算出的只读区间 crc32。
* @return crc32；没算过（关档 / 板级没给区间）返回 0。
*/
uint32 arkrtos_selftest_flash_crc(void);

/**
* @brief 忘掉已学到的基线（只读区间 crc 与时钟比值）。
* @details 受控更新之后要重新学基线时用；忘了之后第一次巡检只学不比，
*          那一项报 SKIP。
* @return ARKRTOS_SELFTEST_OK
*/
int32 arkrtos_selftest_forget(void);

/**
* @brief 开机自检入口：跑全部项，有 FAIL 就记日志与持久审计。
* @return 失败的位掩码（0 = 没有失败）
* @note 由 arkrtos_init 在调度启动前后调（见 arkrtos_init.c 的接线）。失败
*       不阻止启动：诊断的职责是留下证据，要不要停机是产品策略。
*/
uint32 arkrtos_selftest_boot(void);

/* ------------------------------------------------------------------
 * 板级钩子（弱符号；不实现 = 这项报 SKIP）
 * ------------------------------------------------------------------ */

/**
* @brief 告诉自检「本板的只读镜像区间在哪」。
* @param start [out] 起始地址（物理地址 / 运行地址，按板子的映射给）
* @param len   [out] 字节长度
* @return 0 = 支持；负 = 不支持
*/
int32 arkrtos_port_selftest_flash_span(uint32 *start, uint32 *len);

/**
* @brief 第二路独立计数器（不能与 OS 节拍同源）。
* @param now [out] 当前读数（自由运行的 32 位计数，允许回绕）
* @return 0 = 支持；负 = 不支持
*/
int32 arkrtos_port_selftest_clock(uint32 *now);

#ifdef __cplusplus
}
#endif

#endif /* ARKRTOS_SELFTEST_H */
