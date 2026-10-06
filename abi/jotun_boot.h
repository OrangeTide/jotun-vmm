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
#define JOTUN_BOOT_VERSION  1u

/* Byte budget for the packed string blob. The whole block is one page. */
#define JOTUN_BOOT_BLOB_MAX 4000u

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
    uint32_t _pad;                  /* keep blob 8-byte aligned */
    uint8_t  blob[JOTUN_BOOT_BLOB_MAX];
};

#endif /* JOTUN_BOOT_H */
