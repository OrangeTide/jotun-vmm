/* prog.c : the service vector and exec-by-name.
 *
 * A program is a separate ELF in the bundle. prog_run loads it into the fixed
 * program region and runs its entry as a child task, so it has its own stack
 * and its exit status flows back through task_await. The program calls the
 * libOS through the published service table, since there is no privilege
 * boundary to trap across. */
#include "prog.h"

#include "arch.h"
#include "elfload.h"
#include "p9.h"
#include "print.h"
#include "task.h"
#include "jotun_svc.h"

/* The fid prog_run uses to open a program file. */
#define PROG_FID 10u

static void
svc_putc(char c)
{
    con_putc(c);
}

static void
svc_puts(const char *s)
{
    con_puts(s);
}

static void
svc_exit(const char *status)
{
    task_exits(status);
}

static struct jotun_svc svc = {
    .version = JOTUN_SVC_VERSION,
    .putc = svc_putc,
    .puts = svc_puts,
    .exit = svc_exit,
};

void
svc_publish(void)
{
    *(struct jotun_svc **)(uintptr_t)JOTUN_SVC_PTR = &svc;
}

/* The entry of the program currently being launched, read by the trampoline. */
static uint64_t pending_entry;

static void
prog_trampoline(void)
{
    void (*entry)(void) = (void (*)(void))(uintptr_t)pending_entry;
    entry();
    /* If the program returns rather than calling svc->exit, end cleanly. */
    task_exits(NULL);
}

const char *
prog_run(const char *name)
{
    static char status[STATUS_MAX];
    const char *names[1] = { name };

    if (p9_walk(P9_ROOT_FID, PROG_FID, 1, names) != 1)
        return NULL;
    if (p9_open(PROG_FID) != 0) {
        p9_clunk(PROG_FID);
        return NULL;
    }

    uint64_t entry = elf_load_fid(PROG_FID, JOTUN_PROG_BASE,
                                  JOTUN_PROG_BASE + JOTUN_PROG_SIZE);
    p9_clunk(PROG_FID);
    if (entry == 0)
        return NULL;

    pending_entry = entry;
    if (task_spawn(name, prog_trampoline) < 0)
        return NULL;
    if (task_await(status, sizeof status) < 0)
        return NULL;
    return status;
}
