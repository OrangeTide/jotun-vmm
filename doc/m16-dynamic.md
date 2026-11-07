# M16.7 design: dynamic binaries (ld.so, PIE, glibc)

This is a design sketch, not yet implemented. The goal is the one the whole
Linux-compat line was built toward: run an unmodified host binary, which on a
normal distribution is dynamically linked, position independent, and resolved
at startup by `ld-linux-x86-64.so.2` against `libc.so.6`. Static linking (see
`doc/m16-linux-spike.md`) proved the ring-0 SYSCALL trap and the per-process
address-space model. Dynamic linking is the real target, and the host's
`/usr/bin/sed` is a good first subject: PIE, one `DT_NEEDED` on glibc.

## The one design decision everything hangs on

The guest already has the right primitive. `pf_handle` in
`guest/libos/arch/x86_64_vmspace.c` faults any touched user-window page to a
fresh zeroed frame on demand, and `elf_load_fid` fills segments by writing
straight to virtual addresses and letting those writes fault in. So the model
for dynamic support is eager-fill, demand-paged, with protections relaxed:

- A file-backed `mmap` picks or accepts an address, then copies the file bytes
  into those virtual addresses. The writes fault pages in as they go. There is
  no lazy file paging and no copy-on-write. This matches how `elf_load_fid`
  already works and leaves the fault handler untouched.
- `PROT_*` is not enforced. `pf_handle` always maps `VM_W | VM_U`, and
  `mprotect` stays a no-op. RELRO and `PROT_NONE` guard pages become advisory.
  The program still runs; what is lost is W^X enforcement. This is the single
  honest simplification, and it is reversible later. Enforcing it means
  tracking a per-page protection, mapping read-only pages without `VM_W`, and
  teaching `pf_handle` to service a protection fault instead of bailing when
  `err & VM_P` is set.

With that settled, there are two pieces of new machinery.

## Piece 1: file-backed mmap

Today `do_mmap` (`guest/libos/linuxelf.c`) drops `prot` and `offset` and
rejects any `fd >= 0`, and the dispatcher passes only four of the six mmap
arguments. Extend both to carry `prot` (arg 3) and `offset` (arg 6), add
`MAP_FIXED`, and fill mapped pages from the fd.

```c
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10

/* Copy [off, off+len) of the file open on fd into guest VAs [va, va+len).
 * Writing faults each page in via pf_handle (zeroed frame), so the tail of
 * the last page past end-of-file is already zero: that is the .bss fill. */
static long
mmap_fill(long fd, uint64_t va, uint64_t off, uint64_t len)
{
    struct lfd *f = fd_get(fd);
    if (!f)
        return -LERR_BADF;
    uint8_t *dst = (uint8_t *)(uintptr_t)va;
    if (f->isram) {                         /* ramfs file: copy from its buffer */
        struct ramfile *rf = &ramfs[f->ridx];
        for (uint64_t i = 0; i < len && off + i < rf->len; i++)
            dst[i] = rf->data[off + i];
        return 0;
    }
    uint64_t done = 0;                      /* 9P bundle file */
    while (done < len) {
        long n = p9_read(f->p9fid, off + done, dst + done, (uint32_t)(len - done));
        if (n <= 0)
            break;                          /* short read: the rest stays zero */
        done += (uint64_t)n;
    }
    return 0;
}

static long
do_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
        long fd, uint64_t off)
{
    (void)prot;                             /* not enforced (see note above) */
    uint64_t n = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (n == 0)
        return MMAP_FAILED;

    uint64_t va;
    if (flags & MAP_FIXED) {
        va = addr & ~(PAGE_SIZE - 1);       /* honour the exact address */
        if (va < VM_USER_LO || va + n > VM_USER_HI)
            return MMAP_FAILED;
        /* an overlay inside an existing reservation: do not move mmap_cur */
    } else {
        if (mmap_cur - n < brk_cur)
            return MMAP_FAILED;
        mmap_cur -= n;                      /* bump-reserve, descending */
        va = mmap_cur;
    }

    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        long r = mmap_fill(fd, va, off, len);
        if (r < 0)
            return r;
    }
    /* anonymous pages need nothing: pf_handle zeroes them on first touch */
    return (long)va;
}
```

Dispatch becomes:

```c
case SYS_mmap:
    return do_mmap(a1, a2, a3, a4, a5, a6);
```

This is enough for `ld.so`, which maps each library in three steps: a
`mmap(NULL, span, PROT_NONE, MAP_PRIVATE, fd, 0)` to reserve the whole span
(the bump path above), one `MAP_FIXED` `mmap` per `PT_LOAD` at
`base + p_vaddr` (the fixed path, which fills file bytes), and a
`MAP_FIXED | MAP_ANONYMOUS` map for the `.bss` remainder (a no-op here, since
those pages are already zero on fault). `munmap` stays accept-without-reclaim,
so `ld.so` trimming an over-aligned reservation just wastes address space.

Add `pread64` (x86-64 syscall 17, arm64 67) at the same time, since `ld.so`
reads library program headers with it. It reads at an explicit offset without
moving the fd cursor:

```c
case SYS_pread64: {
    struct lfd *f = fd_get(a1);
    if (!f || f->iscon || f->ispipe)
        return -LERR_BADF;
    if (f->isram) { /* copy from ramfs[f->ridx] at offset a4 */ }
    return p9_read(f->p9fid, (uint64_t)a4, (void *)a2, (uint32_t)a3);
}
```

## Piece 2: an ET_DYN-capable loader

`elf_load_fid` loads at a fixed `p_vaddr` and returns only the entry address.
Replace it with a loader that applies a load bias and reports what the auxv and
the interpreter handoff need. Keep `elf_load_fid` as a thin wrapper (bias 0,
return the entry) so the native loader and the static path do not change.

```c
struct elf_info {
    uint64_t entry;        /* bias + e_entry */
    uint64_t bias;         /* load offset: 0 for ET_EXEC, chosen for ET_DYN */
    uint64_t phdr;         /* in-memory address of the program headers */
    uint64_t phentsize, phnum;
    uint64_t load_end;     /* highest mapped vaddr, to place the next object */
    int      has_interp;
    char     interp[128];  /* PT_INTERP path string */
};

/* Load into [lo, hi). For ET_DYN, bias_hint is the base to load at; for
 * ET_EXEC it must be 0, because the segments carry absolute vaddrs. */
int elf_load_ex(uint32_t fid, uint64_t lo, uint64_t hi,
                uint64_t bias_hint, struct elf_info *out)
{
    /* read ehdr; ET_EXEC (2) => bias 0; ET_DYN (3) => bias = bias_hint */
    uint64_t bias = (eh.e_type == 2) ? 0 : bias_hint;
    out->bias = bias;
    out->phentsize = eh.e_phentsize;
    out->phnum = eh.e_phnum;
    out->has_interp = 0;
    out->load_end = 0;

    for (each phdr ph) {
        if (ph.p_type == PT_INTERP) {
            read_full(fid, ph.p_offset, out->interp, min(ph.p_filesz, 127));
            out->has_interp = 1;
            continue;
        }
        if (ph.p_type != PT_LOAD)
            continue;
        uint64_t dst = bias + ph.p_vaddr;
        if (dst < lo || dst + ph.p_memsz > hi)
            return -1;
        read_full(fid, ph.p_offset, (void *)dst, ph.p_filesz);
        /* bss (memsz > filesz) is already zero via pf_handle: nothing to do */
        if (dst + ph.p_memsz > out->load_end)
            out->load_end = dst + ph.p_memsz;
        /* AT_PHDR: the LOAD segment that contains e_phoff */
        if (ph.p_offset <= eh.e_phoff &&
            eh.e_phoff < ph.p_offset + ph.p_filesz)
            out->phdr = bias + ph.p_vaddr + (eh.e_phoff - ph.p_offset);
    }
    out->entry = bias + eh.e_entry;
    return 0;
}
```

## Wiring it together

The load path in `do_execve` and the first-program boot becomes:

```c
struct elf_info exe, interp;
elf_load_ex(FID, VM_USER_LO, VM_USER_HI, LINUX_PROG_BASE, &exe);  /* PIE: bias 0x400000 */

uint64_t entry;
if (exe.has_interp) {
    uint32_t ifid = /* ns_walk + open on bundle_path(exe.interp) */;
    uint64_t ibase = (exe.load_end + 0x1FFFFF) & ~0x1FFFFFULL;    /* 2 MiB-align above exe */
    elf_load_ex(ifid, VM_USER_LO, VM_USER_HI, ibase, &interp);
    entry = interp.entry;     /* enter ld.so; it relocates, then jumps to AT_ENTRY */
} else {
    entry = exe.entry;        /* the existing static non-PIE path */
}
uint64_t rsp = build_stack_dyn(argv, argc, envp, envc, &exe, &interp);
linux_jump(entry, rsp);
```

The auxv in `build_stack` grows from three tags to the set `ld.so` reads:

```c
blk[w++] = AT_PHDR;   blk[w++] = exe->phdr;
blk[w++] = AT_PHENT;  blk[w++] = exe->phentsize;
blk[w++] = AT_PHNUM;  blk[w++] = exe->phnum;
blk[w++] = AT_BASE;   blk[w++] = exe->has_interp ? interp->bias : 0;  /* ld.so base */
blk[w++] = AT_ENTRY;  blk[w++] = exe->entry;    /* where ld.so hands control back */
blk[w++] = AT_EXECFN; blk[w++] = (uintptr_t)argv0_string_on_stack;
blk[w++] = AT_PAGESZ; blk[w++] = 4096;
blk[w++] = AT_RANDOM; blk[w++] = (uintptr_t)rnd;
blk[w++] = AT_SECURE; blk[w++] = 0;
blk[w++] = AT_UID;    blk[w++] = 0;   /* and EUID, GID, EGID = 0 */
blk[w++] = AT_HWCAP;  blk[w++] = 0;   /* glibc tolerates 0 */
blk[w++] = AT_NULL;   blk[w++] = 0;
```

New tag values: `AT_PHDR=3`, `AT_PHENT=4`, `AT_PHNUM=5`, `AT_BASE=7`,
`AT_ENTRY=9`, `AT_UID=11`, `AT_EUID=12`, `AT_GID=13`, `AT_EGID=14`,
`AT_HWCAP=16`, `AT_SECURE=23`, `AT_EXECFN=31`.

## Supporting changes

- Stage the libraries where `ld.so` searches: `lib64/ld-linux-x86-64.so.2` and
  `lib64/libc.so.6`. `execve` and `openat` strip the leading `/` through
  `bundle_path`, so `/lib64/...` resolves. `ld.so` opening `/etc/ld.so.cache`
  and getting `-ENOENT` is fine; it falls back to the default directories.
  Two ways to supply them, from simplest to the intended design:
  - Copy the host files into the bundle staging directory (the quick path, like
    the `EXTRA_BUNDLE_FILES` hook used for the busybox and make probes).
  - Graft a runtime as a read-only mount. The repo already has the mechanism:
    the manifest `mount` entry served by the `p9graft` backend (M13) grafts a
    shared read-only tree at a path. A bundled runtime, or the host's own
    `/lib64` and `/usr/lib`, can be grafted in rather than copied file by file.
    This is the design to aim for, since it lets the guest pick up either an
    installed runtime or the system's native one without baking libraries into
    each image.
- RAM and window sizing. The user window is 24 MiB
  (`[0x400000, 0x1C00000)` in `arch/x86_64_vmspace.h`) and total guest RAM is
  32 MiB. Eager-filling `libc.so.6` into private frames, plus the executable,
  heap, TLS, and stack, is tight but should hold a small program. Expect to
  raise guest RAM, and the physmap span, once real programs map more.
- Syscalls already serviced that `ld.so` and glibc startup use: `openat`,
  `close`, `read`, `fstat`, `newfstatat`, `statx`, `brk`,
  `arch_prctl(ARCH_SET_FS)`, `set_tid_address`, `set_robust_list`,
  `getrandom`, `prlimit64`, `faccessat`. `rseq` returning `-ENOSYS` is
  tolerated, and the `mprotect` no-op is fine. The genuinely new syscalls are
  file-backed `mmap` and `pread64`.

## Order of work, and how to prove each step

1. File-backed `mmap` and `pread64` first, proved in isolation with a static
   program that opens a bundle file, maps it `MAP_PRIVATE`, and reads the bytes
   back. No loader changes yet. This de-risks the core piece on its own.
   **Done.** `example/linuxmmap.c` (run with `make linux-spike SPIKE_PROG=linuxmmap`)
   checks a `MAP_PRIVATE` file mapping against a plain `read`, a `pread` at a
   nonzero offset, and a `MAP_FIXED` overlay onto a prior anonymous reservation,
   which is the exact move the dynamic linker makes. The committed musl tests
   (including the `linuxmem` heap test that leans on anonymous `mmap`) still
   pass. See the mmap and pread64 rows in `doc/linux-syscalls.md`.
2. `elf_load_ex`, ET_DYN loading, and the fuller auxv, proved with a dynamic
   `hello` built against musl's `ld-musl-x86_64.so.1`. One small interpreter
   and one `DT_NEEDED`, simpler than glibc. Stage the musl loader and
   `libc.so`. **Done.** See "Step 2 result" below.
3. glibc: stage the host `ld-linux-x86-64.so.2` and `libc.so.6`, run a dynamic
   glibc hello, then the host's real `/usr/bin/sed`. That last step is the
   thesis, an unmodified host binary running under the trap. **Done.** See
   "Step 3 result" below.

## Step 2 result

The loader gained `elf_load_ex` (in `elfload.c`), which loads an ET_EXEC at its
fixed vaddr or an ET_DYN at a caller-chosen bias, and reports the entry, bias,
in-memory program-header address, and any `PT_INTERP` path. `elf_load_fid` is
now a thin wrapper over it, so the native loader and the static non-PIE path are
unchanged. In `linuxelf.c`, `load_program` loads the executable at
`LINUX_PROG_BASE`, and when it has an interpreter, loads that just above the
executable (2 MiB aligned) and returns the interpreter's entry. `build_stack_vec`
now writes the full auxv the dynamic linker reads: `AT_PHDR`, `AT_PHENT`,
`AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN`, `AT_PAGESZ`, `AT_RANDOM`,
`AT_SECURE`, the uid/gid tags, and `AT_HWCAP`.

Proved with a dynamic musl `hello` (`musl-gcc -O2`, an ET_DYN PIE whose
interpreter is `/lib/ld-musl-x86_64.so.1`). The musl loader is itself `libc.so`,
so the one `DT_NEEDED` on `libc.so` is self-satisfied and no library open is
needed. Staged by hand, since it needs the host musl loader:

```
d=bundle-staging/x86_64
mkdir -p $d/boot $d/lib
cp example/bundle/* $d/; cp libos.elf $d/boot/kernel; cp hello.elf $d/hello
musl-gcc -O2 -o $d/hellodyn example/linuxhello.c
cp /usr/lib/x86_64-linux-musl/libc.so $d/lib/ld-musl-x86_64.so.1
./jbundle -d bundle-depot --ref root --name jotun-demo --arch x86_64=$d
./castool -d bundle-depot pack
JOTUN_BUNDLE=bundle-depot JOTUN_REF=root ./jotun libos.elf bin hellodyn
```

```
[bin] loaded, entry=0x000000000067bf0e interp=/lib/ld-musl-x86_64.so.1 base=0x0000000000600000
...
hello from musl
[bin] pid 1 exited, status=0
```

The interpreter is staged at the nested bundle path `lib/ld-musl-x86_64.so.1`,
which the loader resolves through `bundle_path` and `ns_walk`. This is the same
path handling a read-only runtime mount will use, so real host paths like
`/lib64/ld-linux-x86-64.so.2` need no loader change, only the file served there.
The exe loads at `0x400000`, the interpreter at `0x600000`, both below the heap
base at `0x800000`; glibc's larger `libc.so.6` may push the interpreter past
that, so step 3 likely raises the heap base or places the interpreter higher.
Zero unhandled syscalls, and all ten committed static tests still pass. The dynamic
musl hello stays a documented manual probe, like the busybox and make runs,
because it needs the host musl loader rather than a committed bundle file.

## Step 3 result

The thesis holds: the host's unmodified `/usr/bin/sed` runs under the trap.

```
JOTUN_BUNDLE=bundle-depot JOTUN_REF=root ./jotun libos.elf bin sed 's/red/GREEN/' in.txt
one GREEN fish
two blue fish
three GREEN fish
[bin] pid 1 exited, status=0
```

sed is a glibc PIE with four shared objects (`libc.so.6`, `libacl.so.1`,
`libselinux.so.1`, and `libpcre2-8.so.0` pulled in by libselinux). A glibc
`hello` was the first milestone, then sed. Getting there turned up two bugs that
musl never exercised, because the musl loader is itself libc and opens no
library.

- **A MAP_FIXED overlay must zero its range.** The dynamic linker maps a
  library's whole span file-backed, then overlays the `.bss` with an anonymous
  `MAP_FIXED` mapping and expects zeros. Our anonymous path assumed pages fault
  in zeroed, but these frames were already present (the file-backed reservation
  had filled them), so `.bss` held stale file bytes. A glibc startup lock sat in
  that `.bss`, read non-zero, and glibc blocked on a `futex` that never woke.
  `do_mmap` now zeroes any `MAP_FIXED` range before copying file bytes over it.
- **fstat must report a stable per-file inode.** The linker dedups libraries by
  `(st_dev, st_ino)`. `do_fstat` reported `st_ino = p9fid`, the transient fid,
  which is reused across open and close. So the second library opened reused the
  first library's fid value, the linker concluded they were the same file, and
  reused the wrong mapping, which made every symbol version vanish. The fd now
  carries the 9P qid path captured at open, and `fstat` reports that.

Two syscalls the glibc loader needs were added: `access` (21), which it uses to
probe `/etc/ld.so.cache` and candidate library directories, and quiet
`statfs`/`fstatfs` stubs returning `-ENOSYS` (libselinux probes the filesystem
type; a logged "unhandled" line would otherwise pollute a tool's output).

The loader found `libc.so.6` at `/lib/x86_64-linux-gnu/libc.so.6` with no
`ld.so.cache`, no `RPATH`, and no `LD_LIBRARY_PATH`, because this host's glibc
ld.so has the multiarch directory compiled into its default search path. The
vDSO is absent, so glibc falls back to real syscalls for `clock_gettime` and
the like.

Staged by hand for the probe (the loader plus five libraries at their multiarch
paths):

```
d=bundle-staging/x86_64
mkdir -p $d/boot $d/lib64 $d/lib/x86_64-linux-gnu
cp example/bundle/* $d/; cp libos.elf $d/boot/kernel; cp hello.elf $d/hello
cp -L /usr/bin/sed $d/sed
cp -L /lib64/ld-linux-x86-64.so.2 $d/lib64/ld-linux-x86-64.so.2
for l in libc.so.6 libacl.so.1 libselinux.so.1 libpcre2-8.so.0; do \
    cp -L /lib/x86_64-linux-gnu/$l $d/lib/x86_64-linux-gnu/$l; done
printf 'one red fish\ntwo blue fish\nthree red fish\n' > $d/in.txt
./jbundle -d bundle-depot --ref root --name jotun-demo --arch x86_64=$d
./castool -d bundle-depot pack
JOTUN_BUNDLE=bundle-depot JOTUN_REF=root ./jotun libos.elf bin sed 's/red/GREEN/' in.txt
```

Hand-copying five libraries is the bring-up shortcut. The production path is the
`p9graft` read-only mount: graft a runtime read-only so the libraries stay out
of the app bundle, and sed resolves them at their real paths with no copying. A
dynamic glibc `hello` and all ten committed static tests still pass, each with
zero unhandled syscalls.

## Runtime mount wired

The read-only runtime mount now works, with no loader change. The libraries live
in a separate runtime bundle, grafted into the app guest through the manifest.

The app bundle holds only `sed` and `in.txt`, no libraries. A runtime bundle
holds `lib64/ld-linux-x86-64.so.2` and `lib/x86_64-linux-gnu/*.so*`. The
manifest grafts the runtime's two subtrees at the paths the dynamic linker
expects:

```
bundle app-depot
ref root
init bin sed s/red/GREEN/ in.txt
mount lib64 runtime-depot root lib64
mount lib   runtime-depot root lib
```

```
./jotun libos.elf -m sed.manifest
[9p] guest 0: grafted runtime-depot (ref root) at /lib64
[9p] guest 0: grafted runtime-depot (ref root) at /lib
...
one GREEN fish
two blue fish
three GREEN fish
```

Wiring this up needed two host changes. The manifest took a single `mount`; it
now holds an array and the parser accepts several `mount` lines
(`manifest.{c,h}`, `main.c`). And `p9graft` grafted one tree with a single
reserved qid bit to route nodes, which does not compose: chaining two graft
wrappers made the outer one clear the inner one's bit, so the inner graft's
nodes misrouted. `p9graft` now holds several grafts in one wrapper and routes by
a small tree id in the high bits of each qid (0 for the base, 1..N for the
grafts), so any number of mounts coexist. `main.c` builds one wrapper and adds
each mount to it.

This is the design the project was aiming for: a guest picks up an installed or
bundled runtime, or the host's own `/lib64` and `/lib/x86_64-linux-gnu`, through
a read-only mount, with nothing baked into the app image.

### Grafting a live host directory

A mount source can now be a live host directory, not only a bundle. Written
`host:<path>`, it grafts that directory through `p9hostdir`, a read-only 9P
backend over the real filesystem (`host/p9hostdir.{c,h}`). So the host's own
runtime grafts in with no packing step at all:

```
bundle app-depot
ref root
init bin sed s/red/GREEN/ in.txt
mount lib64 host:/usr/lib/x86_64-linux-gnu
mount lib   host:/usr/lib
```

```
[9p] guest 0: grafted host:/usr/lib/x86_64-linux-gnu at /lib64
[9p] guest 0: grafted host:/usr/lib at /lib
...
one GREEN fish
```

`p9hostdir` serves each node by the path relative to its root (kept in the node
handle) and uses the host inode as the qid, so the dynamic linker's library
dedup works. A one-entry fd cache turns a library's run of sequential reads into
a single `open`. It is confined to the directory it is opened on: a walk accepts
only a single path component, never `.` or `..`, and a path that resolves (via a
symlink) outside the root is refused. A confinement test confirms an in-mount
library opens while `/lib/../../../../etc/hostname` and a symlink-style escape
are denied. The manifest, written by the operator, chooses which host directory
is exposed; the guest only reads within it.

This is the "system's native runtime" half of the goal: the two mount kinds,
`host:<path>` and a bundle, let a guest run against the host's live libraries or
against a packed, content-addressed runtime, whichever the operator mounts.

The host's `gawk` runs the same way, from an app bundle holding only the binary,
a program file, and its input, with its seven libraries
(`libsigsegv`, `libreadline`, `libmpfr`, `libgmp`, `libm`, `libc`, `libtinfo`)
served from the live `/usr/lib`:

```
init bin gawk -f prog.awk data.txt
mount lib64 host:/usr/lib/x86_64-linux-gnu
mount lib   host:/usr/lib
```

`gawk '{ s[$1] += $2 } END { for (k in s) print k, s[k] }'` sums its input
correctly. Bringing it up added quiet stubs for the calls its startup makes that
were not yet serviced: `mincore` (ENOSYS), `getpgrp`, `getgroups`, and
`sigaltstack` (libsigsegv installs a SIGSEGV handler on an alternate stack;
there is no signal delivery, so the call is accepted and ignored). With those,
gawk runs with zero unhandled syscalls. The manifest tokenizes `init` on spaces,
so a multi-word program goes in a file read with `gawk -f`, not inline.

## make, and posix_spawn

The host's `make` running a real Makefile was the first tool that forks a child
per recipe, and it exposed the one process primitive the spike had skipped:
`posix_spawn`. glibc implements it with `clone3` (435) and the flags
`CLONE_VM | CLONE_VFORK`, a sequence the earlier static musl make was rebuilt to
avoid. The host binary cannot be rebuilt, so the primitive is now implemented.

- `clone3` parses the `clone_args` struct and routes like `clone`:
  `CLONE_VM | CLONE_VFORK` is a spawn, `CLONE_VM` alone a thread, otherwise fork.
- A spawn (`do_spawn_vfork`) makes a new child process that borrows the parent's
  address space, runs the spawn trampoline on the given stack (its fd setup,
  then `execve`), while the parent is suspended. Scheduling is cooperative, so
  the parent simply `task_block`s and the child wakes it. `execve` gives the
  child its own address space and wakes the parent then; a child that exits
  before `execve` (a failed exec) wakes it too. `execve` and `wait4` never
  destroy a borrowed space, and the parent reclaims its own.
- The child's startup also calls the credential and session syscalls
  (`setresuid`, `setresgid`, `setsid`, `setpgid`, and the rest). The guest is a
  single root identity, so a set to it returns success and there is nothing else
  to become.

One ABI bug surfaced here: `wait4`'s `pid` is a 32-bit `pid_t`, and glibc passed
`-1` in the low 32 bits only. Treating it as a 64-bit value made `pid > 0` true,
so the match skipped the real child and `wait4` returned `ECHILD`. `do_wait4`
now sign-extends from 32 bits, so `-1` means any child again.

```
init bin make -f Makefile all
mount lib64 host:/usr/lib/x86_64-linux-gnu
mount lib   host:/usr/lib
mount bin   host:/bin
```

```
[cc] a
[cc] b
[all] done
```

The recipe shell is the host's `/bin/sh` (dash), grafted from the live `/bin`
(a symlink to `/usr/bin`), running its own libc from the same mount. make, its
shell, and the shells' three child processes all run with zero unhandled
syscalls.

## bash: an interactive-class shell

bash is the heaviest tool run so far. It is a PIE, so it loads at a bias the
same way `ld.so` does, and it needs `libtinfo` alongside `libc`, both pulled
from the host's live `/usr/lib`. A builtins-only script runs first (variables,
`for`, arithmetic, a function), then a script that forks external commands
through a pipe:

```
init bin bash bashtest.sh
mount lib64   host:/usr/lib/x86_64-linux-gnu
mount lib     host:/usr/lib
mount hostbin host:/usr/bin
```

```
export PATH=/hostbin
echo one two three four | wc -w      ->  4
echo hello | tr a-z A-Z              ->  HELLO
```

bash finds `wc` and `tr` on `PATH`, forks, wires the pipe with `pipe2` and
`dup2`, and `execve`s each from the grafted `/usr/bin`. Those are unmodified
host coreutils binaries, dynamic glibc PIEs, so each one is itself loaded
through the ld.so path and runs its own libc from the same mount. The whole
pipeline (bash plus four child processes) runs with zero unhandled syscalls.

Three small calls surfaced and were added, all additive:

- `gettimeofday` (96): bash reads the wall clock at startup. Split from the
  cycle counter the same way as `clock_gettime`.
- `socket` (41): bash probes for a socket during init. There are no sockets in
  this guest, so it returns a quiet ENOSYS and bash falls back cleanly.
- `fadvise64` (221): coreutils calls `posix_fadvise` on its input. It is
  advisory, so the stub returns 0.

## The C compiler and linker

The next target was an autotools "hello world" (`./configure && make`), which
needs a working C compiler. The host's gcc is a driver that execs the compiler
proper `cc1`, the assembler `as`, and the linker `ld` (via `collect2`), each an
unmodified dynamic binary from the live `/usr` mount.

### What runs

The driver and `cc1` are large: `cc1` alone is a 30 MiB image spanning about
34 MiB of virtual address. The per-process user window was 24 MiB inside 32 MiB
of RAM (both sized for the small demo guests), so `cc1` did not fit and the
frame pool ran dry mid-load. Growing guest RAM to 128 MiB, with the window and
the physmap derived from it (108 MiB window, RAM top less 16 MiB of vcpu
stacks), gave it room. The device MMIO band at 0x0a000000 (160 MiB) is the
ceiling: RAM must stay below it so a doorbell store still faults out to the
host. See `host/main.c` and `arch/x86_64_vmspace.h`.

With the window enlarged, an unmodified host gcc compiles a C file to an object
file end to end:

```
init bin bash ccbuild.sh         # ccbuild.sh: PATH=/usr/bin; cc -c /hello.c -o /h.o
mount lib64 host:/usr/lib/x86_64-linux-gnu
mount lib   host:/usr/lib
mount usr   host:/usr
```

The driver execs `cc1` (which preprocesses and compiles) piped to `as` (which
assembles), producing a valid object in the ramfs overlay. `nm` on it reads
`main`; `readelf` confirms a relocatable x86-64 object. The whole run takes
about 1.4 seconds, pulling `cc1`, `as`, and their libraries on demand from the
host mounts. So the compiler proper runs under the hypervisor.

This needed three fixes besides the window:

- `brk` started at a fixed 8 MiB, inside a large image. `arena_init` now places
  the heap on the first page past the loaded program (interpreter end when
  present), tracked per process so `brk` cannot retreat below it.
- `do_mmap`'s bounds checks were not overflow-safe; a large mapping wrapped the
  arena and returned a wild address the kernel faulted on. Rewritten as
  `n > HI - va` and `n > mmap_cur - brk_cur`.
- `split_path` now normalizes `.` and `..`. gcc's `is_directory()` stats a
  candidate with `/.` appended; failing that, gcc judged every library
  directory absent and passed `ld` no `-L` flags.

Quiet stubs were added for the calls the toolchain makes: `sysinfo` (cc1 sizes
its allocator), `getrusage`, `unlink`/`unlinkat` (remove overlay temporaries),
`chmod`/`fchmod`/`fchmodat`, `umask`, and `readlink` (gcc probes
`/proc/self/exe` and falls back to its default prefix).

### Linking

Linking completes: `cc /hello.c -o /hello` produces a correct dynamic PIE
executable (the right interpreter, LOAD segments, and entry point), default
flags and LTO plugin included.

Getting there meant isolating a failure that first looked like a linker symbol
bug. `ld` reported every cross-object symbol undefined:

```
ld: Scrt1.o: in function `_start': undefined reference to `main'
ld: undefined reference to `__libc_start_main'
ld: crtbeginS.o: undefined reference to `__TMC_END__'
ld: final link failed: bad value
```

The object defining `main` was valid in isolation (`nm` and `readelf` both read
it), so the reads were not the problem. Tracing `ld`'s file opens showed it
never opened the input object at all, nor the end crt objects. The cause was
argument-list truncation: `LINUX_MAX_ARGS` was 32, and `copy_uvec` silently
drops arguments past the cap. gcc's `collect2` builds an `ld` command line with
the crt objects, every `-L` path, the input objects, the `-l` libraries, and
the end crt objects, well over thirty arguments, so the trailing ones fell off.
Which symbol went missing depended only on command-line length: the input
object when the LTO plugin lengthened the line (`main`), the end crt objects
otherwise (`__TMC_END__` from crtend.o). Raising `LINUX_MAX_ARGS` to 256 and
`EXEC_STR_MAX` to 32 KiB fixed it.

The lesson for later tools: a dropped argument reads back as a semantic error
three layers up (an "undefined symbol" from the linker), far from its cause.

### The gap to autotools

With compile and link working, `./configure` still needs more:

- A writable filesystem with directories. **Done.** The overlay is now
  directory-aware (`mkdir`/`rmdir`/`rename`, nested files, and a `getdents` that
  merges the 9P and overlay sides); it holds 256 entries of up to 4 MiB. See
  "The writable overlay" below.
- A real working directory. `chdir` is still a no-op (cwd stays `/`), so
  configure's relative-path and `cd` steps resolve against the root, not a
  build subdirectory.
- Exec of freshly built binaries from the overlay. `do_execve` runs only bundle
  and host-mount files, but configure compiles `conftest.c` and then runs
  `./conftest` to check the compiler works.
- A few more syscalls as they surface (`symlink`, `link`, `select`/`poll`), and
  the residual pipe bug where a reader draining a large multi-process writer
  sees EBADF.

The thesis target, running the unmodified toolchain to compile and link, is
met. The remaining autotools gaps are the working directory and exec from the
overlay.

## The writable overlay

Anything the guest creates or writes lives in an in-memory overlay that shadows
the read-only 9P bundle and host mounts. It began as a flat set of 16
path-keyed files with no directory concept, which a compiler's temporaries
outgrew and a configure run could not use at all. It is now directory-aware:

- Each entry carries an `is_dir` flag and a normalized bundle-relative key
  (`norm_key` collapses `.` and `..`, so `/tmp/./x` and `tmp/x` are one entry).
  The table holds 256 entries, names up to 192 bytes, files up to 4 MiB.
- `do_open` resolves a directory three ways: an explicit overlay directory
  (`mkdir`), one implied by files created below it, or a 9P directory, and when
  a path is both it opens a merged directory. A directory fd records its key.
- `getdents` lists a directory in two phases, the 9P stream then the overlay's
  direct children, so a directory holding both bundle and created files shows
  all of them. There is no dedup: a created file that shadows a bundle name of
  the same path lists twice.
- `mkdir`, `rmdir` (and `unlinkat` with `AT_REMOVEDIR`), and `rename` (a
  directory carries its subtree; a read-only source is `EXDEV`) operate on the
  overlay; `stat` and `test -d` see overlay directories through `meta_path`.

The overlay is shared across processes (it lives in kernel memory, not a
per-process space), so a directory one process creates is visible to the
others, which a shell pipeline and a build both rely on.

## The cut line

Held against scope creep, and all deferred to later hardening milestones:

- no `PROT_*` enforcement (no W^X, RELRO is advisory),
- no copy-on-write and no lazy file paging (mappings are eager copies; a 30 MiB
  `cc1` is copied in full on each exec, about 1.4 s per compile over 9P, which
  a repeated-compile workload such as configure would feel),
- `munmap` accepts but does not reclaim,
- the autotools gaps above (a writable directory tree, a real working
  directory, and exec of freshly built binaries from the overlay).

None of these block compiling and linking a C program.
