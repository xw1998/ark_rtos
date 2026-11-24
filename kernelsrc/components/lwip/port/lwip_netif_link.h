/**
* @file lwip_netif_link.h
* @brief 把板级网卡（arkrtos_net_dev.h）接成 lwIP 的一张 netif
* @details 这一层只有两件事：建 netif（地址、MTU、linkoutput、input）
*          并且起一个轮询任务把收到的帧喂进协议栈。为什么单独一层：板级
*          驱动不该认识 pbuf，lwip_port.c 也不该认识 PCI —— 两者的分界线
*          就是"一整帧以太网报文"。
*
*          只在 ARKRTOS_USE_NET_VIRTIO=1 时参与编译；关档下本文件是空
*          翻译单元，lwip_port.c 的调用点也在同一个开关里。
*
* @author xw
*/

#ifndef ARKRTOS_LWIP_NETIF_LINK_H
#define ARKRTOS_LWIP_NETIF_LINK_H

#include "arkrtos_types.h"

/**
* @brief 建 netif 并注册轮询任务
* @return 0 = 成功；负值 = 网卡初始化失败（此时**不建** netif）
* @note  必须在 tcpip 线程已经起来之后调用（lwip_port.c 的服务任务第一轮），
*        否则 netif_add 与协议栈初始化会互相踩。
*/
int32 arkrtos_netif_link_start(void);

/**
* @brief 一轮收包：把驱动的接收环抽干（最多 ARKRTOS_NETIF_POLL_BUDGET 帧）
* @return 本轮交给协议栈的帧数
* @note  抽干而不是每轮一帧：一轮的节拍（1 ms）内可能有突发，只取一帧
*        会让接收环溢出，而"溢出"在读数上长得像"对端没发"。
*/
uint32 arkrtos_netif_link_poll(void);

/**
* @brief 收发计数器快照（透传驱动 + netif 层丢弃）
*/
void arkrtos_netif_link_stats(uint32 *tx_ok, uint32 *tx_drop,
                              uint32 *rx_ok, uint32 *rx_drop);

#endif /* ARKRTOS_LWIP_NETIF_LINK_H */
