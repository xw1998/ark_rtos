/**
* @file arkrtos_apprun.h
* @brief 板上直跑（app run）：把 arkc 在板上编出来的裸可执行文件直接跑起来
* @details 与小程序（arkrtos_mini.h）的差别只有一句话：
*          - 小程序跑的是**镜像**：256 字节头 + 重定位表 + 负载，装载时按 delta 打补丁，
*            所以负载可以链接在 Flash 的标称地址上、落到池里任意一块。
*          - 板上直跑跑的是**裸文件**：就是 `cc -o` 写出来的那串字节，没有头、没有表、
*            **不做任何重定位**——它必须已经链接在窗口地址上（scatter 写明
*            ARKRTOS_APPRUN_BASE / ARKRTOS_APPRUN_RAM_BASE）。
*          代价是窗口只能有一段、同一时刻只能跑一个；换来的是「编译→执行」链路上
*          不再有镜像打包这一步，棋盘上的全部状态只有一个文件。
*
*          窗口与属性由板级分区头 ARKRTOS_APPRUN_* 给出（config/<chip>/arkrtos_partition.h）：
*          代码块可执行（RAMX）、数据块 RW 不可执行，两块各自一个 MPU region。
*          内存照样从 SLOT_RAM 池里借、退出照样还——借的是**指定的**那两块，
*          借不到就报错：绝不悄悄换到别的地址上去跑一份按固定地址链接的代码。
*
* @author xw
* @date 2026.11.05
*/
#ifndef __ARKRTOS_APPRUN_H__
#define __ARKRTOS_APPRUN_H__

#include "arkrtos_def.h"

/* 本路径独有的三个错误码；其余（NOMAGIC 以外的失败原因）沿用
 * ARKRTOS_LOADER_ERR_x 与 ARKRTOS_MINI_ERR_FS，免得同一种错误在三条路径上
 * 报出三个名字。 */
#define ARKRTOS_APPRUN_ERR_BUSY     (-20)  /* 已有一个在跑（同一时刻只允许一个） */
#define ARKRTOS_APPRUN_ERR_WINDOW   (-21)  /* 窗口被别人占着：拿到的基址不是窗口基址 */
#define ARKRTOS_APPRUN_ERR_PROLOGUE (-22)  /* 文件首半字不是 arkc 序言：不是一份 arkc 产物 */
/* 镜像自带的向量表不是按本窗口链出来的（SP 不是窗口顶、或入口不在代码窗口内）。
 * 与 ERR_WINDOW 分开：那一条说的是「池里没拿到这块地」，这一条说的是「拿到了，
 * 但这份文件不是给这个地址编的」——两种情况的处理办法完全不同（前者等上一个
 * 程序退出，后者去改 .sct 重编），所以不能共用一个错误码。 */
#define ARKRTOS_APPRUN_ERR_LAYOUT   (-23)

/**
* @brief 把文件系统里的一个裸可执行文件装进窗口并起任务
* @param path 卷内绝对路径，如 "/tmp/a.bin"
* @return 0=已启动；负值为 ARKRTOS_LOADER_ERR_x / ARKRTOS_MINI_ERR_FS /
*         ARKRTOS_APPRUN_ERR_x
* @details 顺序是「先占窗、再读文件、再起任务」：读文件之前窗口必须已经在手，
*          否则一次失败的装载会把别人正在跑的代码覆盖掉。
*/
int32 arkrtos_apprun_run(const char *path);

/**
* @brief 同 arkrtos_apprun_run，但把本次起跑的任务号带回来
* @param task_out 非空时写回任务号（>0）
* @note 为什么需要它：app 的优先级高于 shell，arkrtos_apprun_run() 内部一
*       activate 就可能把 CPU 交给 app，而 app 可能在 shell 打印「已启动」
*       之前就自己退出了 —— 这时再用 arkrtos_apprun_task_id() 去读只能读到 0，
*       界面上就会出现「started (task 0)」这种自相矛盾的话。任务号必须在
*       activate 之前带出来。
*/
int32 arkrtos_apprun_run_ex(const char *path, uint32 *task_out);

/**
* @brief 主任务退出时的回收钩子（由 arkrtos_task.c 的任务退出路径调用）
* @return 0=已回收或与本次直跑无关；负值=归还失败（记账出错，要能被看见）
* @note 只认自己的主任务号：别的任务（内核任务、App、小程序）退出原样返回 0。
*/
int32 arkrtos_apprun_on_task_exit(uint32 task_id);

/** @brief 当前在跑的任务号；0 = 没有 */
uint32 arkrtos_apprun_task_id(void);

/** @brief 起跑时用的入口地址与两个窗口（调试 / shell 展示）；0 = 没有在跑 */
int32 arkrtos_apprun_info(uint32 *p_entry, uint32 *p_code_base, uint32 *p_ram_base);

/** @brief 错误码的可读名字（只覆盖本路径独有的三个，其余交给调用方） */
const char *arkrtos_apprun_error_name(int32 rc);

#endif /* __ARKRTOS_APPRUN_H__ */
