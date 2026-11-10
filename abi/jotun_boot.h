/* jotun_boot.h : the host-to-guest boot parameter block.
 *
 * The host writes one of these into guest RAM at a fixed low address before the
 * vcpu starts; the guest library OS reads it at boot. Shared verbatim by host
 * and freestanding guest, so it uses only fixed-width integer types and a
 * fixed layout, no pointers. See jotun_abi.h for the rest of the low-memory
 * map; the boot block sits one page above the ring. */

#ifndef JOTUN_BOOT_H
#define JOTUN_BOOT_H

#include <stdint.h>

/* Fixed guest-physical address of the boot block, above the ring and below the
 * guest image. */
#define JOTUN_BOOT_BASE     0x6000u

/* Sentinel and version, so the guest can tell a block was placed and that the
 * two sides agree on the layout. */
#define JOTUN_BOOT_MAGIC    0x4a4f5442u     /* a fixed sentinel, "JOTB" */
#define JOTUN_BOOT_VERSION  4u

/* Byte budget for the packed string blob. The whole block is one page. */
#define JOTUN_BOOT_BLOB_MAX 3440u

/* Writable-mount guest prefixes. The host lists the guest-visible mountpoint of
 * each writable host mount here (for example "/out"); the guest routes a
 * create or write under one of these prefixes to the host over 9P instead of to
 * its in-RAM overlay. A handful is plenty: a build usually has one output tree. */
#define JOTUN_BOOT_RWMOUNT_MAX  8
#define JOTUN_BOOT_RWMOUNT_LEN  64

/* The parameter block. argc argument strings followed by envc environment
 * strings live in blob[], each NUL-terminated and concatenated in order;
 * blob_len is the number of bytes used. The init program is argv[0]. */
struct jotun_bootinfo {
    uint32_t magic;
    uint32_t version;
    uint64_t ram_size;              /* total guest RAM in bytes */
    uint32_t argc;                  /* argument strings in blob */
    uint32_t envc;                  /* environment strings after them */
    uint32_t blob_len;              /* bytes used in blob */
    uint32_t nrwmount;              /* writable-mount prefixes in rwmount[] */
    char     rwmount[JOTUN_BOOT_RWMOUNT_MAX][JOTUN_BOOT_RWMOUNT_LEN];
    /* Preemption-timer parameters. timer_hz is the tick rate the host drives
     * (0 when no timer is running). timer_ticks is a monotonic counter the host
     * increments once per tick, in wall time, independent of the guest's
     * interrupt state; the guest reads it for a real clock. 8-byte aligned so a
     * guest load and the host store are each atomic. */
    uint32_t timer_hz;
    uint32_t _pad_timer;
    uint64_t timer_ticks;
    /* How an asynchronous interrupt (the preemption tick) reaches an arm64
     * guest, chosen by the host backend. JOTUN_IRQ_* in jotun_abi.h. x86 guests
     * ignore it (they use the local APIC). 0 (JOTUN_IRQ_LINE) is the safe
     * default a pre-v4 or line-model host leaves. */
    uint32_t irq_model;
    uint32_t _pad_irq;
    uint8_t  blob[JOTUN_BOOT_BLOB_MAX];
};

#endif /* JOTUN_BOOT_H */
