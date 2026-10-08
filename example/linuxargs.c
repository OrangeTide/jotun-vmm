/* linuxargs.c : a static Linux test program that echoes its argv.
 *
 * Confirms the guest delivers argc/argv on the initial stack. Run with
 * `make linux-spike SPIKE_PROG="linuxargs one two three"`: the words after the
 * program name become its arguments. An ordinary Linux binary. */
#include <string.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(int argc, char **argv)
{
    char d[2] = { (char)('0' + (argc % 10)), 0 };
    puts2("argc=");
    puts2(d);
    puts2("\n");
    for (int i = 0; i < argc; i++) {
        char n[2] = { (char)('0' + (i % 10)), 0 };
        puts2("argv[");
        puts2(n);
        puts2("]=");
        puts2(argv[i] ? argv[i] : "(null)");
        puts2("\n");
    }
    return 0;
}
