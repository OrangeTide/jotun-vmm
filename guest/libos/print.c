/* print.c : the portable console byte funnel.
 *
 * Every console helper in print.h ends at con_putc. It always writes the byte
 * to the arch raw sink (the UART), so the host log is unaffected, and when a
 * sink is registered with con_set_sink it also hands the byte to that sink. A
 * guest-side terminal registers a sink to capture console output for on-screen
 * display without losing the host console. */
#include "arch.h"
#include "print.h"

static void (*con_tee)(char);

void
con_set_sink(void (*fn)(char))
{
    con_tee = fn;
}

void
con_putc(char c)
{
    con_raw_putc(c);
    if (con_tee)
        con_tee(c);
}
