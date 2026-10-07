/**
* @file arkrtos_bus_port.c
* @brief 软总线（ark_softbus）的板级端口：时钟与临界区两个钩子
* @details ark_softbus 的端口面刻意只留三个函数（见组件里的
*          port/ark_bus_port_arkrtos.h）：毫秒时钟、进入临界区、离开临界区。
*          传输层（UART / loopback）不在这里，它由使用方按需接。
*
*          这里把三个钩子打到内核本体：
*          - 毫秒时钟：arkrtos_kernel_get_tick() 是内核的节拍计数，
*            节拍周期由 ARKRTOS_TICK_PERIOD_US 决定（默认 500us，即 2000Hz）。
*            用「节拍 / 每毫秒节拍数」而不是「节拍 * 周期 / 1000」：后者在
*            500us 下 32 位乘法约 71 分钟就溢出回绕，而总线的心跳/QoS 计时
*            依赖「无符号减法比较」，一次回绕可容忍、频繁回绕不可容忍。
*          - 临界区：调度器锁（不关中断，支持嵌套）。总线的三次加锁窗口都
*            在表的增删改上，长度是几十条指令，符合调度器锁的用法。
*
* @note 只在 ARKRTOS_USE_SOFTBUS=1 时参与编译；为 0 时本文件是空翻译单元，
*       体积与行为零变化。
* @author xw
* @date 2026.10.07
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_SOFTBUS == 1)

#include "arkrtos_config.h"
#include "arkrtos_task.h"
#include "ark_bus_port_arkrtos.h"

/* 每毫秒的节拍数。周期必须整除 1000us，否则下面的整数除法会给出恒定偏差，
 * 而总线把「毫秒」当唯一时间刻度用（心跳 3s、QoS 重传 200ms 都按它算）。 */
#define ARK_BUS_TICKS_PER_MS   (1000u / ARKRTOS_TICK_PERIOD_US)

#if ((ARKRTOS_TICK_PERIOD_US == 0u) || ((1000u % ARKRTOS_TICK_PERIOD_US) != 0u))
#error "ark_softbus 端口要求 ARKRTOS_TICK_PERIOD_US 能整除 1000us"
#endif

uint32_t ark_bus_port_tick_ms(void)
{
    return (uint32_t)(arkrtos_kernel_get_tick() / ARK_BUS_TICKS_PER_MS);
}

void ark_bus_port_lock(void)
{
    arkrtos_sched_lock_internal();
}

void ark_bus_port_unlock(void)
{
    (void)arkrtos_sched_unlock_internal();
}

#endif /* ARKRTOS_USE_SOFTBUS */
