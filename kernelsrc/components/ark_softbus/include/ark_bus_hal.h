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

    /*
     * Optional random source for the handshake.  Fills `out` with `len`
     * random bytes and returns 0, or returns non-zero when it cannot.  A
     * board with a hardware RNG supplies it; a board without one leaves it
     * NULL, and the handshake then refuses to run rather than mint session
     * keys from a weak source -- unless ARK_BUS_CRYPTO_ALLOW_WEAK_RNG is set
     * on purpose (see ark_bus_config.h).  A node that never negotiates a
     * session needs no random source at all.
     */
    int (*rng)(uint8_t *out, uint32_t len);
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

/* Fill `out` with `len` random bytes from the installed HAL.  Returns 0 on
 * success or -1 when no source is installed.  Only used by the handshake. */
int      ark_bus_rng_bytes(uint8_t *out, uint32_t len);

#endif /* ARK_BUS_HAL_H */
