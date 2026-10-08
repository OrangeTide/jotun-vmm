/* x86_64_vmspace.h : per-process address spaces for the Linux-compat guest.
 *
 * The native libOS runs in one flat, identity-mapped address space. To run
 * several Linux processes that each expect private memory at the same virtual
 * addresses (a non-PIE binary is fixed at 0x400000), each process needs its own
 * page tables. This module is the machinery: a physical frame allocator, a
 * builder for a fresh address space that shares the kernel mappings but has a
 * private user window, and CR3 switching.
 *
 * Layout in every process address space:
 *   [0, 0x400000)            kernel, page tables, MMIO: identity, shared
 *   [0x400000, 0x1C00000)    the user window: private 4 KiB pages per process
 *   [0x1C00000, 0x2000000)   vcpu stacks: identity, shared
 *   [1 GiB, 1 GiB + 32 MiB)  physmap: all RAM, so the kernel can reach any
 *                            frame by physical address regardless of CR3
 *
 * x86-64 only; the Linux-compat path is x86-64 only. */
#ifndef X86_64_VMSPACE_H
#define X86_64_VMSPACE_H

#include <stdint.h>

#define VM_PAGE      0x1000ULL
#define VM_USER_LO   0x400000ULL
#define VM_USER_HI   0x1C00000ULL
#define VM_PHYSMAP   0x40000000ULL          /* 1 GiB */

/* Page-table entry flags. */
#define VM_P   0x1ULL                       /* present */
#define VM_W   0x2ULL                       /* writable */
#define VM_U   0x4ULL                       /* user-accessible */
#define VM_PS  0x80ULL                      /* 2 MiB page (in a PD entry) */

typedef uint64_t vmspace_t;                 /* physical address of the PML4 */

/* Thread the physical frames in [lo, hi) onto the free list. Called once. */
void phys_init(uint64_t lo, uint64_t hi);

/* Allocate a zeroed 4 KiB frame (physical address), or 0 if none are free. */
uint64_t phys_alloc(void);

/* Return a frame to the free list. */
void phys_free(uint64_t frame);

/* The kernel virtual address for a physical address: identity before the first
 * vmspace_switch, the physmap afterwards. Lets the kernel touch any frame. */
void *phys_to_virt(uint64_t p);

/* Build a fresh address space: the shared kernel mappings and physmap, with an
 * empty user window. Returns the PML4 physical address, or 0 on failure. */
vmspace_t vmspace_new(void);

/* Map one 4 KiB user-window page to frame with the given flags (VM_P|VM_W|..).
 * Allocates intermediate tables as needed. Returns 0 or -1. */
int vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags);

/* The frame backing vaddr in as, or 0 if unmapped. */
uint64_t vmspace_lookup(vmspace_t as, uint64_t vaddr);

/* Make as the active address space (load CR3) and enable the physmap. */
void vmspace_switch(vmspace_t as);

/* Free every user-window frame and page table of as (not the shared kernel
 * tables, which are shared by reference). */
void vmspace_destroy(vmspace_t as);

/* Copy every mapped user-window page of src into a fresh address space,
 * allocating a new frame for each and copying its contents. Returns the new
 * PML4, or 0 on failure. Used by fork. */
vmspace_t vmspace_clone(vmspace_t src);

#endif /* X86_64_VMSPACE_H */
