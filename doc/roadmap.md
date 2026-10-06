# Jotun development roadmap

This expands the staged roadmap in `unikernel-design.md` into concrete
milestones. Each milestone ends in something runnable, and each names the real
decision or risk it carries. See the design doc for the architecture these
build toward.

## Where we start

After M0, the repository is a small host runtime under `host/`: it creates a
VM, maps guest RAM in multiple slots, runs N vcpus on their own threads with
mutex-guarded devices (a UART), loads an ELF guest, and services IO, MMIO, and
HLT exits. It runs on KVM (x86-64 verified) and Hypervisor.framework (arm64,
verified by inspection only). The sample guests live under `guest/`. The
milestones below continue turning that into Jotun.

The critical path is M1 (the transport ABI) then M2 (the guest library OS).
Everything after rides the ABI. The x86-64 guest execution mode is decided:
64-bit long mode with a trivial identity map. See M2.

## M0: host core refactor (done)

Goal: turn the smoke test into a reusable host runtime.

- Split the tree into `host/` and `guest/`.
- Keep the generic VMM core (`vmm.*`, vcpu and memory lifecycle) separate from
  the device code (`uart.*`) and the loader (`elf.*`). The split is by file
  within `host/`, not subdirectories.
- Support multiple memory slots and N vcpus, each on its own host thread.

Acceptance (met): a test guest runs on two vcpus, both halt cleanly, and guest
RAM mapped as two slots works.

As built, beyond the plan: each vcpu runs on its own thread and is created on
that thread, which Hypervisor.framework requires; `JOTUN_VCPUS=N` selects the
count; each vcpu gets its own stack carved from the top of guest RAM; KVM
assigns a distinct memory slot per mapping and a unique vcpu id per core
(atomic, since threads create vcpus); device state and stdout are mutex-guarded;
the build links `-pthread`.

## M1: transport ABI (done)

Goal: the single contract everything else rides on.

Transition from M0: the rings and event bitmaps live in guest RAM, but after M0
the guest-RAM base is a local in `main` and the vcpu threads only see the
backend-provided `exit_info` buffers. So the first task is a guest-memory handle
(host base, size, and guest base, identity-mapped for now) passed into the vcpu
dispatch, with a `gpa_to_host` accessor. The doorbell itself needs no new
backend work: it is an MMIO address recognized in the existing exit dispatch,
the same hook point where `MMIO_POWEROFF` is handled today.

- Add the guest-memory handle and `gpa_to_host` accessor.
- Define `abi/jotun_abi.h` with the ring descriptors, the doorbell MMIO layout,
  and the event-channel pending and mask bitmaps. It is shared verbatim by host
  and guest, so it uses only fixed-width integer types and no libc, since the
  guest builds freestanding. Both host and guest compiles add `-Iabi`.
- Host: recognize the doorbell MMIO address, drain the ring, and write
  event-channel bits into guest RAM. Serialize ring access across vcpu threads
  with the existing device lock.
- Guest: a small ring library in a throwaway bare-metal echo guest, in the
  style of the current test guests. This milestone does not need the M2 library
  OS.

Acceptance (met): `./jotun echo.elf` runs the echo guest, which sends a message
over the ring, the host echoes it back, the guest verifies it and sends a
second message. The host prints both, so the round trip is visible in each
direction, one doorbell exit per message.

As built: `abi/jotun_abi.h` holds the ring, doorbell, and event-channel layout;
`guest/jotun_ring.h` is the guest ring library; `guest/echo.c` is the throwaway
echo guest; the host adds `gpa_to_host` and `ring_service`, hung off the MMIO
exit dispatch at the doorbell address and serialized by the device lock. The
doorbell is a synchronous MMIO exit, so the response is ready when the store
returns; the event-channel pending bit is set anyway so the mechanism exists
for M4.

Scope: there is one global ring at a fixed address, validated single-vcpu.
Running the echo guest on several vcpus races on that one ring, which is
expected. Per-vcpu rings need a guest-visible vcpu id and come in a later
milestone.

Decision: start with guest-polled event channels. The guest checks the pending
bitmap at safe points, which avoids interrupt injection entirely. Real
asynchronous injection is deferred to M4, where preemption needs it.

## M2: guest library OS bring-up (done)

Goal: boot the JOS-derived library OS as a guest. This was the longest
milestone.

- Import the JOS core: the first-fit allocator is in. The ELF loader, vfs, 9p,
  and device table move with the filesystem work and land in M3.
- Write the greenfield x86-64 and arm64 arch ports: boot to C and the context
  switch are done; exception entry is deferred to M4, where preemption needs
  it. No privilege split, as planned.
- Round-robin scheduler on a single vcpu: done, cooperative.
- `spawn`, `exec`, `await`, notes as signals, string exit status: done.

Acceptance (met): `./jotun libos.elf` boots the library OS, runs an init task
that prints to the console, and halts.

As built: `guest/libos/` boots straight to C with no asm stub, since the host
hands the vcpu a usable stack. The arch seam is `arch.h`: a console byte sink,
a halt, `stack_init`, and `ctx_switch`, implemented per arch under `arch/`
(port I/O and HLT on x86-64, PL011 MMIO and a poweroff store on arm64, and a
greenfield 64-bit context switch in `arch/<arch>_ctx.S`). The core is the
JOS-derived allocator (`mem.c`) and the task layer (`task.c`): a cooperative
round-robin scheduler that ping-pongs with the boot context, with `task_spawn`
(records a parent), `task_exec` (replaces the program on a fresh stack),
`task_await` (reaps a child and returns its string exit status), `task_exits`,
`task_yield`, notes as a small per-task message queue, and `task_dump` for
observability. `exec` resolves a function directly; name-to-program resolution
arrives with the loader at M3.

Scope: scheduling is cooperative. A task runs until it yields, exits, or
exec's; there is no timer and no preemption yet, which is why exception entry
and async note delivery are held for M4. Notes are drained by the receiving
task rather than delivered asynchronously for the same reason.

Decided: x86-64 guests run in 64-bit long mode with a trivial identity map.
32-bit guests are not wanted, so the `-m32` path is interim only, used by the
current smoke-test guest until the 64-bit arch port replaces it. The identity
map has paging enabled but performs no translation and gives no per-process
isolation, which is consistent with the no-MMU-for-isolation stance. arm64
guests run at EL1 with the MMU off.

This replaces the current x86 setup. Today the KVM backend puts the vcpu into
32-bit protected flat mode and the test guest is built with `-m32`; M2 switches
both to 64-bit long mode with an identity-mapped page table and a 64-bit guest
build.

## M3: 9P over the ring, the bundle, and guest parameters (done)

Goal: the guest sees a host filesystem, loads programs from it, and boots with
host-supplied parameters.

Progress: the boot-info block and the 9P path are done. `abi/jotun_boot.h`
defines a parameter block at a fixed guest-physical address (one page above the
ring); the host writes the RAM size and a packed argv blob, and the library OS
reads and reports it at boot. `abi/jotun_9p.h` adds a shared request/response
buffer and a 9P doorbell, with a byte-wise wire codec shared by host and guest.
The host 9P2000 server (`host/p9srv.c`) runs behind a small backend vtable;
`host/p9smolvfs.c` resolves a ref in a smolvfs depot and serves its tree and
blobs, and `host/p9mem.c` is an in-memory fallback. The guest client
(`guest/libos/p9.c`) mounts, walks, reads, and lists. `make bundle` builds a
test depot from `example/bundle/` with smolvfs's castool. The guest ELF loader
and exec-by-name are done too: `guest/libos/elfload.c` loads a program's
PT_LOAD segments over 9P into a fixed program region, and `prog_run`
(`guest/libos/prog.c`) runs it as a child task and collects its exit status. A
program is a standalone ELF (`guest/prog/`) that reaches the libOS through a
service vector the kernel publishes (`abi/jotun_svc.h`), since there is no
privilege boundary to trap across. The shell `jsh` (`guest/libos/jsh.c`) is
done: with no console input device yet it runs a startup script read from the
bundle (named by the first boot argument), with `echo`, `ls`, and `cat`
built in and any other word run as a bundled program. The acceptance is met:
`./jotun libos.elf rc` boots, lists the bundle, cats a file, and runs the
`hello` program. The namespace and the manifest close out M3. `guest/libos/ns.c`
resolves every path through a mount table (longest-prefix) before walking the
9P tree; jsh gains `bind` and `mount`, and `prog_run` resolves names the same
way. `host/manifest.c` reads a per-guest manifest naming the bundle, ref,
subtree, and init arguments; `JOTUN_MANIFEST` selects it. The smolvfs backend
confines the served root to the manifest's subtree, so the guest cannot name
anything outside it. A bad subtree is refused.

Scope: one global namespace (per-process namespaces are later), and the bundle
is flat, since castool's import builds a single directory level; nested
subtrees need the smolvfs library API and are deferred. bind and mount are the
same operation over the single filesystem; real additional mounts arrive with
networking (M5) and display (M6).

Transition from M2: M2's `exec` resolves a function directly and the guest has
no loader. M3 adds the loader and makes `exec` resolve a program by name, read
as an ELF file from the namespace. This is where the JOS ELF loader, vfs, and
device table, held back from M2, land.

- Boot-info block: the host writes a parameter block into guest RAM at a fixed
  low address before the vcpu starts, carrying the RAM size, the init program
  name, and a packed argv and env blob. The library OS reads it at boot and
  runs the named init with those parameters. This is the general host-to-guest
  parameter channel; see `unikernel-design.md` section 7.1.
- Guest 9P client on the `archp9rpc` seam, routed to the ring.
- Host 9P2000.u server, reading from the guest's bundle. The bundle is a
  smolvfs store (`smolvfs`): a content-addressable store, a Merkle tree
  layer with refs and snapshots, and a packfile that is the distributable
  artifact; see `unikernel-design.md` section 7.1. The server resolves a ref to
  a root tree, maps Twalk onto `cas_tree_lookup`, Tstat onto a tree entry, and
  Tread onto `cas_open` (mmap). Content addressing dedups across files and
  bundles and verifies every object against its hash. The first cut reads a
  local packfile with objects uncompressed; the per-object DEFLATE codec is
  deferred, and ref signing (`cas-sign`) lands with the pull stage (M8), where
  bundles first arrive from an untrusted origin. Multi-arch is out of the proof
  of concept. The host owns all bundle access; the guest only issues 9P reads.
- Guest-side ELF loader reading programs from the namespace, so `exec(name)`
  loads the named program out of the bundle. Several bundled programs then run
  together in one guest domain.
- Guest namespace `bind` and `mount`.
- Per-guest manifest, read by the host at launch: it names the bundle, the
  subtree the guest may see, the init program and its parameters, and the
  coarse rights that grow through M5 and M6. It is the full description of a
  guest domain.

Acceptance: a bundle holding `jsh` and a couple of small programs boots; the
host passes init parameters through the boot-info block; `jsh` does directory
and file reads over the mounted bundle and spawns one of the bundled programs
by name.

## M4: preemption and SMP

Goal: multiple cores, preemptive scheduling, and threads.

- A para-virtual timer event.
- Event injection added to the backends. This is where guest polling stops
  being enough.
- A second vcpu, the scheduler across vcpus, threads, and a cross-vcpu wakeup
  event.

Acceptance: two CPU-bound tasks are preempted and balanced across two vcpus, and
a thread test passes.

Risk: interrupt injection without an emulated controller differs per backend
(KVM x86 without an in-kernel irqchip, KVM arm64 without a vGIC, and HVF IRQ
injection). Budget a short research spike here. It is the least certain
mechanism in the design.

## M5: networking

Goal: container-style limited networking.

- Host `/net` provider enforcing the manifest policy: allowed ports, NAT, and
  rate limits.
- Guest `/net` tree and a socket-like API.

Acceptance: the guest dials an allowed host and port through `/net`, and a
disallowed port is refused.

## M6: display, the 2D vector path

Goal: the primary use case, interactive diagrams and schematics in a browser.

- Guest `/dev/draw` emitting the Quaoar line protocol over the ring.
- Host front end, ported from `quaoar-server` to the ring, then a WebSocket to
  the browser.
- Reuse `quaoar-client.html` as the browser display.

Acceptance: a guest app draws an interactive diagram in a browser window and
receives a click or other semantic event back.

## M7: display, the pixel path

Goal: raster content where vector does not fit.

- A shared framebuffer region with dirty rectangles.
- The host validates the region geometry, composites it as a scene node, and
  encodes it for the browser.
- Optional guest-side Triton as a Glide library rendering into that
  framebuffer.

Acceptance: a guest software-renders a wireframe into a framebuffer node
composited under the 2D chrome.

## M8: image distribution, signed pull from a URL

Goal: fetch and authenticate a guest bundle from a remote origin, docker or
podman style.

Depends only on M3, not on M4 through M7, so it can slot in whenever it is
wanted. It adds the client side of the smolvfs REEF protocol and its HTTP
profile (`smolvfs`, REEF.md and DOWNLOAD.md): a client fetches a snapshot
from a static origin over plain HTTP, downloads only the objects it lacks,
verifies each against its hash, and resumes after interruption. Signing is part
of this stage, not a later add-on, since this is where a bundle first arrives
from an origin that is not already trusted. The host then boots the pulled
bundle exactly as a local one.

- Client-side fetch against static hosting (no server logic), resolving a ref
  and walking the object set it is missing.
- A local object cache so repeated pulls and shared bases transfer once.
- Ref signing and verification (`cas-sign`, `smolvfs`). Content
  addressing already verifies each object's bytes against its hash; signing
  covers the missing half, which root ref is authentic. The publisher signs the
  ref; the client verifies the signature against a public key it trusts
  (pinned, or the origin's key obtained out of band) before the pulled root is
  used, and refuses an unsigned or badly signed ref. Key distribution and
  rotation are kept minimal for the proof of concept; multi-arch stays out of
  scope.

Acceptance: `jotun` pulls a bundle from a URL, fetching only the missing
objects, verifies the signed ref against a trusted key, and boots a guest from
it; a bundle whose ref signature is missing or wrong is refused.

## Cross-cutting concerns

- The per-guest manifest is the sandbox's control surface. It appears at M3,
  naming the filesystem bundle and the init parameters, and grows through M5
  for networking and M6 for display.
- Validation of every guest-to-host message is a standing requirement, not a
  phase.
- Multiplexing several guests onto one browser session, and any session
  authentication, land with or after M6.
- Snapshot and restore is optional and comes later.
- Single-file packaging is optional and comes later: the bundle can also carry
  the bootable unikernel ELF, which the host lifts out to boot the guest while
  serving the rest as the filesystem. One file then holds the kernel and its
  filesystem together. Not needed for the M3 first cut.
