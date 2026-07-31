/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open vPCIe provider and consumer sessions.
 */
#include <sys/param.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketops.h>
#include <sys/socketvar.h>
#include <sys/systm.h>
#include <sys/un.h>
#include <sys/uio.h>

#include "vmm_device.h"
#include "vmm_pcie.h"
#include "vmm_pcie_user.h"

struct vmm_pcie_user {
	/* This object is owned only by its session kthread. */
	struct vmm_device	*borrow_imm_device;
	struct socket		*own_mut_peer;
	enum vmm_pcie_user_role	imm_role;
	struct vmm_pcie_abi_consumer_ready imm_consumer_ready;
};

static void	vmm_pcie_user_run(void *arg);
static void	vmm_pcie_user_provider_run(struct vmm_pcie_user *user);
static void	vmm_pcie_user_consumer_run(struct vmm_pcie_user *user);
static int	vmm_pcie_user_receive(struct socket *peer, void *buf,
		    size_t cap, size_t *sizep);

int
vmm_pcie_user_open(struct vmm_device *device, enum vmm_pcie_user_role role,
    struct ucred *cred, struct socket **user_socketp)
{
	struct vmm_pcie_user *user;
	struct socket *peer;
	struct socket *user_socket;
	int error;

	if (device == NULL || cred == NULL || user_socketp == NULL ||
	    (role != VMM_PCIE_USER_PROVIDER && role != VMM_PCIE_USER_CONSUMER))
		return EINVAL;
	*user_socketp = NULL;
	user = kmalloc(sizeof(*user), M_TEMP, M_WAITOK | M_ZERO);
	error = socreate(AF_LOCAL, &user_socket, SOCK_SEQPACKET, 0, curthread);
	if (error != 0)
		goto fail_user;
	error = socreate(AF_LOCAL, &peer, SOCK_SEQPACKET, 0, curthread);
	if (error != 0)
		goto fail_user_socket;
	error = soconnect2(user_socket, peer, cred);
	if (error != 0)
		goto fail_peer;
	user->borrow_imm_device = device;
	user->own_mut_peer = peer;
	user->imm_role = role;
	if (role == VMM_PCIE_USER_PROVIDER) {
		error = vmm_pcie_device_provider_attach(device, user);
	} else {
		error = vmm_pcie_device_consumer_attach(device, user,
		    &user->imm_consumer_ready);
	}
	if (error != 0)
		goto fail_connected;
	error = kthread_create(vmm_pcie_user_run, user, NULL, "vmm pcie");
	if (error != 0) {
		if (role == VMM_PCIE_USER_PROVIDER)
			vmm_pcie_device_provider_detach(device, user);
		else
			vmm_pcie_device_consumer_detach(device, user);
		goto fail_connected;
	}
	*user_socketp = user_socket;
	return 0;

fail_connected:
	(void)soclose(peer, 0);
fail_peer:
	(void)soclose(user_socket, 0);
fail_user_socket:
fail_user:
	kfree(user, M_TEMP);
	return error;
}

int
vmm_pcie_user_force_close(struct vmm_pcie_user *user)
{

	if (user == NULL || user->own_mut_peer == NULL)
		return EINVAL;
	/*
	 * SHUT_RDWR wakes the session kthread from its receive and makes the
	 * provider's peer observe device removal.  The caller keeps the fabric
	 * token, so this session cannot concurrently detach and free itself.
	 */
	return soshutdown(user->own_mut_peer, SHUT_RDWR);
}

static void
vmm_pcie_user_run(void *arg)
{
	struct vmm_pcie_user *user;

	user = arg;
	if (user->imm_role == VMM_PCIE_USER_PROVIDER)
		vmm_pcie_user_provider_run(user);
	else
		vmm_pcie_user_consumer_run(user);
	kthread_exit();
}

static void
vmm_pcie_user_provider_run(struct vmm_pcie_user *user)
{
	struct vmm_pcie_abi_register request;
	struct vmm_pcie_abi_registered response;
	struct file *bar_fps[VMM_PCIE_ABI_MAX_BARS];
	char discard[sizeof(struct vmm_pcie_abi_start)];
	size_t size;
	unsigned int bar_count;
	int error;

	error = vmm_pcie_user_receive(user->own_mut_peer, &request,
	    sizeof(request), &size);
	if (error == 0 && (size != sizeof(request) ||
	    le16toh(request.header.le_type) != VMM_PCIE_ABI_MSG_REGISTER))
		error = EPROTO;
	if (error == 0) {
		error = vmm_pcie_device_provider_register(user->borrow_imm_device,
		    user, &request, &response, bar_fps, &bar_count);
	}
	if (error == 0) {
		error = kern_sendmsg_rights(user->own_mut_peer, &response,
		    sizeof(response), bar_fps, bar_count, 0);
	}
	while (error == 0) {
		error = vmm_pcie_user_receive(user->own_mut_peer, discard,
		    sizeof(discard), &size);
		if (error == 0)
			error = size == 0 ? ECONNRESET : EPROTO;
	}
	vmm_pcie_device_provider_detach(user->borrow_imm_device, user);
	(void)soclose(user->own_mut_peer, 0);
	kfree(user, M_TEMP);
}

static void
vmm_pcie_user_consumer_run(struct vmm_pcie_user *user)
{
	char discard[sizeof(struct vmm_pcie_abi_start)];
	struct iovec iov;
	struct uio uio;
	size_t size;
	int error;

	iov.iov_base = &user->imm_consumer_ready;
	iov.iov_len = sizeof(user->imm_consumer_ready);
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = sizeof(user->imm_consumer_ready);
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = curthread;
	error = sosend(user->own_mut_peer, NULL, &uio, NULL, NULL, 0,
	    curthread);
	while (error == 0) {
		error = vmm_pcie_user_receive(user->own_mut_peer, discard,
		    sizeof(discard), &size);
		if (error == 0)
			error = size == 0 ? ECONNRESET : EPROTO;
	}
	vmm_pcie_device_consumer_detach(user->borrow_imm_device, user);
	(void)soclose(user->own_mut_peer, 0);
	kfree(user, M_TEMP);
}

static int
vmm_pcie_user_receive(struct socket *peer, void *buf, size_t cap,
    size_t *sizep)
{
	struct iovec iov;
	struct uio uio;
	int flags;
	int error;

	if (peer == NULL || buf == NULL || cap == 0 || sizep == NULL)
		return EINVAL;
	iov.iov_base = buf;
	iov.iov_len = cap;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = cap;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_READ;
	uio.uio_td = curthread;
	flags = 0;
	error = so_pru_soreceive(peer, NULL, &uio, NULL, NULL, &flags);
	if (error != 0)
		return error;
	*sizep = cap - uio.uio_resid;
	return 0;
}
