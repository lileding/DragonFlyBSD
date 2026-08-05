# vPCIe Design

## Status

This document defines the target `vPCIe` model.  P0 defines the public packet
layouts and pure validation in `sys/vmm/vmm_pcie_abi.{h,c}`.  P1 adds the
relation core in `vmm_pcie.{h,c}`, one `vmm_pcie_root` per machine plus the
fabric host root, and a core-owned function registry/BDF allocator.

P2 provides permanent `provider`, `consumer`, `state`, and `bdf` leaves.
Opening the first two creates private `SOCK_SEQPACKET` sessions.  A provider
session makes the function a pending member of its root's device set, but
does not expose PCI identity or runtime capability fds.  A running root sends
`START(generation)`; only the matching `REGISTER` creates that generation's
fixed profile and returns BAR/DMA capabilities in `REGISTERED`.  Final close
is surprise removal.  Closing a consumer returns the function to its prior
root.

P3 makes a run-registered function discoverable to a VM.  It fixes a one-segment,
bus-`00..ff` ECAM aperture at `0xe0000000..0xefffffff`, emits MCFG and a
`PNP0A08` ACPI PCI root from the Linux loader, and reserves that aperture from
e820 RAM.  `vmm_pcie_config` handles the standard configuration header,
capabilities, BAR sizing probes and command bits under the fabric token.  An
pc64 Linux gate holds the provider pending until `START`, validates the
generation-matched `REGISTERED` capabilities, and then enumerates
`0000:00:01.0 [1b36:df01]` through MCFG.

P4 maps a run-registered provider BAR directly into the running machine after
the guest enables `PCI_COMMAND.MEMORY` through ECAM.  The pc64 Linux gate
holds the provider pending until `START`, then verifies the guest and provider
read and write the same BAR page after one NPT BAR mapping is installed.

P5 completes the first interrupt path.  The ABI version-3 MSI-X BAR prefix is
retained by the current ABI version 6: BAR 0 begins with the standard MSI-X
table and PBA, while provider payload starts after that prefix.  A provider
waits for a private function event capability and writes a checked `MSI_X`
record only after publishing completion state.  The VMM reads
the live table, validates fixed physical-xAPIC delivery, and injects through
the table-selected vCPU's AVIC path.  The pc64 Linux gate holds its provider pending until `START`,
requires a generation-matched `REGISTERED` reply with BAR, per-run DMA and
function event capabilities, then observes coalesced doorbell completion and
the AVIC-routed MSI-X.

It supersedes the old device-binding discussion in `vmmfs.md` when vPCIe
implementation begins.  In particular, a host device directory is not merely
a physical-device pool, and user-provided devices do not use a separate
namespace or a separate ownership model.

P8 completes the first standard virtio-blk-pci control path. ABI version 6
requires every BAR page to be declared `DIRECT`, `TRAPPED`, or `DOORBELL`. Direct pages
remain shared NPT mappings; trapped pages use a synchronous provider request
and response on the existing provider `SOCK_SEQPACKET` session. The session
kthread is the only kernel socket receiver, so a vCPU waits on a request
object rather than reading the socket. `virtiod` uses trapped common
configuration, ISR and device configuration pages, and a doorbell notify page.
The pc64 gate boots the official Alpine standard ISO to its local console
login through the virtio-blk provider.

P8.2 adds the same modern virtio-pci transport for a TAP-backed
`virtio-net-pci` provider. It has one RX and one TX queue, function-event notification,
three MSI-X vectors, and only `VERSION_1`, MAC and link-status features. The
pc64 isolated TAP/Linux gate verifies guest TX, host TAP delivery, guest RX and
MSI-X delivery; offloads, control queues and multiqueue remain out of scope.

## PCIe Feature Matrix

This table records the guest-visible PCIe feature surface, rather than merely
the vPCIe control-plane API.  It must be updated in the same change that adds,
removes, or materially changes a vPCIe feature.

| PCIe feature | Status | Current implementation boundary |
|---|---|---|
| Single root complex and endpoint functions | complete | One MCFG segment covers bus `00..ff`; current allocation is bus 0, device `1..31`, function 0. |
| ECAM configuration access | complete | 4 KiB ECAM transaction surface; the implemented standard header occupies the conventional first 256 bytes. |
| Type-0 endpoint header | complete | Vendor/device/subsystem IDs, class/revision, command/status, BDF and six BAR descriptors are core-owned. |
| PCIe capability | limited | A minimal endpoint PCIe capability is advertised for modern enumeration; link, slot, power-management and error reporting registers are not implemented. |
| Memory BARs | complete | Up to six memory BAR definitions, including 64-bit and prefetchable attributes, sizing probes, `PCI_COMMAND.MEMORY`, overlap checks, page-granular direct NPT mappings, and provider-declared trapped subranges. |
| I/O BARs | not implemented | No legacy I/O-port BAR or PIO forwarding path is exposed. |
| Function doorbell capability | limited | A declared `DOORBELL` BAR range traps guest writes and coalesces `0->1` into one private bidirectional event fd per PCI function. `REGISTERED` passes it after BAR and DMA fds; read drains its sequence and write injects checked MSI-X. It intentionally has no virtqueue ID. blk and net use kqueue; vsock consumer conversion remains pending. |
| Synchronous BAR control MMIO | complete | Provider-declared trapped pages route common configuration, ISR read-to-clear and device configuration through one checked request/response record. |
| MSI-X | limited | Table/PBA, enable and mask handling, provider requests, and AVIC injection work for physical xAPIC destinations across running vCPUs. The pc64 gate proves CPU1 affinity. x2APIC and logical-destination MSI-X remain rejected. |
| MSI | not implemented | The first vPCIe surface deliberately exposes MSI-X only. |
| Modern virtio-blk-pci | limited | Raw-image read/write/flush, trapped common/ISR/device pages and function-event notify are complete. `queues=N` exposes `VIRTIO_BLK_F_MQ`, reports `num_queues`, allocates one MSI-X config vector plus one per queue, and runs one provider thread per virtqueue. The pc64 gate boots Alpine with `queues=2` and performs a DMA-backed block read; a multi-vCPU parallel-I/O stress gate remains required. |
| Modern virtio-net-pci | limited | One RX and one TX queue over a host TAP interface, trapped common/ISR/device pages, function-event notify, MAC/link-status configuration and three MSI-X vectors. The pc64 TAP gate verifies 3/3 guest-to-host ICMP replies and clean provider/module lifecycle. No offload, control queue or multiqueue is advertised. |
| Modern virtio-vsock-pci | limited | `virtiod` registers `1af4:1053` with RX, TX and event queues, one MSI-X config vector plus one per queue, fixed host CID 2 and configured guest CID. It exposes only standard stream transport and the private Unix broker ABI; pc64 Linux enumeration and stream gates remain pending. |
| PCI vendor-specific capabilities | complete | ABI v6 provides up to four validated, read-only descriptors packed into the remaining conventional 256-byte config header after MSI-X. The core owns linkage; providers own payload semantics. |
| Extended PCIe capabilities | not implemented | No extended-capability list is constructed in config space above offset `0xff`. |
| CPU-accessed guest DMA aperture | complete | Per-run, generation-scoped `OBJT_MGTDEVICE` capability maps authorized GPA ranges zero-copy and is revoked on stop, reset or provider loss. |
| Bus-master DMA / IOMMU / IOVA domain | not implemented | Physical provider DMA remains blocked until DragonFly has IOMMU isolation. Existing IOVA metadata is descriptive only for CPU-accessing providers. |
| Provider lifecycle and surprise removal | complete | `START`/`REGISTER`/`REGISTERED`/`STOP`/`STOPPED`, reset generations, final-close revoke, forceful `rmdir devices/<name>` slot destruction, and retained-mapping unload veto are covered by pc64 gates. |
| PCIe hotplug | not implemented | Ownership changes are supported, but guest-visible hotplug notification and slot state are deferred. |
| Function-level reset | not implemented | Machine reset creates a fresh resource generation; PCIe FLR is not exposed. |
| Bridges, multi-function devices and multiple segments | not implemented | The topology is intentionally a single root with independent endpoint functions. |
| SR-IOV, ATS, PRI and PASID | not implemented | These require IOMMU-backed physical DMA and are outside the current safety boundary. |
| AER, DPC and PCIe power management | not implemented | Provider loss is surfaced through lifecycle/revoke, not PCIe error or power-management capabilities. |

## Purpose

`vPCIe` is not a kernel PCIe device emulator and not a renamed passthrough
wrapper.  It is a per-function one-provider/one-consumer PCIe peer fabric
with an explicit control plane and a zero-copy data plane:

```text
provider <-> BAR/DMA capability objects + interrupt path <-> consumer
```

The fabric lets the same device relation connect a VM, the host, a user
process, a host-kernel service, or a physical PCIe function.  The endpoint
implementation changes; PCIe resource and lifecycle semantics do not.

The primary goals are:

- retain direct shared-memory data paths rather than forwarding ordinary MMIO
  traffic through VMM control code;
- make device ownership, consumer attachment, DMA authority, and teardown
  explicit and auditable;
- use standard PCIe ECAM, BAR, MSI, and MSI-X contracts where they apply;
- let a device move from host consumption to VM consumption through one
  operation, without converting it into a different kind of object;
- provide a common core below future host-kernel, user-space, and physical
  offload implementations.

## Compatibility Targets

vPCIe is intended to carry modern PCIe software stacks, not a project-specific
device ABI.  The first QSD-backed virtio block provider is a bring-up vehicle;
it must not constrain the resource contracts needed by the following targets.

| Target | vPCIe role | Contract that must remain standard |
|---|---|---|
| Linux `virtio-pci` | VM consumer of a user provider | Modern virtio PCI capabilities, 64-bit BARs, shared virtqueues, and MSI-X; no transitional or legacy I/O interface is required. |
| Linux NVMe | VM consumer of a user or future physical provider | Standard NVMe PCIe configuration, controller BAR, admin and I/O queues in consumer memory, PRP/SGL DMA addressing, controller reset, and MSI-X completion delivery. |
| DPDK | VM or host user-space consumer | PCI enumeration, BAR mappings, MSI-X, explicit DMA/IOVA mappings, and a real IOMMU domain whenever a physical function bus-masters memory. |
| SPDK | VM or host user-space consumer; optionally a storage service behind a separate provider | Direct BAR mappings, polled queues, DMA/IOVA mappings, standard NVMe queue semantics, and no requirement to pass ordinary I/O through a host kernel block layer. |

The common vPCIe ABI must therefore describe resources rather than a virtio
queue.  It must support multiple BARs, 64-bit BARs, a finite MSI-X vector set,
queue-local direct doorbells, and scatter/gather DMA windows.  The first
single-queue virtio provider may expose only the subset it implements, but it
must not redefine these common concepts.

## Terms

### Device

A `vmm_device` is one guest-visible PCIe function and its resource set:

- PCIe configuration space and BDF;
- up to six BAR resource definitions;
- MSI and/or MSI-X capability;
- provider and consumer attachments;
- lifecycle and reset generations.

The device is not identified by its guest BDF.  Different consumers may each
assign `0000:00:01.0`; the stable vmmfs identity is the device object and its
path.  The current implementation exposes one segment and advertises an
MCFG/ACPI root bus range of `00..ff`; its allocator currently places functions
only on bus 0, device slots 1 through 31, function 0.  It is not a promise of
multi-function, bridge, hotplug, or multi-segment topology.

### Provider

The provider supplies the function's implementation and resource backing.  A
provider may be:

- a physical PCIe function;
- a user process attached through the `provider` socket;
- a host-kernel service;
- a future forwarding or bridge implementation.

The provider exposes BAR backing, receives authorized DMA access, and raises
MSI/MSI-X.  A provider does not implicitly own the consumer's entire memory.

A user provider session belongs to the socket file reference, not to the
process that opened `provider`.  `fork()` inherits that fd; `SCM_RIGHTS` can
transfer another reference to any process.  The attachment persists until the
last reference to the session endpoint closes.  `O_CLOEXEC` only controls an
`exec` transition.  The sequenced packet transport preserves records, but a
provider that deliberately shares one session between processes must serialize
its own protocol ownership.

### Consumer

The consumer is the one PCI root/domain currently attached to the device.  A
consumer may be:

- `host`;
- a VM's vPCIe root;
- a future host user-space or kernel PCI consumer.

The consumer performs configuration-space and BAR access.  In a VM this is
the guest PCI root and its drivers; in the host it is the host's PCI consumer
domain.  A device has exactly one active consumer.

### One provider, one consumer

A device has exactly one active provider and one active consumer.  PCIe
transactions remain bidirectional:

```text
consumer -> provider: ECAM and BAR accesses
provider -> consumer: DMA and MSI/MSI-X
```

This rule is per function, not per process.  One process may consume an
upstream function and provide several child functions.  That is the software
DPU and software-SR-IOV composition: every child still has its own device
identity, consumer-local BDF, BARs, MSI-X allocation, reset generation, and
DMA authority.  This does not create multiple ownership edges.  Sharing,
multiplexing, or fan-out must use explicit child functions or an explicit
bridge/proxy device, never a hidden second consumer or provider on one
function.

## vmmfs Presentation

`vmmfs` remains the control and observation surface.  It is not Linux sysfs;
leaf names and text formats may resemble sysfs only when the semantics match.

The directory containing a device expresses its current machine ownership:

```text
/dev/vmm/<vm>/devices/<name>/
```

vmmfs has no host device namespace.  A user provider is created directly in
the guest that initially owns it.  Physical-device discovery and the eventual
sysfs integration are separate from this user-provider ABI.

The intended leaf set after later ECAM and resource phases is:

```text
devices/<name>/
  provider             user-provider socket factory, if this is a user provider
  consumer             user-consumer socket factory
  attachment           read-only current consumer/root snapshot
  bdf                  consumer-local BDF
  state                lifecycle snapshot
  vendor
  device
  subsystem_vendor
  subsystem_device
  class
  revision
  modalias
  resource
  resource0 ...
  msi_vectors
  msix_vectors
```

P2/P3 intentionally implement only `provider`, `consumer`, `state`, and
`bdf` as vmmfs leaves.  P3 configuration space is guest-only ECAM state, not
a writable filesystem `config` file.
`state` is exactly two lines: `provider=detached|pending|registered` and
`consumer=root|offloaded`.  The remaining observation/resource leaves appear
only with the phase that owns their backing semantics.

The PCI ID, class, BAR definitions, and interrupt capability become immutable
after provider registration.  `resource` uses the PCI/sysfs-style
`start end flags` line format.  A writable raw `config` file is deliberately
not part of the ABI: all guest configuration writes must pass through ECAM.
P0 has one PCI segment: `REGISTERED.le_bdf` stores the standard low-16-bit
`bus:device.function` encoding, and its high 16 bits must be zero.  A later
multi-segment root must version the ABI rather than reinterpret this field.

### Creating a user provider

Creating a user-provided function attaches it directly to the target machine:

```sh
mkdir /dev/vmm/<vm>/devices/<name>
```

P1 stops after `mkdir`: the new directory is an unregistered `NEW` function.
P2 adds permanent `provider` and `consumer` files.  They are socket factories:
opening `provider` attaches one user provider, and opening `consumer` offloads
the function from its current root to that consumer session.  Do not use
`touch`: its open-and-close sequence would attach and immediately detach a
provider.  A provider opens the file directly and receives a private
`SOCK_SEQPACKET` session:

```c
int provider = open(path_to_provider, O_RDWR | O_CLOEXEC);
recvmsg(provider, START_WITH_GENERATION);
sendmsg(provider, REGISTER_WITH_GENERATION_AND_STATIC_PROFILE);
recvmsg(provider, REGISTERED_WITH_RUNTIME_CAPABILITIES);
```

Opening `provider` adds the function to its current root's device-ownership
set, but it creates no PCI-visible identity and grants no BAR or DMA mapping.
When that root is running, or when it next starts, vmm sends
`START(generation)`.  The provider responds with
`REGISTER(generation, static-profile)`, declaring PCI IDs, BAR layout,
direct/trapped doorbell policy, and MSI/MSI-X limits.  vmm validates the
generation, creates that run's resources, and replies
`REGISTERED(generation)` with BAR and DMA capability fds through `SCM_RIGHTS`.
The fixed little-endian packet layouts are defined by
`sys/vmm/vmm_pcie_abi.h`; `vmm_pcie_abi_validate()` is the shared
kernel/userland structural validator.  The binary packets are not vmmfs
text-file formats.

The provider attachment follows the session endpoint's last file reference.
It can therefore survive both `fork()` and an `SCM_RIGHTS` transfer.  Closing
one inherited or transferred copy does not detach the function while another
copy remains open; closing the last copy removes the function from the
current root's device set.  This is surprise removal: it does not send `STOP`
or wait for the backend.  It is distinct from the independently retained
runtime capability fds returned by `REGISTERED`.

### Opening a consumer session

`open(consumer)` creates a private `SOCK_SEQPACKET` session and offloads the
function from its current root to that session.  Closing the consumer socket
returns the function to the root it had when the session opened.
P2 exposes only the two-line `state` snapshot and `bdf`; `attachment` is a
later observation leaf.  Closing a provider socket detaches that provider and
removes its registered identity and BAR relation; it does not permanently
fail the function.  A process may keep one consumer session for an upstream
function while registering provider sessions for distinct child functions.

A discovered physical device already has a physical provider and cannot be
turned into a user provider by overwriting that attachment.  A future explicit
bridge/provider may consume a physical device and expose a new provider; it
must be modelled as a separate device relation.

### Consumer transfer

`mv` is the sole operation that changes the current machine ownership:

```sh
mv /dev/vmm/<from>/devices/<name> \
   /dev/vmm/<to>/devices/
```

The provider and device identity remain intact while ownership changes from
`<from>` to `<to>`.  The VFS operation expresses ownership transfer, while the
core operation updates the unique machine-root edge.

`mv` changes the device ownership set immediately, regardless of either
root's running state.  Moving out of a running root is surprise removal: vmm
revokes the old root's runtime resources without sending `STOP`.  Moving into
a running root causes vmm to send that provider `START` as soon as possible.
The current design does not yet send a standard PCIe/ACPI hotplug notification
to the guest; this limits guest discovery, not host-side ownership changes.

## Direct Peer Data Plane

When both endpoints can map a resource, vPCIe's default data plane is direct
shared mapping, not a VMM request/reply loop:

```text
provider mmap <-> shared capability object <-> consumer mmap or guest NPT
```

The kernel creates the object, validates the provider/consumer pair, applies
mapping permissions, and owns lifecycle.  It does not copy ordinary BAR or
DMA data and does not relay normal MMIO operations.

### BAR mappings

Each BAR is a provider-owned capability object.

- A user provider receives a BAR fd through `SCM_RIGHTS` and uses
  `mmap(MAP_SHARED)`.
- A host user-space consumer may receive a mapping of the same object through
  its consumer adapter.
- A VM consumer receives the same pages through NPT at the BAR GPA selected
  through PCI configuration space.
- A physical provider uses its bus-space/BAR mapping behind the same core
  resource definition.

The user mapping rejects `MAP_PRIVATE` and `PROT_EXEC`.  Ordinary BAR pages
are directly readable and writable by their permitted peers.  Device-specific
register and ring semantics belong to the provider/consumer protocol, not to
vPCIe.

For the current ABI version 6 MSI-X provider, BAR 0 has the vmm-owned prefix
introduced by ABI version 3.  It begins
with the standard 16-byte entries of the MSI-X table, followed by the
8-byte-aligned PBA; the first provider-defined byte is
`VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(vector_count)`.  Registration rejects a BAR 0
that does not contain this prefix.  The provider owns PBA, pending, and
unmask/reassertion semantics: the guest writes the table directly, so the VMM
cannot observe a vector becoming unmasked without adding a VMEXIT to the data
path.  The VMM only reads a current table entry when it validates an explicit
provider interrupt request.

Opening `provider` declares that the function belongs to its current root.
Closing the final session reference or moving the function out is surprise
removal: vmm removes the function from ECAM, retracts all installed BAR/DMA
mappings for that root, and returns without a `STOP` handshake.  Backend and
guest access to an already mapped resource then faults as an ordinary,
catchable user or guest failure.  A guest restart does not detach a still-open
provider.  Machine deletion first follows normal machine-stop ordering for
every device still in the root's set, then force-closes the provider control
session.  A provider endpoint that has already disappeared cannot delay
teardown and is handled as surprise removal.

BAR and DMA fds are independent `OBJT_MGTDEVICE` capabilities.  Revoke first
publishes the capability's revoked state under its object token, then uses
`vm_object_page_remove()` to remove installed pmap entries.  A retained
capability object may still hold vmm pager code, so `kldunload` returns
`EBUSY` until the last derived fd/mapping is gone.

### Doorbells

The default doorbell mode is direct and does not cause a VMEXIT:

```text
consumer writes descriptors/ring
consumer release-stores a shared kick sequence
provider acquire-loads the sequence and processes the work
```

The provider can busy-poll a dedicated CPU or use an adaptive polling policy.
That CPU/latency trade-off is explicit and belongs to the provider.

An optional trapped mode exists for low-rate control paths.  The relevant page
is write-protected in the guest NPT; a guest write causes a root-only exit,
the core queues a provider notification, and immediately re-enters the guest.
This avoids a user-space VMM exit but cannot avoid the architectural CPU
VMEXIT.  Hardware has no generic mechanism that converts an arbitrary direct
guest store into a host kqueue event without an intercept.

### Interrupts

The provider requests guest delivery with a control message after it has
published device completion state:

```text
sendmsg(provider, MSI)
sendmsg(provider, MSI_X)
```

The core validates the registered capability, selected vector, function and
vector masks, consumer attachment generation, and current MSI-X table entry.
The current one-vCPU route accepts fixed delivery to physical xAPIC
destination 0 and vectors 32 or higher.  For an AMD SVM VM consumer, delivery
uses AVIC.  Disabled or masked requests are intentionally ignored, while a
stale attachment request fails with `ESTALE`.  No fake INTx, PIC, or legacy
interrupt path is introduced.

## DMA Mappings

DMA is also expressed as a capability mapping, but it is not equivalent to a
BAR and must not expose all guest RAM by accident.

```text
provider mmap <-> authorized consumer-memory window
```

- BAR memory is provider-owned device memory offered to the consumer.
- DMA memory is consumer-owned memory offered to the provider.
- A user provider maps only authorized GPA ranges through a DMA capability;
  it never receives loader fd 3 or an unbounded guest-memory handle.
- The mapping is zero-copy: provider CPU access, guest NPT access, and future
  physical DMA refer to the same pages.

Every DMA window has two separately named address views.  A VM consumer uses
GPA to describe its memory in device protocols such as virtio and NVMe.  A
physical provider receives an IOVA chosen by its IOMMU domain for the same
authorized pages:

```text
guest protocol: GPA -> authorized memory pages
physical provider: IOVA -> IOMMU -> HPA
```

The user-provider mapping is an fd-backed CPU view of these pages, not proof
that GPA is a host physical address or an IOVA.  The provider DMA ABI must
carry a run generation, length, permissions, page/segment boundaries, GPA,
and IOVA when one exists.  This prevents the first vhost-user virtio consumer
from locking the future NVMe, DPDK, or SPDK paths into a GPA-only contract.

For a VM consumer, DMA capability is per run generation.  It becomes valid
only after runtime guest memory exists and must become invalid on stop or
reset.  A new run receives a new DMA capability generation.

Physical offload adds an IOMMU mapping of the same authorization decision:

```text
IOVA -> HPA
```

No physical device may bus-master guest memory without an IOMMU domain and a
validated group/isolation policy.  Absence of an IOMMU is a hard prohibition,
not a fallback to unrestricted host DMA.

P7 uses the existing DragonFly `OBJT_MGTDEVICE` pager behavior for already
established mappings.  Each DMA capability publishes its own revoked state
under the object token, then directly calls
`vm_object_page_remove(object, 0, 0, FALSE)`.  That existing API scans the
object's `backing_list` and removes installed pmap entries; it neither scans
process maps nor defines capability policy.  New mapping and later pager
faults fail as ordinary user failures.  While active, the cap holds the
current runtime vmspace.  Revoke removes installed pmap entries and
immediately detaches that vmspace; a pager fault that started before revoke
owns a temporary reference.  Retired mappings therefore retain only the inert
capability object needed to turn later access into a user fault, not guest
memory.  The retained capability object vetoes `kldunload`; that is the
accepted module-lifetime boundary.  A more general VM/DRM revocable-mmap API
is separate work and must not be introduced by dfvmm.

`vmm_dma` owns one runtime-memory generation, while each registered provider
receives a distinct capability fd for that generation.  The capability keeps a
reference to the current runtime COW vmspace and faults a requested GPA for
write before returning its page.  Therefore provider CPU access, guest NPT,
and future IOMMU DMA share the current page with no copy; an old capability
cannot accidentally expose the loader-complete boot snapshot or a later reset
generation.  Provider disconnect revokes only its own capability; stop and
reset revoke all capabilities belonging to the old run.  `OBJT_MGTDEVICE`
still provides the separate module-unload veto while retired mappings retain
pager code.

The pc64 P7 lifecycle gates verify this boundary end to end.  Normal stop
requires a provider `STOPPED` acknowledgement before its retained BAR and DMA
mappings fault and keep `kldunload` busy.  External `reset force` must revoke
the old generation before the same provider session registers a newer one.
Provider final-close is surprise removal: a separate mapping holder retains no
provider session, sees both mappings fault, and still vetoes module unload
until it exits.  QSD-descendant mapping coverage belongs to the first QSD
provider gate.

## PCIe Configuration and BARs

v0 has one segment, one root, bus 0, ECAM, and MSI/MSI-X only.  It does not
provide legacy configuration I/O, INTx, PIC, or PIT compatibility.

The guest writes BAR base registers and `PCI_COMMAND` through ECAM.  vPCIe
validates aperture, alignment, overlap, and memory-decode state; the first
access after memory decode maps the provider's BAR backing into the running
machine, and ordinary later BAR loads/stores use that direct mapping.  The
provider works by BAR index and offset; it does not depend on the GPA that the
consumer selected.  Mapping `resourceN` in Linux does not itself enable
memory decode, so a guest user program must set `PCI_COMMAND.MEMORY` first.

BAR shape is fixed at registration.  Resizing is a separate standard PCIe
Resizable BAR capability, not an arbitrary text configuration update:

- the provider declares a finite supported-size bitmap and an initial size;
- the core exposes the standard Resizable BAR extended capability only when
  such a bitmap exists;
- the consumer must disable memory decode, select a supported size, program a
  valid aligned base, then re-enable decode;
- the BAR backing is allocated at its maximum supported size, while the
  consumer-visible prefix is changed by NPT/resource updates;
- provider mappings remain stable and receive a `BAR_RESIZED` control
  notification with the new visible size and generation.

v0 does not expose Resizable BAR.  Reserving this ABI shape now avoids an
incompatible redesign later.

## Device Ownership And Power Lifecycle

Every root has a real-time device-ownership set `D(root)`.  `mv` and a
provider session's final open/close effect are ownership declarations, not
machine lifecycle operations.  `vmmfs` presents the relation; core owns the
set and all resource transitions.

Each `devices/<name>` directory is a PCI slot, not an inserted function:

```text
mkdir devices/<name>       create an empty slot
open(provider)             insert a card into that slot
close(provider)            remove the card from that slot
REGISTER                   power the inserted card on
UNREGISTER                 power the inserted card off
rmdir devices/<name>       destroy the slot
```

Slot destruction is forceful.  It first powers the card off, disconnects any
provider and consumer session, revokes all slot vnodes, waits for their gates
to drain, and only then removes the slot.  An open provider or registered card
does not change `rmdir` into `EBUSY`.

```text
machine starts:                  START every member of D(machine)
device joins D(running machine): START that device immediately
machine stops:                   STOP every current member of D(machine)
device leaves D(machine):        surprise removal, no STOP
```

`START(generation)` is asynchronous and never delays vCPU start.  A provider
answers it with `REGISTER(generation, static-profile)`; vmm then allocates
and returns the BAR, MSI-X, and DMA capabilities for that generation.  The
device can therefore initialize in parallel with CPU and guest boot.

For normal machine stop, every device that remains in `D(machine)` receives
`STOP(generation)`.  The backend closes its generation resources and replies
`STOPPED(generation)`; after a bounded grace interval, vmm force-revokes any
remaining resources.  A close/provider loss or `mv` out is not normal stop:
the transport or ownership is already gone, so vmm immediately revokes and
does not wait.  There is no guest PCIe/ACPI hotplug notification yet; a guest
may only discover a newly added function on rescan or next boot, while access
to a removed function fails normally.

Each reset creates a fresh resource and DMA generation.  Old capabilities
cannot reach that generation.  Retained capability objects intentionally make
`kldunload` return `EBUSY` until their final fd/mmap reference disappears.

## Implementation Order

1. Core provider/consumer relation and P2 user session capability objects.
2. Single-root ECAM with static BDF and fixed BAR resource allocation.
3. Direct BAR peer mode, direct doorbell sequence ABI, and MSI/MSI-X through
   the existing AVIC path.
4. Host consumer adapter using the same core resource interface.
5. Revocable DMA capability and memory-generation transition rules.
6. IOMMU-backed physical provider/offload.
7. Resizable BAR, hotplug, bridges, and multi-vCPU routing after their
   underlying lifecycle and interrupt requirements are ready.
8. Standard NVMe user provider and host user-space PCI consumer adapters;
   DPDK and SPDK are compatibility gates for the latter, not special vPCIe
   device classes.

## Explicit Non-goals

- QEMU PC compatibility, legacy PCI configuration ports, INTx, PIC, or PIT.
- A software device model that forwards every BAR access through the kernel.
- Giving a provider loader fd 3 or unrestricted guest RAM.
- Giving a physical provider DMA without an IOMMU policy, or a user provider
  DMA before per-run revocable mapping exists.
- Treating `kldunload` `EBUSY` as a substitute for mapping revocation.
- Hidden multi-provider or multi-consumer sharing.
- Host/VM-specific device namespaces with different ownership semantics.
