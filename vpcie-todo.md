# vPCIe Implementation TODO

## Scope And Entry Rule

This is the implementation plan for dfvmm phase 2: the vPCIe and I/O model.
Every vPCIe task starts by reading this file, `vpcie.md`, and
`vpcie-user-provider-plan.md`.

The implementation order is provider-first: standard discovery, direct BARs,
MSI-X, then the revocable CPU-side DMA capability needed by a modern
`virtio-blk-pci` provider.  Host consumer/offload work is deferred until its
IOMMU safety boundary exists.  Virtio is a validation vehicle for the common
vPCIe ABI, not a virtio-specific VMM device model.  The same ABI must remain
suitable for standard NVMe, DPDK, SPDK, software-DPU processes, and
software-SR-IOV-style child functions.

The former remote `vmm_device` and `vmmfs_device` BDF/owner stub was replaced
in P1.  Do not reintroduce a parallel vmmfs-owned relation or a second BDF
allocator.

## Invariants

- A PCI function has one active provider and one active consumer.  This is a
  per-function rule, not a per-process rule.
- One process may hold several provider and consumer sessions.  It may consume
  an upstream function and provide several isolated child functions: software
  DPU and software SR-IOV.
- Each child function has its own device identity, consumer-local BDF, BARs,
  MSI-X allocation, reset generation, and DMA authority.  Multiple providers
  never share one PCI function.
- `provider` and `consumer` are private `SOCK_SEQPACKET` session factories.
  `attachment` is the read-only current consumer/root snapshot.
- `mv` and provider-session attach/detach change a root's device-ownership
  set immediately, independent of machine desired/current state.  Start sends
  asynchronous `START` to every current member; normal machine stop sends
  `STOP` only to members that remain in the set.  Provider loss or move-out is
  surprise removal: revoke immediately, do not send `STOP`, and do not wait.
  No guest PCIe/ACPI hotplug notification exists yet.  Opening `consumer`
  offloads the current root; closing it returns the function to that original
  root.
- BAR and DMA are capability mappings. Queue notify traffic is direct shared
  mapping; common configuration, ISR and device configuration may use an
  explicitly declared synchronous trapped BAR page.
- ECAM is a low-rate control path and may VMEXIT.  Direct BAR accesses and
  doorbells are trapped and coalesced into one private event capability per
  PCI function; vCPU execution never waits for provider I/O.
- DMA is generation-scoped.  A user provider never receives loader fd 3 or
  unbounded guest memory.  A physical provider needs IOMMU isolation; no
  no-IOMMU fallback exists.
- No legacy PCI configuration ports, INTx, PIC, or QEMU compatibility model.

## P0: Freeze The Public Shape

**Status:** complete

- Update `vpcie.md` and `vpcie-user-provider-plan.md` with the multi-role
  process, software-DPU, software-SR-IOV, `consumer` socket, and `attachment`
  status semantics.
- Define a fixed-size, versioned, endian-defined public ABI header for
  `REGISTER`, `REGISTERED`, `START`, `STOP`, `STOPPED`, `MSI_X`, failure, BAR capability,
  and DMA segment records, including the one-segment BDF encoding.  No ioctl
  ABI.
- The common DMA record carries memory generation, permissions, length,
  segment boundaries, GPA, and optional IOVA.  GPA is never host PA.
- Add user-space parser/validation tests before kernel implementation:
  malformed length/version, BAR overlap/alignment, invalid MSI-X limits,
  invalid segment/permission records, and parent/child record pairing.

**Gate met:** `test/vmm/pcie/vmm_pcie_abi_check.sh` validates the fixed packet
layouts and negative cases without loading `vmm.ko`; the three design
documents use provider/consumer session factories and `attachment` consistently.

## P1: Replace The Device Stub With Core PCIe Objects

**Status:** complete

- `struct vmm_pcie` owns the device RB registry, immutable stable name/ID,
  attachment state, and the fabric root.  Every `struct vmm_machine` owns one
  `struct vmm_pcie_root`; the root allocates bus-0 device slots 1 through 31.
- P1 `struct vmm_device` covers the relation-only `NEW` state.  PCI identity,
  provider state, BAR definitions, MSI-X, lifecycle, and reset generation
  begin in P2/P3 rather than being represented by a fake partial object.
- vmmfs now owns only VFS views.  It creates an empty user-provider function
  directly below a machine's `devices/` directory and translates `mv` into a
  core machine-root move.  There is no host staging directory or global device
  symlink index.  A stable name is distinct from its BDF.
- The fixed host BDF pool and bind/unbind semantics are deleted.  The only
  P1 deletion/move path is an unregistered `NEW` function.

**Gate met:** `test/vmm/pcie/vmm_pcie_relation_check.sh` verifies independent
roots, duplicate-name rejection, BDF reuse, move, and exhaustion without the
module.  `test/vmm/pcie/vmm_pcie_fs_test.sh` on pc64 verifies direct guest
creation, machine-to-machine move, deletion, machine reaper, ordinary
unmount, and module unload without starting a guest.

## P2: Provider And Consumer Session Capabilities

**Status:** complete; P3-P5 guest data-path gates are complete

- Populate each existing vmmfs device directory with permanent `provider`,
  `consumer`, `state`, and `bdf` leaves.  `attachment`, PCI identity,
  `resource*`, and MSI/MSI-X leaves remain owned by later phases.
- Implement `open(provider)` and `open(consumer)` as private `AF_LOCAL`,
  `SOCK_SEQPACKET` endpoint creation through the VOP fileops-override path.
  Core retains the peer endpoint; the VFS path is only the factory.
- Register one provider per function.  One process may register several
  functions and may also hold consumer sessions for upstream functions.
- `open(provider)` retains a pending session without exposing a PCI function
  or resource fd.  `START(generation)` causes the provider to send
  `REGISTER(generation, static-profile)`; only `REGISTERED(generation)` sends
  runtime BAR/DMA capability fds with `SCM_RIGHTS`.
- Provider final-close detaches by surprise removal rather than marking it
  failed: revoke resources immediately and do not wait for an acknowledgement.

**Gate met:** `vmm_pcie_session_test.c` verifies that session ownership follows
the socket file reference across `fork()` and `SCM_RIGHTS`, not a PID.
`vmm_pcie_dma_test.sh` on pc64 verifies the live
`START -> REGISTER -> REGISTERED -> STOPPED` exchange.  Provider close after
the acknowledgement follows the surprise-removal path and leaves only revoked
capability mappings.

## P3: Standard PCI Discovery

**Status:** complete

- One PCIe segment, one root, and a 256 MiB ECAM aperture for buses `00..ff`
  are fixed at `0xe0000000..0xefffffff`.  `vmm_pcie_config` owns the 256-byte
  standard configuration header and BAR probe state under the fabric registry
  token; PCI IDs, BAR shape, and MSI-X capability are immutable after
  registration.  The current allocator still assigns functions only on bus 0.
- SVM dispatches ECAM nested-page faults before guest RAM faults.  The x86
  access decoder handles the compiler forms used by Linux configuration
  reads/writes; unregistered functions return all ones.
- The Linux kexec loader emits MCFG plus an ACPI `PNP0A08` root and removes the
  ECAM aperture from e820 usable RAM when guest RAM reaches that GPA.
- Legacy `0xcf8/0xcfc` remains unchanged in this phase.  It is not a required
  Linux discovery path and will be removed only under its own compatibility
  decision.

**Gate met:** pure config-space tests cover IDs, capability chain, command
masking, BAR sizing and extended-zero reads.  The offline Linux loader gate
covers checksums, MCFG and the 4 GiB e820 split.
`vmm_pcie_ecam_test.sh` on pc64 holds the provider at `pending` before machine
start, accepts `START`, performs generation-matched `REGISTER`, and confirms
`REGISTERED` carries BAR and DMA capabilities.  Linux then enumerates
`0000:00:01.0 [1b36:df01]` through MCFG and reports the expected sysfs vendor
and device values.

## P4: Direct BAR Mapping

**Status:** complete

- Provider registration declares fixed BARs, including 64-bit BARs.
- Map the same BAR backing object into the provider and into the consumer
  run-vmspace/NPT when memory decode is enabled.  BAR access then stays in the
  direct shared data path.
- ECAM BAR programming validates aperture, alignment, overlap, and memory
  decode.  The first guest BAR access maps the provider object into the
  consumer run-vmspace/NPT; later accesses stay on the direct shared path.
  The provider works by BAR index and offset, never by the selected guest GPA.
- Provider close or move-out is surprise removal.  It removes the guest BAR
  mapping and topology relation immediately; existing capability mappings are
  revoked through `OBJT_MGTDEVICE` and may still keep module unload busy.
- Resizable BAR, bridges, and hotplug remain deferred; reserve only their ABI
  shape now.

**Gate met:** `vmm_pcie_bar_test.sh` on pc64 holds the provider pending until
machine start, then validates the generation-matched BAR and DMA capability
handoff.  Linux enables `PCI_COMMAND.MEMORY` through ECAM, reads `0x11223344`,
and writes `0x55667788` through the direct BAR mapping; the provider observes
the write.  The initial BAR NPF installs one direct NPT mapping, while later
guest BAR reads and writes do not create VMEXITs.

## P5: MSI-X And Function Doorbells

**Status:** limited

- ABI version 3 introduced the beginning-of-BAR-0 reservation for the standard
  MSI-X table and PBA; the current ABI version 6 retains it. Registration
  rejects a BAR 0 that cannot contain all declared
  vector entries plus the aligned PBA; provider-defined payload starts after
  that reserved prefix.
- The standard MSI-X control, function mask, and vector mask are represented
  in the configuration/BAR objects.  A provider `MSI_X` message is checked
  against its live attachment generation, vector bound, enabled state, and
  masks.  Stale messages fail with `ESTALE`; disabled or masked vectors are
  deliberately ignored.
- The route accepts fixed-delivery MSI-X messages to the physical xAPIC
  destination encoded in the live table, with a vector of at least 32.  The
  backend resolves that destination against the machine's AVIC context and
  injects through the target vCPU's AVIC path.  x2APIC and logical-destination
  MSI formats remain rejected rather than silently falling back to CPU0.
- A `DOORBELL` BAR range traps guest writes and coalesces them into one private
  event capability per PCI function. `REGISTERED` passes that bidirectional
  fd after BAR and DMA capabilities. A provider reads it through kqueue and
  writes `MSI_X` after it has published completion state. Normal kicks do not
  use a kernel request/reply path or wait for provider I/O.
- The provider owns PBA, pending, and reassertion policy.  Direct guest table
  writes cannot trap reliably, so the VMM validates a live table snapshot but
  cannot infer an unmask transition or safely replay a pending vector.

**Gate met:** `test/vmm/pcie/vmm_pcie_msix_test.sh` on pc64 holds the provider
pending until machine start, validates the generation-matched BAR and DMA
capability handoff, then boots Linux and loads a minimal standard PCI driver.
The provider observes the function event kick, reads the live MSI-X table, sends
`MSI_X`, and Linux reports `DFVMM_PCIE_MSIX_IRQ_OK` and
`DFVMM_PCIE_MSIX_PROBE_OK`.  The two-vCPU gate boots with `irqaffinity=1`;
the provider observes `fee01000:00000000` and the guest confirms
`DFVMM_PCIE_MSIX_IRQ_COUNTS 0 1`.  This proves the table-selected physical
xAPIC destination reaches vCPU1 rather than falling back to vCPU0.  P4's
direct-NPT mapping gate remains the direct-BAR proof; the bounded machine
event log is diagnostic and is not used as a trace proof for this path. The
core transport and blk/net consumers are complete; vsock consumer conversion
is pending, so P5 remains limited.

## P8: Standard Virtio-Blk Provider

**Status:** complete for the raw-image target; multi-queue is limited pending
multi-vCPU parallel-I/O stress.

- ABI version 6 requires a complete page-aligned `DIRECT`/`TRAPPED`/`DOORBELL`
  BAR range layout. Direct faults map only the referenced page at its object
  offset; they never expose adjacent trapped pages.
- Trapped accesses use checked synchronous `MMIO_REQUEST`/`MMIO_RESPONSE`
  records. The provider session kthread remains the sole kernel socket reader;
  detach wakes every waiting vCPU request with an error.
- `/sbin/virtiod` supplies modern virtio-blk common configuration, ISR and
  device configuration through trapped pages; its notify page is a function
  doorbell.
  It declares one config MSI-X vector and one vector per configured queue.
  `queues=N` defaults to one, advertises `VIRTIO_BLK_F_MQ` when `N > 1`, and
  reports `num_queues` through the standard device configuration.
- Every configured block virtqueue has one provider worker and its own vring
  state. Control MMIO configures or resets a queue under that queue's mutex;
  unrelated queues continue to process I/O.

**Gate met:** ABI negative cases cover BAR ranges and MMIO packets; `make test`
in `sbin/virtiod` covers raw requests and virtqueues; pc64 boots the official
Alpine 3.24 standard ISO through `virtio_blk` to its local console login.

## P6: Relation Lifecycle And Software DPU

**Status:** deferred until an IOMMU-aware offload policy exists

- Existing P2 consumer sessions remain a control-plane capability, but no
  additional offload, host-consumer, software-DPU, or physical-provider DMA
  work is added before its IOMMU boundary is defined.
- `mv` changes consumer ownership live.  Move-out is surprise removal from
  the old running root; move-in sends asynchronous `START` when the new root
  is running.  Standard guest hotplug notification remains deferred.
- Provider loss, upstream reset, and revoke propagate to dependent child
  functions.  Child reset/failure does not leak into siblings.
- Establish parent/child relations for a process that consumes an upstream
  function and provides software-VF child functions.  The relation is explicit
  in core state; it is not inferred from PID or directory placement.
- Device/session mappings keep `vmm.ko` unload-busy until their ordinary
  capability lifetime ends.

**Tests:** move rejection while active; provider crash; parent reset; two child
functions with independent BAR, vector, and failure state; module unload
returns `EBUSY` while a BAR capability mapping remains.

## P7: Revocable DMA Capability (`OBJT_MGTDEVICE`)

**Status:** complete for non-QSD provider lifecycle; QSD-descendant coverage
belongs to P8

- Use the existing `OBJT_MGTDEVICE` capability pager.  Its owner publishes
  revoked state under the object token, then directly calls the existing
  `vm_object_page_remove(object, 0, 0, FALSE)`, which scans `backing_list` and
  retracts installed pmap entries.  Do not add a dfvmm-specific VM-core revoke
  wrapper or scan process maps.
- Add a per-run `vmm_dma` manager and a distinct DMA capability object for
  each registered provider.  An active capability holds a reference to the
  current runtime COW vmspace; its pager faults the runtime GPA with write
  access so it returns the current COW page, not the boot snapshot.  Revoke
  removes installed pmap entries, detaches that vmspace, and leaves only an
  inert capability object while retired mappings keep the module busy.  This
  preserves lazy allocation, reset isolation, and zero-copy sharing with
  guest NPT without extending guest-RAM lifetime.
- Normal machine stop sends `STOP` to every provider still owned by that
  machine, waits a bounded grace period for `STOPPED`, then revokes every
  remaining old-generation capability.  Provider detach or move-out is
  surprise removal and revokes only that provider's capability descendants
  immediately.  A new run creates new capability objects and memory generation.
- For a physical provider, map the same authorization through an isolated
  IOMMU domain: `IOVA -> HPA`.

**Gates met:** `vmm_pcie_dma_test.sh` verifies normal stop: retained BAR and
DMA mappings fault after `STOPPED`, keep `kldunload` busy after unmount, and
release it after provider exit.  `vmm_pcie_dma_reset_test.sh` verifies an
external `reset force` revokes the old mappings before the same provider
session registers a strictly newer DMA generation.  `vmm_pcie_dma_loss_test.sh`
verifies provider final-close without `STOPPED` is surprise removal: a mapping
holder sees both mappings fault, and keeps unload busy until it exits.

## P8: Modern Virtio Block Provider

**Status:** complete for the raw provider; P8.0 generic read-only PCI
vendor-capability transport and P8.1 modern virtio-blk are complete.
`sbin/virtiod` has raw block-I/O and virtqueue unit coverage. Its `queues=N`
mode creates one worker per block virtqueue, advertises the standard MQ
feature and assigns one config MSI-X vector plus one per queue. The pc64 gate
boots Alpine with `queues=2` and completes a DMA-backed block read. A
multi-vCPU parallel-I/O stress gate remains before calling multi-queue
production-complete. QSD remains a later optional block backend.

## P8.3: Modern Virtio-Vsock Provider

**Status:** limited; userspace build and ABI/configuration gates complete.

- `virtiod` accepts `vsock MACHINE/devices/NAME cid=N`; one process permits one
  configured vsock function and requires its inherited private `-s fd:N`
  broker endpoint.
- The provider registers standard modern `virtio-vsock-pci` (`1af4:1053`) with
  RX, TX and event virtqueues, four MSI-X vectors, direct notify, trapped
  common/ISR/device configuration, host CID 2 and guest CID from the standard
  device configuration.
- Initial transport is `SOCK_STREAM` only. It implements the request,
  response, reset, shutdown, payload and credit packet set; seqpacket and
  datagram remain unadvertised.
- `sys/sys/vmm_vsock_abi.h` defines the private versioned broker records.
  `CONNECT`, `LISTEN`, `ACCEPT` and `UNLISTEN` use the inherited Unix
  seqpacket control capability; connected stream FDs return through
  `SCM_RIGHTS`. Generation stop, reset and provider loss close old flow FDs
  and publish `DEVICE_DOWN`.

**Required pc64 gate:** Linux must bind `virtio_vsock`, then a guest listener
and host `CONNECT` must exchange bidirectional stream bytes. Stop/reset and
provider-close must produce EOF or `ECONNRESET` on every returned stream fd.

- Put the production raw provider in `sbin/virtiod` in the dfvmm tree. It may
  be installed as `/sbin/virtiod`. It is user-space vPCIe code, not
  `sys/vmm` code and not a test helper.
- P8.0 extends `REGISTER` with up to four generic PCI vendor-capability
  descriptors packed into the remaining conventional config header.
  `vmm_pcie_config` owns the capability chain and exposes it read-only; a
  virtio provider uses four descriptors for common, notify, ISR and device
  configuration without putting virtio semantics in the vmm core.
- Implement modern `virtio-blk-pci`, `VIRTIO_F_VERSION_1`, fixed 64-bit BAR,
  MSI-X, read/write/flush and a raw image. Queue count is static per provider
  configuration and each queue owns a data-plane worker.
- Map the per-run DMA capability directly in `virtiod`; it resolves virtqueue
  descriptor GPAs to the same pages used by guest NPT without copying.
- Reuse BSD-licensed bhyve virtqueue and raw block-I/O designs, while replacing
  bhyve PCI/VM interfaces with dfvmm provider, BAR, DMA, and MSI-X lifecycles.
- A later QSD backend may consume the same provider-facing virtio frontend.
- Then boot a disk-root Linux guest, test ext4 write/fsync/reboot, and add
  qcow2 after the raw path is stable.

## P8.2: Modern Virtio-Net Provider

**Status:** complete for the fixed single-queue scope below.  It remains a
limited virtio-net implementation; offloads, control queues and multiqueue are
separate future capabilities.

- Extend `/sbin/virtiod` with `net DEVICE_DIR TAP MAC`.  It registers one
  modern `virtio-net-pci` function with RX queue 0, TX queue 1 and three
  MSI-X vectors for configuration, RX and TX.
- Advertise and implement only `VIRTIO_F_VERSION_1`, `VIRTIO_NET_F_MAC` and
  `VIRTIO_NET_F_STATUS`.  Do not advertise checksum offload, GSO, mergeable
  RX buffers, control virtqueues or multiqueue before their complete paths
  exist.
- Reuse the established direct notify BAR and trapped common/ISR/device BAR
  pages.  TAP RX writes its zeroed virtio header and Ethernet payload directly
  into writable guest DMA descriptors; TAP TX strips the header and writes
  the payload with `writev()`.
- The TAP backend is a BSD-2-Clause port of the relevant bhyve backend
  behavior.  `virtiod` opens `/dev/<tap>` nonblocking but does not configure
  addresses, bridges or host routing; those remain administrator-owned
  `ifconfig` policy.

**Gate met:** `test/vmm/linux/linux_virtiod_net_test.sh` creates an isolated
TAP `/30`, boots the official Alpine extended ISO with block and network
providers, verifies the fixed MAC, then proves three guest-to-host ICMP replies.
It also verifies `STOP -> STOPPED`, both provider exits, device/machine removal,
unmount, and `kldunload`.  The harness changes neither a bridge nor host routing
outside its temporary TAP interface.

## P9: Compatibility Gates And Later Work

- Add a separate standard NVMe user provider using the same ECAM, BAR, DMA,
  and MSI-X core.  It needs controller reset, admin/I/O queues, PRP/SGL, and
  per-queue MSI-X.
- Add host user-space consumer adapters; DPDK and SPDK are compatibility gates
  for BAR mmap, polling, and IOVA DMA rather than special vPCIe classes.
- Add physical-provider offload only with IOMMU group/isolation support.
- Add multi-queue, multi-vCPU routing, Resizable BAR, hotplug, and bridges
  only after their lifecycle and interrupt prerequisites are separately ready.

## Validation Rules

- Do not use vkernel to prove vPCIe execution, mapping, interrupt, or
  lifecycle behavior.
- Start with pure parser tests, then module/mount/session tests without guest
  execution, then one-risk-level pc64 real-guest harnesses.
- Every real-guest harness has a dedicated `/var/tmp/dfvmm-*` mountpoint,
  explicit expected events, and cleanup for provider, guest, mount, and
  module.
- Never mix ECAM enumeration, BAR mapping, MSI-X, DMA, provider crash, and
  host stress in one first-run script.
