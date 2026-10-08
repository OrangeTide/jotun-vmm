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

#include "elfload.h"
#include "ns.h"
#include "p9.h"
#include "string.h"
#include "jotun_svc.h"       /* JOTUN_PROG_BASE / JOTUN_PROG_SIZE */

#include <stdint.h>

/* The 9P fid used to load the program ELF, distinct from the native loader's
 * (prog.c uses 10), and the pool of fids backing open file descriptors. */
#define LINUX_FID      12u
#define LINUX_FID_BASE 32u          /* open files use fids 32..32+LINUX_NFD */

/* Linux syscall numbers (x86-64) we recognise. */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
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
#define SYS_exit            60
#define SYS_mprotect        10
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
#define SYS_prlimit64       302
#define SYS_getrandom       318
#define SYS_rseq            334

#define ARCH_SET_FS 0x1002

#define AT_FDCWD (-100)

/* open() flags we care about (the read-only bundle rejects writes). */
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_DIRECTORY 0x10000

/* lseek whence. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Linux st_mode type bits and dirent types. */
#define S_IFDIR 0040000
#define S_IFREG 0100000
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

/* x86-64 MSRs for the SYSCALL path. */
#define MSR_EFER     0xC0000080u
#define MSR_STAR     0xC0000081u
#define MSR_LSTAR    0xC0000082u
#define MSR_SFMASK   0xC0000084u
#define MSR_FS_BASE  0xC0000100u

/* The handoff and trap, from x86_64_linuxsys.S. */
extern void linux_syscall_entry(void);
extern long linux_enter(uint64_t entry, uint64_t user_rsp);
extern void linux_exit(long code) __attribute__((noreturn));

/* ELF auxv entry tags. */
#define AT_NULL   0
#define AT_PAGESZ 6
#define AT_RANDOM 25

/* The program's stack. 128 KiB is ample for a hello and keeps us well clear of
 * the loaded image at 0x400000 and the vcpu stacks at the top of RAM. */
static uint8_t linux_ustack[128 * 1024] __attribute__((aligned(16)));

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
    int      isdir;
    int      ispipe;            /* this fd is a pipe end */
    int      pidx;              /* pipe pool index when ispipe */
    int      wr;                /* pipe write end when set, else read end */
    uint32_t p9fid;             /* backing 9P fid, 0 for the console fds */
    uint64_t pos;               /* read cursor (file) or 9P dir offset */
    uint64_t size;
};

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

static struct lfd fds[LINUX_NFD];
static struct lpipe pipes[LINUX_NPIPE];

/* A reference count per backing 9P fid, indexed by (p9fid - LINUX_FID_BASE),
 * so dup can share a fid and it is clunked only when the last fd closes. */
static uint8_t fidref[LINUX_NFD];

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

static void
fds_init(void)
{
    memset(fds, 0, sizeof fds);
    memset(fidref, 0, sizeof fidref);
    memset(pipes, 0, sizeof pipes);
    fds[0].used = fds[1].used = fds[2].used = 1;    /* stdin/out/err */
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

    if (fd == 1 || fd == 2) {           /* console */
        for (unsigned long i = 0; i < len; i++)
            con_putc(buf[i]);
        return (long)len;
    }
    if (f && f->ispipe && f->wr)
        return pipe_write(&pipes[f->pidx], buf, len);
    return -LERR_BADF;                  /* the bundle is read-only */
}

static long
do_writev(long fd, const struct iovec *iov, long cnt)
{
    long total = 0;

    if (fd != 1 && fd != 2)
        return -LERR_BADF;
    for (long i = 0; i < cnt; i++) {
        long n = do_write(fd, iov[i].base, iov[i].len);
        if (n < 0)
            return n;
        total += n;
    }
    return total;
}

static long
do_open(const char *path, long flags)
{
    const char *rel = bundle_path(path);

    if (flags & (O_WRONLY | O_RDWR))
        return -LERR_ROFS;              /* read-only bundle */

    int fd = fd_alloc();
    if (fd < 0)
        return -LERR_NOMEM;
    uint32_t p9fid = LINUX_FID_BASE + (uint32_t)fd;

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

    fds[fd].used = 1;
    fds[fd].isdir = isdir;
    fds[fd].p9fid = p9fid;
    fds[fd].pos = 0;
    fds[fd].size = size;
    fidref[p9fid - LINUX_FID_BASE] = 1;
    return fd;
}

static long
do_read(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f || fd < 3)
        return -LERR_BADF;              /* no stdin source */
    if (f->ispipe)
        return f->wr ? -LERR_BADF : pipe_read(&pipes[f->pidx], buf, count);
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
    if (fd < 3)
        return 0;                       /* keep the console open */
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
    if (fidref[slot] && --fidref[slot] == 0)
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
    uint64_t base = (whence == SEEK_CUR) ? f->pos :
                    (whence == SEEK_END) ? f->size : 0;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return -LERR_INVAL;
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

/* stat a path (stat/lstat/newfstatat with AT_FDCWD). Uses a scratch fid. */
static long
do_stat_path(const char *path, struct linux_stat *out)
{
    const char *rel = bundle_path(path);
    uint32_t fid = LINUX_FID_BASE + LINUX_NFD;      /* scratch, above the pool */

    if (ns_walk(rel, fid) != 0)
        return -LERR_NOENT;
    struct p9_stat st;
    int ok = p9_stat(fid, &st) == 0;
    long rc = -LERR_NOENT;
    if (ok) {
        fill_stat(out, (st.qid.type & P9_QTDIR) != 0, st.length, st.qid.path);
        rc = 0;
    }
    p9_clunk(fid);
    return rc;
}

static long
do_fstat(long fd, struct linux_stat *out)
{
    struct lfd *f = fd_get(fd);

    if (!f)
        return -LERR_BADF;
    if (fd < 3) {
        fill_stat(out, 0, 0, (uint64_t)fd);         /* console: a char device-ish reg */
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

/* The C half of the SYSCALL handler. Return value goes back in rax. */
long
linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;
    switch (nr) {
    case SYS_arch_prctl:
        if (a1 == ARCH_SET_FS) {
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
        if (a1 != AT_FDCWD)
            return -LERR_NOSYS;         /* only cwd-relative/absolute for now */
        return do_open((const char *)a2, a3);
    case SYS_close:
        return do_close(a1);
    case SYS_lseek:
        return do_lseek(a1, a2, a3);
    case SYS_stat:
    case SYS_lstat:
        return do_stat_path((const char *)a1, (struct linux_stat *)a2);
    case SYS_newfstatat:
        if (a1 != AT_FDCWD)
            return -LERR_NOSYS;
        return do_stat_path((const char *)a2, (struct linux_stat *)a3);
    case SYS_fstat:
        return do_fstat(a1, (struct linux_stat *)a2);
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
        fds[nfd] = *f;                  /* shares the backing 9P fid */
        fidref[f->p9fid - LINUX_FID_BASE]++;
        return nfd;
    }
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
    case SYS_exit:
    case SYS_exit_group:
        linux_exit(a1);                 /* does not return */
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

/* Build a System V initial stack in linux_ustack and return the rsp to enter
 * with: argc, argv[], NULL, envp[]=NULL, auxv, with the argv strings and 16
 * random bytes placed above it. argc is 1 (the program name). */
static uint64_t
build_stack(const char *argv0)
{
    uint8_t *top = linux_ustack + sizeof linux_ustack;

    /* Copy the program name string near the top. */
    char *name = (char *)(top - 64);
    int i = 0;
    while (argv0[i] && i < 48) {
        name[i] = argv0[i];
        i++;
    }
    name[i] = 0;

    /* 16 bytes for AT_RANDOM, seeded from the cycle counter. */
    uint8_t *rnd = (uint8_t *)(top - 96);
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t seed = ((uint64_t)hi << 32) | lo;
    for (int k = 0; k < 16; k++) {
        rnd[k] = (uint8_t)seed;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    /* The control block, 16-byte aligned, below the strings. Layout:
     *   argc, argv[0], NULL(argv end), NULL(envp end), auxv..., AT_NULL */
    uint64_t *blk = (uint64_t *)((uintptr_t)(top - 256) & ~(uintptr_t)0xF);
    int w = 0;
    blk[w++] = 1;                       /* argc */
    blk[w++] = (uintptr_t)name;         /* argv[0] */
    blk[w++] = 0;                       /* argv terminator */
    blk[w++] = 0;                       /* envp terminator (no env) */
    blk[w++] = AT_PAGESZ; blk[w++] = 4096;
    blk[w++] = AT_RANDOM; blk[w++] = (uintptr_t)rnd;
    blk[w++] = AT_NULL;   blk[w++] = 0;

    return (uint64_t)(uintptr_t)blk;
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

void
linux_elf_run(const char *file)
{
    con_puts("[bin] loading ");
    con_puts(file);
    con_putc('\n');

    if (ns_walk(file, LINUX_FID) != 0) {
        con_puts("[bin] not found in bundle\n");
        return;
    }
    if (p9_open(LINUX_FID) != 0) {
        p9_clunk(LINUX_FID);
        con_puts("[bin] open failed\n");
        return;
    }
    uint64_t entry = elf_load_fid(LINUX_FID, JOTUN_PROG_BASE,
                                  JOTUN_PROG_BASE + JOTUN_PROG_SIZE);
    p9_clunk(LINUX_FID);
    if (entry == 0) {
        con_puts("[bin] load failed (not ET_EXEC at 0x400000?)\n");
        return;
    }
    con_puts("[bin] loaded, entry=");
    con_puthex(entry);
    con_putc('\n');

    fds_init();
    syscall_arm();
    con_puts("[bin] syscall armed\n");
    uint64_t rsp = build_stack(file);
    con_puts("[bin] stack built\n");

    con_puts("[bin] entering guest\n");
    long code = linux_enter(entry, rsp);
    sti();                              /* linux_enter ran with interrupts off */

    con_puts("[bin] exited, status=");
    put_dec(code);
    con_putc('\n');
}

#else  /* !__x86_64__ */

void
linux_elf_run(const char *file)
{
    (void)file;
    con_puts("[bin] the Linux binary-compat spike is x86-64 only\n");
}

#endif
