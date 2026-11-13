/* uart.c : minimal 16550 UART, enough for a guest console on COM1 */

#include "uart.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

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
#define LSR_DR   0x01       /* data ready: a received byte is waiting */
#define LSR_THRE 0x20       /* transmit holding register empty */
#define LSR_TEMT 0x40       /* transmitter empty */

/* The receive ring, shared by the input thread (producer) and the vcpu threads
 * that drain it through uart_io/uart_mmio (consumers). A plain mutex-guarded
 * ring: input is a trickle of keystrokes, so there is no contention to tune
 * for. head is the write index, tail the read index; empty when they meet. */
#define RX_RING 4096
static unsigned char rx_buf[RX_RING];
static unsigned rx_head, rx_tail;
static pthread_mutex_t rx_lock = PTHREAD_MUTEX_INITIALIZER;

void
uart_rx_push(const unsigned char *p, unsigned long n)
{
    pthread_mutex_lock(&rx_lock);
    for (unsigned long i = 0; i < n; i++) {
        unsigned next = (rx_head + 1) % RX_RING;
        if (next == rx_tail)
            break;              /* ring full: drop the rest */
        rx_buf[rx_head] = p[i];
        rx_head = next;
    }
    pthread_mutex_unlock(&rx_lock);
}

/* Pop one received byte, or -1 when the ring is empty. */
static int
rx_pop(void)
{
    int v = -1;
    pthread_mutex_lock(&rx_lock);
    if (rx_tail != rx_head) {
        v = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_RING;
    }
    pthread_mutex_unlock(&rx_lock);
    return v;
}

static bool
rx_ready(void)
{
    bool r;
    pthread_mutex_lock(&rx_lock);
    r = rx_tail != rx_head;
    pthread_mutex_unlock(&rx_lock);
    return r;
}

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
        case UART_THR_RBR:
            if (u->lcr & LCR_DLAB) {
                v = u->dll;
            } else {
                int b = rx_pop();               /* RBR: next received byte */
                v = b < 0 ? 0 : (uint8_t)b;
            }
            break;
        case UART_IER_DLM: v = (u->lcr & LCR_DLAB) ? u->dlm : u->ier; break;
        case UART_IIR_FCR: v = 0x01; break;             /* no interrupt pending */
        case UART_LCR:     v = u->lcr; break;
        case UART_MCR:     v = u->mcr; break;
        case UART_LSR:                                  /* transmit always ready */
            v = LSR_THRE | LSR_TEMT | (rx_ready() ? LSR_DR : 0);
            break;
        case UART_MSR:     v = 0xb0; break;             /* DCD|DSR|CTS */
        case UART_SCR:     v = u->scr; break;
        default: break;
        }
        d[0] = v;
    }
    return true;
}

/* PL011 register offsets (the subset a polling driver touches). */
#define PL011_DR 0x00       /* data register */
#define PL011_FR 0x18       /* flag register */
#define FR_RXFE  (1u << 4)  /* receive FIFO empty */
#define FR_TXFF  (1u << 5)  /* transmit FIFO full */

bool
uart_mmio(vmm_exit_info_t *exit)
{
    uint64_t addr = exit->mmio.address;
    if (addr < MMIO_UART_BASE || addr >= MMIO_UART_BASE + MMIO_UART_SIZE)
        return false;

    uint32_t off = (uint32_t)(addr - MMIO_UART_BASE);
    uint8_t *d = exit->mmio.data;
    size_t n = exit->mmio.size;
    if (n > 4)
        n = 4;

    if (exit->mmio.write) {
        uint32_t v = 0;
        memcpy(&v, d, n);
        if (off == PL011_DR) {
            putchar((int)(v & 0xff));
            fflush(stdout);
        }
    } else {
        /* DR pops a received byte; FR reports the transmitter always ready
         * (TXFF clear) and the receive FIFO empty (RXFE) when the ring has
         * drained; every other register reads back as zero. */
        uint32_t v = 0;
        if (off == PL011_DR) {
            int b = rx_pop();
            v = b < 0 ? 0 : (uint32_t)b;
        } else if (off == PL011_FR) {
            v = rx_ready() ? 0 : FR_RXFE;
        }
        memset(d, 0, exit->mmio.size);
        memcpy(d, &v, n);
    }
    return true;
}
