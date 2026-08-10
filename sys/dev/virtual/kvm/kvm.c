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
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysmsg.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include <sys/kvm.h>

#include "kvm_eventfd.h"
#include "kvm_internal.h"

MALLOC_DEFINE(M_KVM, "kvm", "KVM compatibility frontend");

struct kvm_control {
	int unused;
};

static d_open_t kvm_open;
static d_ioctl_t kvm_ioctl;
static d_priv_dtor_t kvm_control_destroy;

static struct dev_ops kvm_ops = {
	{ "kvm", 0, D_MPSAFE },
	.d_open = kvm_open,
	.d_ioctl = kvm_ioctl,
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

	if ((ap->a_cmd & IOC_VOID) != 0)
		capability = (int)(intptr_t)*(caddr_t *)ap->a_data;
	else
		capability = (int)(intptr_t)ap->a_data;

	/* No capability is advertised before its complete ABI is implemented. */
	(void)capability;
	ap->a_sysmsg->sysmsg_result = 0;
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
	case KVM_LINUX_IO(0x01):
		return ENOTSUP;
	case KVM_GET_VCPU_MMAP_SIZE:
	case KVM_LINUX_IO(0x04):
		return ENOTSUP;
	case KVM_DFLY_CREATE_EVENTFD: {
		struct kvm_dfly_eventfd *request;
		int error;

		request = (struct kvm_dfly_eventfd *)ap->a_data;
		if ((request->flags & ~KVM_DFLY_EVENTFD_VALID_FLAGS) != 0)
			return EINVAL;
		error = kvm_eventfd_create(curthread->td_lwp, request->initial,
		    request->flags, &request->fd);
		return error;
	}
	default:
		return ENOTTY;
	}
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
