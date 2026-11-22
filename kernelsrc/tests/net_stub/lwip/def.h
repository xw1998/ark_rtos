/**
* @file def.h
* @brief 宿主用例用的假 lwIP 字节序宏（只给 test_net_host.c 用）
* @details arkrtos_net.c 只从这里取 htons/htonl/ntohs/ntohl 与 u16_t/u32_t。
*          假实现按真语义交换字节，用例因此可以断言"地址与端口出门时确实
*          换成了网络序"，而不是只断言"函数被调过"。
*
* @note 只在 -Ikernelsrc/tests/net_stub 时可见。
* @author xw
*/
#ifndef ARK_FAKE_LWIP_DEF_H
#define ARK_FAKE_LWIP_DEF_H

#include "lwip/sockets.h"

#define lwip_htons(x)   ((u16_t)((((u16_t)(x) & 0x00ffu) << 8) | \
                                 (((u16_t)(x) & 0xff00u) >> 8)))
#define lwip_ntohs(x)   lwip_htons(x)
#define lwip_htonl(x)   ((u32_t)((((u32_t)(x) & 0x000000ffu) << 24) | \
                                 (((u32_t)(x) & 0x0000ff00u) << 8)  | \
                                 (((u32_t)(x) & 0x00ff0000u) >> 8)  | \
                                 (((u32_t)(x) & 0xff000000u) >> 24)))
#define lwip_ntohl(x)   lwip_htonl(x)

#endif /* ARK_FAKE_LWIP_DEF_H */
