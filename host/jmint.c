/* jmint.c : mint a bearer token for the hosting front end.
 *
 * Generates a random 32-hex token, appends a `token user [cap]` line to the
 * token store that authd reads, and prints the token once on stdout (it is a
 * bearer secret, so this is the only time it is shown). The store path comes
 * from JOTUN_AUTH_TOKENS, or defaults to "tokens".
 *
 *   jmint <user> [cap]
 *
 * cap is the user's concurrent-session limit; omit it to use brokerd's default. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int
main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <user> [cap]\n", argv[0]);
        return 2;
    }
    const char *user = argv[1];
    const char *cap = argc == 3 ? argv[2] : NULL;
    for (const char *c = user; *c; c++)
        if (*c <= ' ' || *c == '#') {
            fprintf(stderr, "jmint: user must have no spaces or '#'\n");
            return 2;
        }

    const char *path = getenv("JOTUN_AUTH_TOKENS");
    if (!path || !*path)
        path = "tokens";

    char token[33];
    make_token(token);

    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "jmint: cannot open %s\n", path);
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
