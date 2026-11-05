/**
* @file drvqspi.h
* @brief STM32H743 board level QSPI NOR (W25Q64) - 读 / 写 / 擦
* @details 证据链的顺序（每一步都留了独立入口，别跳）：
*            1) 引脚复用：来自 ST 自家 CubeMX 器件库，PF6/7/10=AF9、PF8/9=AF10、
*               PG6=AF10。**同一端口上两个不同的复用号**是这个驱动第一版读全 0
*               的根因，详见 drvqspi.c 文件头。
*            2) 器件在位：0x9F 读到 0xEF4017（8 MiB）。
*            3) 数据通路双向：`nor verify` 在最后一个 4 KiB 扇区做
*               擦→全 FF→页编程→逐字节比对→再擦。三步都过才谈块设备。
*            4) 块设备：drvblk.c 用 nor0 注册，littlefs 卷从 0 起 4 MiB。
*          为什么先拿证据再叠文件系统：引脚/器件没验证就往上叠 littlefs，等于把
*          "文件系统有问题"和"根本没接上"混成一个现象——那正是本项目不给的错答案。
*
*          开关：ARKRTOS_USE_QSPI（默认 0）。关着时本文件不进编译（sources.mk
*          按 USE_QSPI 过滤），FLASH/RAM 与命令表逐字节不变。
*
* @author xw
* @date 2026.11.05
*/

#ifndef __DRVQSPI_H__
#define __DRVQSPI_H__

#include "arkrtos_types.h"

/** @brief 参数非法（空指针 / 长度 0 / 越过 8 MB） */
#define ARKRTOS_NOR_ERR_PARAM   (-1)
/** @brief 寄存器配置回读不一致，或命令超时（QUADSPI 没有在预期时间内回到非 BUSY） */
#define ARKRTOS_NOR_ERR_HAL     (-2)
/** @brief 读到的 JEDEC ID 不是 W25Q64（0xEF4017）：器件不在、或引脚对不上 */
#define ARKRTOS_NOR_ERR_ID      (-3)
/** @brief 还没成功 init 就想读 */
#define ARKRTOS_NOR_ERR_STATE   (-4)

/** @brief W25Q64 的 JEDEC ID（厂商 0xEF + 类型 0x40 + 容量 0x17 = 8 MiB） */
#define ARKRTOS_NOR_JEDEC_W25Q64  (0x00EF4017u)
/** @brief W25Q64 容量（字节） */
#define ARKRTOS_NOR_SIZE          (8u * 1024u * 1024u)

/**
* @brief 起来 QUADSPI 并读一次 JEDEC ID；已 init 过则直接返回上次结果
* @return 0 = 器件应答且 ID 正确；负值为 ARKRTOS_NOR_ERR_x
* @note 这个函数会**打印**为什么失败（HAL 返回码、读到的 ID、CR/SR 的值），
*       因为"没起来"的所有原因在串口上必须能区分开。
*/
int32 arkrtos_board_nor_init(void);

/**
* @brief 读 JEDEC ID（0x9F，1 线）
* @param jedec 收 3 字节，按 (mfr<<16)|(type<<8)|cap 组装
* @return 0 = 读到；负值为 ARKRTOS_NOR_ERR_x
*/
int32 arkrtos_board_nor_id(uint32 *jedec);

/**
* @brief 读状态寄存器 1（0x05）
*/
int32 arkrtos_board_nor_status(uint8 *sr1);

/**
* @brief 间接读（0x03，1 线地址 1 线数据），不依赖内存映射窗口
* @note bring-up 阶段先用最慢但最不挑配置的那条读命令：它的成败只说明
*       "引脚 + 片选 + 时钟通不通"，不掺 quad 模式/空周期配置的错误。
*/
int32 arkrtos_board_nor_read(uint32 addr, uint8 *buf, uint32 len);

/**
* @brief 内存映射读（进 MM 模式读一段再 abort），用于区分"芯片能应答"与
*        "0x90000000 这个窗口能不能用"
*/
int32 arkrtos_board_nor_mm_read(uint32 addr, uint8 *buf, uint32 len);

/**
* @brief 写（页编程 0x02）：自动按 256 字节页切分，调用者负责目标已擦
* @return 0 = 成功；负值为 ARKRTOS_NOR_ERR_x
* @note 契约与 arkrtos_blk 一致：只能往已擦成 0xFF 的字节写 1→0，
*       不擦就写是调用者的 bug，本层不会假装成功（写后由调用者读回比对）。
*/
int32 arkrtos_board_nor_write(uint32 addr, const uint8 *buf, uint32 len);

/**
* @brief 擦（扇区擦除 0x20，4 KiB）：addr 与 len 必须 4 KiB 对齐
*/
int32 arkrtos_board_nor_erase(uint32 addr, uint32 len);

/**
* @brief 写擦往返自检：用最后一个 4 KiB 扇区做擦→空→写→比对→再擦
* @return 0 = 四步全过
* @note **破坏性**：会擦掉 0x7FF000 起的 4 KiB。它不在文件系统卷（0..4 MiB）
*       之内，所以正常装配下不会碰到数据。
*/
int32 arkrtos_board_nor_roundtrip(void);

/** @brief 上一次失败的码（0 = 无）；给 shell 与日志用 */
int32 arkrtos_board_nor_last_error(void);

/** @brief 最近一次读到的 JEDEC ID（未读过为 0） */
uint32 arkrtos_board_nor_last_id(void);

/**
* @brief Shell 命令 "nor"
* @details nor [info | init | id | sr | read <hexaddr> [len] | mm <hexaddr> [len] |
*               regs | pins | abort | verify | er <addr> | wr <addr> [len]]
*/
int arkrtos_board_nor_shell_cmd(int argc, char *argv[]);

#endif /* __DRVQSPI_H__ */
