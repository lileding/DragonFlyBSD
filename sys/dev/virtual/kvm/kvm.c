/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly KVM-compatible frontend for the vmm runtime.
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysmsg.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vnode.h>

#include <linux/kvm.h>

#include "../vmm/vmm.h"
#include "kvm_eventfd.h"
#include "kvm_internal.h"
#include "kvm_vcpu.h"
#include "kvm_vm.h"

MALLOC_DEFINE(M_KVM, "kvm", "KVM compatibility frontend");

struct kvm_control {
	int unused;
};

static d_open_t kvm_open;
static d_close_t kvm_close;
static d_ioctl_t kvm_ioctl;
static d_mmap_single_t kvm_mmap_single;
static d_priv_dtor_t kvm_control_destroy;
static int kvm_ioctl_get_supported_cpuid(struct kvm_dfly_buffer *);
static int kvm_ioctl_get_msr_index_list(struct kvm_dfly_buffer *);
static int kvm_ioctl_get_msr_feature_index_list(struct kvm_dfly_buffer *);

static struct dev_ops kvm_ops = {
	{ "kvm", 0, D_MPSAFE },
	.d_open = kvm_open,
	.d_close = kvm_close,
	.d_ioctl = kvm_ioctl,
	.d_mmap_single = kvm_mmap_single,
};

static cdev_t kvm_dev;
struct lwkt_token kvm_frontend_token;
int kvm_file_count;
bool kvm_draining;

/* Linux _IO() commands use no direction bit; accept them during transition. */
#define KVM_LINUX_IO(number)	((unsigned long)((KVMIO << 8) | (number)))

static int
kvm_open(struct dev_open_args *ap)
{
	struct kvm_control *control;
	struct file *fp;
	int error;

	fp = ap->a_fpp ? *ap->a_fpp : NULL;
	if (fp == NULL)
		return ENOENT;

	control = kmalloc(sizeof(*control), M_KVM, M_WAITOK | M_ZERO);
	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		kfree(control, M_KVM);
		return EBUSY;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

	error = devfs_set_cdevpriv(fp, control, kvm_control_destroy);
	if (error != 0) {
		lwkt_gettoken(&kvm_frontend_token);
		--kvm_file_count;
		lwkt_reltoken(&kvm_frontend_token);
		kfree(control, M_KVM);
	}
	return error;
}

static int
kvm_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static void
kvm_control_destroy(void *arg)
{
	struct kvm_control *control = arg;

	lwkt_gettoken(&kvm_frontend_token);
	KKASSERT(kvm_file_count != 0);
	--kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);
	kfree(control, M_KVM);
}

static int
kvm_ioctl_capability(struct dev_ioctl_args *ap)
{
	int capability;

	/*
	 * Both DragonFly and Linux KVM _IO commands arrive through the
	 * ioctl argument slot.  Linux's encoding lacks IOC_VOID, so testing
	 * the command direction would decode its slot address as the request.
	 */
	capability = (int)(intptr_t)*(caddr_t *)ap->a_data;

	switch (capability) {
	case KVM_CAP_IRQCHIP:
	case KVM_CAP_IRQ_ROUTING:
	case KVM_CAP_IRQFD:
	case KVM_CAP_SIGNAL_MSI:
	case KVM_CAP_PIT2:
	case KVM_CAP_PIT_STATE2:
		ap->a_sysmsg->sysmsg_result = vmm_irqchip_available() ?
		    (capability == KVM_CAP_IRQ_ROUTING ? KVM_MAX_IRQ_ROUTES + 1 : 1) :
		    0;
		break;
	case KVM_CAP_USER_MEMORY:
	case KVM_CAP_SET_TSS_ADDR:
	case KVM_CAP_EXT_CPUID:
	case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
	case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
	case KVM_CAP_MP_STATE:
	case KVM_CAP_IOEVENTFD:
	case KVM_CAP_IOEVENTFD_ANY_LENGTH:
	case KVM_CAP_INTERNAL_ERROR_DATA:
	case KVM_CAP_DEBUGREGS:
	case KVM_CAP_XSAVE:
	case KVM_CAP_XCRS:
	case KVM_CAP_VCPU_EVENTS:
	case KVM_CAP_ADJUST_CLOCK:
	case KVM_CAP_SET_IDENTITY_MAP_ADDR:
	case KVM_CAP_IMMEDIATE_EXIT:
		ap->a_sysmsg->sysmsg_result = 1;
		break;
	case KVM_CAP_NR_MEMSLOTS:
		ap->a_sysmsg->sysmsg_result = KVM_MEMORY_SLOTS;
		break;
	case KVM_CAP_NR_VCPUS:
	case KVM_CAP_MAX_VCPUS:
		ap->a_sysmsg->sysmsg_result = KVM_MAX_VCPUS;
		break;
	default:
		ap->a_sysmsg->sysmsg_result = 0;
		break;
	}
	return 0;
}

static int
kvm_ioctl(struct dev_ioctl_args *ap)
{
	struct kvm_control *control;

	if (devfs_get_cdevpriv(ap->a_fp, (void **)&control) != 0)
		return ENXIO;
	KKASSERT(control != NULL);

	switch (ap->a_cmd) {
	case KVM_GET_API_VERSION:
	case KVM_LINUX_IO(0x00):
		ap->a_sysmsg->sysmsg_result = KVM_API_VERSION;
		return 0;
	case KVM_CHECK_EXTENSION:
	case KVM_LINUX_IO(0x03):
		return kvm_ioctl_capability(ap);
	case KVM_CREATE_VM:
	case KVM_LINUX_IO(0x01): {
		struct vnode *vp;
		int fd;
		int error;

		vp = ap->a_fp != NULL ? ap->a_fp->f_data : NULL;
		if (vp == NULL)
			return ENXIO;
		error = kvm_vm_create(curthread->td_lwp, vp, &fd);
		if (error == 0)
			ap->a_sysmsg->sysmsg_result = fd;
		return error;
	}
	case KVM_GET_VCPU_MMAP_SIZE:
	case KVM_LINUX_IO(0x04):
		ap->a_sysmsg->sysmsg_result = 2 * PAGE_SIZE;
		return 0;
	case KVM_DFLY_GET_SUPPORTED_CPUID:
		return kvm_ioctl_get_supported_cpuid(
		    (struct kvm_dfly_buffer *)ap->a_data);
	case KVM_DFLY_GET_MSR_INDEX_LIST:
		return kvm_ioctl_get_msr_index_list(
		    (struct kvm_dfly_buffer *)ap->a_data);
	case KVM_DFLY_GET_MSR_FEATURE_INDEX_LIST:
		return kvm_ioctl_get_msr_feature_index_list(
		    (struct kvm_dfly_buffer *)ap->a_data);
	case KVM_DFLY_CREATE_EVENTFD: {
		struct kvm_dfly_eventfd *request;
		struct vnode *vp;
		int error;

		request = (struct kvm_dfly_eventfd *)ap->a_data;
		if ((request->flags & ~KVM_DFLY_EVENTFD_VALID_FLAGS) != 0)
			return EINVAL;
		vp = ap->a_fp != NULL ? ap->a_fp->f_data : NULL;
		if (vp == NULL)
			return ENXIO;
		error = kvm_eventfd_create(curthread->td_lwp, vp, request->initial,
		    request->flags, &request->fd);
		return error;
	}
	default:
		return ENOTTY;
	}
}

/*
 * All functional KVM descriptors are anchored to the real /dev/kvm vnode.
 * The descriptor's per-file object selects the mapping, never a transient
 * cdev or special vnode.
 */
static int
kvm_mmap_single(struct dev_mmap_single_args *ap)
{

	return kvm_vcpu_mmap_single(ap);
}

static int
kvm_ioctl_get_msr_index_list(struct kvm_dfly_buffer *buffer)
{
	struct kvm_msr_list *list;
	void *user_buffer;
	size_t capacity;
	size_t count;
	size_t length;
	int error;

	if (buffer == NULL || buffer->data == 0 || buffer->reserved != 0)
		return EINVAL;
	capacity = buffer->length;
	if (capacity < sizeof(*list))
		return EINVAL;
	count = 0;
	error = kvm_vcpu_get_supported_msrs(NULL, &count);
	if (error != 0)
		return error;
	if (count > (SIZE_MAX - sizeof(*list)) / sizeof(list->indices[0]))
		return E2BIG;
	length = sizeof(*list) + count * sizeof(list->indices[0]);
	buffer->length = length;
	user_buffer = (void *)(uintptr_t)buffer->data;
	if (capacity < length) {
		struct kvm_msr_list header;

		header.nmsrs = count;
		return copyout(&header, user_buffer, sizeof(header)) == 0 ? E2BIG :
		    EFAULT;
	}
	list = kmalloc(length, M_KVM, M_WAITOK | M_ZERO);
	list->nmsrs = count;
	error = kvm_vcpu_get_supported_msrs(list->indices, &count);
	if (error == 0) {
		list->nmsrs = count;
		if (copyout(list, user_buffer, length) != 0)
			error = EFAULT;
	}
	kfree(list, M_KVM);
	return error;
}

/* No feature MSRs are exposed until the VMM backend implements their ABI. */
static int
kvm_ioctl_get_msr_feature_index_list(struct kvm_dfly_buffer *buffer)
{
	struct kvm_msr_list list;
	void *user_buffer;

	if (buffer == NULL || buffer->data == 0 || buffer->reserved != 0 ||
	    buffer->length < sizeof(list))
		return EINVAL;
	user_buffer = (void *)(uintptr_t)buffer->data;
	list.nmsrs = 0;
	buffer->length = sizeof(list);
	return copyout(&list, user_buffer, sizeof(list)) == 0 ? 0 : EFAULT;
}

static int
kvm_ioctl_get_supported_cpuid(struct kvm_dfly_buffer *buffer)
{
	struct vmm_cpuid_entry *vmm_entries;
	struct kvm_cpuid2 *cpuid;
	void *user_buffer;
	size_t capacity;
	size_t count;
	size_t length;
	size_t index;
	int error;

	if (buffer == NULL || buffer->data == 0 || buffer->reserved != 0)
		return EINVAL;
	capacity = buffer->length;
	if (capacity < sizeof(*cpuid))
		return EINVAL;
	error = vmm_x64_get_supported_cpuid(NULL, &count);
	if (error != 0)
		return error;
	if (count > (SIZE_MAX - sizeof(*cpuid)) / sizeof(cpuid->entries[0]))
		return E2BIG;
	length = sizeof(*cpuid) + count * sizeof(cpuid->entries[0]);
	buffer->length = length;
	user_buffer = (void *)(uintptr_t)buffer->data;
	/* Preserve the required count even when QEMU's initial buffer is small. */
	if (capacity < length) {
		struct kvm_cpuid2 header;

		header.nent = count;
		header.padding = 0;
		return copyout(&header, user_buffer, sizeof(header)) == 0 ? E2BIG :
		    EFAULT;
	}
	vmm_entries = kmalloc(count * sizeof(*vmm_entries), M_KVM,
	    M_WAITOK | M_ZERO);
	cpuid = kmalloc(length, M_KVM, M_WAITOK | M_ZERO);
	error = vmm_x64_get_supported_cpuid(vmm_entries, &count);
	if (error == 0) {
		cpuid->nent = count;
		for (index = 0; index < count; ++index) {
			cpuid->entries[index].function = vmm_entries[index].leaf;
			cpuid->entries[index].index = vmm_entries[index].subleaf;
			cpuid->entries[index].flags = vmm_entries[index].flags;
			cpuid->entries[index].eax = vmm_entries[index].eax;
			cpuid->entries[index].ebx = vmm_entries[index].ebx;
			cpuid->entries[index].ecx = vmm_entries[index].ecx;
			cpuid->entries[index].edx = vmm_entries[index].edx;
		}
		if (copyout(cpuid, user_buffer, length) != 0)
			error = EFAULT;
	}
	kfree(cpuid, M_KVM);
	kfree(vmm_entries, M_KVM);
	return error;
}

static int
kvm_modevent(module_t module, int event, void *arg)
{
	int error;

	(void)module;
	(void)arg;

	switch (event) {
	case MOD_LOAD:
		lwkt_token_init(&kvm_frontend_token, "kvmctl");
		kvm_file_count = 0;
		kvm_draining = false;
		kvm_dev = make_dev(&kvm_ops, 0, UID_ROOT, GID_WHEEL, 0600,
		    "kvm");
		return kvm_dev != NULL ? 0 : ENOMEM;
	case MOD_UNLOAD:
		lwkt_gettoken(&kvm_frontend_token);
		if (kvm_file_count != 0) {
			lwkt_reltoken(&kvm_frontend_token);
			return EBUSY;
		}
		kvm_draining = true;
		lwkt_reltoken(&kvm_frontend_token);
		if (kvm_dev != NULL) {
			destroy_dev(kvm_dev);
			kvm_dev = NULL;
		}
		return 0;
	case MOD_SHUTDOWN:
		return 0;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return error;
}

static moduledata_t kvm_mod = {
	.name = "kvm",
	.evhand = kvm_modevent,
	.priv = NULL,
};

DECLARE_MODULE(kvm, kvm_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(kvm, 1);
MODULE_DEPEND(kvm, vmm, 1, 1, 1);
