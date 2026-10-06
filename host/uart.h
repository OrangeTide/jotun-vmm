/* uart.h : minimal 16550 UART, enough for a guest console on COM1 */

#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>
#include "vmm.h"

/* COM1 base port. The UART occupies this port and the seven above it. */
#define UART_BASE 0x3f8

/* MMIO console for arm64 guests, which have no port I/O. A small PL011 subset
 * sits at MMIO_UART_BASE; a store to MMIO_POWEROFF stops the VMM. Both regions
 * are left unmapped in the guest so accesses fault out as MMIO exits. */
#define MMIO_UART_BASE 0x09000000ull
#define MMIO_UART_SIZE 0x1000
#define MMIO_POWEROFF  0x09010000ull

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

/* Service an MMIO exit against the PL011-style console. Returns true if the
 * address belongs to the console (byte emitted on write, status returned on
 * read), false otherwise. The poweroff register is handled by the caller. */
bool uart_mmio(vmm_exit_info_t *exit);

#endif /* UART_H */
