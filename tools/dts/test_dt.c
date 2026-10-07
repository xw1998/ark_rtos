/* Host self-test for the generated device tree (board/stm32f427/arkrtos_dt.c).
 * Compiled and run by tools/dts/test_dt.py.  It checks that the generated
 * tables compile and that the query API answers exactly what the DTS said. */
#include <stdio.h>
#include "arkrtos_dt.h"

static int fails = 0;

static void chk(int cond, const char *msg)
{
    if(!cond)
    {
        printf("FAIL: %s\n", msg);
        fails++;
    }
}

static const arkrtos_dt_node_t *by_name(const char *name)
{
    uint32 i;
    for(i = 0u; i < arkrtos_dt_count(); i++)
    {
        const arkrtos_dt_node_t *n = arkrtos_dt_node(i);
        if(n != 0 && arkrtos_dt_str_eq(n->name, name) != 0)
        {
            return n;
        }
    }
    return 0;
}

static int idx_of(const char *name)
{
    uint32 i;
    for(i = 0u; i < arkrtos_dt_count(); i++)
    {
        const arkrtos_dt_node_t *n = arkrtos_dt_node(i);
        if(n != 0 && arkrtos_dt_str_eq(n->name, name) != 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static int count_okay(const char *compat)
{
    const arkrtos_dt_node_t *n = 0;
    int c = 0;
    while((n = arkrtos_dt_find_next(compat, n)) != 0)
    {
        c++;
    }
    return c;
}

int main(void)
{
    const arkrtos_dt_node_t *u1, *gpioc, *led0, *flash, *chosen, *spi2;
    uint32 v;

    chk(arkrtos_dt_count() == 20u, "node count == 20");
    chk(arkrtos_dt_str_eq(arkrtos_dt_model(), "Ark RTOS STM32F427") != 0, "model");
    chk(arkrtos_dt_node(arkrtos_dt_count()) == 0, "node(count) == 0");

    u1 = arkrtos_dt_find("st,stm32-usart");
    chk(u1 != 0, "find usart1");
    if(u1)
    {
        chk(arkrtos_dt_str_eq(u1->name, "serial@40011000") != 0, "usart1 name");
        chk(u1->reg_base == 0x40011000u, "usart1 base");
        chk(u1->reg_size == 0x400u, "usart1 size");
        chk(u1->is_device == 1u, "usart1 is_device");
        chk(arkrtos_dt_prop_u32(u1, "current-speed", 0, &v) == 0 && v == 115200u, "usart1 speed");
    }
    /* usart2/usart3 stay disabled in the dtsi, so only one enabled node. */
    chk(count_okay("st,stm32-usart") == 1, "only one enabled usart");

    /* A missing property must be reported as such, not guessed. */
    chk(arkrtos_dt_prop_u32(u1, "no-such-prop", 0, &v) == -1, "missing property -> -1");
    chk(arkrtos_dt_prop_str(u1, "no-such-prop") == 0, "missing string prop -> 0");

    gpioc = by_name("gpio@40020800");
    chk(gpioc != 0, "gpioc present");
    chk(gpioc->status_okay == 1u, "gpioc okay");

    /* gpio: gpioa/gpioc/gpioe are enabled, gpiob/gpiod are not. */
    chk(count_okay("st,stm32-gpio") == 3, "3 enabled gpio banks");

    led0 = by_name("led0");
    chk(led0 != 0, "led0 present");
    if(led0 && gpioc)
    {
        chk(led0->is_device == 0u, "led0 has no compatible");
        chk(arkrtos_dt_prop_u32(led0, "gpios", 0, &v) == 0 && v == gpioc->phandle,
            "led0 gpios -> gpioc phandle");
        chk(arkrtos_dt_str_eq(arkrtos_dt_prop_str(led0, "label"), "status0") != 0,
            "led0 label");
    }

    /* spi1 disabled, spi2 enabled; the NOR hangs under spi2. */
    chk(count_okay("st,stm32-spi") == 1, "only spi2 enabled");
    spi2 = by_name("spi@40003800");
    flash = arkrtos_dt_find("winbond,w25q128");
    chk(flash != 0, "nor flash present");
    if(flash && spi2)
    {
        chk(flash->reg_base == 0u, "nor base");
        chk(flash->reg_size == 0x1000000u, "nor size 16MiB");
        chk((int)flash->parent == idx_of("spi@40003800"), "nor parent == spi2");
    }

    /* chosen/stdout-path must resolve to usart1's phandle. */
    chosen = by_name("chosen");
    chk(chosen != 0, "chosen present");
    if(chosen && u1)
    {
        chk(arkrtos_dt_prop_u32(chosen, "stdout-path", 0, &v) == 0 && v == u1->phandle,
            "chosen stdout-path -> usart1");
    }

    chk(arkrtos_dt_find("no,such,thing") == 0, "unknown compatible -> 0");
    chk(arkrtos_dt_find(0) == 0, "NULL compatible -> 0");

    if(fails == 0)
    {
        printf("dt_selftest: PASS\n");
        return 0;
    }
    printf("dt_selftest: FAIL %d\n", fails);
    return 1;
}
