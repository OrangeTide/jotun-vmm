/* event.h : minimal shim of the macOS <sys/event.h> kqueue interface.
 *
 * This is NOT the real header. It exists only so the `macsyntax` make target
 * can syntax-check host/net.c's __APPLE__ (kqueue) branch on a Linux dev box,
 * where <sys/event.h> does not exist. Building and running on macOS, with
 * Apple's real header, remains the actual confirmation. */

#ifndef SHIM_SYS_EVENT_H
#define SHIM_SYS_EVENT_H

#include <stdint.h>

struct kevent {
    uintptr_t ident;
    int16_t   filter;
    uint16_t  flags;
    uint32_t  fflags;
    intptr_t  data;
    void     *udata;
};

#define EV_SET(kevp, a, b, c, d, e, f) do {     \
    struct kevent *__k = (kevp);                \
    __k->ident  = (uintptr_t)(a);               \
    __k->filter = (int16_t)(b);                 \
    __k->flags  = (uint16_t)(c);                \
    __k->fflags = (uint32_t)(d);                \
    __k->data   = (intptr_t)(e);                \
    __k->udata  = (void *)(f);                  \
} while (0)

#define EVFILT_READ  (-1)
#define EVFILT_WRITE (-2)

#define EV_ADD     0x0001
#define EV_DELETE  0x0002
#define EV_ONESHOT 0x0010

struct timespec;

int kqueue(void);
int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents,
           const struct timespec *timeout);

#endif /* SHIM_SYS_EVENT_H */
