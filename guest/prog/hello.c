/* hello.c : a bundled program that greets through the program C library. */
#include "jlibc.h"
#include "prog.h"

void
_start(void)
{
    char *end;
    long v = strtol("  42rest", &end, 10);

    printf("hello from a loaded program: %ld + %d = %ld (tail \"%s\")\n",
           v, atoi("8"), v + atoi("8"), end);
    sys()->exit("prog-ok");
}
