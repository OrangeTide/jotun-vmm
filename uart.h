/* uart.h : minimal 16550 UART, enough for a guest console on COM1 */

#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>
#include "vmm.h"

/* COM1 base port. The UART occupies this port and the seven above it. */
#define UART_BASE 0x3f8

/* Device state. Only the handful of latched registers a polling driver reads
 * back are kept; transmit goes straight to stdout and there is no receive
 * path. */
typedef struct uart_16550 {
    uint8_t ier, lcr, mcr, fcr, scr, dll, dlm;
} uart_16550;

void uart_init(uart_16550 *u);

/* Service an I/O-port exit. If the port belongs to this UART, handle it (for a
 * write, emit the byte; for a read, fill exit->io.data) and return true.
 * Otherwise touch nothing and return false. */
bool uart_io(uart_16550 *u, vmm_exit_info_t *exit);

#endif /* UART_H */
