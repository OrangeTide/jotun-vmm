/* linuxelf.c : run a static Linux x86-64 binary under a SYSCALL trap (M16.1).
 *
 * The spike: load a static, non-PIE musl ELF (ET_EXEC linked at 0x400000, the
 * same window the native programs use) with the existing elf_load_fid, build a
 * System V initial stack (argc/argv/envp/auxv), arm the SYSCALL machinery, and
 * jump in. A small dispatcher services the syscalls a musl "hello" issues:
 * arch_prctl(ARCH_SET_FS), set_tid_address, ioctl(TIOCGWINSZ), write/writev,
 * and exit/exit_group. Everything else returns -ENOSYS, which is enough to
 * learn where the real work is. The console is the guest's con_putc.
 *
 * x86-64 only. The SYSCALL trap and the kernel<->program handoff live in
 * arch/x86_64_linuxsys.S. On other targets linux_elf_run is a stub. */
#include "linuxelf.h"

#include "print.h"

#if defined(__x86_64__)

#include "boot.h"
#include "elfload.h"
#include "mem.h"
#include "ns.h"
#include "p9.h"
#include "string.h"
#include "task.h"
#include "jotun_svc.h"       /* JOTUN_PROG_BASE */
#include "arch/x86_64_vmspace.h"

#include <stdint.h>

/* The 9P fid used to load the program ELF, distinct from the native loader's
 * (prog.c uses 10), and the pool of fids backing open file descriptors. */
#define LINUX_FID      12u
#define LINUX_FID_BASE 32u          /* open files use fids 32..32+LINUX_NFD */

/* The Linux program's address space, independent of the native program window.
 * A static non-PIE binary is linked at 0x400000; the load window runs to 8 MiB,
 * which holds a 2 MiB-ish glibc image. Above that is the heap: brk grows up from
 * LINUX_HEAP_BASE and anonymous mmap grows down from the top of RAM, less a
 * reservation that matches the host's per-vcpu stacks (MAX_VCPUS * STACK_SLOT =
 * 8 * 512 KiB in host/main.c). They meet in the middle and fail there. */
#define LINUX_PROG_BASE    0x400000ULL
#define LINUX_PROG_LIMIT   0x800000ULL
#define LINUX_HEAP_BASE    0x800000ULL
#define LINUX_USTACK_SIZE  (256u * 1024)
#define LINUX_USTACK_TOP   VM_USER_HI       /* top of the user window */
#define PAGE_SIZE          0x1000ULL

/* mmap flags and failure value. */
#define MAP_ANONYMOUS 0x20
#define MMAP_FAILED   (-LERR_NOMEM)

/* The most argv entries delivered to a Linux program. */
#define LINUX_MAX_ARGS 32

/* Linux syscall numbers (x86-64) we recognise. */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_mmap            9
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_lseek           8
#define SYS_pipe            22
#define SYS_ioctl           16
#define SYS_writev          20
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_fcntl           72
#define SYS_getcwd          79
#define SYS_getuid          102
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_prctl           157
#define SYS_exit            60
#define SYS_execve          59
#define SYS_clone           56
#define SYS_fork            57
#define SYS_vfork           58
#define SYS_wait4           61
#define SYS_getpid          39
#define SYS_getppid         110
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_gettid          186
#define SYS_uname           63
#define SYS_mprotect        10
#define SYS_time            201
#define SYS_clock_gettime   228
#define SYS_arch_prctl      158
#define SYS_getdents64      217
#define SYS_set_tid_address 218
#define SYS_exit_group      231
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_readlinkat      267
#define SYS_set_robust_list 273
#define SYS_dup3            292
#define SYS_pipe2           293
#define SYS_statx           332
#define SYS_prlimit64       302
#define SYS_getrandom       318
#define SYS_rseq            334

#define ARCH_SET_FS 0x1002

#define AT_FDCWD (-100)

/* open() flags we care about. The 9P bundle is read-only; writable files live
 * in the in-memory ramfs, reached by a write intent or O_CREAT. */
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

/* lseek whence. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Linux st_mode type bits and dirent types. */
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define DT_DIR  4
#define DT_REG  8

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

/* x86-64 MSRs for the SYSCALL path. */
#define MSR_EFER     0xC0000080u
#define MSR_STAR     0xC0000081u
#define MSR_LSTAR    0xC0000082u
#define MSR_SFMASK   0xC0000084u
#define MSR_FS_BASE  0xC0000100u

/* The handoff and trap, from x86_64_linuxsys.S. */
extern void linux_syscall_entry(void);

/* ELF auxv entry tags. */
#define AT_NULL   0
#define AT_PAGESZ 6
#define AT_RANDOM 25

/* The program stack lives at the top of the per-process user window, its pages
 * pre-mapped so growth never faults (which would fault on the stack itself). */

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((uint32_t)val),
                          "d"((uint32_t)(val >> 32)));
}

static inline uint64_t
read_efer(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    return ((uint64_t)hi << 32) | lo;
}

static inline void
sti(void)
{
    __asm__ volatile ("sti" ::: "memory");
}

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

/****************************************************************
 * File descriptors over the read-only 9P bundle.
 ****************************************************************/

struct iovec {
    const void *base;
    unsigned long len;
};

/* The x86-64 Linux struct stat the stat/fstat/newfstatat syscalls fill. */
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

/* The getdents64 record. d_name is NUL-terminated and the record is padded so
 * d_reclen is 8-aligned. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define LINUX_NFD   32
#define LINUX_NPIPE 4
#define PIPE_CAP    4096

struct lfd {
    int      used;
    int      iscon;             /* this fd is the console (stdin/out/err) */
    int      isdir;
    int      ispipe;            /* this fd is a pipe end */
    int      isram;             /* this fd is a ramfs file */
    int      pidx;              /* pipe pool index when ispipe */
    int      ridx;              /* ramfs index when isram */
    int      wr;                /* pipe write end when set, else read end */
    uint32_t p9fid;             /* backing 9P fid, 0 for the console fds */
    uint64_t pos;               /* read cursor (file) or 9P dir offset */
    uint64_t size;
};

/* A writable in-memory file. The read-only 9P bundle cannot hold new files, so
 * anything created or written goes here, keyed by its bundle-relative path. The
 * buffer grows on write and lives for the program's run (close keeps it, so a
 * file written early can be reopened and read later). */
#define RAMFS_MAX     16
#define RAMFS_NAME    64
#define RAMFS_FILE_MAX (1024u * 1024u)      /* per-file cap */

struct ramfile {
    int      used;
    char     name[RAMFS_NAME];
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
};

static struct ramfile ramfs[RAMFS_MAX];

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
#define LINUX_NPROC  8
#define LINUX_KSTACK (16u * 1024)       /* per-process syscall handler stack */
#define CLONE_VM     0x00000100u        /* clone shares the address space (thread) */
static struct lpipe pipes[LINUX_NPIPE];

/* Trapframe field offsets, mirrored in arch/x86_64_linuxsys.S as TF_*. The
 * SYSCALL assembly saves the user registers here on entry and restores them on
 * return, and fork copies the frame into the child so it resumes at the same
 * instruction with rax = 0. The checks below catch any layout drift. */
struct lproc {
    uint64_t tf_rax;        /* 0 */
    uint64_t tf_rbx;        /* 8 */
    uint64_t tf_rcx;        /* 16  user rip (SYSCALL return address) */
    uint64_t tf_rdx;        /* 24 */
    uint64_t tf_rsi;        /* 32 */
    uint64_t tf_rdi;        /* 40 */
    uint64_t tf_rbp;        /* 48 */
    uint64_t tf_r8;         /* 56 */
    uint64_t tf_r9;         /* 64 */
    uint64_t tf_r10;        /* 72 */
    uint64_t tf_r11;        /* 80  user rflags */
    uint64_t tf_r12;        /* 88 */
    uint64_t tf_r13;        /* 96 */
    uint64_t tf_r14;        /* 104 */
    uint64_t tf_r15;        /* 112 */
    uint64_t tf_rsp;        /* 120 user rsp */
    uint64_t kstack_top;    /* 128 top of the per-process syscall handler stack */
    /* --- fields below are never touched by the assembly --- */
    vmspace_t vmspace;
    unsigned char *kstack;  /* kmalloc base of the handler stack */
    int used;
    int pid;
    int parent;             /* pid of the parent, or 0 for the first process */
    int task_id;            /* libOS task running this process */
    int exited;
    long exit_status;
    uint64_t fs_base;       /* FS base MSR (TLS), restored on a context switch */
    struct lfd fdtab[LINUX_NFD];
    /* The heap arena. heap_brk grows up from LINUX_HEAP_BASE; heap_mmap grows
     * down from heap_mmaptop. A bump allocator; munmap does not reclaim. The
     * helpers reach these through the fds/brk_cur/mmap_* macros below. */
    uint64_t heap_brk, heap_mmap, heap_mmaptop;
};

_Static_assert(__builtin_offsetof(struct lproc, tf_rax) == 0, "TF_RAX");
_Static_assert(__builtin_offsetof(struct lproc, tf_r11) == 80, "TF_R11");
_Static_assert(__builtin_offsetof(struct lproc, tf_r15) == 112, "TF_R15");
_Static_assert(__builtin_offsetof(struct lproc, tf_rsp) == 120, "TF_RSP");
_Static_assert(__builtin_offsetof(struct lproc, kstack_top) == 128, "TF_KSTACK");

static struct lproc procs[LINUX_NPROC];

/* The running process. Set by the resume hook before the scheduler runs a
 * process task, and read by the assembly as lproc_cur. All the do_* syscall
 * helpers reach their file table and arena through it. */
struct lproc *lproc_cur;

/* Reach the running process's state by the bare names the helpers already use.
 * The struct members are spelled differently so that explicit member access
 * elsewhere (child->fdtab, parent->heap_brk) does not trip these macros. */
#define fds       (lproc_cur->fdtab)
#define brk_cur   (lproc_cur->heap_brk)
#define mmap_cur  (lproc_cur->heap_mmap)
#define mmap_top  (lproc_cur->heap_mmaptop)

static long do_execve(const char *path, const char *const *argv,
                      const char *const *envp);
static vmspace_t linux_vmspace_setup(void);
extern void linux_jump(uint64_t entry, uint64_t user_rsp)
    __attribute__((noreturn));
extern void linux_resume(struct lproc *p) __attribute__((noreturn));

static void
arena_init(void)
{
    /* brk grows up from the heap base; anonymous mmap grows down from just below
     * the user stack at the top of the window. Pages in between fault in on
     * demand, so the arena is lazily backed. */
    brk_cur = LINUX_HEAP_BASE;
    mmap_top = (LINUX_USTACK_TOP - LINUX_USTACK_SIZE) & ~(PAGE_SIZE - 1);
    mmap_cur = mmap_top;
}

static long
do_brk(uint64_t want)
{
    if (want == 0)
        return (long)brk_cur;
    if (want < LINUX_HEAP_BASE || want > mmap_cur)
        return (long)brk_cur;           /* refuse; caller falls back to mmap */
    brk_cur = want;
    return (long)brk_cur;
}

static long
do_mmap(uint64_t addr, uint64_t len, uint64_t flags, long fd)
{
    (void)addr;
    if (!(flags & MAP_ANONYMOUS) && fd >= 0)
        return MMAP_FAILED;             /* file-backed mmap unsupported */
    uint64_t n = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (n == 0 || mmap_cur - n < brk_cur)
        return MMAP_FAILED;
    mmap_cur -= n;
    /* The pages fault in zeroed on first touch (phys_alloc clears frames), so
     * anonymous memory reads as zero without eager mapping. */
    return (long)mmap_cur;
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
#define GFID_MAX 64
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
    if (src->ispipe) {
        struct lpipe *p = &pipes[src->pidx];
        if (src->wr)
            p->writers++;
        else
            p->readers++;
    } else if (src->isram) {
        /* the ramfile persists regardless of fd count: nothing to refcount */
    } else if (src->p9fid) {
        gfidref[src->p9fid - LINUX_FID_BASE]++;
    }
    /* a bare console fd (no backing) needs no refcount */
}

static void
fds_init(void)
{
    memset(fds, 0, sizeof(lproc_cur->fdtab));
    for (int i = 0; i < 3; i++)                     /* stdin/out/err: console */
        fds[i].used = fds[i].iscon = 1;
}

static int
ramfs_find(const char *name)
{
    for (int i = 0; i < RAMFS_MAX; i++)
        if (ramfs[i].used && strcmp(ramfs[i].name, name) == 0)
            return i;
    return -1;
}

static int
ramfs_create(const char *name)
{
    for (int i = 0; i < RAMFS_MAX; i++) {
        if (!ramfs[i].used) {
            size_t n = 0;
            while (name[n] && n < RAMFS_NAME - 1) {
                ramfs[i].name[n] = name[n];
                n++;
            }
            ramfs[i].name[n] = 0;
            ramfs[i].used = 1;
            ramfs[i].data = NULL;
            ramfs[i].len = 0;
            ramfs[i].cap = 0;
            return i;
        }
    }
    return -1;
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
do_pipe2(int *pfd)
{
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
    fds[rfd].ispipe = fds[wfd].ispipe = 1;
    fds[rfd].pidx = fds[wfd].pidx = pidx;
    fds[rfd].wr = 0;
    fds[wfd].wr = 1;

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

static long
do_write(long fd, const char *buf, unsigned long len)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (f->iscon) {                     /* console stdout/stderr */
        for (unsigned long i = 0; i < len; i++)
            con_putc(buf[i]);
        return (long)len;
    }
    if (f->ispipe && f->wr) {
        struct lpipe *p = &pipes[f->pidx];
        unsigned long done = 0;
        /* Write everything, yielding when the ring fills so a reader in another
         * process can drain it. If every reader has closed, report EPIPE. */
        while (done < len) {
            if (p->readers == 0)
                return done ? (long)done : -LERR_PIPE;
            long n = pipe_write(p, (const uint8_t *)buf + done, len - done);
            done += (unsigned long)n;
            if (done < len)
                task_yield();
        }
        return (long)done;
    }
    if (f->isram) {
        struct ramfile *rf = &ramfs[f->ridx];
        uint64_t end = f->pos + len;
        if (end > RAMFS_FILE_MAX || ramfs_grow(rf, (uint32_t)end) < 0)
            return -LERR_NOMEM;
        memcpy(rf->data + f->pos, buf, len);
        f->pos += len;
        if (f->pos > rf->len)
            rf->len = (uint32_t)f->pos;
        return (long)len;
    }
    return -LERR_BADF;                  /* the bundle is read-only */
}

static long
do_writev(long fd, const struct iovec *iov, long cnt)
{
    long total = 0;

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
    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fds[fd].isram = 1;
    fds[fd].ridx = ri;
    fds[fd].pos = (flags & O_APPEND) ? ramfs[ri].len : 0;
    return fd;
}

static long
do_open(const char *path, long flags)
{
    const char *rel = bundle_path(path);
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0;

    /* Writable or newly created paths live in the ramfs. */
    if (writing || (flags & O_CREAT)) {
        int ri = ramfs_find(rel);
        if (ri < 0) {
            if (!(flags & O_CREAT))
                return -LERR_ROFS;      /* writing an existing RO bundle file */
            ri = ramfs_create(rel);
            if (ri < 0)
                return -LERR_NOMEM;
        }
        if (flags & O_TRUNC)
            ramfs[ri].len = 0;
        return open_ram(ri, flags);
    }

    /* Read-only: a ramfs file shadows the bundle; else fall back to 9P. */
    int ri = ramfs_find(rel);
    if (ri >= 0)
        return open_ram(ri, flags);

    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    uint32_t p9fid = fid_alloc();
    if (p9fid == 0)
        return -LERR_NOMEM;

    if (ns_walk(rel, p9fid) != 0)
        return -LERR_NOENT;

    struct p9_stat st;
    int isdir = 0;
    uint64_t size = 0;
    if (p9_stat(p9fid, &st) == 0) {
        isdir = (st.qid.type & P9_QTDIR) != 0;
        size = st.length;
    }
    if ((flags & O_DIRECTORY) && !isdir) {
        p9_clunk(p9fid);
        return -LERR_NOTDIR;
    }
    if (p9_open(p9fid) != 0) {
        p9_clunk(p9fid);
        return -LERR_ACCES;
    }

    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fds[fd].isdir = isdir;
    fds[fd].p9fid = p9fid;
    fds[fd].pos = 0;
    fds[fd].size = size;
    gfidref[p9fid - LINUX_FID_BASE] = 1;
    return fd;
}

static long
do_read(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (f->iscon)
        return 0;                       /* console stdin: EOF */
    if (f->ispipe) {
        if (f->wr)
            return -LERR_BADF;
        struct lpipe *p = &pipes[f->pidx];
        /* Block until data arrives or every writer has closed (then EOF = 0). */
        while (p->count == 0 && p->writers > 0)
            task_yield();
        return pipe_read(p, buf, count);
    }
    if (f->isram) {
        struct ramfile *rf = &ramfs[f->ridx];
        if (f->pos >= rf->len)
            return 0;
        uint32_t avail = rf->len - (uint32_t)f->pos;
        uint32_t n = count < avail ? (uint32_t)count : avail;
        memcpy(buf, rf->data + f->pos, n);
        f->pos += n;
        return n;
    }
    if (f->isdir)
        return -LERR_INVAL;
    long n = p9_read(f->p9fid, f->pos, buf, (uint32_t)count);
    if (n < 0)
        return -LERR_INVAL;
    f->pos += (uint64_t)n;
    return n;
}

static long
do_close(long fd)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (f->iscon) {
        f->used = 0;                    /* just drop this fd; the console stays */
        return 0;
    }
    if (f->isram) {
        f->used = 0;                    /* the ramfile persists for the run */
        return 0;
    }
    if (f->ispipe) {
        struct lpipe *p = &pipes[f->pidx];
        if (f->wr && p->writers > 0)
            p->writers--;
        else if (!f->wr && p->readers > 0)
            p->readers--;
        if (p->readers == 0 && p->writers == 0)
            p->used = 0;
        f->used = 0;
        return 0;
    }
    uint32_t slot = f->p9fid - LINUX_FID_BASE;
    if (gfidref[slot] && --gfidref[slot] == 0)
        p9_clunk(f->p9fid);             /* last reference: release the fid */
    f->used = 0;
    return 0;
}

static long
do_lseek(long fd, long off, long whence)
{
    struct lfd *f = fd_get(fd);

    if (!f || fd < 3)
        return -LERR_BADF;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return -LERR_INVAL;
    uint64_t end = f->isram ? ramfs[f->ridx].len : f->size;
    uint64_t base = (whence == SEEK_CUR) ? f->pos :
                    (whence == SEEK_END) ? end : 0;
    f->pos = base + (uint64_t)off;
    return (long)f->pos;
}

static void
fill_stat(struct linux_stat *s, int isdir, uint64_t size, uint64_t ino)
{
    memset(s, 0, sizeof *s);
    s->st_mode = isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    s->st_nlink = 1;
    s->st_size = (int64_t)size;
    s->st_blksize = 4096;
    s->st_blocks = (int64_t)((size + 511) / 512);
    s->st_ino = ino;
}

/* Resolve a path's metadata (a ramfs file shadows the bundle, else 9P). Returns
 * 0 and fills the out-params, or a negative errno. */
static long
meta_path(const char *path, int *isdir, uint64_t *size, uint64_t *ino)
{
    const char *rel = bundle_path(path);

    int ri = ramfs_find(rel);
    if (ri >= 0) {
        *isdir = 0;
        *size = ramfs[ri].len;
        *ino = (uint64_t)(0x10000 + ri);
        return 0;
    }
    uint32_t fid = LINUX_FID_BASE + GFID_MAX;       /* scratch, above the pool */
    if (ns_walk(rel, fid) != 0)
        return -LERR_NOENT;
    struct p9_stat st;
    int ok = p9_stat(fid, &st) == 0;
    if (ok) {
        *isdir = (st.qid.type & P9_QTDIR) != 0;
        *size = st.length;
        *ino = st.qid.path;
    }
    p9_clunk(fid);
    return ok ? 0 : -LERR_NOENT;
}

/* stat a path (stat/lstat/newfstatat with AT_FDCWD). */
static long
do_stat_path(const char *path, struct linux_stat *out)
{
    int isdir = 0;
    uint64_t size = 0, ino = 1;
    long rc = meta_path(path, &isdir, &size, &ino);

    if (rc == 0)
        fill_stat(out, isdir, size, ino);
    return rc;
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

static void
fill_statx(struct linux_statx *s, int isdir, uint64_t size, uint64_t ino)
{
    memset(s, 0, sizeof *s);
    s->stx_mask = STATX_BASIC_STATS;
    s->stx_blksize = 4096;
    s->stx_nlink = 1;
    s->stx_mode = (uint16_t)(isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    s->stx_ino = ino;
    s->stx_size = size;
    s->stx_blocks = (size + 511) / 512;
}

/* statx(dirfd, path, flags, mask, buf). An empty path with AT_EMPTY_PATH stats
 * the dirfd itself; otherwise the path (ramfs or bundle) is resolved. */
static long
do_statx(long dirfd, const char *path, long flags, struct linux_statx *out)
{
    int isdir = 0;
    uint64_t size = 0, ino = 1;

    if (path && path[0]) {
        long rc = meta_path(path, &isdir, &size, &ino);
        if (rc != 0)
            return rc;
    } else if (flags & AT_EMPTY_PATH) {
        struct lfd *f = fd_get(dirfd);
        if (!f)
            return -LERR_BADF;
        if (f->isram) {
            size = ramfs[f->ridx].len;
            ino = (uint64_t)(0x10000 + f->ridx);
        } else {
            isdir = f->isdir;
            size = f->size;
            ino = f->p9fid;
        }
    } else {
        return -LERR_NOENT;
    }
    fill_statx(out, isdir, size, ino);
    return 0;
}

static long
do_fstat(long fd, struct linux_stat *out)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (f->ispipe) {
        /* Report a FIFO so a tool copies with a read/write loop, not mmap. */
        memset(out, 0, sizeof *out);
        out->st_mode = S_IFIFO | 0644;
        out->st_nlink = 1;
        out->st_blksize = PIPE_CAP;
        out->st_ino = (uint64_t)(0x20000 + f->pidx);
        return 0;
    }
    if (f->iscon) {
        fill_stat(out, 0, 0, (uint64_t)fd);         /* console: a plain reg */
        return 0;
    }
    if (f->isram) {
        fill_stat(out, 0, ramfs[f->ridx].len, (uint64_t)(0x10000 + f->ridx));
        return 0;
    }
    fill_stat(out, f->isdir, f->size, f->p9fid);
    return 0;
}

/* getdents64: read directory entries from the 9P stream at the fd's cursor and
 * pack them as linux_dirent64 records into the user buffer. Advances the cursor
 * only past the entries actually emitted, so the next call resumes correctly. */
static long
do_getdents64(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f || fd < 3)
        return -LERR_BADF;
    if (!f->isdir)
        return -LERR_NOTDIR;

    uint8_t kbuf[4096];
    uint32_t want = sizeof kbuf;
    long got = p9_read(f->p9fid, f->pos, kbuf, want);
    if (got <= 0)
        return 0;                       /* end of directory */

    uint8_t *out = buf;
    unsigned long used = 0;
    uint32_t off = 0, consumed = 0;
    struct p9_stat e;

    while (p9_dir_next(kbuf, (uint32_t)got, &off, &e)) {
        unsigned long nlen = strlen(e.name);
        unsigned long reclen = (19 + nlen + 1 + 7) & ~(unsigned long)7;
        if (used + reclen > count)
            break;                      /* no room: leave for the next call */
        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
        d->d_ino = e.qid.path ? e.qid.path : 1;
        d->d_off = (int64_t)(f->pos + off);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (e.qid.type & P9_QTDIR) ? DT_DIR : DT_REG;
        memcpy(d->d_name, e.name, nlen);
        d->d_name[nlen] = 0;
        used += reclen;
        consumed = off;
    }
    if (used == 0 && off > 0)
        return -LERR_INVAL;             /* a single entry too big for the buffer */
    f->pos += consumed;
    return (long)used;
}

/****************************************************************
 * Processes: fork, wait, exit. Each Linux process is a libOS task with its own
 * address space and file table. They run cooperatively, switching only when a
 * syscall blocks (on a pipe, or in wait4).
 ****************************************************************/

/* The task function for a forked child. Its trapframe was copied from the
 * parent with rax = 0, so it resumes right after fork(), returning 0. */
static void
process_entry(void)
{
    linux_resume(lproc_cur);            /* lproc_cur set by the resume hook */
}

/* Switch the active address space and TLS into p before its task runs
 * (registered with the scheduler as the resume hook). */
static void
linux_on_resume(void *vp)
{
    lproc_cur = (struct lproc *)vp;
    vmspace_switch(lproc_cur->vmspace);
    wrmsr(MSR_FS_BASE, lproc_cur->fs_base);
}

static struct lproc *
proc_alloc(void)
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
        p->pid = i + 1;                 /* slot 0 is pid 1, the first process */
        return p;
    }
    return NULL;
}

/* Release p's open descriptors, dropping the shared pipe and 9P references so a
 * reader sees EOF once every writer is gone. Called on exit and on a failed
 * fork. */
static void
proc_release_fds(struct lproc *p)
{
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &p->fdtab[i];
        if (!f->used || f->iscon)
            continue;
        if (f->ispipe) {
            struct lpipe *pp = &pipes[f->pidx];
            if (f->wr && pp->writers > 0)
                pp->writers--;
            else if (!f->wr && pp->readers > 0)
                pp->readers--;
            if (pp->readers == 0 && pp->writers == 0)
                pp->used = 0;
        } else if (!f->isram && f->p9fid) {
            uint32_t slot = f->p9fid - LINUX_FID_BASE;
            if (gfidref[slot] && --gfidref[slot] == 0)
                p9_clunk(f->p9fid);
        }
        f->used = 0;
    }
}

static void
proc_free(struct lproc *p)
{
    if (p->vmspace)
        vmspace_destroy(p->vmspace);
    if (p->kstack)
        kfree(p->kstack);
    p->used = 0;
}

/* fork: a child process that is a copy of the caller. The child's address space
 * is an eager copy, its trapframe is the parent's with rax = 0, and its open
 * descriptors are the same objects (shared pipe buffers and 9P fids). */
static long
do_fork(void)
{
    struct lproc *parent = lproc_cur;
    struct lproc *child = proc_alloc();

    if (!child)
        return -LERR_NOMEM;
    child->vmspace = vmspace_clone(parent->vmspace);
    if (!child->vmspace) {
        proc_free(child);
        return -LERR_NOMEM;
    }

    /* Copy the trapframe (the first 16 words), then override rax so the child
     * returns 0. kstack_top at offset 128 and the rest stay the child's own. */
    memcpy(child, parent, 16 * sizeof(uint64_t));
    child->tf_rax = 0;
    child->fs_base = parent->fs_base;
    child->parent = parent->pid;
    memcpy(child->fdtab, parent->fdtab, sizeof child->fdtab);
    child->heap_brk = parent->heap_brk;
    child->heap_mmap = parent->heap_mmap;
    child->heap_mmaptop = parent->heap_mmaptop;

    /* Account for the inherited descriptors (the child shares them). */
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &child->fdtab[i];
        if (!f->used || f->iscon)
            continue;
        if (f->ispipe) {
            if (f->wr)
                pipes[f->pidx].writers++;
            else
                pipes[f->pidx].readers++;
        } else if (!f->isram && f->p9fid)
            gfidref[f->p9fid - LINUX_FID_BASE]++;
    }

    int tid = task_spawn_proc("linux", process_entry, child);
    if (tid < 0) {
        proc_release_fds(child);
        proc_free(child);
        return -LERR_NOMEM;
    }
    child->task_id = tid;
    return child->pid;                  /* the parent gets the child's pid */
}

/* wait4: reap a child. Returns its pid and writes an exit-encoded status. With
 * cooperative tasks, yield while children still run so they can finish. */
static long
do_wait4(long pid, int *status)
{
    struct lproc *me = lproc_cur;

    for (;;) {
        int haskid = 0;
        for (int i = 0; i < LINUX_NPROC; i++) {
            struct lproc *c = &procs[i];
            if (!c->used || c->parent != me->pid)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;
            haskid = 1;
            if (c->exited) {
                int rpid = c->pid;
                if (status)
                    *status = (int)((c->exit_status & 0xff) << 8);  /* WIFEXITED */
                task_reap(c->task_id);  /* release the libOS task slot too */
                proc_free(c);
                return rpid;
            }
        }
        if (!haskid)
            return -LERR_CHILD;
        task_yield();                   /* a child is still running: let it run */
    }
}

/* exit: record the status, close descriptors (so pipe readers see EOF), and end
 * the task. The slot lingers as a zombie until the parent's wait4 reaps it. */
static void __attribute__((noreturn))
do_exit(long code)
{
    struct lproc *p = lproc_cur;

    proc_release_fds(p);
    p->exit_status = code;
    p->exited = 1;
    con_puts("[bin] pid ");
    put_dec(p->pid);
    con_puts(" exited, status=");
    put_dec(code);
    con_putc('\n');
    task_exits(NULL);                   /* status kept in the lproc for wait4 */
}

/* The C half of the SYSCALL handler. Return value goes back in rax. */
long
linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;
    switch (nr) {
    case SYS_arch_prctl:
        if (a1 == ARCH_SET_FS) {
            lproc_cur->fs_base = (uint64_t)a2;      /* restored on a switch */
            wrmsr(MSR_FS_BASE, (uint64_t)a2);
            return 0;
        }
        return -LERR_NOSYS;
    case SYS_set_tid_address:
        return 1;                       /* a plausible tid */
    case SYS_set_robust_list:
    case SYS_rseq:
        return 0;                       /* accept and ignore */
    case SYS_mprotect:
        return 0;                       /* flat RW map, nothing to enforce */
    case SYS_brk:
        return do_brk((uint64_t)a1);
    case SYS_mmap:
        return do_mmap((uint64_t)a1, (uint64_t)a2, (uint64_t)a4, a5);
    case SYS_munmap:
        return 0;                       /* bump arena: no reclaim */
    case SYS_time: {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        int64_t t = (int64_t)((((uint64_t)hi << 32) | lo) / 1000000000ULL);
        if (a1)
            *(int64_t *)a1 = t;
        return (long)t;
    }
    case SYS_clock_gettime: {
        /* No real clock: synthesise a monotonic time from the cycle counter,
         * assuming roughly 1 GHz. Good enough to keep libc startup happy. */
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t t = ((uint64_t)hi << 32) | lo;
        uint64_t *ts = (uint64_t *)a2;    /* struct timespec { sec; nsec; } */
        if (ts) {
            ts[0] = t / 1000000000ULL;
            ts[1] = t % 1000000000ULL;
        }
        return 0;
    }
    case SYS_getrandom: {
        uint8_t *buf = (uint8_t *)a1;
        unsigned long len = (unsigned long)a2;
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t s = ((uint64_t)hi << 32) | lo;
        for (unsigned long i = 0; i < len; i++) {
            buf[i] = (uint8_t)s;
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        }
        return (long)len;               /* not cryptographic; a startup seed */
    }
    case SYS_prlimit64: {
        uint64_t *old = (uint64_t *)a4;   /* old_limit: {rlim_cur, rlim_max} */
        if (old) {
            old[0] = ~0ULL;               /* RLIM_INFINITY */
            old[1] = ~0ULL;
        }
        return 0;
    }
    case SYS_ioctl:
        return -LERR_NOTTY;             /* no tty: musl falls back */
    case SYS_read:
        return do_read(a1, (void *)a2, (unsigned long)a3);
    case SYS_write:
        return do_write(a1, (const char *)a2, (unsigned long)a3);
    case SYS_writev:
        return do_writev(a1, (const struct iovec *)a2, a3);
    case SYS_open:
        return do_open((const char *)a1, a2);
    case SYS_openat:
        /* dirfd is ignored: the namespace is effectively rooted, so paths
         * resolve from the 9P root (or absolute). Fine for the flat bundle. */
        return do_open((const char *)a2, a3);
    case SYS_close:
        return do_close(a1);
    case SYS_lseek:
        return do_lseek(a1, a2, a3);
    case SYS_stat:
    case SYS_lstat:
        return do_stat_path((const char *)a1, (struct linux_stat *)a2);
    case SYS_newfstatat:
        /* dirfd ignored (see openat). An empty path with AT_EMPTY_PATH means
         * the dirfd itself, which maps to fstat. */
        if (((const char *)a2)[0] == 0 && (a4 & AT_EMPTY_PATH))
            return do_fstat(a1, (struct linux_stat *)a3);
        return do_stat_path((const char *)a2, (struct linux_stat *)a3);
    case SYS_fstat:
        return do_fstat(a1, (struct linux_stat *)a2);
    case SYS_statx:
        return do_statx(a1, (const char *)a2, a3, (struct linux_statx *)a5);
    case SYS_getdents64:
        return do_getdents64(a1, (void *)a2, (unsigned long)a3);
    case SYS_pipe:
        return do_pipe2((int *)a1);
    case SYS_pipe2:
        return do_pipe2((int *)a1);     /* flags (O_CLOEXEC/O_NONBLOCK) ignored */
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
        if (a2 >= 3 && fds[a2].used)
            do_close(a2);               /* release a real fd; std fds just rebind */
        clone_fd((int)a2, f);
        return a2;
    }
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        return 0;                       /* a single root-ish identity */
    case SYS_prctl:
        return 0;                       /* accept and ignore */
    case SYS_getcwd: {
        char *ubuf = (char *)a1;
        if (a2 < 2)
            return -LERR_INVAL;
        ubuf[0] = '/';
        ubuf[1] = 0;
        return 2;
    }
    case SYS_readlinkat:
        return -LERR_INVAL;             /* nothing in the bundle is a symlink */
    case SYS_fcntl:
        return 0;                       /* minimal: accept F_SETFD/F_SETFL */
    case SYS_execve:
        return do_execve((const char *)a1, (const char *const *)a2,
                         (const char *const *)a3);
    case SYS_fork:
    case SYS_vfork:
        return do_fork();
    case SYS_clone:
        /* glibc/musl fork() is clone with SIGCHLD and no CLONE_VM. A thread
         * (CLONE_VM) is not supported yet; the futex path comes later. */
        if ((uint64_t)a1 & CLONE_VM)
            return -LERR_NOSYS;
        return do_fork();
    case SYS_wait4:
        return do_wait4(a1, (int *)a2);
    case SYS_getpid:
        return lproc_cur->pid;
    case SYS_getppid:
        return lproc_cur->parent;
    case SYS_gettid:
        return lproc_cur->pid;          /* single thread: tid == pid */
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
        return 0;                       /* no signal delivery: accept and ignore */
    case SYS_uname: {
        /* struct utsname: six NUL-terminated 65-byte fields. */
        char *u = (char *)a1;
        if (!u)
            return -LERR_INVAL;
        static const char *const f[6] = {
            "Linux", "jotun", "6.0.0-jotun", "#1 jotun", "x86_64", "(none)",
        };
        for (int i = 0; i < 6; i++) {
            char *d = u + i * 65;
            const char *s = f[i];
            int j = 0;
            while (s[j] && j < 64) { d[j] = s[j]; j++; }
            d[j] = 0;
        }
        return 0;
    }
    case SYS_exit:
    case SYS_exit_group:
        do_exit(a1);                    /* does not return */
    default:
        con_puts("[bin] unhandled syscall ");
        put_dec(nr);
        con_putc('\n');
        return -LERR_NOSYS;
    }
}

/****************************************************************
 * Stack and SYSCALL setup.
 ****************************************************************/

/* Build a System V initial stack at the top of the user window from explicit
 * argv and envp vectors (the stack pages must already be mapped and this
 * process's CR3 active) and return the rsp to enter with: argc, argv[], NULL,
 * envp[], NULL,
 * auxv (AT_PAGESZ, AT_RANDOM, AT_NULL), with the strings and 16 random bytes
 * placed above it. The argv/envp string pointers must stay valid during the
 * build (they come from boot data or an execve snapshot, never live user mem
 * that this overwrites). */
static uint64_t
build_stack_vec(const char *const *argv, int argc,
                const char *const *envp, int envc)
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
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t seed = ((uint64_t)hi << 32) | lo;
    for (int k = 0; k < 16; k++) {
        rnd[k] = (uint8_t)seed;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    /* The control block below that, 16-byte aligned as the entry contract wants
     * rsp (pointing at argc) to be. Words: argc, argv[0..], NULL, envp[0..],
     * NULL, three auxv pairs. */
    int nwords = 1 + argc + 1 + envc + 1 + 6;
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
    blk[w++] = AT_PAGESZ; blk[w++] = 4096;
    blk[w++] = AT_RANDOM; blk[w++] = (uintptr_t)rnd;
    blk[w++] = AT_NULL;   blk[w++] = 0;

    return (uint64_t)(uintptr_t)blk;
}

/* The initial program stack: argv is the loaded file (argv0) followed by the
 * boot arguments after it, no environment. `./jotun libos.elf bin <file>
 * <args...>` thus reaches the program as <file> <args...>. */
static uint64_t
build_stack(const char *argv0)
{
    const char *args[LINUX_MAX_ARGS];
    int argc = 0;

    args[argc++] = argv0;
    for (int i = 2; i < boot_argc() && argc < LINUX_MAX_ARGS; i++)
        args[argc++] = boot_argv(i);
    return build_stack_vec(args, argc, NULL, 0);
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
    while (uvec[n] && n < LINUX_MAX_ARGS - 1) {
        const char *s = uvec[n];
        size_t len = strlen(s) + 1;
        if (off + len > buflen)
            break;
        memcpy(buf + off, s, len);
        out[n] = buf + off;
        off += len;
        n++;
    }
    return n;
}

#define EXEC_STR_MAX 8192

/* execve: replace the running image with a bundle program. The new argv/envp
 * are snapshotted into kernel memory first (they may point into the image we
 * are about to overwrite), then the ELF is loaded over the program window, the
 * heap reset, and a fresh stack built. Control returns to the linux_elf_run
 * loop via linux_exit, which re-enters the new entry. fds and the ramfs persist
 * across the exec, as on Linux. Only bundle files are exec'able for now. */
static long
do_execve(const char *path, const char *const *argv, const char *const *envp)
{
    const char *rel = bundle_path(path);

    if (ns_walk(rel, LINUX_FID) != 0)
        return -LERR_NOENT;
    if (p9_open(LINUX_FID) != 0) {
        p9_clunk(LINUX_FID);
        return -LERR_ACCES;
    }

    static const char *na[LINUX_MAX_ARGS];
    static const char *ne[LINUX_MAX_ARGS];
    char *sbuf = kmalloc(EXEC_STR_MAX);
    if (!sbuf) {
        p9_clunk(LINUX_FID);
        return -LERR_NOMEM;
    }
    int argc = copy_uvec(argv, na, sbuf, EXEC_STR_MAX / 2);
    int envc = copy_uvec(envp, ne, sbuf + EXEC_STR_MAX / 2, EXEC_STR_MAX / 2);
    if (argc == 0) {                    /* execve needs at least argv[0] */
        na[0] = rel;
        argc = 1;
    }

    /* A fresh address space for the new image, replacing the old one. */
    vmspace_t old = lproc_cur->vmspace;
    vmspace_t as = linux_vmspace_setup();
    if (!as) {
        p9_clunk(LINUX_FID);
        kfree(sbuf);
        return -LERR_NOMEM;
    }
    vmspace_switch(as);

    uint64_t entry = elf_load_fid(LINUX_FID, LINUX_PROG_BASE, LINUX_PROG_LIMIT);
    p9_clunk(LINUX_FID);
    if (entry == 0) {
        vmspace_switch(old);            /* keep the old image on failure */
        vmspace_destroy(as);
        kfree(sbuf);
        return -LERR_NOENT;
    }

    lproc_cur->vmspace = as;            /* the resume hook now restores this one */
    brk_cur = LINUX_HEAP_BASE;          /* fresh heap for the new image */
    mmap_cur = mmap_top;
    uint64_t rsp = build_stack_vec(na, argc, ne, envc);
    kfree(sbuf);
    vmspace_destroy(old);               /* release the replaced image's frames */

    linux_jump(entry, rsp);             /* enter the new image; does not return */
}

/* Turn on SYSCALL and point LSTAR at our handler. Our ring-0 CS is 0x08 and SS
 * is 0x10, so STAR's SYSCALL selector field is 0x08 (SYSCALL loads CS=0x08,
 * SS=0x10). SYSRET is unused (we return by jmp), so its field stays 0. */
static void
syscall_arm(void)
{
    wrmsr(MSR_EFER, read_efer() | 1u);          /* EFER.SCE */
    wrmsr(MSR_STAR, (uint64_t)0x08u << 32);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, 0x600u);                  /* mask IF and DF on entry */
}

/* One-time frame-pool init: the free RAM between the kernel image and the vcpu
 * stacks backs every process's private pages and page tables. */
static int phys_ready;

static void
linux_phys_init(void)
{
    if (!phys_ready) {
        phys_init(LINUX_PROG_BASE, VM_USER_HI);
        phys_ready = 1;
    }
}

/* Build a fresh address space with the user stack pre-mapped (so stack growth
 * never faults on the stack page itself). Returns the vmspace, or 0. */
static vmspace_t
linux_vmspace_setup(void)
{
    vmspace_t as = vmspace_new();
    if (!as)
        return 0;
    for (uint64_t v = LINUX_USTACK_TOP - LINUX_USTACK_SIZE;
         v < LINUX_USTACK_TOP; v += VM_PAGE) {
        uint64_t f = phys_alloc();
        if (!f || vmspace_map(as, v, f, VM_W | VM_U) != 0) {
            vmspace_destroy(as);
            return 0;
        }
    }
    return as;
}

void
linux_elf_run(const char *file)
{
    con_puts("[bin] loading ");
    con_puts(file);
    con_putc('\n');

    linux_phys_init();
    task_set_resume_hook(linux_on_resume);

    /* The first process. It runs on the current task (init); forked children
     * become their own tasks. */
    struct lproc *p = proc_alloc();
    if (!p) {
        con_puts("[bin] no process slot\n");
        return;
    }
    p->vmspace = linux_vmspace_setup();
    if (!p->vmspace) {
        con_puts("[bin] no memory for the address space\n");
        proc_free(p);
        return;
    }
    task_set_proc(p);                   /* so the resume hook restores it later */
    lproc_cur = p;
    vmspace_switch(p->vmspace);         /* user window now demand-maps on fault */

    if (ns_walk(file, LINUX_FID) != 0) {
        con_puts("[bin] not found in bundle\n");
        return;
    }
    if (p9_open(LINUX_FID) != 0) {
        p9_clunk(LINUX_FID);
        con_puts("[bin] open failed\n");
        return;
    }
    uint64_t entry = elf_load_fid(LINUX_FID, LINUX_PROG_BASE, LINUX_PROG_LIMIT);
    p9_clunk(LINUX_FID);
    if (entry == 0) {
        con_puts("[bin] load failed (not ET_EXEC at 0x400000?)\n");
        return;
    }
    con_puts("[bin] loaded, entry=");
    con_puthex(entry);
    con_putc('\n');

    fds_init();
    arena_init();
    syscall_arm();
    con_puts("[bin] syscall armed\n");
    uint64_t rsp = build_stack(file);
    con_puts("[bin] stack built\n");

    con_puts("[bin] entering guest\n");
    linux_jump(entry, rsp);             /* enter the program; exit ends the task */
}

#else  /* !__x86_64__ */

void
linux_elf_run(const char *file)
{
    (void)file;
    con_puts("[bin] the Linux binary-compat spike is x86-64 only\n");
}

#endif
