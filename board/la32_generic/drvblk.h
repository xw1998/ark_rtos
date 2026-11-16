/**
* @file drvblk.h
* @brief LA32 参考目标板级块设备注册 - RAM 模拟的 NOR 语义设备
* @details 与 board/qemu_loongarch64/drvblk.h 同构、逐条语义一致。这一块存储
*          只回答一个问题：**在没有 Flash 控制器的平台上，文件系统 / 挂载链路
*          上面的每一层能不能照常跑**。
*
*          它不是「假装有 NOR」。三条与真实 NOR 相同的语义被严格实现：
*            - 擦除单位 4 KB，擦完那一段全是 0xFF；
*            - 写只能落在已经是 0xFF 的字节上（NOR 只能把 1 写成 0），
*              attempt 覆写已编程的字节返回 -1，不做"读-改-写"的假动作；
*            - 容量、偏移、擦除对齐全部走 arkrtos_blk 的同一套检查。
*
* @note **本文件在 LA32 上只验证到「编译 + 链接」。** QEMU 9.2.3 不含
*       loongarch32 系统模拟器，本机也没有 LA32 板子，所以下面任何运行期
*       行为都没有被观察过；介质本身在 la64 上也只有 RAM 模拟。
* @author xw
*/

#ifndef __ARKRTOS_LA32_DRVBLK_H__
#define __ARKRTOS_LA32_DRVBLK_H__

#include "arkrtos_types.h"

/** 注册名（文件系统卷挂在它上面） */
#define ARKRTOS_LA32_BLK_NAME        "ram0"

/** 容量 */
#define ARKRTOS_LA32_BLK_SIZE        (512u * 1024u)

/** 擦除单位（与 littlefs 的 block size 一致） */
#define ARKRTOS_LA32_BLK_ERASE       (4096u)

/**
* @brief 注册 ram0 到块设备层。
* @return 0 成功；-1 注册失败（表满 / 名字非法 / 描述符缺项）
*/
int32 arkrtos_board_blk_init(void);

/** @brief 已编程的字节数（写成功累计），用于证明"真的写进去了" */
uint32 arkrtos_board_blk_bytes_written(void);

/** @brief 被拒绝的覆写次数（目标字节不是 0xFF），用于证明 NOR 语义在生效 */
uint32 arkrtos_board_blk_write_rejected(void);

/** @brief 擦除次数 */
uint32 arkrtos_board_blk_erase_count(void);

#endif /* __ARKRTOS_LA32_DRVBLK_H__ */
