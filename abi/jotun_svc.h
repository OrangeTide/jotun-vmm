/* jotun_svc.h : the service vector a loaded program calls the libOS through.
 *
 * The guest is one trust domain with no privilege split, so a program does not
 * trap into the kernel; it calls libOS services through a table of function
 * pointers the kernel publishes. The kernel writes a pointer to its service
 * table at a fixed guest address; a program reads it there. Shared by the
 * libOS and freestanding programs, so it uses only fixed-width types. */

#ifndef JOTUN_SVC_H
#define JOTUN_SVC_H

#include <stdint.h>

/* Where programs are loaded. A program is linked at JOTUN_PROG_BASE; the
 * loader places its segments in [BASE, BASE+SIZE). One program runs at a time
 * in this region. */
#define JOTUN_PROG_BASE     0x400000u
#define JOTUN_PROG_SIZE     0x200000u

/* Fixed guest address holding a (struct jotun_svc *). The kernel writes it at
 * boot, a program reads it at entry. Sits in the free page below the ring. */
#define JOTUN_SVC_PTR       0x7000u
#define JOTUN_SVC_VERSION   1u

/* The service table. A read-only program needs console output and a way to
 * finish; this grows as programs need more. */
struct jotun_svc {
    uint32_t version;
    uint32_t _pad;
    void (*putc)(char c);
    void (*puts)(const char *s);
    void (*exit)(const char *status);   /* does not return */
};

#endif /* JOTUN_SVC_H */
