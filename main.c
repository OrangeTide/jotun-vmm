#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "vmm.h"
#include "elf.h"
#include "uart.h"

/* Guest physical RAM mapped at address 0. 8 MiB is ample for the test guest
 * plus its stack. */
#define GUEST_RAM_SIZE (8u * 1024 * 1024)

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "test_guest.elf";

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

    // Allocate guest RAM and map it at guest-physical address 0.
    void *ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate guest memory\n");
        vmm_vcpu_destroy(vcpu);
        vmm_vm_destroy(vm);
        return 1;
    }
    vmm_vm_map_memory(vm, ram, 0, GUEST_RAM_SIZE);

    // Load the guest image into RAM and point the vcpu at its entry point.
    elf_image img;
    if (elf_load_file(path, ram, GUEST_RAM_SIZE, &img) != 0) {
        fprintf(stderr, "Failed to load guest image '%s'\n", path);
        munmap(ram, GUEST_RAM_SIZE);
        vmm_vcpu_destroy(vcpu);
        vmm_vm_destroy(vm);
        return 1;
    }
    vmm_vcpu_set_reg(vcpu, VMM_REG_PC, img.entry);
    vmm_vcpu_set_reg(vcpu, VMM_REG_SP, GUEST_RAM_SIZE - 16);

    uart_16550 uart;
    uart_init(&uart);

    // Run loop: execute the guest and service each exit until it halts. The
    // cap keeps a misbehaving guest from spinning here forever.
    vmm_exit_info_t exit_info;
    int rc = 0;
    unsigned long exits = 0;
    const unsigned long max_exits = 1000000;
    for (;;) {
        if (++exits > max_exits) {
            fprintf(stderr, "[giving up after %lu exits]\n", max_exits);
            rc = 1;
            break;
        }
        vmm_vcpu_run(vcpu, &exit_info);
        if (exit_info.reason == VMM_EXIT_REASON_IO) {
            if (!uart_io(&uart, &exit_info) && !exit_info.io.write) {
                // Unhandled input port: return zeroes so the guest proceeds.
                memset(exit_info.io.data, 0,
                       (size_t)exit_info.io.size * exit_info.io.count);
            }
        } else if (exit_info.reason == VMM_EXIT_REASON_HLT) {
            printf("\n[guest halted]\n");
            break;
        } else if (exit_info.reason == VMM_EXIT_REASON_MMIO) {
            if (uart_mmio(&exit_info)) {
                // serviced by the console
            } else if (exit_info.mmio.write &&
                       exit_info.mmio.address == MMIO_POWEROFF) {
                printf("\n[guest powered off]\n");
                break;
            } else {
                fprintf(stderr, "[unhandled MMIO at 0x%" PRIx64 "]\n",
                        exit_info.mmio.address);
                rc = 1;
                break;
            }
        } else {
            fprintf(stderr, "[guest error or unknown exit]\n");
            rc = 1;
            break;
        }
    }

    vmm_vcpu_destroy(vcpu);
    vmm_vm_destroy(vm);
    munmap(ram, GUEST_RAM_SIZE);

    return rc;
}
