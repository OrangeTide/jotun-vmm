#ifndef VMM_H
#define VMM_H

#ifdef __linux__
#include <linux/types.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/kvm.h>
#elif defined(__APPLE__)
#include <Hypervisor/hypervisor.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#endif

// Opaque types for VM and VCPU
typedef struct vmm_vm {
#ifdef __linux__
    int kvm_fd;
    int vm_fd;
#elif defined(__APPLE__)
    hv_vm_t vm;
#endif
} vmm_vm_t;

typedef struct vmm_vcpu {
#ifdef __linux__
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
#elif defined(__APPLE__)
    hv_vm_t vm;
    hv_vcpu_t vcpu;
#endif
} vmm_vcpu_t;

// Execution exit reasons
typedef enum {
    VMM_EXIT_REASON_IO,
    VMM_EXIT_REASON_MMIO,
    VMM_EXIT_REASON_HLT,
    VMM_EXIT_REASON_ERROR
} vmm_exit_reason_t;

// Exit info structure
typedef struct {
    vmm_exit_reason_t reason;
    union {
        struct {
            uint16_t port;
            bool write;
        } io;
        uint64_t address;
    } data;
} vmm_exit_info_t;

// Lifecycle management functions
vmm_vm_t* vmm_init();
vmm_vm_t* vmm_vm_create();
void vmm_vm_map_memory(vmm_vm_t* vm, void* host_addr, uint64_t guest_addr, size_t size);
vmm_vcpu_t* vmm_vcpu_create(vmm_vm_t* vm);
void vmm_vcpu_run(vmm_vcpu_t* vcpu, vmm_exit_info_t* exit_info);
void vmm_vm_destroy(vmm_vm_t* vm);

// Feature check function
bool vmm_check_support(const char* feature);

#endif // VMM_H
