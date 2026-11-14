/*
 * ports/arkrtos/ark_bus_hal_arkrtos.c - glue between the Ark RTOS port hooks
 * and the bus HAL.
 *
 * This file carries no Ark RTOS dependency on purpose: it calls the three
 * ark_bus_port_* hooks the board provides.  That keeps it buildable in the
 * host test suite (where the hooks are mocked) as well as on the real target.
 *
 * The one exception is the "compiled out" case.  ARKRTOS_BOARD_CONFIG is
 * defined by every Ark RTOS project (the kernel cannot build without it) and
 * by no host build, so it tells the two apart: an Ark build asks its own
 * Kconfig whether the bus is in, and when ARKRTOS_USE_SOFTBUS=0 this glue
 * becomes an empty translation unit.  Without that, a trimmed kernel still
 * referenced the three hooks and died at link time with L6218E.
 */
#if defined(ARKRTOS_BOARD_CONFIG)
#include "arkrtos_features.h"
#endif

#if defined(ARKRTOS_USE_SOFTBUS) && (ARKRTOS_USE_SOFTBUS == 0)

/* Bus compiled out: no hooks to install, no symbols to pull in. */
typedef int ark_bus_hal_arkrtos_compiled_out_t;

#else

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

#endif /* ARKRTOS_USE_SOFTBUS */
