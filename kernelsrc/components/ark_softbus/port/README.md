# ports/arkrtos — Ark RTOS port

Integration of the bus core onto Ark RTOS. The port is intentionally thin: the
bus needs a millisecond clock and a critical section, nothing else.

## Files

| File | Role |
|------|------|
| `ark_bus_port_arkrtos.h` | The three hooks the board fills in. |
| `ark_bus_hal_arkrtos.c` | Wires those hooks into `ark_bus_hal_t`. No Ark RTOS headers. |

## What you implement on the board

```c
#include "ark_bus_port_arkrtos.h"

uint32_t ark_bus_port_tick_ms(void)
{
    /* Ark RTOS tick counter times the tick period in ms. */
    return (uint32_t)arkrtos_tick_get() * (1000u / ARKRTOS_TICK_HZ);
}

void ark_bus_port_lock(void)   { arkrtos_sched_lock(); }
void ark_bus_port_unlock(void) { arkrtos_sched_unlock(); }
```

Then, during start-up:

```c
ark_bus_port_arkrtos_install();      /* installs the HAL */
ark_bus_init(&g_bus, uart_adapter);  /* the rest is core code */
ark_bus_start(&g_bus, MY_DEVICE_ID, DEV_TYPE, CAPS);
```

## Checklist

- [ ] `ark_bus_port_tick_ms()` is monotonic and never wraps in under 49 days
      (the core compares tick values with unsigned subtraction, so a single
      wrap is handled, but wildly non-monotonic clocks are not).
- [ ] `ark_bus_port_lock()` / `unlock()` nest the way your scheduler expects:
      the core never nests them itself.
- [ ] On a bare-metal build with no threads, both can be empty and the bus runs
      from a single poll loop.
- [ ] The UART transport's `feed_byte()` is called from the RX ISR; everything
      else (`ark_bus_poll()`) runs from a task.
