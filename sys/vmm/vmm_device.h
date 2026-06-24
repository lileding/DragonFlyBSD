/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A PCIe passthrough device: a bus/device/function identity that is either in
 * the host pool (owner == NULL) or bound to a guest VM.
 * FS presentation: vmmfs_device.c.
 *
 * owner is a pointer to the core VM (struct vmm_machine), never the fs slot --
 * that one-way dependency is what keeps this header reusable by kvm.ko.
 *
 * Types (size_t) come from the includer.
 */
#ifndef VMM_DEVICE_H
#define VMM_DEVICE_H

#define VMM_BDF_MAX	31

struct vmm_machine;		/* owner pointer only; no layout dependency */

struct vmm_device {
	char			bdf[VMM_BDF_MAX + 1];
	struct vmm_machine     *owner;		/* bound VM, or NULL = host pool */
	int			is_host;	/* host device (rm returns it) vs user backend */
};

/* Initialize: copy the bdf (truncated to VMM_BDF_MAX), unbound. */
void	vmm_device_init(struct vmm_device *d, const char *bdf, int is_host);
void	vmm_device_bind(struct vmm_device *d, struct vmm_machine *owner);
void	vmm_device_unbind(struct vmm_device *d);
int	vmm_device_owned_by(const struct vmm_device *d, const struct vmm_machine *m);
/* True if the bdf equals name[0..nlen) exactly. */
int	vmm_device_bdf_eq(const struct vmm_device *d, const char *name, size_t nlen);
/* Serialize the "<bdf>\n" line; bytes written (0 if it does not fit). */
size_t	vmm_device_format(const struct vmm_device *d, char *out, size_t cap);

#endif /* VMM_DEVICE_H */
