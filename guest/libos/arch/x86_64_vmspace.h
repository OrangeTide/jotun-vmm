/* x86_64_vmspace.h : per-process address spaces for the Linux-compat guest.
 *
 * The native libOS runs in one flat, identity-mapped address space. To run
 * several Linux processes that each expect private memory at the same virtual
 * addresses (a non-PIE binary is fixed at 0x400000), each process needs its own
 * page tables. This module is the machinery: a physical frame allocator, a
 * builder for a fresh address space that shares the kernel mappings but has a
 * private user window, and CR3 switching.
 *
 * Layout in every process address space (RAM is VM_RAM_SIZE, mapped at 0):
 *   [0, 0x400000)            kernel image: identity, shared
 *   [0x400000, VM_USER_HI)   the user window: private 4 KiB pages per process
 *   [VM_KHEAP_LO, VM_KHEAP_HI)  the kernel heap bootstrap band: identity, shared
 *   [VM_KHEAP_HI, RAM top)   vcpu stacks: identity, shared
 *   [1 GiB, 1 GiB + RAM)     physmap: all RAM, so the kernel can reach any
 *                            frame by physical address regardless of CR3
 *   [VM_KHEAP_VA, ...)       the kernel heap growth window: shared, demand-grown
 *
 * The kernel heap starts in the bootstrap band above the user window, not in
 * the cramped identity region below it, so kmalloc and the dynamically sized
 * tables are not boxed into the few MiB under the user's load address. The band
 * is identity-mapped like the rest of [0, 1 GiB) outside the user window, so the
 * one base pointer is valid at boot and after every CR3 switch.
 *
 * The band is finite, so once the Linux-compat path is up the heap also grows
 * on demand into the window at VM_KHEAP_VA, backed by frames drawn from the same
 * pool the user pages come from. The window is a separate PDPT entry pointing at
 * one shared page directory (kheap_init installs it in the boot tables and
 * vmspace_new installs it in every process), so a frame mapped into it once is
 * reachable in every address space. See kheap_init in the C file.
 *
 * VM_RAM_SIZE must match GUEST_RAM_SIZE in the host (host/main.c); the window
 * and the physmap are derived from it here. x86-64 only; the Linux-compat path
 * is x86-64 only. */
#ifndef X86_64_VMSPACE_H
#define X86_64_VMSPACE_H

#include <stdint.h>

#define VM_PAGE      0x1000ULL
#define VM_RAM_SIZE  0x08000000ULL           /* 128 MiB; must match the host */
#define VM_USER_LO   0x400000ULL
#define VM_USER_HI   (VM_RAM_SIZE - 0x1000000ULL)  /* RAM top less 16 MiB */
/* The kernel heap: 8 MiB directly above the user window. The vcpu stacks take
 * what remains up to RAM top (8 MiB, ample for 512 KiB x the cpus). Both are
 * 2 MiB aligned so the identity map covers them with whole large pages. */
#define VM_KHEAP_LO  VM_USER_HI
#define VM_KHEAP_HI  (VM_USER_HI + 0x800000ULL)
#define VM_PHYSMAP   0x40000000ULL           /* 1 GiB */
#define VM_PHYSMAP_PAGES (VM_RAM_SIZE / 0x200000ULL)  /* RAM in 2 MiB pages */
/* The demand-grown kernel heap window: 2 GiB, a whole PDPT entry (1 GiB of VA,
 * far more than RAM) above the physmap. Backed by scattered pool frames mapped
 * contiguously here, so a single large allocation still gets contiguous VA. */
#define VM_KHEAP_VA  0x80000000ULL

/* Page-table entry flags. */
#define VM_P   0x1ULL                       /* present */
#define VM_W   0x2ULL                       /* writable */
#define VM_U   0x4ULL                       /* user-accessible */
#define VM_PS  0x80ULL                      /* 2 MiB page (in a PD entry) */

typedef uint64_t vmspace_t;                 /* physical address of the PML4 */

/* The active address space (the running process), tracked by vmspace_switch
 * and consulted by the page-fault handler. */
extern vmspace_t cur_vmspace;

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

/* Unmap one user-window page, free its frame, and flush the TLB entry if as is
 * the active space. Returns 1 if a mapping was present, 0 otherwise. Used by
 * munmap to reclaim both the frame and (via the caller) the address range. */
int vmspace_unmap(vmspace_t as, uint64_t vaddr);

/* Make as the active address space (load CR3) and enable the physmap. */
void vmspace_switch(vmspace_t as);

/* Set up the demand-grown kernel heap window and register it as the allocator's
 * grower. Call once, after phys_init, before the first vmspace_switch. */
void kheap_init(void);

/* Free every user-window frame and page table of as (not the shared kernel
 * tables, which are shared by reference). */
void vmspace_destroy(vmspace_t as);

/* Copy every mapped user-window page of src into a fresh address space,
 * allocating a new frame for each and copying its contents. Returns the new
 * PML4, or 0 on failure. Used by fork. */
vmspace_t vmspace_clone(vmspace_t src);

#endif /* X86_64_VMSPACE_H */
