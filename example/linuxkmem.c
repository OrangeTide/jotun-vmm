/* linuxkmem.c : prove the CPL3 boundary by touching kernel memory from user.
 *
 * With the Linux process running in CPL 3 and the kernel in CPL 0, the kernel
 * image, the physmap, and the page tables are mapped supervisor-only (no U bit
 * in their PTEs). A user read of any of those addresses must take a protection
 * page fault instead of returning data the way a ring-0 process once could.
 *
 * This reads one byte at 0x100000, which is inside the kernel image band
 * [0, 0x400000) that the libOS keeps supervisor-only. On a correct guest the
 * read faults (#PF, error code with the user bit set and present bit set, since
 * the page exists but is off-limits to CPL 3) and the program never reaches its
 * "SURVIVED" print. A guest that still shares ring 0 would read the byte and
 * print SURVIVED. Like linuxstackov, this halts the guest by design, so it runs
 * on its own with SPIKE_PROG=linuxkmem, not as part of the green suite. */
#include <unistd.h>

int
main(void)
{
    write(1, "kmem: reading kernel address\n", 29);

    volatile unsigned char *kaddr = (volatile unsigned char *)0x100000UL;
    unsigned char v = *kaddr;           /* must fault in CPL 3 */

    /* Unreachable on a correct guest. Use v so the read is not optimized away. */
    char msg[20] = "kmem: SURVIVED x\n";
    msg[15] = (char)('0' + (v & 7));
    write(1, msg, 17);
    return 0;
}
