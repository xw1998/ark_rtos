/**
* @file arkrtos_fdt_console.h
* @brief 把 /chosen/stdout-path 变成"控制台绑到哪个设备"的判断
* @details 设备树里说"控制台是这个设备"的字段是 /chosen/stdout-path。读了
*          它不等于消费它：消费的意思是拿它去决定控制台绑到哪儿，并且在不
*          一致时**明确拒绝**——把 DTS 与板级常量对不上的事静默吞掉，等于
*          让树变成装饰品。
*
*          本模块只做判断与报告，不写硬件寄存器：控制台在 fdt 解析之前就
*          已经起来了（打印本身要用它），把地址在运行期改掉会把"谁在说话"
*          变成一个随配置变化的量。所以：
*            - 一致 -> 控制台就是树里指的这一个（OK）；
*            - 不一致 / 解析不出来 -> 保留板级默认，返回具体原因码。
*
*          开关 ARKRTOS_USE_FDT_CONSOLE 默认 0，且依赖 ARKRTOS_USE_FDT。
*
* @author xw
*/

#ifndef __ARKRTOS_FDT_CONSOLE_H__
#define __ARKRTOS_FDT_CONSOLE_H__

#include "arkrtos_types.h"
#include "arkrtos_fdt.h"

/* 失败原因：每一个都指向"树里的哪一条事实不成立"，不是"某一步失败"。 */
#define ARKRTOS_CONSOLE_OK          0
#define ARKRTOS_CONSOLE_NO_STDOUT  (-1)  /* /chosen/stdout-path 不存在 */
#define ARKRTOS_CONSOLE_BAD_PATH   (-2)  /* 值的写法不认识（不猜） */
#define ARKRTOS_CONSOLE_NO_NODE    (-3)  /* 路径/别名指向的节点不存在 */
#define ARKRTOS_CONSOLE_DISABLED   (-4)  /* 节点 status 不是 okay */
#define ARKRTOS_CONSOLE_COMPAT     (-5)  /* compatible 与板级控制台不同 */
#define ARKRTOS_CONSOLE_ADDR       (-6)  /* reg 首单元与板级控制台窗口不同 */
#define ARKRTOS_CONSOLE_BAUD       (-7)  /* stdout-path 的选项与节点 current-speed 冲突 */

/** @brief 板级期望：控制台应该是什么。字段为 0/NULL 表示"这一项不检查"。 */
typedef struct
{
    const char *compatible;   /* 例如 "ns16550a" */
    uint32      reg_base;     /* 例如 UART 窗口基址；0 = 不检查 */
} arkrtos_console_expect_t;

/** @brief 树里读出来、并与板级核对过的结果。 */
typedef struct
{
    char   path[ARKRTOS_FDT_PATH_MAX];
    char   compatible[32];
    uint32 reg_base;
    uint32 baud;      /* 0 = stdout-path 没写 */
    uint32 bits;      /* 0 = 没写 */
    char   parity;    /* 0 = 没写 */
    uint8  enabled;
    uint8  from_alias;
} arkrtos_console_bind_t;

/**
* @brief 解析 /chosen/stdout-path 并与板级期望核对。
* @param expect 期望（可为 0，表示只解析不核对）
* @param out    解析结果，失败时也尽量填（便于把"读到了什么"打出来）
* @return ARKRTOS_CONSOLE_OK 或上面某个负码
* @note 只读：不改变控制台，也不碰硬件。
*/
int arkrtos_console_from_fdt(const arkrtos_console_expect_t *expect,
                             arkrtos_console_bind_t *out);

/** @brief 原因码的稳定短名（用于日志），未知码返回 "?"。 */
const char *arkrtos_console_reason(int rc);

#endif /* __ARKRTOS_FDT_CONSOLE_H__ */
