/* linuxsegv.c : prove a faulting process is contained, not a guest halt.
 *
 * The parent forks a child that writes to a NULL pointer. Address 0 sits in the
 * kernel band below the user window, mapped supervisor-only, so a CPL 3 write
 * there faults. With privilege separation and the fault split in place the
 * kernel terminates only the child (SIGSEGV) and keeps running, so the parent's
 * wait4 returns a WIFSIGNALED status and the parent exits cleanly. Before the
 * fault split such a fault halted the whole guest.
 *
 * Unlike linuxstackov and linuxkmem, this is a positive test: it exits 0 and
 * belongs in the green suite, because the faulting work happens in a child and
 * the guest survives it. */
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

static void
say(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        say("child: faulting\n");
        *(volatile int *)0 = 1;         /* write to NULL: faults in CPL 3 */
        say("child: SURVIVED\n");       /* unreachable on a correct guest */
        _exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        say("parent: child terminated by signal, guest survived\n");
        return 0;
    }
    say("parent: child was not signaled\n");
    return 1;
}
