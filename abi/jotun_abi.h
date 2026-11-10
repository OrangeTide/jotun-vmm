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
 * so a store to it faults out to the host as an MMIO exit. The MMIO band sits
 * well above guest RAM (which reaches GUEST_RAM_SIZE) and below the physmap at
 * 1 GiB, so the addresses stay unbacked and keep faulting out. */
#define JOTUN_RING_BASE     0x4000u
#define JOTUN_DOORBELL      0x3a000000ull

/* SMP coordination doorbells (MMIO, so a store faults out to the host). A
 * store to IDLE blocks the calling vcpu in the host until another vcpu stores
 * to WAKE; a WAKE store carries the target vcpu id, or JOTUN_WAKE_ALL to wake
 * every vcpu. This is how an idle cpu halts and a cpu with new work rouses it,
 * without injecting an interrupt into a halted guest. */
#define JOTUN_IDLE_DOORBELL 0x3a000010ull
#define JOTUN_WAKE_DOORBELL 0x3a000018ull
#define JOTUN_WAKE_ALL      0xffffffffu

/* A store here stops the VMM (MMIO, faults out to the host). The guest uses it
 * to shut down instead of halting: with the in-kernel APIC a HLT is serviced
 * inside KVM and never reaches the host, so a clean exit needs an explicit
 * store. Must match MMIO_POWEROFF in host/uart.h. */
#define JOTUN_POWEROFF      0x39010000ull

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

/* arm64 preemption acknowledge (MMIO, faults out to the host). An x86 guest
 * acknowledges the event interrupt in its local APIC, but arm64 has no in-guest
 * interrupt controller, so the host asserts the IRQ line directly and keeps it
 * asserted until the guest's IRQ handler stores here. The store makes the host
 * lower the line, so it is not re-taken the instant the guest returns to user
 * with IRQs unmasked. Unused on x86. Used only by the JOTUN_IRQ_LINE model. */
#define JOTUN_EVENT_EOI     0x3a000030ull

/* How the preemption tick reaches an arm64 guest (jotun_bootinfo.irq_model).
 * The same guest image runs on more than one host backend, so the host states
 * which mechanism is in play:
 *   LINE  - the host asserts a bare virtual IRQ line and the guest acknowledges
 *           with an MMIO store to JOTUN_EVENT_EOI (Hypervisor.framework, which
 *           has no in-guest interrupt controller).
 *   GICV3 - a real in-kernel GICv3 delivers the interrupt and the guest
 *           acknowledges through the GIC CPU interface, ICC_IAR1_EL1 /
 *           ICC_EOIR1_EL1 (Linux/KVM, whose only injection path is the vGIC).
 * x86 guests ignore this and use their local APIC. */
#define JOTUN_IRQ_LINE      0u
#define JOTUN_IRQ_GICV3     1u
#define JOTUN_IRQ_GICV2     2u          /* GICv2 (e.g. GIC-400): MMIO CPU interface */

/* Guest-physical layout of the KVM vGIC, used by the host to place it and by the
 * guest driver to reach it. All sit in the unbacked device MMIO band (above
 * guest RAM, below the physmap), which the guest maps as Device memory and KVM
 * intercepts for the vGIC. The distributor (GICD) is shared by both GIC
 * generations. GICv3 adds a redistributor (GICR, two 64 KiB frames per cpu);
 * GICv2 instead has an MMIO CPU interface (GICC). JOTUN_EVENT_SPI is the shared
 * peripheral interrupt the host pulses for the tick (the first SPI, INTID 32). */
#define JOTUN_GICD_BASE     0x3b000000ull
#define JOTUN_GICR_BASE     0x3b100000ull   /* GICv3 redistributor */
#define JOTUN_GICC_BASE     0x3b200000ull   /* GICv2 CPU interface */
#define JOTUN_EVENT_SPI     32u

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
