/* prog.h : the program side of the service vector.
 *
 * A bundled program is a standalone freestanding ELF, linked at
 * JOTUN_PROG_BASE and entered at _start. It reaches the libOS only through the
 * service table, whose address the kernel published before the program ran. */
#ifndef JOTUN_PROG_H
#define JOTUN_PROG_H

#include <stdint.h>

#include "jotun_svc.h"

static inline struct jotun_svc *
sys(void)
{
    return *(struct jotun_svc **)(uintptr_t)JOTUN_SVC_PTR;
}

#endif /* JOTUN_PROG_H */
