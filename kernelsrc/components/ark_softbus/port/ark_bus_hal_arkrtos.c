/*
 * ports/arkrtos/ark_bus_hal_arkrtos.c - glue between the Ark RTOS port hooks
 * and the bus HAL.
 *
 * This file carries no Ark RTOS dependency on purpose: it calls the three
 * ark_bus_port_* hooks the board provides.  That keeps it buildable in the
 * host test suite (where the hooks are mocked) as well as on the real target.
 */
#include "ark_bus_hal.h"
#include "ark_bus_port_arkrtos.h"

static const ark_bus_hal_t g_arkrtos_hal = {
    ark_bus_port_tick_ms,
    ark_bus_port_lock,
    ark_bus_port_unlock,
    0
};

void ark_bus_port_arkrtos_install(void)
{
    ark_bus_hal_set(&g_arkrtos_hal);
}
