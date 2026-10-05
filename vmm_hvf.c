/* vmm_hvf.c : macOS Hypervisor.framework (Apple Silicon / ARM64) backend */

#ifdef __APPLE__

#include "vmm.h"

#include <Hypervisor/Hypervisor.h>
#include <stdlib.h>
#include <sys/sysctl.h>

/* AArch64 exception syndrome: top 6 bits of ESR_EL2 are the exception class. */
#define ESR_EC(esr)     (((esr) >> 26) & 0x3f)
#define EC_WFI_WFE      0x01    /* trapped WFI/WFE, treated as guest idle */
#define EC_DATA_ABORT   0x24    /* stage-2 data abort, i.e. an MMIO access */
#define EC_BRK_AARCH64  0x3c    /* BRK software breakpoint */

/* On Apple Silicon the VM is a per-process singleton: hv_vm_create() takes no
 * handle and there is only ever one VM. We still hand back an allocated struct
 * so the abstraction keeps its opaque-handle shape across backends. */
struct vmm_vm {
    bool created;
};

struct vmm_vcpu {
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;   /* framework-owned, refreshed after each run */
};

vmm_vm_t *
vmm_init(void)
{
    vmm_vm_t *vm = malloc(sizeof(*vm));
    if (!vm)
        return NULL;

    if (hv_vm_create(NULL) != HV_SUCCESS) {
        free(vm);
        return NULL;
    }
    vm->created = true;
    return vm;
}

vmm_vm_t *
vmm_vm_create(void)
{
    return vmm_init();
}

void
vmm_vm_map_memory(vmm_vm_t *vm, void *host_addr, uint64_t guest_addr,
                  size_t size)
{
    (void)vm;
    hv_memory_flags_t flags = HV_MEMORY_READ | HV_MEMORY_WRITE
                            | HV_MEMORY_EXEC;
    if (hv_vm_map(host_addr, guest_addr, size, flags) != HV_SUCCESS) {
        /* Mapping failed; the guest will fault on first access. */
    }
}

vmm_vcpu_t *
vmm_vcpu_create(vmm_vm_t *vm)
{
    (void)vm;
    vmm_vcpu_t *vcpu = malloc(sizeof(*vcpu));
    if (!vcpu)
        return NULL;

    /* Must run on the thread that later calls hv_vcpu_run(). */
    if (hv_vcpu_create(&vcpu->vcpu, &vcpu->exit, NULL) != HV_SUCCESS) {
        free(vcpu);
        return NULL;
    }
    return vcpu;
}

void
vmm_vcpu_set_reg(vmm_vcpu_t *vcpu, vmm_reg_t reg, uint64_t value)
{
    switch (reg) {
    case VMM_REG_PC:
        hv_vcpu_set_reg(vcpu->vcpu, HV_REG_PC, value);
        break;
    case VMM_REG_SP:
        /* The guest runs at EL1, so its stack pointer is SP_EL1. */
        hv_vcpu_set_sys_reg(vcpu->vcpu, HV_SYS_REG_SP_EL1, value);
        break;
    }
}

void
vmm_vcpu_run(vmm_vcpu_t *vcpu, vmm_exit_info_t *exit_info)
{
    if (hv_vcpu_run(vcpu->vcpu) != HV_SUCCESS) {
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        return;
    }

    /* ARM64 has no port I/O, so VMM_EXIT_REASON_IO never arises here. Guest
     * exits surface as exceptions; we decode the ones the abstraction models
     * and report everything else as an error. */
    if (vcpu->exit->reason != HV_EXIT_REASON_EXCEPTION) {
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        return;
    }

    uint64_t syndrome = vcpu->exit->exception.syndrome;
    switch (ESR_EC(syndrome)) {
    case EC_DATA_ABORT:
        exit_info->reason = VMM_EXIT_REASON_MMIO;
        exit_info->mmio.address = vcpu->exit->exception.physical_address;
        /* ISS: bit 6 is WnR (write not read), bits 23:22 are the access size
         * (SAS) as a log2 byte count. */
        exit_info->mmio.write = (syndrome >> 6) & 0x1;
        exit_info->mmio.size = (uint8_t)(1u << ((syndrome >> 22) & 0x3));
        /* Completing an MMIO access on AArch64 means decoding the faulting
         * instruction's data register and advancing PC; that register plumbing
         * is not wired up yet, so no data buffer is handed back. */
        exit_info->mmio.data = NULL;
        break;
    case EC_WFI_WFE:
        exit_info->reason = VMM_EXIT_REASON_HLT;
        break;
    default:
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        break;
    }
}

void
vmm_vcpu_destroy(vmm_vcpu_t *vcpu)
{
    if (!vcpu)
        return;
    hv_vcpu_destroy(vcpu->vcpu);
    free(vcpu);
}

void
vmm_vm_destroy(vmm_vm_t *vm)
{
    if (!vm)
        return;
    hv_vm_destroy();
    free(vm);
}

bool
vmm_check_support(const char *feature)
{
    (void)feature;

    /* kern.hv_support is the documented way to ask whether the Hypervisor
     * framework can run on this host. */
    int supported = 0;
    size_t len = sizeof(supported);
    if (sysctlbyname("kern.hv_support", &supported, &len, NULL, 0) != 0)
        return false;
    return supported != 0;
}

#endif /* __APPLE__ */
