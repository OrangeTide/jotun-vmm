/* jsh.c : the script-driven guest shell.
 *
 * Built-in commands are echo, ls, and cat; any other first word is taken as a
 * program name and run from the bundle. The bundle is flat, but path walking
 * is general (slash-separated components). */
#include "jsh.h"

#include "mem.h"
#include "p9.h"
#include "print.h"
#include "prog.h"

#define JSH_FID     5u
#define SCRIPT_MAX  2048

static int
streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Split a slash-separated path into components. Returns the count. */
static int
split_path(const char *path, char comps[][64], int maxc)
{
    int n = 0;
    const char *p = path;

    while (*p && n < maxc) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        int i = 0;
        while (*p && *p != '/') {
            if (i < 63)
                comps[n][i++] = *p;
            p++;
        }
        comps[n][i] = '\0';
        n++;
    }
    return n;
}

/* Walk from the root to path into fid. Returns 0 on a full walk. */
static int
walk_path(const char *path, uint32_t fid)
{
    char comps[P9_MAXWELEM][64];
    const char *names[P9_MAXWELEM];
    int n = split_path(path, comps, P9_MAXWELEM);

    for (int i = 0; i < n; i++)
        names[i] = comps[i];
    return (p9_walk(P9_ROOT_FID, fid, n, names) == n) ? 0 : -1;
}

static void
cmd_ls(const char *path)
{
    if (walk_path(path, JSH_FID) != 0) {
        con_puts("ls: not found: ");
        con_puts(path);
        con_putc('\n');
        return;
    }
    if (p9_open(JSH_FID) == 0) {
        uint8_t dir[1024];
        long n = p9_read(JSH_FID, 0, dir, sizeof dir);
        if (n > 0) {
            uint32_t off = 0;
            struct p9_stat st;
            while (p9_dir_next(dir, (uint32_t)n, &off, &st)) {
                con_puts("  ");
                con_puts(st.name);
                con_putc('\n');
            }
        }
    }
    p9_clunk(JSH_FID);
}

static void
cmd_cat(const char *path)
{
    if (walk_path(path, JSH_FID) != 0) {
        con_puts("cat: not found: ");
        con_puts(path);
        con_putc('\n');
        return;
    }
    if (p9_open(JSH_FID) == 0) {
        char buf[512];
        uint64_t off = 0;
        long n;
        while ((n = p9_read(JSH_FID, off, buf, sizeof buf)) > 0) {
            for (long i = 0; i < n; i++)
                con_putc(buf[i]);
            off += (uint64_t)n;
        }
    }
    p9_clunk(JSH_FID);
}

static void
cmd_run(const char *name)
{
    const char *status = prog_run(name);

    if (!status) {
        con_puts(name);
        con_puts(": not found\n");
        return;
    }
    if (status[0]) {
        con_puts(name);
        con_puts(" -> ");
        con_puts(status);
        con_putc('\n');
    }
}

/* Run one line, modifying it in place while tokenizing. */
static void
exec_line(char *line)
{
    char *s = line;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0' || *s == '#')
        return;

    /* First token is the command. */
    char *cmd = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s) {
        *s++ = '\0';
        while (*s == ' ' || *s == '\t')
            s++;
    }
    char *rest = s;             /* remainder of the line, untruncated */

    /* echo prints the whole remainder, so handle it before truncating. */
    if (streq(cmd, "echo")) {
        con_puts(rest);
        con_putc('\n');
        return;
    }

    /* The other commands take a single path, the first token of the rest. */
    char *arg = rest;
    char *e = arg;
    while (*e && *e != ' ' && *e != '\t')
        e++;
    *e = '\0';

    if (streq(cmd, "ls")) {
        cmd_ls(arg[0] ? arg : "/");
    } else if (streq(cmd, "cat")) {
        if (arg[0])
            cmd_cat(arg);
        else
            con_puts("cat: missing path\n");
    } else {
        cmd_run(cmd);
    }
}

void
jsh_run(const char *script_path)
{
    char *buf = kmalloc(SCRIPT_MAX);
    if (!buf)
        return;

    if (walk_path(script_path, JSH_FID) != 0 || p9_open(JSH_FID) != 0) {
        con_puts("jsh: cannot open script ");
        con_puts(script_path);
        con_putc('\n');
        p9_clunk(JSH_FID);
        kfree(buf);
        return;
    }

    uint32_t len = 0;
    long n;
    while (len < SCRIPT_MAX - 1 &&
           (n = p9_read(JSH_FID, len, buf + len, SCRIPT_MAX - 1 - len)) > 0)
        len += (uint32_t)n;
    buf[len] = '\0';
    p9_clunk(JSH_FID);

    /* Run line by line, terminating each line in place. */
    uint32_t i = 0;
    while (i < len) {
        uint32_t start = i;
        while (i < len && buf[i] != '\n')
            i++;
        buf[i] = '\0';
        exec_line(buf + start);
        i++;
    }

    kfree(buf);
}
