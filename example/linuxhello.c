/* linuxhello.c : a tiny static Linux program for the M16.1 binary-compat spike.
 *
 * Built with musl-gcc -static -no-pie and loaded into the guest by the `bin`
 * experimental guest (guest/libos/linuxelf.c), which traps its Linux syscalls.
 * A plain hello: its whole startup surface is arch_prctl(ARCH_SET_FS),
 * set_tid_address, ioctl(TIOCGWINSZ), writev, and exit_group. See
 * `make linux-spike`. This file is compiled by that target, not the normal
 * build, and needs no Jotun headers: it is an ordinary Linux program. */
#include <stdio.h>

int
main(void)
{
    printf("hello from musl\n");
    return 0;
}
