# Jotun development roadmap

This expands the staged roadmap in `unikernel-design.md` into concrete
milestones. Each milestone ends in something runnable, and each names the real
decision or risk it carries. See the design doc for the architecture these
build toward.

## Where we start

The repository today is a working smoke-test VMM: one vcpu, one memory region,
a UART, an ELF loader, and a run loop, on KVM (x86-64 verified) and
Hypervisor.framework (arm64, verified by inspection only). The milestones below
turn that into Jotun.

The critical path is M1 (the transport ABI) then M2 (the guest library OS).
Everything after rides the ABI. The x86-64 guest execution mode is decided:
64-bit long mode with a trivial identity map. See M2.

## M0: host core refactor

Goal: turn the smoke test into a reusable host runtime.

- Split the tree into `host/` and `guest/`.
- Separate the generic VMM core (vcpu and memory lifecycle) from device and
  service code.
- Support multiple memory slots and N vcpus, each on its own host thread.

Acceptance: a test guest runs on two vcpus, both report exits cleanly, and
multiple mapped regions work.

Decision: do the directory reorganization now, before more code lands on the
current flat layout.

## M1: transport ABI

Goal: the single contract everything else rides on.

- Define `abi/jotun_abi.h` with the ring descriptors, the doorbell MMIO layout,
  and the event-channel pending and mask bitmaps. It is shared verbatim by host
  and guest.
- Host: the doorbell MMIO handler, ring draining, and event-channel writes.
- Guest: a small ring library.

Acceptance: an echo service over the ring (the guest sends bytes, the host
replies), driven by one doorbell exit per batch.

Decision: start with guest-polled event channels. The guest checks the pending
bitmap at safe points, which avoids interrupt injection entirely. Real
asynchronous injection is deferred to M4, where preemption needs it.

## M2: guest library OS bring-up

Goal: boot the JOS-derived library OS as a guest. This is the longest
milestone.

- Import the JOS core: allocator, ELF loader, proc, vfs, 9p, device table.
- Write the greenfield x86-64 and arm64 arch ports: boot to C, context switch,
  exception entry, console. No privilege split.
- Round-robin scheduler on a single vcpu.
- `spawn`, `exec`, `await`, notes as signals, string exit status.

Acceptance: the library OS boots, runs an init task that prints to the console,
and halts.

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

## M3: 9P over the ring

Goal: the guest sees a host filesystem.

- Guest 9P client on the `archp9rpc` seam, routed to the ring.
- Host 9P2000.u server exposing a read-only subtree.
- Guest namespace `bind` and `mount`.
- Introduce the per-guest manifest here; it grants the filesystem subtree.

Acceptance: the `jsh` shell in the guest does directory and file reads over the
mounted host tree and spawns a command.

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

## Cross-cutting concerns

- The per-guest manifest is the sandbox's control surface. It appears at M3 for
  the filesystem and grows through M5 for networking and M6 for display.
- Validation of every guest-to-host message is a standing requirement, not a
  phase.
- Multiplexing several guests onto one browser session, and any session
  authentication, land with or after M6.
- Snapshot and restore is optional and comes later.
