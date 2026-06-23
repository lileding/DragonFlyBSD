/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vcpu config object: the desired CPU count.  Pure value + logic, no
 * kernel/VFS deps -- part of the reusable vmm_ core (vmm.ko and a future
 * kvm.ko).  Its filesystem presentation lives in vmmfs_vcpu.c.
 *
 * Types (uint32_t/size_t) come from the includer.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

struct vmm_vcpu {
	uint32_t	count;		/* 0 = unset */
};

/* Parse the whole buffer; update iff valid (1..256).  1 = updated, 0 = reject. */
int	vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len);
/* Serialize the value as decimal + newline; bytes written (0 if unset). */
size_t	vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap);
int	vmm_vcpu_is_set(const struct vmm_vcpu *v);

#endif /* VMM_VCPU_H */
