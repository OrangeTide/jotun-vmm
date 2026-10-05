#ifdef __linux__

#include <linux/kvm.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include "vmm.h"

// Opaque types for VM and VCPU
typedef struct vmm_vm {
    int kvm_fd;
    int vm_fd;
} vmm_vm_t;

typedef struct vmm_vcpu {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
} vmm_vcpu_t;

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
    return vcpu;
}

void vmm_vcpu_run(vmm_vcpu_t* vcpu, vmm_exit_info_t* exit_info) {
    struct kvm_run *run = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->vcpu_fd, 0);
    if (!run) {
        // Handle error
        return;
    }

    int ret = ioctl(vcpu->vcpu_fd, KVM_RUN, 0);
    if (ret < 0) {
        // Handle error
        munmap(run, getpagesize());
        return;
    }

    switch (run->exit_reason) {
        case KVM_EXIT_IO:
            exit_info->reason = VMM_EXIT_REASON_IO;
            exit_info->data.io.port = run->io.port;
            exit_info->data.io.write = run->io.direction == KVM_EXIT_IO_IN;
            break;
        case KVM_EXIT_MMIO:
            exit_info->reason = VMM_EXIT_REASON_MMIO;
            exit_info->data.address = run->mmio.phys_addr;
            break;
        case KVM_EXIT_HLT:
            exit_info->reason = VMM_EXIT_REASON_HLT;
            break;
        default:
            exit_info->reason = VMM_EXIT_REASON_ERROR;
            break;
    }

    munmap(run, getpagesize());
}

void vmm_vm_destroy(vmm_vm_t* vm) {
    close(vm->vm_fd);
    close(vm->kvm_fd);
    free(vm);
}

bool vmm_check_support(const char* feature) {
    // Implement feature check logic
    return true;
}

#endif // __linux__
