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
- A real working directory. **Done.** Each process has a cwd (`chdir`/`getcwd`);
  a relative path resolves against it before the overlay lookup or 9P walk, and
  it is inherited on fork and the posix_spawn child and kept across `execve`.
  See "The working directory" below.
- Exec of freshly built binaries from the overlay. **Done.** `do_execve` runs a
  program from the writable overlay (an in-memory image) as well as a bundle or
  host-mount file, so configure's `./conftest` and a linked `a.out` run. See
  "Exec from the overlay" below.
- A few more syscalls as they surface (`symlink`, `link`, `select`/`poll`), and
  the residual pipe bug where a reader draining a large multi-process writer
  sees EBADF.

The thesis target, running the unmodified toolchain to compile and link, is
met, and a program built in the overlay now runs. The open items are the
remaining syscalls and the pipe bug.

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

## The working directory

Each process now has a current working directory. It is stored as a normalized
overlay key on the shared process state (the empty string is the root), so it
costs one small field and no path parsing until a relative path is used.

`resolve_path` is the path boundary for the syscall layer. An absolute path, or
any path while the cwd is the root, normalizes directly. A relative path with a
non-root cwd is joined to the cwd first, then normalized, so `.` and `..`
resolve against the current directory. Every path syscall routes through it:
open, stat, access, exec, mkdir, rmdir, rename, and unlink. The internal
helpers that already hold a normalized key (for example `meta_key`) keep using
`norm_key`, which a key passes through unchanged, so a path is never resolved
against the cwd twice.

`chdir` resolves its target, requires that it is a directory, and stores the
key; `getcwd` returns a leading slash followed by the key. The cwd is
per-process: a process's threads share it, `fork` and the posix_spawn child
copy it, and `execve` preserves it, matching Linux. This is what lets a build
step `cd` into a subdirectory and then open, compile, and exec by relative
paths.

## Exec from the overlay

A build produces its output in the writable overlay: configure writes
`conftest.c`, compiles it, and runs `./conftest`; a link writes `a.out`. These
have no backing 9P file, so `execve` had to learn to load from an in-memory
image.

The ELF loader read only over 9P. Its byte source is now a small `elf_src`
(either a 9P fid or an `(image, length)` pair), and the one `read_full` either
loops over 9P reads or copies from the image. `elf_load_ex` keeps the 9P
signature as a thin wrapper, and a new `elf_load_mem` takes the in-memory form;
the segment placement, bss zeroing, and bounds checks are shared.

`do_execve` resolves the path (against the cwd), and if it names an overlay
file it hands that buffer to the loader instead of walking and opening a 9P
fid. The buffer is captured before the address-space switch; it lives in kernel
memory, not the user window, so loading the new image and freeing the old space
leave it intact. An interpreter, when the executable has one, is still a 9P
path: a program built here links against the host's `ld-linux`, which the
loader pulls from the mount as before. So a dynamic PIE copied into the overlay
runs with its interpreter loaded from 9P, which is exactly what a compiled
`conftest` is.

## The autotools attempt

With compile, link, writable directories, a working directory, and exec from
the overlay all in place, the next target was a full `./configure && make` of a
minimal automake "hello world": a project whose `configure` script, `Makefile.in`,
and `hello.c` sit in the read-only bundle, with the host toolchain and coreutils
grafted from live `/usr` mounts, run under `bin bash`. The build directory is
the overlay (configure writes `config.log`, `config.status`, `Makefile`,
`conftest.*` there, shadowing the read-only sources).

configure drove a long way and surfaced several real bugs, each fixed:

- **Shell fd redirection.** configure opens its log on a high fd and juggles
  descriptors (`exec 6>&1`). `fcntl` validated no descriptor, so a closed fd
  reported success; a shell that probes a target with `F_GETFD` before
  redirecting believed it open and then failed to save it. `fcntl` now returns
  EBADF for a closed fd.
- **Command-substitution pipe deadlock.** `` `cmd | sed 1q` `` hung forever.
  `dup2` onto a std fd did not close the previous occupant, so when a pipeline
  stage's stdout (a pipe) was replaced on fd 1 the pipe's writer reference
  leaked and the reader never saw EOF. `dup2`/`dup3` now close the old target
  whatever its number. This also closed the earlier "heavy pipe" bug.
- **Open-unlink aliasing.** `cat confdefs.h - > conftest.c` reported "input file
  is output file". bash writes the heredoc to an overlay temp, unlinks it while
  the fd is open, then creates `conftest.c`, which reused the just-freed slot
  and aliased the two inodes. The overlay now holds an unlinked entry until its
  last fd closes.
- **Timestamps.** Every file reported mtime 0, so automake's sanity check ("a
  freshly created file must not be older than the sources") failed on the
  tie-break. Overlay files now carry a monotonic stamp above a fixed epoch, and
  the wall clock shares that epoch so the new file is newer than the bundle yet
  not in the future.
- **A missing `/tmp`, process headroom, and small stubs.** `sort` needs a
  writable temp dir; configure nests subshells and pipelines and gcc fans out
  to cc1/as/ld, which overran the 8-process table (raised to 48);
  `sched_getaffinity`, `clock_nanosleep`, and `link`/`symlink` (EPERM, so the
  `ln -s` probe falls back to copying) round it out.
- **gcc's tool prefix.** gcc finds `cc1` relative to its own path, so it must be
  invoked as `/usr/bin/gcc` (prefix `/usr/libexec/gcc/...`), not through a
  differently named mount.

With these, configure gets through install detection, the clock sanity check,
`mkdir -p`, `gawk`, the `make` probes, finds gcc, and **passes the "C compiler
works" check**, compiling and linking an overlay `conftest.c` into a running
executable. It then stopped at the next compile with a crash in `cc1`, which
turned out to be a real heap-placement bug, now fixed.

### The cc1 crash: the heap grew through ld.so

`cc1` took a #UD partway into a compile, in `ld.so` code that had run fine
moments before. The cause was the program break being placed in the wrong spot.

When a dynamic program loads, the executable goes at its own address and the
interpreter (`ld.so`) loads 2 MiB-aligned just above it. The heap must start
above both. `image_top()` chose where, and it tested the wrong flag: the
interpreter's `has_interp` instead of the executable's. The interpreter has no
`PT_INTERP` of its own, so that flag is always zero, and `image_top()` always
returned the executable's end, ignoring the interpreter sitting above it.

For the small PIE tools this never bit, because the executable's end falls below
the heap floor (`LINUX_HEAP_BASE`), which lands the heap above their `ld.so`
anyway. `cc1` is different: a 30 MiB non-PIE `ET_EXEC` whose `_end` is at
`0x22c7228`, with `ld.so` placed at `0x2400000` and ending near `0x243a000`. The
heap was set to `0x22c8000`, **below** `ld.so`. glibc's `brk` heap then grew
upward, crossed `0x2400000`, and `malloc` began writing chunk-size headers over
`ld.so`'s `.text`. `ld.so` kept running until it reached one of the overwritten
instructions and took the #UD. `cc1 -E` and `-S` escaped it only because they
allocate little and finish before the heap climbs that far.

The fix is one line of intent in `image_top()`: take the executable's end, and
when the executable has an interpreter, the interpreter's end if it is higher.

How it was found, after several wrong guesses (it is not `AT_RANDOM`, AVX, a
frame leak, or argv truncation): a byte dump at the faulting rip showed it was
`ld.so` `.text` holding garbage; a re-read of `ld.so` straight after loading
proved it had loaded clean, so something corrupted it at run time. Watches in
`phys_alloc` and `vmspace_map` stayed silent, ruling out a re-handed frame or a
remap. Since the guest runs in ring 0, `CR0.WP` was clear and ring-0 stores
ignored a read-only PTE, so marking `ld.so` `.text` read-only caught nothing
until `WP` was set too. With both in place the write faulted, and the culprit
was glibc `malloc`. A `brk` trace then showed the heap starting at `0x22c8000`
and marching straight through `0x2400000`.

One unrelated limit noted along the way: `copy_uvec` caps argv and envp at
`EXEC_STR_MAX`/2 (16 KiB each) and silently drops the overflow, so a very large
environment would reach the child truncated. It did not cause this crash, but it
is worth raising when a workload needs it.

### Two more fixes, then configure runs to the end

With the heap fixed, `cc1` ran and `./configure` cleared every compiler check.
Two further bugs stood between there and a finished build.

The first was `lseek`. `do_lseek` rejected every descriptor below 3 as a bad
file descriptor, on the assumption that 0, 1 and 2 are always the console. But a
redirect moves a real file onto a standard descriptor, and `grep` does
`lseek(0, 0, SEEK_CUR)` at startup, so it failed with "Bad file descriptor" on
every input, pipe or file. The fix seeks a file-backed descriptor whatever its
number and returns `ESPIPE` for a pipe or the console, which is what a caller
expects of a non-seekable descriptor.

The second was the file offset. Each descriptor carried its own read/write
cursor inlined in its table entry, copied on dup and fork. On Unix those share
one open-file description, so a write through any of them advances the single
offset. Without that, a shell pointing several commands at one redirected file
had them write at independent offsets and overwrite each other. config.status
builds its substitution script exactly that way:

```sh
{ echo 'cat >subs.awk <<_ACEOF' && echo "$vars" | sed ... && echo _ACEOF; } >subs.sh
```

The three writers landed out of order and clobbered each other, so the here-doc
header was lost and the shell ran the data lines as commands. The fix moves the
cursor (and the getdents directory-enumeration cursor) into a shared, refcounted
open-file description that descriptors point at; dup, fork and the vfork inherit
path copy the pointer and take a reference, and close drops one. The backing
objects (pipe, ramfile, 9P fid) were already refcounted this way.

With those three fixes the automake "hello world" goes the whole way:
`./configure` exits 0, `make` compiles `hello.c` to `hello.o` and links `hello`,
and `./hello` prints its line.

A handful of non-fatal messages were left over from that run and each turned out
to be a small real gap. `make` warned of clock skew because overlay timestamps
came from a monotonic counter running ahead of the cycle-based wall clock; the
stamp now tracks the same clock the time syscalls report. `gcc` made a
`getsockname`/`getpeername` call that fell through to "unhandled syscall"; both
now return a quiet `ENOSYS`. A producer feeding a closed pipe printed an error
per line because no `SIGPIPE` was ever delivered; a write to a pipe with no
readers now raises the signal's default action. `sort` could not create a temp
file because `fcntl(F_GETFL)` returned `O_RDONLY`, so glibc `fdopen(fd, "w")`
refused it; descriptors now record their access mode. And `rm -r` left
"Directory not empty" because coreutils removes a tree with
`unlinkat(dirfd, "name")` against an open directory fd, which the overlay
resolved against the wrong directory; an `at_path` helper now honors `dirfd`
(sign-extending `AT_FDCWD`, the same 32-bit trap the `wait4` pid hit).

### Sorting with a tiny buffer

One deferred case remained: a `sort` forced to spill, `sort -S 1k` over several
thousand lines, which builds hundreds of small sorted runs in `/tmp` and merges
them. It failed three ways in succession, each a real bug the small synthetic
case made visible where the real configure (which never spills) did not.

First, `open` ignored `O_EXCL`. glibc `mkstemp` creates each temp with
`O_CREAT | O_EXCL` and a random name; the guest `getrandom` is a weak seed, so
two names eventually collided. A real kernel returns `EEXIST` and `mkstemp`
retries, but the overlay silently reopened and truncated the existing entry.
Two of `sort`'s temp nodes then aliased one overlay slot, and unlinking one
deleted the file the other still needed. The reopen hit `ENOENT`, the temp file
had vanished. Honoring `O_EXCL` (a create over an existing overlay name returns
`EEXIST`) makes `mkstemp` draw a fresh name, whatever the quality of the seed.

Second, the overlay leaked memory. `ramfs_remove` freed a slot but left its
`kmalloc`'d data buffer attached, and `ramfs_create` cleared the pointer without
freeing, so every recycled slot leaked its buffer until the kernel heap was
exhausted and the next write or `fork` failed with `ENOMEM`. A single
`ramfs_free_slot` now returns the buffer at every point a slot is destroyed:
unlink, the deferred open-unlink close, `rmdir`, and a `rename` over an existing
destination.

Third, the overlay held only 256 entries, fewer than the runs `sort` keeps alive
at once with a 1 KiB buffer. It now holds 1024.

With those, `sort -S 1k` over 4000 lines sorts correctly, its output matches a
reference sort, and `/tmp` drains back to empty.

### The descriptor cap

The per-process descriptor table held 32 entries, so a process had 29 usable
descriptors (0, 1 and 2 are the console). That is few: a program that keeps many
files open at once runs out, and `bash`, which relocates its script descriptor
to a high number, had to scan downward from a guessed high value because the
guest reported `RLIMIT_NOFILE` as `RLIM_INFINITY` rather than the real limit.

The table could not simply grow, because it is embedded in each process and each
entry carried a 192-byte directory key, so a larger table would push the kernel
image into the program's address space at 4 MiB. That key is only used by
directory descriptors, and every directory descriptor already owns a shared
open-file description, so the key moved there. Each table entry then dropped to
about a quarter of its size, and the table grew to 128 entries at no extra cost
to the kernel image. The 9P descriptor pool on both sides grew to match, so a
process can hold that many host files open at once, not just that many numbers.
And `prlimit64` now reports `RLIMIT_NOFILE` as the real cap, so a program sizes
its descriptor arrays correctly and bash lands its script descriptor directly.
A test held 87 host files open at once, where the old pool capped near 63.

## The cut line

Held against scope creep, and all deferred to later hardening milestones:

- no `PROT_*` enforcement (no W^X, RELRO is advisory),
- no copy-on-write and no lazy file paging (mappings are eager copies; a 30 MiB
  `cc1` is copied in full on each exec, about 1.4 s per compile over 9P, which
  a repeated-compile workload such as configure would feel),
- `munmap` accepts but does not reclaim,
- a directory listing is capped: the 9P server serializes a directory into one
  fixed buffer and returns at most 64 entries, so `ls` of a large host directory
  (`/usr/bin` has thousands) is silently truncated. The autotools build never
  reads such a directory, but a tool that scans one would see a short list,
- the autotools gaps above (a writable directory tree, a real working
  directory, and exec of freshly built binaries from the overlay).

None of these block compiling and linking a C program.
