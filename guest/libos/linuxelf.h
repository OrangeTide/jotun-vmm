/* linuxelf.h : the M16.1 Linux binary-compat spike entry point.
 *
 * linux_elf_run loads a static, non-PIE Linux x86-64 ELF from the bundle and
 * runs it under a SYSCALL trap, servicing the handful of syscalls a musl
 * "hello" needs. This is an experiment to learn what full Linux binary support
 * would take, not a supported feature. On non-x86-64 targets it is a stub. */
#ifndef LINUXELF_H
#define LINUXELF_H

/* Load and run the named bundle file as a Linux program. Prints progress and
 * the exit status to the console. Returns when the program exits. */
void linux_elf_run(const char *file);

#endif /* LINUXELF_H */
