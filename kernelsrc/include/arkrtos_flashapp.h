/**
* @file arkrtos_flashapp.h
* @brief 阶段 3c：把 arkc 产物烧进**片内 Flash**，并由内核引导器跳转执行
* @details 与 app run（L1 固定窗口，arkrtos_apprun.h）的差别只有一句话：
*          - app run 跑的是**读进 RAM 的文件**：代码从 /tmp 拷到 8 KB 代码窗，
*            掉电即失，每次跑都要重新读一遍。
*          - flash app 跑的是**已经烧在 Flash 里的那份**：代码留在片内 Flash
*            应用区（ARKRTOS_FLASHAPP_BASE），内核只做三件事——校验元数据与
*            向量表、借数据窗口、起任务。掉电不丢，复位后不用重新烧。
*
*          两条路径共用同一段 RAM 数据窗口（ARKRTOS_APPRUN_RAM_*），所以
*          同一时刻只跑得了一个：这不是额外闸门，而是池分配器自己会拒绝第二次。
*
*          产物形状（宿主侧 `cc --scatter <flash.sct> --vector-table -o x.bin`）：
*          text/rodata 链接在 Flash 应用区，data/bss/stack 链接在 RAM 窗口，
*          文件 +0 是向量表（+0 = 初始 SP = RAM 窗口顶，+4 = Reset_Handler）。
*          Flash 应用区末尾固定 32 字节是元数据 flash word（魔数/长度/CRC32），
*          见分区头 ARKRTOS_FLASHAPP_META_*。
*
* @author xw
* @date 2026.11.09
*/
#ifndef __ARKRTOS_FLASHAPP_H__
#define __ARKRTOS_FLASHAPP_H__

#include "arkrtos_def.h"

/* 本路径独有的错误码。-40.. 这段故意与 apprun（-20..-23）和 apppack（-30..-34）
 * 分开：三条路径的失败原因不同，共用一个数字会让现场只能靠猜。 */
#define ARKRTOS_FLASHAPP_ERR_BUSY     (-40)  /* 已经有一个直跑程序在跑 */
#define ARKRTOS_FLASHAPP_ERR_WINDOW   (-41)  /* 数据窗口拿不到（被占 / 池表满） */
#define ARKRTOS_FLASHAPP_ERR_LAYOUT   (-42)  /* 产物不是给这个 Flash 区链的（SP/入口越界） */
#define ARKRTOS_FLASHAPP_ERR_PROLOGUE (-43)  /* 入口首半字不是 arkc 序言 */
#define ARKRTOS_FLASHAPP_ERR_META     (-44)  /* 元数据无效：这里没有烧过产物（或被擦过） */
#define ARKRTOS_FLASHAPP_ERR_VERIFY   (-45)  /* 回读比对 / CRC 不符：写进去的和文件不一样 */
#define ARKRTOS_FLASHAPP_ERR_FLASH    (-46)  /* 底层擦/写失败 */

/**
* @brief 把文件系统里的一个 arkc 裸产物烧进片内 Flash 应用区
* @param path 卷内绝对路径，如 "/tmp/f1.bin"
* @param p_len 非空时写回本次写入的负载长度（字节）
* @return 0=已写入并校验通过；负值为 ARKRTOS_FLASHAPP_ERR_x /
*         ARKRTOS_LOADER_ERR_x / ARKRTOS_MINI_ERR_FS
* @details 顺序是「先整份读一遍验形状（长度/向量表/CRC）→ 擦整个区 →
*          分块读+写 → 写元数据 → 回读逐字节比对」。
*          先验后擦：一份明显不对的文件不该把已经能跑的那份擦掉。
*          **不回滚**：擦除一旦开始，失败就是「区里现在是空的」——这一点
*          在 shell 输出里明说，不做「假装失败没发生」的补偿。
*/
int32 arkrtos_flashapp_write(const char *path, uint32 *p_len);

/**
* @brief 擦掉 Flash 应用区（连元数据一起）
* @return 0=成功；ARKRTOS_FLASHAPP_ERR_FLASH=底层失败
*/
int32 arkrtos_flashapp_erase(void);

/**
* @brief 查 Flash 应用区里有没有一份有效产物
* @param p_len 非空时写回负载长度
* @param p_crc 非空时写回负载 CRC32
* @param p_entry 非空时写回入口地址（向量表 +4，已带 Thumb 位）
* @return 0=有效；ARKRTOS_FLASHAPP_ERR_META=没有（未烧 / 已擦 / 元数据坏）
* @details 只读元数据与向量表，不校验 CRC（CRC 要整份扫一遍，那是 write 的事）。
*          「有效」的定义是三条：元数据魔数+版本对、长度在 [8, MAX]、向量表
*          SP 等于 RAM 窗口顶。入口是否在区内由调用方（run）再核一次。
*/
int32 arkrtos_flashapp_info(uint32 *p_len, uint32 *p_crc, uint32 *p_entry);

/**
* @brief 把 Flash 里的产物跳起来（起任务，从 Flash 直接取指）
* @param task_out 非空时写回本次起跑的任务号（必须在 activate 之前带出去）
* @return 0=已启动；负值为 ARKRTOS_FLASHAPP_ERR_x / ARKRTOS_LOADER_ERR_x
* @details 顺序与 app run 一致：判并发 → 借窗 → 校验元数据与向量表 →
*          起任务（非特权、MPU 两窗口）→ 激活。
*          代码窗口记成 Flash 应用区（只读可执行），RAM 窗口记成借到的
*          数据块；任务退出时由 arkrtos_flashapp_on_task_exit() 还窗。
*/
int32 arkrtos_flashapp_run(uint32 *task_out);

/** @brief 当前在跑的 Flash 产物任务号；0 = 没有 */
uint32 arkrtos_flashapp_task_id(void);

/** @brief 主任务退出时的回收钩子（由 arkrtos_task.c 的任务退出路径调用） */
int32 arkrtos_flashapp_on_task_exit(uint32 task_id);

/** @brief 错误码的可读名字（只覆盖本路径独有的，其余交调用方） */
const char *arkrtos_flashapp_error_name(int32 rc);

#endif /* __ARKRTOS_FLASHAPP_H__ */
