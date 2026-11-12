/* linuxsockstat.c : a static Linux test that a bound AF_UNIX socket path is
 * reported as a socket by stat/lstat/fstat and exists for access().
 *
 * bind() records a name but creates no file behind it; the libOS still answers
 * stat on that name with S_IFSOCK (what `test -S` and a server-readiness poll
 * check) instead of ENOENT. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxsockstat`. An ordinary Linux binary. */
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    const char *path = "/sockstat.sock";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        puts2("socket failed\n");
        return 1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        puts2("bind failed\n");
        return 1;
    }

    struct stat st;
    if (stat(path, &st) < 0 || !S_ISSOCK(st.st_mode)) {
        puts2("stat not a socket\n");
        return 1;
    }
    if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode)) {
        puts2("lstat not a socket\n");
        return 1;
    }
    if (fstat(fd, &st) < 0 || !S_ISSOCK(st.st_mode)) {
        puts2("fstat not a socket\n");
        return 1;
    }
    if (access(path, F_OK) != 0) {
        puts2("access says missing\n");
        return 1;
    }

    close(fd);
    puts2("sockstat ok\n");
    return 0;
}
