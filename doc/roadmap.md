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
networking (M5).

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
  smolvfs store: a content-addressable store, a Merkle tree
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
  coarse rights that grow through M5. It is the full description of a
  guest domain.

Acceptance: a bundle holding `jsh` and a couple of small programs boots; the
host passes init parameters through the boot-info block; `jsh` does directory
and file reads over the mounted bundle and spawns one of the bundled programs
by name.

## M4: preemption and SMP (done)

Goal: multiple cores, preemptive scheduling, and threads.

- A para-virtual timer event.
- Event injection added to the backends. This is where guest polling stops
  being enough.
- A second vcpu, the scheduler across vcpus, threads, and a cross-vcpu wakeup
  event.

Acceptance: two CPU-bound tasks are preempted and balanced across two vcpus, and
a thread test passes.

Design decision (made): to preempt a running, possibly spinning, guest, a host
timer thread (opt-in, `JOTUN_HZ`) sets a per-vcpu pending flag and signals the
vcpu thread (`SIGUSR1`) so `KVM_RUN` returns `EINTR`. On x86 KVM with no
in-kernel irqchip, the backend then injects with `KVM_INTERRUPT` when
`ready_for_interrupt_injection`, else sets `request_interrupt_window` and
retries on `KVM_EXIT_IRQ_WINDOW_OPEN`. The event vector is fixed
(`JOTUN_EVENT_VECTOR`, 0x20). The guest installs a GDT (so the interrupt path
can reload CS/SS), an IDT gate, and a full-register ISR; `ctx_switch` now
saves and restores RFLAGS so the interrupt-enable bit travels with each
context, and scheduler-critical regions mask interrupts. The ISR reschedules
through the existing cooperative switch, so preemption and cooperative yield
share one path.

Progress (M4.1, x86): single-vcpu preemption works and is verified. With
`JOTUN_HZ=100`, two CPU-bound tasks that never yield interleave; without the
timer one runs to completion before the other.

SMP (M4.2, x86, done): every vcpu starts at the same entry and the asm `_start`
(`arch/<arch>_boot.S`) passes the vcpu id to `kmain` in the first argument
register (`VMM_REG_ARG`: RDI on x86, X0 on arm64). Each cpu finds its private
state through a per-cpu base register (GS base on x86, `TPIDR_EL1` on arm64)
and runs its own scheduler loop over one shared run queue guarded by a
spinlock, so ready tasks spread across whichever cpus are idle. cpu 0 does the
one-time bring-up (console, heap, 9P, namespace) and spawns init; the others
wait on a boot flag, then join. The heap allocator takes a spinlock too, since
it is now shared across cpus. The SMP-critical rule is that a yielding or
exec'ing task is not re-queued until its stack pointer is saved: the task only
marks itself ready and switches away, and the scheduler enqueues it after the
switch, so no other cpu can resume a half-saved context.

An idle cpu blocks in the host with a store to `JOTUN_IDLE_DOORBELL` (an MMIO
exit the host parks the vcpu thread on, holding no device lock); a cpu that
queues work stores `JOTUN_WAKE_ALL` to `JOTUN_WAKE_DOORBELL` to rouse the
idlers. When the run queue is empty and no cpu is running a task, the system
has quiesced: the discovering cpu sets a shutdown flag and wakes the rest so
every scheduler returns and each cpu halts.

Acceptance met on x86 KVM: with `JOTUN_VCPUS=2`, two CPU-bound tasks run at the
same time (one per cpu) and a locked-counter test (four tasks racing on a
shared counter) produces the exact total, with clean shutdown. The same binary
still runs correctly on one vcpu, cooperatively or preempted.

Scope: injection and SMP are implemented and tested on x86 KVM only. KVM arm64
(no vGIC) and HVF IRQ injection need hardware to validate, so the arm64 guest
keeps the cooperative path (it runs at EL1 with IRQs masked); the HVF backend
sets the pending IRQ by analogy with KVM but is unverified. The per-cpu base,
boot path, and idle/wake doorbells are arch-neutral and build clean for arm64
on both the gcc and clang toolchains, but are not yet run on arm64 hardware.
The arm64 trap vector (`VBAR_EL1`) is installed when that work lands.

Risk: interrupt injection without an emulated controller differs per backend
(KVM x86 without an in-kernel irqchip, KVM arm64 without a vGIC, and HVF IRQ
injection). It is the least certain mechanism in the design.

## M5: networking (done)

Goal: container-style limited networking.

- Host `/net` provider enforcing the manifest policy: allowed ports, NAT, and
  rate limits.
- Guest `/net` tree and a socket-like API.

Acceptance: the guest dials an allowed host and port through `/net`, and a
disallowed port is refused.

Decisions (made): the guest-facing API is a hybrid. A socket-like client
(`net_dial`/`net_read`/`net_write`/`net_close`, plus UDP `net_sendto`/
`net_recvfrom`) carries the data path over a new net channel with the same
shape as the 9P channel: a shared request/response buffer and a synchronous
doorbell. `/net` itself is a read-only information tree served through the
existing 9P backend (`/net/policy`, `/net/conn/...`), for naming and
observability, not for byte transfer. Scope here is outbound TCP connect and
UDP datagrams; listen/accept and inter-guest links landed later in M14.

The host provider opens real host sockets (that is the NAT) and keeps them
non-blocking. A host poll thread is event-driven, not periodic: it blocks in
`epoll_wait` with no timeout and wakes only when a socket actually transitions.
An fd is in the epoll set only while a guest task is waiting on it: the net
doorbell handler arms it (one-shot) the moment a non-blocking `recv` or
`connect` returns `EAGAIN`/`EINPROGRESS`, and readiness disarms it until the
next would-block. On readiness the thread injects the event vector into running
vcpus and wakes any idle ones. An idle connection has no registered fd, so
there is nothing to spin on. A single `eventfd` in the set breaks the wait at
shutdown. (`epoll` is Linux-only. The net provider now carries a `kqueue`
backend behind the same `poller_*` interface for macOS, with a self-pipe in
place of the `eventfd` wakeup; it is arm-on-`EAGAIN` and one-shot like the
Linux path. The macOS backend is confirmed to build and run on macos/arm64.)

Blocking is at task granularity, not vcpu granularity, so a task waiting on the
network does not stall the other tasks on its cpu. A `net_read` that would
block parks the task (the `TASK_BLOCKED` state from M5.1) and the task is
re-readied on the next net event. The parking reuses the yield invariant that a
task is never re-queued until its stack pointer is saved: a wake only re-queues
a task the scheduler has already parked. A wake that lands in the window after
the task marks itself blocked but before it finishes parking is not lost: it is
held as `wake_pending` and honoured by the scheduler's park step. That is what
lets the host side be strictly edge-triggered and one-shot without a re-delivery
timer.

Steps:

- M5.1 (done): the scheduler sleep/wake primitive. A `TASK_BLOCKED` state,
  `task_block` and `task_wake`, a quiescence check that does not shut down while
  tasks are blocked, and a re-ready pass when an idle cpu is woken. Self-tested in
  the guest with no sockets.
- M5.2 (done): the net channel ABI (`abi/jotun_net.h`), the host net provider
  (`host/net.c`: TCP connect, non-blocking sockets, an epoll poll thread,
  policy stubbed to allow-all), and the guest net client
  (`guest/libos/net.c`: `net_dial`/`net_write`/`net_read`/`net_close`). A
  would-block parks the calling task through the M5.1 primitive and the net
  event hook re-readies it. Verified end to end on x86 KVM against a loopback
  server: dial, write, blocking read, close, on one and two vcpus and with the
  preemption timer on (10 iterations clean), plus a clean error on a refused
  connection. Policy enforcement and the `/net` tree arrive in M5.4.
- M5.3 (done): UDP. `net_udp` opens an unconnected datagram socket, and
  `net_sendto`/`net_recvfrom` carry a per-datagram "host!port" address through a
  new `addr` field in the channel, with three ops (`NET_OP_UDP`,
  `NET_OP_SENDTO`, `NET_OP_RECVFROM`). recvfrom reports the source as a numeric
  address and blocks the task until a datagram arrives, reusing the same park
  path as TCP. IPv4 for now. Verified on x86 KVM against a loopback UDP echo
  server on one and two vcpus with the timer on.
- M5.4 (done): manifest network policy and its enforcement. The manifest gains
  `net allow <cidr> <ports> [tcp|udp]` rules; the host resolves each dial or
  sendto target and the net provider allows it only if the resolved address
  matches a rule. With no rules every target is allowed; with one or more the
  default is deny. Matching on the resolved address, not the dialed name, means
  a name cannot reach a blocked address. IPv4 rules for now. The allowed/
  refused acceptance test passes on x86 KVM: a permitted loopback port connects
  and exchanges data, a port outside the rules is refused (NET_EPERM), and the
  proto field distinguishes a TCP rule from a UDP one on the same port.
- M5.5 (done): the `/net` information tree over 9P. A read-only overlay backend
  (`host/p9overlay.c`) serves `/net` synthetically, delegating every other path
  to the bundle backend. `/net/policy` renders the active rules (or "allow
  all"), `/net/conn` lists live connections, and `/net/conn/<handle>` shows a
  connection's protocol and peer. Synthetic nodes are tagged by a low byte in
  the backend handle, which real backends never produce, and carry qids in a
  reserved high range. Verified on x86 KVM: `ls /net`, `cat /net/policy` with
  and without rules, and `cat /net/conn/0` against a live connection, with
  bundle delegation and policy enforcement intact.

## M6-M7: display (removed)

The 2D vector and pixel display paths were built here (a `/dev/draw` channel, a
host WebSocket front end, and a browser client). They were removed in v0.34.0
when jotun became Linux-only. The detailed history is dropped; see CHANGELOG.md.

## M8: image distribution, signed pull from a URL (done)

Goal: fetch and authenticate a guest bundle from a remote origin, docker or
podman style.

Depends only on M3, not on M4 through M7, so it can slot in whenever it is
wanted. It adds the client side of the smolvfs REEF protocol and its HTTP
profile (smolvfs REEF.md and DOWNLOAD.md): a client fetches a snapshot
from a static origin over plain HTTP, downloads only the objects it lacks,
verifies each against its hash, and resumes after interruption. Signing is part
of this stage, not a later add-on, since this is where a bundle first arrives
from an origin that is not already trusted. The host then boots the pulled
bundle exactly as a local one.

- Client-side fetch against static hosting (no server logic), resolving a ref
  and walking the object set it is missing.
- A local object cache so repeated pulls and shared bases transfer once.
- Ref signing and verification (smolvfs `cas-sign`). Content
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

Status (done):

- M8.1: the remaining smolvfs sources are vendored in-tree and built into the
  host: the sign path (`cas-sign`, `cas-topic`) with the bundled monocypher
  EdDSA-BLAKE2b backend, and the miniz DEFLATE codec, under
  `-DCAS_WITH_MONOCYPHER -DCAS_WITH_MINIZ`. castool builds with the same
  backends, so it can sign refs and write compressed objects.
- M8.2: `host/http.c`, a minimal HTTP/1.1 GET client over plain sockets (no
  libcurl): parse an `http://` origin, connect with a bounded time, GET a static
  path, and return the body (Content-Length, chunked, or read-to-close). The
  transport is plain http; authenticity rests on the signature, not the
  transport.
- M8.3: `host/bundlepull.c`, the pull client. It resolves the publisher's topic
  ref from the trusted public key, fetches the signed version record, and
  verifies it with `cas_vrec_decode` (format, key binding, and EdDSA signature)
  plus a pin check that the record's key is the trusted one. It then walks the
  object set reachable from the published root, fetching only what `cas_exists`
  reports missing, verifying every object against its content hash
  (`cas_fsck_object`, escalating to `cas_tree_verify` for htrees), and writing
  each atomically. The verified root is committed under a local ref, so a rerun
  resumes by skipping present objects and the host serves the cache depot
  through the existing `p9smolvfs_open` path unchanged. Compressed (v2) objects
  are decoded and verified through the miniz codec.
- M8.4: config. A manifest may name `origin`, `key` (trusted publisher key,
  hex), and `label`; the env fallbacks are `JOTUN_ORIGIN`, `JOTUN_KEY`,
  `JOTUN_LABEL`, and `JOTUN_CACHE`. When an origin is set the host pulls before
  serving, and any failure refuses the boot rather than falling back. See
  `example/manifest-pull`.
- M8.5: pack byte-range transport. When the origin serves a packfile, the client
  reads only the extents it needs instead of downloading whole objects. It
  range-fetches the footer (a suffix request) for the entry count, then the
  sorted index, then each missing object's own `[data | trailer]` span, whose
  bytes are byte-identical to a loose object and so verify through the same
  path. `host/http.c` gained `http_get_range` (a `Range` request that reads the
  resource size from `Content-Range`). An origin may mix a pack with loose
  objects: an object absent from the index falls back to a loose GET, and an
  origin with no pack (or a server that ignores `Range`) pulls loose as before.
  The walk also learned to descend a jimage, enqueuing each per-arch root tree,
  so a multi-arch image pulls every arch with the shared objects transferred
  once (this is what makes an M11 jimage bundle pullable at all).
- M8.6: key rotation. A topic name is the hash of its key, so a key is never
  changed in place; rotation is an endorsement from the retiring key to its
  successor. The publisher puts a record on the retiring key's reserved
  `successor` topic whose root names the successor's own head record. A client
  pinned to the old key follows the endorsement (signed by the old key) to that
  record, adopts the key it is signed by, and resolves the bundle under the new
  key, all without trusting the transport. The follow is bounded and chains
  across several rotations.

Verified end to end on linux/x86-64: a cold pull fetches and verifies the
objects and boots the guest; a warm run fetches only the ref and reuses the
cache; a wrong trusted key, a corrupt object, a corrupt record, and a record
signed by an untrusted key are each refused; and a compressed depot pulls,
decodes, verifies, and boots. A packed origin (loose objects pruned) pulls by
byte range and boots; a client pinned to a retired key follows a signed
endorsement to the active key and boots, while an untrusted key is refused.

## M9: guest libc-lite (done)

Goal: a small freestanding library for the guest, so the libOS stops
open-coding the same primitives and bundle programs become writable. Not a
full libc. It is cross-cutting and independent of the distribution
work, so it lands opportunistically rather than in sequence.

Scope: `string.h` (mem{cpy,set,move,cmp}, str{len,cmp,ncmp}, a bounded
strlcpy); a bounded formatter (`vsnprintf` or a sink-callback `vfctprintf`)
whose output goes through the service vector, never a syscall; `strtol`/`atoi`.
Deliberately excluded: `malloc` (the libOS already has `kmalloc`; programs get
a bump allocator if needed later), any file or stdio-stream model, and anything
syscall-shaped. These are written jotun-native; the formatter may be based on
the nyctos `printf.c` format engine (0BSD) with
its header preserved if copied. nyctos's `sys`, `console`, `file`, and `compose`
layers do not transfer, because they are bound to its syscall and fs model and
it has no x86_64 port.

Steps:

- M9.1 (done): the `mem*`/`string.h` insurance. A freestanding target must
  provide memcpy, memmove, memset, and memcmp, because the compiler can lower a
  struct copy or an array initializer into a call to one even where the source
  never names it; the guest referenced none of them and was one such construct
  away from a link failure. `guest/libos/string.c` provides those four plus
  strlen, strcmp, strncmp, and strlcpy, compiled into the libOS.
- M9.2 (done): de-duplicate the open-coded helpers onto this header. ns.c
  `sstrlen`/`sstrcpy` became `strlen`/`strlcpy`, net.c `ncopy` became `memcpy`,
  task.c `scopy` and ns.c's copies became `strlcpy`, and jsh.c `streq` became
  `strcmp() == 0`; each file now includes `string.h`. p9.c was left as is: its
  loops are 9P wire marshalling and stream decoding through the bounds-checked
  codec in `abi/jotun_9p.h`, which host and guest share, not string helpers.
- M9.3 (done): the bounded formatter. `guest/libos/fmt.c` is a sink-based
  engine, `vfctprintf(out, arg, fmt, ap)`, with `snprintf`/`vsnprintf` built on
  a bounded buffer sink and `con_printf`/`con_vprintf` (print.h) built on a
  console sink, so formatted output reaches con_putc without a trap (the guest
  has no syscalls). It is jotun-native, not copied, so it carries no third-party
  header. Flags `- 0 + space #`, numeric or `*` width and precision, length
  `h hh l ll z t j L`, and conversions `d i u o x X b c s p %`; no allocation. The
  integer engine compiles integer-only on arm64 under -mgeneral-regs-only (no
  64-bit-divide helper) and matches glibc snprintf across a case sweep. The
  floating conversions `f F e E g G` were added later (done 2026-10-07): because
  even reading a double argument needs FP instructions the integer-only flag
  forbids, they live in their own translation unit `guest/libos/fmtfloat.c`,
  built with the FPU on like the rasterizer and called from the integer core.
  They round half to even, cover infinity and NaN, and are checked against glibc
  across a case sweep by `make fmt-test`. Exposing the formatter to bundle
  programs is M9.4.
- M9.4 (done): expose the library to bundle programs. The `jotun_svc` ABI stays
  minimal (putc/puts/exit); the convenience code is a small program C library
  linked into each program instead. `guest/prog/libc/` holds the umbrella header
  `jlibc.h`, program-side `printf`/`vprintf`/`putchar`/`puts` that route through
  the service vector's putc (progio.c), and `strtol`/`atoi` (progstdlib.c); the
  shared `fmt.c` and `string.c` compile into it too. It is linked as plain
  objects, not an ar archive, for cross-toolchain portability; each function is
  its own section and programs link with `--gc-sections`, so unused library code
  costs nothing. `hello` now uses printf and strtol/atoi. strtol matches glibc
  across a case sweep; the program objects compile integer-only on arm64 under
  -mgeneral-regs-only.

Atomics and threading are deliberately not libc work here. C11 `<stdatomic.h>`
is a compiler-provided freestanding header, and the word-sized atomics the guest
uses lower to inline instructions with no out-of-line library calls (verified on
x86-64 and arm64 under the guest's flags, with `-mno-outline-atomics` on arm64).
So atomics need no library code; use `<stdatomic.h>` directly in place of raw
`__atomic_*` builtins. (Only 16-byte atomics would pull in libatomic, and the
guest uses none.) Threading already has its substrate in `task.c` (spawn,
block/wake, self, spinlocks). The standard `<threads.h>` shim (`thrd_*`/`mtx_*`/
`cnd_*`, plus `call_once`) over `task.c` landed in `guest/libos/threads.c`: a
thrd_t is a task id, a thread is a child task joined by its creator (task_await
reaps it), create hands the start function to a trampoline through a per-id
table with a release/acquire handshake, join collects the int return and buffers
out-of-order completions, mutexes are compare-and-swap locks that yield while
contended, and condition variables park waiters with task_block/task_wake. The
`threads` guest demo runs it (a condvar-gated waiter, mutex-protected counting
workers, and call_once), verified under multiple vcpus. Limits of the subset,
noted for the next consumer: a thread is joined by its creator, there is no
guest clock so the timed variants are omitted and thrd_sleep is a cooperative
yield, and tss_* thread-specific storage is not provided.

## After M8: the forward milestones

The core plan in the design doc is now implemented end to end: the transport
ABI, 9P and the bundle, the shell, preemption and SMP, and networking, with
signed distribution on top. (The display paths built here were removed in
v0.34.0; see M6-M7.) What remains are the forward milestones below, the
cross-cutting concerns, and the deferrals accumulated along the way. They are
largely independent, so they land as wanted rather than in a strict sequence.
M9 (above) stays opportunistic.

## M10: multi-guest multiplexing (done)

Goal: run several isolated guests in one host process, each with its own VM,
RAM, and device channels.

The host was strictly one guest: one process was one VM, and the N vcpus were
SMP cpus of that one guest, sharing one RAM and one set of device channels (net,
p9) at fixed GPAs resolved against a single backing buffer.

Locked decisions (interview, 2026-10-06):

- Process model: several VMs in one host process, each its own guest with
  private RAM and its own net/p9 devices. Guests are isolated by default
  (separate VMs and address spaces), so the fixed channel GPAs do not collide.
  The host is the only thing that can bridge two guests, which keeps the trust
  model intact. Chosen over a process-per-guest aggregator, since that would
  need an inter-host IPC bus for the same result.
- Launch: one guest per manifest, named with repeated `-m <manifest>` arguments
  (`jotun -m web.manifest -m db.manifest libos.elf`). The existing manifest
  (bundle, net policy, init params) is unchanged and each guest is specified
  independently.

Sub-milestones:

- M10.0: run N guests in one host process. Collapse the file-scope singletons
  (vm, backing RAM, vcpu table, netdev, p9srv, manifest) into a per-guest
  struct, launch one VM per manifest, and carry a `struct guest *` on each vcpu
  thread so the doorbell demux and the net/p9/ring service calls target that
  guest's devices. The SMP wake and event-vector injection become per-guest.
  This is the schedule risk in M10, so it is done in small steps that each keep
  the build and the existing single-guest tests green, with N held at 1 until
  the last:
  - M10.0a: introduce `struct guest` and group the file-scope singletons into
    one instance of it. Pure data-grouping, no behavior change.
  - M10.0b: thread `struct guest *` through run_vcpu, the doorbell demux, and
    the net/p9/ring service calls, replacing the global reaches. Still one
    guest, identical behavior.
  - M10.0c: make the wake and event path per-guest (rouse_vcpus, the SIGUSR1
    wake, event-vector injection target one guest's vcpu set, not the global
    table). Isolated because it is the easy place to introduce a cross-guest
    wake bug. Identical with one guest.
  - M10.0d: give each guest its own RAM buffer and VM through the struct (both
    are global today); still allocate one, to prove the per-guest path.
  - M10.0e (done): flip to N guests. `jotun [-m <manifest>]... <kernel> [args]`
    runs one guest per `-m` (each its own VM, RAM, net/9p devices, and manifest
    config), with no `-m` keeping the single env/CLI guest exactly as before.
    guest_bringup resolves config, brings a guest up, and spawns its vcpus. The
    preemption timer and the join and shutdown loops span all guests. Known
    limitation: all guests share the one serial console, so their output
    interleaves per character; per-guest console tagging is a separate concern,
    not part of M10.
- M10.1-M10.3: per-guest id namespacing, host-side compositing, the browser
  session layer, and its auth boundary. These were display work (the removed
  draw front end), done then removed in v0.34.0. See CHANGELOG.md.
- M10.4 (done): shared read-only filesystem mount between guests. Two guests
  point their 9P subtree at the same bundle depot, one side owning ref commits
  and the other reading. This already worked for a live ref (the depot is a
  content-addressed, immutable CAS, so concurrent readers never block a reader:
  the depot lock is taken only on a ref commit, not on reads). The step that
  landed is snapshot pinning: a manifest ref may be either a ref name (which its
  owner may advance) or a fixed 64-hex root tree hash, which pins an immutable
  snapshot. A consumer serving `ref <hash>` sees a stable tree even as the owner
  commits new roots behind the same `head` ref. A pinned hash is caller-supplied,
  so the host validates it loads as a real tree and fails the mount cleanly
  otherwise, rather than serving an empty root. Demonstrated by the two-guest
  run in M10.0e, which already shares one depot. Grafting a shared mount at a
  path inside a guest's own tree (rather than as the whole root) landed later in
  M13. The
  inter-guest network link (an in-host virtual switch) is also deferred: it needs
  a net listen/accept or rendezvous primitive that does not exist yet (design
  section 8), so it is tracked under opportunistic work, not M10.

Suggested order: M10.0 first (unblocks the rest), then M10.4.

## M11: single-file packaging (done)

Goal: one artifact that carries both the bootable unikernel ELF and its
filesystem, docker-image style, and that spans several CPU architectures. A
pull delivers a complete, signed, bootable image in one object set. Depends on
M3 and M8.

What landed: image identity is a small typed CAS object, a jimage manifest,
that names a root tree hash per architecture. The object's own hash is the
distributable image id. Each arch root is an independent tree, so in the
content-addressed store arch-neutral files and whole identical subtrees are
shared across arches, and migrating between arches, or between a single-arch
and a multi-arch image, only transfers the objects that are missing.

- Host read path. `p9smolvfs_open` takes an arch token (uname-style `x86_64` /
  `aarch64`, fixed at build time from `__x86_64__` / `__aarch64__`, since KVM
  and HVF both run the host CPU). When the resolved ref or pinned hash names a
  jimage object it selects the matching arch row and serves that root; an
  ordinary tree root is served directly as before (so existing bundles are
  unchanged). The kernel is lifted from `boot/kernel` inside the served root;
  `elf_load_mem` loads it from memory. A command-line kernel still overrides for
  development, and the historical `test_guest.elf` default still boots a bare
  run. A pinned jimage hash composes with M10.4, giving an immutable image
  snapshot.
- jimage format. An RFC 822 / deb822 style text object: a magic line `jimage
  <version>`, then `key value` fields where a line beginning with whitespace
  folds onto the previous field (so a `note` may span lines), blank lines are
  ignored, and unknown keys are skipped for forward growth. The host reads only
  the version line and the `arch <token> <root-hash>` rows; `name`, `version`,
  `upstream`, and `note` are informational. See `doc/jimage-format.md`.
- Build path. `jbundle` mirrors a staging directory per arch into the depot
  (preserving nested directories, so `boot/kernel` lands where the host looks),
  then writes the jimage object and commits the ref. Entry uid/gid/mtime are
  zeroed so the same inputs always hash the same way. `make bundle` builds a
  multi-arch image by default: the native arch reuses the tested `libos.elf` /
  `hello.elf`, and every other arch is cross-built with clang and lld.
  `BUNDLE_ARCHS` selects which arches go in; set it to this host's arch for a
  single-arch image that needs no cross toolchain.

## M12: snapshot and restore (deferred)

Goal: capture a running guest's memory and device state and resume it later.
Optional, and the heaviest of the forward items; it interacts with SMP (M4) and
with the channel state on each device. Deferred: there is no concrete need yet,
so it stays unbuilt until one appears.

Intended shape, when it is built (decided 2026-10-06, so the groundwork is not
relitigated later):

- Cooperative checkpoint, not host-forced capture. The guest reaches a
  quiescent point and writes a new snapshot doorbell (the same MMIO doorbell
  mechanism the ring, 9P, and net channels already use). The guest handles
  its own register context, which its task switcher already saves and restores
  (`guest/libos/task.c` and `arch/*_ctx.S`). The host captures guest RAM plus a
  small resume vector, and on restore reloads RAM and re-enters a guest resume
  trampoline; the guest re-initialises its devices (reconnect sockets, remount
  9P). This was chosen because two facts make a host-forced capture
  costly and low-value here: the VMM abstraction cannot read back a vcpu's full
  architectural register state today (it only sets PC/SP/ARG at creation, so a
  forced capture needs a large new get/set-all-registers surface in both
  `vmm_kvm.c` and `vmm_hvf.c`), and the net device holds live host sockets that
  have no serialisable form. The cooperative model sidesteps both.
- A snapshot is a flat file for the first cut, not CAS objects. Simpler, with no
  dedup against the bundle; revisit CAS storage (chunked, content-addressed,
  signable and pullable like a jimage) only if snapshots are distributed.
- Live sockets are not preserved: the guest re-establishes connections on
  resume. A snapshot therefore captures a guest's own state, not its external
  I/O, which suits the cooperative model.

## M13: graft a shared read-only mount at a path (done)

Goal: mount another guest's (or a shared depot's) read-only tree at a path
inside a guest's own filesystem, rather than serving it as the whole root as
M10.4 does, so a guest sees both its own tree and a shared tree at once.

What landed: `host/p9graft.c`, a backend that wraps a base backend and makes a
second backend appear as one top-level directory in the base root. It models on
the `/net` overlay, but the injected subtree is a real backend rather than
synthetic files, and it composes under the overlay (a guest root shows its own
entries, the grafted name, and `net`). Each operation routes to the backend that
owns the node, tagged by the top bit of the qid path; the same bit keeps the two
trees' qids disjoint, so the 9P client never confuses a base file with a graft
file. The manifest gains `mount <name> <bundle> <ref> [subtree]`, and the shared
bundle resolves a ref, a pinned hash, or a jimage exactly as the guest's own
bundle does, so a snapshot can be pinned for a stable shared tree. Serving a
second smolvfs bundle in one guest needed the per-open context fix (each
`p9smolvfs_open` now owns its context rather than a single file-scope one).

Scope of the first cut: one graft, at a top-level name in the root. The name
must not already exist in the base root, or the graft shadows it. Grafting at a
deeper path, or more than one graft per guest, is a mechanical extension (chain
the wrapper) left until a need appears.

## Opportunistic and deferred work

Independent of the milestones above, tracked so they are not lost:

- Pull transport (M8): the pack byte-range transport and key rotation landed
  (see M8). Still open: pack-aware resume granularity and a published multi-pack
  layout for very large origins.
- libc-lite (M9): the `<threads.h>` shim landed (see M9). Still open: the timed
  variants (`mtx_timedlock`, `cnd_timedwait`) and precise `thrd_sleep`, which
  need a guest clock, and `tss_*` thread-specific storage. M9.1 through M9.4 are
  done.
- Inter-guest network link: landed in M14.

## M14: the net name service, listen/accept, and inter-guest links (done)

Goal: let guests offer and reach services, and let one guest reach another
without touching the real network, with the same address form as an external
connection.

What landed:
- Plan 9 dial strings. A connection is named `network!host!port` (`tcp` or
  `udp`), replacing the separate proto field, so the network is part of the
  address and external and inter-guest links share one form.
- A host name service. One process-wide switch (`vswitch` in `host/net.c`),
  shared by every guest's provider, holds the guest network names (a new
  manifest `name` field) and the registered services. The host field of a dial
  string resolves through it: a co-running guest's name is an in-host link, any
  other name resolves externally through DNS.
- listen/accept. `net_listen("tcp!*!port")` registers a service in-host under
  the guest's name; `net_accept` blocks the task until a peer connects. The
  listener's wake pipe doubles as its readiness fd, so accept parks on it like
  any socket.
- External bind. The same listen also opens a real listening socket, so a guest
  can accept connections from outside the host. It is opt-in: with no policy
  rules a listen is in-host only, and a real bind needs an explicit manifest
  rule naming the port (binding is default-deny, unlike dialing). A named guest
  that binds is reachable both ways at once: accept takes a pending in-host
  peer first, then a real inbound connection. The poller arms an arbitrary fd,
  so a listener watches its in-host wake pipe and its real socket together.
- Inter-guest links over a socketpair. A dial to a guest makes a socketpair,
  keeps one end as the dialer's connection, and queues the other on the
  target's accept backlog, poking its wake pipe. Send, recv, readiness, and
  close are then the ordinary socket path, so inter-guest traffic never touches
  the real network and the host stays the only bridge. The trust model is flat
  and host-mediated: any co-running guest may offer or reach any name.
- Startup-race rendezvous. A dial to a co-running guest that has not listened
  yet reports NET_EAGAIN rather than failing. The dialing task parks and retries
  the way a recv or accept does, and a listen wakes every guest so the parked
  dials retry and connect. All guest names are registered in the name service
  before any guest runs, so a dial to a peer that boots later still resolves
  in-host and parks instead of resolving the name externally. A client that
  starts before its server therefore connects regardless of bring-up order.
- Inter-guest UDP over the same name service. A datagram addressed to a
  co-running guest's name is routed through the switch and never touches the
  real network, like a dial. A guest binds a UDP port under its name with
  net_bind ("udp!*!port"); a socket that sends before it binds gets an ephemeral
  in-host source port, so a receiver can always reply. Delivery is a bounded
  per-endpoint datagram queue woken through the usual event path, and an
  over-full queue or an unbound target drops the datagram, matching UDP.
- Policy over inter-guest links. The manifest "net allow <who> ..." rule now
  takes a co-running guest's name in the <who> field, beside a CIDR or "*", so
  one rule set gates a guest's whole outbound reach. Like external dialing it is
  default-allow: with no rules a guest reaches any peer; with rules, a by-name
  dial or datagram must match a name rule or "*". A CIDR rule authorises only
  external addresses and a name rule only that guest, so external and in-host
  targets never authorise each other. What a guest may offer in-host is not
  gated, keeping the flat trust model.

Deferred: rate limits.

## M15: web hosting (removed)

This milestone remoted the display to a browser over the internet: a host
WebSocket front end, a session broker, and per-session `jotun` children routing
draw frames to browsers. It was display work and was dropped with the display
subsystem in v0.34.0. See CHANGELOG.md. Serving Linux guests over the network is
unscoped.

## Cross-cutting concerns

- The per-guest manifest is the sandbox's control surface. It appears at M3,
  naming the filesystem bundle and the init parameters, and grows through M5 for
  networking and M8 for the pull origin and trusted key.
- Validation of every guest-to-host message is a standing requirement, not a
  phase.
- Host portability to macos/arm64 is a standing concern. As of the M8 work the
  host builds and runs on macOS: the Makefile sets
  `_DARWIN_C_SOURCE`/`_DEFAULT_SOURCE` so the vendored smolvfs sources see the
  BSD names they use, and the net provider has a `kqueue` backend (self-pipe
  wakeup, `SO_NOSIGPIPE` per socket) beside the Linux `epoll` one. The build and
  run on macos/arm64 are confirmed. Development here is on Linux, so a quick
  clang syntax check of the macOS net path (against a kqueue-API shim) guards it
  between macOS builds. See the net event loop under M5 and the HVF notes under
  M4.
- Multi-guest multiplexing and session authentication, snapshot and restore, and
  single-file packaging were earlier cross-cutting notes; they are now the
  forward milestones M10, M12, M11, and M13 above.
