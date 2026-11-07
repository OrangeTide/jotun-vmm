/* linuxexecram.c : a static Linux test for exec from the writable overlay.
 *
 * A freshly built binary (configure's ./conftest, a linked a.out) lives in the
 * in-memory overlay, not the read-only bundle. This test copies a bundle
 * program (linuxhello) into an overlay path, then execs it from there. On
 * success this image is replaced and linuxhello's own output follows; the
 * "execve failed" line should never print. Run with
 * `make linux-spike SPIKE_PROG=linuxexecram`. An ordinary Linux binary. */
#include <fcntl.h>
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
    /* Copy the bundle program into the writable overlay, under a new directory
     * so the directory-aware overlay is exercised too. */
    int in = open("linuxhello", O_RDONLY);
    if (in < 0) {
        puts2("open source failed\n");
        return 1;
    }
    if (mkdir("built", 0755) != 0) {
        puts2("mkdir failed\n");
        return 1;
    }
    int out = open("built/prog", O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (out < 0) {
        puts2("create dest failed\n");
        return 1;
    }
    char buf[4096];
    long n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                puts2("write failed\n");
                return 1;
            }
            off += w;
        }
    }
    close(in);
    close(out);

    puts2("before exec (from overlay)\n");

    /* Exec by a relative path, so the cwd resolution and the overlay exec path
     * are both covered. */
    char *argv[] = { "built/prog", 0 };
    char *envp[] = { 0 };
    execve("built/prog", argv, envp);

    puts2("execve failed\n");
    return 1;
}
