/* uart.c : minimal 16550 UART, enough for a guest console on COM1 */

#include "uart.h"

#include <stdio.h>

/* Register offsets from UART_BASE. */
#define UART_THR_RBR 0      /* THR (write) / RBR (read) / DLL when DLAB set */
#define UART_IER_DLM 1      /* IER / DLM when DLAB set */
#define UART_IIR_FCR 2      /* IIR (read) / FCR (write) */
#define UART_LCR     3      /* line control; bit 7 is DLAB */
#define UART_MCR     4      /* modem control */
#define UART_LSR     5      /* line status (read only here) */
#define UART_MSR     6      /* modem status (read only here) */
#define UART_SCR     7      /* scratch */

#define LCR_DLAB 0x80
#define LSR_THRE 0x20       /* transmit holding register empty */
#define LSR_TEMT 0x40       /* transmitter empty */

void
uart_init(uart_16550 *u)
{
    u->ier = 0;
    u->lcr = 0;
    u->mcr = 0;
    u->fcr = 0;
    u->scr = 0;
    u->dll = 0;
    u->dlm = 0;
}

bool
uart_io(uart_16550 *u, vmm_exit_info_t *exit)
{
    if (exit->io.port < UART_BASE || exit->io.port > UART_BASE + 7)
        return false;

    int off = exit->io.port - UART_BASE;
    uint8_t *d = exit->io.data;

    if (exit->io.write) {
        uint8_t v = d[0];
        switch (off) {
        case UART_THR_RBR:
            if (u->lcr & LCR_DLAB) {
                u->dll = v;
            } else {
                putchar((int)v);
                fflush(stdout);
            }
            break;
        case UART_IER_DLM:
            if (u->lcr & LCR_DLAB)
                u->dlm = v;
            else
                u->ier = v;
            break;
        case UART_IIR_FCR: u->fcr = v; break;
        case UART_LCR:     u->lcr = v; break;
        case UART_MCR:     u->mcr = v; break;
        case UART_SCR:     u->scr = v; break;
        default: break;
        }
    } else {
        uint8_t v = 0;
        switch (off) {
        case UART_THR_RBR: v = (u->lcr & LCR_DLAB) ? u->dll : 0; break;
        case UART_IER_DLM: v = (u->lcr & LCR_DLAB) ? u->dlm : u->ier; break;
        case UART_IIR_FCR: v = 0x01; break;             /* no interrupt pending */
        case UART_LCR:     v = u->lcr; break;
        case UART_MCR:     v = u->mcr; break;
        case UART_LSR:     v = LSR_THRE | LSR_TEMT; break; /* always ready */
        case UART_MSR:     v = 0xb0; break;             /* DCD|DSR|CTS */
        case UART_SCR:     v = u->scr; break;
        default: break;
        }
        d[0] = v;
    }
    return true;
}
