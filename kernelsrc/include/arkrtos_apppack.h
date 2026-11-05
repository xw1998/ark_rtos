/**
* @file arkrtos_apppack.h
* @brief 把「同一份源码的四遍链接结果」打成 Ark RTOS 可重定位镜像（.svcm）
* @details 本文件只有纯计算：输入是四份负载字节 + 窗口参数，输出是一份镜像。
*          不读文件、不调编译器、不碰全局状态——四遍链接由调用方（板上是
*          `cc --pack`，宿主上是测试程序）负责，因此这份算法可以在 PC 上
*          与 tools/pack_app.py 逐字节对拍，不必先上板。
*
*          为什么是四遍而不是一遍：镜像被链接在标称基址上，运行期却会被装到
*          RAM 池里任意一个 2 的幂对齐的空位。代码里所有绝对地址（函数指针、
*          字符串字面量、指向自身 RAM 的指针、MOVW/MOVT 立即数对里的地址）
*          都必须随落点平移，所以打包时必须先找出「哪些位置是绝对地址」。
*          差分链接是唯一不依赖编译器配合的办法（不改上游 codegen）：
*              A = 标称基址；B = ROM 基址 +delta；C = RAM 基址 +delta；
*              D = ROM 基址 +2*delta、RAM 基址 +delta（验证用）
*          A→B 恰好平移 delta 且 A→C 不变 → ROM 类
*          A→C 恰好平移 delta 且 A→B 不变 → RAM 类
*          A、B、C 完全相同              → 不是绝对地址
*          其余情况                      → 打包拒绝（镜像不可重定位）
*          然后把表应用回 A，必须逐字节等于 D；不等就拒绝出镜像。
*          于是「这份镜像能不能搬到别的 2 的幂对齐落点」在打包阶段就被证明，
*          而不是留到板子上随机崩。
*
*          「其余情况就拒绝」是本模块唯一的行为准则：一个位置只要不能用一个
*          基址增量解释，就说明它依赖基址的方式超出模型（例如 ADR 型引用或
*          常量池里的相对偏移），此时给出任何一份能跑的镜像都只是碰巧能跑。
*
* @note 与 tools/pack_app.py 的关系：算法同源（diff_reloc / verify_reloc 的
*       C 移植），格式同一个头文件（arkrtos_app_image.h）。两者输出的镜像
*       必须一致，不一致就是其中一份错了——PC 对拍是这一条的判据。
*
* @author xw
* @date 2026.11.05
*/
#ifndef __ARKRTOS_APPPACK_H__
#define __ARKRTOS_APPPACK_H__

#include "arkrtos_def.h"
#include "arkrtos_app_image.h"

/** @brief 打包失败：参数非法（空指针 / 窗口不是 2 的幂 / payload 长度不一致） */
#define ARKRTOS_APPPACK_ERR_PARAM   (-30)
/** @brief 打包失败：某个位置无法用一个基址增量解释（不可重定位） */
#define ARKRTOS_APPPACK_ERR_DIFF    (-31)
/** @brief 打包失败：把表应用回 A 之后与 D 不逐字节相同（表是错的） */
#define ARKRTOS_APPPACK_ERR_VERIFY  (-32)
/** @brief 打包失败：表项数超过 ARKRTOS_APP_RELOC_MAX，或输出缓冲装不下 */
#define ARKRTOS_APPPACK_ERR_SIZE    (-33)
/** @brief 打包失败：A 里没有可用的向量表（+4 不是本窗口内的地址） */
#define ARKRTOS_APPPACK_ERR_ENTRY   (-34)

/**
* @brief 打包窗口：镜像被链接到的标称基址与两个窗口长度
* @details 两个 size 都必须是 2 的幂且 >= 8：落点由池的分配粒度决定，非 2 的幂
*          的窗口在运行期永远对不上，与其在板上 Camb 不如在这里拒绝。
*/
typedef struct {
    uint32 rom_base;    /* 负载的标称 ROM 基址（链接 .sct 里的 ER_IROM1 起点） */
    uint32 rom_size;    /* ROM 窗口字节数（2 的幂） */
    uint32 ram_base;    /* 负载的标称 RAM 基址（ER_IRAM1 起点） */
    uint32 ram_size;    /* RAM 窗口字节数（2 的幂），作为镜像头的 ram_size 上报给池 */
} arkrtos_apppack_win_t;

/**
* @brief 打包统计（只读输出，用于「表项数 × 4 对镜像大小的影响」这条账本）
* @details 四类计数分开报：某一类恒为 0 就说明这一类的绝对地址在测试程序里
*          根本没出现，等于那一类没被验证——这是反假绿要看的读数。
*/
typedef struct {
    uint32 n_rom;       /* 整字 ROM 类表项数 */
    uint32 n_ram;       /* 整字 RAM 类表项数 */
    uint32 n_rom_movw;  /* MOVW/MOVT 立即数对里的 ROM 类表项数 */
    uint32 n_ram_movw;  /* MOVW/MOVT 立即数对里的 RAM 类表项数 */
    uint32 entry_offset;/* 从向量表 +4 读出的入口偏移（相对负载起始） */
    uint32 image_size;  /* 负载字节数 */
} arkrtos_apppack_stats_t;

/**
* @brief 由四份链接结果打出最终镜像
* @param a         标称基址那份负载（镜像的最后内容就是它）
* @param b         ROM 基址 +delta 那份负载
* @param c         RAM 基址 +delta 那份负载
* @param d         ROM 基址 +2*delta、RAM 基址 +delta 那份负载（验证用）
* @param len       四份负载共用的长度（必须相等且为 4 的倍数）
* @param delta     差分链接的基址增量（必须 >= rom_size 且 >= ram_size，见下）
* @param win       窗口参数；ram_base 会写进镜像头的 nominal_ram_base
* @param type      镜像类型（小程序走 ARKRTOS_APP_TYPE_MINIAPP）
* @param version   镜像版本号（写进头里，仅供上层记账）
* @param out       输出缓冲；布局 = 头(256) + 重定位表 + 负载
* @param out_cap   输出缓冲容量，不够时报 ARKRTOS_APPPACK_ERR_SIZE（不截断）
* @param out_len   收镜像总长度
* @param stats     可为 0；非 0 时收四类表项计数与入口偏移
* @return 0=成功；负值为 ARKRTOS_APPPACK_ERR_x
* @note delta 要求 >= 两个窗口：delta 小于窗口时 B/C 的地址段会与 A 重叠，
*       「该位置的值恰好差 delta」就不再是「它是个绝对地址」的证据——重叠段里
*       一个普通常量也可能满足。这不是保守，是判据成立的前提。
* @note 本函数不校验 D 的来源：D 必须由同一个 delta 与同一份源码链接得到，
*       否则 verify 会失败并报 ARKRTOS_APPPACK_ERR_VERIFY（这已经足够，
*       不需要额外声明一个「你是不是真的这么链的」参数）。
*/
int32 arkrtos_apppack_build(const uint8 *a, const uint8 *b, const uint8 *c,
                          const uint8 *d, uint32 len, uint32 delta,
                          const arkrtos_apppack_win_t *win, uint32 type,
                          uint32 version, uint8 *out, uint32 out_cap,
                          uint32 *out_len, arkrtos_apppack_stats_t *stats);

/**
* @brief 错误码的可读名（给 shell 用；未知码如实回 "unknown"，不猜）
*/
const char *arkrtos_apppack_error_name(int32 code);

#endif /* __ARKRTOS_APPPACK_H__ */
