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

## M4: preemption and SMP (done)

Goal: multiple cores, preemptive scheduling, and threads.

- A para-virtual timer event.
- Event injection added to the backends. This is where guest polling stops
  being enough.
- A second vcpu, the scheduler across vcpus, threads, and a cross-vcpu wakeup
  event.

Acceptance: two CPU-bound tasks are preempted and balanced across two vcpus, and
a thread test passes.

Spike decision (made): to preempt a running, possibly spinning, guest, a host
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

## M5: networking (in progress)

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
observability, not for byte transfer. Scope is outbound TCP connect and UDP
datagrams; listen/accept is deferred.

The host provider opens real host sockets (that is the NAT) and keeps them
non-blocking. A host poll thread is event-driven, not periodic: it blocks in
`epoll_wait` with no timeout and wakes only when a socket actually transitions.
An fd is in the epoll set only while a guest task is waiting on it: the net
doorbell handler arms it (one-shot) the moment a non-blocking `recv` or
`connect` returns `EAGAIN`/`EINPROGRESS`, and readiness disarms it until the
next would-block. On readiness the thread injects the event vector into running
vcpus and wakes any idle ones. An idle connection has no registered fd, so
there is nothing to spin on. A single `eventfd` in the set breaks the wait at
shutdown. (`epoll` is Linux-only; the macOS/HVF path uses the same arm-on-
`EAGAIN`, one-shot model over `kqueue`, implemented when that backend is
validated on hardware.)

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

- M5.1: the scheduler sleep/wake primitive. A `TASK_BLOCKED` state, `task_block`
  and `task_wake`, a quiescence check that does not shut down while tasks are
  blocked, and a re-ready pass when an idle cpu is woken. Self-tested in the
  guest with no sockets.
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
- M5.4: the `/net` information tree over 9P, the manifest network-policy fields
  and their enforcement, and the allowed/refused acceptance test.

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
