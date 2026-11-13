/* linuxcmdline.c : a static Linux test that /proc/cmdline exposes the kernel
 * command line the host supplied (manifest "cmdline <string>"). It reads the
 * file, prints "cmdline: <contents>", and exits clean. With no command line
 * (the smoke suite runs it without a manifest) the file holds just a newline,
 * so the program still reaches a clean halt. The manifest-driven linux-cmdline
 * target greps the printed value to prove the pass-through. */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) {
        puts2("cmdline: open failed\n");
        return 1;
    }

    char buf[512];
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) {
        puts2("cmdline: read failed\n");
        return 2;
    }
    buf[n] = '\0';

    /* The file ends with a newline, as Linux writes it; strip it so the label
     * and the value sit on one line. */
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';

    puts2("cmdline: [");
    puts2(buf);
    puts2("]\n");
    return 0;
}
