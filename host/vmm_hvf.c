/* vmm_hvf.c : macOS Hypervisor.framework (Apple Silicon / ARM64) backend */

#ifdef __APPLE__

#include "vmm.h"

#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* AArch64 exception syndrome: top 6 bits of ESR_EL2 are the exception class. */
#define ESR_EC(esr)     (((esr) >> 26) & 0x3f)
#define EC_WFI_WFE      0x01    /* trapped WFI/WFE, treated as guest idle */
#define EC_DATA_ABORT   0x24    /* stage-2 data abort, i.e. an MMIO access */
#define EC_BRK_AARCH64  0x3c    /* BRK software breakpoint */

/* Initial PSTATE for the guest: EL1h (so the stack pointer is SP_EL1, the one
 * set through VMM_REG_SP) with DAIF masked so a stray interrupt cannot vector
 * into an uninitialised exception table before the guest sets one up. */
#define PSTATE_EL1H_DAIF 0x3c5

/* On Apple Silicon the VM is a per-process singleton: hv_vm_create() takes no
 * handle and there is only ever one VM. We still hand back an allocated struct
 * so the abstraction keeps its opaque-handle shape across backends. */
struct vmm_vm {
    bool created;
};

struct vmm_vcpu {
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;   /* framework-owned, refreshed after each run */

    /* State of an MMIO access waiting to be completed on the next run: KVM
     * finishes these in the kernel, but on HVF the backend must transfer the
     * data register and advance PC itself. */
    bool mmio_pending;
    bool mmio_write;        /* true: store (no writeback); false: load */
    bool mmio_sign;         /* sign-extend a load result */
    bool mmio_wide;         /* destination is a 64-bit (X) register */
    uint32_t mmio_srt;      /* data register number, 31 == XZR */
    uint8_t mmio_size;      /* access width in bytes */
    uint8_t mmio_data[8];   /* exit_info->mmio.data points here */

    volatile int irq_pending;   /* set by another thread, delivered on run */
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
    hv_vcpu_set_reg(vcpu->vcpu, HV_REG_CPSR, PSTATE_EL1H_DAIF);
    vcpu->mmio_pending = false;
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

/* Queue an asynchronous IRQ for the vcpu. On AArch64 there is no vector
 * number: the interrupt enters through the guest's exception vector (VBAR_EL1)
 * when IRQs are unmasked, so the vector argument is ignored. The flag is
 * applied before the next run. This path is implemented by analogy with the
 * KVM backend and is not yet validated on Apple Silicon. */
void
vmm_vcpu_interrupt(vmm_vcpu_t *vcpu, uint8_t vector)
{
    (void)vector;
    __atomic_store_n(&vcpu->irq_pending, 1, __ATOMIC_SEQ_CST);
}

/* Finish the MMIO access the previous run faulted on: deliver a load result
 * into its destination register, then step PC past the faulting instruction
 * (all AArch64 instructions are 4 bytes). */
static void
complete_mmio(vmm_vcpu_t *vcpu)
{
    if (!vcpu->mmio_write && vcpu->mmio_srt != 31) {
        uint64_t val = 0;
        memcpy(&val, vcpu->mmio_data, vcpu->mmio_size);
        if (vcpu->mmio_sign && vcpu->mmio_size < 8) {
            unsigned bits = vcpu->mmio_size * 8;
            if (val & (1ull << (bits - 1)))
                val |= ~0ull << bits;
        }
        if (!vcpu->mmio_wide)
            val &= 0xffffffffull;
        hv_vcpu_set_reg(vcpu->vcpu, HV_REG_X0 + vcpu->mmio_srt, val);
    }

    uint64_t pc = 0;
    hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, &pc);
    hv_vcpu_set_reg(vcpu->vcpu, HV_REG_PC, pc + 4);
    vcpu->mmio_pending = false;
}

void
vmm_vcpu_run(vmm_vcpu_t *vcpu, vmm_exit_info_t *exit_info)
{
    if (vcpu->mmio_pending)
        complete_mmio(vcpu);

    /* Assert or clear the IRQ line for this run (by analogy with KVM; not yet
     * validated on hardware). Delivered when the guest unmasks IRQs. */
    hv_vcpu_set_pending_interrupt(vcpu->vcpu, HV_INTERRUPT_TYPE_IRQ,
        __atomic_load_n(&vcpu->irq_pending, __ATOMIC_SEQ_CST) != 0);
    __atomic_store_n(&vcpu->irq_pending, 0, __ATOMIC_SEQ_CST);

    if (hv_vcpu_run(vcpu->vcpu) != HV_SUCCESS) {
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        return;
    }

    /* Opt-in per-exit trace: set JOTUN_DEBUG in the environment to print one
     * line per exit (reason, exception class, ISS, PC, faulting address). */
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("JOTUN_DEBUG") != NULL;
    if (dbg) {
        uint64_t pc = 0, s = vcpu->exit->exception.syndrome;
        hv_vcpu_get_reg(vcpu->vcpu, HV_REG_PC, &pc);
        fprintf(stderr, "[hvf] reason=%u ec=0x%llx iss=0x%llx pc=0x%llx "
                "pa=0x%llx\n", (unsigned)vcpu->exit->reason,
                (unsigned long long)ESR_EC(s),
                (unsigned long long)(s & 0x1ffffff),
                (unsigned long long)pc,
                (unsigned long long)vcpu->exit->exception.physical_address);
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
    case EC_DATA_ABORT: {
        /* Data-abort ISS fields (ESR_EL2[24:0]):
         *   [24]    ISV  syndrome is valid (set for normal loads/stores)
         *   [23:22] SAS  access size, log2 bytes
         *   [21]    SSE  sign-extend a load
         *   [20:16] SRT  transfer register (Xt), 31 == XZR
         *   [15]    SF   64-bit register
         *   [6]     WnR  write, not read */
        uint32_t iss = (uint32_t)(syndrome & 0x1ffffff);
        if (!((iss >> 24) & 0x1)) {
            /* No decoded syndrome: the abstraction cannot complete it. */
            exit_info->reason = VMM_EXIT_REASON_ERROR;
            break;
        }
        vcpu->mmio_size = (uint8_t)(1u << ((iss >> 22) & 0x3));
        vcpu->mmio_sign = (iss >> 21) & 0x1;
        vcpu->mmio_srt = (iss >> 16) & 0x1f;
        vcpu->mmio_wide = (iss >> 15) & 0x1;
        vcpu->mmio_write = (iss >> 6) & 0x1;
        memset(vcpu->mmio_data, 0, sizeof(vcpu->mmio_data));
        if (vcpu->mmio_write) {
            /* Hand the store value to the VMM. */
            uint64_t val = 0;
            if (vcpu->mmio_srt != 31)
                hv_vcpu_get_reg(vcpu->vcpu, HV_REG_X0 + vcpu->mmio_srt, &val);
            memcpy(vcpu->mmio_data, &val, vcpu->mmio_size);
        }
        vcpu->mmio_pending = true;

        exit_info->reason = VMM_EXIT_REASON_MMIO;
        exit_info->mmio.address = vcpu->exit->exception.physical_address;
        exit_info->mmio.write = vcpu->mmio_write;
        exit_info->mmio.size = vcpu->mmio_size;
        exit_info->mmio.data = vcpu->mmio_data;
        break;
    }
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
