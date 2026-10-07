# Jotun: a hypervisor-hosted unikernel sandbox

Status: design. Nothing here is implemented yet. The current repository is the
small hypervisor abstraction (KVM and Hypervisor.framework) that this design
builds on.

## 1. Goal

Run a single application, linked against a small library OS, as a sandboxed
guest on the native hypervisor (KVM on Linux, Hypervisor.framework on macOS).
The guest gets substantial isolation from the host because the virtual machine
boundary is the security boundary. The application can spawn a few
subprocesses like a Unix shell and can run threads across one or more
processors with preemptive scheduling. The host provides filesystem access
over 9P, limited networking controlled like a container, and a display channel
that reaches the user through a browser or a standalone client.

The target workloads are non-realtime: data visualizations, interactive
diagrams, and schematics, the sort of thing that works well over a remote
connection. That shapes the display design toward a vector-first protocol, with
pixels used only where raster content is unavoidable, rather than a
high-framerate GPU pipeline.

This is closer to "a tiny Plan 9 style Unix running as one VM" than to a
classic single-task unikernel. The multi-process model is wanted, not avoided.

## 2. Naming

The project is named **Jotun**. The two roles are referred to plainly:

- the **host**: the trusted monitor. It owns the hypervisor, the service
  backends, and the display front end.
- the **guest**: one sandbox, the library OS plus the linked application.

No further code names for the roles; "host" and "guest" are used throughout.

## 3. Trust and isolation model

- The untrusted thing is the guest. The trusted thing is the host.
- The VM is the entire security boundary. Everything inside one guest is a
  single trust domain.
- The guest runs with no MMU. Processes inside a guest share one flat address
  space and have identical full access and permissions. Separate processes are
  a portability and programming convenience, not a security mechanism. A bug in
  one process can corrupt another, and that is acceptable because they are the
  same application's trust domain.
- Because of this, there is no in-guest capability system, no user and
  supervisor split, and no page tables used for isolation. All of that
  machinery would only earn its keep if the guest had to isolate mutually
  distrusting parties, which it does not. (x86-64 long mode requires a trivial
  identity-mapped page table, which performs no translation; arm64 runs with
  the MMU off. Guests are 64-bit; 32-bit guests are not supported.)
- If mutually distrusting tasks are ever needed, the answer is a second guest,
  not an MMU inside one guest. The arch layer should be written so stage-1
  paging could be added later, but it is out of scope.

## 4. Host and guest roles

The host is a single trusted process that is, at once:

- the hypervisor driver (the existing `vmm_kvm.c` and `vmm_hvf.c` backends),
- the 9P filesystem server, reading from the guest's bundle (a smolvfs
  content-addressed store, section 7.1),
- the `/net` policy engine,
- the display front end: it terminates the browser transports, composites the
  guest's 2D vector scene and any pixel regions, and delivers them to the
  client.

The guest is the library OS (derived from JOS, see section 6) linked with one
application image.

## 5. Transport ABI

This is the contract everything else rides on, so it is defined first.

- **Shared-memory rings** in guest RAM carry descriptors. The guest fills a
  batch and rings a doorbell once, which is a single VM exit per batch, not per
  access. This is the key difference from the current per-byte MMIO console,
  which is fine for a log but far too many exits for a data path.
- **One doorbell**: a single MMIO page the guest writes to signal the host.
  That write is the VM exit.
- **Xen style event channels** are the only notification primitive in the other
  direction. A shared structure holds per-channel pending and mask bits. The
  host sets a channel pending and injects the guest's event interrupt; the
  guest reads the pending set, clears bits, and dispatches. One mechanism serves
  the timer tick, cross-processor wakeups, and every device completion (9P,
  net, display input).
- Do not emulate a GIC, an APIC, or virtio-pci. The host owns both ends, so
  the event-channel model replaces interrupt-controller emulation and collapses
  a large amount of per-architecture work. The same ABI is used on x86-64 and
  arm64; only the final interrupt injection differs per backend.
- **Bulk data** (framebuffer regions, large file reads) moves through separate
  shared-memory regions passed by reference, not streamed through the ring
  messages. 9P requests are pipelined with many outstanding tags rather than
  one in flight.

## 6. Guest library OS (derived from JOS)

JOS (jon-os) is a V6 Unix and Plan 9 flavored OS for MMU-less
machines, with 9P2000 as its native filesystem protocol. Its I/O and namespace
layer is the reusable core. Its 64-bit and hypervisor ports do not exist yet
and would be greenfield.

Keep:

- the 9P client, used to talk to the host's 9P server,
- the per-process namespace with `bind` and `mount`, deep-copied at spawn,
  and lexical `..` resolution that cannot escape a mount,
- the device table, the flat first-fit allocator, and the position-independent
  ELF loader,
- `spawn`, `exec`, `await`, `reap`, notes as signals, and string exit status,
- the arch transport seam (`archp9rpc` and friends), where the ring transport
  slots in.

Add:

- SMP. Each virtual CPU is a host thread looping on `vmm_vcpu_run`. The guest
  scheduler places tasks on virtual CPUs.
- Real threads.
- Preemption driven by a para-virtual timer event, with a cross-processor event
  for wakeups and reschedule. JOS has neither SMP nor threads today.

Drop:

- the user and supervisor privilege split, including the drop-to-user-mode path
  and the user-stack-pointer juggling. In one trust domain this is dead weight.
  Every task runs at the guest's single privilege level, EL1 on arm64 or ring 0
  on x86-64, with its own stack. The timer still preempts; there is simply no
  ring to cross.

## 7. Services, namespace, and capability model

The model is "the namespace is the authority," refined with three ideas from
BRUTAL. BRUTAL is a full capability microkernel; its
in-kernel capability machinery (per-task handle tables, cross-domain handle
re-stamping, passable address spaces and tasks, rights delegation to children)
is dropped, because it only pays off when isolating mutually distrusting
servers. What is kept is its external service model:

1. **Boot-time service grant.** A per-guest manifest, read by the host at
   launch, declares which filesystem subtree, which network ports, and whether
   display access the guest gets. This is the container-style policy surface.
   It decides which service classes exist at all; the namespace decides which
   files within them are visible.
2. **Typed, sequence-matched request and response stubs** over the ring, the
   small stable contract between guest and host. This can be generated from an
   IDL later, or hand-written at first.
3. **Shared memory passed by reference** for bulk and for the display
   framebuffer. A pixel surface is a shared region plus geometry; file bulk
   rides the same path.

Net capability model: a fixed, boot-granted set of typed service endpoints plus
shared-memory regions. No dynamic capabilities, no revocation, the host as
the sole authority.

Services appear in the guest namespace, Plan 9 style:

- filesystem: the host's 9P server, rooted at the granted subtree,
- networking: a `/net` tree (section 8),
- display: a `/dev/draw` endpoint the guest writes a command stream to.

### 7.1 Guest parameters and the filesystem bundle

A guest launch takes two host-supplied inputs beyond the guest kernel image: a
set of parameters and a filesystem bundle.

Parameters reach the guest through a boot-info block the host writes into guest
RAM at a fixed low address before the vcpu starts. The library OS reads it at
boot. It carries the essentials the guest cannot discover on its own: the guest
RAM size, the name of the init program to run, and a packed argv and env blob
for it. The ring and doorbell layout is already fixed by the ABI, so it need
not be repeated here. Richer or late-bound configuration is exposed as files in
the namespace, Plan 9 style, for example an `/env` or `/cfg` tree, rather than
growing the block.

The filesystem bundle is a single read-only image the host serves to the guest
over 9P. It packages everything the guest domain runs together: the init
program, a shell interpreter, the additional small programs that init or the
shell spawn, and their config and data files. One guest domain, one bundle, so
the whole domain is a single immutable artifact that is easy to distribute,
pin, and reason about. Because exec resolves a program by name (section 6), the
bundle is what turns that name into an ELF image. The loader reads the file
from the 9P namespace and loads it, so several programs cooperate inside one
guest the way processes do on a normal system, while still sharing the single
trust domain.

The host owns the bundle; the guest only issues 9P reads, so no image-format
code runs in the guest and the host validates every path. The bundle is a
smolvfs store, a dependency-free content-addressable store
with a tree layer and a packfile:

- **CAS.** Objects are addressed by their BLAKE2b-256 hash, written atomically,
  and read by mmap. Identical content deduplicates automatically, across files
  and across bundles, with no separate dedup pass.
- **cas-tree.** Directories form a Merkle tree of tree objects with named refs
  and an append-only snapshot log. Structural sharing makes a one-file change
  cost O(depth) new objects, so snapshots and branches are cheap.
- **cas-pack.** Many objects roll into a single packfile with a
  binary-searchable index. That packfile is the distributable bundle.

This maps onto 9P almost directly. The host resolves a ref to a root tree hash;
a Twalk is then a chain of `cas_tree_lookup` calls, a Tstat is a tree entry,
and a Tread opens the blob with `cas_open` and serves the mmap'd bytes, which
also feed the shared-memory bulk path of the ABI. Because exec resolves a
program by name (section 6), the loader reads that program's blob the same way,
so several programs cooperate inside one guest the way processes do on a normal
system, while still sharing the single trust domain.

Content addressing gives integrity for free: every object verifies against its
own hash, so neither the transport nor the host's cache need be trusted for the
object bytes. What it does not establish is which root hash is authentic; that
is the ref, and ref authenticity is the trust anchor. smolvfs signs refs with
`cas-sign`: the publisher signs a ref, and the client verifies the signature
against a trusted public key before using the root. Signing lands with the
pull-from-URL stage (roadmap M8), where a bundle first arrives from an origin
that is not already trusted; a local bundle until then carries a ref trusted
directly. Multi-arch stays out of scope.

Because the store is immutable and content addressed, several guests in one host
process can read the same depot at once without copying it (roadmap M10.4). The
depot's write lock is taken only when a ref is committed, never on a read, so
concurrent readers never block each other. A manifest ref is resolved as either
a ref name or a fixed 64-hex root tree hash. A ref name tracks whatever root its
owner last committed, so one guest can own the ref and advance it while another
reads the live tree. A root hash instead pins an immutable snapshot: a consumer
serving `ref <hash>` sees exactly that tree forever, even as the owner commits
new roots behind the same name. A pinned hash is caller-supplied rather than
resolved from a signed ref, so the host checks that it loads as a real tree and
refuses the mount otherwise. This shares a whole tree as a guest's root. A guest
can instead graft a shared tree at a top-level name inside its own tree, so it
sees both at once: the manifest line `mount <name> <bundle> <ref> [subtree]`
wraps the guest's backend so the shared tree appears as `/<name>`, with the two
trees' qids kept disjoint (roadmap M13). The shared `ref` resolves the same way,
so a pinned hash gives a stable shared tree.

Per-object compression is available (smolvfs ships a DEFLATE codec); the first
cut can leave objects uncompressed. Compression is per object, so a very large
file decompresses whole rather than by cluster, which does not matter for a
bundle of small programs and config. A large file can be chunked into a blob
list if it ever does.

The content-addressed design also gives a natural distribution path: a published
depot is a set of static files, and a client fetches a snapshot over plain HTTP,
downloading only the objects it lacks and verifying each against its hash
(smolvfs REEF and its HTTP profile). This is the docker or podman style pull
from a URL, and it is deferred to its own later milestone; the first cut uses a
local packfile.

The same bundle also carries the bootable unikernel ELF as one more object in
the tree, at the well-known path boot/kernel. The host lifts that image out to
boot the guest, then serves the rest as the filesystem, so one signed object set
packages the kernel and its filesystem together.

One image can span several CPU architectures. Image identity is a small text
object in the store, a jimage manifest, that names a root tree hash per arch;
the object's own hash is what gets distributed. The host selects the row for its
own arch, serves that root, and lifts boot/kernel from it. Because each arch
root is an independent tree in a content-addressed store, arch-neutral files and
whole identical subtrees are shared across arches, so a multi-arch image costs
little more than the arch-specific binaries, and migrating between arches
transfers only the objects that are missing. A ref may point at a jimage object
or at a plain root tree, and the jimage object can be pinned by hash for an
immutable snapshot. The format and the builder are in doc/jimage-format.md.

The manifest ties these together. One manifest, read by the host at launch,
names the bundle, the subtree within it the guest may see, the init program and
its parameters, and the coarse network and display rights. It is the full
description of a guest domain.

## 8. Networking

A `/net` provider in the host enforces container-style policy from the guest
manifest: which ports may be bound or dialed, NAT, and rate limits. The guest
cannot name anything outside what `/net` exposes, so there is no path to the
real network except through the host's mediation.

As built (M5), the data path is a socket-like client over a dedicated net
channel (`net_dial`/`net_read`/`net_write`/`net_close`, plus UDP
`net_sendto`/`net_recvfrom`); the host opens the real sockets, which is the
NAT. A would-block parks the calling task, not the vcpu, and an event-driven
epoll poll thread wakes it when the socket is ready. Policy is manifest
`net allow <cidr> <ports> [proto]` rules matched against the resolved address,
default-allow with no rules and default-deny with any. `/net` itself is a
read-only 9P tree (`/net/policy`, `/net/conn`) overlaid on the bundle, for
naming and observability rather than byte transfer.

A connection is named by a Plan 9 dial string, `network!host!port` (the network
is `tcp` or `udp`). The host runs one name service over the host field, so the
same address form reaches either a real host or another guest. Each guest has a
network name, assigned by the manifest `name` field. A name that belongs to a
co-running guest resolves to an in-host link; any other name resolves
externally through DNS. An in-host link is a socketpair: on a dial to a guest
the host makes a socketpair, keeps one end as the dialer's connection, and
queues the other on the target's accept backlog, so inter-guest traffic never
touches the real network and the host stays the only bridge. From there send,
recv, readiness, and close are the ordinary socket path, so external and
inter-guest connections differ only in how the name resolved.

A guest offers a service with `net_listen`/`net_accept`. `net_listen("tcp!*!
port")` registers the service in-host under the guest's own name, so a peer
reaches it by dialing `tcp!<thatguest>!port`; accept blocks the calling task
until a peer connects and returns an ordinary connection. The same listen also
binds a real external port when the manifest explicitly allows it, so the guest
can accept connections from outside the host as well. Binding is opt-in and
default-deny: with no rules a listen is in-host only, and a real bind needs a
`net allow` rule naming the port, since offering a port to the outside is more
sensitive than dialing out. A named guest that binds is reachable both ways at
once, and accept takes a pending in-host peer before a real inbound connection.

Dialing a co-running guest is a rendezvous, so bring-up order does not matter.
If the target has not listened yet, the dial reports a retryable status and the
dialing task parks the way a recv or accept does; a listen then wakes the parked
dials, which retry and connect. Every guest name is registered in the name
service before any guest runs, so a dial to a peer that boots later still
resolves in-host and waits for it, rather than resolving the name externally and
failing.

UDP uses the same name service. A datagram addressed to a co-running guest's
name is routed through the switch and never touches the real network; any other
name resolves externally. A guest makes itself reachable by binding a UDP port
under its name (`net_bind` with `udp!*!port`), which parallels `net_listen` for
TCP but needs no accept. A socket that sends before it binds is given an
ephemeral in-host source port, the way a kernel UDP socket auto-binds on its
first send, so the receiver can always reply. In-host delivery is a small
bounded per-endpoint queue drained by recvfrom and woken through the usual event
path; a datagram to an unbound name, or to an endpoint whose queue is full, is
dropped, as UDP is lossy.

Network policy gates a guest's outbound reach uniformly. A manifest rule, `net
allow <who> <ports> [proto]`, takes `<who>` as an IPv4 CIDR (matched against the
resolved address of an external target), a co-running guest's name, or `*` for
any target. With no rules a guest may reach anywhere; with rules the default
flips to deny, and an external dial, a by-name dial, or a by-name datagram must
match a rule. A CIDR rule authorises only external addresses and a name rule
only that one guest, so external and in-host targets cannot authorise each
other. The rules gate reach, not what a guest offers in-host, which keeps the
flat trust model; external binding stays separately opt-in and default-deny.
Rate limits are not yet implemented.

## 9. Display subsystem

The target workloads are non-realtime and remote-friendly: visualizations,
interactive diagrams, and schematics. The design is therefore vector-first.
Most content is a retained 2D scene the client renders and animates locally;
pixels cross the wire only for raster content that has no vector form.

It reuses the Quaoar display research
(Quaoar). Quaoar's three-hop pipeline
already splits where this design needs it: application, then a bridge, then the
browser. That maps onto guest, then host front end, then client. Quaoar uses
reliable stream transports (a local socket with netstrings framing, and a
WebSocket to the browser), not UDP.

### 9.1 Primary path: the 2D vector scene

The guest speaks the Quaoar line protocol over a `/dev/draw` endpoint on the
ring. This is the literal Plan 9 `/dev/draw` idea: write a drawing command
stream to a file; only the transport under Quaoar changes, from a local socket
to the ring. `libquaoar` is the template for the guest emitter.

The content is a retained SVG scene graph. The guest sends the scene once, then
small mutations. Interaction feedback such as hover, press, and drag is handled
by the client with declarative animation, so it never crosses the wire. Only
semantic events return (click, a normalized scroll value, an x-y value,
debounced text, paste). This fits both the workload and the medium: diagrams
and schematics are vector-native, the data is bandwidth-light, and most
interaction never reaches the guest, which matters because every guest-to-host
crossing is a VM exit.

The command stream is small and textual, so the host validates it cheaply. The
SVG scene graph tops out around five to ten thousand nodes, which is ample for
diagrams and UI; denser raster content uses the pixel path below.

### 9.2 Secondary path: raster frames as pixels

For content with no vector form (a rendered 3D view, an image, a dense plot),
the guest produces pixels itself and the host simply delivers them.

- The guest renders into a framebuffer in its own RAM. A guest app that wants a
  3D API can link a software rasterizer for this; Triton
  (Triton), a subset of the 3Dfx Glide 3.0 API, is the one in tree
  (`guest/libos/triton.c`). It rasterizes gouraud-shaded, z-buffered triangles
  with perspective-correct texturing, the Glide color-combine modes (iterated,
  texture, modulate, add), point and bilinear filtering, mip-mapped trilinear
  sampling, anisotropic filtering, and source-over alpha blending, fronted by a
  vertex transform and near/far clip stage (a 4x4 matrix transform, homogeneous
  clipping, perspective divide and viewport map). The rasterizer runs entirely
  inside the guest.
- The framebuffer lives in a shared-memory region. The guest marks dirty
  rectangles and signals the host.
- The host validates the region geometry (width, height, stride, and bytes per
  pixel against the region size), composites the pixels as one node in the 2D
  scene, and encodes them for the client. Because the workload is non-realtime,
  dirty-rectangle updates and ordinary image compression are enough; there is
  no video-rate pipeline.
- The client blits the pixel node under the vector chrome, so it stays a simple
  display surface.

### 9.3 Why this split, on security grounds

The security boundary is the VM: the guest is attacker-controlled, the host is
trusted. The decisive question is where an adversarial command stream is parsed
and executed.

- No rendering command stream crosses into the host. The host never runs a
  rasterizer on guest-controlled vertices, textures, or framebuffer-write
  regions, so there is no rasterizer parser to exploit in the trusted process.
- The only display inputs the host handles are the small textual 2D command
  stream and a pixel buffer of declared geometry. Both are cheap to validate.
- Rendering bugs stay where they can do no harm. A software-rasterizer bug is
  inside the guest, a single trust domain that can already corrupt itself; a
  client-side rendering bug is inside the browser's own sandbox.

This is why the earlier alternatives are rejected. Running Triton on the host
over a guest command stream would put an adversarial parser in the trusted
process. Serializing a WebGL or Glide command stream across the HV adds a large,
stateful protocol for no security gain, and the buffer and texture upload paths
are exactly the awkward part. Keeping the rasterizer in the guest and shipping
only pixels is both simpler and safer.

### 9.4 The host front end

The host terminates the browser-facing transports (WebSocket handshake and
framing, TLS or a reverse proxy). It multiplexes several guests onto one client
session using per-guest and per-surface ids. It composites each guest's 2D
scene and pixel regions and encodes them for the client. All transport
concerns, ordering, pacing, and any image encoding live here, never in the
guest, which only ever sees the reliable ring.

### 9.5 Tradeoff

There is no hardware 3D acceleration: raster content is software-rendered in the
guest. For non-realtime visualization, diagrams, and schematics this is an
acceptable price for the small, safe host surface. If hardware 3D ever becomes a
requirement, the right route is a narrowly scoped and separately hardened GPU
path (for example a virtio-gpu or VirGL style interface), which is its own
project and is out of scope here.

## 10. Staged roadmap

1. Define and build the transport ABI: rings, doorbell, Xen style event
   channels, host event injection on both backends.
2. Retarget the JOS 9P client and namespace onto the ring. The host runs a 9P
   server exposing a read-only tree plus a console.
3. Application is the JOS shell doing directory and file reads over the
   namespace. This exercises spawn when the shell runs a command.
4. Add the preemptive timer, then a second virtual CPU (SMP) and threads.
5. Add `/net` with container-style policy.
6. Add `/dev/draw` speaking the Quaoar 2D vector protocol, with the host as the
   browser front end. This covers the primary visualization and diagram cases.
7. Add the pixel path: a shared framebuffer region with dirty-rectangle updates,
   composited by the host as a scene node. A guest software rasterizer
   (optionally Triton for a Glide 3D API) is a guest-side library on top of it.

## 11. Open decisions

- Whether the guest-to-host stubs are generated from an IDL or hand-written.
- The pixel encoding for the host-to-client hop (for example per-tile PNG versus
  a light codec), which only affects the browser-facing side.

Decisions already made: the project is named Jotun; no MMU and a single trust
domain, with in-guest processes sharing full access as a convenience; Xen style
event channels rather than emulated interrupt controllers; no in-guest privilege
split; the display is vector-first (Quaoar 2D) with raster content rendered in
the guest and shipped to the host as pixels; no rendering command stream and no
rasterizer crosses into the host; no hardware 3D acceleration for now.

## 12. Sources

- JOS (jon-os): the library OS core, 9P client, namespace, scheduler, and arch
  seam.
- BRUTAL: the external service capability model, shared memory by reference,
  typed RPC.
- Triton (triton-gpu): a subset of the 3Dfx Glide 3.0 API with a software
  rasterizer, used as an optional guest-side 3D library that renders into a
  guest framebuffer.
- Quaoar (programmable-display-protocols): the retained SVG display protocol,
  behavior offloading, and the three-hop pipeline, the basis for the primary 2D
  vector path.
- smolvfs: the bundle format and store (section 7.1). A dependency-free
  content-addressable store (CAS, BLAKE2b-256, mmap reads), a Merkle tree layer
  with refs and snapshots (cas-tree), a packfile (cas-pack), optional ref
  signing (cas-sign), and a git-style HTTP pull protocol (REEF and its DOWNLOAD
  profile). Dual licensed MIT / BSD-2-Clause-Patent. Its sources are vendored
  in-tree under `third_party/smolvfs`.
