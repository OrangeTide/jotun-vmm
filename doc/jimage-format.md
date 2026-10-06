# The jimage guest-image manifest

A jimage is how a jotun guest image is named and distributed. It is a small text
object stored in the smolvfs content-addressed store. The object names a root
tree for each CPU architecture the image supports. The object's own hash is the
image identity: that one hash resolves to a complete, bootable, possibly
multi-arch image.

## Why a manifest object

Programs and the kernel are architecture-specific ELF files, so a multi-arch
image cannot be a single tree. Instead each architecture has its own root tree,
and the jimage object ties them together by hash. Because the store is content
addressed, arch-neutral files and whole identical subtrees are the same objects
across architectures, so they are stored and transferred once. Consequences:

- A multi-arch image costs little more than the architecture-specific binaries.
- Migrating a guest from one arch to another transfers only the objects that
  arch does not already have.
- Moving between a single-arch and a multi-arch image is cheap, because it only
  changes the jimage object, not the roots it points at.

## The object

The jimage is a typed CAS object of type `jimage`. Its payload is UTF-8 text.
The host reads it with a single object read while resolving the bundle, so the
cost is one small object.

The host dispatches on what a ref or a pinned hash resolves to. If it is a
`jimage` object, the host selects the row for its own architecture and serves
that root. If it is an ordinary directory tree, the host serves it directly, so
bundles that predate jimage keep working unchanged.

## Format

The text is RFC 822 / deb822 style. The rules:

- The first non-blank line is the magic: `jimage <version>`. This host
  understands version `1`.
- Every other content line is a field, written `key value`.
- A line that begins with a space or a tab is a continuation of the previous
  field's value. This lets a field such as `note` span several lines.
- A blank line is ignored.
- Unknown keys are ignored, so the format can grow without breaking old readers.

Fields:

- `arch <token> <root-tree-hash>`: one architecture variant. Repeat it once per
  architecture. At least one is required. The token is uname style, for example
  `x86_64` or `aarch64`.
- `name <text>`: a human-readable image name. Optional.
- `version <text>`: the image version. Optional.
- `upstream <text>`: where the image comes from. Optional.
- `note <text>`: free comment, which may fold across several lines. Optional,
  and may repeat.

The host reads only the version line and the `arch` rows. Every other field is
informational.

### Example

```
jimage 1
name jotun-demo
version 1.0
upstream https://example.com/images/jotun-demo
note a two-arch demo image. The second and third lines of this
 note are folded continuations, indented by one space, so they
 belong to the note field rather than starting new fields.
arch x86_64  4b1467c019a378ef655544cb547ce076763e4a5a2d68069d973d53cd4e87752e
arch aarch64 5a68925fad238e29fe225eb8c116ae9fc60b972297f6bbdb20e0fef487ecf120
```

## Booting from an image

The host serves the selected arch root as the guest's 9P filesystem and boots
the kernel it finds at the well-known path `boot/kernel` inside that root. So
the kernel is both the bootable image and a file in the guest's own namespace.

A ref may name a jimage object, and the jimage hash may be pinned directly (the
same ref-or-hash resolution as a plain root tree), which pins an immutable image
snapshot. A kernel path given on the host command line overrides the image, for
development.

## Building an image

`jbundle` builds an image into a depot. It mirrors a staging directory per
architecture into the store, preserving nested directories so `boot/kernel`
lands where the host looks, then writes the jimage object and commits a ref to
it. Entry uid, gid, and mtime are zeroed, so the same inputs always hash the
same way and reproducible builds dedupe cleanly.

```
jbundle -d <depot> --ref root --name jotun-demo \
    --arch x86_64=staging/x86_64 \
    --arch aarch64=staging/aarch64
```

`make bundle` drives this for the project's example bundle. It builds a
multi-arch image by default: the native architecture reuses the tested
`libos.elf` and `hello.elf`, and every other architecture is cross-built with
clang and lld. `BUNDLE_ARCHS` selects which architectures go in. Set it to this
host's architecture for a single-arch image that needs no cross toolchain:

```
make bundle                          # all supported arches
make bundle BUNDLE_ARCHS=x86_64      # this host's arch only
```
