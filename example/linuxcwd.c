/* linuxcwd.c : a static Linux test for the per-process working directory.
 *
 * Exercises getcwd/chdir and relative-path resolution against the writable
 * overlay: make a directory, change into it, create a file by a relative name,
 * read it back by relative and absolute names, resolve "." and "..", and check
 * that chdir rejects a nonexistent path and a plain file. Run with
 * `make linux-example EXAMPLE_PROG=linuxcwd`. An ordinary Linux binary. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static int fails;

static void
check(int ok, const char *what)
{
    puts2(ok ? "[ok]   " : "[FAIL] ");
    puts2(what);
    puts2("\n");
    if (!ok)
        fails++;
}

/* Create path (relative or absolute), write body, close. Returns 0 on success. */
static int
writefile(const char *path, const char *body)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    long n = write(fd, body, strlen(body));
    close(fd);
    return n == (long)strlen(body) ? 0 : -1;
}

/* Read path into buf (NUL-terminated). Returns the byte count, or -1. */
static long
readfile(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

int
main(void)
{
    char cwd[256];
    char buf[64];

    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/") == 0,
          "getcwd starts at /");

    check(mkdir("sub", 0755) == 0, "mkdir sub");
    check(chdir("sub") == 0, "chdir sub");
    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/sub") == 0,
          "getcwd is /sub");

    /* A relative create lands under the cwd. */
    check(writefile("f.txt", "hi") == 0, "write f.txt (relative)");
    check(readfile("f.txt", buf, sizeof buf) == 2 && strcmp(buf, "hi") == 0,
          "read f.txt (relative)");
    check(readfile("./f.txt", buf, sizeof buf) == 2 && strcmp(buf, "hi") == 0,
          "read ./f.txt");

    /* A nested directory under the cwd, reached and left with "..". */
    check(mkdir("deep", 0755) == 0, "mkdir deep (under /sub)");
    check(chdir("deep") == 0, "chdir deep");
    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/sub/deep") == 0,
          "getcwd is /sub/deep");
    check(readfile("../f.txt", buf, sizeof buf) == 2 && strcmp(buf, "hi") == 0,
          "read ../f.txt");
    check(writefile("../g.txt", "yo") == 0, "write ../g.txt");

    check(chdir("..") == 0, "chdir ..");
    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/sub") == 0,
          "getcwd back to /sub");
    check(readfile("g.txt", buf, sizeof buf) == 2 && strcmp(buf, "yo") == 0,
          "read g.txt after chdir ..");

    /* Absolute paths are unaffected by the cwd. */
    check(chdir("/") == 0, "chdir /");
    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/") == 0,
          "getcwd back to /");
    check(readfile("/sub/f.txt", buf, sizeof buf) == 2 && strcmp(buf, "hi") == 0,
          "read /sub/f.txt (absolute)");
    check(readfile("sub/f.txt", buf, sizeof buf) == 2 && strcmp(buf, "hi") == 0,
          "read sub/f.txt (relative from root)");

    /* chdir errors. */
    check(chdir("nope") < 0, "chdir nonexistent fails");
    check(getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/") == 0,
          "cwd unchanged after failed chdir");
    check(chdir("/sub/f.txt") < 0, "chdir onto a file fails");

    if (fails == 0)
        puts2("cwd test ok\n");
    else
        puts2("cwd test FAILED\n");
    return fails ? 1 : 0;
}
