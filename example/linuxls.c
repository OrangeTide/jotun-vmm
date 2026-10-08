/* linuxls.c : a static Linux test program for the M16.2 getdents64 path.
 *
 * Opens the bundle root as a directory and lists it with getdents64, marking
 * directories with a trailing slash. It uses the raw open/getdents64/close
 * syscalls (no opendir, so no heap), exercising directory enumeration over the
 * read-only 9P bundle. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxls`. An ordinary Linux binary. */
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

#define DT_DIR 4

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    char buf[2048];
    long n;

    int fd = open("/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        puts2("open / failed\n");
        return 1;
    }

    puts2("entries in /:\n");
    while ((n = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0) {
        for (long off = 0; off < n;) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            puts2("  ");
            puts2(d->d_name);
            puts2(d->d_type == DT_DIR ? "/\n" : "\n");
            off += d->d_reclen;
        }
    }
    close(fd);
    return 0;
}
