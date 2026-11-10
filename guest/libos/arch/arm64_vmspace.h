/* arm64_vmspace.h : per-process address spaces on AArch64. See x86_64_vmspace.h
 * for the shared design; this is the ARMv8 translation-table version.
 *
 * The guest boots with the MMU off (flat physical). The first vmspace_switch
 * turns the MMU on with that process's tables in TTBR0_EL1, and later switches
 * just reload TTBR0 and flush. The layout matches the x86 backend:
 *
 *   [0, 0x400000)            kernel image: identity, shared
 *   [0x400000, VM_USER_HI)   the user window: private 16 KiB pages per process,
 *                            now crossing 1 GiB, so on 4 KiB it spans two L1 entries
 *   [VM_KHEAP_LO, VM_KHEAP_HI)  the kernel heap bootstrap band: identity, shared
 *   [VM_KHEAP_HI, RAM top)   vcpu stacks: identity, shared
 *   [VM_DEV_LO, VM_DEV_HI)   the device band (MMU as device): identity, shared
 *   [4 GiB, 4 GiB + RAM)     physmap: all RAM, reached by physical address
 *   [VM_KHEAP_VA, ...)       the kernel heap growth window: shared, demand-grown
 *
 * 36-bit VA, 16 KiB granule (Apple Silicon's native page): the walk starts at
 * level 2 (32 MiB/entry) and ends at level 3 (16 KiB pages). The 16 KiB granule
 * is deliberate: on this host the page-table walker's coherency works in 16 KiB
 * units, so a 4 KiB table (four to an Apple page) aliases its neighbours and the
 * walker reads stale descriptors. A 16 KiB granule puts every table in its own
 * Apple page and makes every user page a full Apple page, which the walker keeps
 * coherent. The low RAM is mapped as 32 MiB level-2 blocks with the two blocks
 * that straddle the user window (the first, holding the kernel image, and the
 * last, holding the heap band and stacks) punched out into level-3 tables; the
 * physmap is RAM/32 MiB blocks sitting directly in the root table, with no
 * second-level table to go stale. The kernel heap starts in the bootstrap band
 * above the user window (see x86_64_vmspace.h) and grows on demand into the
 * window at VM_KHEAP_VA, a run of shared level-3 tables vmspace_new installs in
 * every process. The guest boots with the MMU off, so the window becomes
 * reachable at the first switch and the grower stays idle until then. AArch64
 * only; the Linux-compat path needs the MMU. */
#ifndef ARM64_VMSPACE_H
#define ARM64_VMSPACE_H

#include <stdint.h>

#define VM_PAGE      0x4000ULL                /* 16 KiB: the translation granule */
#define VM_RAM_SIZE  0x80000000ULL           /* 2 GiB; must match the host */
#define VM_USER_LO   0x400000ULL
#define VM_USER_HI   (VM_RAM_SIZE - 0x1000000ULL)  /* RAM top less 16 MiB */
/* The kernel heap: 8 MiB above the user window, with the vcpu stacks taking
 * what remains up to RAM top. Both 2 MiB aligned. See x86_64_vmspace.h. */
#define VM_KHEAP_LO  VM_USER_HI
#define VM_KHEAP_HI  (VM_USER_HI + 0x800000ULL)
/* The device MMIO band: 32 MiB just above RAM, holding the doorbells, console,
 * poweroff, and vGIC (see jotun_abi.h). Mapped as Device, unbacked, so stores
 * fault out as MMIO. 32 MiB == one 16 KiB-granule level-2 block. */
#define VM_DEV_LO    0x80000000ULL           /* 2 GiB, == RAM top */
#define VM_DEV_HI    0x82000000ULL           /* 2 GiB + 32 MiB */
#define VM_PHYSMAP   0x100000000ULL          /* 4 GiB */
/* The demand-grown kernel heap window at 8 GiB, well above the physmap (which
 * spans [4 GiB, 4 GiB + RAM)). Scattered pool frames are mapped contiguously
 * here, so a single large allocation still gets contiguous VA. */
#define VM_KHEAP_VA  0x200000000ULL

/* The arch-neutral core passes these to vmspace_map; the AArch64 backend maps
 * every user page the same way (RW at EL1), so the flags carry no bits here. */
#define VM_W   0x0ULL
#define VM_U   0x0ULL

typedef uint64_t vmspace_t;                 /* physical address of the L1 table */

/* The active address space (the running process), tracked by vmspace_switch
 * and consulted by the page-fault handler. */
extern vmspace_t cur_vmspace;

/* A guard band in the user window that pf_handle refuses to demand-page: a
 * fault in [pf_guard_lo, pf_guard_hi) is reported fatal instead of being backed
 * with a fresh frame. The Linux-compat loader sets it to the gap just below the
 * user stack, so a stack overflow past the reservation faults cleanly rather
 * than silently growing into the mmap arena. lo == hi (the default) disables it. */
extern uint64_t pf_guard_lo;
extern uint64_t pf_guard_hi;

void phys_init(uint64_t lo, uint64_t hi);
uint64_t phys_alloc(void);
void phys_free(uint64_t frame);
void *phys_to_virt(uint64_t p);

vmspace_t vmspace_new(void);
int vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags);
uint64_t vmspace_lookup(vmspace_t as, uint64_t vaddr);
int vmspace_unmap(vmspace_t as, uint64_t vaddr);
void vmspace_switch(vmspace_t as);
void vmspace_destroy(vmspace_t as);
vmspace_t vmspace_clone(vmspace_t src);

/* Set up the demand-grown kernel heap window and register it as the allocator's
 * grower. Call once, after phys_init, before the first vmspace_switch. */
void kheap_init(void);

/* The data/instruction-abort handler's C half (called from the arm64 exception
 * vector). Demand-maps a user-window page and returns 1, else returns 0. */
int pf_handle(uint64_t far, uint64_t esr);

#endif /* ARM64_VMSPACE_H */
