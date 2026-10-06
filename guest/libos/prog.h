/* prog.h : publish the service vector and run bundled programs by name. */
#ifndef LIBOS_PROG_H
#define LIBOS_PROG_H

/* Install the libOS service table at the fixed address programs read. Call
 * once at boot before any program runs. */
void svc_publish(void);

/* Load the named program from the mounted 9P root, run it to completion as a
 * child task, and return its exit status string (empty on clean exit), or
 * NULL if the program could not be found or loaded. The returned pointer is
 * valid until the next prog_run call. */
const char *prog_run(const char *name);

#endif /* LIBOS_PROG_H */
