#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmm.h"

int main() {
    vmm_vm_t* vm = vmm_vm_create();
    if (!vm) {
        fprintf(stderr, "Failed to create VM\n");
        return 1;
    }

    vmm_vcpu_t* vcpu = vmm_vcpu_create(vm);
    if (!vcpu) {
        fprintf(stderr, "Failed to create VCPU\n");
        vmm_vm_destroy(vm);
        return 1;
    }

    // Load a 5-byte test binary into memory
    uint8_t test_binary[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    vmm_vm_map_memory(vm, test_binary, 0x1000, sizeof(test_binary));

    // Execute the test binary
    vmm_exit_info_t exit_info;
    vmm_vcpu_run(vcpu, &exit_info);

    // Print the exit reason
    switch (exit_info.reason) {
        case VMM_EXIT_REASON_IO:
            printf("Exit reason: IO\n");
            break;
        case VMM_EXIT_REASON_MMIO:
            printf("Exit reason: MMIO\n");
            printf("Address: 0x%llx\n", exit_info.data.address);
            break;
        case VMM_EXIT_REASON_HLT:
            printf("Exit reason: HLT\n");
            break;
        case VMM_EXIT_REASON_ERROR:
            printf("Exit reason: ERROR\n");
            break;
        default:
            printf("Unknown exit reason\n");
            break;
    }

    vmm_vcpu_destroy(vcpu);
    vmm_vm_destroy(vm);

    return 0;
}
