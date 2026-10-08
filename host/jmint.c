/* jmint.c : manage bearer tokens for the hosting front end.
 *
 * The token store is a file of `token user [cap]` lines that authd reads (path
 * from JOTUN_AUTH_TOKENS, default "tokens"). authd reloads it on change, so edits
 * take effect without a restart. Three commands:
 *
 *   jmint <user> [cap]     mint a random token for user (cap is the per-user
 *                          session limit; omit it for brokerd's default), append
 *                          it to the store, and print it once (it is a bearer
 *                          secret, so this is the only time it is shown).
 *   jmint -l               list the stored tokens as `user  cap  prefix...`,
 *                          with the token masked so a listing leaks no secret.
 *   jmint -r <user>        revoke every token for user, rewriting the store
 *                          without those lines, and report how many were removed.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *
store_path(void)
{
    const char *p = getenv("JOTUN_AUTH_TOKENS");
    return (p && *p) ? p : "tokens";
}

static void
make_token(char out[33])
{
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
        fprintf(stderr, "jmint: cannot read /dev/urandom\n");
        exit(1);
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 0xf];
    }
    out[32] = '\0';
}

static int
valid_user(const char *user)
{
    if (!*user || *user == '-')
        return 0;
    for (const char *c = user; *c; c++)
        if (*c <= ' ' || *c == '#')
            return 0;
    return 1;
}

static int
do_mint(const char *user, const char *cap)
{
    if (!valid_user(user)) {
        fprintf(stderr, "jmint: user must be non-empty, no spaces or '#', "
                "and not start with '-'\n");
        return 2;
    }
    char token[33];
    make_token(token);
    FILE *f = fopen(store_path(), "a");
    if (!f) {
        fprintf(stderr, "jmint: cannot open %s\n", store_path());
        return 1;
    }
    if (cap)
        fprintf(f, "%s %s %s\n", token, user, cap);
    else
        fprintf(f, "%s %s\n", token, user);
    fclose(f);
    printf("%s\n", token);
    return 0;
}

static int
do_list(void)
{
    FILE *f = fopen(store_path(), "r");
    if (!f)
        return 0;                       /* an absent store is simply empty */
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char tok[64], user[64], cap[32] = "-";
        if (line[0] == '#' || line[0] == '\n')
            continue;
        int got = sscanf(line, "%63s %63s %31s", tok, user, cap);
        if (got < 2)
            continue;
        if (got < 3)
            snprintf(cap, sizeof cap, "-");
        printf("%-16s %-4s %.8s...\n", user, cap, tok);
        n++;
    }
    fclose(f);
    if (n == 0)
        printf("(no tokens in %s)\n", store_path());
    return 0;
}

static int
do_revoke(const char *user)
{
    const char *path = store_path();
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("revoked 0 token(s) for %s\n", user);
        return 0;
    }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "jmint: cannot write %s\n", tmp);
        fclose(f);
        return 1;
    }
    char line[256];
    int removed = 0;
    while (fgets(line, sizeof line, f)) {
        char tok[64], u[64];
        if (line[0] != '#' && line[0] != '\n' &&
            sscanf(line, "%63s %63s", tok, u) == 2 && strcmp(u, user) == 0) {
            removed++;
            continue;                   /* drop this user's line */
        }
        fputs(line, out);
    }
    fclose(f);
    if (fclose(out) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "jmint: cannot replace %s\n", path);
        unlink(tmp);
        return 1;
    }
    printf("revoked %d token(s) for %s\n", removed, user);
    return 0;
}

static int
usage(const char *me)
{
    fprintf(stderr,
            "usage: %s <user> [cap]   mint a token\n"
            "       %s -l             list tokens (masked)\n"
            "       %s -r <user>      revoke a user's tokens\n",
            me, me, me);
    return 2;
}

int
main(int argc, char **argv)
{
    if (argc < 2)
        return usage(argv[0]);

    if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "list") == 0)
        return do_list();
    if (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "revoke") == 0) {
        if (argc != 3)
            return usage(argv[0]);
        return do_revoke(argv[2]);
    }
    if (argv[1][0] == '-')
        return usage(argv[0]);
    if (argc > 3)
        return usage(argv[0]);
    return do_mint(argv[1], argc == 3 ? argv[2] : NULL);
}
