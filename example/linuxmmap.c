/* linuxmmap.c : a static Linux test for file-backed mmap and pread.
 *
 * The dynamic linker maps a shared object by reserving a span with an
 * anonymous mmap, then overlaying each file segment with MAP_FIXED, and it
 * reads program headers with pread. This program exercises those moves against
 * a read-only 9P bundle file: it maps the file MAP_PRIVATE and checks the bytes
 * match a plain read, preads at a nonzero offset, and overlays the file onto a
 * prior anonymous reservation with MAP_FIXED. Run with
 * `make linux-example EXAMPLE_PROG=linuxmmap`. An ordinary Linux binary. */
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static void
putnum(long v)
{
    char b[24];
    char o[24];
    int n = 0, m = 0;

    if (v == 0)
        b[n++] = '0';
    while (v) {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n)
        o[m++] = b[--n];
    write(1, o, m);
}

int
main(void)
{
    const char *path = "motd";
    char rbuf[256];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        puts2("open failed\n");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        puts2("fstat failed\n");
        return 1;
    }
    size_t sz = (size_t)st.st_size;
    if (sz > sizeof rbuf)
        sz = sizeof rbuf;

    /* A plain read is the reference the mappings are checked against. */
    long rn = read(fd, rbuf, sz);
    if (rn <= 0) {
        puts2("read failed\n");
        return 1;
    }
    sz = (size_t)rn;

    puts2("[mmap] ");
    puts2(path);
    puts2(" is ");
    putnum((long)st.st_size);
    puts2(" bytes\n");

    /* 1. A read-only private file mapping at an address of the kernel's choice. */
    char *m = mmap(0, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        puts2("[mmap] MAP_PRIVATE failed\n");
        return 1;
    }
    if (memcmp(m, rbuf, sz) != 0) {
        puts2("[mmap] MAP_PRIVATE mismatch\n");
        return 1;
    }
    puts2("[mmap] MAP_PRIVATE read-only: match\n");

    /* 2. pread at a nonzero offset, checked against the mapped bytes. */
    size_t off = sz / 2;
    char pbuf[64];
    size_t pn = sz - off;
    if (pn > sizeof pbuf)
        pn = sizeof pbuf;
    long got = pread(fd, pbuf, pn, (off_t)off);
    if (got != (long)pn || memcmp(pbuf, m + off, pn) != 0) {
        puts2("[pread] mismatch\n");
        return 1;
    }
    puts2("[pread] nonzero offset: match\n");

    /* 3. Reserve an anonymous span, then overlay the file onto it with
     * MAP_FIXED, the way the dynamic linker lays a segment into a reservation.
     * The returned address must be the one requested, and the bytes must be the
     * file's. */
    char *res = mmap(0, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        puts2("[mmap] reservation failed\n");
        return 1;
    }
    char *fx = mmap(res, sz, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (fx != res) {
        puts2("[mmap] MAP_FIXED moved\n");
        return 1;
    }
    if (memcmp(fx, rbuf, sz) != 0) {
        puts2("[mmap] MAP_FIXED mismatch\n");
        return 1;
    }
    puts2("[mmap] MAP_FIXED overlay: addr reused, match\n");

    close(fd);
    puts2("mmap test ok\n");
    return 0;
}
