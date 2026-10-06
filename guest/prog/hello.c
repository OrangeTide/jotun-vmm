/* hello.c : a bundled program that greets through the service vector. */
#include "prog.h"

void
_start(void)
{
    sys()->puts("hello from a loaded program\n");
    sys()->exit("prog-ok");
}
