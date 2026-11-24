/*
 * ark_bus_line_port.h - the board half of the soft-bus physical line.
 *
 * The bus core is platform free: it hands bytes to a transport and asks for
 * bytes back.  Everything that touches a real UART register belongs here, on
 * the board, exactly like ark_bus_port_arkrtos.h does for the clock and the
 * critical section.
 *
 * The three hooks are deliberately polled, not interrupt driven: this port
 * (and the MCU boards that will reuse it) may have no external interrupt
 * controller wired up, and a fake interrupt that is really a poll hides the
 * one fact a reader needs.  A board with a spare UART IRQ can implement the
 * hooks with a ring buffer and an ISR without touching the bus core.
 *
 * All three are only referenced when ARKRTOS_USE_BUS_UART=1.
 */
#ifndef ARK_BUS_LINE_PORT_H
#define ARK_BUS_LINE_PORT_H

#include <stdint.h>

/* Bring the second UART up (8N1, FIFOs on, interrupts off). */
void ark_bus_line_uart_init(void);

/* Push len bytes out.  Returns len, or a negative value when the transmitter
 * never became ready (that is a wiring fault, not a slow peer). */
int ark_bus_line_uart_write(const uint8_t *data, uint16_t len);

/* Non-blocking read: returns the number of bytes taken (0 = nothing waiting),
 * never blocks and never returns a negative value for an empty FIFO. */
int ark_bus_line_uart_read(uint8_t *buf, uint16_t max_len);

/* Bytes lost because the transmit side timed out.  Monotonic; 0 on a healthy
 * line.  Exposed so a status line can tell "slow peer" from "no peer". */
uint32_t ark_bus_line_uart_tx_timeouts(void);

#endif /* ARK_BUS_LINE_PORT_H */