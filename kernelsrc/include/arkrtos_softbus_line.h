/**
* @file arkrtos_softbus_line.h
* @brief 软总线物理线：两个节点、一根真线（第二条 UART，默认关）
* @details `arkrtos_softbus.c` 那条链路是**板内 loopback**：两个 ark_bus_t 实例
*          走真实的帧编解码/环形缓冲/收发路径，但字节没有出过芯片。本文件补
*          的是另一半 —— 同样的总线核心，传输层换成组件自带的 UART 适配器
*          （transport/transport_uart.c），字节真的从第二路 ns16550a 出去，
*          对端是**另一台机器实例**上的同一个内核。
*
*          分工刻意分清：
*            - 本文件（kernelsrc/src/arkrtos_softbus_line.c）只做「抽字节 →
*              喂帧解析器 → poll 总线」和一层薄封装，不含任何寄存器；
*            - 板上寄存器在 board/<板> 的线端口（QEMU 是 drvuart_line.c），
*              三个钩子由 port/ark_bus_line_port.h 声明；
*            - 策略（什么时候发什么、怎么判成功）留在用例里，不埋进内核。
*
*          **边界（必须一起读）**：
*            1) 开关 ARKRTOS_USE_BUS_UART 默认 0：关档下本文件与板上端口一起
*               编成空翻译单元，镜像体积与行为逐位不变；
*            2) 线是**轮询**的（pump 抽干 FIFO 再 poll），不是中断驱动 ——
*               LoongArch 这一版没有接外部中断控制器，写成"看起来是中断"
*               只会给出假证据；
*            3) 这里证明的是「同一内核的两个实例经物理字节线完成发现/发布/
*               RPC」，**不**证明与非 Ark 对端互通，也**不**证明真板可用
*               （真板需要一块有闲置 UART 的板子，欠账 R5 一类）；
*            4) 没有硬件流控、没有波特率编程：QEMU 的线是内存 FIFO，真板接入
*               时必须补这两条（见 docs/软总线接入.md）。
*/

#ifndef __ARKRTOS_SOFTBUS_LINE_H__
#define __ARKRTOS_SOFTBUS_LINE_H__

#include "arkrtos_types.h"
#include "arkrtos_features.h"

#if ((ARKRTOS_USE_SOFTBUS == 1) && (ARKRTOS_USE_BUS_UART == 1))

/* 借用 arkrtos_softbus.h 的档位闸门：总线 MCU 档（ARK_BUS_*）没在工程里
 * 定死时会 #error，而不是悄悄按组件默认值占掉几 KB RAM。 */
#include "arkrtos_softbus.h"
#include "ark_bus.h"
#include "ark_bus_rpc.h"

/**
* @brief 物理线的统计读数（全部单调递增）
* @details 前八项取自总线核心的 ark_bus_stats_t，后三项是本层自己的账：
*          uart_tx_timeout 由板上端口给出（发送器不 ready），msg_count 与
*          rpc_served 是本节点作为接收方看到的量。它们分开记是为了让
*          「线坏了」与「对端没答」在控制台上是两行不同的数。
*/
typedef struct
{
    uint32 node_id;        /* 本节点 id（init 时给定）                    */
    uint32 tx_frames;      /* 发出去的完整帧                              */
    uint32 tx_bytes;       /* 发出去的字节（含帧头/CRC）                  */
    uint32 rx_frames;      /* 收进来并校验通过的帧                        */
    uint32 rx_bytes;       /* 收进来的字节                                */
    uint32 rx_crc_err;     /* CRC 校验失败                                */
    uint32 rx_bad;         /* 帧结构不合法                                */
    uint32 rx_dropped;     /* 帧解析器丢掉的字节（重同步）                */
    uint32 retrans;        /* QoS1 重传次数                               */
    uint32 timeouts;       /* QoS1 超时次数（= 对端没回执的证据）         */
    uint32 uart_tx_timeout;/* 发送器等待超时（= 线/后端有问题的证据）     */
} arkrtos_softbus_line_stats_t;

/**
* @brief 在本节点上架起物理线并启动总线
* @details 装板级三钩子与线端口，把 UART 传输层接到总线核心，再以给定 id
*          启动节点。重复调用会按新 id 重新来过（先停后起，不清硬件）。
* @param node_id 本节点 id，0 非法（0 是总线保留的广播/空标识）
* @param node_type 节点类型（1 = MCU 边缘节点，2 = 执行器，见 ark_bus_types.h）
* @return 0 = 成功，负值 = 参数非法或启动失败
*/
int32 arkrtos_softbus_line_init(uint32 node_id, uint32 node_type);

/**
* @brief 推进物理线：抽干接收 FIFO → 喂帧解析器 → poll 一轮总线
* @param rounds 轮数，0 表示 1 轮
* @return 0 = 成功，负值 = 尚未 init
*/
int32 arkrtos_softbus_line_pump(uint32 rounds);

/**
* @brief 订阅一个主题（收到时回调）
* @param topic 主题名（最长 ARK_BUS_TOPIC_NAME_MAX - 1）
* @param cb 收到消息的回调
* @param user 回调上下文
* @return 主题 id（> 0）或负的 ARK_BUS_E_* 值
*/
int32 arkrtos_softbus_line_subscribe(const char *topic, ark_bus_msg_cb_t cb,
                                     void *user);

/**
* @brief 定向发布（带 QoS1：对端 ACK 前会重传）
* @param target_id 对端节点 id
* @param topic 主题名
* @param payload 载荷
* @param len 载荷长度（<= ARK_BUS_MSG_MAX_SIZE）
* @return 交给传输层的字节数，或负的 ARK_BUS_E_* 值
*/
int32 arkrtos_softbus_line_publish(uint32 target_id, const char *topic,
                                   const void *payload, uint16 len);

/**
* @brief 发一次远程调用
* @details 回复可能在若干次 pump 之后才到，回调由 ark_bus_poll 触发；没有
*          回复时回调拿到 ARK_BUS_E_TIMEOUT，本函数本身**不阻塞等待**。
* @param target_id 对端节点 id
* @param method 方法号（两端必须一致，见 ark_bus_protocol.h）
* @param params 参数
* @param len 参数长度
* @param cb 回复回调（可为 NULL）
* @param user 回调上下文
* @return 交给传输层的字节数，或负的 ARK_BUS_E_* 值
*/
int32 arkrtos_softbus_line_rpc(uint32 target_id, uint16 method,
                               const void *params, uint16 len,
                               ark_bus_rpc_cb_t cb, void *user);

/**
* @brief 注册一个远程调用方法供对端调用
* @param method 方法号
* @param handler 处理器（返回 ARK_BUS_OK 并按 rlen 给出回复）
* @param user 处理器上下文
* @return 0 = 成功，负的 ARK_BUS_E_* 值 = 失败
*/
int32 arkrtos_softbus_line_rpc_register(uint16 method,
                                        ark_bus_rpc_handler_t handler,
                                        void *user);

/**
* @brief 直接往线上写一行文本（接线核验用，不是总线帧）
* @details 只用来回答「第二路 UART 到底连上没有」这一个问题：它绕开帧编码，
*          对端任何字节都收得到。收方会把它当作坏帧丢掉并记 rx_dropped ——
*          这是预期行为，不是缺陷。
* @param lines 行数（0 表示 1 行）
* @return 写出的字节数，或负值
*/
int32 arkrtos_softbus_line_probe(uint32 lines);

/**
* @brief 取本节点 id
* @return 本节点 id，未 init 时为 0
*/
uint32 arkrtos_softbus_line_node_id(void);

/**
* @brief 读一份统计快照
* @param out 输出缓冲（不可为空）
* @return 0 = 成功，负值 = 参数非法或尚未 init
*/
int32 arkrtos_softbus_line_stats(arkrtos_softbus_line_stats_t *out);

#endif /* ARKRTOS_USE_SOFTBUS && ARKRTOS_USE_BUS_UART */

#endif /* __ARKRTOS_SOFTBUS_LINE_H__ */