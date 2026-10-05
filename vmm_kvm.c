#ifdef __linux__

#include "vmm.h"

#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__aarch64__)
/* Encode an AArch64 core register id for KVM_SET_ONE_REG from the matching
 * field in struct kvm_regs (the id carries the field's u32 offset). */
#define ARM64_CORE_REG(name) \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
     KVM_REG_ARM_CORE_REG(name))
#endif

/* Concrete layout for the opaque handles declared in vmm.h. */
struct vmm_vm {
    int kvm_fd;
    int vm_fd;
};

struct vmm_vcpu {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    struct kvm_run *run;    /* shared run structure, mapped for the vcpu's life */
    int mmap_size;
};

// Lifecycle management functions
vmm_vm_t* vmm_init() {
    vmm_vm_t* vm = (vmm_vm_t*)malloc(sizeof(vmm_vm_t));
    vm->kvm_fd = open("/dev/kvm", O_RDWR);
    if (vm->kvm_fd < 0) {
        free(vm);
        return NULL;
    }
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        close(vm->kvm_fd);
        free(vm);
        return NULL;
    }
    return vm;
}

vmm_vm_t* vmm_vm_create() {
    return vmm_init();
}

void vmm_vm_map_memory(vmm_vm_t* vm, void* host_addr, uint64_t guest_addr, size_t size) {
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = guest_addr,
        .memory_size = size,
        .userspace_addr = (uint64_t)host_addr
    };
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        // Handle error
    }
}

vmm_vcpu_t* vmm_vcpu_create(vmm_vm_t* vm) {
    vmm_vcpu_t* vcpu = (vmm_vcpu_t*)malloc(sizeof(vmm_vcpu_t));
    vcpu->kvm_fd = vm->kvm_fd;
    vcpu->vm_fd = vm->vm_fd;
    vcpu->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu->vcpu_fd < 0) {
        free(vcpu);
        return NULL;
    }

#if defined(__x86_64__)
    // A fresh x86 vcpu resets into 16-bit real mode, which cannot run a
    // toolchain-built 32-bit ELF. Put it straight into 32-bit protected mode
    // with flat, paging-off segments by programming the segment descriptor
    // cache directly (no GDT in guest memory is needed as long as the guest
    // does not reload the segment registers). The entry point and stack are
    // set later via VMM_REG_PC / VMM_REG_SP.
    struct kvm_sregs sregs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    struct kvm_segment code = {
        .base = 0, .limit = 0xffffffff, .selector = 0x08,
        .type = 0xb,        /* execute/read, accessed */
        .present = 1, .dpl = 0, .db = 1, .s = 1, .l = 0, .g = 1,
    };
    struct kvm_segment data = code;
    data.type = 0x3;        /* read/write, accessed */
    data.selector = 0x10;
    sregs.cr0 |= 0x1;       /* CR0.PE: enter protected mode */
    sregs.cs = code;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
    if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }

    struct kvm_regs regs = {
        .rflags = 0x2,
    };
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
#elif defined(__aarch64__)
    // An arm64 vcpu must be initialised to a CPU model before KVM_RUN. Ask the
    // kernel for the host's preferred target and apply it. After this the vcpu
    // resets with PC = 0; the entry point is set later via VMM_REG_PC.
    struct kvm_vcpu_init init;
    if (ioctl(vm->vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
#else
#error "unsupported KVM host architecture"
#endif

    // Map the shared run structure once; it stays mapped for the vcpu's life so
    // I/O data written back on a read exit survives into the next KVM_RUN.
    vcpu->mmap_size = ioctl(vcpu->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu->mmap_size < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    vcpu->run = mmap(NULL, vcpu->mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     vcpu->vcpu_fd, 0);
    if (vcpu->run == MAP_FAILED) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    return vcpu;
}

void vmm_vcpu_set_reg(vmm_vcpu_t* vcpu, vmm_reg_t reg, uint64_t value) {
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0) {
        // Handle error
        return;
    }
    switch (reg) {
        case VMM_REG_PC:
            regs.rip = value;
            break;
        case VMM_REG_SP:
            regs.rsp = value;
            break;
    }
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        // Handle error
    }
#elif defined(__aarch64__)
    // AArch64 registers are set one at a time through KVM_SET_ONE_REG. The
    // guest runs at EL1, so its stack pointer is sp_el1.
    uint64_t id;
    switch (reg) {
        case VMM_REG_PC:
            id = ARM64_CORE_REG(regs.pc);
            break;
        case VMM_REG_SP:
            id = ARM64_CORE_REG(sp_el1);
            break;
        default:
            return;
    }
    struct kvm_one_reg one = {
        .id = id,
        .addr = (uint64_t)(uintptr_t)&value,
    };
    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &one) < 0) {
        // Handle error
    }
#endif
}

// Read the guest program counter. Used only by the VMM_DEBUG trace, so the
// cost of this register-read ioctl never touches the normal path.
static uint64_t vcpu_pc(vmm_vcpu_t* vcpu) {
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return 0;
    return regs.rip;
#elif defined(__aarch64__)
    uint64_t pc = 0;
    struct kvm_one_reg one = {
        .id = ARM64_CORE_REG(regs.pc),
        .addr = (uint64_t)(uintptr_t)&pc,
    };
    ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &one);
    return pc;
#endif
}

void vmm_vcpu_run(vmm_vcpu_t* vcpu, vmm_exit_info_t* exit_info) {
    struct kvm_run *run = vcpu->run;

    if (ioctl(vcpu->vcpu_fd, KVM_RUN, 0) < 0) {
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        return;
    }

    // Opt-in per-exit trace: set VMM_DEBUG in the environment to print one
    // line per exit. PC is left out to avoid a register-read ioctl per exit.
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("VMM_DEBUG") != NULL;
    if (dbg) {
        unsigned long long pc = (unsigned long long)vcpu_pc(vcpu);
        switch (run->exit_reason) {
            case KVM_EXIT_IO:
                fprintf(stderr, "[kvm] pc=0x%llx exit=IO port=0x%x dir=%s "
                        "size=%u count=%u\n", pc, run->io.port,
                        run->io.direction == KVM_EXIT_IO_OUT ? "out" : "in",
                        run->io.size, run->io.count);
                break;
            case KVM_EXIT_MMIO:
                fprintf(stderr, "[kvm] pc=0x%llx exit=MMIO addr=0x%llx len=%u "
                        "write=%u\n", pc,
                        (unsigned long long)run->mmio.phys_addr,
                        run->mmio.len, (unsigned)run->mmio.is_write);
                break;
            case KVM_EXIT_HLT:
                fprintf(stderr, "[kvm] pc=0x%llx exit=HLT\n", pc);
                break;
            default:
                fprintf(stderr, "[kvm] pc=0x%llx exit=%u\n", pc,
                        run->exit_reason);
                break;
        }
    }

    switch (run->exit_reason) {
        case KVM_EXIT_IO:
            exit_info->reason = VMM_EXIT_REASON_IO;
            exit_info->io.port = run->io.port;
            exit_info->io.write = run->io.direction == KVM_EXIT_IO_OUT;
            exit_info->io.size = run->io.size;
            exit_info->io.count = run->io.count;
            // The data for the access lives inside the shared run page.
            exit_info->io.data = (uint8_t*)run + run->io.data_offset;
            break;
        case KVM_EXIT_MMIO:
            exit_info->reason = VMM_EXIT_REASON_MMIO;
            exit_info->mmio.address = run->mmio.phys_addr;
            exit_info->mmio.write = run->mmio.is_write;
            exit_info->mmio.size = (uint8_t)run->mmio.len;
            exit_info->mmio.data = run->mmio.data;
            break;
        case KVM_EXIT_HLT:
            exit_info->reason = VMM_EXIT_REASON_HLT;
            break;
        default:
            exit_info->reason = VMM_EXIT_REASON_ERROR;
            break;
    }
}

void vmm_vcpu_destroy(vmm_vcpu_t* vcpu) {
    if (!vcpu)
        return;
    if (vcpu->run && vcpu->run != MAP_FAILED)
        munmap(vcpu->run, vcpu->mmap_size);
    close(vcpu->vcpu_fd);
    free(vcpu);
}

void vmm_vm_destroy(vmm_vm_t* vm) {
    close(vm->vm_fd);
    close(vm->kvm_fd);
    free(vm);
}

bool vmm_check_support(const char* feature) {
    (void)feature;
    // Implement feature check logic
    return true;
}

#endif // __linux__
