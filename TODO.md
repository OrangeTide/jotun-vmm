# TODO

## Self-containment: no references to paths outside this project

Constraint: nothing in this project may refer to directories on the filesystem
outside the project root, and the build must not depend on any external
checkout. Upstream projects are named, never cited by a filesystem path.

- [x] Vendor the smolvfs dependency into the tree under `third_party/smolvfs/`
      and change the Makefile to build those in-tree sources. The external
      home-directory checkout dependency is removed. Vendored the CAS read path
      the host compiles (cas, cas-tree, cas-pack, cas-codec) plus the sources
      castool needs for the `bundle` target (cas-gc, cas-sign, cas-topic,
      castool, version.h), all built in-tree. The ref-signing crypto backend
      (monocypher) is still to be vendored when M8 lands.
- [x] Remove external home-directory path references from tracked files: the
      Makefile dependency variable, and the "Sources" citations in
      `doc/roadmap.md` and `doc/unikernel-design.md`. Upstream projects are now
      named without a filesystem path.
- [ ] Rewrite the entire git history to remove these external path references
      from historical file contents, so none remain in any commit. This is
      destructive: it rewrites every commit hash. Do it only with an explicit
      go-ahead and a backup.

Relates to M8: the pull client adds more smolvfs sources, so vendoring comes
first.
