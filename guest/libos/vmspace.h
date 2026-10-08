/* vmspace.h : per-process address spaces, arch-neutral front door.
 *
 * The Linux-compat guest gives each process a private address space (a private
 * user window over shared kernel and MMIO mappings) so several processes can
 * expect memory at the same virtual addresses. The interface is the same on
 * every arch; the page-table format and the register that selects the active
 * space (CR3 on x86, TTBR0_EL1 on arm64) live in the per-arch backend.
 *
 * Every backend provides the same names, so linuxelf.c includes only this:
 *   type       vmspace_t
 *   constants  VM_PAGE, VM_USER_LO, VM_USER_HI, and the map flags VM_W, VM_U
 *   state      cur_vmspace (the running process's space)
 *   frames     phys_init, phys_alloc, phys_free, phys_to_virt
 *   spaces     vmspace_new, vmspace_map, vmspace_lookup, vmspace_switch,
 *              vmspace_destroy, vmspace_clone
 *   faults     pf_handle (called from the arch fault vector)
 */
#ifndef LIBOS_VMSPACE_H
#define LIBOS_VMSPACE_H

#if defined(__x86_64__)
#include "arch/x86_64_vmspace.h"
#elif defined(__aarch64__)
#include "arch/arm64_vmspace.h"
#else
#error "no vmspace backend for this architecture"
#endif

#endif /* LIBOS_VMSPACE_H */
