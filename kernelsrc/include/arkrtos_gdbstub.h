/**
* @file arkrtos_gdbstub.h
* @brief GDB 远程串行协议（RSP）目标侧代理 —— 只做「看」，不做「停」（默认关）
* @details 这份代理回答的是「目标里现在是什么」，不是「让目标停下来单步」。
*          它实现 RSP 的帧格式（`$body#cs`、校验和、ack/无 ack 模式、二进制
*          转义）与一组**只读为主**的包，让一台标准 gdb 能连上来做现场勘验：
*
*            `?`            停在哪（固定回 `S05`，本代理不改变目标状态）
*            `m ADDR,LEN`   读内存（走板级 mem_read 钩子）
*            `M ADDR,LEN:H` 写内存（走板级 mem_write 钩子）
*            `X ADDR,LEN:B` 二进制写内存（带 `}` 转义，同 `M` 的钩子）
*            `p N` / `P N=V` 读/写单个寄存器（走板级 reg_* 钩子）
*            `qRcmd,H`      监视命令，转发给板级 monitor 钩子
*            `qSupported` / `qAttached` / `qC` / `qOffsets` / `H` / `T` / `D`
*            `QStartNoAckMode`、`!`（扩展模式）、`k`（回 OK 并置位）
*
*          **明确不做**（回空包，gdb 会把它们当"不支持"，不会当成成功）：
*            `c` / `s` / `C` / `S` / `vCont` —— 继续与单步要求代理在陷阱帧
*              上接管执行流；本代理是被调用方轮询的（poll/feed），它没有
*              执行流可接管。假装支持会得到一个"gdb 以为停下了、目标其实
*              在跑"的假绿，比不支持更糟；
*            `Z0` / `z0` / 硬件断点 —— 软件断点要改内存，而改内存的钩子是
*              板级的；代理不替板级决定哪段可写；
*            `g` / `G`（整包寄存器）在板级没给 reg_count/reg_read 时回 `E01`。
*
*          **边界（必须一起读）**：
*            1) 没有任何认证。它和 gdb 之间就是一根串口/一条字节流，谁接上
*               谁能读。所以默认关，并且只应出现在调试口上，不能挂到业务
*               总线上；
*            2) 寄存器编号是**平台约定**（本代理不下发 target description），
*               换架构要改板级钩子与 gdb 侧口径；
*            3) 内存读写完全信任板级钩子；钩子拒绝（返回 <0）时代理回 `E01`
*               而不是"写成功"。
*
* @note 开关 ARKRTOS_USE_GDBSTUB 默认 0：关档下本模块是空翻译单元，不产生
*       任何符号，镜像与行为逐位不变。
*/

#ifndef __ARKRTOS_GDBSTUB_H__
#define __ARKRTOS_GDBSTUB_H__

#include "arkrtos_types.h"

#if (ARKRTOS_USE_GDBSTUB == 1)

/** @brief 一个包体（不含 `$`/`#cs`）的上限。512 是 gdb 的常用 PacketSize 下界
 *  @details 超过就丢弃到 `#` 并回 `-`：宁可让 gdb 重发/降级，也不允许一个
 *           超长包把栈上的缓冲写穿。 */
#ifndef ARKRTOS_GDBSTUB_PKT_MAX
#define ARKRTOS_GDBSTUB_PKT_MAX     512u
#endif

/** @brief 监视命令转发的输出上限（hex 编码前） */
#ifndef ARKRTOS_GDBSTUB_MON_MAX
#define ARKRTOS_GDBSTUB_MON_MAX     128u
#endif

/** @brief 计数（只增不减，reset 时才清零） */
typedef struct
{
    uint32 rx_pkt;          /* 校验和正确、被处理的包                 */
    uint32 rx_bad_cs;       /* 校验和错（已回 `-`）                   */
    uint32 rx_oversize;     /* 超长被丢弃的包                         */
    uint32 tx_pkt;          /* 发出去的包（含空包）                   */
    uint32 unsupported;     /* 明确不支持、回空包的包                 */
    uint32 mem_reads;       /* 成功的 m/X 之外的读                    */
    uint32 mem_writes;      /* 成功的 M/X 写                          */
    uint32 reg_access;      /* 成功的 p/P                             */
    uint32 monitor;         /* 成功的 qRcmd                           */
    uint32 bad_arg;         /* 参数坏、回 E01 的包                    */
    uint32 kill;            /* 收到过 k                               */
    uint8  ack;             /* 当前是否还在回 '+'（1 = 是）           */
} arkrtos_gdbstub_stat_t;

/** @brief 板级钩子。除 mem_* 外都可以留空（留空即回 E01/空包） */
typedef struct
{
    /** 取一个字节：1 = 取到，0 = 暂时没有，<0 = 传输错误 */
    int32 (*byte_get)(uint8 *b);
    /** 发一个字节（无缓冲，调用者即把整包按序写出） */
    void  (*byte_put)(uint8 b);
    /** 读内存：0 = 成功，<0 = 拒绝（不在可访问区间等） */
    int32 (*mem_read)(uint32 addr, uint8 *buf, uint32 len);
    /** 写内存：0 = 成功，<0 = 拒绝 */
    int32 (*mem_write)(uint32 addr, const uint8 *buf, uint32 len);
    /** 读第 idx 个寄存器（4 字节值）：0 = 成功，<0 = 拒绝 */
    int32 (*reg_read)(uint32 idx, uint32 *val);
    /** 写第 idx 个寄存器：0 = 成功，<0 = 拒绝 */
    int32 (*reg_write)(uint32 idx, uint32 val);
    /** 寄存器个数（0 = 本板不给寄存器） */
    uint32 (*reg_count)(void);
    /** 监视命令：把文本写进 out（cap 字节），返回写入长度；<0 = 不支持 */
    int32 (*monitor)(const char *cmd, uint32 len, uint8 *out, uint32 cap);
} arkrtos_gdbstub_ops_t;

/**
* @brief 装上钩子并复位状态机
* @param ops 钩子表（调用方保证生命周期长于本模块；至少 byte_put 非空）
* @return 0 成功，-1 参数错（ops 为空或 byte_put 为空）
*/
int32 arkrtos_gdbstub_init(const arkrtos_gdbstub_ops_t *ops);

/** @brief 清空状态机与计数（init 内部会调一次；用例也用它复位） */
void arkrtos_gdbstub_reset(void);

/**
* @brief 喂入一段字节（宿主用例、自检与"已经在手上"的传输都走这条）
* @return 消耗的字节数（正常等于 len），-1 = 未 init
*/
int32 arkrtos_gdbstub_feed(const uint8 *buf, uint32 len);

/**
* @brief 从 byte_get 拉最多 max_bytes 个字节并处理（板级主循环/调试任务用）
* @return 实际处理的字节数（>= 0），-1 = 未 init 或 byte_get 为空
*/
int32 arkrtos_gdbstub_poll(uint32 max_bytes);

/** @brief 只读计数表 */
const arkrtos_gdbstub_stat_t *arkrtos_gdbstub_stat(void);

#endif /* ARKRTOS_USE_GDBSTUB */

#endif /* __ARKRTOS_GDBSTUB_H__ */
