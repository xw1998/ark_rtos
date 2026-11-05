/**
* @file arkvim_port_arkrtos.c
* @brief arkvim 传输端口：把编辑器接到 Ark RTOS 的控制台
* @details arkvim（kernelsrc/components/arkvim/src/，上游原样引入）只要求宿主
*          提供 arkvim_port_t 的六个函数，不包含任何 shell 头。这里用内核
*          shell 已有的平台层（kernelsrc/shell/platform.h）实现这六个函数：
*          - 发送/接收走控制台设备（SHELL_DEV_NAME），与 shell 本身同一个出口，
*            因此不会和内核日志抢串口（platform_uart_* 自己带控制台锁）；
*          - tick_ms 走内核 tick；
*          - cleanup 留空：串口由 shell 持有，编辑器不该关它。
*
*          为什么复用 platform.h 而不是直接调用 arkrtos_dev_*：
*          控制台锁与「管道/重定向捕获」两条约定都在那一层里，绕过去就会
*          在 `vim /tmp/x.c > out` 这种写法下把字符写丢一半。
*
* @note 与 shell 的关系：ark_shell 的行编辑器在调用命令处理函数前已经停止
*       读串口，编辑器独占输入直到退出，返回后行编辑器继续工作。
*/

#include "arkvim.h"
#include "arkvim_port.h"

#include "platform.h"

static void port_send_byte(uint8_t b)
{
    platform_uart_send((unsigned char)b);
}

static void port_send_string(const char *s)
{
    platform_uart_send_string(s);
}

static void port_send_buf(const uint8_t *buf, uint16_t len)
{
    platform_uart_send_buf((const unsigned char *)buf, len);
}

static int port_recv_byte(void)
{
    return platform_uart_recv();
}

static uint32_t port_tick_ms(void)
{
    return platform_tick_ms();
}

/* 静态常量：arkvim_port_init() 保存的是指针，不是拷贝，所以它必须常驻。 */
static const arkvim_port_t g_arkshell_port =
{
    port_send_byte,
    port_send_string,
    port_send_buf,
    port_recv_byte,
    port_tick_ms,
    0                   /* cleanup：控制台归 shell，编辑器不接管 */
};

void arkvim_port_init_arkrtos(void)
{
    arkvim_port_init(&g_arkshell_port);
}
