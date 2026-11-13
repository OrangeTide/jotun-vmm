/* linuxelf.c : run unmodified static Linux binaries under a syscall trap (M16).
 *
 * This is the arch-neutral core. It loads a static, non-PIE ELF (ET_EXEC linked
 * at 0x400000, the window the native programs use), builds a System V initial
 * stack (argc/argv/envp/auxv), gives each process a private address space, and
 * services the syscalls real musl and glibc programs issue: a file table over
 * the 9P bundle and a writable ramfs, a heap arena, fork/clone/wait4, pipes,
 * threads, and a futex. The machine-specific parts (the syscall numbers, the
 * user trapframe, entering user code, the thread pointer, arming the trap) come
 * from the per-arch backend selected by linux_arch.h; the page tables come from
 * vmspace.h. On an unsupported arch linux_elf_run is a stub. */
#include "linuxelf.h"

#include "print.h"

#if defined(__x86_64__) || defined(__aarch64__)

#include "arch.h"
#include "boot.h"
#include "elfload.h"
#include "mem.h"
#include "ns.h"
#include "p9.h"
#include "string.h"
#include "task.h"
#include "jotun_boot.h"      /* JOTUN_BOOT_RWMOUNT_MAX */
#include "jotun_svc.h"       /* JOTUN_PROG_BASE */
#include "vmspace.h"
#include "linux_arch.h"

#include <stdint.h>

/* The uname machine string, the name Linux reports for this architecture. */
#if defined(__aarch64__)
#define UNAME_MACHINE "aarch64"
#else
#define UNAME_MACHINE "x86_64"
#endif

/* The 9P fid used to load the program ELF, distinct from the native loader's
 * (prog.c uses 10), and the pool of fids backing open file descriptors. */
#define LINUX_FID      12u
#define LINUX_IFID     13u          /* loads the PT_INTERP dynamic linker */
#define LINUX_FID_BASE 32u          /* 9P-backed opens use fids 32..32+GFID_MAX */

/* The Linux program's address space, independent of the native program window.
 * A static non-PIE binary is linked at 0x400000; the load window runs to 8 MiB,
 * which holds a 2 MiB-ish glibc image. Above that is the heap: brk grows up from
 * LINUX_HEAP_BASE and anonymous mmap grows down from the top of RAM, less the
 * stack reservation. They meet in the middle and fail there. The stack is the
 * top LINUX_USTACK_SIZE of the window. It is sized to match a Linux main-thread
 * stack (8 MiB): the optimizer recurses deeply on a large translation unit, and
 * a smaller stack silently grew down into the mmap arena and corrupted it.
 *
 * Below the stack reservation sits a LINUX_STACK_GUARD guard band: mmap never
 * allocates into it, and pf_handle refuses to demand-page it (see pf_guard_lo in
 * vmspace.h). A stack frame that overflows past the reservation faults there,
 * which is a clean fatal fault, instead of silently writing into the mmap arena
 * below. The 1 MiB width matches the Linux stack guard gap default; a frame that
 * steps more than that past the reservation in one go could still skip it. */
#define LINUX_PROG_BASE    0x400000ULL
#define LINUX_PROG_LIMIT   0x800000ULL
#define LINUX_HEAP_BASE    0x800000ULL
#define LINUX_USTACK_SIZE  (8u * 1024 * 1024)
#define LINUX_USTACK_TOP   VM_USER_HI       /* top of the user window */
#define LINUX_STACK_GUARD  (1u * 1024 * 1024)   /* unmapped gap below the stack */
#define LINUX_STACK_BOTTOM (LINUX_USTACK_TOP - LINUX_USTACK_SIZE)
#define PAGE_SIZE          VM_PAGE      /* the arch translation granule (vmspace.h) */

/* mmap flags and failure value. */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MMAP_FAILED   (-LERR_NOMEM)

/* The most argv (or envp) entries delivered to a Linux program. A compiler
 * driver builds long linker command lines: gcc's collect2 invocation of ld
 * passes the crt objects, every -L path, the input objects, the -l libraries,
 * and the end crt objects, which runs well past a few dozen arguments. Too low
 * a cap silently drops the trailing arguments (the end crt objects, or the
 * input object itself), which a linker then reports as undefined symbols. */
#define LINUX_MAX_ARGS 256

#define AT_FDCWD (-100)

/* open() flags we care about. The 9P bundle is read-only; writable files live
 * in the in-memory ramfs, reached by a write intent or O_CREAT. */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800      /* non-blocking: reads/writes return EAGAIN not block */
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000     /* do not follow a symlink at the final component */
#define O_LARGEFILE 0x8000      /* the kernel sets this on every 64-bit open */
#define O_CLOEXEC   0x80000     /* close this fd on execve */
#define FD_CLOEXEC  1           /* the fcntl F_GETFD/F_SETFD close-on-exec bit */

/* lseek whence. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Linux st_mode type bits and dirent types. */
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFSOCK 0140000
#define DT_DIR  4
#define DT_REG  8
#define DT_CHR  2

/* Signals. The guest tracks dispositions, masks, and pending sets, acts on the
 * default-terminate action (so a process is killable, and a fatal signal sets
 * WIFSIGNALED), and delivers a caught signal to its handler: at the syscall
 * return boundary it builds a handler frame on the user stack and returns into
 * the handler, which resumes the interrupted code through rt_sigreturn. The
 * handler modes are supported: SA_SIGINFO, SA_ONSTACK (the alternate stack),
 * SA_RESTART (restart an interrupted syscall instead of EINTR), SA_NODEFER, and
 * SA_RESETHAND. Synchronous fault signals (SIGSEGV and the like) still terminate
 * rather than invoking a handler. */
#define NSIG         64         /* signals 1..64 (32 standard, the rest realtime) */
#define SIG_DFL      0          /* default action */
#define SIG_IGN      1          /* ignore */
#define SIGKILL      9          /* cannot be caught, blocked, or ignored */
#define SIGSTOP      19         /* likewise; we have no stop state, so a no-op */
#define SIGPIPE      13
#define SIGALRM      14
#define SIGCHLD      17
#define SIGCONT      18
#define SIGURG       23
#define SIGWINCH     28

/* Bit for signal s (1-based) in a 64-bit signal set. */
#define SIGBIT(s)    (1ULL << ((s) - 1))

/* sigaction flags that affect delivery. */
#define SA_SIGINFO   0x00000004u    /* 3-argument handler (sig, info, ucontext) */
#define SA_RESTORER  0x04000000u    /* sa_restorer holds the return trampoline */
#define SA_ONSTACK   0x08000000u    /* run the handler on the alternate stack */
#define SA_RESTART   0x10000000u    /* restart an interrupted syscall, not EINTR */
#define SA_NODEFER   0x40000000u    /* do not mask the signal inside its handler */
#define SA_RESETHAND 0x80000000u    /* reset to SIG_DFL after one delivery */
#define SS_DISABLE   2              /* sigaltstack is not installed */

/* A few errnos, returned as negatives in rax the way the kernel does. */
#define LERR_BADF   9
#define LERR_NOMEM  12
#define LERR_ACCES  13
#define LERR_NOTDIR 20
#define LERR_INVAL  22
#define LERR_NOTTY  25
#define LERR_NOSYS  38
#define LERR_NOENT  2
#define LERR_ROFS   30
#define LERR_PIPE   32          /* EPIPE: write to a pipe with no readers */
#define LERR_CHILD  10          /* ECHILD: wait with no children */
#define LERR_AGAIN  11          /* EAGAIN */
#define LERR_EXIST  17          /* EEXIST */
#define LERR_XDEV   18          /* EXDEV: cross-device link/rename */
#define LERR_ISDIR  21          /* EISDIR */
#define LERR_NOTEMPTY 39        /* ENOTEMPTY */
#define LERR_RANGE  34          /* ERANGE: getcwd buffer too small */
#define LERR_PERM   1           /* EPERM: an operation the overlay forbids */
#define LERR_SPIPE  29          /* ESPIPE: seek on a pipe or other non-seekable fd */
#define LERR_NOTSUP 95          /* EOPNOTSUPP: the filesystem lacks this feature */
#define LERR_FAULT  14          /* EFAULT: a user pointer fell outside the user window */
#define LERR_NAMETOOLONG 36     /* ENAMETOOLONG: a user path did not fit the bounce buffer */
#define LERR_NOSPC  28          /* ENOSPC: a write to /dev/full */
#define LERR_SRCH   3           /* ESRCH: kill of a pid that does not exist */
#define LERR_INTR   4           /* EINTR: a blocking call interrupted by a signal */
#define LERR_NOEXEC 8           /* ENOEXEC: not an executable (bad "#!" line) */
#define LERR_LOOP   40          /* ELOOP: too many nested "#!" interpreters */
#define LERR_NOTSOCK   88       /* ENOTSOCK: a socket op on a non-socket fd */
#define LERR_ADDRINUSE 98       /* EADDRINUSE: bind to a name already in use */
#define LERR_ISCONN    106      /* EISCONN: connect on a connected socket */
#define LERR_NOTCONN   107      /* ENOTCONN: op needs a connected socket */
#define LERR_CONNREFUSED 111    /* ECONNREFUSED: no listener at the address */

/* An internal marker (never returned to user): a blocking syscall was cut short
 * by a deliverable caught signal. The signal-delivery boundary turns it into a
 * handler invocation, then either restarts the syscall (SA_RESTART) or reports
 * EINTR. Matches the kernel's internal ERESTARTSYS value. */
#define LERR_RESTARTSYS 512

/* ELF auxv entry tags. */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

/* How many auxv entries build_stack_vec writes, AT_NULL included. */
#define LINUX_NAUXV 15

/* The program stack lives at the top of the per-process user window, its pages
 * pre-mapped so growth never faults (which would fault on the stack itself). */

/* Print a signed decimal, since con_printf's specifier set is not relied on. */
static void
put_dec(long v)
{
    char b[24];
    int n = 0;
    unsigned long u;

    if (v < 0) {
        con_putc('-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        b[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    while (n)
        con_putc(b[--n]);
}

/* Print an unsigned value in hex with a 0x prefix, for fault diagnostics. */
static void
put_hex(uint64_t u)
{
    char b[16];
    int n = 0;

    con_putc('0');
    con_putc('x');
    do {
        int d = (int)(u & 0xf);
        b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        u >>= 4;
    } while (u);
    while (n)
        con_putc(b[--n]);
}

/****************************************************************
 * File descriptors over the read-only 9P bundle.
 ****************************************************************/

struct iovec {
    const void *base;
    unsigned long len;
};

/* The Linux struct stat the stat/fstat/newfstatat syscalls fill. Its layout is
 * architecture-specific: x86-64 orders st_nlink before st_mode, while the
 * generic layout AArch64 uses puts a 32-bit st_mode first, so the field offsets
 * up to st_size differ. The field names match across both, so fill_stat and the
 * stat handlers are written once against them. */
#if defined(__aarch64__)
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    uint64_t st_atime;  uint64_t st_atime_nsec;
    uint64_t st_mtime;  uint64_t st_mtime_nsec;
    uint64_t st_ctime;  uint64_t st_ctime_nsec;
    uint32_t __unused4; uint32_t __unused5;
};
#else
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;  uint64_t st_atime_nsec;
    uint64_t st_mtime;  uint64_t st_mtime_nsec;
    uint64_t st_ctime;  uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
#endif

/* The getdents64 record. d_name is NUL-terminated and the record is padded so
 * d_reclen is 8-aligned. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define LINUX_NFD   128                 /* per-process descriptors (0..NFD-1) */
#define LINUX_NPIPE 4
#define PIPE_CAP    4096

/* The writable in-memory overlay: files and directories created or written by
 * the guest, keyed by normalized bundle-relative path ("" is the root). A
 * compiler toolchain and a configure run create many files across a few
 * directories, so the table is generous and names hold a nested path. The
 * overlay is the fallback for anything not under a writable host mount; a real
 * build directs its large outputs (object files, archives, the linked binary)
 * to such a mount, so the overlay holds scratch and generated sources. Both
 * caps are sized for that: the per-file cap clears a medium generated file
 * (ICU's data is ~30 MiB), and the table holds a configure/compile run's files.
 * The buffers are kmalloc'd on demand from the growable kernel heap, so the
 * real ceiling is guest RAM, not these numbers. RAMFS_MAX must stay below
 * 0x10000: an overlay inode is 0x10000+index and the pipe inode range starts
 * at 0x20000 (see the inode comments below), so the two must not overlap. */
#define RAMFS_MAX     8192
#define RAMFS_NAME    192
#define RAMFS_FILE_MAX (128u * 1024u * 1024u)   /* per-file cap */

struct ofile;                   /* shared open-file description, defined below */
struct lfd;

/* Every descriptor has a kind that names its backend. The kind selects an ops
 * table (read/write/poll/ref/close), so the generic syscall paths dispatch
 * through the backend instead of testing a pile of per-type booleans, and a new
 * backend is added by giving it a kind and an ops entry. Fields below the kind
 * are backend-private (a pipe's direction, a device number, a pool index). */
enum fd_kind {
    FD_NONE = 0,                /* an unused slot */
    FD_CON,                     /* the console (stdin/out/err) */
    FD_PIPE,                    /* a pipe end (wr picks the direction) */
    FD_RAM,                     /* a writable-overlay file */
    FD_9P,                      /* a 9P-backed file (bundle ro, or host mount) */
    FD_DIR,                     /* a directory (9P-backed or overlay-only) */
    FD_DEV,                     /* a synthetic /dev character device */
    FD_EPOLL,                   /* an epoll instance */
    FD_EVENTFD,                 /* an eventfd counter */
    FD_SOCK,                    /* an AF_UNIX socket endpoint */
};

struct fd_ops {
    const char *name;
    long  (*read)(struct lfd *f, void *buf, unsigned long n);
    long  (*write)(struct lfd *f, const void *buf, unsigned long n);
    short (*poll)(struct lfd *f, short events);
    void  (*ref)(struct lfd *f);        /* dup/fork: take one backend reference */
    void  (*close)(struct lfd *f);      /* drop one; free the backing when last */
};

struct lfd {
    int      used;
    enum fd_kind kind;          /* the backend this descriptor speaks to */
    const struct fd_ops *ops;   /* kind's ops table (set with the kind) */
    int      is9w;              /* FD_9P opened writable (a host rw mount) */
    int      devno;             /* which device (a DEV_* kind) when FD_DEV */
    int      epidx;             /* epoll pool index when FD_EPOLL */
    int      efdidx;            /* eventfd pool index when FD_EVENTFD */
    int      sidx;              /* usock pool index when FD_SOCK */
    int      pidx;              /* pipe pool index when FD_PIPE */
    int      ridx;              /* ramfs index when FD_RAM */
    int      wr;                /* pipe write end when set, else read end */
    int      cloexec;           /* close this fd on execve (O_CLOEXEC/FD_CLOEXEC);
                                 * subprocess marks its exec-error pipe this way
                                 * and waits for it to close as the exec signal */
    int      oflags;            /* open access mode (O_RDONLY/WRONLY/RDWR) plus
                                 * O_APPEND, reported by fcntl(F_GETFL): glibc
                                 * fdopen checks it to accept the stream mode */
    uint32_t p9fid;             /* backing 9P fid, 0 for the console fds */
    /* The seekable position lives in a shared open-file description, so a dup'd
     * or forked fd advances the same offset, as Unix requires: writes by any
     * sharer move the one cursor, which is what lets a shell point several
     * commands at one redirected file. NULL for a console or pipe fd, which
     * have no position. */
    struct ofile *of;
    uint64_t size;
    uint64_t ino;               /* stable identity (9P qid path): a fresh fd for
                                 * the same file reports the same inode, which
                                 * the dynamic linker relies on to dedup a
                                 * library it has already mapped */
};

/* Bind a descriptor to a kind and its ops table (defined with the backends). */
static void fd_set_kind(struct lfd *f, enum fd_kind k);

/* A writable overlay entry: a file (buffer grows on write, persists across
 * close for the run) or a directory. The read-only 9P bundle and host mounts
 * cannot hold new files, so anything created or written goes here, shadowing a
 * bundle path of the same name. See the RAMFS_* sizes above. */
struct ramfile {
    int      used;
    int      is_dir;
    int      is_link;           /* symlink: data holds the target path string */
    int      unlinked;          /* name removed; slot held until the last fd closes */
    int      nref;              /* open fds referencing this entry */
    char     name[RAMFS_NAME];
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    uint64_t mtime;             /* seconds; stamped on create and write */
};

/* The guest has no battery clock. The host passes its real wall-clock time at
 * boot (boot_unix_time); the realtime clock is that plus the elapsed monotonic
 * time. A file written during a run is then stamped near real time, so it beats
 * both the read-only bundle files (reported at the epoch) and host-mounted
 * sources that carry their real host mtimes: autotools' sanity check rejects a
 * build tree whose freshly created file is older than its sources. A host that
 * supplies no time (0) falls back to the fixed date below. */
#define GUEST_EPOCH 1735689600ULL       /* 2025-01-01 UTC: fallback only */

static uint64_t
guest_epoch(void)
{
    uint64_t t = boot_unix_time();
    return t ? t : GUEST_EPOCH;
}

/* Nanoseconds since boot, monotonic. With the host preemption timer running,
 * this comes from the tick counter the host advances in wall time (accurate and
 * steady regardless of the guest's interrupt state). Without a timer there is no
 * real clock, so it falls back to the cycle counter read as nanoseconds (the old
 * behavior: it assumes roughly 1 GHz and only stands in for a real clock). */
static uint64_t
guest_mono_ns(void)
{
    uint32_t hz = boot_timer_hz();
    if (hz == 0)
        return arch_cycles();           /* no timer: cycles as ns, approximate */
    uint64_t per_tick_ns = 1000000000ULL / hz;
    return boot_timer_ticks() * per_tick_ns;
}

static uint64_t
guest_now_sec(void)
{
    return guest_epoch() + guest_mono_ns() / 1000000000ULL;
}

/* Overlay files are timestamped from the same wall clock the time syscalls
 * report (GUEST_EPOCH + elapsed), so a freshly built file is never in the
 * future relative to the clock make reads, which otherwise warns of clock skew.
 * The stamp is held strictly above the epoch, so a generated file still beats
 * the read-only bundle files (reported at the epoch) and passes autotools'
 * "not older than the sources" sanity check, and never steps backwards, so a
 * later write is never older than an earlier one. Within one clock second
 * several files share a stamp, as on a real second-granularity filesystem. */
static uint64_t fs_last_stamp;

static uint64_t
fs_stamp(void)
{
    uint64_t t = guest_now_sec();
    uint64_t ep = guest_epoch();
    if (t <= ep)
        t = ep + 1;
    if (t < fs_last_stamp)
        t = fs_last_stamp;
    fs_last_stamp = t;
    return t;
}

/* The big per-run tables (the overlay, the open-file descriptions, and the
 * per-process state and thread records) are allocated once from the kernel heap
 * at init, not kept in .bss, so their element counts are not bounded by the
 * kernel image's room below the user window. See linux_tables_init. */
static struct ramfile *ramfs;           /* [RAMFS_MAX] */

/* A unidirectional in-memory pipe: a byte ring with a reader and writer end
 * count. Single-threaded and non-blocking, which is enough for a program that
 * writes then reads; a read on an empty pipe returns 0 (end of data). */
struct lpipe {
    int      used;
    int      readers;
    int      writers;
    uint32_t head;              /* next byte to read */
    uint32_t count;             /* bytes buffered */
    uint8_t  buf[PIPE_CAP];
};

/* The pipe pool is shared across processes: a pipe inherited across fork is the
 * same buffer in parent and child. File tables and heaps are per process. */
/* Enough live threads (processes and their threads, plus zombies awaiting reap)
 * for a real parallel build: configure nests subshells and pipelines, gcc fans
 * out to cc1/as/ld, and a `make -j` holds many such pipelines live at once. The
 * procs and shareds tables are kmalloc'd from the growable kernel heap at init
 * (see linux_tables_init), so a slot costs one struct lshared (~15 KiB, its fd
 * table and mmap-hole list dominate) off the heap, not the sub-VM_USER_LO
 * kernel image. TASK_MAX must stay above this (every thread runs on a libOS
 * task); see task.h. */
#define LINUX_NPROC  256
#define LINUX_MMAP_HOLES 256            /* freed mmap spans tracked per address space */
#define LINUX_KSTACK (16u * 1024)       /* per-process syscall handler stack */
#define CLONE_VM     0x00000100u        /* clone shares the address space (thread) */
static struct lpipe pipes[LINUX_NPIPE];

/* The epoll pool. An epoll fd owns an interest list of (fd, events, data)
 * entries that epoll_wait scans for readiness the same way poll does. The pool
 * is global and refcounted so dup and fork can share one instance, freed when
 * the last fd across all processes closes. Level-triggered: a watched fd is
 * reported every time it is ready, which matches poll's model; EPOLLET is
 * accepted but treated as level-triggered. */
#define EPOLL_MAX   16              /* concurrent epoll instances, all processes */
#define EPOLL_NINT  256             /* interest-list entries per epoll */
struct epitem {
    int      used;
    int      fd;                    /* the watched descriptor */
    int      armed;                 /* 0 after a ONESHOT report, until re-MOD'd */
    uint32_t events;                /* requested EPOLL* mask */
    uint64_t data;                  /* opaque epoll_data the caller gets back */
};
struct epoll {
    int      used;
    int      nref;                  /* open fds referencing this instance */
    struct epitem items[EPOLL_NINT];
};
static struct epoll epolls[EPOLL_MAX];

/* The eventfd pool. An eventfd is a 64-bit counter behind a descriptor: a write
 * adds to it, a read returns it (and clears it, or in semaphore mode returns 1
 * and decrements), and it polls readable while non-zero. libuv builds its async
 * wakeup handle on one (EFD_NONBLOCK|EFD_CLOEXEC), and uv_loop_init fails without
 * it, so node's snapshot builder could not start. The pool is global and
 * refcounted so dup and fork share one counter, as they must: one thread writes
 * to wake the loop that another reads. */
#define EVENTFD_MAX  16
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  0x800
#define EVENTFD_MAXVAL 0xfffffffffffffffeULL    /* ULLONG_MAX - 1, the Linux cap */
struct leventfd {
    int      used;
    int      nref;                  /* open fds referencing this counter */
    int      semaphore;             /* EFD_SEMAPHORE: read returns 1 and decrements */
    uint64_t count;
};
static struct leventfd eventfds[EVENTFD_MAX];

/* AF_UNIX stream sockets, entirely in-guest. A socket endpoint is one of three
 * shapes: fresh (just socket()), a passive listener (bind + listen), or a
 * connected stream with a peer endpoint. A connection is a pair of stream
 * endpoints, each holding the receive ring the other writes into, so a write on
 * one appends to the peer's buf and a read drains this endpoint's own buf. The
 * pool is global and refcounted like pipes so an endpoint inherited across fork
 * or dup is the same object. connect() finds a listener by its bound path (a
 * plain in-guest name, no filesystem node), makes the server-side endpoint, and
 * queues it for accept(). Readiness feeds poll/epoll through poll_ready, so the
 * sockets work under an event loop. */
#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_TYPE_MASK  0x0f            /* type is the low bits; flags are high */
#define SOCK_NONBLOCK   0x800           /* == O_NONBLOCK */
#define SOCK_CLOEXEC    0x80000         /* == O_CLOEXEC */
#define USOCK_MAX       64              /* concurrent endpoints, all processes */
#define USOCK_BUF       16384           /* per-direction stream buffer */
#define USOCK_BACKLOG   64              /* pending connections a listener holds */
#define USOCK_PATHMAX   108             /* sun_path */
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2
struct usock {
    int      used;
    int      nref;                  /* open fds referencing this endpoint */
    int      listening;             /* a passive (listen) socket */
    int      bound;                 /* bind() gave it a path */
    char     path[USOCK_PATHMAX];   /* the bound/listen address */
    int      backlog;
    int      acceptq[USOCK_BACKLOG];/* server endpoint indices awaiting accept */
    int      acceptq_n;
    int      acceptq_head;
    int      peer;                  /* connected: peer endpoint index, else -1 */
    int      peer_closed;           /* the peer endpoint has gone away (read EOF) */
    int      shut_rd, shut_wr;      /* shutdown() halves */
    uint32_t head, count;           /* receive ring: bytes the peer wrote to us */
    uint8_t  buf[USOCK_BUF];
};
static struct usock *usocks;            /* [USOCK_MAX], kmalloc'd at init */

/* A shared open-file description: the offset state that several descriptors can
 * share. One is made per open(); dup and fork copy the pointer, not the offset,
 * and bump nref, so every sharer advances the same cursor. The pool is global
 * (like pipes and the 9P fid pool) because a fork shares it across processes,
 * while the fd tables are per process. Freed when the last fd closes (nref 0). */
struct ofile {
    int      nref;              /* fds across all processes pointing here */
    uint64_t pos;               /* read/write cursor, or 9P dir offset (phase 1) */
    int      d9done;            /* getdents: the 9P phase is finished */
    uint32_t dov;               /* getdents: overlay scan cursor */
    int      dsynth;            /* getdents: synthetic-entry cursor (/dev nodes,
                                 * and the "dev" entry listed in the root) */
    /* A directory's normalized key, for the overlay phase of getdents and for
     * resolving *at paths and fchdir against an open directory fd. Fixed at
     * open; lives here (not in struct lfd) so the per-process fd table stays
     * small enough to grow the fd count without overrunning the kernel image. */
    char     dpath[RAMFS_NAME];
};

/* One per concurrently open file or directory; dups and forks share, so this
 * need only cover distinct opens. Sized above the per-process fd cap times a
 * few busy processes, since the pool is global while the fd tables are not. */
#define LINUX_NOFILE 512
static struct ofile *ofiles;            /* [LINUX_NOFILE] */

static struct ofile *
ofile_alloc(void)
{
    for (int i = 0; i < LINUX_NOFILE; i++)
        if (ofiles[i].nref == 0) {
            memset(&ofiles[i], 0, sizeof ofiles[i]);
            ofiles[i].nref = 1;
            return &ofiles[i];
        }
    return NULL;
}

static void
ofile_unref(struct ofile *of)
{
    if (of && of->nref > 0)
        of->nref--;
}

/* Trapframe field offsets, mirrored in arch/x86_64_linuxsys.S as TF_*. The
 * SYSCALL assembly saves the user registers here on entry and restores them on
 * return, and fork copies the frame into the child so it resumes at the same
 * instruction with rax = 0. The checks below catch any layout drift. */
/* Process state shared by the threads of one process: the address space, file
 * table, and heap arena. fork makes a new one (a private copy); a thread
 * (clone with CLONE_VM) shares its parent's by pointer. refs counts the live
 * threads holding it; the address space and files are freed when refs hits 0. */
struct lshared {
    int      used;
    int      refs;              /* live threads sharing this process */
    vmspace_t vmspace;
    int      pid;
    int      parent;            /* pid of the parent, or 0 for the first process */
    int      main_task;         /* libOS task of the main thread, for wait4 reap */
    int      exited;            /* the process has terminated; status is set */
    long     exit_status;
    int      exit_sig;          /* if nonzero, terminated by this signal (wait4
                                 * reports WIFSIGNALED instead of WIFEXITED) */
    /* Signal dispositions are per process (shared by its threads); the pending
     * set here holds process-directed signals (kill). sa_handler is SIG_DFL,
     * SIG_IGN, or a user handler address (invoked at the syscall boundary by
     * deliver_signal). Inherited across fork, reset to default (ignored kept)
     * across execve. */
    struct ksigaction {
        uint64_t handler;
        uint64_t flags;
        uint64_t restorer;
        uint64_t mask;
    } sigact[NSIG];
    uint64_t sig_pending;       /* process-directed signals awaiting action */
    /* ITIMER_REAL: a per-process interval timer that raises SIGALRM. The deadline
     * is a timer tick (0 disarmed); the interval, in ticks, re-arms it on expiry
     * (0 for a one-shot). The tick hook raises SIGALRM when the deadline passes. */
    uint64_t itimer_real_deadline;
    uint64_t itimer_real_interval;
    /* A posix_spawn child (clone with CLONE_VM|CLONE_VFORK) borrows its parent's
     * address space until it execs its own, so execve and wait4 must not destroy
     * it. While vforking, the parent is blocked on vfork_waiter and the child
     * wakes it at exec or exit. */
    int      vm_borrowed;
    int      vforking;
    int      vfork_waiter;      /* task id of the blocked parent */
    struct lfd fdtab[LINUX_NFD];
    /* The heap arena. heap_brk grows up from heap_base (set just above the
     * loaded program image, which varies by binary); heap_mmap grows down from
     * heap_mmaptop. munmap reclaims: a freed span at the heap_mmap low-water is
     * popped back into the bump region, and one in the middle becomes a hole in
     * mmap_holes that a later mmap reuses first-fit. Without this a program that
     * churns mmap/munmap (a compiler's gc) exhausts the descending arena though
     * its live footprint is small. */
    uint64_t heap_base, heap_brk, heap_mmap, heap_mmaptop;
    struct { uint64_t va, len; } mmap_holes[LINUX_MMAP_HOLES];
    int      mmap_nhole;
    /* The current working directory, as a normalized overlay key (bundle-relative,
     * no leading slash; "" is the root). Relative paths resolve against it. It is
     * per-process: shared by a process's threads, copied on fork and on the
     * posix_spawn child, and preserved across execve. */
    char     cwd[RAMFS_NAME];
};

/* A thread: one schedulable context. Its trapframe (the arch's user register
 * set) is first so the trap assembly reaches it by fixed offsets, with the
 * kstack_top right after it (the assembly's TF_KSTACK), and lproc_cur the
 * running thread. Everything it shares with its peers lives in sh. */
struct lproc {
    struct trapframe tf;    /* arch backend; offset 0, size feeds TF_KSTACK */
    uint64_t kstack_top;    /* top of the per-thread syscall handler stack */
    /* --- fields below are never touched by the assembly --- */
    struct lshared *sh;     /* the process this thread belongs to */
    unsigned char *kstack;  /* kmalloc base of the handler stack */
    int used;
    int tid;                /* thread id; for the main thread, equals sh->pid */
    int ismain;             /* the process's first thread */
    int task_id;            /* libOS task running this thread */
    int exited;
    long exit_status;
    uint64_t tls;           /* thread pointer (FS base / TPIDR), restored on switch */
    uint64_t clear_ctid;    /* CLONE_CHILD_CLEARTID: zero + futex-wake on exit */
    /* The signal mask is per thread; sig_pending holds thread-directed signals
     * (tgkill). The alt stack is used when an SA_ONSTACK handler is delivered. */
    uint64_t sig_blocked;   /* blocked (masked) signals, inherited on clone */
    uint64_t sig_pending;   /* thread-directed signals awaiting action */
    uint64_t ss_sp, ss_size;
    int      ss_flags;
    uint64_t sleep_deadline;/* nonzero: this thread sleeps until this timer tick */
    long cur_nr;            /* syscall this thread is currently in (spin watchdog) */
};

_Static_assert(__builtin_offsetof(struct lproc, tf) == 0, "trapframe first");
_Static_assert(__builtin_offsetof(struct lproc, kstack_top) == sizeof(struct trapframe),
               "kstack_top follows the trapframe (the assembly's TF_KSTACK)");

static struct lshared *shareds;         /* [LINUX_NPROC], heap, see linux_tables_init */
static struct lproc *procs;             /* [LINUX_NPROC] */

/* The running thread. Set by the resume hook before the scheduler runs a thread
 * task, and read by the assembly as lproc_cur. All the do_* syscall helpers
 * reach their file table and arena through it. */
struct lproc *lproc_cur;

/* Reach the running process's shared state by the bare names the helpers use. */
#define fds       (lproc_cur->sh->fdtab)
#define heap_base (lproc_cur->sh->heap_base)
#define brk_cur   (lproc_cur->sh->heap_brk)
#define mmap_cur  (lproc_cur->sh->heap_mmap)
#define mmap_top  (lproc_cur->sh->heap_mmaptop)
#define mholes    (lproc_cur->sh->mmap_holes)
#define nhole     (lproc_cur->sh->mmap_nhole)
#define cwd_key   (lproc_cur->sh->cwd)

static long do_execve(const char *path, const char *const *argv,
                      const char *const *envp);
static struct lfd *fd_get(long fd);
static void __attribute__((noreturn)) do_exit(long code, int group);
static vmspace_t linux_vmspace_setup(void);
/* Signals: take_fatal_signal reports the lowest pending signal whose action is
 * to terminate (or 0), consuming any pending signal that is ignored.
 * signal_killpoint acts on it by terminating the process; it is called at the
 * syscall boundary and after each cooperative sleep, so a thread blocked in a
 * syscall is killable. Defined with the signal code below. */
static int take_fatal_signal(void);
static void signal_killpoint(void);
/* next_caught_signal reports the lowest pending, unblocked signal that has a user
 * handler installed; signal_deliverable is the boolean form blocking syscalls
 * poll to break out so the handler runs at the dispatch boundary. */
static int signal_deliverable(void);
static void deliver_signal(int sig, long result, int restart, int si_code,
                           uint64_t si_addr);
static struct lshared *proc_by_pid(int pid);
static void wake_process_threads(const struct lshared *sh);
/* linux_jump is declared by the arch backend (linux_arch.h); linux_resume takes
 * struct lproc, which is defined here, so it is declared here. */
extern void linux_resume(struct lproc *p) __attribute__((noreturn));

/* Called from the CPU fault path (x86_64_fault.S) when a process faults in
 * CPL 3. The fault path saves the faulting user context into lproc_cur->tf and
 * redirects its IRETQ here, in ring 0 on the process's handler stack, with the
 * trap vector and the faulting address. It runs a handler if one is installed
 * for the signal, else terminates just that process instead of halting the
 * guest. Defined with the signal code below. */
void linux_fault_entry(long vec, uint64_t addr) __attribute__((noreturn));

/* The giant lock for the Linux syscall layer. The scheduler is preemptive and
 * SMP-capable, but the syscall handlers share mutable tables (the fd table, the
 * mmap arena, the writable overlay, the futex wait set) with no finer-grained
 * locking. Without serialisation the timer can switch Linux tasks in the middle
 * of a handler, so two threads of one process interleave on those tables, and
 * the futex compare-and-park can lose a wakeup. One lock, held across each
 * syscall dispatch, makes a handler body run to a well-defined point before any
 * other Linux syscall starts.
 *
 * It is dropped around every voluntary sleep (lx_block / lx_yield) so a parked
 * task never shuts the others out: a task that waits on a pipe, a child, or a
 * futex must let the task it is waiting for take the lock and make progress.
 * The acquire path yields on contention rather than spinning hard, so a holder
 * that the timer preempted is rescheduled and finishes instead of deadlocking a
 * single vcpu. (lproc_cur is still one global, so the Linux path remains single
 * vcpu for now; this lock is what a move to several would also need.) */
static volatile int giant_held;

static void
giant_lock(void)
{
    while (__atomic_exchange_n(&giant_held, 1, __ATOMIC_ACQUIRE))
        task_yield();           /* contended: let the holder run, then retry */
}

static void
giant_unlock(void)
{
    __atomic_store_n(&giant_held, 0, __ATOMIC_RELEASE);
}

/* Sleep from inside a syscall: drop the giant lock before parking so the task
 * that will wake us (or that we are waiting on) can take it, then reacquire on
 * the way out. A plain task_block/task_yield here would keep the lock held
 * across the sleep and deadlock whoever has to run to wake us. */
static void
lx_block(void)
{
    giant_unlock();
    task_block();
    giant_lock();
}

/* Yields since any syscall last completed. A syscall that finishes resets it
 * (lx_progress, called from the dispatch wrapper). If it climbs without bound
 * the cooperative set is livelocked: every runnable thread is yielding and none
 * is completing work, so a thread is waiting on a condition nothing will make
 * true. The watchdog dumps the task table and each stuck thread's syscall once,
 * to turn a silent 100%-CPU hang into a diagnosable one. */
static long g_yields_since_progress;

static void
lx_progress(void)
{
    g_yields_since_progress = 0;
}

static void
lx_yield(void)
{
    if (++g_yields_since_progress == 8000000) {
        con_puts("[bin] SPIN watchdog: tid ");
        put_dec(lproc_cur->tid);
        con_puts(" in syscall ");
        put_dec(lproc_cur->cur_nr);
        con_putc('\n');
        task_dump();
        /* Each live Linux thread and the syscall it is parked in. */
        for (int i = 0; i < LINUX_NPROC; i++) {
            struct lproc *p = &procs[i];
            if (!p->used)
                continue;
            con_puts("  lproc pid ");
            put_dec(p->sh ? p->sh->pid : -1);
            con_puts(" tid ");
            put_dec(p->tid);
            con_puts(p->ismain ? " main" : " thr ");
            con_puts(" syscall ");
            put_dec(p->cur_nr);
            con_puts(p->exited ? " exited" : "");
            con_putc('\n');
        }
    }
    giant_unlock();
    task_yield();
    giant_lock();
}

/* User-pointer access.
 *
 * A Linux process will run in CPL 3 once privilege separation lands, so a
 * pointer it passes to a syscall is untrusted: it may point at kernel memory,
 * the physmap, the page tables, or off the end of the address space. These
 * helpers are the single gate the kernel uses to touch user memory. Each one
 * range-checks the whole span against the user window [VM_USER_LO, VM_USER_HI)
 * and refuses anything outside it with EFAULT, so a process can never steer a
 * kernel read or write at a kernel address (the classic confused-deputy write,
 * where a bad output-buffer pointer turns a syscall into an arbitrary poke).
 *
 * While the guest still shares ring 0 the window pages and the kernel share one
 * page table, so the copy itself is a plain memcpy once the bound passes. When
 * CPL 3 and SMAP arrive, only the copy bodies change (an access-fault fixup and
 * stac/clac around the move); every caller already routes through here, so the
 * bound is enforced from this commit on, before the ring flip. */

/* True if [va, va+n) lies wholly inside the user window and clear of the stack
 * guard band. Overflow-safe: the span test is written so va+n cannot wrap past
 * the top. n == 0 is allowed and always passes (an empty access touches
 * nothing).
 *
 * The guard band is rejected so a process cannot hand the kernel a pointer into
 * it: an in-window address there is the one spot pf_handle refuses to back, so
 * a kernel copy that reached it would fault in ring 0 and halt the guest. Every
 * other in-window page either is mapped or demand-pages on the kernel's touch,
 * so once the guard is excluded a copy helper never faults (short of true OOM).
 * The band straddles the span when it starts below the end and ends above the
 * start. pf_guard_lo == pf_guard_hi (the default before arena_init) is empty
 * and rejects nothing. */
static int
user_ok(uint64_t va, uint64_t n)
{
    if (va < VM_USER_LO || va >= VM_USER_HI || n > VM_USER_HI - va)
        return 0;
    if (n && pf_guard_lo < pf_guard_hi && va < pf_guard_hi && va + n > pf_guard_lo)
        return 0;                       /* the span touches the stack guard band */
    return 1;
}

/* Copy n bytes from a user buffer into the kernel. Returns 0, or -LERR_FAULT if
 * the user range is out of bounds (and copies nothing in that case). */
static long
copy_from_user(void *dst, uint64_t usrc, uint64_t n)
{
    if (!user_ok(usrc, n))
        return -LERR_FAULT;
    memcpy(dst, (const void *)(uintptr_t)usrc, n);
    return 0;
}

/* Copy n bytes from the kernel into a user buffer. Returns 0, or -LERR_FAULT. */
static long
copy_to_user(uint64_t udst, const void *src, uint64_t n)
{
    if (!user_ok(udst, n))
        return -LERR_FAULT;
    memcpy((void *)(uintptr_t)udst, src, n);
    return 0;
}

/* Zero n bytes of a user buffer. Returns 0, or -LERR_FAULT. */
static long
clear_user(uint64_t udst, uint64_t n)
{
    if (!user_ok(udst, n))
        return -LERR_FAULT;
    memset((void *)(uintptr_t)udst, 0, n);
    return 0;
}

/* Copy a NUL-terminated string from user space into dst, at most max bytes
 * including the terminator. Returns the string length (not counting the NUL) on
 * success, -LERR_FAULT if the pointer leaves the window before a NUL is found,
 * or -LERR_NOMEM if the string does not fit in max. Checks each byte's address
 * as it goes, since the length is not known up front. */
static long
strncpy_from_user(char *dst, uint64_t usrc, uint64_t max)
{
    if (max == 0)
        return -LERR_NOMEM;
    for (uint64_t i = 0; i < max; i++) {
        uint64_t va = usrc + i;
        if (!user_ok(va, 1))
            return -LERR_FAULT;
        char c = *(const char *)(uintptr_t)va;
        dst[i] = c;
        if (c == '\0')
            return (long)i;
    }
    return -LERR_NOMEM;              /* no terminator within max bytes */
}

/* The longest user path the syscall layer accepts. Paths normalize to overlay
 * keys of at most RAMFS_NAME, so a longer raw path could never resolve anyway;
 * doubling that leaves room for a cwd-relative path before normalization. */
#define UPATH_MAX (RAMFS_NAME * 2)

/* Bounce a user path argument into a kernel buffer of UPATH_MAX bytes. Returns
 * kbuf on success, or NULL with *err set (EFAULT for a bad pointer,
 * ENAMETOOLONG for one that overruns the buffer). Every path-taking syscall
 * copies through here at the dispatcher boundary, so the path readers below
 * (resolve_path, at_path, norm_key) only ever walk kernel memory. This is also
 * why those readers cannot do the check themselves: at_path hands an absolute
 * path straight back to its caller, and do_open then resolves either that user
 * string or an internal kernel buffer, so only the dispatcher knows which
 * pointers came from user space. */
static const char *
user_path(uint64_t uptr, char *kbuf, long *err)
{
    long n = strncpy_from_user(kbuf, uptr, UPATH_MAX);
    if (n < 0) {
        *err = (n == -LERR_NOMEM) ? -LERR_NAMETOOLONG : n;
        return NULL;
    }
    *err = 0;
    return kbuf;
}

/* Set up the heap arena above the loaded program image. brk grows up from
 * heap_base (the first page past the highest loaded segment, so it never
 * overlaps the executable or its interpreter); anonymous mmap grows down from
 * just below the user stack at the top of the window. Pages in between fault in
 * on demand, so the arena is lazily backed. */
static void
arena_init(uint64_t image_top)
{
    heap_base = (image_top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (heap_base < LINUX_HEAP_BASE)
        heap_base = LINUX_HEAP_BASE;    /* a small image still gets a sane floor */
    brk_cur = heap_base;
    /* mmap grows down from just below the guard band, which is itself just below
     * the stack reservation. The guard is left unmapped; pf_handle refuses it. */
    mmap_top = (LINUX_STACK_BOTTOM - LINUX_STACK_GUARD) & ~(PAGE_SIZE - 1);
    mmap_cur = mmap_top;
    nhole = 0;
    pf_guard_hi = LINUX_STACK_BOTTOM;
    pf_guard_lo = mmap_top;
}

/* The highest address occupied by the loaded image: the executable's end, or
 * the interpreter's when the executable has one (the interpreter loads above
 * the executable). The test is the executable's has_interp, not the
 * interpreter's: the interpreter itself has no PT_INTERP, so its has_interp is
 * always zero. Getting this wrong placed the heap at the executable's end,
 * below the interpreter, so a large executable's heap grew up through the
 * interpreter's code. */
static uint64_t
image_top(const struct elf_info *exe, const struct elf_info *interp)
{
    uint64_t top = exe->load_end;
    if (exe->has_interp && interp->load_end > top)
        top = interp->load_end;
    return top;
}

static long
do_brk(uint64_t want)
{
    if (want == 0)
        return (long)brk_cur;
    if (want < heap_base || want > mmap_cur)
        return (long)brk_cur;           /* refuse; caller falls back to mmap */
    /* Linux returns brk-extended memory zero-filled. When glibc trims the heap
     * (brk shrinks) and later regrows it, the regrown pages must read as zero.
     * Release the frames on a shrink so a later regrow faults in fresh zeroed
     * pages through pf_handle, matching Linux. Without this the pages keep their
     * pre-shrink content and glibc's allocator can build chunks on dirty memory
     * and hand out allocations that overlap live ones. The range is page
     * aligned up, so a partial final page below brk_cur keeps its data. */
    if (want < brk_cur) {
        uint64_t lo = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t p = lo; p < brk_cur; p += PAGE_SIZE)
            vmspace_unmap(cur_vmspace, p);
    }
    brk_cur = want;
    return (long)brk_cur;
}

/* Copy [off, off+len) of the file open on fd into guest VAs [va, va+len). The
 * writes fault each page in via pf_handle (a zeroed frame), so the tail of the
 * last page past end-of-file is already zero: that is the implicit .bss fill a
 * file mapping gets for the part of the final page beyond the file. There is no
 * lazy file paging and no copy-on-write; the mapping is a private eager copy,
 * which suits a run-once program and matches how elf_load_fid loads. */
static long
mmap_fill(long fd, uint64_t va, uint64_t off, uint64_t len)
{
    struct lfd *f = fd_get(fd);
    if (!f || f->kind == FD_CON || f->kind == FD_PIPE || f->kind == FD_DIR)
        return -LERR_BADF;
    uint8_t *dst = (uint8_t *)(uintptr_t)va;
    if (f->kind == FD_RAM) {                     /* ramfs file: copy from its buffer */
        struct ramfile *rf = &ramfs[f->ridx];
        for (uint64_t i = 0; i < len && off + i < rf->len; i++)
            dst[i] = rf->data[off + i];
        return 0;
    }
    uint64_t done = 0;                  /* 9P bundle file */
    while (done < len) {
        long n = p9_read(f->p9fid, off + done, dst + done, (uint32_t)(len - done));
        if (n <= 0)
            break;                      /* short read: the rest stays zero */
        done += (uint64_t)n;
    }
    return 0;
}

/* Reserve n bytes (page-rounded) of mmap address space: reuse a freed hole
 * first-fit, else bump the descending low-water down. Returns the base VA, or 0
 * if no hole fits and the gap to brk is too small. Does not back the pages; they
 * fault in lazily (a reused hole was fully unmapped by munmap, so it reads as
 * zero, which anonymous mmap requires). */
static uint64_t
mmap_reserve(uint64_t n)
{
    for (int i = 0; i < nhole; i++) {
        if (mholes[i].len < n)
            continue;
        uint64_t va = mholes[i].va;
        mholes[i].va += n;
        mholes[i].len -= n;
        if (mholes[i].len == 0)
            mholes[i] = mholes[--nhole];    /* drop the emptied slot */
        return va;
    }
    if (n > mmap_cur - brk_cur)             /* overflow-safe: mmap_cur > brk_cur */
        return 0;
    mmap_cur -= n;
    return mmap_cur;
}

/* Coalesce the hole list and pop any hole sitting at the low-water back into the
 * bump region, repeating until stable. Keeps the list short (so it rarely
 * overflows) and lets the descending arena recover as soon as its bottom frees. */
static void
mmap_coalesce(void)
{
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < nhole; i++) {
            if (mholes[i].va == mmap_cur) {         /* bottom of the stack: pop it */
                mmap_cur += mholes[i].len;
                mholes[i] = mholes[--nhole];
                changed = 1;
                break;
            }
            for (int j = 0; j < nhole; j++) {       /* merge an adjacent pair */
                if (i != j && mholes[i].va + mholes[i].len == mholes[j].va) {
                    mholes[i].len += mholes[j].len;
                    mholes[j] = mholes[--nhole];
                    changed = 1;
                    break;
                }
            }
            if (changed)
                break;
        }
    }
}

/* Return a freed mmap span [va, va+n) to the arena: record it as a hole (or, if
 * the list is full, leak the address range; the frames were still freed), then
 * coalesce. */
static void
mmap_release(uint64_t va, uint64_t n)
{
    /* Guard against a double free: if the span is already inside a hole, adding
     * it again would let mmap_reserve hand the same address out twice. */
    for (int i = 0; i < nhole; i++)
        if (va >= mholes[i].va && va + n <= mholes[i].va + mholes[i].len)
            return;
    if (nhole < LINUX_MMAP_HOLES) {
        mholes[nhole].va = va;
        mholes[nhole].len = n;
        nhole++;
    }
    mmap_coalesce();
}

/* munmap(addr, len): free the frames under the mmap arena part of the range and
 * return that span to the arena for reuse. Addresses outside [mmap_cur,
 * mmap_top) are not the mmap arena (the image, brk heap, or stack), so they are
 * left untouched. */
static long
do_munmap(uint64_t addr, uint64_t len)
{
    uint64_t n = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t va = addr & ~(PAGE_SIZE - 1);
    if (n == 0)
        return 0;

    uint64_t lo = va < mmap_cur ? mmap_cur : va;
    uint64_t hi = va + n > mmap_top ? mmap_top : va + n;
    if (lo >= hi)
        return 0;                           /* nothing of it is in the mmap arena */

    for (uint64_t p = lo; p < hi; p += PAGE_SIZE)
        vmspace_unmap(cur_vmspace, p);
    mmap_release(lo, hi - lo);
    return 0;
}

/* mmap(addr, len, prot, flags, fd, off). prot is not enforced: pf_handle maps
 * every user page writable, so this grants access, it does not restrict it.
 * MAP_FIXED maps at the exact address (the dynamic linker overlays a file
 * segment onto a span it first reserved); otherwise the mapping is bump
 * allocated descending from the top of the arena. Anonymous pages need nothing,
 * since pf_handle zeroes each frame on first touch. */
static long
do_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
        long fd, uint64_t off)
{
    (void)prot;
    uint64_t n = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (n == 0)
        return MMAP_FAILED;

    uint64_t va;
    if (flags & MAP_FIXED) {
        va = addr & ~(PAGE_SIZE - 1);
        if (va < VM_USER_LO || n > VM_USER_HI - va)   /* overflow-safe bound */
            return MMAP_FAILED;
        /* An overlay inside an existing reservation: do not move mmap_cur. The
         * frames may already hold stale bytes (the dynamic linker first maps a
         * library's whole span file-backed, then overlays each segment here),
         * so clear the range. For a file mapping the file bytes are copied back
         * over it next; for an anonymous one the zeroing is the whole point
         * (this is how the .bss overlay must read). */
        memset((void *)(uintptr_t)va, 0, n);
    } else {
        va = mmap_reserve(n);
        if (va == 0)
            return MMAP_FAILED;
        /* A bumped address was never mapped and a reused hole was unmapped by
         * munmap, so either way an anonymous mapping reads as zero on fault and
         * needs no eager backing. */
    }

    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        long r = mmap_fill(fd, va, off, len);
        if (r < 0)
            return r;
    }
    return (long)va;
}

/* fds 0,1,2 are the console; higher fds are 9P-backed. */
static struct lfd *
fd_get(long fd)
{
    if (fd < 0 || fd >= LINUX_NFD || !fds[fd].used)
        return NULL;
    return &fds[fd];
}

static int
fd_alloc(void)
{
    for (int i = 3; i < LINUX_NFD; i++)
        if (!fds[i].used)
            return i;
    return -1;
}

/* Open bundle files draw a 9P fid from a single global pool, refcounted in
 * gfidref so dup and fork can share one fid (one server-side file, hence one
 * shared read offset, as fork semantics want) and it is clunked only when the
 * last fd across all processes closes. Index into gfidref is (fid - FID_BASE). */
#define GFID_MAX 128                    /* concurrent 9P-backed opens; the host
                                         * server (P9_MAXFID) must cover this
                                         * plus the interp and scratch fids */
static uint8_t gfidref[GFID_MAX];

/* A free pool fid (count 0), or 0 if the pool is exhausted. */
static uint32_t
fid_alloc(void)
{
    for (int i = 0; i < GFID_MAX; i++)
        if (gfidref[i] == 0)
            return LINUX_FID_BASE + (uint32_t)i;
    return 0;
}

/* Copy src into fd slot dst, taking a reference on whatever backs it so a later
 * close releases it exactly once. Used by dup/dup2/dup3. */
static void
clone_fd(int dst, struct lfd *src)
{
    fds[dst] = *src;
    fds[dst].used = 1;
    fds[dst].cloexec = 0;               /* dup clears close-on-exec; the F_DUPFD_
                                         * CLOEXEC and dup3 callers set it back */
    /* Take one reference on the backend (the shared offset, pipe end, 9P fid,
     * epoll/eventfd/socket object) so a later close releases it exactly once. A
     * console or device fd has no backing and no ref op. */
    if (fds[dst].ops && fds[dst].ops->ref)
        fds[dst].ops->ref(&fds[dst]);
}

static void
fds_init(void)
{
    memset(fds, 0, sizeof(lproc_cur->sh->fdtab));
    for (int i = 0; i < 3; i++) {                   /* stdin/out/err: console */
        fds[i].used = 1;
        fd_set_kind(&fds[i], FD_CON);
        fds[i].oflags = O_RDWR;                     /* readable and writable */
    }
}

/* Normalize a path to an overlay key: strip leading slashes, drop "."
 * components, resolve ".." by popping, and join with single slashes. The root
 * is the empty string. Keeps keys canonical so create/find/stat/list all agree
 * (e.g. "/tmp/./x" and "tmp/x" map to the same entry). */
static void
norm_key(const char *path, char *out, size_t n)
{
    const char *p = path;
    size_t w = 0;

    out[0] = 0;
    while (*p == '/')
        p++;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t clen = (size_t)(p - s);
        while (*p == '/')
            p++;
        if (clen == 1 && s[0] == '.')
            continue;
        if (clen == 2 && s[0] == '.' && s[1] == '.') {
            while (w > 0 && out[w - 1] != '/')
                w--;
            if (w > 0)
                w--;                    /* drop the trailing slash */
            out[w] = 0;
            continue;
        }
        if (w > 0 && w < n - 1)
            out[w++] = '/';
        for (size_t i = 0; i < clen && w < n - 1; i++)
            out[w++] = s[i];
        out[w] = 0;
    }
}

/* Resolve a userspace path to a normalized overlay key, applying the current
 * working directory to a relative path. An absolute path (leading slash), or any
 * path when the cwd is the root, normalizes directly; otherwise the cwd is
 * prepended before normalization so "." and ".." resolve against it. This is the
 * path boundary for the syscall layer; internal helpers that already hold a key
 * use norm_key, which is idempotent on a key. */
static void
resolve_path(const char *path, char *out, size_t n)
{
    if (path[0] == '/' || cwd_key[0] == 0) {
        norm_key(path, out, n);
        return;
    }
    char tmp[RAMFS_NAME * 2];
    size_t w = 0;
    for (const char *s = cwd_key; *s && w < sizeof tmp - 1; s++)
        tmp[w++] = *s;
    if (w < sizeof tmp - 1)
        tmp[w++] = '/';
    for (const char *s = path; *s && w < sizeof tmp - 1; s++)
        tmp[w++] = *s;
    tmp[w] = 0;
    norm_key(tmp, out, n);
}

/****************************************************************
 * A synthetic /dev. These character devices are emulated entirely inside the
 * guest: no request reaches the host, so the sandbox presents the same /dev on
 * any host OS. This is part of making the guest a self-contained Linux
 * emulation rather than a pass-through to a Linux host.
 ****************************************************************/

enum {
    DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_URANDOM,
    DEV_TTY, DEV_CONSOLE, DEV_STDIN, DEV_STDOUT, DEV_STDERR,
};

/* Synthetic inode numbers for /dev, in a range clear of the ramfs (0x10000),
 * pipe (0x20000) ranges so identities never collide. */
#define DEV_INO_BASE 0x30000ULL

/* Synthetic inode base for bound AF_UNIX socket paths, clear of the ranges
 * above. A bound socket has a filesystem name but no file behind it. */
#define SOCK_INO_BASE 0x40000ULL

static const struct devnode {
    const char *name;           /* leaf name under /dev */
    int         kind;           /* a DEV_* value */
} devtab[] = {
    { "null",    DEV_NULL },    { "zero",    DEV_ZERO },
    { "full",    DEV_FULL },    { "random",  DEV_RANDOM },
    { "urandom", DEV_URANDOM }, { "tty",     DEV_TTY },
    { "console", DEV_CONSOLE }, { "stdin",   DEV_STDIN },
    { "stdout",  DEV_STDOUT },  { "stderr",  DEV_STDERR },
};
#define NDEV ((int)(sizeof devtab / sizeof devtab[0]))

/* The device kind at a normalized key ("dev/null" -> DEV_NULL), or 0 when the
 * key does not name a known /dev node. */
static int
dev_lookup(const char *key)
{
    if (key[0] != 'd' || key[1] != 'e' || key[2] != 'v' || key[3] != '/')
        return 0;
    const char *leaf = key + 4;
    for (int i = 0; i < NDEV; i++)
        if (strcmp(leaf, devtab[i].name) == 0)
            return devtab[i].kind;
    return 0;
}

/* True when the key names the synthetic /dev directory itself. */
static int
dev_is_dir(const char *key)
{
    return key[0] == 'd' && key[1] == 'e' && key[2] == 'v' && key[3] == 0;
}

/* Bind a fresh fd to a /dev character device. These carry no offset, no 9P fid,
 * and no overlay entry: read and write are computed on the fly by kind. */
static long
open_dev(int kind, long flags)
{
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], FD_DEV);
    fds[fd].devno = kind;
    fds[fd].ino = DEV_INO_BASE + (uint64_t)kind;
    fds[fd].oflags = (int)(flags & (O_ACCMODE | O_APPEND));
    return fd;
}

/* read() from a /dev node. zero and full yield an endless run of zero bytes;
 * random and urandom yield a (non-cryptographic) pseudo-random stream; the rest
 * are at end of file. */
static long
dev_read(int kind, void *buf, unsigned long count)
{
    switch (kind) {
    case DEV_ZERO:
    case DEV_FULL:
        memset(buf, 0, count);
        return (long)count;
    case DEV_RANDOM:
    case DEV_URANDOM: {
        uint8_t *b = buf;
        uint64_t s = arch_cycles();
        for (unsigned long i = 0; i < count; i++) {
            b[i] = (uint8_t)(s >> 24);
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        }
        return (long)count;
    }
    default:                    /* null, tty, console, std*, and anything else */
        return 0;               /* EOF */
    }
}

/* write() to a /dev node. The console-like nodes echo to the UART; full reports
 * a disk-full error; the rest silently swallow the bytes. */
static long
dev_write(int kind, const char *buf, unsigned long len)
{
    switch (kind) {
    case DEV_FULL:
        return -LERR_NOSPC;
    case DEV_TTY:
    case DEV_CONSOLE:
    case DEV_STDOUT:
    case DEV_STDERR:
        for (unsigned long i = 0; i < len; i++)
            con_putc(buf[i]);
        return (long)len;
    default:                    /* null, zero, random, urandom, stdin: discard */
        return (long)len;
    }
}

static int
ramfs_find(const char *name)
{
    char key[RAMFS_NAME];
    norm_key(name, key, sizeof key);
    for (int i = 0; i < RAMFS_MAX; i++)
        if (ramfs[i].used && !ramfs[i].unlinked &&
            strcmp(ramfs[i].name, key) == 0)
            return i;
    return -1;
}

/* Writable host-mount prefixes (normalized keys, no leading slash), delivered
 * by the host in the boot block. A create or write under one of these goes to
 * the host over 9P instead of to the in-RAM overlay, so a build's output lands
 * on the host filesystem. Everything else keeps the overlay as its writable
 * layer. */
#define RWMOUNT_MAX JOTUN_BOOT_RWMOUNT_MAX
static char rwmounts[RWMOUNT_MAX][RAMFS_NAME];
static int  nrwmounts;

static void
linux_rwmount_init(void)
{
    nrwmounts = 0;
    int n = boot_nrwmount();
    for (int i = 0; i < n && nrwmounts < RWMOUNT_MAX; i++) {
        const char *p = boot_rwmount(i);
        if (!p)
            continue;
        norm_key(p, rwmounts[nrwmounts], RAMFS_NAME);
        if (rwmounts[nrwmounts][0])     /* ignore "/" (the whole root) */
            nrwmounts++;
    }
}

/* True when key equals a writable-mount prefix or is nested under one. */
static int
under_rw_mount(const char *key)
{
    for (int i = 0; i < nrwmounts; i++) {
        size_t l = strlen(rwmounts[i]);
        if (strncmp(key, rwmounts[i], l) == 0 && (key[l] == 0 || key[l] == '/'))
            return 1;
    }
    return 0;
}

/* Follow an overlay symlink at the final path component. If key names an
 * overlay symlink, replace it in place with the link's target resolved to a
 * key, repeating for a chain of links up to a small limit. An absolute target
 * re-normalizes from the root; a relative target resolves against the link's
 * parent directory. This is leaf-only: a symlink in an interior component is
 * not followed, which suits the overlay (it holds only the links a program
 * creates, and those are the tail of the path, e.g. out/tools/bin/python). A
 * non-link key, a bundle path, or a chain that exceeds the limit is left as is.
 * Callers that must act on the link itself (lstat, readlink, symlink, unlink)
 * do not call this. */
static void
follow_link_key(char *key, size_t n)
{
    for (int depth = 0; depth < 16; depth++) {
        int ri = ramfs_find(key);
        if (ri < 0 || !ramfs[ri].is_link)
            return;
        char target[UPATH_MAX];
        uint32_t tl = ramfs[ri].len;
        if (tl >= sizeof target)
            tl = sizeof target - 1;
        memcpy(target, ramfs[ri].data, tl);
        target[tl] = 0;
        if (target[0] == '/') {
            norm_key(target, key, n);
        } else {
            /* Join the target onto the link's parent directory (key up to its
             * last slash), then normalize so "." and ".." resolve. */
            char joined[UPATH_MAX];
            size_t cut = strlen(key);
            while (cut > 0 && key[cut - 1] != '/')
                cut--;
            if (cut > 0)
                cut--;                      /* drop the slash */
            size_t w = 0;
            for (size_t i = 0; i < cut && w < sizeof joined - 1; i++)
                joined[w++] = key[i];
            if (w < sizeof joined - 1)
                joined[w++] = '/';
            for (size_t i = 0; target[i] && w < sizeof joined - 1; i++)
                joined[w++] = target[i];
            joined[w] = 0;
            norm_key(joined, key, n);
        }
    }
}

/* Release a destroyed slot's backing buffer and mark it free. The buffer is
 * kmalloc'd by ram_grow, so it must be returned here; leaving it attached to a
 * freed slot leaks it, since ramfs_create clears data without freeing. */
static void
ramfs_free_slot(int i)
{
    if (ramfs[i].data)
        kfree(ramfs[i].data);
    ramfs[i].data = NULL;
    ramfs[i].len = 0;
    ramfs[i].cap = 0;
    ramfs[i].unlinked = 0;
    ramfs[i].used = 0;
}

/* Drop one open reference to overlay entry ri; free the slot once the last fd
 * on an unlinked entry closes (open-unlink semantics: the inode outlives the
 * name). */
static void
ramfs_unref(int ri)
{
    if (ri < 0 || !ramfs[ri].used)
        return;
    if (ramfs[ri].nref > 0)
        ramfs[ri].nref--;
    if (ramfs[ri].nref == 0 && ramfs[ri].unlinked)
        ramfs_free_slot(ri);
}

static int
ramfs_create(const char *name, int is_dir)
{
    char key[RAMFS_NAME];
    norm_key(name, key, sizeof key);
    for (int i = 0; i < RAMFS_MAX; i++) {
        if (!ramfs[i].used) {
            size_t n = 0;
            while (key[n] && n < RAMFS_NAME - 1) {
                ramfs[i].name[n] = key[n];
                n++;
            }
            ramfs[i].name[n] = 0;
            ramfs[i].used = 1;
            ramfs[i].is_dir = is_dir;
            ramfs[i].is_link = 0;
            ramfs[i].unlinked = 0;
            ramfs[i].nref = 0;
            ramfs[i].data = NULL;
            ramfs[i].len = 0;
            ramfs[i].cap = 0;
            ramfs[i].mtime = fs_stamp();
            return i;
        }
    }
    return -1;
}

/* Remove an overlay entry by name. Returns 0 if one was removed, -1 if absent.
 * An entry with open fds is kept (marked unlinked, its name no longer resolves)
 * so the fds keep reading and the slot is not reused under them; the slot and
 * its buffer free when the last fd closes. */
static int
ramfs_remove(const char *name)
{
    int i = ramfs_find(name);
    if (i < 0)
        return -1;
    if (ramfs[i].nref > 0) {
        ramfs[i].unlinked = 1;          /* free deferred to the last close */
        return 0;
    }
    ramfs_free_slot(i);
    return 0;
}

/* If entry name `nm` is a direct child of normalized directory key `dk`, return
 * a pointer to its leaf name; otherwise NULL. The root (dk == "") parents every
 * top-level name (one with no slash). */
static const char *
ramfs_child_leaf(const char *nm, const char *dk)
{
    size_t dl = strlen(dk);
    const char *leaf;

    if (dl == 0) {
        if (nm[0] == 0)
            return NULL;
        leaf = nm;
    } else {
        if (strncmp(nm, dk, dl) != 0 || nm[dl] != '/')
            return NULL;
        leaf = nm + dl + 1;
        if (*leaf == 0)
            return NULL;
    }
    for (const char *q = leaf; *q; q++)
        if (*q == '/')
            return NULL;                /* deeper descendant, not a direct child */
    return leaf;
}

/* Does any overlay entry live under directory key `dk` (at any depth)? Used to
 * recognize an implicit directory and to refuse removing a non-empty one. */
static int
ramfs_has_desc(const char *dk)
{
    size_t dl = strlen(dk);
    for (int i = 0; i < RAMFS_MAX; i++) {
        if (!ramfs[i].used || ramfs[i].unlinked)
            continue;
        if (dl == 0) {
            if (ramfs[i].name[0])
                return 1;
        } else if (strncmp(ramfs[i].name, dk, dl) == 0 && ramfs[i].name[dl] == '/') {
            return 1;
        }
    }
    return 0;
}

/* Is normalized key `key` a directory in the overlay? The root always is; an
 * explicit entry reports its kind; otherwise a key with descendants is an
 * implicit directory (a file was created below it without an explicit mkdir). */
static int
ramfs_is_dir_key(const char *key)
{
    if (key[0] == 0)
        return 1;
    for (int i = 0; i < RAMFS_MAX; i++)
        if (ramfs[i].used && !ramfs[i].unlinked &&
            strcmp(ramfs[i].name, key) == 0)
            return ramfs[i].is_dir;
    return ramfs_has_desc(key);
}

/* Ensure a ramfile's buffer holds at least `need` bytes, doubling as needed. */
static int
ramfs_grow(struct ramfile *rf, uint32_t need)
{
    if (need <= rf->cap)
        return 0;
    if (need > RAMFS_FILE_MAX)
        return -1;
    uint32_t ncap = rf->cap ? rf->cap : 256;
    while (ncap < need)
        ncap *= 2;
    if (ncap > RAMFS_FILE_MAX)
        ncap = RAMFS_FILE_MAX;
    uint8_t *nd = kmalloc(ncap);
    if (!nd)
        return -1;
    if (rf->len)
        memcpy(nd, rf->data, rf->len);
    if (rf->data)
        kfree(rf->data);
    rf->data = nd;
    rf->cap = ncap;
    return 0;
}

/* Create a pipe: a reader fd in pfd[0] and a writer fd in pfd[1]. */
static long
do_pipe2(int *pfd, int flags)
{
    if (!user_ok((uint64_t)(uintptr_t)pfd, 2 * sizeof *pfd))
        return -LERR_FAULT;
    int pidx = -1;
    for (int i = 0; i < LINUX_NPIPE; i++)
        if (!pipes[i].used) {
            pidx = i;
            break;
        }
    if (pidx < 0)
        return -LERR_NOMEM;

    int rfd = fd_alloc();
    if (rfd < 0)
        return -LERR_NOMEM;
    fds[rfd].used = 1;                   /* reserve so the writer gets a new slot */
    int wfd = fd_alloc();
    if (wfd < 0) {
        fds[rfd].used = 0;
        return -LERR_NOMEM;
    }

    memset(&pipes[pidx], 0, sizeof pipes[pidx]);
    pipes[pidx].used = 1;
    pipes[pidx].readers = 1;
    pipes[pidx].writers = 1;

    memset(&fds[rfd], 0, sizeof fds[rfd]);
    memset(&fds[wfd], 0, sizeof fds[wfd]);
    fds[rfd].used = fds[wfd].used = 1;
    fd_set_kind(&fds[rfd], FD_PIPE);
    fd_set_kind(&fds[wfd], FD_PIPE);
    fds[rfd].pidx = fds[wfd].pidx = pidx;
    fds[rfd].wr = 0;
    fds[wfd].wr = 1;
    fds[rfd].oflags = O_RDONLY;
    fds[wfd].oflags = O_WRONLY;

    fds[rfd].cloexec = fds[wfd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;

    pfd[0] = rfd;
    pfd[1] = wfd;
    return 0;
}

static long
pipe_read(struct lpipe *p, void *buf, unsigned long count)
{
    uint8_t *out = buf;
    unsigned long n = 0;

    while (n < count && p->count > 0) {
        out[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_CAP;
        p->count--;
    }
    return (long)n;                     /* 0 when empty: no blocking here */
}

static long
pipe_write(struct lpipe *p, const void *buf, unsigned long count)
{
    const uint8_t *in = buf;
    unsigned long n = 0;

    while (n < count && p->count < PIPE_CAP) {
        uint32_t tail = (p->head + p->count) % PIPE_CAP;
        p->buf[tail] = in[n++];
        p->count++;
    }
    return (long)n;                     /* short write when the ring fills */
}

/* Strip leading slashes: bundle paths are relative to the 9P root, and musl
 * passes absolute paths. An empty result means the root itself. */
static const char *
bundle_path(const char *p)
{
    while (*p == '/')
        p++;
    return p;
}

static long eventfd_read(struct lfd *f, void *buf, unsigned long count);
static long eventfd_write(struct lfd *f, const void *buf, unsigned long count);

/* read/write go through the fd's backend ops. The common argument checks stay
 * here; the per-kind body lives in the backend (con_*, pipe_*, ram_*, p9_*,
 * dev_*, eventfd_*, sock_*), selected by fd_set_kind. */
static long
do_write(long fd, const char *buf, unsigned long len)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (len && !user_ok((uint64_t)(uintptr_t)buf, len))
        return -LERR_FAULT;
    return f->ops->write(f, buf, len);
}

static long
do_writev(long fd, const struct iovec *iov, long cnt)
{
    long total = 0;

    if (cnt < 0)
        return -LERR_INVAL;
    if (cnt && !user_ok((uint64_t)(uintptr_t)iov, (uint64_t)cnt * sizeof *iov))
        return -LERR_FAULT;
    for (long i = 0; i < cnt; i++) {
        long n = do_write(fd, iov[i].base, iov[i].len);
        if (n < 0)
            return total ? total : n;
        total += n;
    }
    return total;
}

/* Bind a free fd to ramfile ri, positioned per O_APPEND. */
static long
open_ram(int ri, long flags)
{
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    struct ofile *of = ofile_alloc();
    if (!of)
        return -LERR_NOMEM;
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], FD_RAM);
    fds[fd].ridx = ri;
    fds[fd].of = of;
    fds[fd].oflags = (int)(flags & (O_ACCMODE | O_APPEND));
    of->pos = (flags & O_APPEND) ? ramfs[ri].len : 0;
    ramfs[ri].nref++;
    return fd;
}

/* Record a directory's normalized key on its open-file description, for the
 * overlay phase of getdents. d9done starts set when there is no 9P side to
 * enumerate. */
static void
fd_set_dir(struct lfd *f, const char *key)
{
    size_t i = 0;
    while (key[i] && i < RAMFS_NAME - 1) {
        f->of->dpath[i] = key[i];
        i++;
    }
    f->of->dpath[i] = 0;
    f->of->d9done = f->p9fid ? 0 : 1;
    f->of->dov = 0;
}

/* Rewrite a *at (dirfd, path) pair into a path the do_* helpers can resolve.
 * An absolute path, or a relative one with dirfd AT_FDCWD, is returned
 * unchanged (the do_* helpers resolve it against the cwd). Otherwise the path
 * is taken relative to the directory already open on dirfd, built as an
 * absolute "/<dir key>/<path>" in buf. coreutils removes a tree this way: it
 * opens each directory and calls unlinkat(dirfd, "name"), never chdir. On a bad
 * dirfd *err is set and NULL returned. */
static const char *
at_path(long dirfd, const char *path, char *buf, size_t n, long *err)
{
    /* dirfd is a 32-bit int; a caller passing AT_FDCWD (-100) may set only the
     * low 32 bits, so sign-extend before comparing or looking it up. */
    dirfd = (long)(int)dirfd;
    *err = 0;
    if (!path || path[0] == '/' || dirfd == AT_FDCWD)
        return path;
    struct lfd *f = fd_get(dirfd);
    if (!f) {
        *err = -LERR_BADF;
        return NULL;
    }
    if (f->kind != FD_DIR || !f->of) {
        *err = -LERR_NOTDIR;
        return NULL;
    }
    size_t w = 0;
    buf[w++] = '/';
    for (const char *s = f->of->dpath; *s && w < n - 1; s++)
        buf[w++] = *s;
    if (w > 1 && w < n - 1)             /* a slash only when the key was nonempty */
        buf[w++] = '/';
    for (const char *s = path; *s && w < n - 1; s++)
        buf[w++] = *s;
    buf[w] = 0;
    return buf;
}

/* utimensat/futimens: set a file's timestamps. A host-mount file keeps its own
 * host mtime (times are not pushed back over 9P), so for those this succeeds
 * without changing anything, which is all a build's `touch` of a freshly
 * written dependency file needs. An overlay file's mtime is stamped so a later
 * make still sees it as current. Both forms are accepted: the path form (a
 * dirfd and a user path pointer) and the fd form (a NULL user path, i.e.
 * futimens on the dirfd). The requested times are not honored precisely, only
 * "now". Kept out of the syscall dispatch so its path buffers do not add to
 * that function's single -O0 stack frame. */
static long
do_utimensat(long dirfd, uint64_t upath)
{
    if (upath == 0) {                           /* futimens(fd) */
        struct lfd *f = fd_get(dirfd);
        if (!f)
            return -LERR_BADF;
        if (f->kind == FD_RAM)
            ramfs[f->ridx].mtime = fs_stamp();
        return 0;
    }
    char pb[UPATH_MAX], ab[RAMFS_NAME], kb[RAMFS_NAME];
    long e;
    const char *up = user_path(upath, pb, &e);
    if (!up)
        return e;
    const char *p = at_path(dirfd, up, ab, sizeof ab, &e);
    if (e)
        return e;
    resolve_path(p, kb, sizeof kb);
    int ri = ramfs_find(kb);
    if (ri >= 0)
        ramfs[ri].mtime = fs_stamp();
    return 0;                                   /* host/bundle files keep mtime */
}

/* Open a file on a writable host mount over 9P: Topen an existing file (with
 * OTRUNC when asked), or Tcreate a new one in its parent directory. The fd is
 * is9w, so do_read/do_write go to the host, not the overlay. Returns the fd or
 * a negative errno. */
static long
open_9p_write(const char *key, long flags, long mode)
{
    /* Split key into its parent directory and leaf name. */
    char parent[RAMFS_NAME];
    size_t kl = strlen(key), cut = kl;
    while (cut > 0 && key[cut - 1] != '/')
        cut--;
    const char *leaf = key + cut;
    size_t plen = cut > 0 ? cut - 1 : 0;        /* drop the slash */
    memcpy(parent, key, plen);
    parent[plen] = '\0';
    if (leaf[0] == '\0')
        return -LERR_ISDIR;                     /* a trailing slash is not a file */

    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    uint32_t p9fid = fid_alloc();
    if (p9fid == 0)
        return -LERR_NOMEM;

    uint64_t size = 0, ino = p9fid;
    int exists = 0, isdir9 = 0;
    if (ns_walk(key, p9fid) == 0) {
        struct p9_stat st;
        if (p9_stat(p9fid, &st) == 0) {
            exists = 1;
            isdir9 = (st.qid.type & P9_QTDIR) != 0;
            size = st.length;
            ino = st.qid.path;
        }
    }

    if (exists && isdir9) {
        p9_clunk(p9fid);
        return -LERR_ISDIR;
    }
    if (exists) {
        if (flags & O_EXCL) {
            p9_clunk(p9fid);
            return -LERR_EXIST;
        }
        uint8_t mode = (uint8_t)(P9_ORDWR | ((flags & O_TRUNC) ? P9_OTRUNC : 0));
        if (p9_open_mode(p9fid, mode) != 0) {
            p9_clunk(p9fid);
            return -LERR_ACCES;
        }
        if (flags & O_TRUNC)
            size = 0;
    } else {
        if (!(flags & O_CREAT))
            return -LERR_NOENT;
        /* The failed walk left p9fid unallocated server-side, so reuse it to
         * walk the parent directory, then create the leaf (Tcreate leaves the
         * fid on the new file, open for writing). */
        if (ns_walk(parent, p9fid) != 0)
            return -LERR_NOENT;
        /* Honor the caller's mode (masked to permission bits) so an executable
         * created here, which the linker opens 0777, keeps its execute bits on
         * the host. Fall back to 0644 when the caller passed no mode. */
        uint32_t perm = (uint32_t)(mode & 0777);
        if (perm == 0)
            perm = 0644;
        if (p9_create(p9fid, leaf, perm) != 0) {
            p9_clunk(p9fid);
            return -LERR_ACCES;
        }
        size = 0;
    }

    struct ofile *of = ofile_alloc();
    if (!of) {
        p9_clunk(p9fid);
        return -LERR_NOMEM;
    }
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], FD_9P);
    fds[fd].is9w = 1;
    fds[fd].p9fid = p9fid;
    fds[fd].of = of;
    fds[fd].size = size;
    fds[fd].ino = ino;
    fds[fd].oflags = (int)(flags & (O_ACCMODE | O_APPEND));
    of->pos = (flags & O_APPEND) ? size : 0;
    gfidref[p9fid - LINUX_FID_BASE] = 1;
    return fd;
}

/* Create a directory on a writable host mount over 9P: walk its parent and
 * Tcreate the leaf with the directory bit. Returns 0 or a negative errno. */
static long
create_9p_dir(const char *key)
{
    char parent[RAMFS_NAME];
    size_t kl = strlen(key), cut = kl;
    while (cut > 0 && key[cut - 1] != '/')
        cut--;
    const char *leaf = key + cut;
    size_t plen = cut > 0 ? cut - 1 : 0;
    memcpy(parent, key, plen);
    parent[plen] = '\0';
    if (leaf[0] == '\0')
        return -LERR_EXIST;

    uint32_t fid = fid_alloc();
    if (fid == 0)
        return -LERR_NOMEM;
    if (ns_walk(parent, fid) != 0)
        return -LERR_NOENT;
    if (p9_create(fid, leaf, P9_DMDIR | 0755) != 0) {
        p9_clunk(fid);
        return -LERR_ACCES;
    }
    p9_clunk(fid);
    return 0;
}

/* Remove a file or empty directory on a writable host mount over 9P. The key is
 * already normalized and known to be under a writable mount. Tremove clunks the
 * fid on the server, so no clunk follows. A build removes its own intermediate
 * files (gyp drops each .o.d.raw after folding it into the .o.d), and those live
 * on the host mount, not the overlay, so the overlay remove path cannot reach
 * them. */
static long
remove_9p(const char *key)
{
    uint32_t fid = fid_alloc();
    if (fid == 0)
        return -LERR_NOMEM;
    if (ns_walk(key, fid) != 0)
        return -LERR_NOENT;
    if (p9_remove(fid) != 0)
        return -LERR_ACCES;
    return 0;
}

static long
do_open(const char *path, long flags, long mode)
{
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    if (!(flags & O_NOFOLLOW))
        follow_link_key(key, sizeof key);
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0;

    /* A /dev character device is served from the guest, never the overlay or a
     * mount, so it must be caught before either path. */
    int dk = dev_lookup(key);
    if (dk)
        return open_dev(dk, flags);
    int isdevdir = dev_is_dir(key);

    /* A write or create under a writable host mount goes to the host over 9P,
     * not to the in-RAM overlay, so build output lands on the host filesystem. */
    if ((writing || (flags & O_CREAT)) && under_rw_mount(key))
        return open_9p_write(key, flags, mode);

    int ri = ramfs_find(key);               /* overlay entry (file or dir) */
    int is_ovl_dir = (ri >= 0 && ramfs[ri].is_dir);

    /* Writing or creating a plain file lands in the writable overlay. */
    if (writing || (flags & O_CREAT)) {
        if (is_ovl_dir || (ri < 0 && ramfs_is_dir_key(key)))
            return -LERR_ISDIR;
        /* O_EXCL demands the create make a new file. When the name already
         * exists in the overlay, fail with EEXIST so a caller like mkstemp
         * retries with a fresh name instead of aliasing an existing entry. */
        if (ri >= 0 && (flags & O_CREAT) && (flags & O_EXCL))
            return -LERR_EXIST;
        if (ri < 0) {
            if (!(flags & O_CREAT))
                return -LERR_ROFS;          /* writing an existing RO bundle file */
            ri = ramfs_create(key, 0);
            if (ri < 0)
                return -LERR_NOMEM;
        }
        if (flags & O_TRUNC)
            ramfs[ri].len = 0;
        return open_ram(ri, flags);
    }

    /* Read-only: an overlay file shadows the bundle. */
    if (ri >= 0 && !ramfs[ri].is_dir)
        return open_ram(ri, flags);

    /* A directory, or a bundle/host file. Probe the 9P side. */
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    uint32_t p9fid = fid_alloc();
    if (p9fid == 0)
        return -LERR_NOMEM;

    int have9 = 0, isdir9 = 0;
    uint64_t size = 0, ino = p9fid;
    if (ns_walk(key, p9fid) == 0) {
        struct p9_stat st;
        if (p9_stat(p9fid, &st) == 0) {
            have9 = 1;
            isdir9 = (st.qid.type & P9_QTDIR) != 0;
            size = st.length;
            ino = st.qid.path;
        }
    }
    int isdir = is_ovl_dir || ramfs_is_dir_key(key) || (have9 && isdir9)
                || isdevdir;

    if (!isdir && !have9) {
        p9_clunk(p9fid);
        return -LERR_NOENT;
    }
    if ((flags & O_DIRECTORY) && !isdir) {
        p9_clunk(p9fid);
        return -LERR_NOTDIR;
    }

    struct ofile *of = ofile_alloc();
    if (!of) {
        p9_clunk(p9fid);
        return -LERR_NOMEM;
    }
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], isdir ? FD_DIR : FD_9P);
    fds[fd].size = size;
    fds[fd].ino = ino;
    fds[fd].of = of;
    fds[fd].oflags = (int)(flags & (O_ACCMODE | O_APPEND));
    /* Keep a 9P fid open when there is a 9P file to read, or a 9P directory to
     * enumerate; an overlay-only directory needs none. */
    if (have9 && (!isdir || isdir9)) {
        if (p9_open(p9fid) != 0) {
            p9_clunk(p9fid);
            fds[fd].used = 0;
            ofile_unref(of);
            return -LERR_ACCES;
        }
        fds[fd].p9fid = p9fid;
        gfidref[p9fid - LINUX_FID_BASE] = 1;
    } else {
        p9_clunk(p9fid);
    }
    if (isdir)
        fd_set_dir(&fds[fd], key);
    return fd;
}

/* poll(2) event bits. POLLHUP, POLLERR, and POLLNVAL are output only: poll
 * reports them in revents whether or not the caller asked for them. */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* The readiness of one descriptor for poll. A read or write that would not
 * block counts as ready. The only descriptor that can block is a pipe: its
 * read end is readable once it holds data or every writer has closed (EOF, a
 * read that returns 0), and its write end is writable while the ring has room.
 * The console, ram files, 9P files, and directories never block, so they are
 * always ready for whichever direction the caller asked about. */
static short
poll_ready(struct lfd *f, short events)
{
    if (!f)
        return POLLNVAL;
    return f->ops->poll(f, events);
}

/* poll(fds, nfds, timeout_ms). Cooperative: scan the set, and if nothing is
 * ready yet drop the giant lock and yield so a writer or child can make a pipe
 * ready, then rescan. timeout 0 is a non-blocking single scan; a negative
 * timeout waits indefinitely; a positive timeout is bounded with the cycle
 * counter (about 1 GHz, the same rough clock the time syscalls use), since
 * there is no real timer. Returns the number of ready descriptors, 0 on
 * timeout. */
static long
do_poll(uint64_t ufds, uint64_t nfds, long timeout_ms)
{
    if (nfds > 1024)
        return -LERR_INVAL;             /* far past anything the guest needs */
    struct pollfd *pf = NULL;
    uint64_t sz = nfds * sizeof *pf;
    if (nfds) {
        pf = kmalloc(sz);
        if (!pf)
            return -LERR_NOMEM;
        if (copy_from_user(pf, ufds, sz) < 0) {
            kfree(pf);
            return -LERR_FAULT;
        }
    }

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = arch_cycles() + (uint64_t)timeout_ms * 1000000ULL;

    long ready;
    int interrupted = 0;
    for (;;) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            pf[i].revents = 0;
            if (pf[i].fd < 0)
                continue;               /* a negative fd is skipped, revents 0 */
            short re = poll_ready(fd_get(pf[i].fd), pf[i].events);
            if (re) {
                pf[i].revents = re;
                ready++;
            }
        }
        if (ready > 0 || timeout_ms == 0)
            break;
        if (timeout_ms > 0 && arch_cycles() >= deadline)
            break;                      /* timed out with nothing ready */
        lx_yield();                     /* let a peer make a descriptor ready */
        signal_killpoint();             /* a fatal signal unblocks and terminates */
        if (signal_deliverable()) {     /* a caught signal: run its handler */
            interrupted = 1;
            break;
        }
    }

    if (nfds) {
        copy_to_user(ufds, pf, sz);     /* write back revents */
        kfree(pf);
    }
    return interrupted ? -LERR_RESTARTSYS : ready;
}

/* ppoll(fds, nfds, tmo_p, sigmask, sigsetsize): poll with a timespec timeout and
 * an optional signal mask applied for the wait. AArch64 has no poll or pause
 * syscall, so musl routes both poll() and pause() (ppoll with no fds) through
 * here. The timeout is a timespec by pointer, or NULL to wait indefinitely. */
static long
do_ppoll(uint64_t ufds, uint64_t nfds, uint64_t tmo_ptr, uint64_t sigmask_ptr)
{
    long timeout_ms = -1;               /* NULL timespec: wait indefinitely */
    if (tmo_ptr) {
        uint64_t ts[2];                 /* tv_sec, tv_nsec */
        long e = copy_from_user(ts, tmo_ptr, sizeof ts);
        if (e < 0)
            return e;
        if (ts[1] >= 1000000000ULL)
            return -LERR_INVAL;
        /* Round the nanoseconds up to a whole millisecond so a sub-millisecond
         * timeout still polls at least once rather than returning at once. */
        timeout_ms = (long)(ts[0] * 1000ULL + (ts[1] + 999999ULL) / 1000000ULL);
    }

    /* Apply the caller's signal mask for the wait so a signal it unblocks can
     * interrupt the poll, then restore it before returning, so a delivered
     * handler runs under the thread's original mask. The atomic pselect-style
     * guarantee for a signal unblocked only by this temporary mask is not
     * modelled; the common callers (a NULL mask, as in pause and plain poll)
     * do not need it. */
    uint64_t saved = lproc_cur->sig_blocked;
    int swapped = 0;
    if (sigmask_ptr) {
        uint64_t m;
        long e = copy_from_user(&m, sigmask_ptr, sizeof m);
        if (e < 0)
            return e;
        lproc_cur->sig_blocked = m;
        swapped = 1;
    }

    long r = do_poll(ufds, nfds, timeout_ms);

    if (swapped)
        lproc_cur->sig_blocked = saved;
    return r;
}

/* epoll. The event bits below the output-only ones match the poll bits exactly
 * (EPOLLIN==POLLIN, EPOLLOUT==POLLOUT, ...), so epoll readiness reuses
 * poll_ready. EPOLLET (edge-triggered) is accepted but treated as level, which
 * is correct but may wake more often than a strict edge; EPOLLONESHOT disables
 * an entry after one report until a later EPOLL_CTL_MOD re-arms it. On x86-64
 * struct epoll_event is packed (12 bytes); on aarch64 it is naturally aligned
 * (16 bytes), so the layout is selected by arch. */
#define EPOLLONESHOT 0x40000000u
#define EPOLLET      0x80000000u
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 02000000          /* == O_CLOEXEC */

#if defined(__x86_64__)
struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));
#else
struct epoll_event {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
};
#endif

static long
do_epoll_create1(long flags)
{
    int ei = -1;
    for (int i = 0; i < EPOLL_MAX; i++)
        if (!epolls[i].used) {
            ei = i;
            break;
        }
    if (ei < 0)
        return -LERR_NOMEM;             /* EMFILE-ish: no instance free */
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    struct epoll *ep = &epolls[ei];
    memset(ep, 0, sizeof *ep);
    ep->used = 1;
    ep->nref = 1;
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], FD_EPOLL);
    fds[fd].epidx = ei;
    fds[fd].cloexec = (flags & EPOLL_CLOEXEC) ? 1 : 0;
    return fd;
}

/* Find the interest entry for a watched fd, or NULL. */
static struct epitem *
ep_find(struct epoll *ep, int fd)
{
    for (int i = 0; i < EPOLL_NINT; i++)
        if (ep->items[i].used && ep->items[i].fd == fd)
            return &ep->items[i];
    return NULL;
}

static long
do_epoll_ctl(long epfd, long op, long fd, uint64_t uev)
{
    struct lfd *ef = fd_get(epfd);
    if (!ef || ef->kind != FD_EPOLL)
        return -LERR_BADF;
    if (!fd_get(fd))
        return -LERR_BADF;              /* the watched fd must be open */
    if (fd == epfd)
        return -LERR_INVAL;             /* an epoll cannot watch itself here */
    struct epoll *ep = &epolls[ef->epidx];

    struct epoll_event ev;
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (!uev)
            return -LERR_FAULT;
        if (copy_from_user(&ev, uev, sizeof ev) < 0)
            return -LERR_FAULT;
    }

    struct epitem *it = ep_find(ep, (int)fd);
    switch (op) {
    case EPOLL_CTL_ADD:
        if (it)
            return -LERR_EXIST;
        for (int i = 0; i < EPOLL_NINT; i++)
            if (!ep->items[i].used) {
                ep->items[i].used = 1;
                ep->items[i].fd = (int)fd;
                ep->items[i].armed = 1;
                ep->items[i].events = ev.events;
                ep->items[i].data = ev.data;
                return 0;
            }
        return -LERR_NOMEM;             /* interest list full */
    case EPOLL_CTL_MOD:
        if (!it)
            return -LERR_NOENT;
        it->events = ev.events;
        it->data = ev.data;
        it->armed = 1;                  /* a MOD re-arms a ONESHOT entry */
        return 0;
    case EPOLL_CTL_DEL:
        if (!it)
            return -LERR_NOENT;
        it->used = 0;
        return 0;
    default:
        return -LERR_INVAL;
    }
}

/* One scan of an epoll's interest list into the caller's event array. Returns
 * the number of ready descriptors written (at most maxevents). A closed watched
 * fd reports EPOLLHUP|EPOLLERR, as Linux does, rather than being skipped. */
static int
ep_scan(struct epoll *ep, struct epoll_event *out, int maxevents)
{
    int n = 0;
    for (int i = 0; i < EPOLL_NINT && n < maxevents; i++) {
        struct epitem *it = &ep->items[i];
        if (!it->used || !it->armed)
            continue;
        struct lfd *f = fd_get(it->fd);
        short want = (short)(it->events & (POLLIN | POLLPRI | POLLOUT));
        short re;
        if (!f)
            re = POLLHUP | POLLERR;     /* the watched fd was closed */
        else
            re = poll_ready(f, want);
        if (!re)
            continue;
        out[n].events = (uint32_t)(re & 0xffff);
        out[n].data = it->data;
        n++;
        if (it->events & EPOLLONESHOT)
            it->armed = 0;              /* disabled until a later MOD re-arms */
    }
    return n;
}

static long
do_epoll_wait(long epfd, uint64_t uevents, long maxevents, long timeout_ms)
{
    struct lfd *ef = fd_get(epfd);
    if (!ef || ef->kind != FD_EPOLL)
        return -LERR_BADF;
    if (maxevents <= 0)
        return -LERR_INVAL;
    if (maxevents > EPOLL_NINT)
        maxevents = EPOLL_NINT;
    struct epoll *ep = &epolls[ef->epidx];

    /* The output buffer is kmalloc'd, not on the stack: this runs atop the giant
     * -O0 syscall-dispatch frame, and a kilobytes-wide array here could overflow
     * the syscall kstack under a preemption interrupt. */
    uint64_t sz = (uint64_t)maxevents * sizeof(struct epoll_event);
    struct epoll_event *evs = kmalloc(sz);
    if (!evs)
        return -LERR_NOMEM;
    memset(evs, 0, sz);                 /* zero the aarch64 pad as the kernel does */

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = arch_cycles() + (uint64_t)timeout_ms * 1000000ULL;

    int interrupted = 0;
    int n;
    for (;;) {
        n = ep_scan(ep, evs, (int)maxevents);
        if (n > 0 || timeout_ms == 0)
            break;
        if (timeout_ms > 0 && arch_cycles() >= deadline)
            break;                      /* timed out with nothing ready */
        lx_yield();                     /* let a peer make a descriptor ready */
        signal_killpoint();
        if (signal_deliverable()) {
            interrupted = 1;
            break;
        }
    }
    long rc = n;
    if (interrupted)
        rc = -LERR_RESTARTSYS;
    else if (n > 0 && copy_to_user(uevents, evs, (uint64_t)n * sizeof evs[0]) < 0)
        rc = -LERR_FAULT;
    kfree(evs);
    return rc;
}

/* Allocate an eventfd with an initial counter value. flags take EFD_SEMAPHORE,
 * EFD_NONBLOCK and EFD_CLOEXEC. */
static long
do_eventfd(unsigned initval, int flags)
{
    int ei = -1;
    for (int i = 0; i < EVENTFD_MAX; i++)
        if (!eventfds[i].used) {
            ei = i;
            break;
        }
    if (ei < 0)
        return -LERR_NOMEM;
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    memset(&eventfds[ei], 0, sizeof eventfds[ei]);
    eventfds[ei].used = 1;
    eventfds[ei].nref = 1;
    eventfds[ei].semaphore = (flags & EFD_SEMAPHORE) ? 1 : 0;
    eventfds[ei].count = initval;
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fd_set_kind(&fds[fd], FD_EVENTFD);
    fds[fd].efdidx = ei;
    fds[fd].oflags = O_RDWR;
    if (flags & EFD_NONBLOCK)
        fds[fd].oflags |= O_NONBLOCK;
    if (flags & EFD_CLOEXEC)
        fds[fd].cloexec = 1;
    return fd;
}

/* capget: no capability model, so report an empty set, which is the honest
 * answer for an unprivileged process and what node/libuv expect when they probe
 * during init. The data is two __user_cap_data_struct words addressed by a v3
 * header (effective/permitted/inheritable); zero them. A helper, not an inline
 * case, so its buffer stays off the one -O0 syscall-dispatch stack frame. */
static long
do_capget(uint64_t udata)
{
    uint32_t zero[6] = {0};

    if (udata && copy_to_user(udata, zero, sizeof zero) < 0)
        return -LERR_FAULT;
    return 0;
}

/* clock_getres: report a 1 ns resolution for every clock. Callers use it only to
 * size sleeps and timers; the guest clock is coarser but nothing needs the true
 * grain. A helper for the same stack reason as do_capget. */
static long
do_clock_getres(uint64_t ures)
{
    struct { int64_t sec; int64_t nsec; } ts = { 0, 1 };

    if (ures && copy_to_user(ures, &ts, sizeof ts) < 0)
        return -LERR_FAULT;
    return 0;
}

/* eventfd read: deliver the counter (the whole value, or 1 in semaphore mode)
 * into an 8-byte buffer, blocking while it is zero unless the fd is non-blocking. */
static long
eventfd_read(struct lfd *f, void *buf, unsigned long count)
{
    struct leventfd *e = &eventfds[f->efdidx];

    if (count < sizeof(uint64_t))
        return -LERR_INVAL;
    while (e->count == 0) {
        if (f->oflags & O_NONBLOCK)
            return -LERR_AGAIN;
        lx_yield();                     /* empty: let a writer post to it */
        signal_killpoint();
        if (signal_deliverable())
            return -LERR_RESTARTSYS;
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count = e->semaphore ? e->count - 1 : 0;
    memcpy(buf, &v, sizeof v);
    return (long)sizeof v;
}

/* eventfd write: add an 8-byte value to the counter, blocking (or EAGAIN when
 * non-blocking) if it would exceed the Linux cap. A write of ULLONG_MAX is the
 * documented error. */
static long
eventfd_write(struct lfd *f, const void *buf, unsigned long count)
{
    struct leventfd *e = &eventfds[f->efdidx];
    uint64_t add;

    if (count < sizeof(uint64_t))
        return -LERR_INVAL;
    memcpy(&add, buf, sizeof add);
    if (add == 0xffffffffffffffffULL)
        return -LERR_INVAL;
    while (e->count + add > EVENTFD_MAXVAL) {
        if (f->oflags & O_NONBLOCK)
            return -LERR_AGAIN;
        lx_yield();                     /* full: let a reader drain it */
        signal_killpoint();
        if (signal_deliverable())
            return -LERR_RESTARTSYS;
    }
    e->count += add;
    return (long)sizeof add;
}

/* ------------------------------------------------------------------------- *
 * Backend ops. Each descriptor kind provides read/write/poll and, when it has
 * backing state, ref/close. fd_set_kind binds a kind to its table. The generic
 * do_read/do_write/do_close/poll_ready/clone_fd/fork paths dispatch here.
 * ------------------------------------------------------------------------- */

/* A kind with no real read or write side rejects it rather than crashing. */
static long rd_einval(struct lfd *f, void *b, unsigned long n)
{ (void)f; (void)b; (void)n; return -LERR_INVAL; }
static long wr_ebadf(struct lfd *f, const void *b, unsigned long n)
{ (void)f; (void)b; (void)n; return -LERR_BADF; }

/* Files and devices never block a poll: they are always reported ready. */
static short dflt_poll(struct lfd *f, short ev)
{
    (void)f;
    short r = 0;
    if (ev & POLLIN)  r |= POLLIN;
    if (ev & POLLOUT) r |= POLLOUT;
    return r;
}

/* ------------------------------------------------------------------------- *
 * Console tty discipline. On an interactive guest the host puts its terminal
 * in raw mode and feeds keystrokes into the UART; this gives the init program a
 * line discipline over that byte stream: canonical-mode line editing with echo
 * by default, raw mode when a program (a shell's line editor) clears ICANON. A
 * non-interactive console still reads as EOF, exactly as before.
 *
 * There is no terminal translation: bytes pass through untouched, so the guest
 * emits escape sequences for the host's own terminal (its TERM is passed in).
 * A host terminal that is not xterm-compatible may therefore mis-render some
 * sequences; a translating line discipline is future work.
 * ------------------------------------------------------------------------- */

/* The kernel termios ABI, as TCGETS/TCSETS exchange it. */
struct linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct linux_winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* c_iflag */
#define T_ICRNL     0x0100
#define T_IXON      0x0400
/* c_oflag */
#define T_OPOST     0x0001
#define T_ONLCR     0x0004
/* c_cflag */
#define T_B38400    0x000f
#define T_CS8       0x0030
#define T_CREAD     0x0080
/* c_lflag */
#define T_ISIG      0x0001
#define T_ICANON    0x0002
#define T_ECHO      0x0008
#define T_ECHOE     0x0010
#define T_ECHOK     0x0020
#define T_IEXTEN    0x8000
/* c_cc indices */
#define T_VINTR     0
#define T_VQUIT     1
#define T_VERASE    2
#define T_VKILL     3
#define T_VEOF      4
#define T_VMIN      6

/* ioctl requests (same values on the architectures this guest targets). */
#define T_TCGETS        0x5401
#define T_TCSETS        0x5402
#define T_TCSETSW       0x5403
#define T_TCSETSF       0x5404
#define T_TIOCGPGRP     0x540F
#define T_TIOCSPGRP     0x5410
#define T_TIOCGWINSZ    0x5413

static int con_interactive;             /* mirror of the boot block flag */
static struct linux_termios con_tio;    /* current console line settings */
static struct linux_winsize con_ws = { 24, 80, 0, 0 };

/* Parked console readers, mirroring net.c's waiter model: the event hook wakes
 * them on any device event and each rechecks the UART, reblocking if still dry.
 * No lost wakeup: a reader sets its flag before testing the UART, so input that
 * races in is caught by the next hook, which task_wake records as pending. */
static volatile int con_waiting[TASK_MAX];

static void
con_wake_waiters(void)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (con_waiting[i])
            task_wake(i);
}

/* Default the console to a cooked tty: CR to NL on input, output post-processed
 * to CR+LF, canonical line editing with echo. Registers the wake hook once. */
static void
con_tty_init(void)
{
    con_interactive = boot_console_interactive();
    con_tio.c_iflag = T_ICRNL | T_IXON;
    con_tio.c_oflag = T_OPOST | T_ONLCR;
    con_tio.c_cflag = T_B38400 | T_CS8 | T_CREAD;
    con_tio.c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_IEXTEN;
    con_tio.c_cc[T_VINTR] = 3;          /* ^C */
    con_tio.c_cc[T_VQUIT] = 28;         /* ^\ */
    con_tio.c_cc[T_VERASE] = 127;       /* DEL */
    con_tio.c_cc[T_VKILL] = 21;         /* ^U */
    con_tio.c_cc[T_VEOF] = 4;           /* ^D */
    con_tio.c_cc[T_VMIN] = 1;
    if (con_interactive)
        task_add_event_hook(con_wake_waiters);
}

/* Block until the UART has a byte and return it (0-255), or -1 when a signal
 * becomes deliverable while parked so the caller can report EINTR. */
static int
con_getc_blocking(void)
{
    int me = task_self();

    __atomic_store_n(&con_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        int c = con_raw_getc();
        if (c >= 0) {
            __atomic_store_n(&con_waiting[me], 0, __ATOMIC_SEQ_CST);
            return c;
        }
        if (signal_deliverable()) {
            __atomic_store_n(&con_waiting[me], 0, __ATOMIC_SEQ_CST);
            return -1;
        }
        task_block();
    }
}

static void
con_echo(char c)
{
    if (con_tio.c_lflag & T_ECHO)
        con_putc(c);
}

/* Raw-mode read: return as soon as one byte is available (the VMIN=1 case),
 * draining whatever else is already buffered, up to n. */
static long
con_read_raw(char *buf, unsigned long n)
{
    unsigned long got = 0;
    int c;

    if (n == 0)
        return 0;
    c = con_getc_blocking();
    if (c < 0)
        return -LERR_RESTARTSYS;
    buf[got++] = (char)c;
    con_echo((char)c);
    while (got < n) {
        c = con_raw_getc();
        if (c < 0)
            break;
        buf[got++] = (char)c;
        con_echo((char)c);
    }
    return (long)got;
}

/* Canonical read: gather one line, echoing and honouring erase, kill, and EOF,
 * and return it (newline included) once Return is pressed. ^D returns the
 * partial line, or 0 (EOF) at the start of a line. */
static long
con_read_canon(char *buf, unsigned long n)
{
    unsigned long got = 0;

    for (;;) {
        int ci = con_getc_blocking();
        char c;
        if (ci < 0)
            return got ? (long)got : -LERR_RESTARTSYS;
        c = (char)ci;

        if (c == '\r' && (con_tio.c_iflag & T_ICRNL))
            c = '\n';
        if (c == '\n') {
            con_putc('\n');
            if (got < n)
                buf[got++] = '\n';
            return (long)got;
        }
        if ((unsigned char)c == con_tio.c_cc[T_VERASE] || c == 8) {
            if (got > 0) {
                got--;
                if (con_tio.c_lflag & T_ECHOE)
                    con_puts("\b \b");
            }
            continue;
        }
        if ((unsigned char)c == con_tio.c_cc[T_VKILL]) {
            while (got > 0) {
                got--;
                if (con_tio.c_lflag & T_ECHOK)
                    con_puts("\b \b");
            }
            continue;
        }
        if ((unsigned char)c == con_tio.c_cc[T_VEOF])
            return (long)got;
        if (got < n) {
            buf[got++] = c;
            con_echo(c);
        }
    }
}

/* Service an ioctl on the console. Returns 0 (or a negative error) when the
 * request is a tty request, or -LERR_NOTTY when it is not, so a non-interactive
 * console and unknown requests behave as a non-tty. */
static long
con_ioctl(unsigned long req, uint64_t arg)
{
    if (!con_interactive)
        return -LERR_NOTTY;
    switch (req) {
    case T_TCGETS:
        return copy_to_user(arg, &con_tio, sizeof con_tio) < 0
            ? -LERR_FAULT : 0;
    case T_TCSETS:
    case T_TCSETSW:
    case T_TCSETSF:
        return copy_from_user(&con_tio, arg, sizeof con_tio) < 0
            ? -LERR_FAULT : 0;
    case T_TIOCGWINSZ:
        return copy_to_user(arg, &con_ws, sizeof con_ws) < 0
            ? -LERR_FAULT : 0;
    case T_TIOCGPGRP:
    case T_TIOCSPGRP:
        return 0;                   /* no job control: accept and ignore */
    default:
        return -LERR_NOTTY;
    }
}

/* Console. */
static long con_read(struct lfd *f, void *buf, unsigned long n)
{
    (void)f;
    if (!con_interactive)
        return 0;                   /* no terminal attached: EOF, as before */
    if (con_tio.c_lflag & T_ICANON)
        return con_read_canon(buf, n);
    return con_read_raw(buf, n);
}
static long con_write(struct lfd *f, const void *buf, unsigned long n)
{
    (void)f;
    const char *b = buf;
    for (unsigned long i = 0; i < n; i++)
        con_putc(b[i]);
    return (long)n;
}

/* Pipe. */
static long pipe_fd_read(struct lfd *f, void *buf, unsigned long count)
{
    if (f->wr)
        return -LERR_BADF;
    struct lpipe *p = &pipes[f->pidx];
    while (p->count == 0 && p->writers > 0) {
        lx_yield();
        signal_killpoint();
        if (signal_deliverable())
            return -LERR_RESTARTSYS;
    }
    return pipe_read(p, buf, count);
}
static long pipe_fd_write(struct lfd *f, const void *buf, unsigned long len)
{
    if (!f->wr)
        return -LERR_BADF;
    struct lpipe *p = &pipes[f->pidx];
    unsigned long done = 0;
    while (done < len) {
        if (p->readers == 0) {
            if (done)
                return (long)done;
            if (lproc_cur->sh->sigact[SIGPIPE - 1].handler == SIG_DFL) {
                lproc_cur->sh->exit_sig = SIGPIPE;
                do_exit(0, 1);
            }
            return -LERR_PIPE;
        }
        long n = pipe_write(p, (const uint8_t *)buf + done, len - done);
        done += (unsigned long)n;
        if (done < len)
            lx_yield();
        signal_killpoint();
    }
    return (long)done;
}
static short pipe_poll(struct lfd *f, short ev)
{
    struct lpipe *p = &pipes[f->pidx];
    short r = 0;
    if (f->wr) {
        if (p->readers == 0)
            r |= POLLERR;
        if (p->count < PIPE_CAP)
            r |= (short)(ev & POLLOUT);
    } else {
        if (p->count > 0)
            r |= (short)(ev & POLLIN);
        if (p->writers == 0)
            r |= POLLHUP;
    }
    return r;
}
static void pipe_ref(struct lfd *f)
{
    struct lpipe *p = &pipes[f->pidx];
    if (f->wr) p->writers++; else p->readers++;
}
static void pipe_close(struct lfd *f)
{
    struct lpipe *p = &pipes[f->pidx];
    if (f->wr && p->writers > 0) p->writers--;
    else if (!f->wr && p->readers > 0) p->readers--;
    if (p->readers == 0 && p->writers == 0) p->used = 0;
}

/* Overlay (ramfs) file. */
static long ram_read(struct lfd *f, void *buf, unsigned long count)
{
    struct ramfile *rf = &ramfs[f->ridx];
    if (f->of->pos >= rf->len)
        return 0;
    uint32_t avail = rf->len - (uint32_t)f->of->pos;
    uint32_t n = count < avail ? (uint32_t)count : avail;
    memcpy(buf, rf->data + f->of->pos, n);
    f->of->pos += n;
    return n;
}
static long ram_write(struct lfd *f, const void *buf, unsigned long len)
{
    struct ramfile *rf = &ramfs[f->ridx];
    uint64_t end = f->of->pos + len;
    if (end > RAMFS_FILE_MAX || ramfs_grow(rf, (uint32_t)end) < 0)
        return -LERR_NOMEM;
    memcpy(rf->data + f->of->pos, buf, len);
    f->of->pos += len;
    if (f->of->pos > rf->len)
        rf->len = (uint32_t)f->of->pos;
    rf->mtime = fs_stamp();
    return (long)len;
}
static void ram_ref(struct lfd *f)
{
    ramfs[f->ridx].nref++;
    if (f->of) f->of->nref++;
}
static void ram_close(struct lfd *f)
{
    ramfs_unref(f->ridx);
    ofile_unref(f->of);
}

/* 9P-backed file (also the ref/close for a directory, which shares the fid and
 * open-file description). */
static long p9_fd_read(struct lfd *f, void *buf, unsigned long count)
{
    long n = p9_read(f->p9fid, f->of->pos, buf, (uint32_t)count);
    if (n < 0)
        return -LERR_INVAL;
    f->of->pos += (uint64_t)n;
    return n;
}
static long p9_fd_write(struct lfd *f, const void *buf, unsigned long len)
{
    if (!f->is9w)
        return -LERR_BADF;              /* a read-only bundle file */
    unsigned long done = 0;
    while (done < len) {
        long n = p9_write(f->p9fid, f->of->pos + done,
                          (const char *)buf + done, (uint32_t)(len - done));
        if (n <= 0)
            break;
        done += (unsigned long)n;
    }
    if (done == 0 && len > 0)
        return -LERR_ROFS;
    f->of->pos += done;
    return (long)done;
}
static void p9_ref(struct lfd *f)
{
    if (f->p9fid) gfidref[f->p9fid - LINUX_FID_BASE]++;
    if (f->of) f->of->nref++;
}
static void p9_close(struct lfd *f)
{
    if (f->p9fid) {
        uint32_t slot = f->p9fid - LINUX_FID_BASE;
        if (gfidref[slot] && --gfidref[slot] == 0)
            p9_clunk(f->p9fid);
    }
    ofile_unref(f->of);
}

/* Synthetic /dev character device. */
static long dev_fd_read(struct lfd *f, void *buf, unsigned long n)
{ return dev_read(f->devno, buf, n); }
static long dev_fd_write(struct lfd *f, const void *buf, unsigned long n)
{ return dev_write(f->devno, (const char *)buf, n); }

/* epoll instance. */
static void epoll_ref(struct lfd *f) { epolls[f->epidx].nref++; }
static void epoll_close(struct lfd *f)
{
    struct epoll *ep = &epolls[f->epidx];
    if (ep->nref > 0 && --ep->nref == 0)
        ep->used = 0;
}

/* eventfd counter. */
static short eventfd_poll(struct lfd *f, short ev)
{
    struct leventfd *e = &eventfds[f->efdidx];
    short r = 0;
    if (e->count > 0) r |= (short)(ev & POLLIN);
    if (e->count < EVENTFD_MAXVAL) r |= (short)(ev & POLLOUT);
    return r;
}
static void eventfd_ref(struct lfd *f) { eventfds[f->efdidx].nref++; }
static void eventfd_close(struct lfd *f)
{
    struct leventfd *e = &eventfds[f->efdidx];
    if (e->nref > 0 && --e->nref == 0)
        e->used = 0;
}

/* AF_UNIX socket. A connected endpoint reads from its own receive ring and
 * writes into the peer's; a listener holds an accept queue. */
static long
sock_read(struct lfd *f, void *buf, unsigned long count)
{
    struct usock *s = &usocks[f->sidx];
    if (s->listening)
        return -LERR_INVAL;
    int nb = (f->oflags & O_NONBLOCK) != 0;
    while (s->count == 0) {
        if (s->peer < 0 || s->peer_closed || s->shut_rd)
            return 0;                   /* EOF */
        if (nb)
            return -LERR_AGAIN;
        lx_yield();
        signal_killpoint();
        if (signal_deliverable())
            return -LERR_RESTARTSYS;
    }
    uint8_t *out = buf;
    unsigned long n = 0;
    while (n < count && s->count > 0) {
        out[n++] = s->buf[s->head];
        s->head = (s->head + 1) % USOCK_BUF;
        s->count--;
    }
    return (long)n;
}
static long
sock_write(struct lfd *f, const void *buf, unsigned long count)
{
    struct usock *s = &usocks[f->sidx];
    if (s->listening)
        return -LERR_INVAL;
    int nb = (f->oflags & O_NONBLOCK) != 0;
    const uint8_t *in = buf;
    unsigned long done = 0;
    while (done < count) {
        if (s->peer < 0 || s->peer_closed || s->shut_wr) {
            if (done)
                return (long)done;
            if (lproc_cur->sh->sigact[SIGPIPE - 1].handler == SIG_DFL) {
                lproc_cur->sh->exit_sig = SIGPIPE;
                do_exit(0, 1);
            }
            return -LERR_PIPE;
        }
        struct usock *peer = &usocks[s->peer];
        while (done < count && peer->count < USOCK_BUF) {
            uint32_t tail = (peer->head + peer->count) % USOCK_BUF;
            peer->buf[tail] = in[done++];
            peer->count++;
        }
        if (done < count) {             /* peer buffer full */
            if (nb)
                return done ? (long)done : -LERR_AGAIN;
            lx_yield();
            signal_killpoint();
            if (signal_deliverable())
                return done ? (long)done : -LERR_RESTARTSYS;
        }
    }
    return (long)done;
}
static short
sock_poll(struct lfd *f, short ev)
{
    struct usock *s = &usocks[f->sidx];
    short r = 0;
    if (s->listening) {
        if (s->acceptq_n > 0)
            r |= (short)(ev & POLLIN);
        return r;
    }
    if (s->count > 0)
        r |= (short)(ev & POLLIN);
    if (s->peer < 0 || s->peer_closed) {
        r |= POLLHUP;                   /* readable: a read returns EOF */
        r |= (short)(ev & POLLIN);
    } else if (usocks[s->peer].count < USOCK_BUF) {
        r |= (short)(ev & POLLOUT);
    }
    return r;
}
static void sock_ref(struct lfd *f) { usocks[f->sidx].nref++; }
static void
sock_close(struct lfd *f)
{
    struct usock *s = &usocks[f->sidx];
    if (s->nref > 0)
        s->nref--;
    if (s->nref > 0)
        return;                         /* other descriptors still hold it */
    if (s->listening) {                 /* drop connections queued but unaccepted */
        for (int k = 0; k < s->acceptq_n; k++) {
            int qi = s->acceptq[(s->acceptq_head + k) % USOCK_BACKLOG];
            struct usock *q = &usocks[qi];
            if (q->peer >= 0) {
                usocks[q->peer].peer_closed = 1;
                usocks[q->peer].peer = -1;
            }
            q->used = 0;
        }
    }
    if (s->peer >= 0) {                 /* tell the peer we are gone (its read EOFs) */
        usocks[s->peer].peer_closed = 1;
        usocks[s->peer].peer = -1;
    }
    s->used = 0;
}

static const struct fd_ops con_ops = {
    "con", con_read, con_write, dflt_poll, NULL, NULL,
};
static const struct fd_ops pipe_ops = {
    "pipe", pipe_fd_read, pipe_fd_write, pipe_poll, pipe_ref, pipe_close,
};
static const struct fd_ops ram_ops = {
    "ram", ram_read, ram_write, dflt_poll, ram_ref, ram_close,
};
static const struct fd_ops p9_ops = {
    "9p", p9_fd_read, p9_fd_write, dflt_poll, p9_ref, p9_close,
};
static const struct fd_ops dir_ops = {
    "dir", rd_einval, wr_ebadf, dflt_poll, p9_ref, p9_close,
};
static const struct fd_ops dev_ops = {
    "dev", dev_fd_read, dev_fd_write, dflt_poll, NULL, NULL,
};
static const struct fd_ops epoll_ops = {
    "epoll", rd_einval, wr_ebadf, dflt_poll, epoll_ref, epoll_close,
};
static const struct fd_ops eventfd_ops = {
    "eventfd", eventfd_read, eventfd_write, eventfd_poll, eventfd_ref, eventfd_close,
};
static const struct fd_ops sock_ops = {
    "sock", sock_read, sock_write, sock_poll, sock_ref, sock_close,
};

static void
fd_set_kind(struct lfd *f, enum fd_kind k)
{
    f->kind = k;
    switch (k) {
    case FD_CON:     f->ops = &con_ops;     break;
    case FD_PIPE:    f->ops = &pipe_ops;    break;
    case FD_RAM:     f->ops = &ram_ops;     break;
    case FD_9P:      f->ops = &p9_ops;      break;
    case FD_DIR:     f->ops = &dir_ops;     break;
    case FD_DEV:     f->ops = &dev_ops;     break;
    case FD_EPOLL:   f->ops = &epoll_ops;   break;
    case FD_EVENTFD: f->ops = &eventfd_ops; break;
    case FD_SOCK:    f->ops = &sock_ops;    break;
    default:         f->ops = NULL;         break;
    }
}

/* ---- AF_UNIX socket syscalls ---- */

static int
usock_alloc(void)
{
    for (int i = 0; i < USOCK_MAX; i++)
        if (!usocks[i].used)
            return i;
    return -1;
}

/* Parse a user sockaddr_un into a path key (its sun_path, NUL-terminated). */
static long
read_sun(uint64_t uaddr, uint64_t addrlen, char *out, size_t outsz)
{
    if (!uaddr || addrlen < 2)
        return -LERR_INVAL;
    uint8_t tmp[2 + USOCK_PATHMAX];
    uint64_t n = addrlen > sizeof tmp ? sizeof tmp : addrlen;
    if (copy_from_user(tmp, uaddr, n) < 0)
        return -LERR_FAULT;
    uint16_t fam = (uint16_t)(tmp[0] | (tmp[1] << 8));
    if (fam != AF_UNIX)
        return -LERR_INVAL;
    size_t pl = (size_t)n - 2;
    size_t i = 0;
    for (; i < pl && i < outsz - 1 && tmp[2 + i]; i++)
        out[i] = (char)tmp[2 + i];
    out[i] = '\0';
    return 0;
}

/* Fill a user sockaddr_un from a path and update its socklen_t in/out length. */
static long
write_sun(uint64_t uaddr, uint64_t ulenptr, const char *path)
{
    if (!uaddr || !ulenptr)
        return 0;                       /* the caller does not want the address */
    uint8_t tmp[2 + USOCK_PATHMAX];
    memset(tmp, 0, sizeof tmp);
    tmp[0] = AF_UNIX & 0xff;
    tmp[1] = (AF_UNIX >> 8) & 0xff;
    uint32_t pl = 0;
    if (path)
        while (path[pl] && pl < USOCK_PATHMAX - 1) {
            tmp[2 + pl] = (uint8_t)path[pl];
            pl++;
        }
    uint32_t full = 2 + pl + (pl ? 1 : 0);      /* include the NUL for a name */
    uint32_t want;
    if (copy_from_user(&want, ulenptr, sizeof want) < 0)
        return -LERR_FAULT;
    uint32_t n = want < full ? want : full;
    if (n && copy_to_user(uaddr, tmp, n) < 0)
        return -LERR_FAULT;
    if (copy_to_user(ulenptr, &full, sizeof full) < 0)
        return -LERR_FAULT;
    return 0;
}

static long
do_socket(int domain, int type, int protocol)
{
    if (domain != AF_UNIX)
        return -LERR_NOSYS;             /* only AF_UNIX; caller falls back */
    if ((type & SOCK_TYPE_MASK) != SOCK_STREAM)
        return -LERR_NOSYS;
    (void)protocol;
    int si = usock_alloc();
    if (si < 0)
        return -LERR_NOMEM;
    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    struct usock *s = &usocks[si];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->nref = 1;
    s->peer = -1;
    struct lfd *f = &fds[fd];
    memset(f, 0, sizeof *f);
    f->used = 1;
    fd_set_kind(f, FD_SOCK);
    f->sidx = si;
    f->oflags = O_RDWR;
    if (type & SOCK_NONBLOCK) f->oflags |= O_NONBLOCK;
    if (type & SOCK_CLOEXEC)  f->cloexec = 1;
    return fd;
}

static long
do_bind(long fd, uint64_t uaddr, uint64_t addrlen)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (s->bound)
        return -LERR_INVAL;
    char path[USOCK_PATHMAX];
    long e = read_sun(uaddr, addrlen, path, sizeof path);
    if (e < 0)
        return e;
    for (int i = 0; i < USOCK_MAX; i++)
        if (usocks[i].used && usocks[i].bound && strcmp(usocks[i].path, path) == 0)
            return -LERR_ADDRINUSE;
    size_t n = 0;
    while (path[n] && n < USOCK_PATHMAX - 1) { s->path[n] = path[n]; n++; }
    s->path[n] = '\0';
    s->bound = 1;
    return 0;
}

/* Index of the bound AF_UNIX socket whose path normalizes to `key`, or -1.
 * bind records a path but creates no file, so this lets stat report S_IFSOCK at
 * that name (what `test -S` and a readiness poll look for) instead of ENOENT. */
static int
usock_bound_key(const char *key)
{
    for (int i = 0; i < USOCK_MAX; i++) {
        if (!usocks[i].used || !usocks[i].bound)
            continue;
        char k[RAMFS_NAME];
        norm_key(usocks[i].path, k, sizeof k);
        if (strcmp(k, key) == 0)
            return i;
    }
    return -1;
}

static long
do_listen(long fd, int backlog)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (!s->bound)
        return -LERR_INVAL;
    s->listening = 1;
    s->backlog = backlog < 1 ? 1
               : (backlog > USOCK_BACKLOG ? USOCK_BACKLOG : backlog);
    return 0;
}

static long
do_connect(long fd, uint64_t uaddr, uint64_t addrlen)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (s->listening)
        return -LERR_INVAL;
    if (s->peer >= 0)
        return -LERR_ISCONN;
    char path[USOCK_PATHMAX];
    long e = read_sun(uaddr, addrlen, path, sizeof path);
    if (e < 0)
        return e;
    int li = -1;
    for (int i = 0; i < USOCK_MAX; i++)
        if (usocks[i].used && usocks[i].listening && usocks[i].bound &&
            strcmp(usocks[i].path, path) == 0) {
            li = i;
            break;
        }
    if (li < 0)
        return -LERR_CONNREFUSED;
    struct usock *L = &usocks[li];
    if (L->acceptq_n >= L->backlog)
        return -LERR_CONNREFUSED;
    int sv = usock_alloc();
    if (sv < 0)
        return -LERR_CONNREFUSED;
    struct usock *S = &usocks[sv];
    memset(S, 0, sizeof *S);
    S->used = 1;
    S->nref = 0;                        /* held by the accept queue until accept */
    S->peer = f->sidx;
    size_t n = 0;                       /* the accepted side's local name */
    while (L->path[n] && n < USOCK_PATHMAX - 1) { S->path[n] = L->path[n]; n++; }
    S->path[n] = '\0';
    s->peer = sv;
    L->acceptq[(L->acceptq_head + L->acceptq_n) % USOCK_BACKLOG] = sv;
    L->acceptq_n++;
    return 0;
}

static long
do_accept(long fd, uint64_t uaddr, uint64_t uaddrlen, int flags)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (!s->listening)
        return -LERR_INVAL;
    int nb = (f->oflags & O_NONBLOCK) || (flags & SOCK_NONBLOCK);
    while (s->acceptq_n == 0) {
        if (nb)
            return -LERR_AGAIN;
        lx_yield();
        signal_killpoint();
        if (signal_deliverable())
            return -LERR_RESTARTSYS;
    }
    int sv = s->acceptq[s->acceptq_head];
    s->acceptq_head = (s->acceptq_head + 1) % USOCK_BACKLOG;
    s->acceptq_n--;
    int nfd = fd_alloc();
    if (nfd < 0) {                      /* no fd: drop the connection */
        struct usock *S = &usocks[sv];
        if (S->peer >= 0) {
            usocks[S->peer].peer_closed = 1;
            usocks[S->peer].peer = -1;
        }
        S->used = 0;
        return -LERR_NOMEM;
    }
    usocks[sv].nref = 1;
    struct lfd *nf = &fds[nfd];
    memset(nf, 0, sizeof *nf);
    nf->used = 1;
    fd_set_kind(nf, FD_SOCK);
    nf->sidx = sv;
    nf->oflags = O_RDWR;
    if (flags & SOCK_NONBLOCK) nf->oflags |= O_NONBLOCK;
    if (flags & SOCK_CLOEXEC)  nf->cloexec = 1;
    write_sun(uaddr, uaddrlen, "");     /* the connecting peer is unnamed */
    return nfd;
}

static long
do_getsockname(long fd, uint64_t uaddr, uint64_t ulen)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    return write_sun(uaddr, ulen, usocks[f->sidx].path);
}

static long
do_getpeername(long fd, uint64_t uaddr, uint64_t ulen)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (s->peer < 0)
        return -LERR_NOTCONN;
    return write_sun(uaddr, ulen, usocks[s->peer].path);
}

static long
do_shutdown(long fd, int how)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    struct usock *s = &usocks[f->sidx];
    if (how == SHUT_RD || how == SHUT_RDWR)
        s->shut_rd = 1;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        s->shut_wr = 1;
        if (s->peer >= 0)
            usocks[s->peer].peer_closed = 1;
    }
    return 0;
}

static long
do_setsockopt(long fd, uint64_t optval, uint64_t optlen)
{
    struct lfd *f = fd_get(fd);
    (void)optval; (void)optlen;
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    return 0;                           /* accept and ignore every option */
}

static long
do_getsockopt(long fd, uint64_t optval, uint64_t optlenptr)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    if (f->kind != FD_SOCK)
        return -LERR_NOTSOCK;
    int val = 0;                        /* SO_ERROR and all others report 0 */
    uint32_t olen = 0;
    if (optlenptr && copy_from_user(&olen, optlenptr, sizeof olen) < 0)
        return -LERR_FAULT;
    uint32_t n = olen < sizeof val ? olen : sizeof val;
    if (optval && n && copy_to_user(optval, &val, n) < 0)
        return -LERR_FAULT;
    uint32_t full = sizeof val;
    if (optlenptr && copy_to_user(optlenptr, &full, sizeof full) < 0)
        return -LERR_FAULT;
    return 0;
}

static long
do_read(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (count && !user_ok((uint64_t)(uintptr_t)buf, count))
        return -LERR_FAULT;
    return f->ops->read(f, buf, count);
}

/* Positioned read into a kernel-accessible buffer, on an already validated
 * non-pipe/non-console fd. do_pread wraps it with the fd and user-buffer checks;
 * splice reuses it for a seekable end with an explicit offset. */
static long
pread_core(struct lfd *f, void *buf, unsigned long count, uint64_t off)
{
    if (f->kind == FD_DEV)
        return dev_read(f->devno, buf, count);  /* char device: offset ignored */
    if (f->kind == FD_RAM) {
        struct ramfile *rf = &ramfs[f->ridx];
        if (off >= rf->len)
            return 0;
        uint32_t avail = rf->len - (uint32_t)off;
        uint32_t n = count < avail ? (uint32_t)count : avail;
        memcpy(buf, rf->data + off, n);
        return n;
    }
    long n = p9_read(f->p9fid, off, buf, (uint32_t)count);
    return n < 0 ? -LERR_INVAL : n;
}

/* Positioned write from a kernel-accessible buffer, the mirror of pread_core. */
static long
pwrite_core(struct lfd *f, const void *buf, unsigned long count, uint64_t off)
{
    if (f->kind == FD_DEV)
        return dev_write(f->devno, buf, count);  /* char device: offset ignored */
    if (f->kind == FD_RAM) {
        struct ramfile *rf = &ramfs[f->ridx];
        uint64_t end = off + count;
        if (end > RAMFS_FILE_MAX || ramfs_grow(rf, (uint32_t)end) < 0)
            return -LERR_NOMEM;
        memcpy(rf->data + off, buf, count);
        if ((uint32_t)end > rf->len)
            rf->len = (uint32_t)end;
        rf->mtime = fs_stamp();
        return (long)count;
    }
    if (f->is9w) {
        unsigned long done = 0;
        while (done < count) {
            long n = p9_write(f->p9fid, off + done,
                              (const char *)buf + done, (uint32_t)(count - done));
            if (n <= 0)
                break;
            done += (unsigned long)n;
        }
        if (done == 0 && count > 0)
            return -LERR_ROFS;
        return (long)done;
    }
    return -LERR_BADF;                  /* read-only bundle file */
}

/* pread: read at an explicit offset without moving the fd cursor. The dynamic
 * linker reads library program headers this way. Pipes and the console have no
 * position, so they are rejected. */
static long
do_pread(long fd, void *buf, unsigned long count, uint64_t off)
{
    struct lfd *f = fd_get(fd);

    if (!f || f->kind == FD_CON || f->kind == FD_PIPE || f->kind == FD_DIR)
        return -LERR_BADF;
    if (count && !user_ok((uint64_t)(uintptr_t)buf, count))
        return -LERR_FAULT;
    return pread_core(f, buf, count, off);
}

/* pwrite: write at an explicit offset without disturbing the fd's own
 * position, the mirror of do_pread. A ram file grows to fit; a writable 9P
 * file writes through to the host at off. Pipes, the console and directories
 * have no position, so they are rejected, as on Linux. */
static long
do_pwrite(long fd, const void *buf, unsigned long count, uint64_t off)
{
    struct lfd *f = fd_get(fd);

    if (!f || f->kind == FD_CON || f->kind == FD_PIPE || f->kind == FD_DIR)
        return -LERR_BADF;
    if (count && !user_ok((uint64_t)(uintptr_t)buf, count))
        return -LERR_FAULT;
    return pwrite_core(f, buf, count, off);
}

/* splice(fd_in, off_in, fd_out, off_out, len, flags): move up to len bytes
 * between two descriptors, one of which must be a pipe, without passing the
 * data through userspace. We bounce it through a kernel buffer: the stream end
 * (a NULL offset) reads or writes through its backend ops (so a pipe blocks and
 * reports EOF as a normal read does), and a non-NULL offset drives the seekable
 * end through pread_core/pwrite_core and is advanced in place. One chunk per
 * call, like Linux, which may return a short count; callers loop until 0. GNU
 * grep uses this to drain its input to /dev/null, and a real kernel always has
 * splice, so returning ENOSYS made grep treat it as a fatal input error. */
#define SPLICE_CHUNK 65536
static long
do_splice(long fd_in, uint64_t uoff_in, long fd_out, uint64_t uoff_out,
          unsigned long len, unsigned int flags)
{
    (void)flags;
    struct lfd *fin = fd_get(fd_in);
    struct lfd *fout = fd_get(fd_out);
    if (!fin || !fout)
        return -LERR_BADF;
    if (fin->kind != FD_PIPE && fout->kind != FD_PIPE)
        return -LERR_INVAL;             /* splice needs one pipe end */
    if ((fin->kind == FD_PIPE && uoff_in) || (fout->kind == FD_PIPE && uoff_out))
        return -LERR_SPIPE;             /* an offset on the pipe end is ESPIPE */
    if (len == 0)
        return 0;
    if (len > SPLICE_CHUNK)
        len = SPLICE_CHUNK;

    int64_t in_off = -1, out_off = -1;
    if (uoff_in && copy_from_user(&in_off, uoff_in, sizeof in_off) != 0)
        return -LERR_FAULT;
    if (uoff_out && copy_from_user(&out_off, uoff_out, sizeof out_off) != 0)
        return -LERR_FAULT;

    char *buf = kmalloc(len);
    if (!buf)
        return -LERR_NOMEM;

    long got = uoff_in ? pread_core(fin, buf, len, (uint64_t)in_off)
                       : fin->ops->read(fin, buf, len);
    if (got <= 0) {
        kfree(buf);
        return got;                     /* 0 at EOF, or a negative errno */
    }
    long put = uoff_out ? pwrite_core(fout, buf, (unsigned long)got, (uint64_t)out_off)
                        : fout->ops->write(fout, buf, (unsigned long)got);
    kfree(buf);
    if (put < 0)
        return put;
    if (uoff_in && put > 0) {
        in_off += put;
        copy_to_user(uoff_in, &in_off, sizeof in_off);
    }
    if (uoff_out && put > 0) {
        out_off += put;
        copy_to_user(uoff_out, &out_off, sizeof out_off);
    }
    return put;
}

/* sendfile(out_fd, in_fd, offset, count): copy up to count bytes from a
 * seekable in_fd to out_fd, like splice but with no pipe requirement. When
 * offset is non-NULL the copy starts there and advances it in place without
 * moving the file position; when NULL it reads from and advances in_fd's own
 * position. One chunk per call, as on Linux; callers loop. busybox cat and cp
 * use it, and it returned ENOSYS before. */
static long
do_sendfile(long out_fd, long in_fd, uint64_t uoff, unsigned long count)
{
    struct lfd *fin = fd_get(in_fd);
    struct lfd *fout = fd_get(out_fd);
    if (!fin || !fout)
        return -LERR_BADF;
    if (fin->kind == FD_PIPE || fin->kind == FD_CON || fin->kind == FD_DIR)
        return -LERR_INVAL;             /* the source must be seekable */
    if (count == 0)
        return 0;
    if (count > SPLICE_CHUNK)
        count = SPLICE_CHUNK;

    int64_t off = 0;
    uint64_t pos;
    if (uoff) {
        if (copy_from_user(&off, uoff, sizeof off) != 0)
            return -LERR_FAULT;
        pos = (uint64_t)off;
    } else {
        pos = fin->of ? fin->of->pos : 0;
    }

    char *buf = kmalloc(count);
    if (!buf)
        return -LERR_NOMEM;

    long got = pread_core(fin, buf, count, pos);
    if (got <= 0) {
        kfree(buf);
        return got;
    }
    long put = fout->ops->write(fout, buf, (unsigned long)got);
    kfree(buf);
    if (put < 0)
        return put;
    if (uoff) {
        off += put;
        copy_to_user(uoff, &off, sizeof off);
    } else if (fin->of) {
        fin->of->pos += (uint64_t)put;
    }
    return put;
}

static long
do_close(long fd)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (f->ops && f->ops->close)
        f->ops->close(f);               /* drop one backend reference */
    f->used = 0;                        /* release the descriptor slot */
    return 0;
}

static long
do_lseek(long fd, long off, long whence)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    /* A /dev character device has no meaningful position; a seek on it is a
     * no-op that reports offset 0, as /dev/null and friends do on Linux. */
    if (f->kind == FD_DEV)
        return 0;
    /* A pipe or the console is not seekable, whatever fd number it sits on.
     * A file-backed fd is seekable even when it has been moved onto 0, 1 or 2
     * by a redirect, which is exactly what grep does: lseek(0, 0, SEEK_CUR). */
    if (f->kind == FD_CON || f->kind == FD_PIPE)
        return -LERR_SPIPE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return -LERR_INVAL;
    uint64_t end = f->kind == FD_RAM ? ramfs[f->ridx].len : f->size;
    uint64_t base = (whence == SEEK_CUR) ? f->of->pos :
                    (whence == SEEK_END) ? end : 0;
    f->of->pos = base + (uint64_t)off;
    return (long)f->of->pos;
}

/* An overlay inode is 0x10000+ri, so recover a created file's own mtime from it;
 * bundle files and other objects report the epoch. */
static uint64_t
mtime_of_ino(uint64_t ino)
{
    if (ino >= 0x10000 && ino < 0x10000 + (uint64_t)RAMFS_MAX)
        return ramfs[ino - 0x10000].mtime;
    return guest_epoch();
}

static void
fill_stat(struct linux_stat *s, int isdir, uint64_t size, uint64_t ino,
          uint64_t mtime)
{
    uint64_t mt = mtime ? mtime : mtime_of_ino(ino);
    memset(s, 0, sizeof *s);
    s->st_mode = isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    s->st_nlink = 1;
    s->st_size = (int64_t)size;
    s->st_blksize = 4096;
    s->st_blocks = (int64_t)((size + 511) / 512);
    s->st_ino = ino;
    s->st_atime = s->st_mtime = s->st_ctime = mt;
}

/* Resolve a normalized key's metadata (a ramfs file shadows the bundle, else
 * 9P). Returns 0 and fills the out-params, or a negative errno. Callers that
 * hold a raw userspace path use meta_path, which applies the cwd first. */
static long
meta_key(const char *key, int *isdir, uint64_t *size, uint64_t *ino,
         uint64_t *mtime)
{
    if (mtime)
        *mtime = 0;                     /* 0 means: fall back to mtime_of_ino */
    int dk = dev_lookup(key);           /* a /dev node, or the /dev directory */
    if (dk) {
        *isdir = 0;
        *size = 0;
        *ino = DEV_INO_BASE + (uint64_t)dk;
        return 0;
    }
    if (dev_is_dir(key)) {
        *isdir = 1;
        *size = 0;
        *ino = DEV_INO_BASE;
        return 0;
    }
    int ri = ramfs_find(key);
    if (ri >= 0) {
        *isdir = ramfs[ri].is_dir;
        *size = ramfs[ri].is_dir ? 0 : ramfs[ri].len;
        *ino = (uint64_t)(0x10000 + ri);
        return 0;
    }
    /* The root, or a directory implied by overlay files created below it. */
    if (key[0] == 0 || ramfs_has_desc(key)) {
        *isdir = 1;
        *size = 0;
        *ino = 1;
        return 0;
    }
    int su = usock_bound_key(key);      /* a bound AF_UNIX socket name */
    if (su >= 0) {
        *isdir = 0;
        *size = 0;
        *ino = SOCK_INO_BASE + (uint64_t)su;
        return 0;
    }
    uint32_t fid = LINUX_FID_BASE + GFID_MAX;       /* scratch, above the pool */
    if (ns_walk(key, fid) != 0)
        return -LERR_NOENT;
    struct p9_stat st;
    int ok = p9_stat(fid, &st) == 0;
    if (ok) {
        *isdir = (st.qid.type & P9_QTDIR) != 0;
        *size = st.length;
        *ino = st.qid.path;
        if (mtime)
            *mtime = st.mtime;          /* real host mtime, Unix seconds */
    }
    p9_clunk(fid);
    return ok ? 0 : -LERR_NOENT;
}

/* Metadata for a raw userspace path: resolve the cwd, then meta_key. */
static long
meta_path(const char *path, int *isdir, uint64_t *size, uint64_t *ino,
          uint64_t *mtime)
{
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    return meta_key(key, isdir, size, ino, mtime);
}

/* stat a path. follow=1 (stat) resolves a final-component symlink to its
 * target; follow=0 (lstat) stats the link itself and reports it as S_IFLNK. */
static long
do_stat_path(const char *path, struct linux_stat *out, int follow)
{
    if (!user_ok((uint64_t)(uintptr_t)out, sizeof *out))
        return -LERR_FAULT;
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    if (follow)
        follow_link_key(key, sizeof key);
    int isdir = 0;
    uint64_t size = 0, ino = 1, mtime = 0;
    long rc = meta_key(key, &isdir, &size, &ino, &mtime);

    if (rc == 0) {
        fill_stat(out, isdir, size, ino, mtime);
        if (dev_lookup(key))
            out->st_mode = S_IFCHR | 0666;      /* a /dev character device */
        else if (usock_bound_key(key) >= 0)
            out->st_mode = S_IFSOCK | 0755;     /* a bound AF_UNIX socket name */
        else if (!follow) {
            int ri = ramfs_find(key);
            if (ri >= 0 && ramfs[ri].is_link)
                out->st_mode = S_IFLNK | 0777;  /* lstat sees the link, not its target */
        }
    }
    return rc;
}

/* Create an overlay symlink: a ramfile flagged is_link whose data holds the
 * target path string. The target is stored verbatim and not resolved, so a
 * dangling or absolute target is kept as given (readlink returns it as-is).
 * Node's configure makes out/tools/bin/python point at the real interpreter. */
static long
do_symlink(const char *target, const char *linkpath)
{
    char key[RAMFS_NAME];
    resolve_path(linkpath, key, sizeof key);
    if (key[0] == 0)
        return -LERR_EXIST;             /* the root always exists */
    int isdir;
    uint64_t sz, ino;
    if (meta_key(key, &isdir, &sz, &ino, NULL) == 0)
        return -LERR_EXIST;             /* overlay entry or bundle name taken */
    int ri = ramfs_create(key, 0);
    if (ri < 0)
        return -LERR_NOMEM;             /* overlay table full */
    size_t tl = strlen(target);
    if (ramfs_grow(&ramfs[ri], (uint32_t)tl + 1) < 0) {
        ramfs_free_slot(ri);
        return -LERR_NOMEM;
    }
    memcpy(ramfs[ri].data, target, tl + 1);
    ramfs[ri].len = (uint32_t)tl;
    ramfs[ri].is_link = 1;
    return 0;
}

/* readlink: copy an overlay symlink's stored target into the user buffer,
 * truncated to bufsiz (no terminator, as Linux specifies). Returns the byte
 * count, -LERR_INVAL for a non-symlink, or -LERR_NOENT if nothing resolves. */
static long
do_readlink(const char *path, char *ubuf, long bufsiz)
{
    if (bufsiz <= 0)
        return -LERR_INVAL;
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    int ri = ramfs_find(key);
    if (ri < 0)
        return -LERR_NOENT;             /* bundle entries are never symlinks */
    if (!ramfs[ri].is_link)
        return -LERR_INVAL;
    uint32_t n = ramfs[ri].len;
    if ((long)n > bufsiz)
        n = (uint32_t)bufsiz;
    if (copy_to_user((uint64_t)(uintptr_t)ubuf, ramfs[ri].data, n) != 0)
        return -LERR_FAULT;
    return (long)n;
}

/* mkdir: create a writable-overlay directory. Fails if anything already
 * resolves at the path (an overlay entry, a bundle path, or an implicit
 * directory). mkdir -p ignores the resulting EEXIST. */
static long
do_mkdir(const char *path)
{
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    if (key[0] == 0)
        return -LERR_EXIST;             /* the root always exists */
    int isdir;
    uint64_t sz, ino;
    if (meta_key(key, &isdir, &sz, &ino, NULL) == 0)
        return -LERR_EXIST;
    if (under_rw_mount(key))            /* land the directory on the host */
        return create_9p_dir(key);
    if (ramfs_create(key, 1) < 0)
        return -LERR_NOMEM;
    return 0;
}

/* rmdir: remove an empty writable-overlay directory. A non-empty one is
 * ENOTEMPTY; a read-only bundle directory is EROFS. */
static long
do_rmdir(const char *path)
{
    char key[RAMFS_NAME];
    resolve_path(path, key, sizeof key);
    if (key[0] == 0)
        return -LERR_ACCES;
    if (ramfs_has_desc(key))
        return -LERR_NOTEMPTY;
    int ri = ramfs_find(key);
    if (ri >= 0 && ramfs[ri].is_dir) {
        ramfs_free_slot(ri);
        return 0;
    }
    int isdir;
    uint64_t sz, ino;
    if (meta_key(key, &isdir, &sz, &ino, NULL) != 0)
        return -LERR_NOENT;
    if (!isdir)
        return -LERR_NOTDIR;
    if (under_rw_mount(key))            /* remove the host-mount directory */
        return remove_9p(key);
    return -LERR_ROFS;
}

/* Overwrite entry index i's stored name with `name` (assumed normalized). */
static void
ramfs_rename_entry(int i, const char *name)
{
    size_t k = 0;
    while (name[k] && k < RAMFS_NAME - 1) {
        ramfs[i].name[k] = name[k];
        k++;
    }
    ramfs[i].name[k] = 0;
}

/* rename within the writable overlay. A directory carries its descendants
 * (their key prefix is rewritten). A read-only source cannot be renamed
 * (EXDEV), which leaves the caller to fall back to copy-and-delete. */
static long
do_rename(const char *oldp, const char *newp)
{
    char ok[RAMFS_NAME], nk[RAMFS_NAME];
    resolve_path(oldp, ok, sizeof ok);
    resolve_path(newp, nk, sizeof nk);
    if (ok[0] == 0 || nk[0] == 0)
        return -LERR_ACCES;

    int ri = ramfs_find(ok);
    if (ri < 0) {
        int isdir;
        uint64_t sz, ino;
        if (meta_key(ok, &isdir, &sz, &ino, NULL) == 0)
            return -LERR_XDEV;          /* read-only source: cannot rename */
        return -LERR_NOENT;
    }

    int di = ramfs_find(nk);            /* overwrite an existing destination */
    if (di >= 0 && di != ri)
        ramfs_free_slot(di);

    if (ramfs[ri].is_dir) {             /* carry the subtree: rewrite prefixes */
        size_t ol = strlen(ok);
        for (int i = 0; i < RAMFS_MAX; i++) {
            if (!ramfs[i].used || ramfs[i].unlinked || i == ri)
                continue;
            if (strncmp(ramfs[i].name, ok, ol) != 0 || ramfs[i].name[ol] != '/')
                continue;
            char nn[RAMFS_NAME];
            size_t w = 0;
            for (size_t j = 0; nk[j] && w < RAMFS_NAME - 1; j++)
                nn[w++] = nk[j];
            for (const char *s = ramfs[i].name + ol; *s && w < RAMFS_NAME - 1; s++)
                nn[w++] = *s;
            nn[w] = 0;
            ramfs_rename_entry(i, nn);
        }
    }
    ramfs_rename_entry(ri, nk);
    return 0;
}

/* The subset of struct statx the basic-stats mask needs, padded to the kernel's
 * 256-byte record. glibc's stat/ls go through statx on current systems. */
struct statx_ts {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_ts stx_atime;
    struct statx_ts stx_btime;
    struct statx_ts stx_ctime;
    struct statx_ts stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor;
    uint32_t stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2[13];
};

#define STATX_BASIC_STATS 0x000007ffu
#define AT_EMPTY_PATH     0x1000
#define AT_SYMLINK_NOFOLLOW 0x100

static void
fill_statx(struct linux_statx *s, int isdir, uint64_t size, uint64_t ino,
           uint64_t mtime)
{
    uint64_t mt = mtime ? mtime : mtime_of_ino(ino);
    memset(s, 0, sizeof *s);
    s->stx_mask = STATX_BASIC_STATS;
    s->stx_blksize = 4096;
    s->stx_nlink = 1;
    s->stx_mode = (uint16_t)(isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    s->stx_ino = ino;
    s->stx_size = size;
    s->stx_blocks = (size + 511) / 512;
    s->stx_atime.tv_sec = s->stx_mtime.tv_sec = s->stx_ctime.tv_sec = (int64_t)mt;
    s->stx_btime.tv_sec = (int64_t)mt;
}

/* statx(dirfd, path, flags, mask, buf). An empty path with AT_EMPTY_PATH stats
 * the dirfd itself; otherwise the path (ramfs or bundle) is resolved. */
static long
do_statx(long dirfd, const char *path, long flags, struct linux_statx *out)
{
    int isdir = 0, islink = 0, ischar = 0, issock = 0;
    uint64_t size = 0, ino = 1, mtime = 0;

    if (!user_ok((uint64_t)(uintptr_t)out, sizeof *out))
        return -LERR_FAULT;
    if (path && path[0]) {
        char key[RAMFS_NAME];
        resolve_path(path, key, sizeof key);
        if (!(flags & AT_SYMLINK_NOFOLLOW))
            follow_link_key(key, sizeof key);
        long rc = meta_key(key, &isdir, &size, &ino, &mtime);
        if (rc != 0)
            return rc;
        ischar = dev_lookup(key) != 0;
        issock = usock_bound_key(key) >= 0;
        if (!ischar && (flags & AT_SYMLINK_NOFOLLOW)) {
            int ri = ramfs_find(key);       /* lstat-style: see the link itself */
            islink = ri >= 0 && ramfs[ri].is_link;
        }
    } else if (flags & AT_EMPTY_PATH) {
        struct lfd *f = fd_get(dirfd);
        if (!f)
            return -LERR_BADF;
        if (f->kind == FD_DEV) {
            ischar = 1;
            ino = f->ino;
        } else if (f->kind == FD_RAM) {
            size = ramfs[f->ridx].len;
            ino = (uint64_t)(0x10000 + f->ridx);
        } else if (f->kind == FD_SOCK) {
            issock = 1;
            ino = SOCK_INO_BASE + (uint64_t)f->sidx;
        } else {
            isdir = f->kind == FD_DIR;
            size = f->size;
            ino = f->p9fid;
        }
    } else {
        return -LERR_NOENT;
    }
    fill_statx(out, isdir, size, ino, mtime);
    if (ischar)
        out->stx_mode = S_IFCHR | 0666;
    else if (issock)
        out->stx_mode = S_IFSOCK | 0755;
    else if (islink)
        out->stx_mode = S_IFLNK | 0777;
    return 0;
}

static long
do_fstat(long fd, struct linux_stat *out)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (!user_ok((uint64_t)(uintptr_t)out, sizeof *out))
        return -LERR_FAULT;
    if (f->kind == FD_PIPE) {
        /* Report a FIFO so a tool copies with a read/write loop, not mmap. */
        memset(out, 0, sizeof *out);
        out->st_mode = S_IFIFO | 0644;
        out->st_nlink = 1;
        out->st_blksize = PIPE_CAP;
        out->st_ino = (uint64_t)(0x20000 + f->pidx);
        return 0;
    }
    if (f->kind == FD_DEV) {
        fill_stat(out, 0, 0, f->ino, 0);
        out->st_mode = S_IFCHR | 0666;              /* a /dev character device */
        return 0;
    }
    if (f->kind == FD_CON) {
        fill_stat(out, 0, 0, (uint64_t)fd, 0);      /* console: a plain reg */
        return 0;
    }
    if (f->kind == FD_RAM) {
        fill_stat(out, 0, ramfs[f->ridx].len, (uint64_t)(0x10000 + f->ridx), 0);
        return 0;
    }
    if (f->kind == FD_SOCK) {
        fill_stat(out, 0, 0, SOCK_INO_BASE + (uint64_t)f->sidx, 0);
        out->st_mode = S_IFSOCK | 0755;             /* an AF_UNIX socket fd */
        return 0;
    }
    fill_stat(out, f->kind == FD_DIR, f->size, f->ino, 0);
    return 0;
}

/* Pack one getdents64 record into buf at `used`. Returns the new `used`, or 0
 * if it would overflow `count` (the record is left for the next call). */
static unsigned long
dirent_emit(uint8_t *out, unsigned long used, unsigned long count,
            uint64_t ino, int64_t off, int dtype, const char *name)
{
    unsigned long nlen = strlen(name);
    unsigned long reclen = (19 + nlen + 1 + 7) & ~(unsigned long)7;
    if (used + reclen > count)
        return 0;
    struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
    d->d_ino = ino ? ino : 1;
    d->d_off = off;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = (uint8_t)dtype;
    memcpy(d->d_name, name, nlen);
    d->d_name[nlen] = 0;
    return used + reclen;
}

/* getdents64 over a directory that may have both a read-only 9P side and a
 * writable overlay side. The 9P entries come first (phase 1, cursor in of->pos),
 * then the overlay's direct children (phase 2, cursor in of->dov). Each call
 * emits as many whole records as fit and resumes where it left off; a return of
 * 0 means the directory is exhausted. Overlay and 9P are not deduplicated, so a
 * created file that shadows a bundle file of the same name lists twice. */
static long
do_getdents64(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f || fd < 3)
        return -LERR_BADF;
    if (f->kind != FD_DIR)
        return -LERR_NOTDIR;
    if (count && !user_ok((uint64_t)(uintptr_t)buf, count))
        return -LERR_FAULT;

    uint8_t *out = buf;
    unsigned long used = 0;

    /* Phase 1: the 9P directory stream. */
    if (f->p9fid && !f->of->d9done) {
        uint8_t kbuf[4096];
        long got = p9_read(f->p9fid, f->of->pos, kbuf, sizeof kbuf);
        if (got > 0) {
            uint32_t off = 0, consumed = 0;
            struct p9_stat e;
            while (p9_dir_next(kbuf, (uint32_t)got, &off, &e)) {
                unsigned long u = dirent_emit(out, used, count,
                                              e.qid.path, (int64_t)(f->of->pos + off),
                                              (e.qid.type & P9_QTDIR) ? DT_DIR : DT_REG,
                                              e.name);
                if (u == 0)
                    break;              /* no room: leave for the next call */
                used = u;
                consumed = off;
            }
            if (used == 0 && off > 0)
                return -LERR_INVAL;     /* a single entry too big for the buffer */
            f->of->pos += consumed;
            if (used > 0)
                return (long)used;      /* more 9P may remain; caller re-enters */
        }
        f->of->d9done = 1;              /* 9P stream exhausted */
    }

    /* Phase 1.5: synthetic entries. A listing of /dev yields its character
     * nodes; a listing of the root yields a "dev" directory, so both are
     * visible without the host serving them. */
    if (dev_is_dir(f->of->dpath)) {
        for (int i = f->of->dsynth; i < NDEV; i++) {
            f->of->dsynth = i;
            unsigned long u = dirent_emit(out, used, count,
                                          DEV_INO_BASE + (uint64_t)devtab[i].kind,
                                          (int64_t)(i + 1), DT_CHR, devtab[i].name);
            if (u == 0)
                return used ? (long)used : -LERR_INVAL;
            used = u;
        }
        f->of->dsynth = NDEV;
    } else if (f->of->dpath[0] == 0 && f->of->dsynth == 0) {
        unsigned long u = dirent_emit(out, used, count, DEV_INO_BASE,
                                      1, DT_DIR, "dev");
        if (u == 0)
            return used ? (long)used : -LERR_INVAL;
        used = u;
        f->of->dsynth = 1;
    }

    /* Phase 2: the writable overlay's direct children of this directory. */
    for (int i = (int)f->of->dov; i < RAMFS_MAX; i++) {
        f->of->dov = (uint32_t)i;
        if (!ramfs[i].used || ramfs[i].unlinked)
            continue;
        const char *leaf = ramfs_child_leaf(ramfs[i].name, f->of->dpath);
        if (!leaf)
            continue;
        unsigned long u = dirent_emit(out, used, count, (uint64_t)(0x10000 + i),
                                      (int64_t)(i + 1),
                                      ramfs[i].is_dir ? DT_DIR : DT_REG, leaf);
        if (u == 0)
            return used ? (long)used : -LERR_INVAL;     /* resume at i next call */
        used = u;
    }
    f->of->dov = RAMFS_MAX;             /* overlay exhausted */
    return (long)used;
}

/****************************************************************
 * Processes: fork, wait, exit. Each Linux process is a libOS task with its own
 * address space and file table. They run cooperatively, switching only when a
 * syscall blocks (on a pipe, or in wait4).
 ****************************************************************/

/* clone flags we care about. */
#define CLONE_VFORK          0x00004000u    /* parent blocks until exec or exit */
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_SETTLS         0x00080000u

static int next_pid = 1;
static int next_tid = 1;

/* The task function for a new thread or forked child: its trapframe was set so
 * it resumes with rax = 0 (fork/clone return 0 in the child). */
static void
process_entry(void)
{
    linux_resume(lproc_cur);            /* lproc_cur set by the resume hook */
}

/* Switch the active address space and TLS into the thread before its task runs
 * (registered with the scheduler as the resume hook). */
static void
linux_on_resume(void *vp)
{
    lproc_cur = (struct lproc *)vp;
    vmspace_switch(lproc_cur->sh->vmspace);
    arch_load_tls(lproc_cur->tls);
    /* A timer tick taken while this thread runs in user mode enters the kernel
     * through TSS.rsp0; point it at this thread's own handler stack so the
     * preemption frame lands there (private and idle while the thread is in
     * user mode), which makes the reschedule reentrant across threads. */
    arch_set_kstack(lproc_cur->kstack_top);
}

/* Threads that have exited but still hold a libOS task slot and kstack. They
 * cannot free their own kstack (they run on it), so another thread drains them:
 * reap the task, free the stack, release the slot. */
static struct lproc *zombie_thr[LINUX_NPROC];
static int n_zombie_thr;

static void
reap_zombie_threads(void)
{
    int kept = 0;
    for (int i = 0; i < n_zombie_thr; i++) {
        struct lproc *t = zombie_thr[i];
        if (task_reap(t->task_id) == 0) {   /* was a zombie: now reaped */
            if (t->kstack)
                kfree(t->kstack);
            t->used = 0;
        } else {
            zombie_thr[kept++] = t;         /* not finished yet; keep it */
        }
    }
    n_zombie_thr = kept;
}

static struct lshared *
sh_alloc(void)
{
    for (int i = 0; i < LINUX_NPROC; i++) {
        if (shareds[i].used)
            continue;
        memset(&shareds[i], 0, sizeof shareds[i]);
        shareds[i].used = 1;
        return &shareds[i];
    }
    return NULL;
}

static struct lproc *
thr_alloc(void)
{
    for (int i = 0; i < LINUX_NPROC; i++) {
        if (procs[i].used)
            continue;
        struct lproc *p = &procs[i];
        memset(p, 0, sizeof *p);
        p->kstack = kmalloc(LINUX_KSTACK);
        if (!p->kstack)
            return NULL;
        p->kstack_top = ((uint64_t)(uintptr_t)p->kstack + LINUX_KSTACK)
                        & ~(uint64_t)15;
        p->used = 1;
        return p;
    }
    return NULL;
}

/* A new process: a fresh shared block and its main thread (no address space
 * yet; the caller clones or builds one). */
static struct lproc *
proc_new(void)
{
    struct lshared *sh = sh_alloc();
    if (!sh)
        return NULL;
    struct lproc *t = thr_alloc();
    if (!t) {
        sh->used = 0;
        return NULL;
    }
    sh->refs = 1;
    sh->pid = next_pid++;
    t->sh = sh;
    t->tid = sh->pid;                   /* main thread: tid == pid */
    t->ismain = 1;
    return t;
}

/* Release a process's open descriptors, dropping the shared pipe and 9P
 * references so a reader sees EOF once every writer is gone. */
static void
release_fds(struct lshared *sh)
{
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &sh->fdtab[i];
        if (!f->used || f->kind == FD_CON)
            continue;
        ofile_unref(f->of);             /* drop this fd's share of the offset */
        if (f->kind == FD_PIPE) {
            struct lpipe *pp = &pipes[f->pidx];
            if (f->wr && pp->writers > 0)
                pp->writers--;
            else if (!f->wr && pp->readers > 0)
                pp->readers--;
            if (pp->readers == 0 && pp->writers == 0)
                pp->used = 0;
        } else if (f->kind == FD_RAM) {
            ramfs_unref(f->ridx);       /* drop this process's ramfile reference */
        } else if (f->kind == FD_SOCK || f->kind == FD_EVENTFD ||
                   f->kind == FD_EPOLL) {
            /* These share a global backend pool across fork and hold no ofile.
             * Their close op drops the reference, and for a socket it marks the
             * peer closed so a client blocked in read/recv gets EOF when the
             * server process exits. Missing this leaks the slot and hangs the
             * peer. */
            if (f->ops && f->ops->close)
                f->ops->close(f);
        } else if (f->p9fid) {
            uint32_t slot = f->p9fid - LINUX_FID_BASE;
            if (gfidref[slot] && --gfidref[slot] == 0)
                p9_clunk(f->p9fid);
        }
        f->used = 0;
    }
}

/* Futex: a small wait queue keyed by (process, user address). Enough for the
 * pthread primitives (mutex, cond, join), which use private futexes. */
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10

struct fwaiter {
    int             used;
    struct lshared *sh;
    uint64_t        uaddr;
    int             task_id;
};
static struct fwaiter fwaiters[LINUX_NPROC * 2];

static long
futex_wake(uint64_t uaddr, int n)
{
    int woke = 0;
    for (int i = 0; i < (int)(sizeof fwaiters / sizeof fwaiters[0]) && woke < n;
         i++) {
        struct fwaiter *w = &fwaiters[i];
        if (w->used && w->sh == lproc_cur->sh && w->uaddr == uaddr) {
            w->used = 0;
            task_wake(w->task_id);
            woke++;
        }
    }
    return woke;
}

static long
do_futex(uint64_t uaddr, int op, int val)
{
    int cmd = op & 0x7f;                /* strip PRIVATE and CLOCK_REALTIME */

    if (!user_ok(uaddr, sizeof(int)))
        return -LERR_FAULT;
    /* FUTEX_WAIT_BITSET/WAKE_BITSET carry a bit mask (and the WAIT form an
     * absolute rather than relative timeout). Callers that use them, including
     * abseil's thread pool that V8's concurrent builtin generation runs on, pass
     * FUTEX_BITSET_MATCH_ANY, so the mask selects every waiter and they reduce to
     * plain WAIT/WAKE. This layer already ignores the timeout and the mask (not
     * forwarded from the dispatch), so route them through the same paths; a
     * mismatch here is fatal to the caller (abseil RAW-logs and aborts on an
     * ENOSYS from a futex). */
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        /* The compare and the enlisting both run under the giant lock, and so
         * does futex_wake, so a concurrent wake cannot slip between them: either
         * the waker's value store lands first and the compare fails here (no
         * block), or we are enlisted before the waker scans and it wakes us. The
         * lock is only dropped inside lx_block, after we are in the wait set,
         * and a wake in that final window is caught by task_block's wake_pending.
         * That closes the lost-wakeup race a bare compare-then-block would have. */
        if (*(volatile int *)(uintptr_t)uaddr != val)
            return -LERR_AGAIN;         /* value changed: do not block */
        int slot = -1;
        for (int i = 0; i < (int)(sizeof fwaiters / sizeof fwaiters[0]); i++)
            if (!fwaiters[i].used) {
                slot = i;
                break;
            }
        if (slot < 0)
            return -LERR_AGAIN;
        int me = lproc_cur->task_id;
        fwaiters[slot].used = 1;
        fwaiters[slot].sh = lproc_cur->sh;
        fwaiters[slot].uaddr = uaddr;
        fwaiters[slot].task_id = me;
        lx_block();                     /* drops the giant lock while parked */
        signal_killpoint();             /* a fatal signal unblocks and terminates */
        /* Reclaim the slot, unless a wake already freed it and another waiter
         * has since taken it (then used is set with a different task_id, and
         * clearing it would strand that waiter). */
        if (fwaiters[slot].used && fwaiters[slot].task_id == me)
            fwaiters[slot].used = 0;
        if (signal_deliverable())       /* a caught signal interrupts the wait */
            return -LERR_RESTARTSYS;
        return 0;
    }
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET)
        return futex_wake(uaddr, val);
    return -LERR_NOSYS;
}

/* fork: a child process that is a copy of the caller. The address space is an
 * eager copy; the main thread's trapframe is the parent's with rax = 0; the
 * open descriptors are the same objects (shared pipe buffers and 9P fids). */
static long
do_fork(void)
{
    struct lproc *parent = lproc_cur;
    struct lproc *child = proc_new();

    if (!child)
        return -LERR_NOMEM;
    struct lshared *csh = child->sh;
    csh->vmspace = vmspace_clone(parent->sh->vmspace);
    if (!csh->vmspace) {
        kfree(child->kstack);
        child->used = 0;
        csh->used = 0;
        return -LERR_NOMEM;
    }
    csh->parent = parent->sh->pid;
    memcpy(csh->fdtab, parent->sh->fdtab, sizeof csh->fdtab);
    memcpy(csh->cwd, parent->sh->cwd, sizeof csh->cwd);
    csh->heap_brk = parent->sh->heap_brk;
    csh->heap_mmap = parent->sh->heap_mmap;
    csh->heap_mmaptop = parent->sh->heap_mmaptop;
    csh->mmap_nhole = parent->sh->mmap_nhole;
    memcpy(csh->mmap_holes, parent->sh->mmap_holes,
           (size_t)csh->mmap_nhole * sizeof csh->mmap_holes[0]);

    /* Account for the inherited descriptors (the child shares them): one backend
     * reference each, through the same ops->ref a dup takes. */
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &csh->fdtab[i];
        if (!f->used)
            continue;
        if (f->ops && f->ops->ref)
            f->ops->ref(f);
    }

    /* The main thread resumes at the fork() site on the cloned stack, rax = 0. */
    child->tf = parent->tf;
    tf_set_ret0(&child->tf);
    child->tls = parent->tls;
    /* Signal dispositions are inherited across fork; the child starts with no
     * pending signals and the parent's blocked mask. */
    memcpy(csh->sigact, parent->sh->sigact, sizeof csh->sigact);
    child->sig_blocked = parent->sig_blocked;

    int tid = task_spawn_proc("linux", process_entry, child);
    if (tid < 0) {
        release_fds(csh);
        vmspace_destroy(csh->vmspace);
        kfree(child->kstack);
        child->used = 0;
        csh->used = 0;
        return -LERR_NOMEM;
    }
    child->task_id = tid;
    csh->main_task = tid;
    return csh->pid;                    /* the parent gets the child's pid */
}

/* clone for a thread (CLONE_VM): a new thread in the caller's process, sharing
 * the address space and files, running on child_stack with its own TLS. */
static long
do_clone_thread(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                uint64_t ctid, uint64_t tls)
{
    struct lproc *parent = lproc_cur;
    struct lshared *sh = parent->sh;

    /* Validate the tid write targets before allocating, so a bad pointer fails
     * cleanly with EFAULT instead of leaking a half-built thread. */
    if ((flags & CLONE_CHILD_SETTID) && !user_ok(ctid, sizeof(int)))
        return -LERR_FAULT;
    if ((flags & CLONE_PARENT_SETTID) && !user_ok(ptid, sizeof(int)))
        return -LERR_FAULT;
    if ((flags & CLONE_CHILD_CLEARTID) && !user_ok(ctid, sizeof(int)))
        return -LERR_FAULT;

    struct lproc *t = thr_alloc();

    if (!t)
        return -LERR_NOMEM;
    sh->refs++;
    t->sh = sh;
    t->tid = next_tid++;
    t->ismain = 0;

    /* Resume at the clone() site (rax = 0) on the new stack. */
    t->tf = parent->tf;
    tf_set_ret0(&t->tf);
    tf_set_sp(&t->tf, child_stack);
    t->tls = (flags & CLONE_SETTLS) ? tls : parent->tls;
    tf_set_tls(&t->tf, t->tls);         /* where the arch keeps TLS in the frame */
    t->sig_blocked = parent->sig_blocked;   /* a new thread inherits the mask */
    if (flags & CLONE_CHILD_SETTID)
        *(int *)(uintptr_t)ctid = t->tid;
    if (flags & CLONE_PARENT_SETTID)
        *(int *)(uintptr_t)ptid = t->tid;
    t->clear_ctid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;

    int tsk = task_spawn_proc("thread", process_entry, t);
    if (tsk < 0) {
        sh->refs--;
        kfree(t->kstack);
        t->used = 0;
        return -LERR_NOMEM;
    }
    t->task_id = tsk;
    return t->tid;                      /* the parent gets the child tid */
}

/* Refcount the descriptors a child inherits (shared pipe buffers and 9P fids). */
static void
inherit_fds(struct lshared *csh)
{
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &csh->fdtab[i];
        if (!f->used || f->kind == FD_CON)
            continue;
        if (f->of)
            f->of->nref++;              /* child shares the offset */
        if (f->kind == FD_PIPE) {
            if (f->wr)
                pipes[f->pidx].writers++;
            else
                pipes[f->pidx].readers++;
        } else if (f->kind == FD_RAM)
            ramfs[f->ridx].nref++;
        else if (f->kind == FD_EPOLL)
            epolls[f->epidx].nref++;
        else if (f->kind == FD_EVENTFD)
            eventfds[f->efdidx].nref++;
        else if (f->p9fid)
            gfidref[f->p9fid - LINUX_FID_BASE]++;
    }
}

/* clone with CLONE_VM|CLONE_VFORK: the posix_spawn path. The child is a new
 * process that borrows the parent's address space, runs the spawn trampoline on
 * the given stack (fd setup, then execve), and the parent is suspended until the
 * child execs its own image or exits. Because scheduling is cooperative, the
 * child runs first and the parent simply blocks until the child releases it. */
static long
do_spawn_vfork(uint64_t flags, uint64_t child_stack)
{
    (void)flags;
    struct lproc *parent = lproc_cur;
    struct lproc *child = proc_new();
    if (!child)
        return -LERR_NOMEM;
    struct lshared *csh = child->sh;

    /* Borrow the parent's address space; execve will give the child its own. */
    csh->vmspace = parent->sh->vmspace;
    csh->vm_borrowed = 1;
    csh->parent = parent->sh->pid;

    /* The child gets its own copy of the fd table (clone without CLONE_FILES),
     * so its file_actions dup2/close touch only its descriptors. */
    memcpy(csh->fdtab, parent->sh->fdtab, sizeof csh->fdtab);
    memcpy(csh->cwd, parent->sh->cwd, sizeof csh->cwd);
    inherit_fds(csh);
    csh->heap_brk = parent->sh->heap_brk;
    csh->heap_mmap = parent->sh->heap_mmap;
    csh->heap_mmaptop = parent->sh->heap_mmaptop;
    csh->mmap_nhole = parent->sh->mmap_nhole;
    memcpy(csh->mmap_holes, parent->sh->mmap_holes,
           (size_t)csh->mmap_nhole * sizeof csh->mmap_holes[0]);
    memcpy(csh->sigact, parent->sh->sigact, sizeof csh->sigact);
    child->sig_blocked = parent->sig_blocked;

    csh->vforking = 1;
    csh->vfork_waiter = parent->task_id;

    /* Resume at the clone site (rax = 0) on the spawn stack, sharing the VM. */
    child->tf = parent->tf;
    tf_set_ret0(&child->tf);
    if (child_stack)
        tf_set_sp(&child->tf, child_stack);
    child->tls = parent->tls;

    int tid = task_spawn_proc("spawn", process_entry, child);
    if (tid < 0) {
        release_fds(csh);
        kfree(child->kstack);
        child->used = 0;
        csh->used = 0;
        return -LERR_NOMEM;
    }
    child->task_id = tid;
    csh->main_task = tid;

    /* vfork: block until the child execs (new address space) or exits. The
     * giant lock is dropped while parked so the child can run its own syscalls
     * (its execve is what clears vforking and wakes us). */
    while (csh->vforking)
        lx_block();

    return csh->pid;
}

/* clone3(&args, size): parse the clone_args and route like clone. The struct is
 * a run of u64s; we read flags, the stack and its size, and tls. */
static long
do_clone3(uint64_t uargs, uint64_t size)
{
    if (size < 8 * 8)
        return -LERR_INVAL;
    uint64_t a[8];
    if (copy_from_user(a, uargs, sizeof a) < 0)
        return -LERR_FAULT;
    uint64_t flags = a[0];
    uint64_t ctid  = a[2];              /* child_tid */
    uint64_t ptid  = a[3];              /* parent_tid */
    uint64_t stack = a[5];
    uint64_t ssize = a[6];
    uint64_t tls   = a[7];
    uint64_t top = stack ? stack + ssize : 0;   /* clone3 gives base + size */

    if ((flags & CLONE_VM) && (flags & CLONE_VFORK))
        return do_spawn_vfork(flags, top);
    if (flags & CLONE_VM)
        return do_clone_thread(flags, top, ptid, ctid, tls);
    return do_fork();
}

/* wait4: reap a child process. Returns its pid and writes an exit-encoded
 * status. Yields while children still run so they can finish, unless WNOHANG is
 * set: then a run with no reapable child returns 0 at once instead of blocking.
 * A shell polling a background job (dash does this after a foreground command
 * completes) passes WNOHANG and must not be parked on the still-running job. */
#define LINUX_WNOHANG 1
static long
do_wait4(long pid, int *status, int options)
{
    struct lshared *mysh = lproc_cur->sh;

    if (status && !user_ok((uint64_t)(uintptr_t)status, sizeof *status))
        return -LERR_FAULT;

    /* pid is a pid_t (32-bit). A caller passing -1 (wait for any child) may set
     * only the low 32 bits, so sign-extend before the > 0 and match tests. */
    pid = (long)(int)pid;

    for (;;) {
        int haskid = 0;
        reap_zombie_threads();
        for (int i = 0; i < LINUX_NPROC; i++) {
            struct lshared *c = &shareds[i];
            if (!c->used || c->parent != mysh->pid)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;
            haskid = 1;
            if (c->exited && c->refs == 0) {
                int rpid = c->pid;
                if (status)
                    *status = c->exit_sig
                        ? (c->exit_sig & 0x7f)                      /* WIFSIGNALED */
                        : (int)((c->exit_status & 0xff) << 8);      /* WIFEXITED */
                task_reap(c->main_task);
                /* Destroy the child's own space, never a borrowed one (a
                 * posix_spawn child that died before exec still points at the
                 * parent's space). */
                if (c->vmspace && !c->vm_borrowed)
                    vmspace_destroy(c->vmspace);
                c->used = 0;
                return rpid;
            }
        }
        if (!haskid)
            return -LERR_CHILD;
        if (options & LINUX_WNOHANG)
            return 0;                   /* children live but none reapable yet */
        lx_yield();                     /* a child is still running: let it run */
        signal_killpoint();             /* a fatal signal unblocks and terminates */
        if (signal_deliverable())       /* a caught signal (e.g. SIGCHLD handler) */
            return -LERR_RESTARTSYS;
    }
}

/* exit a thread, or the whole process when group is set. The last thread to
 * leave frees the address space and files and leaves the shared block as a
 * zombie carrying the exit status for the parent's wait4. */
static void __attribute__((noreturn))
do_exit(long code, int group)
{
    struct lproc *t = lproc_cur;
    struct lshared *sh = t->sh;

    /* A joined thread clears its tid word and wakes whoever waits on it. The
     * address was validated when clone/set_tid_address recorded it, but recheck
     * here so a later address-space change cannot turn it into a kernel write. */
    if (t->clear_ctid && user_ok(t->clear_ctid, sizeof(int))) {
        *(volatile int *)(uintptr_t)t->clear_ctid = 0;
        futex_wake(t->clear_ctid, 1);
    }
    t->exited = 1;
    if (group) {
        sh->exited = 1;
        sh->exit_status = code;
        /* exit_group terminates the whole thread group, not just this thread.
         * Mark the process exited (above) and rouse every other live thread so
         * it runs, reaches a killpoint (every blocking loop calls
         * signal_killpoint after a wake), sees sh->exited, and tears itself
         * down. Without this a worker parked in futex_wait never exits, the
         * process never reaches refs 0, and a waiting parent spins in wait4. */
        for (int i = 0; i < LINUX_NPROC; i++) {
            struct lproc *p = &procs[i];
            if (p->used && !p->exited && p->sh == sh && p != t)
                task_wake(p->task_id);
        }
    }

    /* A posix_spawn child that exits before exec (its exec failed) still has to
     * release the parent blocked in vfork. */
    if (sh->vforking) {
        sh->vforking = 0;
        task_wake(sh->vfork_waiter);
    }

    if (--sh->refs == 0) {
        if (!sh->exited) {
            sh->exited = 1;
            sh->exit_status = code;
        }
        release_fds(sh);
        con_puts("[bin] pid ");
        put_dec(sh->pid);
        con_puts(" exited, status=");
        put_dec(sh->exit_status);
        con_putc('\n');
        /* Do not free the address space here: we are still running on it (CR3
         * points at these page tables). The parent's wait4, running under its
         * own CR3, destroys it; with no parent it is left until shutdown. The
         * slot lingers (used, exited) carrying the status for wait4. */

        /* Notify the parent that a child died. A shell reaping a background job
         * (dash does this from a SIGCHLD handler woken by sigsuspend) relies on
         * the signal, not just the zombie: it polls with WNOHANG, then blocks
         * for SIGCHLD. Post it process-directed and rouse the parent's threads
         * so a blocked wait or sigsuspend re-checks and the handler runs at the
         * next syscall boundary. */
        struct lshared *par = proc_by_pid(sh->parent);
        if (par) {
            par->sig_pending |= SIGBIT(SIGCHLD);
            wake_process_threads(par);
        }
    }

    /* This thread cannot free its own kstack (it runs on it); hand it to the
     * zombie-thread drainer, which another thread reaps. */
    if (n_zombie_thr < LINUX_NPROC)
        zombie_thr[n_zombie_thr++] = t;
    giant_unlock();                     /* task_exits never returns to the dispatcher */
    task_exits(NULL);
}

/* Map a CPU exception vector to the signal and the fault si_code it raises. */
static int
fault_signal(long vec, int *si_code)
{
    switch (vec) {
    case 0:  *si_code = 1; return 8;    /* #DE  -> SIGFPE, FPE_INTDIV */
    case 6:  *si_code = 1; return 4;    /* #UD  -> SIGILL, ILL_ILLOPC */
    case 14: *si_code = 1; return 11;   /* #PF  -> SIGSEGV, SEGV_MAPERR (addr=cr2) */
    case 13: *si_code = 0x80; return 11;/* #GP  -> SIGSEGV, SI_KERNEL */
    default: *si_code = 0x80; return 11;/* anything else -> SIGSEGV */
    }
}

/* A CPU fault taken in CPL 3. The fault path (x86_64_fault.S) saves the full
 * faulting user context into lproc_cur->tf and redirects its IRETQ to land here,
 * in ring 0 on this process's handler stack, with the trap vector and the
 * faulting address (cr2) as arguments. We were in user mode when the fault hit,
 * so no syscall held the giant lock; take it.
 *
 * If the process has a handler installed for the fault's signal and has not
 * blocked it, the handler runs (deliver_signal builds its frame from the saved
 * context, with the fault address in the siginfo), then we resume into it. A
 * synchronous fault that is ignored, defaulted, or blocked is forced to
 * terminate (you cannot resume past a faulting instruction), which sets exit_sig
 * so the parent's wait4 reports WIFSIGNALED. do_exit releases the lock and
 * switches away, so one process faulting no longer halts the guest. */
void linux_fault_entry(long vec, uint64_t addr) __attribute__((noreturn));
void
linux_fault_entry(long vec, uint64_t addr)
{
    int si_code = 0;
    int sig = fault_signal(vec, &si_code);
    if (vec != 14)
        addr = 0;                       /* cr2 is only meaningful for a page fault */

    giant_lock();
    struct lproc *t = lproc_cur;
    uint64_t h = t->sh->sigact[sig - 1].handler;
    int caught = (h != SIG_DFL && h != SIG_IGN) && !(t->sig_blocked & SIGBIT(sig));
    if (caught) {
        /* The faulting return register is the context's own; preserve it. */
        deliver_signal(sig, (long)tf_retval(&t->tf), 0, si_code, addr);
        giant_unlock();
        linux_resume(t);                /* enter the handler; does not return */
    }

    con_puts("[bin] pid ");
    put_dec(t->sh->pid);
    con_puts(" killed by signal ");
    put_dec(sig);
    con_puts(" (trap ");
    put_dec(vec);
    con_puts(" rip ");
    put_hex(tf_pc(&t->tf));
    con_puts(" addr ");
    put_hex(addr);
    con_puts(")\n");
    t->sh->exit_sig = sig;
    do_exit(0, 1);                      /* noreturn: releases the lock, tears down */
}

/****************************************************************
 * Signals: disposition and mask state, generation (kill, tgkill), the default
 * terminate action (which makes a process killable even while blocked in a
 * syscall), and caught-signal delivery (building a handler frame at the syscall
 * boundary, and rt_sigreturn to resume the interrupted code).
 ****************************************************************/

/* The default action for a signal: 1 to terminate the process, 0 to ignore.
 * We have no stop state, so the stop signals are treated as ignored. */
static int
sig_default_terminates(int s)
{
    switch (s) {
    case SIGCHLD:                       /* 17 */
    case SIGCONT:                       /* 18 */
    case SIGSTOP:                       /* 19: cannot stop, so a no-op */
    case 20: case 21: case 22:          /* SIGTSTP/TTIN/TTOU: no stop state */
    case SIGURG:                        /* 23 */
    case SIGWINCH:                      /* 28 */
        return 0;                       /* default action is ignore */
    default:
        return 1;                       /* term or core (we terminate for both) */
    }
}

/* The lowest pending, unblocked signal whose action is to terminate the
 * process, or 0 if none. Any pending signal that is ignored (SIG_IGN or a
 * default-ignore disposition) is consumed here. A caught signal (a handler is
 * installed) is left pending for deliver_signal, not acted on here. SIGKILL
 * ignores the block mask and the disposition. */
static int
take_fatal_signal(void)
{
    struct lproc *t = lproc_cur;
    struct lshared *sh = t->sh;
    uint64_t pending = sh->sig_pending | t->sig_pending;
    uint64_t ready = (pending & ~t->sig_blocked) | (pending & SIGBIT(SIGKILL));

    for (int s = 1; s <= NSIG; s++) {
        if (!(ready & SIGBIT(s)))
            continue;
        if (s != SIGKILL && s != SIGSTOP) {
            uint64_t h = sh->sigact[s - 1].handler;
            if (h == SIG_IGN) {                 /* explicitly ignored: consume */
                sh->sig_pending &= ~SIGBIT(s);
                t->sig_pending &= ~SIGBIT(s);
                continue;
            }
            if (h != SIG_DFL)                   /* caught: delivered by deliver_signal */
                continue;
        }
        if (!sig_default_terminates(s)) {       /* default-ignore: consume */
            sh->sig_pending &= ~SIGBIT(s);
            t->sig_pending &= ~SIGBIT(s);
            continue;
        }
        return s;                               /* default action is terminate */
    }
    return 0;
}

/* Act on a pending fatal signal by terminating the process. Called at the
 * syscall boundary and after each cooperative sleep, so a thread blocked in a
 * syscall becomes killable. Returns normally when nothing is pending. */
static void
signal_killpoint(void)
{
    struct lproc *t = lproc_cur;

    /* A group exit started by another thread (exit_group) tears this one down
     * too. The process status is already recorded, so exit this thread alone. */
    if (t->sh->exited && !t->exited)
        do_exit(0, 0);                  /* noreturn: releases the lock, tears down */

    int s = take_fatal_signal();
    if (s) {
        t->sh->exit_sig = s;
        do_exit(0, 1);                  /* noreturn: releases the lock, tears down */
    }
}

/* The lowest pending, unblocked signal that has a user handler installed (a
 * caught signal), or 0 if none. Unlike take_fatal_signal this neither consumes
 * nor terminates; it only reports what is ready for handler delivery. SIGKILL
 * and SIGSTOP are never caught. */
static int
next_caught_signal(void)
{
    struct lproc *t = lproc_cur;
    struct lshared *sh = t->sh;
    uint64_t pending = sh->sig_pending | t->sig_pending;
    uint64_t ready = pending & ~t->sig_blocked;

    for (int s = 1; s <= NSIG; s++) {
        if (!(ready & SIGBIT(s)))
            continue;
        if (s == SIGKILL || s == SIGSTOP)
            continue;
        uint64_t h = sh->sigact[s - 1].handler;
        if (h != SIG_DFL && h != SIG_IGN)
            return s;                   /* a real handler address: deliver it */
    }
    return 0;
}

/* Whether a caught signal is ready to run. Blocking syscalls poll this to break
 * out (with LERR_RESTARTSYS) so the handler runs at the dispatch boundary. */
static int
signal_deliverable(void)
{
    return next_caught_signal() != 0;
}

/* Deliver one signal: build a handler frame on the user stack and re-point this
 * thread's trapframe at the handler. `result` is the syscall result the
 * interrupted code would otherwise return (or the preserved register for a
 * fault); `restart` arranges the trapped syscall to re-run after the handler
 * (used for SA_RESTART). si_code/si_addr fill the siginfo: 0 for a sent signal
 * (SI_USER), a fault code plus the fault address for a CPU fault. A bad user
 * stack turns into a fatal SIGSEGV, matching Linux. */
static void
deliver_signal(int sig, long result, int restart, int si_code, uint64_t si_addr)
{
    struct lproc *t = lproc_cur;
    struct ksigaction *ka = &t->sh->sigact[sig - 1];
    uint64_t flags = ka->flags;

    /* Pick the stack: the alternate stack if the handler asked for it and one is
     * installed, otherwise the interrupted user stack. */
    uint64_t sp;
    int onstack = 0;
    uint64_t altsp = 0, altsize = 0;
    if ((flags & SA_ONSTACK) && t->ss_sp && !(t->ss_flags & SS_DISABLE)) {
        sp = t->ss_sp + t->ss_size;
        altsp = t->ss_sp;
        altsize = t->ss_size;
        onstack = 1;
    } else {
        sp = tf_user_sp(&t->tf);
    }

    uint64_t frame = arch_sigframe_place(sp, onstack);
    if (!user_ok(frame, arch_sigframe_size())) {
        t->sh->exit_sig = 11;           /* SIGSEGV: the handler stack is unusable */
        do_exit(0, 1);
    }

    arch_sigframe_build(frame, &t->tf, sig, ka->handler, flags, ka->restorer,
                        (uint64_t)result, restart, t->sig_blocked,
                        altsp, altsize, si_code, 0, 0, si_addr);

    /* Consume the signal and mask it (and the handler's sa_mask) for the handler,
     * unless SA_NODEFER. rt_sigreturn restores the saved mask. */
    t->sh->sig_pending &= ~SIGBIT(sig);
    t->sig_pending     &= ~SIGBIT(sig);
    t->sig_blocked |= ka->mask;
    if (!(flags & SA_NODEFER))
        t->sig_blocked |= SIGBIT(sig);
    t->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));

    if (flags & SA_RESETHAND)           /* one-shot: back to the default action */
        ka->handler = SIG_DFL;
}

/* The caught-signal half of the syscall-return boundary (the fatal half is
 * signal_killpoint). `r` is the dispatcher's result. If a handler is ready, this
 * builds its frame so the return path enters the handler instead of the user's
 * next instruction. It also resolves the LERR_RESTARTSYS marker a blocking
 * syscall may have returned: restart the call for an SA_RESTART handler, else
 * report EINTR. Returns the value to put in the return register (immaterial once
 * a handler frame is in place, since the real result is saved in the frame). */
static long
maybe_deliver_signal(long r)
{
    int sig = next_caught_signal();
    if (!sig) {
        /* Nothing to deliver. A restart marker with no handler (the signal was a
         * default/ignored one consumed by signal_killpoint) becomes EINTR so the
         * blocking call still returns to the user. */
        return r == -LERR_RESTARTSYS ? -LERR_INTR : r;
    }

    int restart = 0;
    long result = r;
    if (r == -LERR_RESTARTSYS) {
        if (lproc_cur->sh->sigact[sig - 1].flags & SA_RESTART)
            restart = 1;
        else
            result = -LERR_INTR;
    }
    deliver_signal(sig, result, restart, 0 /* SI_USER */, 0);
    /* The return path stores this into the syscall's return register. On arm64
     * that register (x0) is also the handler's first argument, so it must be the
     * signal number; arch_sigframe_build has saved the interrupted call's real
     * result into the frame for rt_sigreturn to restore. On x86 the handler's
     * argument is a separate register, so there this value is only scratch. */
    return sig;
}

/* rt_sigreturn: restore the context a handler frame saved and resume the
 * interrupted code. The restorer left the user sp at the ucontext. */
static long
do_rt_sigreturn(void)
{
    struct lproc *t = lproc_cur;
    uint64_t ucaddr = arch_sigframe_ucaddr(&t->tf);
    struct linux_ucontext uc;
    if (copy_from_user(&uc, ucaddr, sizeof uc) < 0) {
        t->sh->exit_sig = 11;           /* a corrupt frame is fatal */
        do_exit(0, 1);
    }
    uint64_t mask = 0;
    long rax = (long)arch_sigframe_restore(&t->tf, &uc, &mask);
    t->sig_blocked = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    return rax;                         /* the interrupted call's saved result */
}

/* Block until a caught signal is ready (pause). Returns EINTR after the handler
 * is arranged; pause is never restarted, even under SA_RESTART. */
static long
do_pause(void)
{
    while (!signal_deliverable()) {
        lx_block();
        signal_killpoint();             /* a fatal signal terminates here */
    }
    return -LERR_INTR;
}

/* Timed sleep (nanosleep, clock_nanosleep). The clock is the host-maintained
 * tick counter, so a sleep is a wait until the counter reaches a deadline tick.
 * The sleeper parks (lx_block, dropping the giant lock) and sleep_wake_hook,
 * registered as a scheduler event hook, wakes it once the deadline passes; the
 * hook runs on each timer tick (a ring-3 preemption) and whenever the scheduler
 * idles, and the host rouses an idle vcpu each tick so a fully asleep guest still
 * wakes on time. Without a host timer (hz 0) there is no tick to wait on, so a
 * sleep returns at once, as it did before. */
static uint64_t
ns_to_deadline_tick(uint64_t target_ns)
{
    uint32_t hz = boot_timer_hz();
    uint64_t per_tick_ns = 1000000000ULL / hz;      /* hz != 0 checked by caller */
    /* Round up so the counter reaching the deadline means at least target_ns of
     * clock time has elapsed. */
    return (target_ns + per_tick_ns - 1) / per_tick_ns;
}

/* ITIMER_REAL, the only interval timer supported. The value and interval are
 * carried as timer ticks in the process's lshared; getitimer/setitimer convert
 * to and from the itimerval {it_interval, it_value} the user passes, each a
 * struct timeval {tv_sec, tv_usec} (two 64-bit words), so the whole structure is
 * four words. */
#define LINUX_ITIMER_REAL 0

static uint64_t
tv_to_ticks(uint64_t sec, uint64_t usec)
{
    uint32_t hz = boot_timer_hz();
    if (hz == 0)
        return 0;                       /* no clock: nothing to arm against */
    return sec * hz + (usec * hz) / 1000000ULL;
}

static void
ticks_to_tv(uint64_t ticks, uint64_t *sec, uint64_t *usec)
{
    uint32_t hz = boot_timer_hz();
    if (hz == 0) {
        *sec = 0;
        *usec = 0;
        return;
    }
    *sec = ticks / hz;
    *usec = ((ticks % hz) * 1000000ULL) / hz;
}

static long
do_getitimer(int which, uint64_t ucur)
{
    if (which != LINUX_ITIMER_REAL)
        return -LERR_INVAL;             /* VIRTUAL/PROF are not supported */
    struct lshared *sh = lproc_cur->sh;
    uint64_t now = boot_timer_ticks();
    uint64_t rem = sh->itimer_real_deadline > now
                 ? sh->itimer_real_deadline - now : 0;
    uint64_t tv[4];
    ticks_to_tv(sh->itimer_real_interval, &tv[0], &tv[1]);
    ticks_to_tv(rem, &tv[2], &tv[3]);
    if (ucur && copy_to_user(ucur, tv, sizeof tv) < 0)
        return -LERR_FAULT;
    return 0;
}

static long
do_setitimer(int which, uint64_t unew, uint64_t uold)
{
    if (which != LINUX_ITIMER_REAL)
        return -LERR_INVAL;
    struct lshared *sh = lproc_cur->sh;
    uint64_t now = boot_timer_ticks();

    /* Report the previous setting first: a caller may pass the same buffer for
     * old and new, so the old value is read before the new one overwrites it. */
    if (uold) {
        uint64_t rem = sh->itimer_real_deadline > now
                     ? sh->itimer_real_deadline - now : 0;
        uint64_t tv[4];
        ticks_to_tv(sh->itimer_real_interval, &tv[0], &tv[1]);
        ticks_to_tv(rem, &tv[2], &tv[3]);
        if (copy_to_user(uold, tv, sizeof tv) < 0)
            return -LERR_FAULT;
    }
    if (unew) {
        uint64_t tv[4];
        if (copy_from_user(tv, unew, sizeof tv) < 0)
            return -LERR_FAULT;
        if (tv[1] >= 1000000ULL || tv[3] >= 1000000ULL)
            return -LERR_INVAL;         /* tv_usec out of range */
        sh->itimer_real_interval = tv_to_ticks(tv[0], tv[1]);
        if ((tv[2] | tv[3]) == 0) {
            sh->itimer_real_deadline = 0;   /* it_value zero disarms the timer */
        } else {
            uint64_t v = tv_to_ticks(tv[2], tv[3]);
            sh->itimer_real_deadline = now + (v ? v : 1);  /* at least one tick */
        }
    }
    return 0;
}

/* Wake any thread whose sleep deadline has passed, and fire ITIMER_REAL. Runs
 * from the event ISR and at scheduler idle. */
static void
sleep_wake_hook(void)
{
    uint64_t now = boot_timer_ticks();
    for (int i = 0; i < LINUX_NPROC; i++) {
        struct lproc *t = &procs[i];
        if (t->used && t->sleep_deadline && now >= t->sleep_deadline)
            task_wake(t->task_id);
    }
    /* Raise SIGALRM on a process whose interval timer has elapsed, then re-arm
     * by its interval (or disarm a one-shot). Waking the main thread lets a
     * blocking syscall return so the signal is delivered at the boundary. */
    for (int i = 0; i < LINUX_NPROC; i++) {
        struct lshared *sh = &shareds[i];
        if (sh->used && sh->itimer_real_deadline &&
            now >= sh->itimer_real_deadline) {
            sh->sig_pending |= SIGBIT(SIGALRM);
            if (sh->itimer_real_interval)
                sh->itimer_real_deadline += sh->itimer_real_interval;
            else
                sh->itimer_real_deadline = 0;
            task_wake(sh->main_task);
        }
    }
}

/* Block until the clock reaches target_ns (in the monotonic nanosecond base), or
 * return 0 at once if there is no host timer. Returns 0 on completion; a fatal
 * signal terminates the thread inside the loop and never returns. */
static long
sleep_until_ns(uint64_t target_ns)
{
    if (boot_timer_hz() == 0)
        return 0;                       /* no clock to wait on */
    uint64_t deadline = ns_to_deadline_tick(target_ns);
    if (boot_timer_ticks() >= deadline)
        return 0;                       /* already elapsed */

    lproc_cur->sleep_deadline = deadline;
    while (boot_timer_ticks() < deadline) {
        lx_block();                     /* drops the giant lock while parked */
        signal_killpoint();             /* a fatal signal unblocks and terminates */
        if (signal_deliverable()) {     /* a caught signal cuts the sleep short */
            lproc_cur->sleep_deadline = 0;
            return -LERR_RESTARTSYS;
        }
    }
    lproc_cur->sleep_deadline = 0;
    return 0;
}

/* nanosleep / clock_nanosleep. relative requests (and nanosleep) sleep for req
 * past now; an absolute (TIMER_ABSTIME) request sleeps until req on its clock.
 * rem is left untouched: the only wakeups are the deadline or a fatal signal
 * (which terminates), so a short count is never reported. */
#define LINUX_TIMER_ABSTIME 1
static long
do_nanosleep(int clockid, int flags, uint64_t req_ptr)
{
    if (!req_ptr)
        return -LERR_INVAL;
    uint64_t ts[2];
    long e = copy_from_user(ts, req_ptr, sizeof ts);
    if (e < 0)
        return e;
    if (ts[1] >= 1000000000ULL)
        return -LERR_INVAL;
    uint64_t req_ns = ts[0] * 1000000000ULL + ts[1];

    uint64_t target_ns;
    if (flags & LINUX_TIMER_ABSTIME) {
        /* Absolute: req is on the named clock. Convert a realtime deadline into
         * the monotonic base the tick counter uses. */
        int realtime = (clockid == 0);          /* CLOCK_REALTIME */
        uint64_t epoch_ns = guest_epoch() * 1000000000ULL;
        target_ns = (realtime && req_ns > epoch_ns) ? req_ns - epoch_ns : req_ns;
    } else {
        target_ns = guest_mono_ns() + req_ns;
    }
    return sleep_until_ns(target_ns);
}

/* Find a live process by pid, or NULL. A zombie (exited, not yet reaped) still
 * counts as existing, so a signal to it is accepted and dropped. */
static struct lshared *
proc_by_pid(int pid)
{
    for (int i = 0; i < LINUX_NPROC; i++)
        if (shareds[i].used && shareds[i].pid == pid)
            return &shareds[i];
    return NULL;
}

/* Find a live thread by tid, or NULL. */
static struct lproc *
thread_by_tid(int tid)
{
    for (int i = 0; i < LINUX_NPROC; i++)
        if (procs[i].used && procs[i].tid == tid)
            return &procs[i];
    return NULL;
}

/* Wake every live thread of a process so a blocked one re-checks its signals. */
static void
wake_process_threads(const struct lshared *sh)
{
    for (int i = 0; i < LINUX_NPROC; i++)
        if (procs[i].used && !procs[i].exited && procs[i].sh == sh)
            task_wake(procs[i].task_id);
}

/* kill(pid, sig): post a process-directed signal. sig 0 is an existence probe.
 * Only pid > 0 (a specific process) is supported; process-group forms are
 * accepted as a no-op. */
static long
do_kill(int pid, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -LERR_INVAL;
    if (pid <= 0)
        return 0;                       /* group/broadcast forms: accept, no-op */
    struct lshared *sh = proc_by_pid(pid);
    if (!sh)
        return -LERR_SRCH;
    if (sig == 0)
        return 0;                       /* existence check only */
    sh->sig_pending |= SIGBIT(sig);
    wake_process_threads(sh);           /* a blocked thread re-checks and may die */
    return 0;
}

/* tgkill(tgid, tid, sig): post a thread-directed signal (raise and abort route
 * here). tgid is accepted but not cross-checked against the thread's process. */
static long
do_tgkill(int tid, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -LERR_INVAL;
    struct lproc *t = thread_by_tid(tid);
    if (!t || t->exited)
        return -LERR_SRCH;
    if (sig == 0)
        return 0;
    t->sig_pending |= SIGBIT(sig);
    task_wake(t->task_id);
    return 0;
}

/* rt_sigaction(sig, act, oldact): store or retrieve a disposition. The x86-64
 * kernel struct sigaction is { handler, flags, restorer, mask }, matching
 * struct ksigaction. SIGKILL and SIGSTOP cannot be changed. */
static long
do_rt_sigaction(int sig, uint64_t act, uint64_t oldact)
{
    if (sig < 1 || sig > NSIG)
        return -LERR_INVAL;
    struct ksigaction *slot = &lproc_cur->sh->sigact[sig - 1];
    if (oldact) {
        if (copy_to_user(oldact, slot, sizeof *slot) < 0)
            return -LERR_FAULT;
    }
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return -LERR_INVAL;
        struct ksigaction ka;
        if (copy_from_user(&ka, act, sizeof ka) < 0)
            return -LERR_FAULT;
        *slot = ka;
    }
    return 0;
}

/* rt_sigprocmask(how, set, oldset): update the thread's blocked mask. SIGKILL
 * and SIGSTOP are never actually blocked. Only the low 64 bits matter. */
static long
do_rt_sigprocmask(int how, uint64_t set, uint64_t oldset)
{
    struct lproc *t = lproc_cur;
    if (oldset) {
        if (copy_to_user(oldset, &t->sig_blocked, sizeof t->sig_blocked) < 0)
            return -LERR_FAULT;
    }
    if (set) {
        uint64_t m;
        if (copy_from_user(&m, set, sizeof m) < 0)
            return -LERR_FAULT;
        switch (how) {
        case 0: t->sig_blocked |= m; break;             /* SIG_BLOCK */
        case 1: t->sig_blocked &= ~m; break;            /* SIG_UNBLOCK */
        case 2: t->sig_blocked = m; break;              /* SIG_SETMASK */
        default: return -LERR_INVAL;
        }
        t->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    return 0;
}

/* sigaltstack(ss, old): store the alternate signal stack, used once handler
 * delivery lands. Accepted and recorded now. */
static long
do_sigaltstack(uint64_t ss, uint64_t old)
{
    struct lproc *t = lproc_cur;
    if (old) {
        struct { uint64_t sp; int flags; uint64_t size; } o;
        memset(&o, 0, sizeof o);        /* clear the padding hole before the copy */
        o.sp = t->ss_sp;
        o.flags = t->ss_flags;
        o.size = t->ss_size;
        if (copy_to_user(old, &o, sizeof o) < 0)
            return -LERR_FAULT;
    }
    if (ss) {
        struct { uint64_t sp; int flags; uint64_t size; } n;
        if (copy_from_user(&n, ss, sizeof n) < 0)
            return -LERR_FAULT;
        t->ss_sp = n.sp;
        t->ss_flags = n.flags;
        t->ss_size = n.size;
    }
    return 0;
}

/* rt_sigsuspend(mask, sigsetsize): install mask as the blocked set, block until
 * a caught signal is deliverable, then return EINTR with the handler poised to
 * run at the syscall-return boundary (maybe_deliver_signal sees it under this
 * mask). A shell waiting for a background child suspends here with SIGCHLD
 * unblocked, so the SIGCHLD posted when the child exits wakes it and runs the
 * handler. The suspend mask is left in effect (rt_sigreturn keeps it across the
 * handler); callers that care, like dash, reset their mask explicitly on return,
 * which matches how they bracket the call. */
static long
do_rt_sigsuspend(uint64_t umask, uint64_t sigsetsize)
{
    if (sigsetsize != sizeof(uint64_t))
        return -LERR_INVAL;
    struct lproc *t = lproc_cur;
    uint64_t newmask;
    if (copy_from_user(&newmask, umask, sizeof newmask) < 0)
        return -LERR_FAULT;

    t->sig_blocked = newmask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    while (!signal_deliverable()) {
        lx_block();
        signal_killpoint();             /* a fatal signal terminates here */
    }
    return -LERR_INTR;
}

/* The body of the SYSCALL handler, run with the giant lock held (see
 * linux_sys_dispatch). Return value goes back in rax. */
static long
linux_sys_dispatch_inner(long nr, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;
    reap_zombie_threads();              /* free the slots of any exited threads */
    switch (nr) {
    case SYS_arch_prctl:
        if (a1 == ARCH_SET_FS) {
            lproc_cur->tls = (uint64_t)a2;          /* restored on a switch */
            arch_load_tls((uint64_t)a2);
            return 0;
        }
        return -LERR_NOSYS;
    case SYS_set_tid_address:
        lproc_cur->clear_ctid = (uint64_t)a1;   /* cleared and woken on exit */
        return lproc_cur->tid;
    case SYS_set_robust_list:
    case SYS_rseq:
        return 0;                       /* accept and ignore */
    case SYS_mprotect:
    case SYS_madvise:
    case SYS_pkey_mprotect:             /* no protection keys: ignore the key */
        return 0;                       /* flat RW map, nothing to enforce */
    case SYS_pkey_alloc:
        return -LERR_NOSYS;             /* no PKU; callers fall back to mprotect */
    case SYS_io_uring_setup:
        return -LERR_NOSYS;             /* no io_uring; libuv falls back to epoll */
    case SYS_pkey_free:
        return 0;
    case SYS_eventfd:
        return do_eventfd((unsigned)a1, 0);
    case SYS_eventfd2:
        return do_eventfd((unsigned)a1, (int)a2);
    case SYS_capget:
        return do_capget((uint64_t)a2);
    case SYS_clock_getres:
        return do_clock_getres((uint64_t)a2);
    case SYS_fadvise64:
        return 0;                       /* advisory only; accept and ignore */
    case SYS_copy_file_range:
        return -LERR_NOSYS;             /* cp/mv fall back to a read/write loop */
    case SYS_brk:
        return do_brk((uint64_t)a1);
    case SYS_mmap:
        /* The 6th argument (offset) is not in the dispatcher's register set;
         * read it from the saved trapframe. */
        return do_mmap((uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4,
                       a5, tf_arg6(&lproc_cur->tf));
    case SYS_munmap:
        return do_munmap((uint64_t)a1, (uint64_t)a2);
    case SYS_time: {
        int64_t t = (int64_t)guest_now_sec();
        if (a1) {
            long e = copy_to_user((uint64_t)a1, &t, sizeof t);
            if (e < 0)
                return e;
        }
        return (long)t;
    }
    case SYS_clock_gettime: {
        /* The host preemption timer drives a real wall clock; without it this is
         * the cycle counter read as nanoseconds (approximate). It shares the
         * epoch with file mtimes (both the boot epoch + elapsed), so a fresh
         * build file is never in the future. A MONOTONIC request gets uptime from
         * the same source without the epoch offset. */
        uint64_t ns = guest_mono_ns();
        int monotonic = (a1 == 1 || a1 == 4);   /* MONOTONIC, MONOTONIC_RAW */
        uint64_t base = monotonic ? 0 : guest_epoch();
        if (a2) {                         /* struct timespec { sec; nsec; } */
            uint64_t ts[2];
            ts[0] = base + ns / 1000000000ULL;
            ts[1] = ns % 1000000000ULL;
            long e = copy_to_user((uint64_t)a2, ts, sizeof ts);
            if (e < 0)
                return e;
        }
        return 0;
    }
    case SYS_gettimeofday: {
        uint64_t ns = guest_mono_ns();
        if (a1) {                         /* struct timeval { sec; usec; } */
            uint64_t tv[2];
            tv[0] = guest_epoch() + ns / 1000000000ULL;
            tv[1] = (ns / 1000ULL) % 1000000ULL;
            long e = copy_to_user((uint64_t)a1, tv, sizeof tv);
            if (e < 0)
                return e;
        }
        return 0;
    }
    case SYS_socket:
        /* Only AF_UNIX is backed; do_socket returns ENOSYS for anything else,
         * which is the "not a socket" answer gcc's getpeername probe expects. */
        return do_socket((int)a1, (int)a2, (int)a3);
    case SYS_bind:
        return do_bind(a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_listen:
        return do_listen(a1, (int)a2);
    case SYS_connect:
        return do_connect(a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_accept:
        return do_accept(a1, (uint64_t)a2, (uint64_t)a3, 0);
    case SYS_accept4:
        return do_accept(a1, (uint64_t)a2, (uint64_t)a3, (int)a4);
    case SYS_getsockname:
        return do_getsockname(a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_getpeername:
        return do_getpeername(a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_shutdown:
        return do_shutdown(a1, (int)a2);
    case SYS_setsockopt:
        return do_setsockopt(a1, (uint64_t)a4, (uint64_t)a5);
    case SYS_getsockopt:
        return do_getsockopt(a1, (uint64_t)a4, (uint64_t)a5);
    case SYS_sendto: {
        struct lfd *f = fd_get(a1);
        if (!f)
            return -LERR_BADF;
        if (f->kind != FD_SOCK)
            return -LERR_NOTSOCK;
        if (a3 && !user_ok((uint64_t)a2, (unsigned long)a3))
            return -LERR_FAULT;
        return sock_write(f, (const void *)a2, (unsigned long)a3);
    }
    case SYS_recvfrom: {
        struct lfd *f = fd_get(a1);
        if (!f)
            return -LERR_BADF;
        if (f->kind != FD_SOCK)
            return -LERR_NOTSOCK;
        if (a3 && !user_ok((uint64_t)a2, (unsigned long)a3))
            return -LERR_FAULT;
        return sock_read(f, (void *)a2, (unsigned long)a3);
    }
    case SYS_getrandom: {
        uint8_t *buf = (uint8_t *)a1;
        unsigned long len = (unsigned long)a2;
        if (len && !user_ok((uint64_t)a1, len))
            return -LERR_FAULT;
        uint64_t s = arch_cycles();
        for (unsigned long i = 0; i < len; i++) {
            buf[i] = (uint8_t)s;
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        }
        return (long)len;               /* not cryptographic; a startup seed */
    }
    case SYS_prlimit64: {
        /* prlimit64(pid, resource, new, old): report the descriptor cap
         * truthfully for RLIMIT_NOFILE(7) so a caller sizes fd arrays and
         * targets a valid high fd (bash moves its script fd just below this
         * limit); everything else is unbounded. The new limit is ignored. */
        if (a4) {                         /* old_limit: {rlim_cur, rlim_max} */
            uint64_t old[2];
            if (a2 == 7) {                /* RLIMIT_NOFILE */
                old[0] = LINUX_NFD;
                old[1] = LINUX_NFD;
            } else {
                old[0] = ~0ULL;           /* RLIM_INFINITY */
                old[1] = ~0ULL;
            }
            long e = copy_to_user((uint64_t)a4, old, sizeof old);
            if (e < 0)
                return e;
        }
        return 0;
    }
    case SYS_ioctl: {
        /* A console descriptor gets the tty requests (so isatty and a shell's
         * line-editor setup work); everything else is still "not a tty". */
        struct lfd *f = fd_get(a1);
        if (f && f->kind == FD_CON)
            return con_ioctl((unsigned long)a2, (uint64_t)a3);
        return -LERR_NOTTY;
    }
    case SYS_poll:
        return do_poll((uint64_t)a1, (uint64_t)a2, (long)a3);
    case SYS_ppoll:
        return do_ppoll((uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4);
    case SYS_epoll_create:
        return do_epoll_create1(0);     /* legacy; the size hint is ignored */
    case SYS_epoll_create1:
        return do_epoll_create1((long)a1);
    case SYS_epoll_ctl:
        return do_epoll_ctl((long)a1, (long)a2, (long)a3, (uint64_t)a4);
    case SYS_epoll_wait:
        return do_epoll_wait((long)a1, (uint64_t)a2, (long)a3, (long)a4);
    case SYS_epoll_pwait: {
        /* Apply the caller's signal mask for the wait, as ppoll does, so a
         * signal it unblocks can interrupt epoll_wait; restore it after. */
        uint64_t saved = lproc_cur->sig_blocked;
        int swapped = 0;
        if (a5) {
            uint64_t m;
            if (copy_from_user(&m, (uint64_t)a5, sizeof m) < 0)
                return -LERR_FAULT;
            lproc_cur->sig_blocked = m;
            swapped = 1;
        }
        long r = do_epoll_wait((long)a1, (uint64_t)a2, (long)a3, (long)a4);
        if (swapped)
            lproc_cur->sig_blocked = saved;
        return r;
    }
    case SYS_read:
        return do_read(a1, (void *)a2, (unsigned long)a3);
    case SYS_pread64:
        return do_pread(a1, (void *)a2, (unsigned long)a3, (uint64_t)a4);
    case SYS_pwrite64:
        return do_pwrite(a1, (const void *)a2, (unsigned long)a3, (uint64_t)a4);
    case SYS_splice:
        return do_splice(a1, (uint64_t)a2, a3, (uint64_t)a4,
                         (unsigned long)a5, (unsigned int)tf_arg6(&lproc_cur->tf));
    case SYS_sendfile:
        return do_sendfile(a1, a2, (uint64_t)a3, (unsigned long)a4);
    case SYS_write:
        return do_write(a1, (const char *)a2, (unsigned long)a3);
    case SYS_writev:
        return do_writev(a1, (const struct iovec *)a2, a3);
    case SYS_open: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        long fd = do_open(up, a2, a3);
        if (fd >= 0 && (a2 & O_CLOEXEC))
            fds[fd].cloexec = 1;
        return fd;
    }
    case SYS_openat: {
        /* A relative path is taken against dirfd's directory (AT_FDCWD means the
         * cwd); an absolute path ignores dirfd. */
        char pb[UPATH_MAX], ab[RAMFS_NAME];
        long e;
        const char *up = user_path((uint64_t)a2, pb, &e);
        if (!up)
            return e;
        const char *p = at_path(a1, up, ab, sizeof ab, &e);
        if (e)
            return e;
        long fd = do_open(p, a3, a4);
        if (fd >= 0 && (a3 & O_CLOEXEC))
            fds[fd].cloexec = 1;
        return fd;
    }
    case SYS_close:
        return do_close(a1);
    case SYS_close_range: {
        /* close_range(first, last, flags): close every open descriptor in
         * [first, last]. glibc's posix_spawn and CPython's subprocess use it in
         * the child to drop inherited descriptors before exec; without it the
         * child keeps a pipe's far end open, so the parent's close never yields
         * EOF and a reader like "gcc -E -" blocks on stdin forever. CLOSE_RANGE_
         * CLOEXEC (flags bit 0) would only mark them close-on-exec, but a plain
         * close is correct here and the child execs immediately. */
        uint64_t first = (uint64_t)a1, last = (uint64_t)a2;
        if (first > last)
            return -LERR_INVAL;
        if (last >= LINUX_NFD)
            last = LINUX_NFD - 1;
        for (uint64_t fd = first; fd <= last; fd++)
            if (fds[fd].used)
                do_close((long)fd);
        return 0;
    }
    case SYS_lseek:
        return do_lseek(a1, a2, a3);
    case SYS_stat:
    case SYS_lstat: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        return do_stat_path(up, (struct linux_stat *)a2, nr == SYS_stat);
    }
    case SYS_newfstatat: {
        /* An empty path with AT_EMPTY_PATH means the dirfd itself (fstat);
         * otherwise resolve the path against dirfd. */
        char pb[UPATH_MAX], ab[RAMFS_NAME];
        long e;
        const char *up = user_path((uint64_t)a2, pb, &e);
        if (!up)
            return e;
        if (up[0] == 0 && (a4 & AT_EMPTY_PATH))
            return do_fstat(a1, (struct linux_stat *)a3);
        const char *p = at_path(a1, up, ab, sizeof ab, &e);
        if (e)
            return e;
        return do_stat_path(p, (struct linux_stat *)a3,
                            !(a4 & AT_SYMLINK_NOFOLLOW));
    }
    case SYS_fstat:
        return do_fstat(a1, (struct linux_stat *)a2);
    case SYS_statx: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a2, pb, &e);
        if (!up)
            return e;
        return do_statx(a1, up, a3, (struct linux_statx *)a5);
    }
    case SYS_getdents64:
        return do_getdents64(a1, (void *)a2, (unsigned long)a3);
    case SYS_pipe:
        return do_pipe2((int *)a1, 0);
    case SYS_pipe2:
        return do_pipe2((int *)a1, (int)a2);    /* O_CLOEXEC honored; O_NONBLOCK ignored */
    case SYS_setxattr:
    case SYS_lsetxattr:
    case SYS_fsetxattr:
    case SYS_getxattr:
    case SYS_lgetxattr:
    case SYS_fgetxattr:
        /* No extended attributes. ENOTSUP is what a filesystem without xattr
         * support returns, and it is what cp and install expect: they then copy
         * the file without its attributes instead of failing. Quiet, like the
         * other feature probes below. */
        return -LERR_NOTSUP;
    case SYS_statfs:
    case SYS_fstatfs:
    case SYS_mincore:
        /* Nothing to describe (no real filesystem; no page residency to report).
         * Report ENOSYS quietly: callers such as libselinux probing a filesystem
         * type, or a residency hint, fall back cleanly, and a logged "unhandled"
         * line here would pollute a tool's output stream. */
        return -LERR_NOSYS;
    case SYS_mremap:
        /* glibc realloc grows a large (mmap-backed) block with mremap. Our mmap
         * is a non-reclaiming bump allocator, so a real mremap would just copy
         * into a fresh region exactly as glibc's own fallback does on ENOSYS.
         * Report ENOSYS quietly and let glibc do the copy. */
        return -LERR_NOSYS;
    case SYS_getgroups:
        return 0;                       /* no supplementary groups */
    case SYS_getpgrp:
        return 1;                       /* a single process group */
    case SYS_getrusage: {
        /* usage is the second argument; who (a1) is ignored. Zeroed counters:
         * no CPU-time accounting, which gcc and as query but tolerate. */
        if (a2) {                               /* struct rusage is 18 longs */
            long e = clear_user((uint64_t)a2, 18 * sizeof(uint64_t));
            if (e < 0)
                return e;
        }
        return 0;
    }
    case SYS_sysinfo: {
        /* Report the configured RAM as total and a fixed free fraction, zero for
         * everything else. cc1 reads this to size its garbage collector; the
         * numbers need only be plausible. struct sysinfo layout: uptime, three
         * load averages, totalram, freeram, then fields this leaves zero. */
        if (a1) {
            uint64_t si[14];
            for (int i = 0; i < 14; i++)
                si[i] = 0;
            si[4] = VM_RAM_SIZE;                /* totalram */
            si[5] = VM_RAM_SIZE / 2;            /* freeram (a plausible fraction) */
            long e = copy_to_user((uint64_t)a1, si, sizeof si);
            if (e < 0)
                return e;
        }
        return 0;
    }
    case SYS_sigaltstack:
        return do_sigaltstack((uint64_t)a1, (uint64_t)a2);
    case SYS_umask:
        return 022;                     /* report a conventional prior mask */
    case SYS_chmod:
    case SYS_fchmod:
    case SYS_fchmodat:
        return 0;                       /* no permission bits to enforce */
    case SYS_utimensat:
        /* The path buffers live in the helper's own frame, not this giant
         * dispatch function's (-O0 sums every case's locals onto one frame, so
         * an extra kilobyte here can overflow the syscall kstack under a
         * preemption interrupt). */
        return do_utimensat(a1, (uint64_t)a2);
    case SYS_dup: {
        struct lfd *f = fd_get(a1);
        if (!f || a1 < 3)
            return -LERR_BADF;          /* console dup not needed here */
        int nfd = fd_alloc();
        if (nfd < 0)
            return -LERR_NOMEM;
        clone_fd(nfd, f);
        return nfd;
    }
    case SYS_dup2:
    case SYS_dup3: {
        struct lfd *f = fd_get(a1);
        if (!f)
            return -LERR_BADF;
        if (a2 < 0 || a2 >= LINUX_NFD)
            return -LERR_BADF;          /* a2 may be 0/1/2: a shell redirect */
        if (a1 == a2)
            return (nr == SYS_dup3) ? -LERR_INVAL : a2;
        if (fds[a2].used)
            do_close(a2);               /* release the old target, std fd or not:
                                         * a std fd may hold a refcounted pipe (a
                                         * pipeline stage's stdout), and dropping
                                         * it without a close leaks that writer so
                                         * the reader never sees EOF */
        clone_fd((int)a2, f);
        if (nr == SYS_dup3 && (a3 & O_CLOEXEC))
            fds[a2].cloexec = 1;        /* dup3 can set close-on-exec; dup2 clears it */
        return a2;
    }
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        return 0;                       /* a single root-ish identity */
    case SYS_setuid:
    case SYS_setgid:
    case SYS_setreuid:
    case SYS_setregid:
    case SYS_setresuid:
    case SYS_setresgid:
    case SYS_setgroups:
    case SYS_setpgid:
        /* One identity and one process group; a set to it succeeds, and there
         * is nothing else to become. posix_spawn's child sets these before it
         * execs the recipe shell. */
        return 0;
    case SYS_setsid:
        return lproc_cur->sh->pid;      /* the new session id is our pid */
    case SYS_prctl:
        return 0;                       /* accept and ignore */
    case SYS_getcwd: {
        /* Return the absolute cwd: a leading slash then the stored key. The
         * syscall's return value is the length including the NUL terminator. */
        size_t klen = strlen(cwd_key);
        size_t need = 1 + klen + 1;         /* '/' + key + NUL */
        if (a2 < need)
            return -LERR_RANGE;
        char kbuf[1 + RAMFS_NAME + 1];
        kbuf[0] = '/';
        memcpy(kbuf + 1, cwd_key, klen);
        kbuf[1 + klen] = 0;
        long e = copy_to_user((uint64_t)a1, kbuf, need);
        if (e < 0)
            return e;
        return (long)need;
    }
    case SYS_chdir: {
        /* Move the process's working directory. Resolve the target against the
         * current cwd, require that it is a directory, and store its key. The
         * root clears the key. */
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        char key[RAMFS_NAME];
        resolve_path(up, key, sizeof key);
        if (key[0] == 0) {
            cwd_key[0] = 0;             /* the root */
            return 0;
        }
        int isdir;
        uint64_t size, ino;
        if (meta_key(key, &isdir, &size, &ino, NULL) < 0)
            return -LERR_NOENT;
        if (!isdir)
            return -LERR_NOTDIR;
        size_t i = 0;
        while (key[i] && i < sizeof cwd_key - 1) {
            cwd_key[i] = key[i];
            i++;
        }
        cwd_key[i] = 0;
        return 0;
    }
    case SYS_fchdir: {
        /* Move the working directory to a directory already open on a fd. Its
         * normalized key is kept on the open-file description (dpath), so this
         * just copies it in. coreutils walk trees this way (mkdir -p, rm -rf
         * via fts), so without it rm -rf cannot descend to empty a directory
         * and rmdir fails. */
        struct lfd *f = fd_get(a1);
        if (!f)
            return -LERR_BADF;
        if (f->kind != FD_DIR || !f->of)
            return -LERR_NOTDIR;
        size_t i = 0;
        while (f->of->dpath[i] && i < sizeof cwd_key - 1) {
            cwd_key[i] = f->of->dpath[i];
            i++;
        }
        cwd_key[i] = 0;
        return 0;
    }
    case SYS_link:
    case SYS_linkat:
        /* Hard links are still unsupported. Report EPERM ("the filesystem does
         * not support this"), which configure and install scripts test for:
         * they fall back to copying. */
        return -LERR_PERM;
    case SYS_symlink:
    case SYS_symlinkat: {
        /* symlink(target, linkpath): a1=target, a2=linkpath.
         * symlinkat(target, newdirfd, linkpath): a1=target, a2=newdirfd,
         * a3=linkpath. Only the writable overlay takes a new link. Node's
         * configure points out/tools/bin/python at the real interpreter. */
        char tb[UPATH_MAX], lb[UPATH_MAX], lab[RAMFS_NAME];
        long et, el;
        const char *target = user_path((uint64_t)a1, tb, &et);
        if (!target)
            return et;
        const char *lp = user_path(nr == SYS_symlink ? (uint64_t)a2 : (uint64_t)a3,
                                   lb, &el);
        if (!lp)
            return el;
        const char *linkpath = lp;
        if (nr == SYS_symlinkat) {
            linkpath = at_path(a2, lp, lab, sizeof lab, &el);
            if (el)
                return el;
        }
        return do_symlink(target, linkpath);
    }
    case SYS_readlink:
    case SYS_readlinkat: {
        /* readlink(path, buf, bufsiz): a1,a2,a3.
         * readlinkat(dirfd, path, buf, bufsiz): a1=dirfd, a2..a4. Only overlay
         * symlinks resolve; bundle entries are never links, so gcc's probe of
         * /proc/self/exe still falls back to its default prefix. */
        char pb[UPATH_MAX], pab[RAMFS_NAME];
        long e;
        const char *up = user_path(nr == SYS_readlink ? (uint64_t)a1 : (uint64_t)a2,
                                   pb, &e);
        if (!up)
            return e;
        const char *path = up;
        if (nr == SYS_readlinkat) {
            path = at_path(a1, up, pab, sizeof pab, &e);
            if (e)
                return e;
        }
        if (nr == SYS_readlink)
            return do_readlink(path, (char *)a2, a3);
        return do_readlink(path, (char *)a3, a4);
    }
    case SYS_unlink:
    case SYS_unlinkat: {
        /* unlink(path): path is a1. unlinkat(dirfd, path, flags): path is a2,
         * dirfd is treated as AT_FDCWD. AT_REMOVEDIR (0x200) routes to rmdir.
         * Only the writable overlay can be removed; the 9P bundle and host
         * mounts are read-only. ld, configure, and make all unlink temporaries,
         * which live in the overlay. */
        char pb[UPATH_MAX], uab[RAMFS_NAME];
        long e;
        const char *up = user_path(nr == SYS_unlink ? (uint64_t)a1 : (uint64_t)a2,
                                   pb, &e);
        if (!up)
            return e;
        const char *upath;
        if (nr == SYS_unlink)
            upath = up;
        else {
            upath = at_path(a1, up, uab, sizeof uab, &e);
            if (e)
                return e;
        }
        if (nr == SYS_unlinkat && (a3 & 0x200))
            return do_rmdir(upath);
        char ukey[RAMFS_NAME];
        resolve_path(upath, ukey, sizeof ukey);
        if (ramfs_remove(ukey) == 0)
            return 0;
        int isdir;
        uint64_t size, ino;
        if (under_rw_mount(ukey)) {     /* land the removal on the host mount */
            if (meta_key(ukey, &isdir, &size, &ino, NULL) != 0)
                return -LERR_NOENT;
            return isdir ? -LERR_ISDIR : remove_9p(ukey);
        }
        if (meta_path(upath, &isdir, &size, &ino, NULL) == 0)
            return isdir ? -LERR_ISDIR : -LERR_ROFS;
        return -LERR_NOENT;
    }
    case SYS_mkdir: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        return do_mkdir(up);
    }
    case SYS_mkdirat: {
        char pb[UPATH_MAX], ab[RAMFS_NAME];
        long e;
        const char *up = user_path((uint64_t)a2, pb, &e);
        if (!up)
            return e;
        const char *p = at_path(a1, up, ab, sizeof ab, &e);
        if (e)
            return e;
        return do_mkdir(p);
    }
    case SYS_rmdir: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        return do_rmdir(up);
    }
    case SYS_rename: {
        char ob[UPATH_MAX], nb[UPATH_MAX];
        long eo, en;
        const char *op = user_path((uint64_t)a1, ob, &eo);
        if (!op)
            return eo;
        const char *np = user_path((uint64_t)a2, nb, &en);
        if (!np)
            return en;
        return do_rename(op, np);
    }
    case SYS_renameat:
    case SYS_renameat2: {       /* (olddirfd, old, newdirfd, new); flags ignored */
        char ob[UPATH_MAX], nb[UPATH_MAX], oab[RAMFS_NAME], nab[RAMFS_NAME];
        long eo, en;
        const char *ou = user_path((uint64_t)a2, ob, &eo);
        if (!ou)
            return eo;
        const char *nu = user_path((uint64_t)a4, nb, &en);
        if (!nu)
            return en;
        const char *op = at_path(a1, ou, oab, sizeof oab, &eo);
        const char *np = at_path(a3, nu, nab, sizeof nab, &en);
        if (eo)
            return eo;
        if (en)
            return en;
        return do_rename(op, np);
    }
    case SYS_access: {
        /* access(path, mode): path is the first argument. The dynamic linker
         * probes /etc/ld.so.cache and candidate library directories this way. */
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        int isdir;
        uint64_t size, ino;
        long r = meta_path(up, &isdir, &size, &ino, NULL);
        return r < 0 ? r : 0;
    }
    case SYS_faccessat:
    case SYS_faccessat2: {
        /* faccessat(dirfd, path, mode, ...): the bundle and ramfs carry no
         * permission bits, so an existing path is accessible for any mode.
         * Paths are resolved against the bundle root; dirfd is AT_FDCWD in
         * practice. make checks its shell this way before execve. */
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a2, pb, &e);
        if (!up)
            return e;
        int isdir;
        uint64_t size, ino;
        long r = meta_path(up, &isdir, &size, &ino, NULL);
        return r < 0 ? r : 0;
    }
    case SYS_fcntl: {
        /* Every command first validates the fd: a closed fd is EBADF. This
         * matters because a shell probes whether a descriptor is open with
         * F_GETFD before redirecting onto it; a blanket success made bash think
         * a closed fd (e.g. 6 in `exec 6>&1`) was open, then fail to save it.
         * F_DUPFD (0) and F_DUPFD_CLOEXEC (1030) duplicate to the lowest free fd
         * >= a3 (used to save and restore a redirected fd). Other commands
         * (F_GETFD/F_SETFD/F_GETFL/F_SETFL) are accepted with a benign value. */
        struct lfd *src = fd_get(a1);
        if (!src)
            return -LERR_BADF;
        if (a2 == 0 || a2 == 1030) {
            int lo = a3 < 0 ? 0 : (int)a3;
            for (int i = lo; i < LINUX_NFD; i++) {
                if (!fds[i].used) {
                    clone_fd(i, src);
                    if (a2 == 1030)     /* F_DUPFD_CLOEXEC sets close-on-exec */
                        fds[i].cloexec = 1;
                    return i;
                }
            }
            return -LERR_NOMEM;         /* EMFILE: no free descriptor */
        }
        if (a2 == 1)                    /* F_GETFD: the close-on-exec flag */
            return src->cloexec ? FD_CLOEXEC : 0;
        if (a2 == 2) {                  /* F_SETFD: set or clear close-on-exec */
            src->cloexec = (a3 & FD_CLOEXEC) ? 1 : 0;
            return 0;
        }
        if (a2 == 3)                    /* F_GETFL: the access mode and flags.
                                         * glibc fdopen reads this to check the
                                         * fd allows the stream's mode; a bare 0
                                         * (O_RDONLY) made fdopen(fd, "w") fail
                                         * EINVAL, so a write temp file could not
                                         * be opened (sort, among others). */
            return src->oflags | O_LARGEFILE;
        if (a2 == 4) {                  /* F_SETFL: update the changeable flags,
                                         * keeping the access mode. O_NONBLOCK
                                         * here is what a socket event loop sets
                                         * so accept/recv return EAGAIN. */
            src->oflags = (src->oflags & O_ACCMODE) |
                          (int)(a3 & (O_NONBLOCK | O_APPEND));
            return 0;
        }
        return 0;
    }
    case SYS_execve: {
        char pb[UPATH_MAX];
        long e;
        const char *up = user_path((uint64_t)a1, pb, &e);
        if (!up)
            return e;
        return do_execve(up, (const char *const *)a2,
                         (const char *const *)a3);
    }
    case SYS_fork:
    case SYS_vfork:
        return do_fork();
    case SYS_clone:
        /* CLONE_VM|CLONE_VFORK is posix_spawn; CLONE_VM alone is a thread;
         * otherwise clone is the fork path (fork() is clone with SIGCHLD and no
         * CLONE_VM). */
        if (((uint64_t)a1 & CLONE_VM) && ((uint64_t)a1 & CLONE_VFORK))
            return do_spawn_vfork((uint64_t)a1, (uint64_t)a2);
        if ((uint64_t)a1 & CLONE_VM) {
            /* The TLS and child-tid pointer swap argument slots across
             * architectures (aarch64 is CLONE_BACKWARDS); normalize them. */
            uint64_t tls, ctid;
            clone_split_tls_ctid((uint64_t)a4, (uint64_t)a5, &tls, &ctid);
            return do_clone_thread((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                                   ctid, tls);
        }
        return do_fork();
    case SYS_clone3:
        return do_clone3((uint64_t)a1, (uint64_t)a2);
    case SYS_wait4:
        return do_wait4(a1, (int *)a2, (int)a3);
    case SYS_futex:
        return do_futex((uint64_t)a1, (int)a2, (int)a3);
    case SYS_sched_yield:
        lx_yield();                     /* drop the giant lock so a peer can run */
        return 0;
    case SYS_sched_getaffinity: {
        /* sched_getaffinity(pid, cpusetsize, mask): report one online CPU so a
         * tool that sizes its thread pool from the mask sees a single processor.
         * The return value is the number of bytes written. */
        unsigned long mask = 1;         /* cpu 0 only */
        if (a2 < sizeof mask || !a3)
            return -LERR_INVAL;
        long e = copy_to_user((uint64_t)a3, &mask, sizeof mask);
        if (e < 0)
            return e;
        return (long)sizeof mask;
    }
    case SYS_setitimer:
        /* setitimer(which, new, old): arm/disarm ITIMER_REAL (SIGALRM). */
        return do_setitimer((int)a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_getitimer:
        /* getitimer(which, cur): report the remaining ITIMER_REAL value. */
        return do_getitimer((int)a1, (uint64_t)a2);
    case SYS_nanosleep:
        /* nanosleep(req, rem): a relative sleep on the monotonic clock. */
        return do_nanosleep(1, 0, (uint64_t)a1);
    case SYS_clock_nanosleep:
        /* clock_nanosleep(clockid, flags, req, rem). */
        return do_nanosleep((int)a1, (int)a2, (uint64_t)a3);
    case SYS_getpid:
        return lproc_cur->sh->pid;
    case SYS_getppid:
        return lproc_cur->sh->parent;
    case SYS_gettid:
        return lproc_cur->tid;
    case SYS_kill:
        return do_kill((int)a1, (int)a2);
    case SYS_tkill:
        /* tkill(tid, sig): the two-argument form musl's raise and abort use. */
        return do_tgkill((int)a1, (int)a2);
    case SYS_tgkill:
        /* tgkill(tgid, tid, sig): the three-argument thread-directed form. */
        return do_tgkill((int)a2, (int)a3);
    case SYS_rt_sigaction:
        return do_rt_sigaction((int)a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_rt_sigprocmask:
        return do_rt_sigprocmask((int)a1, (uint64_t)a2, (uint64_t)a3);
    case SYS_rt_sigreturn:
        /* Return from a handler: restore the frame maybe_deliver_signal built. */
        return do_rt_sigreturn();
    case SYS_rt_sigsuspend:
        return do_rt_sigsuspend((uint64_t)a1, (uint64_t)a2);
    case SYS_pause:
        return do_pause();
    case SYS_uname: {
        /* struct utsname: six NUL-terminated 65-byte fields. */
        if (!a1)
            return -LERR_INVAL;
        static const char *const f[6] = {
            "Linux", "jotun", "6.0.0-jotun", "#1 jotun", UNAME_MACHINE, "(none)",
        };
        char u[6 * 65];
        memset(u, 0, sizeof u);
        for (int i = 0; i < 6; i++) {
            char *d = u + i * 65;
            const char *s = f[i];
            int j = 0;
            while (s[j] && j < 64) { d[j] = s[j]; j++; }
            d[j] = 0;
        }
        long e = copy_to_user((uint64_t)a1, u, sizeof u);
        if (e < 0)
            return e;
        return 0;
    }
    case SYS_exit:
        do_exit(a1, 0);                 /* this thread only; does not return */
    case SYS_exit_group:
        do_exit(a1, 1);                 /* the whole process; does not return */
    default:
        con_puts("[bin] unhandled syscall ");
        put_dec(nr);
        con_putc('\n');
        return -LERR_NOSYS;
    }
}

/* The C half of the SYSCALL handler. One Linux syscall runs at a time: take the
 * giant lock, run the handler body, release it. Handlers that never return here
 * (execve on success, exit) release the lock themselves before they leave. */
long
linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
{
    giant_lock();
    lproc_cur->cur_nr = nr;                  /* for the lx_yield spin watchdog */
    long r = linux_sys_dispatch_inner(nr, a1, a2, a3, a4, a5);
    lx_progress();          /* a completed syscall clears the spin watchdog */
    signal_killpoint();     /* a default-fatal signal terminates here */
    r = maybe_deliver_signal(r);    /* a caught signal runs its handler on return */
    giant_unlock();
    return r;
}

/****************************************************************
 * Stack and SYSCALL setup.
 ****************************************************************/

/* Build a System V initial stack at the top of the user window from explicit
 * argv and envp vectors (the stack pages must already be mapped and this
 * process's CR3 active) and return the rsp to enter with: argc, argv[], NULL,
 * envp[], NULL, auxv, with the strings and 16 random bytes placed above it. The
 * auxv (AT_PHDR/PHENT/PHNUM/BASE/ENTRY and the identity and AT_RANDOM/EXECFN
 * tags) is what a dynamic linker reads to find the program image it must
 * relocate; exe describes the loaded executable and interp the loaded dynamic
 * linker (AT_BASE), or has_interp is 0 for a static image. The argv/envp string
 * pointers must stay valid during the build (they come from boot data or an
 * execve snapshot, never live user memory that this overwrites). */
static uint64_t
build_stack_vec(const char *const *argv, int argc,
                const char *const *envp, int envc,
                const struct elf_info *exe, const struct elf_info *interp)
{
    uint8_t *sp = (uint8_t *)(uintptr_t)LINUX_USTACK_TOP;
    char *argp[LINUX_MAX_ARGS];
    char *envpp[LINUX_MAX_ARGS];

    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        memcpy(sp, envp[i], len);
        envpp[i] = (char *)sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy(sp, argv[i], len);
        argp[i] = (char *)sp;
    }

    /* 16 bytes for AT_RANDOM, seeded from the cycle counter, below the strings. */
    sp -= 16;
    uint8_t *rnd = sp;
    uint64_t seed = arch_cycles();
    for (int k = 0; k < 16; k++) {
        rnd[k] = (uint8_t)seed;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    /* The control block below that, 16-byte aligned as the entry contract wants
     * rsp (pointing at argc) to be. Words: argc, argv[0..], NULL, envp[0..],
     * NULL, LINUX_NAUXV auxv pairs. */
    int nwords = 1 + argc + 1 + envc + 1 + 2 * LINUX_NAUXV;
    uint64_t *blk = (uint64_t *)((uintptr_t)sp - (uintptr_t)nwords * 8);
    blk = (uint64_t *)((uintptr_t)blk & ~(uintptr_t)0xF);

    int w = 0;
    blk[w++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        blk[w++] = (uintptr_t)argp[i];
    blk[w++] = 0;                       /* argv terminator */
    for (int i = 0; i < envc; i++)
        blk[w++] = (uintptr_t)envpp[i];
    blk[w++] = 0;                       /* envp terminator */
    blk[w++] = AT_PHDR;   blk[w++] = exe->phdr;
    blk[w++] = AT_PHENT;  blk[w++] = exe->phentsize;
    blk[w++] = AT_PHNUM;  blk[w++] = exe->phnum;
    blk[w++] = AT_BASE;   blk[w++] = exe->has_interp ? interp->bias : 0;
    blk[w++] = AT_ENTRY;  blk[w++] = exe->entry;
    blk[w++] = AT_EXECFN; blk[w++] = (uintptr_t)argp[0];
    blk[w++] = AT_PAGESZ; blk[w++] = VM_PAGE;
    blk[w++] = AT_RANDOM; blk[w++] = (uintptr_t)rnd;
    blk[w++] = AT_SECURE; blk[w++] = 0;
    blk[w++] = AT_UID;    blk[w++] = 0;
    blk[w++] = AT_EUID;   blk[w++] = 0;
    blk[w++] = AT_GID;    blk[w++] = 0;
    blk[w++] = AT_EGID;   blk[w++] = 0;
    blk[w++] = AT_HWCAP;  blk[w++] = 0;
    blk[w++] = AT_NULL;   blk[w++] = 0;

    return (uint64_t)(uintptr_t)blk;
}

/* The initial program stack: argv is the loaded file (argv0) followed by the
 * boot arguments after it, no environment. `./jotun libos.elf bin <file>
 * <args...>` thus reaches the program as <file> <args...>. */
static uint64_t
build_stack(const char *argv0, const struct elf_info *exe,
            const struct elf_info *interp)
{
    const char *args[LINUX_MAX_ARGS];
    /* The boot environment is a handful of strings (the host caps it well below
     * this); keep the array small so this frame plus build_stack_vec's stays
     * within the init task's kernel stack. */
    const char *env[32];
    int argc = 0;
    int envc = 0;
    int envmax = (int)(sizeof env / sizeof env[0]);

    args[argc++] = argv0;
    for (int i = 2; i < boot_argc() && argc < LINUX_MAX_ARGS; i++)
        args[argc++] = boot_argv(i);
    /* The host supplies an environment only for an interactive guest (a PATH,
     * HOME, PS1, and a TERM for the host terminal); otherwise envc is 0 and the
     * program starts with an empty environment, exactly as before. */
    for (int i = 0; i < boot_envc() && envc < envmax; i++)
        env[envc++] = boot_envp(i);
    return build_stack_vec(args, argc, envc ? env : NULL, envc, exe, interp);
}

/* Copy a NULL-terminated user vector of string pointers into out[] with the
 * strings packed into buf. Returns the count. Snapshotting the strings lets
 * execve overwrite the old image (where argv may live) before building the new
 * stack from them. */
static int
copy_uvec(const char *const *uvec, const char **out, char *buf, size_t buflen)
{
    int n = 0;
    size_t off = 0;

    if (!uvec)
        return 0;
    while (n < LINUX_MAX_ARGS - 1) {
        /* The pointer slot and the string it names both live in user memory;
         * validate the slot, then bounce the string. A fault or a full buffer
         * ends the vector here, as a too-long argv would on Linux. */
        if (!user_ok((uint64_t)(uintptr_t)&uvec[n], sizeof uvec[n]))
            break;
        const char *s = uvec[n];
        if (!s)
            break;
        long len = strncpy_from_user(buf + off, (uint64_t)(uintptr_t)s,
                                     buflen - off);
        if (len < 0)
            break;
        out[n] = buf + off;
        off += (size_t)len + 1;
        n++;
    }
    return n;
}

/* Scratch for one execve's argv and envp strings, split half and half. A
 * compiler's linker command line plus gcc's inherited COLLECT and COMPILER_PATH
 * environment runs to several kilobytes, so this is sized well above that. */
#define EXEC_STR_MAX 32768

/* Load a program already open on LINUX_FID into the active address space, and
 * its PT_INTERP dynamic linker if it has one. The executable loads at
 * LINUX_PROG_BASE (an ET_DYN PIE biased there, an ET_EXEC at its fixed vaddr);
 * the interpreter loads just above it, 2 MiB aligned. On return *entry is where
 * to begin: the interpreter's entry when present (it relocates, then jumps to
 * the executable via AT_ENTRY), else the executable's own. Returns 0, or -1 on
 * any load error. */
static int
load_program(struct elf_info *exe, struct elf_info *interp, uint64_t *entry,
             const unsigned char *exemem, uint64_t exelen)
{
    /* The executable comes from the writable overlay (an in-memory image) when
     * exemem is set, otherwise from the 9P fid already walked and opened. Its
     * interpreter, if any, is always a 9P path (the host's ld-linux). */
    int lr = exemem
        ? elf_load_mem(exemem, exelen, VM_USER_LO, VM_USER_HI, LINUX_PROG_BASE, exe)
        : elf_load_ex(LINUX_FID, VM_USER_LO, VM_USER_HI, LINUX_PROG_BASE, exe);
    if (lr != 0)
        return -1;

    if (!exe->has_interp) {
        interp->has_interp = 0;
        *entry = exe->entry;
        return 0;
    }

    const char *irel = bundle_path(exe->interp);
    if (ns_walk(irel, LINUX_IFID) != 0)
        return -1;
    if (p9_open(LINUX_IFID) != 0) {
        p9_clunk(LINUX_IFID);
        return -1;
    }
    uint64_t ibase = (exe->load_end + 0x1FFFFF) & ~0x1FFFFFULL;  /* 2 MiB aligned */
    int r = elf_load_ex(LINUX_IFID, VM_USER_LO, VM_USER_HI, ibase, interp);
    p9_clunk(LINUX_IFID);
    if (r != 0)
        return -1;

    *entry = interp->entry;
    return 0;
}

/* execve: replace the running image with a bundle program. The new argv/envp
 * are snapshotted into kernel memory first (they may point into the image we
 * are about to overwrite), then the ELF is loaded over the program window, the
 * heap reset, and a fresh stack built. Control returns to the linux_elf_run
 * loop via linux_exit, which re-enters the new entry. fds and the ramfs persist
 * across the exec, as on Linux. The program may come from a bundle or host
 * mount (over 9P) or from the writable overlay (an in-memory image). */
static long
do_execve(const char *path, const char *const *argv, const char *const *envp)
{
    char relbuf[RAMFS_NAME];
    resolve_path(path, relbuf, sizeof relbuf);
    follow_link_key(relbuf, sizeof relbuf);
    const char *rel = relbuf;

    /* Snapshot argv/envp into kernel memory first: they may point into the image
     * about to be overwritten, and a "#!" script rewrites argv below. */
    static const char *na[LINUX_MAX_ARGS];
    static const char *ne[LINUX_MAX_ARGS];
    char *sbuf = kmalloc(EXEC_STR_MAX);
    if (!sbuf)
        return -LERR_NOMEM;
    int argc = copy_uvec(argv, na, sbuf, EXEC_STR_MAX / 2);
    int envc = copy_uvec(envp, ne, sbuf + EXEC_STR_MAX / 2, EXEC_STR_MAX / 2);
    if (argc == 0) {                    /* execve needs at least argv[0] */
        na[0] = rel;
        argc = 1;
    }

    /* Open the program to run, resolving "#!" interpreters. A program built in
     * the writable overlay (configure's ./conftest, a linked a.out) execs from
     * its in-memory image; a bundle or host-mount file execs over a 9P fid. A
     * script whose first two bytes are "#!" is not loadable as an ELF: the named
     * interpreter runs instead, with the script path (and any one "#!" argument)
     * spliced into argv ahead of the script's own arguments, exactly as Linux
     * does. shbuf holds the interpreter/argument/script strings so they survive
     * relbuf being rewritten across nested levels. */
    const unsigned char *exemem = NULL;
    uint64_t exelen = 0;
    char *shbuf = kmalloc(2048);
    if (!shbuf) {
        kfree(sbuf);
        return -LERR_NOMEM;
    }
    size_t shoff = 0;
    int shdepth = 0;
    for (;;) {
        int ri = ramfs_find(rel);
        exemem = NULL;
        exelen = 0;
        if (ri >= 0 && !ramfs[ri].is_dir) {
            exemem = ramfs[ri].data;
            exelen = ramfs[ri].len;
        } else {
            if (ns_walk(rel, LINUX_FID) != 0) {
                kfree(shbuf);
                kfree(sbuf);
                return -LERR_NOENT;
            }
            if (p9_open(LINUX_FID) != 0) {
                p9_clunk(LINUX_FID);
                kfree(shbuf);
                kfree(sbuf);
                return -LERR_ACCES;
            }
        }

        /* Peek the first bytes to spot a "#!" interpreter line. 9P reads are
         * offset-based, so the loader re-reads from 0 afterwards. */
        unsigned char hdr[160];
        int hn = 0;
        if (exemem) {
            hn = exelen < sizeof hdr ? (int)exelen : (int)sizeof hdr;
            if (hn > 0)
                memcpy(hdr, exemem, (size_t)hn);
        } else {
            long r = p9_read(LINUX_FID, 0, hdr, sizeof hdr);
            hn = r > 0 ? (int)r : 0;
        }
        if (hn < 2 || hdr[0] != '#' || hdr[1] != '!')
            break;                      /* an ELF (or the loader rejects it) */

        if (!exemem)
            p9_clunk(LINUX_FID);
        if (++shdepth > 4) {            /* bounded against a script cycle */
            kfree(shbuf);
            kfree(sbuf);
            return -LERR_LOOP;
        }

        /* Parse "#!": skip blanks, take the interpreter path, then at most one
         * argument (the rest of the line, trimmed), stopping at a newline. */
        int i = 2;
        while (i < hn && (hdr[i] == ' ' || hdr[i] == '\t'))
            i++;
        int is = i;
        while (i < hn && hdr[i] != ' ' && hdr[i] != '\t' && hdr[i] != '\n')
            i++;
        int ie = i;
        while (i < hn && (hdr[i] == ' ' || hdr[i] == '\t'))
            i++;
        int as = i;
        while (i < hn && hdr[i] != '\n')
            i++;
        int ae = i;
        while (ae > as && (hdr[ae - 1] == ' ' || hdr[ae - 1] == '\t'))
            ae--;
        int ilen = ie - is, alen = ae - as, slen = (int)strlen(rel);
        if (ilen <= 0) {                /* "#!" with no interpreter */
            kfree(shbuf);
            kfree(sbuf);
            return -LERR_NOEXEC;
        }
        if (shoff + (size_t)ilen + (size_t)alen + (size_t)slen + 4 > 2048) {
            kfree(shbuf);
            kfree(sbuf);
            return -LERR_NOMEM;
        }

        char *interp = shbuf + shoff;
        memcpy(interp, hdr + is, (size_t)ilen);
        interp[ilen] = '\0';
        shoff += (size_t)ilen + 1;
        char *iarg = NULL;
        if (alen > 0) {
            iarg = shbuf + shoff;
            memcpy(iarg, hdr + as, (size_t)alen);
            iarg[alen] = '\0';
            shoff += (size_t)alen + 1;
        }
        /* The interpreter resolves this path against its own cwd, which need not
         * be the guest root, so pass an absolute guest path (rel is the resolved
         * key with no leading slash). "#!" exec via a relative path (make's bare
         * ./configure) otherwise fails to open in the interpreter. */
        char *script = shbuf + shoff;   /* copy before relbuf is rewritten */
        script[0] = '/';
        memcpy(script + 1, rel, (size_t)slen);
        script[slen + 1] = '\0';
        shoff += (size_t)slen + 2;

        /* Rewrite argv to: interp [iarg] script  then the script's own argv[1..].
         * The script's argv[0] is dropped; the script path takes its place. */
        static const char *na2[LINUX_MAX_ARGS];
        int k = 0;
        na2[k++] = interp;
        if (iarg)
            na2[k++] = iarg;
        na2[k++] = script;
        for (int j = 1; j < argc && k < LINUX_MAX_ARGS - 1; j++)
            na2[k++] = na[j];
        for (int j = 0; j < k; j++)
            na[j] = na2[j];
        argc = k;

        resolve_path(interp, relbuf, sizeof relbuf);
        follow_link_key(relbuf, sizeof relbuf);
        rel = relbuf;
    }

    /* A fresh address space for the new image, replacing the old one. A
     * posix_spawn child runs in its parent's borrowed space, which execve must
     * not destroy and which the parent reclaims itself. */
    vmspace_t old = lproc_cur->sh->vmspace;
    int borrowed = lproc_cur->sh->vm_borrowed;
    vmspace_t as = linux_vmspace_setup();
    if (!as) {
        if (!exemem)
            p9_clunk(LINUX_FID);
        kfree(shbuf);
        kfree(sbuf);
        return -LERR_NOMEM;
    }
    vmspace_switch(as);

    struct elf_info exe, interp;
    uint64_t entry;
    int lr = load_program(&exe, &interp, &entry, exemem, exelen);
    if (!exemem)
        p9_clunk(LINUX_FID);
    if (lr != 0) {
        vmspace_switch(old);            /* keep the old image on failure */
        vmspace_destroy(as);
        kfree(shbuf);
        kfree(sbuf);
        return -LERR_NOENT;
    }

    lproc_cur->sh->vmspace = as;        /* the resume hook now restores this one */
    lproc_cur->sh->vm_borrowed = 0;

    /* The exec has committed, so now drop every close-on-exec descriptor. This
     * is what delivers EOF on a subprocess exec-error pipe (the child marks it
     * O_CLOEXEC and the parent waits for it to close as the signal that exec
     * succeeded); without it the parent blocks forever and Popen hangs. */
    for (int i = 0; i < LINUX_NFD; i++)
        if (fds[i].used && fds[i].cloexec)
            do_close(i);

    /* execve resets caught signals to the default action but keeps those set to
     * SIG_IGN; the blocked mask and pending signals are preserved. */
    for (int s = 0; s < NSIG; s++)
        if (lproc_cur->sh->sigact[s].handler != SIG_IGN)
            memset(&lproc_cur->sh->sigact[s], 0, sizeof lproc_cur->sh->sigact[s]);

    arena_init(image_top(&exe, &interp));   /* fresh heap above the new image */
    uint64_t rsp = build_stack_vec(na, argc, ne, envc, &exe, &interp);
    kfree(shbuf);
    kfree(sbuf);

    /* The child now has its own space, so the vfork parent can safely resume. */
    if (lproc_cur->sh->vforking) {
        lproc_cur->sh->vforking = 0;
        task_wake(lproc_cur->sh->vfork_waiter);
    }
    if (!borrowed)
        vmspace_destroy(old);           /* release the replaced image's frames */

    giant_unlock();                     /* linux_jump never returns to the dispatcher */
    linux_jump(entry, rsp);             /* enter the new image; does not return */
}

/* One-time frame-pool init: the free RAM between the kernel image and the vcpu
 * stacks backs every process's private pages and page tables. */
static int phys_ready;

/* Allocate the big per-run tables from the kernel heap, once. They are pointers
 * (not .bss arrays) so raising a count costs heap, not the kernel's room below
 * the user window. kmalloc'd memory is not zeroed, and the code relies on a
 * cleared table (used == 0, nref == 0), so each is memset. */
static void
linux_tables_init(void)
{
    ramfs   = kmalloc(sizeof *ramfs * RAMFS_MAX);
    ofiles  = kmalloc(sizeof *ofiles * LINUX_NOFILE);
    shareds = kmalloc(sizeof *shareds * LINUX_NPROC);
    procs   = kmalloc(sizeof *procs * LINUX_NPROC);
    usocks  = kmalloc(sizeof *usocks * USOCK_MAX);
    if (!ramfs || !ofiles || !shareds || !procs || !usocks) {
        con_puts("[bin] kernel heap too small for the process tables\n");
        arch_halt();
    }
    memset(ramfs, 0, sizeof *ramfs * RAMFS_MAX);
    memset(ofiles, 0, sizeof *ofiles * LINUX_NOFILE);
    memset(shareds, 0, sizeof *shareds * LINUX_NPROC);
    memset(procs, 0, sizeof *procs * LINUX_NPROC);
    memset(usocks, 0, sizeof *usocks * USOCK_MAX);
    linux_rwmount_init();
}

static void
linux_phys_init(void)
{
    if (!phys_ready) {
        phys_init(LINUX_PROG_BASE, VM_USER_HI);
        linux_tables_init();
        kheap_init();           /* heap now grows past the bootstrap band */
        task_add_event_hook(sleep_wake_hook);   /* wake timed sleepers on a tick */
        con_tty_init();         /* console line discipline + input wake hook */
        phys_ready = 1;
    }
}

/* Build a fresh address space. The user stack is demand-paged like the rest of
 * the window: pf_handle backs each stack page on first touch (a user access, or
 * a kernel copy_*_user into an initial-stack page build_stack has not reached
 * yet), and the guard band below it still faults because pf_handle refuses that
 * range. Pre-mapping the whole 8 MiB reservation here was the scaling ceiling: a
 * process cost 2048 frames before it ran, and fork copied every one of them, so
 * a handful of live processes exhausted the frame pool. Demand paging makes a
 * fresh process and a fork cost only the pages actually touched (tens of KiB),
 * which is what lets a real build hold many processes live at once. Returns the
 * vmspace, or 0. */
static vmspace_t
linux_vmspace_setup(void)
{
    return vmspace_new();
}

void
linux_elf_run(const char *file)
{
    con_puts("[bin] loading ");
    con_puts(file);
    con_putc('\n');

    linux_phys_init();
    task_set_resume_hook(linux_on_resume);

    /* The first process and its main thread. It runs on the current task
     * (init); forked children and threads become their own tasks. */
    struct lproc *p = proc_new();
    if (!p) {
        con_puts("[bin] no process slot\n");
        return;
    }
    p->sh->vmspace = linux_vmspace_setup();
    if (!p->sh->vmspace) {
        con_puts("[bin] no memory for the address space\n");
        return;
    }
    p->task_id = task_self();
    p->sh->main_task = p->task_id;
    task_set_proc(p);                   /* so the resume hook restores it later */
    lproc_cur = p;
    vmspace_switch(p->sh->vmspace);     /* user window now demand-maps on fault */

    if (ns_walk(file, LINUX_FID) != 0) {
        con_puts("[bin] not found in bundle\n");
        return;
    }
    if (p9_open(LINUX_FID) != 0) {
        p9_clunk(LINUX_FID);
        con_puts("[bin] open failed\n");
        return;
    }
    struct elf_info exe, interp;
    uint64_t entry;
    int lr = load_program(&exe, &interp, &entry, NULL, 0);
    p9_clunk(LINUX_FID);
    if (lr != 0) {
        con_puts("[bin] load failed (bad ELF, or interpreter not in bundle?)\n");
        return;
    }
    con_puts("[bin] loaded, entry=");
    con_puthex(entry);
    if (exe.has_interp) {
        con_puts(" interp=");
        con_puts(exe.interp);
        con_puts(" base=");
        con_puthex(interp.bias);
    }
    con_putc('\n');

    fds_init();
    arena_init(image_top(&exe, &interp));
    linux_trap_init();
    con_puts("[bin] syscall armed\n");
    uint64_t rsp = build_stack(file, &exe, &interp);
    con_puts("[bin] stack built\n");

    con_puts("[bin] entering guest\n");
    /* The resume hook sets TSS.rsp0 on a scheduler switch, but the main thread
     * enters user mode here on the init task without going through it, so arm its
     * per-thread kernel stack for a preemption tick now. */
    arch_set_kstack(p->kstack_top);
    linux_jump(entry, rsp);             /* enter the program; exit ends the task */
}

#else  /* unsupported architecture */

void
linux_elf_run(const char *file)
{
    (void)file;
    con_puts("[bin] the Linux binary-compat guest needs x86-64 or arm64\n");
}

#endif
