/**
* @file arkrtos_net_dev.h
* @brief 以太网设备端口契约：板级给内核一张网卡，内核把它接到 lwIP
* @details 这一层只搬「一整帧以太网报文」，不认识 IP、不认识 ARP，也不
*          认识 lwIP 的 pbuf。分工如此，是因为三件事的分界线本来就在这里：
*
*            1) **板上**（例如 board/qemu_loongarch64/drvnet.c）负责找到
*               控制器、把帧推进发送环、把收到的帧从接收环里取出来；
*            2) **lwIP 接入层**（kernelsrc/components/lwip/port/
*               lwip_netif_link.c）负责 netif、pbuf 与轮询节拍；
*            3) **上层**（socket 服务、ping 演示）只看见一个普通网卡。
*
*          为什么是轮询而不是中断：本移植（LoongArch64 / RISC-V）没有把外部
*          中断控制器接进来，把轮询伪装成中断只会给出假证据——读代码的人
*          会以为"没进中断"是个 bug。板上若有可用的网卡中断，实现这四个
*          钩子时改成环形缓冲 + ISR 即可，lwIP 接入层不用动。
*
*          **帧的边界**：tx/rx 的 frame 指针指向**完整以太网帧**（含 14 字节
*          目的/源 MAC 与类型/长度），不含前导码、不含 FCS；长度上限由
*          ARKRTOS_NET_DEV_FRAME_MAX 给出。驱动内部的分帧头（virtio 的
*          virtio_net_hdr）属于驱动私有，不得漏到这一层之上。
*
*          只在 ARKRTOS_USE_NET_VIRTIO=1 时被引用：关档下这些符号一个都
*          不出现在镜像里（调用点全在同一个开关里）。
*
* @author xw
*/

#ifndef ARKRTOS_NET_DEV_H
#define ARKRTOS_NET_DEV_H

#include "arkrtos_types.h"

/* 一帧的最大长度：以太网 MTU 1500 + 14 字节头 = 1514。给到 1536 是为了
 * 容纳 VLAN 标签（1518）而不必再改契约；驱动侧的接收缓冲按这个值取。 */
#define ARKRTOS_NET_DEV_FRAME_MAX    1536u

/**
* @brief 找到并初始化网卡
* @return 0 = 成功（此后 tx/rx 可用）；负值 = 失败，调用方据此**不建 netif**
* @note  可重复调用：第二次直接返回上一次的结果，不重新复位设备。
*        失败必须如实返回负值——"没找到设备"绝不能变成一个看上去正常的
*        网卡，否则上层看到的是"包发出去了但没人回"，那是另一种 bug。
*/
int32 arkrtos_net_dev_init(void);

/**
* @brief 取本卡 MAC（6 字节，驱动内静态存储）
* @return 指针；init 之前调用返回 0
*/
const uint8 *arkrtos_net_dev_mac(void);

/**
* @brief 发送一整帧
* @return 0 = 已交给硬件（不代表对方收到）；负值 = 丢弃（环满 / 未初始化）
*/
int32 arkrtos_net_dev_tx(const uint8 *frame, uint16 len);

/**
* @brief 取一整帧
* @param buf     接收缓冲
* @param max_len buf 的字节数
* @param out_len 实际帧长（成功时写）
* @return 1 = 取到一帧；0 = 暂时没有；负值 = 硬件错误（调用方计数，不重试）
* @note  每次只取一帧：一轮里取多少帧由调用方的节拍决定，驱动不擅自循环，
*        否则"收得多"和"收得慢"在读数上分不开。
*/
int32 arkrtos_net_dev_rx(uint8 *buf, uint16 max_len, uint16 *out_len);

/**
* @brief 计数器快照（全部单调递增）
* @param tx_ok   已交硬件的帧数（成功返回 0 的那些）
* @param tx_drop 发送被拒/丢弃的帧数
* @param rx_ok   已交给上层的帧数
* @param rx_drop 收到但因缓冲不足或驱动错误丢掉的帧数
*/
void arkrtos_net_dev_stats(uint32 *tx_ok, uint32 *tx_drop,
                           uint32 *rx_ok, uint32 *rx_drop);

/**
* @brief 链路是否已就绪（初始化成功且设备状态为 DRIVER_OK）
* @return 1 = 就绪；0 = 未就绪
*/
int32 arkrtos_net_dev_link_up(void);

/**
* @brief 控制器名字（一行文本，给 shell / 演示打读数用）
* @return 指向常量字符串；未初始化时返回 "none"
*/
const char *arkrtos_net_dev_name(void);

#endif /* ARKRTOS_NET_DEV_H */
