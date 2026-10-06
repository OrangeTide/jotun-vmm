/* jotun_abi.h : the host and guest transport contract.
 *
 * Shared verbatim by the host and by freestanding guests, so it uses only
 * fixed-width integer types and no libc. The structures have identical layout
 * on 32-bit and 64-bit builds (no pointers, natural alignment only), which is
 * required because a 64-bit host and a 32-bit guest share them. */

#ifndef JOTUN_ABI_H
#define JOTUN_ABI_H

#include <stdint.h>

/* Fixed guest-physical layout the two sides agree on. The ring sits low in
 * guest RAM, below the guest image. The doorbell is an unmapped MMIO address,
 * so a store to it faults out to the host as an MMIO exit. */
#define JOTUN_RING_BASE     0x4000u
#define JOTUN_DOORBELL      0x0a000000ull

#define JOTUN_RING_SLOTS    8u
#define JOTUN_SLOT_SIZE     256u                    /* bytes, header included */
#define JOTUN_MSG_MAX       (JOTUN_SLOT_SIZE - 2u)  /* payload per slot */

/* Event-channel bits: the host sets pending, the guest reads and clears it.
 * For now the doorbell is a synchronous MMIO exit, so the response is ready as
 * soon as the doorbell store returns and polling is not strictly needed; the
 * pending bit is still set so the mechanism is in place for M4. */
#define JOTUN_EVT_RSP       0x1u

/* The x86 interrupt vector the host injects for guest events (timer tick,
 * cross-vcpu wakeup). The guest installs an IDT gate for it. Chosen above the
 * CPU exception range (0..31). On arm64 there is no vector; the IRQ enters
 * through the guest's exception vector instead. */
#define JOTUN_EVENT_VECTOR  0x20u

struct jotun_msg {
    uint16_t len;                   /* payload length, <= JOTUN_MSG_MAX */
    uint8_t  data[JOTUN_MSG_MAX];
};

struct jotun_ring {
    uint32_t req_head;              /* guest produces requests here */
    uint32_t req_tail;             /* host consumes requests here */
    uint32_t rsp_head;             /* host produces responses here */
    uint32_t rsp_tail;             /* guest consumes responses here */
    uint32_t evt_pending;          /* host sets, guest clears */
    uint32_t evt_mask;             /* guest sets */
    uint32_t _pad[2];
    struct jotun_msg req[JOTUN_RING_SLOTS];
    struct jotun_msg rsp[JOTUN_RING_SLOTS];
};

#endif /* JOTUN_ABI_H */
