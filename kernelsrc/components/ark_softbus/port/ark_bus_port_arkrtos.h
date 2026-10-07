/*
 * ports/arkrtos/ark_bus_port_arkrtos.h - the three functions an Ark RTOS
 * integrator supplies so the bus can tell time and take a critical section.
 *
 * The port glue (ark_bus_hal_arkrtos.c) depends only on this header, never on
 * Ark RTOS headers, so it compiles anywhere and links against whatever the
 * board provides.  A concrete integration implements these three in a board
 * file:
 *
 *     uint32_t ark_bus_port_tick_ms(void)  -> arkrtos_tick_get() * tick_ms
 *     void     ark_bus_port_lock(void)     -> scheduler critical section on
 *     void     ark_bus_port_unlock(void)   -> the matching release
 */
#ifndef ARK_BUS_PORT_ARKRTOS_H
#define ARK_BUS_PORT_ARKRTOS_H

#include <stdint.h>

/* Monotonic milliseconds since boot. */
uint32_t ark_bus_port_tick_ms(void);

/* Enter / leave a critical section that protects the bus tables. */
void ark_bus_port_lock(void);
void ark_bus_port_unlock(void);

/*
 * Install the bus HAL on top of the three hooks above.  Call once during
 * system start-up, before ark_bus_init().
 */
void ark_bus_port_arkrtos_install(void);

#endif /* ARK_BUS_PORT_ARKRTOS_H */
