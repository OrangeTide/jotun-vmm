/* vmm.h : cross-platform hypervisor abstraction (Linux KVM / macOS HVF) */

#ifndef VMM_H
#define VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque handles. Each backend (vmm_kvm.c, vmm_hvf.c) defines the concrete
 * layout in its own translation unit. Callers only ever hold pointers. */
typedef struct vmm_vm vmm_vm_t;
typedef struct vmm_vcpu vmm_vcpu_t;

/* Guest registers that can be set through the abstraction. Kept intentionally
 * small and architecture-neutral: PC is the entry point (RIP on x86, PC on
 * AArch64), SP is the stack pointer. */
typedef enum {
    VMM_REG_PC,
    VMM_REG_SP,
} vmm_reg_t;

/* Execution exit reasons */
typedef enum {
    VMM_EXIT_REASON_IO,
    VMM_EXIT_REASON_MMIO,
    VMM_EXIT_REASON_HLT,
    VMM_EXIT_REASON_ERROR,
} vmm_exit_reason_t;

/* Exit info structure.
 *
 * For an I/O or MMIO exit, `data` points at a backend-owned buffer holding the
 * bytes the guest wrote, or waiting to receive the bytes it is reading. The
 * VMM reads it on a write exit and fills it on a read exit; the next run then
 * delivers that value to the guest. */
typedef struct {
    vmm_exit_reason_t reason;
    struct {
        uint16_t port;
        bool write;         /* true when the guest is writing (OUT) */
        uint8_t size;       /* bytes per access */
        uint32_t count;     /* number of accesses (string I/O) */
        uint8_t *data;      /* buffer of size * count bytes */
    } io;
    struct {
        uint64_t address;
        bool write;
        uint8_t size;
        uint8_t *data;      /* `size` bytes */
    } mmio;
} vmm_exit_info_t;

/* Lifecycle management functions */
vmm_vm_t *vmm_init(void);
vmm_vm_t *vmm_vm_create(void);
void vmm_vm_map_memory(vmm_vm_t *vm, void *host_addr, uint64_t guest_addr,
                       size_t size);
vmm_vcpu_t *vmm_vcpu_create(vmm_vm_t *vm);
void vmm_vcpu_set_reg(vmm_vcpu_t *vcpu, vmm_reg_t reg, uint64_t value);
void vmm_vcpu_run(vmm_vcpu_t *vcpu, vmm_exit_info_t *exit_info);
void vmm_vcpu_destroy(vmm_vcpu_t *vcpu);
void vmm_vm_destroy(vmm_vm_t *vm);

/* Feature check function */
bool vmm_check_support(const char *feature);

#endif /* VMM_H */
