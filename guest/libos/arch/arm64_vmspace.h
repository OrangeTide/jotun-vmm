/* arm64_vmspace.h : per-process address spaces on AArch64. See x86_64_vmspace.h
 * for the shared design; this is the ARMv8 translation-table version.
 *
 * The guest boots with the MMU off (flat physical). The first vmspace_switch
 * turns the MMU on with that process's tables in TTBR0_EL1, and later switches
 * just reload TTBR0 and flush. The layout matches the x86 backend:
 *
 *   [0, 0x400000)            kernel, page tables, MMIO: identity, shared
 *   [0x400000, 0x1C00000)    the user window: private 4 KiB pages per process
 *   [0x1C00000, 0x40000000)  rest of the low 1 GiB: identity, shared
 *   [1 GiB, 1 GiB + 32 MiB)  physmap: all RAM, reached by physical address
 *
 * 39-bit VA, 4 KiB granule: L1 (1 GiB/entry), L2 (2 MiB), L3 (4 KiB). The low
 * gigabyte is one L2 table of 2 MiB blocks with the user window punched out for
 * L3 page tables; the physmap is a second L2 of 2 MiB blocks. AArch64 only; the
 * Linux-compat path needs the MMU. */
#ifndef ARM64_VMSPACE_H
#define ARM64_VMSPACE_H

#include <stdint.h>

#define VM_PAGE      0x1000ULL
#define VM_USER_LO   0x400000ULL
#define VM_USER_HI   0x1C00000ULL
#define VM_PHYSMAP   0x40000000ULL          /* 1 GiB */

/* The arch-neutral core passes these to vmspace_map; the AArch64 backend maps
 * every user page the same way (RW at EL1), so the flags carry no bits here. */
#define VM_W   0x0ULL
#define VM_U   0x0ULL

typedef uint64_t vmspace_t;                 /* physical address of the L1 table */

/* The active address space (the running process), tracked by vmspace_switch
 * and consulted by the page-fault handler. */
extern vmspace_t cur_vmspace;

void phys_init(uint64_t lo, uint64_t hi);
uint64_t phys_alloc(void);
void phys_free(uint64_t frame);
void *phys_to_virt(uint64_t p);

vmspace_t vmspace_new(void);
int vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags);
uint64_t vmspace_lookup(vmspace_t as, uint64_t vaddr);
void vmspace_switch(vmspace_t as);
void vmspace_destroy(vmspace_t as);
vmspace_t vmspace_clone(vmspace_t src);

/* The data/instruction-abort handler's C half (called from the arm64 exception
 * vector). Demand-maps a user-window page and returns 1, else returns 0. */
int pf_handle(uint64_t far, uint64_t esr);

#endif /* ARM64_VMSPACE_H */
