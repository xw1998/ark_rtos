/*
 * ark_bus_hal.h - the small hook surface between the portable core and the
 * platform.  The core only ever needs two things from the outside world: a
 * monotonic millisecond clock, and an optional critical section.
 *
 * Everything else that touches real hardware (UART registers, sockets) lives
 * behind a transport adapter, not here.
 */
#ifndef ARK_BUS_HAL_H
#define ARK_BUS_HAL_H

#include <stdint.h>

typedef struct {
    /* Monotonic millisecond counter.  Required for QoS and heartbeat timing. */
    uint32_t (*get_tick_ms)(void);

    /*
     * Optional critical section, called around core table mutations.  Both are
     * used together or neither is; a NULL pair means "single threaded, no
     * locking needed", which is the normal case on a bare-metal MCU with the
     * bus polled from one context.
     */
    void (*lock)(void);
    void (*unlock)(void);
} ark_bus_hal_t;

/*
 * Install a HAL.  `hal` must stay valid for the lifetime of the bus.  Passing
 * NULL restores the built-in fallback (tick always 0, no locking), which is
 * useful for unit tests that drive time by hand.
 */
void ark_bus_hal_set(const ark_bus_hal_t *hal);

/* Return the currently installed HAL (never NULL). */
const ark_bus_hal_t *ark_bus_hal_get(void);

/* Convenience wrappers used by the core. */
uint32_t ark_bus_tick_ms(void);
void     ark_bus_lock(void);
void     ark_bus_unlock(void);

#endif /* ARK_BUS_HAL_H */
