# vPCIe User Provider Implementation Plan

## Goal

Deliver the first real vPCIe user providers: a modern, single-queue
`virtio-blk-pci` function backed directly by a raw image and a TAP-backed
`virtio-net-pci` function. Their source lives in `dfvmm/sbin/virtiod`;
deployments may install the resulting binary as `/sbin/virtiod`.

The result is not a QEMU VM and does not import QEMU's PCI device model.  The
first user program owns one vPCIe `provider` session and implements the
minimal modern virtio-pci frontend and raw I/O backend.  QSD is a later
optional block backend behind the same frontend.

## Current Implementation

P8.1 is implemented as `sbin/virtiod` in the dfvmm tree. It opens one
`provider` session, waits for `START`, registers one modern `virtio-blk-pci`
function, maps the generation-scoped BAR and DMA capabilities, and serves a
regular raw image. For `blk`, `queues=N` is static configuration: one provider worker
owns each virtqueue and its vring state, while the device thread keeps sole
ownership of the provider socket and trapped control path. It exposes
`VERSION_1`, read/write/flush, a 64-bit BAR, one config MSI-X vector and one
MSI-X vector per queue. Common configuration, ISR and device configuration
are synchronous trapped BAR pages; notify is a `DOORBELL` BAR range. A guest
kick causes a bounded VMEXIT that coalesces into a private PCI-function event
fd, which `virtiod` reads through kqueue. The pc64
gate boots Alpine with `queues=2` and completes a DMA-backed raw-image read.

The QSD/vhost-user material below is a later optional backend design.  It does
not describe the P8.1 runtime path and does not gate raw-image Linux bring-up.

`virtiod net DEVICE_DIR TAP MAC` is implemented using a BSD-2-Clause port of
the relevant bhyve TAP and virtio-net queue behavior. It provides RX queue 0,
TX queue 1, config/RX/TX MSI-X vectors, a fixed locally administered MAC and
link-up status. It advertises only `VIRTIO_F_VERSION_1`, `VIRTIO_NET_F_MAC`
and `VIRTIO_NET_F_STATUS`; no checksum offload, GSO, mergeable buffers,
control queue or multiqueue is present. TAP is opened as `/dev/TAP` in
nonblocking mode. Address assignment, bridge membership and host routing are
not provider behavior and remain administrator policy. The pc64 isolated-TAP
Linux gate is complete: it boots Alpine, verifies the guest MAC and 3/3 ICMP
replies to the host TAP address, then verifies provider stop, unmount and module
unload. This confirms the fixed one-RX/one-TX queue scope, not offload or
multiqueue support.

## Current Implementation Boundary

P0 has frozen and validated the fixed provider packet ABI.  P1 created the
core per-function relation: stable name/ID, host or machine consumer root, and
consumer-local BDF allocation.  P2 exposes permanent provider/consumer
session factories and its power lifecycle: `open(provider)` retains a pending
provider session in the current root's device set, and each
`START(generation)` drives `REGISTER` and the runtime resource handoff.
Provider loss is surprise removal and revokes resources; retained
`OBJT_MGTDEVICE` objects make `vmm.ko` unload return `EBUSY` until their final
mapping/fd reference disappears.  A consumer session temporarily offloads the
function and returns it to its original root on close.

P3 makes a registered provider discoverable through the guest's standard ACPI
MCFG/ECAM path.  The one-segment aperture covers buses `00..ff` at
`0xe0000000..0xefffffff`; the current allocator places the first function at
`0000:00:01.0 [1b36:df01]`.  The pc64 P3 gate holds that provider pending
until `START`, then confirms Linux enumerates it through MCFG after the
generation-matched registration.  P4 adds direct BAR mapping after the guest
sets `PCI_COMMAND.MEMORY`; its pc64 gate verifies shared provider/guest BAR
reads and writes after the same lifecycle handoff.  P5 adds standard MSI-X
table validation and one-vCPU AVIC injection after a provider control
message; its pc64 gate verifies the same START-driven lifecycle before Linux
receives the interrupt.  P7 provides the revocable CPU-side DMA capability
with normal-stop, reset, and provider-loss gates complete.  The first QSD
provider must extend that coverage to descendant mappings.

```text
Linux virtio-blk driver
        |
        | ECAM, BAR, MSI-X, guest memory
        v
vmm-user-virtio-blk            vPCIe provider
        |
        | vhost-user protocol, memory region fd, vring eventfd
        v
qemu-storage-daemon            block service
        |
        v
raw/qcow2 block graph
```

QSD supports `vhost-user-blk` exports over a Unix socket or a supplied fd and
can run one or more request queues.  The first implementation deliberately
uses one queue because vmm currently supports one vCPU.

## Why vhost-user-blk, Not NBD

QSD can also export NBD.  NBD is useful as a diagnostic fallback but is not
the primary path:

- An NBD adapter would have to parse virtio descriptors and copy each request
  through a socket itself.
- A QSD `vhost-user-blk` export already consumes virtqueues and receives
  memory-region fds through the vhost-user protocol.
- The vhost-user path therefore preserves the intended zero-copy guest-memory
  access once the vmm DMA capability exists.

`vmm-user-virtio-blk` is the vhost-user frontend/client.  QSD is the
vhost-user-blk server and remains a block service, not an independent vPCIe
provider or a second PCIe endpoint.

## Fixed Scope

The first provider supports:

- one PCIe segment, one root, one function and a static configured set of
  virtqueues;
- modern virtio-pci only, with `VIRTIO_F_VERSION_1`;
- PCIe MSI-X only; no INTx or legacy virtio I/O ports;
- block read, write, flush, and basic identification/configuration;
- a raw image first, then qcow2 once the data path is stable;
- an initramfs Linux smoke guest first, then a disk-root Linux guest.

The same `virtiod` process also hosts the separate `virtio-vsock-pci`
provider used by runv and Kata. Its flow transport is not a block extension:
it has independent RX, TX and event virtqueues, a configured guest CID, and a
private Unix stream broker defined in `sys/sys/vmm_vsock_abi.h`.

It does not initially claim multi-vCPU parallel-I/O stress coverage, packed
virtqueues, transitional virtio, live migration, dirty logging, physical PCIe
offload, or host hotplug.

## Compatibility Constraints Beyond the First Virtio Provider

The first provider is deliberately `virtio-blk-pci`, but it is not the vPCIe
ABI definition.  The common provider/consumer records, capability fds, and
DMA descriptions must remain usable by the following important targets.

| Target | Intended role | Constraints imposed on this plan |
|---|---|---|
| `virtio-pci` | Linux VM consumer of the first user provider | Modern PCI capabilities, fixed 64-bit BARs, direct virtqueue notification, and MSI-X are sufficient for the first milestone. |
| NVMe PCIe | Linux VM consumer of a future user or physical provider | Do not make BAR handling virtio-specific.  It needs a standard controller BAR, reset-visible register state, admin plus I/O queues, PRP/SGL DMA, and per-queue MSI-X. |
| DPDK | Guest or host user-space PCI consumer | Do not require a host kernel driver/data path.  A later host-consumer adapter must expose normal PCI resources, BAR mmap, MSI-X, and explicit IOVA DMA mappings. |
| SPDK | Guest or host user-space PCI consumer; a possible storage service behind a separate provider | Preserve direct BAR access, polling, standard NVMe queue semantics, and pinned IOVA-backed DMA.  SPDK does not justify sharing one provider with multiple consumers. |

The QSD path is only a user-provider implementation choice.  QSD's
`vhost-user-blk` server is not a model for a host PCI consumer and does not
replace the future standard NVMe provider or the DPDK/SPDK adapters.

### Multi-role process rule

One active provider and one active consumer is a per-function invariant, not
a per-process restriction.  A process may open a `consumer` session for an
upstream function and register provider sessions for several distinct child
functions.  This is the software-DPU/software-SR-IOV composition.  Each child
must retain independent BDF assignment, BAR objects, MSI-X vectors, reset
generation, and DMA authority; the relation is explicit in vmm core state,
not inferred from PID or vmmfs placement.  The first QSD provider does not use
this composition, but its ABI must not prevent it.

## Required vPCIe Surface

### Provider registration

The provider process creates and opens the device directly under the VM that
initially owns it:

```sh
mkdir /dev/vmm/<vm>/devices/vblk0
```

`provider` is a permanent socket factory.  The provider opens it directly;
`touch` would only attach and immediately detach.  `open(provider)` returns a
private `SOCK_SEQPACKET` session and adds the device to its root's ownership
set, but grants no runtime mapping.  When that root runs, vmm sends
`START(generation)`.  The provider answers with a versioned
`REGISTER(generation, static-profile)` declaring:

- a modern virtio-block PCI ID and class;
- one fixed 64-bit memory BAR containing virtio PCI capabilities;
- one MSI-X capability and its vector limit;
- a direct doorbell page and its shared kick-sequence layout;
- the requirement for a VM DMA aperture during each VM run generation.

On success, `recvmsg()` returns `REGISTERED(generation)` with:

- BAR backing fd(s) and the DMA capability fd, via `SCM_RIGHTS`;
- provider lifecycle/control messages on the same socket.

The session is an fd capability, not a PID lease.  `fork()` inherits the
provider fd, and `SCM_RIGHTS` can transfer it to another process.  The
function remains attached until the final session-fd reference closes; sharing
one session between processes requires the provider to serialize ownership of
its control-record stream.  A BAR fd received in `REGISTERED` has a separate
capability lifetime.

The detailed C record layouts are in `sys/vmm/vmm_pcie_abi.h`.  They are
fixed-size, versioned, little-endian, and validated without loading `vmm.ko`
by `test/vmm/pcie/vmm_pcie_abi_check.sh`.  DMA records describe a memory
generation, permissions, lengths, and segments.  GPA is the address used by
guest virtio/NVMe descriptors; it is not
an IOVA or host physical address.  An IOVA field is optional for this
CPU-accessing QSD provider but mandatory in the common representation when a
physical provider or DPDK/SPDK host consumer needs an IOMMU mapping.

P8.0 is complete: ABI version 6 carries up to four generic PCI vendor-
capability descriptors in `REGISTER`, packed into the remaining conventional
config header after MSI-X.  The core builds and owns the read-only PCI
capability chain; the provider supplies only payload bytes.  The virtio
provider uses this transport for its common, notify, ISR and device
configuration capabilities.  It does not make `vmm_pcie` aware of virtio.

### BAR and virtio-pci layout

`vmm-user-virtio-blk` maps the BAR fd with `MAP_SHARED`.  The guest sees the
same pages through NPT after PCI BAR assignment.  The provider fills and
maintains the virtio PCI common configuration, notification, ISR, and
device-specific regions in those pages.

For the current ABI version 6, BAR 0 retains the vmm-reserved MSI-X prefix
introduced by version 3: the
16-byte-per-vector table followed by an 8-byte-aligned PBA.  Provider payload
must begin at `VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(vector_count)`, not at BAR offset
zero.  The provider owns PBA/pending/reassertion policy because direct guest
MSI-X table writes do not exit to the VMM.  On completion it updates its
BAR-visible ISR/PBA state first, then sends `MSI_X`; vmm validates the current
table entry and injects only a valid unmasked vector.

The vPCIe core owns PCI configuration space, BAR placement, MSI-X capability
validation, and range dispatch. The user provider owns virtio-specific state:
each register access to a trapped page has a checked synchronous RPC, while
direct pages remain shared memory. This prevents device-specific virtio logic
from leaking into `vmm_pcie` or `vmmfs`.

The first feature set is intentionally narrow.  It must be sufficient for a
current Linux `virtio_blk` driver, but every advertised feature must be fully
implemented.  Do not advertise optional virtio features as placeholders.

### Doorbells and calls

The normal virtio queue notification is a function doorbell:

```text
guest publishes descriptors
guest writes the notify BAR range
vmm traps and coalesces the write into the PCI-function event capability
provider kqueue drains the sequence and processes all configured virtqueues
```

This is the default data path. It causes one bounded VMEXIT per coalesced
pending epoch, but does not make the vCPU wait for provider I/O and carries no
per-queue request/reply protocol. The event capability is function-level, so
the provider checks all of its virtqueues after every drain.

QSD returns completion notifications through its vhost-user call eventfd.
The provider receives the event, updates virtio ISR/MSI-X state in the BAR,
and writes an MSI-X record to the same function event fd. The vmm core
validates it and injects through AVIC.

The event fd is bidirectional: read drains guest doorbell state; write injects
MSI-X. It replaces KVM's separate ioeventfd and irqfd objects without a new
global syscall ABI.

## DMA Capability: Blocking Prerequisite

Virtio descriptors contain guest physical addresses.  QSD's vhost-user memory
table needs file descriptors that map those guest pages.  This requires a
vmm-owned DMA capability, not loader fd 3.

At VM start, vmm first sends `START(generation)`.  After the provider returns
the matching `REGISTER`, vmm sends `REGISTERED(generation)` with the per-run
DMA aperture, BAR fds, and the function event fd through `SCM_RIGHTS`:

```text
REGISTERED { memory_generation } + bar_fd[] + dma_fd + event_fd
```

The fd maps authorized GPA offsets with `MAP_SHARED`; it is the only source
from which the provider may construct QSD vhost-user memory regions.  It
provides zero-copy CPU access to the same pages mapped into guest NPT.  Its
metadata describes the valid segments and generation, so the provider cannot
mistake a sparse/future IOVA layout for one contiguous guest address range.

The provider passes appropriately bounded regions of this capability to QSD
with the standard vhost-user `SET_MEM_TABLE` exchange.  QSD is a delegated
block service inside the provider implementation, not an additional vPCIe
attachment.  Its access must cease with the provider's DMA generation.

The P7 capability contract uses DragonFly's existing `OBJT_MGTDEVICE` pager
behavior.  The capability publishes its revoked state under the object token,
then calls `vm_object_page_remove(object, 0, 0, FALSE)` to remove installed
pmap entries found through the object's `backing_list`; dfvmm adds no VM-core
API:

1. Stop/reset marks the current DMA capability revoked.
2. New mmap attempts fail after revocation.
3. Existing provider and QSD mappings are removed or fault as ordinary
   user-space failures.
4. Old mappings cannot access the next runtime guest-memory generation.
5. `kldunload` remains `EBUSY` while any capability mapping retains module
   code.

The kernel gives each provider its own fd for a run.  Provider socket loss
revokes that fd and all of its inherited mappings without revoking the DMA
authority of sibling functions.  While active, the fd pager holds the runtime
COW vmspace, forces write faulting for its returned GPA page, and then returns
that same page zero-copy.  Revoke removes installed pmap entries and releases
that vmspace; any retained mapping then holds only the inert capability object
until it faults or closes.  It never maps loader fd 3 or the boot snapshot
directly.

Runtime P7 validation uses the existing boot-kernel VM API and needs no shared
kernel update or reboot.  Retained capability objects still veto `kldunload`;
that residual symbol lifetime is an accepted `OBJT_MGTDEVICE` boundary.

An IOMMU is not required for this user-process provider: QSD accesses memory
with the host CPU through the explicit DMA mmap capability.  IOMMU mapping is
mandatory only when a physical provider bus-masters guest pages.

## Lifecycle Contract

The provider process and QSD form one provider implementation.  vmm must not
depend on either process voluntarily exiting to recover guest memory.

### Start

1. Machine worker prepares runtime guest memory and sends `START` to every
   current provider session without waiting for it.
2. The provider answers `REGISTER` for that generation; vmm returns
   `REGISTERED` with BAR and DMA fds.
3. `vmm-user-virtio-blk` establishes or resets its vhost-user connection to
   QSD, sends the memory table, configures the one virtqueue, and installs
   kick/call eventfds.
4. The provider can use the resources and sends MSI-X only for that generation.

Provider initialization runs in parallel with guest execution.  Without guest
hotplug notification, a late provider may require guest rescan or next boot
for discovery; it does not block machine start.

### Stop and failure

1. Normal machine stop sends `STOP` to every provider still owned by the
   machine.
2. The provider disables vhost-user vrings, stops submitting I/O to QSD,
   closes that generation's mappings, and returns `STOPPED`.
3. vmm waits only a bounded grace interval, then revokes any remaining
   BAR/DMA resources before releasing runtime guest memory.

Provider final-close, socket loss, or move-out is surprise removal, not
normal stop: vmm immediately revokes that provider's generation without
sending `STOP` or waiting.  A concurrent backend access receives a catchable
mapping fault rather than process termination.

### Reset

Guest reset returns virtio state and BAR-visible device state to the registered
reset state.  It ends the old DMA generation, tears down the vhost-user memory
table, and performs the next start with a fresh DMA fd.  External reset may
also replace the provider configuration only through the normal stopped
configuration path; it does not reuse stale QSD mappings.

## Implementation Phases

### Phase 0: ABI and test assets

- Write the provider socket record header and parser tests.
- Define the fixed modern virtio-pci BAR layout and MSI-X vector contract.
- Add a user-space provider skeleton under `test/vmm/user/`; it is a test
  asset, never code under `sys/vmm`.
- Add a QSD launch helper in the same test area.  It creates an isolated
  `/var/tmp/dfvmm-*` Unix socket, QMP socket, image, and log.

Success criterion: the parser accepts the generation-matched
`START -> REGISTER -> REGISTERED` exchange, recognizes `STOPPED`, and all
failure paths close/revoke provider-side BAR capabilities cleanly.

### Phase 1: ECAM and BAR-only smoke

- Single-root ECAM and the minimal standard configuration image are complete.
  The Linux loader emits MCFG/`PNP0A08` for buses `00..ff` and reserves the
  256 MiB aperture in e820; the pc64 Linux harness enumerated a registered
  provider through ECAM.
- The START-driven BAR delivery path is complete as data-path evidence.
- A BAR-only provider publishes a small read/write test register block.
- Linux booted from initramfs, enabled `PCI_COMMAND.MEMORY` through config
  space, mapped the assigned BAR, and verified provider/guest shared state.

Success criterion: Linux sees the device, maps its BAR, and provider/guest
observe shared BAR state without a kernel MMIO forwarding loop.

### Phase 2: Revocable DMA aperture

- Use the existing `OBJT_MGTDEVICE` capability pager: publish revoked state
  under its object token and call `vm_object_page_remove()` to retract
  installed mappings, leaving later access to fault normally.  Do not add a
  dfvmm-specific VM-core invalidation primitive.
- Add per-run `vmm_dma` generation managers and per-provider DMA capability
  objects on top of that capability-object lifecycle.
- The pc64 P7 gates cover normal stop, reset, and provider loss.  They retain
  revoked BAR and DMA mappings, confirm they fault, and require `kldunload`
  to remain `EBUSY` until the relevant mapping holder exits.  QSD crash with
  retained descendant mappings is a P8 gate.

Success criterion: all stale DMA mappings fail after stop/reset; no mapping
can reach a new memory generation; module unload reliably returns `EBUSY`
while a relevant mapping remains.

### Phase 3: vhost-user frontend and QSD

- Implement the vhost-user frontend subset required by QSD vhost-user-blk:
  ownership, feature negotiation, memory table, one vring, kick fd, call fd,
  and reset.
- Launch QSD with `--export type=vhost-user-blk` over a private Unix socket.
- Pass the DMA aperture regions to QSD through vhost-user fd passing.
- Convert QSD call notifications into MSI-X through the provider socket.
- Handle QSD disconnect and QMP-driven shutdown as provider failure.

Success criterion: Linux runs from initramfs, discovers `/dev/vda`, performs
read/write/flush I/O against a QSD raw image, and receives completion MSI-X.

### Phase 4: Disk-root and resilience

- Boot a small Linux rootfs from the QSD disk image.
- Verify ext4 create/write/fsync/reboot persistence, read-only mode, and
  qcow2 backing behavior.
- Run repeated start/stop/reset, provider restart, QSD restart, and forced
  teardown tests.
- Measure direct-doorbell CPU use and completion latency before selecting the
  adaptive poll policy.

Success criterion: a Linux disk-root guest remains correct across controlled
lifecycle transitions and fails visibly, rather than corrupting memory or
hanging, when its provider disappears.

### Phase 5: Standard NVMe and user-space consumer gates

- Add a separate modern NVMe user-provider plan.  It must use the same ECAM,
  BAR, DMA-window, and MSI-X ABI rather than copying virtio control state into
  vPCIe core.
- Add the host user-space consumer adapter for the same resource interface.
  Its API is a PCI resource adapter, not a second user-provider protocol.
- Validate a Linux guest NVMe driver before claiming NVMe compatibility.
- Validate DPDK and SPDK separately against the host-consumer adapter with
  BAR mapping and IOVA DMA.  A physical provider remains blocked on IOMMU
  group/isolation support.

Success criterion: virtio, NVMe, DPDK, and SPDK use the same provider/consumer
resource semantics, with no per-stack fast path or unrestricted memory access.

## Test Matrix

| Scenario | Required observation |
|---|---|
| Registration reject | No device reaches consumer PCI topology; error in machine events |
| Linux PCI enumeration | Correct modern virtio PCI IDs, BAR, MSI-X, no legacy I/O BAR |
| BAR sharing | Provider and guest observe the same values; no ordinary MMIO forwarding |
| Direct doorbell | Queue progress without guest VMEXIT attributable to doorbell writes |
| QSD I/O | Read, write, flush, and block-size/config values are correct |
| Provider loss | Device failure event; no stale DMA authority |
| QSD loss | Provider failure event; no guest-memory leak or use-after-free |
| Stop/reset | Old DMA mappings fault; next run uses a distinct generation |
| Module unload | `EBUSY` while live provider/DMA capabilities remain |
| NVMe ABI regression | A future NVMe provider can register controller BARs, multiple queue doorbells, and MSI-X without changing the common records |
| DPDK/SPDK ABI regression | A host consumer can receive BAR and IOVA DMA capability descriptions without a virtio-specific adapter |

## References

- QEMU Storage Daemon: <https://www.qemu.org/docs/master/tools/qemu-storage-daemon.html>
- QEMU vhost-user protocol: <https://www.qemu.org/docs/master/interop/vhost-user.html>
- QEMU vhost-user backend overview: <https://www.qemu.org/docs/master/system/devices/virtio/vhost-user.html>
- DPDK Linux drivers and VFIO: <https://doc.dpdk.org/guides-25.03/linux_gsg/linux_drivers.html>
- SPDK user-space PCI drivers: <https://spdk.io/doc/userspace.html>
- SPDK DMA memory: <https://spdk.io/doc/memory.html>
