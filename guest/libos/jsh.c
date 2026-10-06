/* jsh.c : the script-driven guest shell.
 *
 * Built-in commands are echo, ls, and cat; any other first word is taken as a
 * program name and run from the bundle. The bundle is flat, but path walking
 * is general (slash-separated components). */
#include "jsh.h"

#include "mem.h"
#include "ns.h"
#include "p9.h"
#include "print.h"
#include "prog.h"
#include "string.h"

#define JSH_FID     5u
#define SCRIPT_MAX  2048

static void
cmd_ls(const char *path)
{
    if (ns_walk(path, JSH_FID) != 0) {
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
    if (ns_walk(path, JSH_FID) != 0) {
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
    if (strcmp(cmd, "echo") == 0) {
        con_puts(rest);
        con_putc('\n');
        return;
    }

    /* Split the remainder into up to two whitespace-separated arguments. */
    char *a1 = rest;
    char *e = a1;
    while (*e && *e != ' ' && *e != '\t')
        e++;
    char *a2 = e;
    if (*e) {
        *e++ = '\0';
        while (*e == ' ' || *e == '\t')
            e++;
        a2 = e;
        while (*e && *e != ' ' && *e != '\t')
            e++;
        *e = '\0';
    }

    if (strcmp(cmd, "ls") == 0) {
        cmd_ls(a1[0] ? a1 : "/");
    } else if (strcmp(cmd, "cat") == 0) {
        if (a1[0])
            cmd_cat(a1);
        else
            con_puts("cat: missing path\n");
    } else if (strcmp(cmd, "bind") == 0) {
        if (a1[0] && a2[0])
            ns_bind(a1, a2);        /* bind src onto mountpoint */
        else
            con_puts("bind: usage: bind SRC DIR\n");
    } else if (strcmp(cmd, "mount") == 0) {
        if (a1[0] && a2[0])
            ns_mount(a2, a1);       /* mount SRC at DIR */
        else
            con_puts("mount: usage: mount SRC DIR\n");
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

    if (ns_walk(script_path, JSH_FID) != 0 || p9_open(JSH_FID) != 0) {
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
