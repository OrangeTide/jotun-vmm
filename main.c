#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vmm.h"

#define GUEST_RAM_SIZE (2 * 1024 * 1024) // 2 MB guest RAM

int main(void) {
    // 1. Check if the host platform supports hardware-assisted virtualization
    if (!vmm_check_support()) {
        fprintf(stderr, "Error: Hypervisor support is not available on this host.\n");
        return EXIT_FAILURE;
    }

    // 2. Initialize the global VMM subsystem
    if (vmm_init() != 0) {
        fprintf(stderr, "Error: Failed to initialize VMM subsystem.\n");
        return EXIT_FAILURE;
    }

    // 3. Create a new virtual machine context
    vmm_vm_t *vm = vmm_vm_create();
    if (!vm) {
        fprintf(stderr, "Error: Failed to create VM instance.\n");
        return EXIT_FAILURE;
    }

    // 4. Allocate host memory to serve as Guest Physical Address space
    void *guest_mem = malloc(GUEST_RAM_SIZE);
    if (!guest_mem) {
        perror("malloc guest RAM");
        vmm_vm_destroy(vm);
        return EXIT_FAILURE;
    }
    memset(guest_mem, 0, GUEST_RAM_SIZE);

    // Map host buffer to Guest Physical Address 0x0
    if (vmm_vm_map_memory(vm, 0x0, guest_mem, GUEST_RAM_SIZE) != 0) {
        fprintf(stderr, "Error: Failed to map guest memory region.\n");
        free(guest_mem);
        vmm_vm_destroy(vm);
        return EXIT_FAILURE;
    }

    // 5. Write a small test payload directly into guest memory
    // Example x86 16-bit real mode payload:
    // mov al, 'A'; out 0xE9, al; hlt
    const uint8_t guest_code[] = { 0xB0, 'A', 0xE6, 0xE9, 0xF4 };
    memcpy(guest_mem, guest_code, sizeof(guest_code));

    // 6. Create a virtual CPU (vCPU ID 0)
    vmm_vcpu_t *vcpu = vmm_vcpu_create(vm, 0);
    if (!vcpu) {
        fprintf(stderr, "Error: Failed to create vCPU 0.\n");
        free(guest_mem);
        vmm_vm_destroy(vm);
        return EXIT_FAILURE;
    }

    // 7. Core Execution Loop
    printf("[+] Starting virtual machine...\n");
    int running = 1;
    vmm_exit_info_t exit_info;

    while (running) {
        int res = vmm_vcpu_run(vcpu, &exit_info);
        if (res != 0) {
            fprintf(stderr, "Fatal error during vCPU execution.\n");
            break;
        }

        // Handle exits surfaced by your vmm_hvf.c or vmm_kvm.c backends
        switch (exit_info.reason) {
            case VMM_EXIT_REASON_IO:
                // Trap port I/O writes (e.g., debug port 0xE9)
                if (exit_info.io.is_write && exit_info.io.port == 0xE9) {
                    putchar(exit_info.io.data);
                    fflush(stdout);
                }
                break;

            case VMM_EXIT_REASON_MMIO:
                // Handle memory-mapped I/O access (where Virtio registers live)
                printf("[MMIO] Address: 0x%lx, Write: %d, Data: 0x%x\n",
                       exit_info.mmio.phys_addr, exit_info.mmio.is_write, exit_info.mmio.data);
                break;

            case VMM_EXIT_REASON_HLT:
                printf("\n[+] Guest executed HLT instruction. Shutting down.\n");
                running = 0;
                break;

            case VMM_EXIT_REASON_ERROR:
            default:
                fprintf(stderr, "[-] Unhandled VM exit or runtime error (Reason: %d).\n", exit_info.reason);
                running = 0;
                break;
        }
    }

    // 8. Clean up resources
    vmm_vm_destroy(vm); // Implementation should clean up vCPUs & internal state
    free(guest_mem);

    printf("[+] VM destroyed cleanly.\n");
    return EXIT_SUCCESS;
}
