/*
 * ark_bus_hal.c - storage for the installed HAL and the wrappers the core uses.
 *
 * When no HAL is installed the fallback reports a tick that never advances and
 * performs no locking.  That keeps unit tests deterministic: time only moves
 * when the test says so.
 */
#include "ark_bus_priv.h"

static uint32_t fallback_tick(void)
{
    return 0u;
}

static const ark_bus_hal_t g_fallback = {
    fallback_tick,
    0,
    0
};

static const ark_bus_hal_t *g_hal = &g_fallback;

void ark_bus_hal_set(const ark_bus_hal_t *hal)
{
    g_hal = hal ? hal : &g_fallback;
}

const ark_bus_hal_t *ark_bus_hal_get(void)
{
    return g_hal;
}

uint32_t ark_bus_tick_ms(void)
{
    return g_hal->get_tick_ms ? g_hal->get_tick_ms() : 0u;
}

void ark_bus_lock(void)
{
    if (g_hal->lock) {
        g_hal->lock();
    }
}

void ark_bus_unlock(void)
{
    if (g_hal->unlock) {
        g_hal->unlock();
    }
}
