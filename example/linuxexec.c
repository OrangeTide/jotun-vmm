/* linuxexec.c : a static Linux test for execve.
 *
 * Prints a line, then replaces itself with another bundle program (linuxhello)
 * via execve. On success this process's image is gone and linuxhello's output
 * follows; the "execve failed" line should never print. Run with
 * `make linux-spike SPIKE_PROG=linuxexec`. An ordinary Linux binary. */
#include <string.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    puts2("before exec\n");

    char *argv[] = { "linuxhello", 0 };
    char *envp[] = { 0 };
    execve("linuxhello", argv, envp);

    puts2("execve failed\n");
    return 1;
}
