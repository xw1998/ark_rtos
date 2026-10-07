/**
* @file arkrtos_softbus.h
* @brief 软总线（ark_softbus）在内核里的接入面
* @details 组件本体是 ark_softbus 源仓库的同步副本（kernelsrc/components/
*          ark_softbus，见那边的 VENDOR.md）。内核这一层只做三件事：
*            1) 把板级三钩子装进总线 HAL（board/<板>/arkrtos_bus_port.c）；
*            2) 用一条**板内 loopback 链路**把本节点与一个常驻对端节点接起来，
*               让总线在只有一块板、一根控制台串口的情况下也能被真实驱动；
*            3) 提供控制台命令 `bus`（自检 / 信息 / 收发 / 对端）。
*
*          【大小档位】MCU 档不是总线头里的默认值，而是由工程侧 -D 指定，
*          默认值（32 topic / 256B 载荷 / 全层开启）在 128KB RAM 的板上
*          一个总线实例就要 6116B，两个实例放不下。工程必须带上：
*
*            -DARK_BUS_MAX_TOPICS=12        -DARK_BUS_MAX_DEVICES=4
*            -DARK_BUS_MSG_MAX_SIZE=128     -DARK_BUS_TOPIC_NAME_MAX=24
*            -DARK_BUS_QOS_MAX_SESSIONS=2
*            -DARK_BUS_RPC_MAX_SLOTS=4      -DARK_BUS_RPC_MAX_METHODS=4
*            -DARK_BUS_TASK_ENABLED=0       -DARK_BUS_COLL_ENABLED=0
*            -DARK_BUS_INFER_ENABLED=0
*            -DARK_BUS_STREAM_TX_SLOTS=1    -DARK_BUS_STREAM_RX_SLOTS=1
*            -DARK_BUS_STREAM_RX_MAX=256    -DARK_BUS_STREAM_MAX_BYTES=256
*
*          下面的 #error 就是这条约定的闸门：档位一改就必须两处同时改，
*          否则编译期报错，而不是悄悄多占几 KB RAM（掉进「默认值与工程值
*          不同档」的坑：自测全绿、上板 RAM 不够）。
*
* @note 整个文件、以及 arkrtos_softbus.c / arkrtos_bus_port.c 只在
*       ARKRTOS_USE_SOFTBUS=1 时参与编译；为 0 时体积与行为零变化。
* @author xw
* @date 2026.10.07
*/

#ifndef __ARKRTOS_SOFTBUS_H__
#define __ARKRTOS_SOFTBUS_H__

#include "arkrtos_features.h"
#include "arkrtos_types.h"

#if (ARKRTOS_USE_SOFTBUS == 1)

#include "ark_bus.h"

#if ((ARK_BUS_MSG_MAX_SIZE != 128u) || (ARK_BUS_MAX_TOPICS != 12u) || \
     (ARK_BUS_MAX_DEVICES != 4u) || (ARK_BUS_STREAM_RX_MAX != 256u) || \
     (ARK_BUS_TASK_ENABLED != 0) || (ARK_BUS_COLL_ENABLED != 0) || \
     (ARK_BUS_INFER_ENABLED != 0))
#error "ark_softbus MCU 档未生效：本工程缺少 -DARK_BUS_* 档位宏（清单见 arkrtos_softbus.h 顶部注释）"
#endif

/**
* @brief 建立板内软总线（幂等）
* @details 装 HAL、建 loopback 链路、起本节点与常驻对端节点。
*          重复调用只会把两个节点复位重来。
* @return 0 = 成功，负值 = 失败
*/
int32 arkrtos_softbus_init(void);

/**
* @brief 推进一次总线事件循环（本节点 + 常驻对端各一轮）
* @param rounds 轮数，0 表示 1 轮
* @return 0
*/
int32 arkrtos_softbus_pump(uint32 rounds);

/**
* @brief 打印本节点、常驻对端与计数
* @return 0
*/
int32 arkrtos_softbus_info(void);

/**
* @brief 端到端自检：协议编解码、发现、发布/订阅、定向+ACK、QoS 重传、
*        RPC 请求应答、数据平面分块、错误路径
* @return 失败项数（0 = 全部通过）；每项一行 PASS/FAIL 输出到控制台
*/
int32 arkrtos_softbus_selftest(void);

/**
* @brief 控制台命令 `bus` 的处理器（内核 Shell 命令表里注册的就是它）
*/
int arkrtos_softbus_shell_cmd(int argc, char **argv);

#endif /* ARKRTOS_USE_SOFTBUS */

#endif /* __ARKRTOS_SOFTBUS_H__ */
