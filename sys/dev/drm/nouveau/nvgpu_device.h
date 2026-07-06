/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#ifndef _NVGPU_DEVICE_H_
#define _NVGPU_DEVICE_H_

#include <sys/bus.h>

struct nvgpu_device;

/*
 * struct nvgpu_device
 *
 * Ownership:
 *   Owned by the PCI/newbus attachment.  DRM devices, DRM files, GPU process
 *   objects, GSP helpers, and display helpers borrow this object; none of them
 *   may free it or retain it past their documented lifetime boundary.
 *
 * Lifetime:
 *   Created by PCI attach before the boot LWKT starts.  It remains alive until
 *   kldunload or detach has rejected new opens, destroyed the DRM registration,
 *   drained userspace-visible references, stopped interrupts, and torn down GSP
 *   state.  Boot failure unwinds through the same owner.
 *
 * Threading:
 *   The object is the global coordination root, but it is not itself a giant
 *   lock.  Each mutable subsystem owns its token, spinlock, lock, or scheduler
 *   queue.  Cross-subsystem call paths must document the lock order at the API
 *   boundary that crosses the subsystem.
 */
struct nvgpu_device;

/*
 * nvgpu_device_boot()
 *
 * Ownership:
 *   Borrows gpu from PCI attach.  It does not consume the device; success makes
 *   the device public through nvdrm_register(), and failure asks the PCI owner
 *   to tear the device down.
 *
 * Lifetime:
 *   Runs once in a dedicated boot LWKT after the newbus attach method has
 *   returned.  It initializes GSP, BAR mappings, GPU memory management, display,
 *   interrupts, and finally the DRM registration.
 *
 * Threading:
 *   May sleep and perform RPC waits.  It must not run on the newbus attach
 *   thread, and it must not publish DRM nodes until the GPU is ready to service
 *   opens without additional boot work.
 */
void nvgpu_device_boot(void *arg);

/*
 * nvgpu_device_stop()
 *
 * Ownership:
 *   Borrows gpu from PCI detach or kldunload teardown.  It does not free the
 *   memory containing the nvgpu_device; the PCI owner frees that storage after
 *   this function returns.
 *
 * Lifetime:
 *   Called only after unload admission has established that no userspace open,
 *   scheduler, or mmap reference can still enter the driver.
 *
 * Threading:
 *   May sleep while shutting down DRM, interrupts, display, GSP, and BAR state.
 *   It must stop external event sources before freeing objects reachable from
 *   interrupt or scheduler paths.
 */
void nvgpu_device_stop(struct nvgpu_device *gpu);

/*
 * nvgpu_device_from_softc()
 *
 * Ownership:
 *   Returns a borrowed nvgpu_device pointer from the temporary legacy softc
 *   bridge used during migration.
 *
 * Lifetime:
 *   Valid only while the caller already owns a lifetime guarantee for the
 *   enclosing PCI device or DRM device.
 *
 * Threading:
 *   Performs no synchronization.  The caller must hold the same protection that
 *   made the legacy softc pointer valid.
 */
struct nvgpu_device *nvgpu_device_from_softc(void *softc);

/*
 * nvgpu_device_pci_probe()
 *
 * Ownership:
 *   Borrows the newbus device.  It does not allocate or retain GPU state.
 *
 * Lifetime:
 *   Called by newbus before attach.  It only matches supported NVIDIA PCI IDs
 *   and publishes the device description.
 *
 * Threading:
 *   Runs in the newbus probe path and must not sleep on GSP, DRM, or display
 *   work.  It is a pure discovery step.
 */
int nvgpu_device_pci_probe(device_t dev);

/*
 * nvgpu_device_pci_attach()
 *
 * Ownership:
 *   Borrows the newbus device and creates the driver-owned GPU state currently
 *   represented by the legacy nvkm_softc.  The newbus child keeps the lifetime
 *   anchor until detach.
 *
 * Lifetime:
 *   Called once after probe accepts the device.  The current implementation is
 *   still the legacy synchronous attach body; later refactor steps will move
 *   boot work into the nvgpu_device boot LWKT.
 *
 * Threading:
 *   Runs on the newbus attach path today.  While this migration step keeps
 *   behavior unchanged, future work must move long GSP boot waits out of this
 *   thread before publishing DRM nodes.
 */
int nvgpu_device_pci_attach(device_t dev);

/*
 * nvgpu_device_pci_detach()
 *
 * Ownership:
 *   Borrows the newbus device and tears down the GPU state owned by attach.
 *
 * Lifetime:
 *   Called by detach/kldunload after the bus layer has admitted teardown.  It
 *   must leave no interrupt, scheduler, mmap, DRM, or GSP reference reachable
 *   after returning success.
 *
 * Threading:
 *   May sleep during teardown.  It must stop external event sources before
 *   freeing state they can reach.
 */
int nvgpu_device_pci_detach(device_t dev);

#endif /* _NVGPU_DEVICE_H_ */
