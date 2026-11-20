# Changelog

Notable changes to Jotun. The version is the single line in `version.mk`; each
release is tagged `v<version>`.

## 0.34.1

### Changed

- Docs only. Pruned the removed display subsystem from the roadmap and the
  design doc: roadmap M6/M7 and M15 are reduced to removal stubs, M10 is
  refocused on multi-guest multiplexing, and `doc/unikernel-design.md` Section 9
  is removed with its sections renumbered. Release-version prose references were
  made uniform (`v0.34.0`). No code change.

## 0.34.0

### Removed

- The native (non-Linux) guest program environment. Jotun now hosts only
  Linux-like guests. The original QNX-like native OS had been replaced piecemeal
  while building Linux support, and its remaining vestiges are gone:
  `guest/prog/` (the native `hello`/`malloctest` programs and the `jlibc` under
  `libc/`), `guest/jotun_ring.h` (the native program ABI), `host/drawws.h` (an
  orphaned draw front-end header), `doc/draw-vocabulary.md` (the `/dev/draw`
  display vocabulary), and `example/bundle/rc` (which ran the long-gone
  `jsh`/`hello`/`bind`). The demo bundle ships only the kernel now; guest
  programs are Linux binaries added through `EXTRA_BUNDLE_FILES`.

### Added

- `make linux-kernel-demo` prints where to download and unpack a kernel source
  tree (the kernel.org tarball, via `curl` and `tar`) when `KSRC` is unset, and
  the kernel-build runbook shows the same. The demo is runnable without hunting
  for the source.

## 0.33.0

### Added

- Versioning. `version.mk` is the single source of truth for the version,
  stamped into the binary through a generated `build/version.h` (not derived
  from git, so a tarball or tagless checkout still reports it).
- A comprehensive `jotun` usage. `--version`/`-V` prints `jotun <version>
  (<arch>)`; `--help`/`-h` prints the command line, the options, and every
  `JOTUN_*` environment variable.
