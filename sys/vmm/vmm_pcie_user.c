/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open vPCIe provider and consumer sessions.
 */
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kthread.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketops.h>
#include <sys/socketvar.h>
#include <sys/systm.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <machine/atomic.h>

#include "vmm_device.h"
#include "vmm_pcie.h"
#include "vmm_pcie_user.h"

struct vmm_pcie_user {
	/*
	 * Ref map:
 * The session kthread owns the initial reference.  A machine START/STOP
	 * holds a temporary reference after snapshotting an attached provider under
	 * token_registry.  The final release closes own_mut_peer, so a concurrent
	 * lifecycle send never dereferences a socket freed by session teardown.
	 */
	struct vmm_device	*borrow_imm_device;
	void			*borrow_imm_release_arg;
	vmm_pcie_user_release_fn	*fnonce_release;
	struct socket		*own_mut_peer;
	enum vmm_pcie_user_role	imm_role;
	struct vmm_pcie_abi_consumer_ready imm_consumer_ready;
	/* token_requests protects mut_requests and mut_next_request_id. */
	struct lwkt_token	token_requests;
	TAILQ_HEAD(, vmm_pcie_mmio_request) mut_requests;
	uint64_t		mut_next_request_id;
	int			atomic_mut_refs;
};

struct vmm_pcie_mmio_request {
	TAILQ_ENTRY(vmm_pcie_mmio_request) own_mut_entry;
	struct vmm_pcie_abi_mmio own_mut_message;
	uint64_t		mut_value;
	int			mut_error;
	int			mut_done;
};

volatile u_int vmm_pcie_user_session_count;

static void	vmm_pcie_user_run(void *arg);
static void	vmm_pcie_user_provider_run(struct vmm_pcie_user *user);
static void	vmm_pcie_user_consumer_run(struct vmm_pcie_user *user);
static int	vmm_pcie_user_receive(struct socket *peer, void *buf,
		    size_t cap, size_t *sizep);
static int	vmm_pcie_user_receive_nowait(struct socket *peer, void *buf,
		    size_t cap, size_t *sizep);
static int	vmm_pcie_user_send(struct socket *peer, const void *message,
		    size_t size);
static void	vmm_pcie_user_requests_abort(struct vmm_pcie_user *user,
		    int error);
static void	vmm_pcie_user_provider_message(struct vmm_pcie_user *user,
		    const void *message, size_t size, int *errorp);

int
vmm_pcie_user_open(struct vmm_device *device, enum vmm_pcie_user_role role,
    struct ucred *cred, void *release_arg, vmm_pcie_user_release_fn *release,
    struct socket **user_socketp)
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
	user->borrow_imm_release_arg = release_arg;
	user->fnonce_release = release;
	user->own_mut_peer = peer;
	user->imm_role = role;
	lwkt_token_init(&user->token_requests, "vmmpcieq");
	TAILQ_INIT(&user->mut_requests);
	user->mut_next_request_id = 1;
	user->atomic_mut_refs = 1;
	if (role == VMM_PCIE_USER_PROVIDER) {
		error = vmm_pcie_device_provider_attach(device, user);
	} else {
		error = vmm_pcie_device_consumer_attach(device, user,
		    &user->imm_consumer_ready);
	}
	if (error != 0)
		goto fail_connected;
	atomic_add_int(&vmm_pcie_user_session_count, 1);
	error = kthread_create(vmm_pcie_user_run, user, NULL, "vmm pcie");
	if (error != 0) {
		atomic_subtract_int(&vmm_pcie_user_session_count, 1);
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

void
vmm_pcie_user_hold(struct vmm_pcie_user *user)
{

	if (user != NULL)
		atomic_add_int(&user->atomic_mut_refs, 1);
}

void
vmm_pcie_user_release(struct vmm_pcie_user *user)
{
	struct socket *peer;

	if (user == NULL || atomic_fetchadd_int(&user->atomic_mut_refs, -1) != 1)
		return;
	peer = user->own_mut_peer;
	vmm_pcie_user_requests_abort(user, ENXIO);
	user->own_mut_peer = NULL;
	if (peer != NULL)
		(void)soclose(peer, 0);
	if (user->fnonce_release != NULL)
		user->fnonce_release(user->borrow_imm_release_arg);
	kfree(user, M_TEMP);
	atomic_subtract_int(&vmm_pcie_user_session_count, 1);
}

int
vmm_pcie_user_mmio_access(struct vmm_pcie_user *user, uint64_t device_id,
    uint64_t attachment_generation, unsigned int bar_index, uint64_t offset,
    int write, int size, uint64_t *valuep)
{
	struct vmm_pcie_mmio_request request;
	uint64_t request_id;
	int error;

	if (user == NULL || valuep == NULL ||
	    user->imm_role != VMM_PCIE_USER_PROVIDER || device_id == 0 ||
	    attachment_generation == 0 || bar_index >= VMM_PCIE_ABI_MAX_BARS ||
	    (size != 1 && size != 2 && size != 4))
		return EINVAL;
	bzero(&request, sizeof(request));
	lwkt_gettoken(&user->token_requests);
	if (user->own_mut_peer == NULL || user->mut_next_request_id == 0) {
		lwkt_reltoken(&user->token_requests);
		return ENXIO;
	}
	request_id = user->mut_next_request_id++;
	request.own_mut_message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	request.own_mut_message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	request.own_mut_message.header.le_type = htole16(
	    VMM_PCIE_ABI_MSG_MMIO_REQUEST);
	request.own_mut_message.header.le_size = htole32(sizeof(
	    request.own_mut_message));
	request.own_mut_message.header.le_flags = htole32(write ?
	    VMM_PCIE_ABI_MMIO_F_WRITE : 0);
	request.own_mut_message.header.le_sequence = htole64(request_id);
	request.own_mut_message.le_device_id = htole64(device_id);
	request.own_mut_message.le_attachment_generation = htole64(
	    attachment_generation);
	request.own_mut_message.le_request_id = htole64(request_id);
	request.own_mut_message.le_offset = htole64(offset);
	request.own_mut_message.le_value = htole64(*valuep);
	request.own_mut_message.le_bar_index = htole32(bar_index);
	request.own_mut_message.le_size = htole32(size);
	TAILQ_INSERT_TAIL(&user->mut_requests, &request, own_mut_entry);
	wakeup(user);
	while (!request.mut_done)
		tsleep(&request, 0, "vmmpciemm", 0);
	error = request.mut_error;
	if (error == 0)
		*valuep = request.mut_value;
	lwkt_reltoken(&user->token_requests);
	return error;
}

int
vmm_pcie_user_send_start(struct vmm_pcie_user *user,
    const struct vmm_pcie_abi_start *message)
{
	if (user == NULL || user->imm_role != VMM_PCIE_USER_PROVIDER ||
	    user->own_mut_peer == NULL || message == NULL)
		return EINVAL;
	return vmm_pcie_user_send(user->own_mut_peer, message, sizeof(*message));
}

int
vmm_pcie_user_send_stop(struct vmm_pcie_user *user,
    const struct vmm_pcie_abi_stop *message)
{
	if (user == NULL || user->imm_role != VMM_PCIE_USER_PROVIDER ||
	    user->own_mut_peer == NULL || message == NULL)
		return EINVAL;
	return vmm_pcie_user_send(user->own_mut_peer, message, sizeof(*message));
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
	vmm_pcie_user_release(user);
	kthread_exit();
}

static void
vmm_pcie_user_provider_run(struct vmm_pcie_user *user)
{
	union {
		struct vmm_pcie_abi_register register_message;
		struct vmm_pcie_abi_msix msix_message;
		struct vmm_pcie_abi_stopped stopped_message;
		struct vmm_pcie_abi_mmio mmio_message;
	} request;
	struct vmm_pcie_mmio_request *mmio;
	size_t size;
	int error;

	error = 0;
	for (;;) {
		lwkt_gettoken(&user->token_requests);
		mmio = TAILQ_FIRST(&user->mut_requests);
		if (mmio != NULL)
			TAILQ_REMOVE(&user->mut_requests, mmio, own_mut_entry);
		lwkt_reltoken(&user->token_requests);
		if (mmio != NULL) {
			error = vmm_pcie_user_send(user->own_mut_peer,
			    &mmio->own_mut_message, sizeof(mmio->own_mut_message));
			if (error != 0) {
				lwkt_gettoken(&user->token_requests);
				mmio->mut_error = error;
				mmio->mut_done = 1;
				wakeup(mmio);
				lwkt_reltoken(&user->token_requests);
				break;
			}
			for (;;) {
				error = vmm_pcie_user_receive(user->own_mut_peer, &request,
				    sizeof(request), &size);
				if (error != 0 || size == 0) {
					if (error == 0)
						error = ECONNRESET;
					break;
				}
				if (vmm_pcie_abi_validate(&request, size) != 0) {
					error = EPROTO;
					break;
				}
				if (le16toh(request.mmio_message.header.le_type) ==
				    VMM_PCIE_ABI_MSG_MMIO_RESPONSE) {
					if (le64toh(request.mmio_message.le_request_id) !=
					    le64toh(mmio->own_mut_message.le_request_id) ||
					    request.mmio_message.le_device_id !=
					    mmio->own_mut_message.le_device_id ||
					    request.mmio_message.le_attachment_generation !=
					    mmio->own_mut_message.le_attachment_generation) {
						error = EPROTO;
						break;
					}
					lwkt_gettoken(&user->token_requests);
					mmio->mut_value = le64toh(request.mmio_message.le_value);
					mmio->mut_error = le32toh(request.mmio_message.le_error);
					mmio->mut_done = 1;
					wakeup(mmio);
					lwkt_reltoken(&user->token_requests);
					break;
				}
				vmm_pcie_user_provider_message(user, &request, size, &error);
				if (error != 0) {
					lwkt_gettoken(&user->token_requests);
					mmio->mut_error = error;
					mmio->mut_done = 1;
					wakeup(mmio);
					lwkt_reltoken(&user->token_requests);
					break;
				}
			}
			if (error != 0) {
				lwkt_gettoken(&user->token_requests);
				mmio->mut_error = error;
				mmio->mut_done = 1;
				wakeup(mmio);
				lwkt_reltoken(&user->token_requests);
				break;
			}
			continue;
		}
		error = vmm_pcie_user_receive_nowait(user->own_mut_peer, &request,
		    sizeof(request), &size);
		if (error == EWOULDBLOCK || error == EAGAIN) {
			error = 0;
			tsleep(user, 0, "vmmpcieq", hz / 100 + 1);
			continue;
		}
		if (error != 0 || size == 0) {
			if (error == 0)
				error = ECONNRESET;
			break;
		}
		vmm_pcie_user_provider_message(user, &request, size, &error);
		if (error != 0)
			break;
	}
	vmm_pcie_user_requests_abort(user, error == 0 ? ENXIO : error);
	vmm_pcie_device_provider_detach(user->borrow_imm_device, user);
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

static int
vmm_pcie_user_receive_nowait(struct socket *peer, void *buf, size_t cap,
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
	flags = MSG_DONTWAIT;
	error = so_pru_soreceive(peer, NULL, &uio, NULL, NULL, &flags);
	if (error != 0)
		return error;
	*sizep = cap - uio.uio_resid;
	return 0;
}

static int
vmm_pcie_user_send(struct socket *peer, const void *message, size_t size)
{
	struct iovec iov;
	struct uio uio;

	if (peer == NULL || message == NULL || size == 0)
		return EINVAL;
	iov.iov_base = __DECONST(void *, message);
	iov.iov_len = size;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = size;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = curthread;
	return sosend(peer, NULL, &uio, NULL, NULL, 0, curthread);
}

static void
vmm_pcie_user_requests_abort(struct vmm_pcie_user *user, int error)
{
	struct vmm_pcie_mmio_request *request;

	if (user == NULL)
		return;
	lwkt_gettoken(&user->token_requests);
	while ((request = TAILQ_FIRST(&user->mut_requests)) != NULL) {
		TAILQ_REMOVE(&user->mut_requests, request, own_mut_entry);
		request->mut_error = error;
		request->mut_done = 1;
		wakeup(request);
	}
	lwkt_reltoken(&user->token_requests);
}

static void
vmm_pcie_user_provider_message(struct vmm_pcie_user *user,
    const void *message, size_t size, int *errorp)
{
	const struct vmm_pcie_abi_header *header;
	struct vmm_pcie_abi_registered response;
	struct file *fps[VMM_PCIE_ABI_MAX_BARS + 2];
	unsigned int file_count;
	unsigned int i;

	if (user == NULL || message == NULL || errorp == NULL ||
	    vmm_pcie_abi_validate(message, size) != 0) {
		if (errorp != NULL)
			*errorp = EPROTO;
		return;
	}
	header = message;
	switch (le16toh(header->le_type)) {
	case VMM_PCIE_ABI_MSG_REGISTER:
		file_count = 0;
		*errorp = vmm_pcie_device_provider_register(
		    user->borrow_imm_device, user, message, &response, fps,
		    &file_count);
		if (*errorp == 0)
			*errorp = kern_sendmsg_rights(user->own_mut_peer, &response,
			    sizeof(response), fps, file_count, 0);
		for (i = 0; i < file_count; i++)
			fdrop(fps[i]);
		break;
	case VMM_PCIE_ABI_MSG_STOPPED:
		*errorp = vmm_pcie_device_provider_stopped(
		    user->borrow_imm_device, user, message);
		break;
	case VMM_PCIE_ABI_MSG_MSIX:
		*errorp = vmm_pcie_device_provider_msix(
		    user->borrow_imm_device, user, message);
		break;
	default:
		*errorp = EPROTO;
		break;
	}
}
