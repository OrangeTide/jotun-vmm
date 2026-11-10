/* linuxtmp.c : a static Linux test that /tmp, when the manifest maps it to a
 * writable host directory ("mount tmp host:/tmp/<dir> rw"), behaves as a real
 * directory: stat it, create a file under it, write, close, reopen, read back,
 * and confirm the bytes. The created file lands on the host, not the in-RAM
 * overlay, so it survives the run and an operator can inspect it. */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static const char PATH[] = "/tmp/jotest.txt";
static const char MSG[]  = "tmp-mount-ok";

int
main(void)
{
    struct stat st;
    if (stat("/tmp", &st) != 0 || !S_ISDIR(st.st_mode)) {
        puts2("tmp: /tmp is not a directory\n");
        return 1;
    }

    int fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        puts2("tmp: create failed\n");
        return 2;
    }
    if (write(fd, MSG, sizeof MSG - 1) != (long)(sizeof MSG - 1)) {
        puts2("tmp: write failed\n");
        return 3;
    }
    close(fd);

    char buf[64];
    fd = open(PATH, O_RDONLY);
    if (fd < 0) {
        puts2("tmp: reopen failed\n");
        return 4;
    }
    long n = read(fd, buf, sizeof buf);
    close(fd);
    if (n != (long)(sizeof MSG - 1) || memcmp(buf, MSG, sizeof MSG - 1) != 0) {
        puts2("tmp: readback mismatch\n");
        return 5;
    }

    puts2("tmp ok\n");
    return 0;
}
