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

## M2: guest library OS bring-up (in progress)

Goal: boot the JOS-derived library OS as a guest. This is the longest
milestone.

Progress: the x86-64 64-bit long-mode switch is done. The KVM backend builds an
identity-mapped page table in guest RAM and enters the vcpu in long mode, the
guests build 64-bit, and the `-m32` path is retired. The libOS skeleton is up:
`guest/libos/` boots to C with no asm stub (the host hands the vcpu a stack),
has an arch seam (`arch.h` with a console byte sink and halt, implemented per
arch under `arch/`), and carries the JOS-derived first-fit allocator. `make`
builds `libos.elf`; it boots, runs a heap self-test, prints `libos up` /
`heap ok`, and halts. The cooperative round-robin scheduler is up too: a task
is a function on a `kmalloc`'d stack, with greenfield 64-bit context switches
(`arch/<arch>_ctx.S` plus a `stack_init` per arch) and a scheduler that
ping-pongs with the boot context. `task_spawn`, `task_yield`, `task_exit`, and
a `task_dump` for observability are in `task.c`. The demo spawns two tasks that
interleave and exit, dumps the table before and after, then halts. Next:
spawn/exec/await and notes as signals (preemption waits for M4).

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
