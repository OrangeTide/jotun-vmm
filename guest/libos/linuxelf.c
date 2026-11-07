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

#include "boot.h"
#include "elfload.h"
#include "mem.h"
#include "ns.h"
#include "p9.h"
#include "string.h"
#include "task.h"
#include "jotun_svc.h"       /* JOTUN_PROG_BASE */
#include "vmspace.h"
#include "linux_arch.h"

#include <stdint.h>

/* The 9P fid used to load the program ELF, distinct from the native loader's
 * (prog.c uses 10), and the pool of fids backing open file descriptors. */
#define LINUX_FID      12u
#define LINUX_IFID     13u          /* loads the PT_INTERP dynamic linker */
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
#define LERR_EXIST  17          /* EEXIST */
#define LERR_XDEV   18          /* EXDEV: cross-device link/rename */
#define LERR_ISDIR  21          /* EISDIR */
#define LERR_NOTEMPTY 39        /* ENOTEMPTY */

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

/* The writable in-memory overlay: files and directories created or written by
 * the guest, keyed by normalized bundle-relative path ("" is the root). A
 * compiler toolchain and a configure run create many files across a few
 * directories, so the table is generous and names hold a nested path. */
#define RAMFS_MAX     256
#define RAMFS_NAME    192
#define RAMFS_FILE_MAX (4u * 1024u * 1024u)     /* per-file cap */

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
    /* Directory enumeration state (getdents): a directory may have both 9P and
     * writable-overlay entries, so it is listed in two phases. dpath is the
     * directory's normalized key; d9done marks the 9P phase finished; dov is the
     * overlay scan cursor. */
    char     dpath[RAMFS_NAME];
    int      d9done;
    uint32_t dov;
    uint64_t size;
    uint64_t ino;               /* stable identity (9P qid path): a fresh fd for
                                 * the same file reports the same inode, which
                                 * the dynamic linker relies on to dedup a
                                 * library it has already mapped */
};

/* A writable overlay entry: a file (buffer grows on write, persists across
 * close for the run) or a directory. The read-only 9P bundle and host mounts
 * cannot hold new files, so anything created or written goes here, shadowing a
 * bundle path of the same name. See the RAMFS_* sizes above. */
struct ramfile {
    int      used;
    int      is_dir;
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
     * heap_mmaptop. A bump allocator; munmap does not reclaim. */
    uint64_t heap_base, heap_brk, heap_mmap, heap_mmaptop;
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
};

_Static_assert(__builtin_offsetof(struct lproc, tf) == 0, "trapframe first");
_Static_assert(__builtin_offsetof(struct lproc, kstack_top) == sizeof(struct trapframe),
               "kstack_top follows the trapframe (the assembly's TF_KSTACK)");

static struct lshared shareds[LINUX_NPROC];
static struct lproc procs[LINUX_NPROC];

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

static long do_execve(const char *path, const char *const *argv,
                      const char *const *envp);
static struct lfd *fd_get(long fd);
static vmspace_t linux_vmspace_setup(void);
/* linux_jump is declared by the arch backend (linux_arch.h); linux_resume takes
 * struct lproc, which is defined here, so it is declared here. */
extern void linux_resume(struct lproc *p) __attribute__((noreturn));

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
    mmap_top = (LINUX_USTACK_TOP - LINUX_USTACK_SIZE) & ~(PAGE_SIZE - 1);
    mmap_cur = mmap_top;
}

/* The highest address occupied by the loaded image: the interpreter's end when
 * present (it loads above the executable), otherwise the executable's. */
static uint64_t
image_top(const struct elf_info *exe, const struct elf_info *interp)
{
    return interp->has_interp ? interp->load_end : exe->load_end;
}

static long
do_brk(uint64_t want)
{
    if (want == 0)
        return (long)brk_cur;
    if (want < heap_base || want > mmap_cur)
        return (long)brk_cur;           /* refuse; caller falls back to mmap */
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
    if (!f || f->iscon || f->ispipe || f->isdir)
        return -LERR_BADF;
    uint8_t *dst = (uint8_t *)(uintptr_t)va;
    if (f->isram) {                     /* ramfs file: copy from its buffer */
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
        if (n > mmap_cur - brk_cur)     /* overflow-safe: mmap_cur > brk_cur */
            return MMAP_FAILED;
        mmap_cur -= n;
        va = mmap_cur;
        /* Fresh descending address: never mapped before, so an anonymous
         * mapping already reads as zero on fault and needs no eager backing. */
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
    memset(fds, 0, sizeof(lproc_cur->sh->fdtab));
    for (int i = 0; i < 3; i++)                     /* stdin/out/err: console */
        fds[i].used = fds[i].iscon = 1;
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

static int
ramfs_find(const char *name)
{
    char key[RAMFS_NAME];
    norm_key(name, key, sizeof key);
    for (int i = 0; i < RAMFS_MAX; i++)
        if (ramfs[i].used && strcmp(ramfs[i].name, key) == 0)
            return i;
    return -1;
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
            ramfs[i].data = NULL;
            ramfs[i].len = 0;
            ramfs[i].cap = 0;
            return i;
        }
    }
    return -1;
}

/* Remove an overlay entry by name. Returns 0 if one was removed, -1 if absent.
 * The buffer is kept (freed lazily on reuse) so a stale pointer is never left. */
static int
ramfs_remove(const char *name)
{
    int i = ramfs_find(name);
    if (i < 0)
        return -1;
    ramfs[i].used = 0;
    ramfs[i].len = 0;
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
        if (!ramfs[i].used)
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
        if (ramfs[i].used && strcmp(ramfs[i].name, key) == 0)
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

/* Record a directory's normalized key on its fd, for the overlay phase of
 * getdents. d9done starts set when there is no 9P side to enumerate. */
static void
fd_set_dir(struct lfd *f, const char *key)
{
    size_t i = 0;
    while (key[i] && i < RAMFS_NAME - 1) {
        f->dpath[i] = key[i];
        i++;
    }
    f->dpath[i] = 0;
    f->d9done = f->p9fid ? 0 : 1;
    f->dov = 0;
}

static long
do_open(const char *path, long flags)
{
    char key[RAMFS_NAME];
    norm_key(path, key, sizeof key);
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0;

    int ri = ramfs_find(key);               /* overlay entry (file or dir) */
    int is_ovl_dir = (ri >= 0 && ramfs[ri].is_dir);

    /* Writing or creating a plain file lands in the writable overlay. */
    if (writing || (flags & O_CREAT)) {
        if (is_ovl_dir || (ri < 0 && ramfs_is_dir_key(key)))
            return -LERR_ISDIR;
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
    int isdir = is_ovl_dir || ramfs_is_dir_key(key) || (have9 && isdir9);

    if (!isdir && !have9) {
        p9_clunk(p9fid);
        return -LERR_NOENT;
    }
    if ((flags & O_DIRECTORY) && !isdir) {
        p9_clunk(p9fid);
        return -LERR_NOTDIR;
    }

    memset(&fds[fd], 0, sizeof fds[fd]);
    fds[fd].used = 1;
    fds[fd].isdir = isdir;
    fds[fd].size = size;
    fds[fd].ino = ino;
    /* Keep a 9P fid open when there is a 9P file to read, or a 9P directory to
     * enumerate; an overlay-only directory needs none. */
    if (have9 && (!isdir || isdir9)) {
        if (p9_open(p9fid) != 0) {
            p9_clunk(p9fid);
            fds[fd].used = 0;
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

/* pread: read at an explicit offset without moving the fd cursor. The dynamic
 * linker reads library program headers this way. Pipes and the console have no
 * position, so they are rejected. */
static long
do_pread(long fd, void *buf, unsigned long count, uint64_t off)
{
    struct lfd *f = fd_get(fd);

    if (!f || f->iscon || f->ispipe || f->isdir)
        return -LERR_BADF;
    if (f->isram) {
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
    if (f->p9fid) {                     /* an overlay-only directory has none */
        uint32_t slot = f->p9fid - LINUX_FID_BASE;
        if (gfidref[slot] && --gfidref[slot] == 0)
            p9_clunk(f->p9fid);         /* last reference: release the fid */
    }
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
    char key[RAMFS_NAME];
    norm_key(path, key, sizeof key);

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
    uint32_t fid = LINUX_FID_BASE + GFID_MAX;       /* scratch, above the pool */
    if (ns_walk(key, fid) != 0)
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

/* mkdir: create a writable-overlay directory. Fails if anything already
 * resolves at the path (an overlay entry, a bundle path, or an implicit
 * directory). mkdir -p ignores the resulting EEXIST. */
static long
do_mkdir(const char *path)
{
    char key[RAMFS_NAME];
    norm_key(path, key, sizeof key);
    if (key[0] == 0)
        return -LERR_EXIST;             /* the root always exists */
    int isdir;
    uint64_t sz, ino;
    if (meta_path(key, &isdir, &sz, &ino) == 0)
        return -LERR_EXIST;
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
    norm_key(path, key, sizeof key);
    if (key[0] == 0)
        return -LERR_ACCES;
    if (ramfs_has_desc(key))
        return -LERR_NOTEMPTY;
    int ri = ramfs_find(key);
    if (ri >= 0 && ramfs[ri].is_dir) {
        ramfs[ri].used = 0;
        return 0;
    }
    int isdir;
    uint64_t sz, ino;
    if (meta_path(key, &isdir, &sz, &ino) != 0)
        return -LERR_NOENT;
    return isdir ? -LERR_ROFS : -LERR_NOTDIR;
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
    norm_key(oldp, ok, sizeof ok);
    norm_key(newp, nk, sizeof nk);
    if (ok[0] == 0 || nk[0] == 0)
        return -LERR_ACCES;

    int ri = ramfs_find(ok);
    if (ri < 0) {
        int isdir;
        uint64_t sz, ino;
        if (meta_path(ok, &isdir, &sz, &ino) == 0)
            return -LERR_XDEV;          /* read-only source: cannot rename */
        return -LERR_NOENT;
    }

    int di = ramfs_find(nk);            /* overwrite an existing destination */
    if (di >= 0 && di != ri)
        ramfs[di].used = 0;

    if (ramfs[ri].is_dir) {             /* carry the subtree: rewrite prefixes */
        size_t ol = strlen(ok);
        for (int i = 0; i < RAMFS_MAX; i++) {
            if (!ramfs[i].used || i == ri)
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
    fill_stat(out, f->isdir, f->size, f->ino);
    return 0;
}

/* Pack one getdents64 record into buf at `used`. Returns the new `used`, or 0
 * if it would overflow `count` (the record is left for the next call). */
static unsigned long
dirent_emit(uint8_t *out, unsigned long used, unsigned long count,
            uint64_t ino, int64_t off, int is_dir, const char *name)
{
    unsigned long nlen = strlen(name);
    unsigned long reclen = (19 + nlen + 1 + 7) & ~(unsigned long)7;
    if (used + reclen > count)
        return 0;
    struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
    d->d_ino = ino ? ino : 1;
    d->d_off = off;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = is_dir ? DT_DIR : DT_REG;
    memcpy(d->d_name, name, nlen);
    d->d_name[nlen] = 0;
    return used + reclen;
}

/* getdents64 over a directory that may have both a read-only 9P side and a
 * writable overlay side. The 9P entries come first (phase 1, cursor in f->pos),
 * then the overlay's direct children (phase 2, cursor in f->dov). Each call
 * emits as many whole records as fit and resumes where it left off; a return of
 * 0 means the directory is exhausted. Overlay and 9P are not deduplicated, so a
 * created file that shadows a bundle file of the same name lists twice. */
static long
do_getdents64(long fd, void *buf, unsigned long count)
{
    struct lfd *f = fd_get(fd);

    if (!f || fd < 3)
        return -LERR_BADF;
    if (!f->isdir)
        return -LERR_NOTDIR;

    uint8_t *out = buf;
    unsigned long used = 0;

    /* Phase 1: the 9P directory stream. */
    if (f->p9fid && !f->d9done) {
        uint8_t kbuf[4096];
        long got = p9_read(f->p9fid, f->pos, kbuf, sizeof kbuf);
        if (got > 0) {
            uint32_t off = 0, consumed = 0;
            struct p9_stat e;
            while (p9_dir_next(kbuf, (uint32_t)got, &off, &e)) {
                unsigned long u = dirent_emit(out, used, count,
                                              e.qid.path, (int64_t)(f->pos + off),
                                              (e.qid.type & P9_QTDIR) != 0, e.name);
                if (u == 0)
                    break;              /* no room: leave for the next call */
                used = u;
                consumed = off;
            }
            if (used == 0 && off > 0)
                return -LERR_INVAL;     /* a single entry too big for the buffer */
            f->pos += consumed;
            if (used > 0)
                return (long)used;      /* more 9P may remain; caller re-enters */
        }
        f->d9done = 1;                  /* 9P stream exhausted */
    }

    /* Phase 2: the writable overlay's direct children of this directory. */
    for (int i = (int)f->dov; i < RAMFS_MAX; i++) {
        f->dov = (uint32_t)i;
        if (!ramfs[i].used)
            continue;
        const char *leaf = ramfs_child_leaf(ramfs[i].name, f->dpath);
        if (!leaf)
            continue;
        unsigned long u = dirent_emit(out, used, count, (uint64_t)(0x10000 + i),
                                      (int64_t)(i + 1), ramfs[i].is_dir, leaf);
        if (u == 0)
            return used ? (long)used : -LERR_INVAL;     /* resume at i next call */
        used = u;
    }
    f->dov = RAMFS_MAX;                  /* overlay exhausted */
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

/* Futex: a small wait queue keyed by (process, user address). Enough for the
 * pthread primitives (mutex, cond, join), which use private futexes. */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

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

    if (cmd == FUTEX_WAIT) {
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
        fwaiters[slot].used = 1;
        fwaiters[slot].sh = lproc_cur->sh;
        fwaiters[slot].uaddr = uaddr;
        fwaiters[slot].task_id = lproc_cur->task_id;
        task_block();                   /* woken by futex_wake or thread exit */
        fwaiters[slot].used = 0;
        return 0;
    }
    if (cmd == FUTEX_WAKE)
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
    csh->heap_brk = parent->sh->heap_brk;
    csh->heap_mmap = parent->sh->heap_mmap;
    csh->heap_mmaptop = parent->sh->heap_mmaptop;

    /* Account for the inherited descriptors (the child shares them). */
    for (int i = 0; i < LINUX_NFD; i++) {
        struct lfd *f = &csh->fdtab[i];
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

    /* The main thread resumes at the fork() site on the cloned stack, rax = 0. */
    child->tf = parent->tf;
    tf_set_ret0(&child->tf);
    child->tls = parent->tls;

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
    inherit_fds(csh);
    csh->heap_brk = parent->sh->heap_brk;
    csh->heap_mmap = parent->sh->heap_mmap;
    csh->heap_mmaptop = parent->sh->heap_mmaptop;

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

    /* vfork: block until the child execs (new address space) or exits. */
    while (csh->vforking)
        task_block();

    return csh->pid;
}

/* clone3(&args, size): parse the clone_args and route like clone. The struct is
 * a run of u64s; we read flags, the stack and its size, and tls. */
static long
do_clone3(uint64_t uargs, uint64_t size)
{
    if (size < 8 * 8)
        return -LERR_INVAL;
    const uint64_t *a = (const uint64_t *)(uintptr_t)uargs;
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
 * status. Yields while children still run so they can finish. */
static long
do_wait4(long pid, int *status)
{
    struct lshared *mysh = lproc_cur->sh;

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
                    *status = (int)((c->exit_status & 0xff) << 8);  /* WIFEXITED */
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
        task_yield();                   /* a child is still running: let it run */
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

    /* A joined thread clears its tid word and wakes whoever waits on it. */
    if (t->clear_ctid) {
        *(volatile int *)(uintptr_t)t->clear_ctid = 0;
        futex_wake(t->clear_ctid, 1);
    }
    t->exited = 1;
    if (group) {
        sh->exited = 1;
        sh->exit_status = code;
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
    }

    /* This thread cannot free its own kstack (it runs on it); hand it to the
     * zombie-thread drainer, which another thread reaps. */
    if (n_zombie_thr < LINUX_NPROC)
        zombie_thr[n_zombie_thr++] = t;
    task_exits(NULL);
}

/* The C half of the SYSCALL handler. Return value goes back in rax. */
long
linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
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
        return 0;                       /* flat RW map, nothing to enforce */
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
        return 0;                       /* bump arena: no reclaim */
    case SYS_time: {
        int64_t t = (int64_t)(arch_cycles() / 1000000000ULL);
        if (a1)
            *(int64_t *)a1 = t;
        return (long)t;
    }
    case SYS_clock_gettime: {
        /* No real clock: synthesise a monotonic time from the cycle counter,
         * assuming roughly 1 GHz. Good enough to keep libc startup happy. */
        uint64_t t = arch_cycles();
        uint64_t *ts = (uint64_t *)a2;    /* struct timespec { sec; nsec; } */
        if (ts) {
            ts[0] = t / 1000000000ULL;
            ts[1] = t % 1000000000ULL;
        }
        return 0;
    }
    case SYS_gettimeofday: {
        uint64_t t = arch_cycles();
        uint64_t *tv = (uint64_t *)a1;    /* struct timeval { sec; usec; } */
        if (tv) {
            tv[0] = t / 1000000000ULL;
            tv[1] = (t / 1000ULL) % 1000000ULL;
        }
        return 0;
    }
    case SYS_socket:
        return -LERR_NOSYS;             /* no sockets in this guest; quiet */
    case SYS_getrandom: {
        uint8_t *buf = (uint8_t *)a1;
        unsigned long len = (unsigned long)a2;
        uint64_t s = arch_cycles();
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
    case SYS_pread64:
        return do_pread(a1, (void *)a2, (unsigned long)a3, (uint64_t)a4);
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
    case SYS_statfs:
    case SYS_fstatfs:
    case SYS_mincore:
        /* Nothing to describe (no real filesystem; no page residency to report).
         * Report ENOSYS quietly: callers such as libselinux probing a filesystem
         * type, or a residency hint, fall back cleanly, and a logged "unhandled"
         * line here would pollute a tool's output stream. */
        return -LERR_NOSYS;
    case SYS_getgroups:
        return 0;                       /* no supplementary groups */
    case SYS_getpgrp:
        return 1;                       /* a single process group */
    case SYS_getrusage: {
        /* usage is the second argument; who (a1) is ignored. Zeroed counters:
         * no CPU-time accounting, which gcc and as query but tolerate. */
        uint64_t *ru = (uint64_t *)a2;
        if (ru)
            for (int i = 0; i < 18; i++)        /* struct rusage is 18 longs */
                ru[i] = 0;
        return 0;
    }
    case SYS_sysinfo: {
        /* Report the configured RAM as total and a fixed free fraction, zero for
         * everything else. cc1 reads this to size its garbage collector; the
         * numbers need only be plausible. struct sysinfo layout: uptime, three
         * load averages, totalram, freeram, then fields this leaves zero. */
        uint64_t *si = (uint64_t *)a1;
        if (si) {
            for (int i = 0; i < 14; i++)
                si[i] = 0;
            si[4] = VM_RAM_SIZE;                /* totalram */
            si[5] = VM_RAM_SIZE / 2;            /* freeram (a plausible fraction) */
        }
        return 0;
    }
    case SYS_sigaltstack:
        return 0;                       /* no signal delivery: accept and ignore */
    case SYS_umask:
        return 022;                     /* report a conventional prior mask */
    case SYS_chmod:
    case SYS_fchmod:
    case SYS_fchmodat:
        return 0;                       /* no permission bits to enforce */
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
        char *ubuf = (char *)a1;
        if (a2 < 2)
            return -LERR_INVAL;
        ubuf[0] = '/';
        ubuf[1] = 0;
        return 2;
    }
    case SYS_chdir: {
        /* The cwd is always "/" (getcwd above), so this cannot really move; but
         * accept a chdir to the root or to a directory that exists, so a tool
         * restoring its cwd at exit (make does) succeeds quietly. */
        const char *rel = bundle_path((const char *)a1);
        if (rel[0] == '\0')
            return 0;                   /* the root */
        int isdir;
        uint64_t size, ino;
        if (meta_path((const char *)a1, &isdir, &size, &ino) < 0)
            return -LERR_NOENT;
        return isdir ? 0 : -LERR_NOTDIR;
    }
    case SYS_readlink:
    case SYS_readlinkat:
        return -LERR_INVAL;             /* nothing in the bundle is a symlink;
                                         * gcc probes /proc/self/exe and falls
                                         * back to its default prefix */
    case SYS_unlink:
    case SYS_unlinkat: {
        /* unlink(path): path is a1. unlinkat(dirfd, path, flags): path is a2,
         * dirfd is treated as AT_FDCWD. AT_REMOVEDIR (0x200) routes to rmdir.
         * Only the writable overlay can be removed; the 9P bundle and host
         * mounts are read-only. ld, configure, and make all unlink temporaries,
         * which live in the overlay. */
        const char *upath = (nr == SYS_unlink) ? (const char *)a1
                                               : (const char *)a2;
        if (nr == SYS_unlinkat && (a3 & 0x200))
            return do_rmdir(upath);
        if (ramfs_remove(bundle_path(upath)) == 0)
            return 0;
        int isdir;
        uint64_t size, ino;
        if (meta_path(upath, &isdir, &size, &ino) == 0)
            return isdir ? -LERR_ISDIR : -LERR_ROFS;
        return -LERR_NOENT;
    }
    case SYS_mkdir:
        return do_mkdir((const char *)a1);
    case SYS_mkdirat:
        return do_mkdir((const char *)a2);      /* dirfd is AT_FDCWD */
    case SYS_rmdir:
        return do_rmdir((const char *)a1);
    case SYS_rename:
        return do_rename((const char *)a1, (const char *)a2);
    case SYS_renameat:
        return do_rename((const char *)a2, (const char *)a4);   /* dirfds AT_FDCWD */
    case SYS_renameat2:
        return do_rename((const char *)a2, (const char *)a4);   /* flags ignored */
    case SYS_access: {
        /* access(path, mode): path is the first argument. The dynamic linker
         * probes /etc/ld.so.cache and candidate library directories this way. */
        int isdir;
        uint64_t size, ino;
        long r = meta_path((const char *)a1, &isdir, &size, &ino);
        return r < 0 ? r : 0;
    }
    case SYS_faccessat:
    case SYS_faccessat2: {
        /* faccessat(dirfd, path, mode, ...): the bundle and ramfs carry no
         * permission bits, so an existing path is accessible for any mode.
         * Paths are resolved against the bundle root; dirfd is AT_FDCWD in
         * practice. make checks its shell this way before execve. */
        int isdir;
        uint64_t size, ino;
        long r = meta_path((const char *)a2, &isdir, &size, &ino);
        return r < 0 ? r : 0;
    }
    case SYS_fcntl: {
        /* F_DUPFD (0) and F_DUPFD_CLOEXEC (1030) duplicate a1 to the lowest
         * free fd >= a3. bash uses this to save and restore a redirected fd
         * (e.g. `echo > file` saves fd 1), so returning a real new fd matters;
         * the old stub returned 0, which bash then treated as the saved fd.
         * Other commands (F_GETFD/F_SETFD/F_GETFL/F_SETFL) are accepted. */
        if (a2 == 0 || a2 == 1030) {
            struct lfd *src = fd_get(a1);
            if (!src)
                return -LERR_BADF;
            int lo = a3 < 3 ? 3 : (int)a3;
            for (int i = lo; i < LINUX_NFD; i++) {
                if (!fds[i].used) {
                    clone_fd(i, src);
                    return i;
                }
            }
            return -LERR_NOMEM;         /* EMFILE: no free descriptor */
        }
        return 0;
    }
    case SYS_execve:
        return do_execve((const char *)a1, (const char *const *)a2,
                         (const char *const *)a3);
    case SYS_fork:
    case SYS_vfork:
        return do_fork();
    case SYS_clone:
        /* CLONE_VM|CLONE_VFORK is posix_spawn; CLONE_VM alone is a thread;
         * otherwise clone is the fork path (fork() is clone with SIGCHLD and no
         * CLONE_VM). */
        if (((uint64_t)a1 & CLONE_VM) && ((uint64_t)a1 & CLONE_VFORK))
            return do_spawn_vfork((uint64_t)a1, (uint64_t)a2);
        if ((uint64_t)a1 & CLONE_VM)
            return do_clone_thread((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                                   (uint64_t)a4, (uint64_t)a5);
        return do_fork();
    case SYS_clone3:
        return do_clone3((uint64_t)a1, (uint64_t)a2);
    case SYS_wait4:
        return do_wait4(a1, (int *)a2);
    case SYS_futex:
        return do_futex((uint64_t)a1, (int)a2, (int)a3);
    case SYS_sched_yield:
        task_yield();
        return 0;
    case SYS_getpid:
        return lproc_cur->sh->pid;
    case SYS_getppid:
        return lproc_cur->sh->parent;
    case SYS_gettid:
        return lproc_cur->tid;
    case SYS_tgkill:
        return 0;                       /* no signal delivery: accept and ignore */
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
    blk[w++] = AT_PAGESZ; blk[w++] = 4096;
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
    int argc = 0;

    args[argc++] = argv0;
    for (int i = 2; i < boot_argc() && argc < LINUX_MAX_ARGS; i++)
        args[argc++] = boot_argv(i);
    return build_stack_vec(args, argc, NULL, 0, exe, interp);
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
load_program(struct elf_info *exe, struct elf_info *interp, uint64_t *entry)
{
    if (elf_load_ex(LINUX_FID, VM_USER_LO, VM_USER_HI, LINUX_PROG_BASE, exe) != 0)
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

    /* A fresh address space for the new image, replacing the old one. A
     * posix_spawn child runs in its parent's borrowed space, which execve must
     * not destroy and which the parent reclaims itself. */
    vmspace_t old = lproc_cur->sh->vmspace;
    int borrowed = lproc_cur->sh->vm_borrowed;
    vmspace_t as = linux_vmspace_setup();
    if (!as) {
        p9_clunk(LINUX_FID);
        kfree(sbuf);
        return -LERR_NOMEM;
    }
    vmspace_switch(as);

    struct elf_info exe, interp;
    uint64_t entry;
    int lr = load_program(&exe, &interp, &entry);
    p9_clunk(LINUX_FID);
    if (lr != 0) {
        vmspace_switch(old);            /* keep the old image on failure */
        vmspace_destroy(as);
        kfree(sbuf);
        return -LERR_NOENT;
    }

    lproc_cur->sh->vmspace = as;        /* the resume hook now restores this one */
    lproc_cur->sh->vm_borrowed = 0;
    arena_init(image_top(&exe, &interp));   /* fresh heap above the new image */
    uint64_t rsp = build_stack_vec(na, argc, ne, envc, &exe, &interp);
    kfree(sbuf);

    /* The child now has its own space, so the vfork parent can safely resume. */
    if (lproc_cur->sh->vforking) {
        lproc_cur->sh->vforking = 0;
        task_wake(lproc_cur->sh->vfork_waiter);
    }
    if (!borrowed)
        vmspace_destroy(old);           /* release the replaced image's frames */

    linux_jump(entry, rsp);             /* enter the new image; does not return */
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
    int lr = load_program(&exe, &interp, &entry);
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
