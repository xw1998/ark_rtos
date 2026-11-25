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
*          加密档位同样由工程给出，且**必须与 Kconfig 同值**：
*
*            -DARK_BUS_CRYPTO_ENABLED=0/1   （Kconfig: ARKRTOS_USE_BUS_CRYPTO）
*            -DARK_BUS_ENCRYPT_ENABLED=0/1  （Kconfig: ARKRTOS_USE_BUS_ENCRYPT）
*            -DARK_BUS_REPLAY_ENABLED=0/1   （Kconfig: ARKRTOS_USE_BUS_REPLAY）
*
*          为什么这三个也要工程侧给：组件自己 include 的是 ark_bus_config.h，
*          看不到 Kconfig；而它的开关会改 struct ark_bus 的布局（密钥字段）。
*          只让内核这一层「知道开了加密」而组件那一层不知道，两边的结构体
*          就不是同一个类型——不是编译错误，是运行期踩内存。所以下面有第二
*          条 #error：不一致就编不过，而不是上板后才慢慢发现。
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

/* 软总线门面与 shell 命令的返回值：0/成功值为好，负值失败。
 * 原先失败处一律写裸 -1，读代码看不出「为什么失败」；下面这些名字把原因
 * 写进代码，**值不得单独修改**（多个名字共用 -1 是有意的，分名只为表意）。 */
#define ARKRTOS_SOFTBUS_ERR_INIT      (-1)   /* ark_bus_start / 初始化失败 */
#define ARKRTOS_SOFTBUS_ERR_NOTUP     (-1)   /* 软总线尚未起来，先调 init */
#define ARKRTOS_SOFTBUS_ERR_PARAM     (-1)   /* 参数/缓冲为空 */
#define ARKRTOS_SOFTBUS_ERR_PARSE     (-1)   /* 命令行数字解析失败（溢出或没有数字） */
#define ARKRTOS_SOFTBUS_ERR_USAGE     (-1)   /* 命令行参数个数不对 */
#define ARKRTOS_SOFTBUS_ERR_SUBSCRIBE (-1)   /* 订阅失败（主题表满） */
#define ARKRTOS_SOFTBUS_ERR_PUBLISH   (-1)   /* 发布失败 */
#define ARKRTOS_SOFTBUS_ERR_RPC       (-1)   /* RPC 请求发不出去 */
#define ARKRTOS_SOFTBUS_ERR_NOREPLY   (-1)   /* RPC 发出去了但对端没有回调 */
#define ARKRTOS_SOFTBUS_ERR_STREAM    (-1)   /* 流发送被拒（多半超过长度上限） */
#if (ARKRTOS_USE_SOFTBUS == 1)

#include "ark_bus.h"

#if ((ARK_BUS_MSG_MAX_SIZE != 128u) || (ARK_BUS_MAX_TOPICS != 12u) || \
     (ARK_BUS_MAX_DEVICES != 4u) || (ARK_BUS_STREAM_RX_MAX != 256u) || \
     (ARK_BUS_TASK_ENABLED != 0) || (ARK_BUS_COLL_ENABLED != 0) || \
     (ARK_BUS_INFER_ENABLED != 0))
#error "ark_softbus MCU 档未生效：本工程缺少 -DARK_BUS_* 档位宏（清单见 arkrtos_softbus.h 顶部注释）"
#endif

/* 加密档位必须与 Kconfig 同值（见顶部注释）。缺省时组件侧是 0，Kconfig 是 1
 * 的情形会在这一行被抓住；反过来组件开着、Kconfig 关着同样抓住。 */
#if ((ARK_BUS_CRYPTO_ENABLED != ARKRTOS_USE_BUS_CRYPTO) || \
     (ARK_BUS_ENCRYPT_ENABLED != ARKRTOS_USE_BUS_ENCRYPT) || \
     (ARK_BUS_REPLAY_ENABLED != ARKRTOS_USE_BUS_REPLAY))
#error "软总线加密档位与 Kconfig 不一致：工程的 -DARK_BUS_CRYPTO/ENCRYPT/REPLAY_ENABLED 必须等于 ARKRTOS_USE_BUS_CRYPTO/ENCRYPT/REPLAY"
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

#if (ARK_BUS_CRYPTO_ENABLED == 1)
/**
* @brief 给板内这条总线装预共享密钥（本节点与常驻对端都装，两者同信任域）
* @details 装了密钥之后，本节点发出的每一帧都带 HMAC-SHA256 截断标签，收到
*          的帧先验签再分发；带标签但验不过的帧、以及**根本没有标签**的帧
*          一律丢弃（fail-closed：否则攻击者把标签剥掉就能伪造 src_id）。
*          开 ARKRTOS_USE_BUS_ENCRYPT 时载荷同时用 ChaCha20 加密
*          （encrypt-then-MAC）。
*
*          对真实的第二个节点（另一块板 / PC），两端必须装同一把密钥、用
*          同一个标签长度，并各自调用自己的 set_key——本函数只装本固件内的
*          两个实例。
*
*          @note 密钥只在调用瞬间被复制进总线实例，调用方应当把自己那份
*                清掉；本函数不保留指针。长度 1..ARK_BUS_CRYPTO_KEY_MAX。
* @param key 密钥字节
* @param len 密钥长度
* @return 0 = 成功，负值 = 参数非法
*/
int32 arkrtos_softbus_set_key(const uint8_t *key, uint16 len);

/**
* @brief 卸掉密钥：总线退回「不打标签、也不拒未打标签的帧」
* @details 用于轮换密钥：先 clear 再 set，两端必须都做，中间那段时间的帧
*          会因验签失败被丢弃（这是有意的，不静默降级）。
*/
void arkrtos_softbus_clear_key(void);

/**
* @brief 当前总线的加密状态：bit0 = 已装密钥，bit1 = 编译了加密，
*        bit2 = 编译了重放窗口
*/
uint32 arkrtos_softbus_crypto_state(void);
#endif /* ARK_BUS_CRYPTO_ENABLED == 1 */

#endif /* ARKRTOS_USE_SOFTBUS */

#endif /* __ARKRTOS_SOFTBUS_H__ */
