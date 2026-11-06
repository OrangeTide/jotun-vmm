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

- Stage the libraries in the bundle at the paths `ld.so` searches:
  `lib64/ld-linux-x86-64.so.2` and `lib64/libc.so.6`, copied from the host.
  `execve` and `openat` strip the leading `/` through `bundle_path`, so
  `/lib64/...` resolves. `ld.so` opening `/etc/ld.so.cache` and getting
  `-ENOENT` is fine; it falls back to the default directories.
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
2. `elf_load_ex`, ET_DYN loading, and the fuller auxv, proved with a dynamic
   `hello` built against musl's `ld-musl-x86_64.so.1`. One small interpreter
   and one `DT_NEEDED`, simpler than glibc. Stage the musl loader and
   `libc.so`.
3. glibc: stage the host `ld-linux-x86-64.so.2` and `libc.so.6`, run a dynamic
   glibc hello, then the host's real `/usr/bin/sed`. That last step is the
   thesis, an unmodified host binary running under the trap.

## The cut line

Held against scope creep, and all deferred to later hardening milestones:

- no `PROT_*` enforcement (no W^X, RELRO is advisory),
- no copy-on-write and no lazy file paging (mappings are eager copies),
- `munmap` accepts but does not reclaim,
- `posix_spawn` via `clone(CLONE_VM | CLONE_VFORK)` is not handled; see the
  make notes in `doc/m16-linux-spike.md`.

None of these block `ld.so` or a first dynamic program.
