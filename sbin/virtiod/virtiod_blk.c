/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Modern virtio-blk-pci provider for a regular raw image.
 */
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/vmm_pcie_abi.h>

#include "virtiod.h"

#define VIRTIOD_BAR_SIZE 0x8000U
#define VIRTIOD_COMMON_OFFSET 0x1000U
#define VIRTIOD_NOTIFY_OFFSET 0x2000U
#define VIRTIOD_ISR_OFFSET 0x3000U
#define VIRTIOD_DEVICE_OFFSET 0x4000U

#define VIRTIOD_VIRTIO_PCI_CAP_COMMON_CFG 1U
#define VIRTIOD_VIRTIO_PCI_CAP_NOTIFY_CFG 2U
#define VIRTIOD_VIRTIO_PCI_CAP_ISR_CFG 3U
#define VIRTIOD_VIRTIO_PCI_CAP_DEVICE_CFG 4U
#define VIRTIOD_F_VERSION_1 0x00000001U
#define VIRTIOD_BLK_F_FLUSH (1U << 9)
#define VIRTIOD_BLK_F_MQ (1U << 12)
#define VIRTIOD_BLK_F_RO (1U << 5)
#define VIRTIOD_STATUS_DRIVER_OK 0x04U
#define VIRTIOD_STATUS_NEEDS_RESET 0x40U
#define VIRTIOD_VRING_AVAIL_F_NO_INTERRUPT 0x0001U
#define VIRTIOD_MSI_NO_VECTOR 0xffffU

struct virtiod_common_config {
	uint32_t le_device_feature_select;
	uint32_t le_device_feature;
	uint32_t le_driver_feature_select;
	uint32_t le_driver_feature;
	uint16_t le_msix_config;
	uint16_t le_num_queues;
	uint8_t device_status;
	uint8_t config_generation;
	uint16_t le_queue_select;
	uint16_t le_queue_size;
	uint16_t le_queue_msix_vector;
	uint16_t le_queue_enable;
	uint16_t le_queue_notify_off;
	uint64_t le_queue_desc;
	uint64_t le_queue_driver;
	uint64_t le_queue_device;
} __attribute__((__packed__));

struct virtiod_block_header {
	uint32_t le_type;
	uint32_t le_reserved;
	uint64_t le_sector;
} __attribute__((__packed__));

struct virtiod_block_config {
	uint64_t le_capacity;
	uint32_t le_size_max;
	uint32_t le_seg_max;
	uint16_t le_cylinders;
	uint8_t heads;
	uint8_t sectors;
	uint32_t le_blk_size;
	uint8_t physical_block_exp;
	uint8_t alignment_offset;
	uint16_t le_min_io_size;
	uint32_t le_opt_io_size;
	uint8_t writeback;
	uint8_t unused0;
	uint16_t le_num_queues;
} __attribute__((__packed__));

struct virtiod_state;

struct virtiod_queue {
	struct virtiod_state *borrow_mut_state;
	struct virtiod_vring own_mut_vring;
	pthread_cond_t own_cond;
	pthread_mutex_t own_mutex;
	pthread_t own_thread;
	uint16_t atomic_mut_msix_vector;
	int mut_ready;
	int mut_kicked;
	int mut_running;
};

struct virtiod_state {
	int own_provider_fd;
	int own_bar_fd;
	int own_dma_fd;
	int own_event_fd;
	struct virtiod_block own_block;
	struct vmm_pcie_abi_start own_start;
	struct vmm_pcie_abi_registered own_registered;
	struct virtiod_dma_segment own_dma[VIRTIOD_MAX_DMA_SEGMENTS];
	struct virtiod_queue own_mut_queues[VIRTIOD_MAX_QUEUES];
	struct virtiod_common_config own_mut_common;
	uint8_t	atomic_mut_isr;
	uint8_t *own_mut_bar;
	uint32_t mut_driver_features[2];
	uint8_t mut_last_device_status;
	unsigned int mut_dma_count;
	unsigned int imm_queue_count;
	int mut_needs_reset;
};

static void virtiod_build_register(struct vmm_pcie_abi_register *, uint64_t,
    unsigned int);
static int virtiod_receive_start(int, struct vmm_pcie_abi_start *);
static int virtiod_receive_registered(int, const struct vmm_pcie_abi_start *,
    struct vmm_pcie_abi_registered *, int *, int *, int *);
static int virtiod_send_stopped(const struct virtiod_state *);
static int virtiod_send_msix(const struct virtiod_state *, uint16_t);
static int virtiod_map_dma(struct virtiod_state *);
static void virtiod_unmap_dma(struct virtiod_state *);
static void virtiod_initialize_bar(struct virtiod_state *);
static int virtiod_sync_queue(struct virtiod_state *);
static void *virtiod_queue_main(void *);
static int virtiod_process_queue(struct virtiod_state *, struct virtiod_queue *);
static int virtiod_process_chain(struct virtiod_state *, struct virtiod_queue *,
    const struct virtiod_chain *);
static int virtiod_queue_interrupt_disabled(const struct virtiod_state *,
    const struct virtiod_queue *);
static int virtiod_handle_mmio(struct virtiod_state *,
    const struct vmm_pcie_abi_mmio *);
static int virtiod_send_mmio_response(const struct virtiod_state *,
    const struct vmm_pcie_abi_mmio *, uint64_t, int);
static int virtiod_header_valid(const struct vmm_pcie_abi_header *, uint16_t,
    size_t);
static void virtiod_generation_fini(struct virtiod_state *);
static void virtiod_state_fini(struct virtiod_state *);

int
virtiod_blk_run(const struct virtiod_device *device)
{
	struct vmm_pcie_abi_register register_message;
	struct kevent change[2];
	struct kevent ready;
	struct virtiod_state state;
	char provider_path[1024];
	int error;

	if (device == NULL || device->imm_type != VIRTIOD_DEVICE_BLK)
		return EINVAL;
	memset(&state, 0, sizeof(state));
	state.own_provider_fd = -1;
	state.own_bar_fd = -1;
	state.own_dma_fd = -1;
	state.own_event_fd = -1;
	state.own_block.own_fd = -1;
	if (snprintf(provider_path, sizeof(provider_path), "%s/provider",
	    device->imm_slot_path)
	    >= (int)sizeof(provider_path))
		return ENAMETOOLONG;
	error = virtiod_block_open(&state.own_block, device->imm_path);
	if (error != 0)
		errno = error, err(1, "open raw image");
	state.own_provider_fd = open(provider_path, O_RDWR);
	if (state.own_provider_fd < 0)
		err(1, "open %s", provider_path);

restart:
	memset(&state.own_registered, 0, sizeof(state.own_registered));
	memset(state.own_dma, 0, sizeof(state.own_dma));
	memset(state.own_mut_queues, 0, sizeof(state.own_mut_queues));
	memset(&state.own_mut_common, 0, sizeof(state.own_mut_common));
	memset(state.mut_driver_features, 0, sizeof(state.mut_driver_features));
	__atomic_store_n(&state.atomic_mut_isr, 0, __ATOMIC_RELEASE);
	state.mut_last_device_status = 0;
	state.mut_needs_reset = 0;
	error = virtiod_receive_start(state.own_provider_fd, &state.own_start);
	if (error != 0) {
		virtiod_state_fini(&state);
		return error == ECONNRESET ? 0 : error;
	}
	state.imm_queue_count = device->imm_queue_count;
	virtiod_build_register(&register_message,
	    le64toh(state.own_start.header.le_sequence), state.imm_queue_count);
	if (send(state.own_provider_fd, &register_message,
	    sizeof(register_message), 0) != sizeof(register_message))
		err(1, "send REGISTER");
	error = virtiod_receive_registered(state.own_provider_fd, &state.own_start,
	    &state.own_registered, &state.own_bar_fd, &state.own_dma_fd,
	    &state.own_event_fd);
	if (error != 0)
		errno = error, err(1, "recv REGISTERED");
	state.own_mut_bar = mmap(NULL, VIRTIOD_BAR_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, state.own_bar_fd, 0);
	if (state.own_mut_bar == MAP_FAILED)
		err(1, "mmap BAR");
	virtiod_initialize_bar(&state);
	error = virtiod_map_dma(&state);
	if (error != 0)
		errno = error, err(1, "mmap DMA");
	for (unsigned int i = 0; i < state.imm_queue_count; i++) {
		struct virtiod_queue *queue = &state.own_mut_queues[i];

		memset(queue, 0, sizeof(*queue));
		queue->borrow_mut_state = &state;
		queue->mut_running = 1;
		if ((error = pthread_mutex_init(&queue->own_mutex, NULL)) != 0 ||
		    (error = pthread_cond_init(&queue->own_cond, NULL)) != 0 ||
		    (error = pthread_create(&queue->own_thread, NULL,
		    virtiod_queue_main, queue)) != 0) {
			errno = error;
			err(1, "create block queue worker");
		}
	}
	printf("virtiod: ready slot=%s raw=%s capacity=%ju\n",
	    device->imm_slot_path, device->imm_path,
	    (uintmax_t)(state.own_block.imm_size / 512U));
	if (fflush(stdout) != 0)
		err(1, "flush ready");
	{
		int kq;

		kq = kqueue();
		if (kq < 0)
			err(1, "kqueue");
		EV_SET(&change[0], state.own_provider_fd, EVFILT_READ, EV_ADD, 0,
		    0, NULL);
		EV_SET(&change[1], state.own_event_fd, EVFILT_READ, EV_ADD, 0, 0,
		    NULL);
		if (kevent(kq, change, 2, NULL, 0, NULL) != 0)
			err(1, "kevent register");
	for (;;) {
		int result;

		result = kevent(kq, NULL, 0, &ready, 1, NULL);
		if (result < 0)
			err(1, "kevent wait");
		if (ready.ident == (uintptr_t)state.own_event_fd) {
			uint64_t sequence;

			if (read(state.own_event_fd, &sequence, sizeof(sequence)) !=
			    sizeof(sequence))
				err(1, "read doorbell");
			for (unsigned int i = 0; i < state.imm_queue_count; i++) {
				struct virtiod_queue *queue = &state.own_mut_queues[i];

				pthread_mutex_lock(&queue->own_mutex);
				if (queue->mut_ready) {
					queue->mut_kicked = 1;
					pthread_cond_signal(&queue->own_cond);
				}
				pthread_mutex_unlock(&queue->own_mutex);
			}
			continue;
		}
		if (ready.ident == (uintptr_t)state.own_provider_fd) {
			union {
				struct vmm_pcie_abi_stop stop;
				struct vmm_pcie_abi_mmio mmio;
			} message;
			ssize_t size;

			size = recv(state.own_provider_fd, &message, sizeof(message), 0);
			if (size == 0) {
				virtiod_state_fini(&state);
				return 0;
			}
			if (size < 0)
				err(1, "recv provider");
			if (size == sizeof(message.stop) &&
			    virtiod_header_valid(&message.stop.header,
			    VMM_PCIE_ABI_MSG_STOP, sizeof(message.stop)))
				break;
			if (size == sizeof(message.mmio) &&
			    virtiod_header_valid(&message.mmio.header,
			    VMM_PCIE_ABI_MSG_MMIO_REQUEST, sizeof(message.mmio))) {
				error = virtiod_handle_mmio(&state, &message.mmio);
				if (error != 0)
					errno = error, err(1, "handle MMIO");
				continue;
			}
			errno = EPROTO;
			err(1, "provider message");
		}
	}
	(void)close(kq);
	}
	virtiod_generation_fini(&state);
	error = virtiod_send_stopped(&state);
	if (error != 0) {
		virtiod_state_fini(&state);
		return error;
	}
	goto restart;
}

static void
virtiod_build_register(struct vmm_pcie_abi_register *message,
    uint64_t generation, unsigned int queue_count)
{
	struct vmm_pcie_abi_vendor_cap *cap;

	memset(message, 0, sizeof(*message));
	message->header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message->header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message->header.le_type = htole16(VMM_PCIE_ABI_MSG_REGISTER);
	message->header.le_size = htole32(sizeof(*message));
	message->header.le_flags = htole32(VMM_PCIE_ABI_REGISTER_F_MSIX);
	message->header.le_sequence = htole64(generation);
	message->le_vendor_id = htole16(0x1af4);
	message->le_device_id = htole16(0x1042);
	message->le_subsystem_vendor_id = htole16(0x1af4);
	message->le_subsystem_device_id = htole16(0x0002);
	message->le_class_code = htole32(0x010000);
	message->revision = 1;
	/* Linux allocates one config and one queue vector even for one queue. */
	message->le_msix_vectors = htole16((uint16_t)(queue_count + 1));
	message->bar[0].le_size = htole64(VIRTIOD_BAR_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_64BIT);
	message->bar_range_count = 6;
	message->bar_range[0].le_offset = htole64(0);
	message->bar_range[0].le_size = htole64(0x1000);
	message->bar_range[0].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->bar_range[1].le_offset = htole64(VIRTIOD_COMMON_OFFSET);
	message->bar_range[1].le_size = htole64(0x1000);
	message->bar_range[1].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[2].le_offset = htole64(VIRTIOD_NOTIFY_OFFSET);
	message->bar_range[2].le_size = htole64(0x1000);
	message->bar_range[2].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DOORBELL);
	message->bar_range[3].le_offset = htole64(VIRTIOD_ISR_OFFSET);
	message->bar_range[3].le_size = htole64(0x1000);
	message->bar_range[3].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[4].le_offset = htole64(VIRTIOD_DEVICE_OFFSET);
	message->bar_range[4].le_size = htole64(0x1000);
	message->bar_range[4].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[5].le_offset = htole64(0x5000);
	message->bar_range[5].le_size = htole64(VIRTIOD_BAR_SIZE - 0x5000);
	message->bar_range[5].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->vendor_cap_count = 4;
	cap = &message->vendor_cap[0];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_VIRTIO_PCI_CAP_COMMON_CFG;
	cap->bytes[5] = VIRTIOD_COMMON_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_COMMON_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_common_config);
	cap = &message->vendor_cap[1];
	cap->length = 20;
	cap->bytes[0] = VIRTIOD_VIRTIO_PCI_CAP_NOTIFY_CFG;
	cap->bytes[5] = VIRTIOD_NOTIFY_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_NOTIFY_OFFSET >> 8;
	/* virtio_pci_cap.length: the complete direct notify BAR page. */
	cap->bytes[9] = 0;
	cap->bytes[10] = 0x10;
	cap->bytes[13] = 4;
	cap = &message->vendor_cap[2];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_VIRTIO_PCI_CAP_ISR_CFG;
	cap->bytes[5] = VIRTIOD_ISR_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_ISR_OFFSET >> 8;
	cap->bytes[9] = 1;
	cap = &message->vendor_cap[3];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_VIRTIO_PCI_CAP_DEVICE_CFG;
	cap->bytes[5] = VIRTIOD_DEVICE_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_DEVICE_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_block_config);
}

static int
virtiod_receive_start(int fd, struct vmm_pcie_abi_start *message)
{
	ssize_t size;

	size = recv(fd, message, sizeof(*message), 0);
	if (size == 0)
		return ECONNRESET;
	if (size != sizeof(*message))
		return size < 0 ? errno : EPROTO;
	if (!virtiod_header_valid(&message->header, VMM_PCIE_ABI_MSG_START,
	    sizeof(*message)) || le32toh(message->le_dma_segment_count) == 0 ||
	    le32toh(message->le_dma_segment_count) > VIRTIOD_MAX_DMA_SEGMENTS)
		return EPROTO;
	return 0;
}

static int
virtiod_receive_registered(int fd, const struct vmm_pcie_abi_start *start,
    struct vmm_pcie_abi_registered *message, int *bar_fdp, int *dma_fdp,
    int *event_fdp)
{
	char control[CMSG_SPACE(sizeof(int) * 3)];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr socket_message;
	int fds[3];
	ssize_t size;

	memset(control, 0, sizeof(control));
	memset(&socket_message, 0, sizeof(socket_message));
	iov.iov_base = message;
	iov.iov_len = sizeof(*message);
	socket_message.msg_iov = &iov;
	socket_message.msg_iovlen = 1;
	socket_message.msg_control = control;
	socket_message.msg_controllen = sizeof(control);
	size = recvmsg(fd, &socket_message, 0);
	if (size != sizeof(*message) ||
	    (socket_message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
		warnx("REGISTERED size=%zd flags=%#x", size,
		    socket_message.msg_flags);
		return size < 0 ? errno : EPROTO;
	}
	if (!virtiod_header_valid(&message->header, VMM_PCIE_ABI_MSG_REGISTERED,
	    sizeof(*message)) || message->header.le_sequence !=
	    start->header.le_sequence || le32toh(message->le_bar_fd_mask) != 1 ||
	    (le32toh(message->header.le_flags) &
	    (VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY |
	    VMM_PCIE_ABI_REGISTERED_F_EVENT_CAPABILITY)) !=
	    (VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY |
	    VMM_PCIE_ABI_REGISTERED_F_EVENT_CAPABILITY)) {
		warnx("REGISTERED magic=%#x version=%u type=%u size=%u flags=%#x "
		    "sequence=%ju expected=%ju bars=%#x", le32toh(message->header.le_magic),
		    le16toh(message->header.le_version),
		    le16toh(message->header.le_type), le32toh(message->header.le_size),
		    le32toh(message->header.le_flags),
		    (uintmax_t)le64toh(message->header.le_sequence),
		    (uintmax_t)le64toh(start->header.le_sequence),
		    le32toh(message->le_bar_fd_mask));
		return EPROTO;
	}
	cmsg = CMSG_FIRSTHDR(&socket_message);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len !=
	    CMSG_LEN(sizeof(fds)) || CMSG_NXTHDR(&socket_message, cmsg) != NULL) {
		warnx("REGISTERED rights control=%zu level=%d type=%d length=%zu",
		    (size_t)socket_message.msg_controllen,
		    cmsg == NULL ? -1 : cmsg->cmsg_level,
		    cmsg == NULL ? -1 : cmsg->cmsg_type,
		    cmsg == NULL ? 0 : (size_t)cmsg->cmsg_len);
		return EPROTO;
	}
	memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
	if (bar_fdp == NULL || dma_fdp == NULL || event_fdp == NULL ||
	    fds[0] < 0 || fds[1] < 0 || fds[2] < 0)
		return EPROTO;
	*bar_fdp = fds[0];
	*dma_fdp = fds[1];
	*event_fdp = fds[2];
	return 0;
}

static int
virtiod_send_stopped(const struct virtiod_state *state)
{
	struct vmm_pcie_abi_stopped message;

	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_STOPPED);
	message.header.le_size = htole32(sizeof(message));
	message.header.le_sequence = state->own_start.header.le_sequence;
	message.le_device_id = state->own_registered.le_device_id;
	message.le_memory_generation = state->own_start.le_memory_generation;
	return send(state->own_provider_fd, &message, sizeof(message), 0) ==
	    sizeof(message) ? 0 : errno;
}

static int
virtiod_send_msix(const struct virtiod_state *state, uint16_t vector)
{
	struct vmm_pcie_abi_msix message;

	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_MSIX);
	message.header.le_size = htole32(sizeof(message));
	message.le_device_id = state->own_registered.le_device_id;
	message.le_attachment_generation =
	    state->own_registered.le_attachment_generation;
	message.le_vector = htole16(vector);
	return write(state->own_event_fd, &message, sizeof(message)) ==
	    sizeof(message) ? 0 : errno;
}

static int
virtiod_map_dma(struct virtiod_state *state)
{
	unsigned int i;

	state->mut_dma_count = le32toh(state->own_start.le_dma_segment_count);
	for (i = 0; i < state->mut_dma_count; i++) {
		struct vmm_pcie_abi_dma_segment *source;
		void *mapping;

		source = &state->own_start.dma_segment[i];
		state->own_dma[i].imm_gpa = le64toh(source->le_gpa);
		state->own_dma[i].imm_size = le64toh(source->le_length);
		if (state->own_dma[i].imm_size == 0 ||
		    (state->own_dma[i].imm_gpa & (VMM_PCIE_ABI_PAGE_SIZE - 1U)) != 0 ||
		    (state->own_dma[i].imm_size & (VMM_PCIE_ABI_PAGE_SIZE - 1U)) != 0)
			return EPROTO;
		mapping = mmap(NULL, state->own_dma[i].imm_size,
		    PROT_READ | PROT_WRITE, MAP_SHARED, state->own_dma_fd,
		    (off_t)state->own_dma[i].imm_gpa);
		if (mapping == MAP_FAILED)
			return errno;
		state->own_dma[i].own_mut_bytes = mapping;
	}
	return 0;
}

static void
virtiod_unmap_dma(struct virtiod_state *state)
{
	unsigned int i;

	for (i = 0; i < state->mut_dma_count; i++) {
		if (state->own_dma[i].own_mut_bytes != NULL)
			(void)munmap(state->own_dma[i].own_mut_bytes,
			    state->own_dma[i].imm_size);
		state->own_dma[i].own_mut_bytes = NULL;
	}
	state->mut_dma_count = 0;
}

static void
virtiod_initialize_bar(struct virtiod_state *state)
{
	struct virtiod_common_config *common;
	uint64_t capacity;

	common = &state->own_mut_common;
	__atomic_store_n(&common->le_msix_config, htole16(VIRTIOD_MSI_NO_VECTOR),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_num_queues, htole16(1), __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_queue_size, htole16(VIRTIOD_QUEUE_SIZE),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_queue_msix_vector,
	    htole16(VIRTIOD_MSI_NO_VECTOR), __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_queue_notify_off, htole16(0),
	    __ATOMIC_RELEASE);
	capacity = htole64(state->own_block.imm_size / 512U);
	(void)capacity;
}

static int
virtiod_sync_queue(struct virtiod_state *state)
{
	struct virtiod_common_config *common;
	struct virtiod_queue *queue;
	uint32_t selector;
	uint32_t features;
	uint16_t queue_index;
	uint16_t queue_size;
	uint8_t status;
	uint64_t capacity;
	int error;

	common = &state->own_mut_common;
	status = __atomic_load_n(&common->device_status, __ATOMIC_ACQUIRE);
	if (status == 0 && state->mut_last_device_status != 0) {
		memset(state->mut_driver_features, 0,
		    sizeof(state->mut_driver_features));
		for (unsigned int i = 0; i < state->imm_queue_count; i++) {
			queue = &state->own_mut_queues[i];
			pthread_mutex_lock(&queue->own_mutex);
			memset(&queue->own_mut_vring, 0, sizeof(queue->own_mut_vring));
			queue->mut_ready = 0;
			pthread_mutex_unlock(&queue->own_mutex);
		}
		state->mut_needs_reset = 0;
	}
	state->mut_last_device_status = status;
	if (state->mut_needs_reset)
		return 0;
	selector = le32toh(__atomic_load_n(&common->le_device_feature_select,
	    __ATOMIC_ACQUIRE));
	features = 0;
	if (selector == 0) {
		features = VIRTIOD_BLK_F_FLUSH;
		if (state->imm_queue_count > 1)
			features |= VIRTIOD_BLK_F_MQ;
		if (state->own_block.imm_read_only)
			features |= VIRTIOD_BLK_F_RO;
	} else if (selector == 1) {
		features = VIRTIOD_F_VERSION_1;
	}
	__atomic_store_n(&common->le_device_feature, htole32(features),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_num_queues,
	    htole16((uint16_t)state->imm_queue_count), __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_queue_size, htole16(VIRTIOD_QUEUE_SIZE),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_queue_notify_off, htole16(0),
	    __ATOMIC_RELEASE);
	capacity = htole64(state->own_block.imm_size / 512U);
	(void)capacity;
	if ((status & VIRTIOD_STATUS_DRIVER_OK) == 0 || le16toh(__atomic_load_n(
	    &common->le_queue_enable, __ATOMIC_ACQUIRE)) == 0) {
		for (unsigned int i = 0; i < state->imm_queue_count; i++) {
			queue = &state->own_mut_queues[i];
			pthread_mutex_lock(&queue->own_mutex);
			queue->mut_ready = 0;
			pthread_mutex_unlock(&queue->own_mutex);
		}
		return 0;
	}
	queue_index = le16toh(__atomic_load_n(&common->le_queue_select,
	    __ATOMIC_ACQUIRE));
	if (queue_index >= state->imm_queue_count)
		return EPROTO;
	queue = &state->own_mut_queues[queue_index];
	if ((state->mut_driver_features[1] & VIRTIOD_F_VERSION_1) == 0)
		return EPROTO;
	queue_size = le16toh(__atomic_load_n(&common->le_queue_size,
	    __ATOMIC_ACQUIRE));
	if (queue_size == 0)
		queue_size = VIRTIOD_QUEUE_SIZE;
	if (queue_size != VIRTIOD_QUEUE_SIZE)
		return EPROTO;
	pthread_mutex_lock(&queue->own_mutex);
	__atomic_store_n(&queue->atomic_mut_msix_vector, le16toh(__atomic_load_n(
	    &common->le_queue_msix_vector, __ATOMIC_ACQUIRE)), __ATOMIC_RELEASE);
	if (!queue->mut_ready) {
		error = virtiod_vring_configure(&queue->own_mut_vring, state->own_dma,
		    state->mut_dma_count, queue_size, le64toh(__atomic_load_n(
	    &common->le_queue_desc, __ATOMIC_ACQUIRE)), le64toh(__atomic_load_n(
	    &common->le_queue_driver, __ATOMIC_ACQUIRE)), le64toh(__atomic_load_n(
	    &common->le_queue_device, __ATOMIC_ACQUIRE)));
		if (error != 0) {
			pthread_mutex_unlock(&queue->own_mutex);
			return error;
		}
		queue->mut_ready = 1;
		queue->mut_kicked = 1;
		pthread_cond_signal(&queue->own_cond);
	}
	pthread_mutex_unlock(&queue->own_mutex);
	return 0;
}

static void *
virtiod_queue_main(void *argument)
{
	struct virtiod_queue *queue;
	struct virtiod_state *state;

	queue = argument;
	state = queue->borrow_mut_state;
	pthread_mutex_lock(&queue->own_mutex);
	while (queue->mut_running) {
		while (queue->mut_running && (!queue->mut_ready ||
		    !queue->mut_kicked))
			pthread_cond_wait(&queue->own_cond, &queue->own_mutex);
		if (!queue->mut_running)
			break;
		queue->mut_kicked = 0;
		if (virtiod_process_queue(state, queue) != 0) {
			__atomic_fetch_or(&state->own_mut_common.device_status,
			    VIRTIOD_STATUS_NEEDS_RESET, __ATOMIC_RELEASE);
			queue->mut_ready = 0;
		}
		pthread_mutex_unlock(&queue->own_mutex);
		pthread_mutex_lock(&queue->own_mutex);
	}
	pthread_mutex_unlock(&queue->own_mutex);
	return NULL;
}

static int
virtiod_process_queue(struct virtiod_state *state, struct virtiod_queue *queue)
{
	struct virtiod_chain chain;
	int error;

	for (;;) {
		error = virtiod_vring_pop(&queue->own_mut_vring, &chain);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
		error = virtiod_process_chain(state, queue, &chain);
		if (error != 0)
			return error;
	}
}

static int
virtiod_process_chain(struct virtiod_state *state, struct virtiod_queue *queue,
    const struct virtiod_chain *chain)
{
	struct virtiod_block_header header;
	uint8_t *status;
	uint32_t type;
	uint32_t used_length;
	unsigned int data_count;
	unsigned int i;
	int error;

	if (chain->mut_iov_count < 2 ||
	    chain->own_mut_iov[0].iov_len != sizeof(header) ||
	    (chain->own_mut_flags[0] & VIRTIOD_DESC_F_WRITE) != 0 ||
	    chain->own_mut_iov[chain->mut_iov_count - 1U].iov_len != 1 ||
	    (chain->own_mut_flags[chain->mut_iov_count - 1U] &
	    VIRTIOD_DESC_F_WRITE) == 0)
		return EPROTO;
	memcpy(&header, chain->own_mut_iov[0].iov_base, sizeof(header));
	type = le32toh(header.le_type);
	data_count = chain->mut_iov_count - 2U;
	for (i = 0; i < data_count; i++) {
		int writable = (chain->own_mut_flags[i + 1U] &
		    VIRTIOD_DESC_F_WRITE) != 0;

		if ((type == VIRTIOD_BLK_T_IN && !writable) ||
		    (type == VIRTIOD_BLK_T_OUT && writable) ||
		    (type == VIRTIOD_BLK_T_FLUSH && data_count != 0))
			return EPROTO;
	}
	status = chain->own_mut_iov[chain->mut_iov_count - 1U].iov_base;
	error = virtiod_block_request(&state->own_block, type,
	    le64toh(header.le_sector), &chain->own_mut_iov[1], data_count, status);
	if (error != 0)
		return error;
	used_length = 1;
	if (type == VIRTIOD_BLK_T_IN && *status == VIRTIOD_BLK_S_OK) {
		for (i = 0; i < data_count; i++)
			used_length += chain->own_mut_iov[i + 1U].iov_len;
	}
	error = virtiod_vring_complete(&queue->own_mut_vring, chain, used_length);
	if (error != 0)
		return error;
	__atomic_store_n(&state->atomic_mut_isr, 1, __ATOMIC_RELEASE);
	if (!virtiod_queue_interrupt_disabled(state, queue)) {
		uint16_t vector;

		vector = __atomic_load_n(&queue->atomic_mut_msix_vector,
		    __ATOMIC_ACQUIRE);
		if (vector == VIRTIOD_MSI_NO_VECTOR)
			return 0;
		if (vector >= state->imm_queue_count + 1)
			return EPROTO;
		return virtiod_send_msix(state, vector);
	}
	return 0;
}

static int
virtiod_queue_interrupt_disabled(const struct virtiod_state *state,
    const struct virtiod_queue *queue)
{
	uint16_t *available;
	void *pointer;
	int error;

	error = virtiod_dma_translate(state->own_dma, state->mut_dma_count,
	    queue->own_mut_vring.mut_avail_gpa, sizeof(*available), &pointer);
	if (error != 0)
		return 0;
	available = pointer;
	return (le16toh(__atomic_load_n(available, __ATOMIC_ACQUIRE)) &
	    VIRTIOD_VRING_AVAIL_F_NO_INTERRUPT) != 0;
}

static int
virtiod_handle_mmio(struct virtiod_state *state,
    const struct vmm_pcie_abi_mmio *request)
{
	struct virtiod_block_config config;
	uint64_t value;
	uint64_t offset;
	uint32_t flags;
	uint32_t size;
	int error;

	if (request->le_device_id != state->own_registered.le_device_id ||
	    request->le_attachment_generation !=
	    state->own_registered.le_attachment_generation)
		return EPROTO;
	offset = le64toh(request->le_offset);
	value = le64toh(request->le_value);
	flags = le32toh(request->header.le_flags);
	size = le32toh(request->le_size);
	error = 0;
	if (le32toh(request->le_bar_index) != 0)
		error = EINVAL;
	else if (offset >= VIRTIOD_COMMON_OFFSET &&
	    offset - VIRTIOD_COMMON_OFFSET <= sizeof(state->own_mut_common) &&
	    size <= sizeof(state->own_mut_common) -
	    (offset - VIRTIOD_COMMON_OFFSET)) {
		uint8_t *bytes;

		bytes = (uint8_t *)(void *)&state->own_mut_common;
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0) {
			uint32_t selector;

			memcpy(bytes + offset - VIRTIOD_COMMON_OFFSET, &value, size);
			if (offset == VIRTIOD_COMMON_OFFSET + 12U && size == 4) {
				selector = le32toh(state->own_mut_common.le_driver_feature_select);
				if (selector < 2)
					state->mut_driver_features[selector] = le32toh(
					    state->own_mut_common.le_driver_feature);
			}
		}
		if (virtiod_sync_queue(state) != 0) {
			state->mut_needs_reset = 1;
			for (unsigned int i = 0; i < state->imm_queue_count; i++) {
				struct virtiod_queue *queue = &state->own_mut_queues[i];

				pthread_mutex_lock(&queue->own_mutex);
				queue->mut_ready = 0;
				pthread_mutex_unlock(&queue->own_mutex);
			}
			state->own_mut_common.device_status |= VIRTIOD_STATUS_NEEDS_RESET;
		}
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) == 0) {
			value = 0;
			memcpy(&value, bytes + offset - VIRTIOD_COMMON_OFFSET, size);
		}
	} else if (offset == VIRTIOD_ISR_OFFSET && size == 1) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else {
			value = __atomic_exchange_n(&state->atomic_mut_isr, 0,
			    __ATOMIC_ACQ_REL);
		}
	} else if (offset >= VIRTIOD_DEVICE_OFFSET &&
	    offset - VIRTIOD_DEVICE_OFFSET <= sizeof(config) &&
	    size <= sizeof(config) &&
	    size <= sizeof(config) - (offset - VIRTIOD_DEVICE_OFFSET)) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else {
			memset(&config, 0, sizeof(config));
			config.le_capacity = htole64(state->own_block.imm_size / 512U);
			config.le_num_queues = htole16((uint16_t)state->imm_queue_count);
			value = 0;
			memcpy(&value, (const uint8_t *)(const void *)&config +
			    offset - VIRTIOD_DEVICE_OFFSET, size);
		}
	} else {
		error = EINVAL;
	}
	return virtiod_send_mmio_response(state, request, value, error);
}

static int
virtiod_send_mmio_response(const struct virtiod_state *state,
    const struct vmm_pcie_abi_mmio *request, uint64_t value, int error)
{
	struct vmm_pcie_abi_mmio response;

	memset(&response, 0, sizeof(response));
	response.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	response.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	response.header.le_type = htole16(VMM_PCIE_ABI_MSG_MMIO_RESPONSE);
	response.header.le_size = htole32(sizeof(response));
	response.header.le_sequence = request->header.le_sequence;
	response.le_device_id = state->own_registered.le_device_id;
	response.le_attachment_generation = request->le_attachment_generation;
	response.le_request_id = request->le_request_id;
	response.le_offset = request->le_offset;
	response.le_value = htole64(value);
	response.le_bar_index = request->le_bar_index;
	response.le_size = request->le_size;
	response.le_error = htole32(error);
	return send(state->own_provider_fd, &response, sizeof(response), 0) ==
	    sizeof(response) ? 0 : errno;
}

static int
virtiod_header_valid(const struct vmm_pcie_abi_header *header, uint16_t type,
    size_t size)
{

	return header != NULL && le32toh(header->le_magic) == VMM_PCIE_ABI_MAGIC &&
	    le16toh(header->le_version) == VMM_PCIE_ABI_VERSION &&
	    le16toh(header->le_type) == type && le32toh(header->le_size) == size;
}

static void
virtiod_generation_fini(struct virtiod_state *state)
{
	unsigned int i;

	for (i = 0; i < state->imm_queue_count; i++) {
		struct virtiod_queue *queue = &state->own_mut_queues[i];

		if (!queue->mut_running)
			continue;
		pthread_mutex_lock(&queue->own_mutex);
		queue->mut_running = 0;
		pthread_cond_signal(&queue->own_cond);
		pthread_mutex_unlock(&queue->own_mutex);
		(void)pthread_join(queue->own_thread, NULL);
		(void)pthread_cond_destroy(&queue->own_cond);
		(void)pthread_mutex_destroy(&queue->own_mutex);
		queue->mut_running = 0;
		queue->mut_ready = 0;
	}

	virtiod_unmap_dma(state);
	if (state->own_mut_bar != NULL && state->own_mut_bar != MAP_FAILED)
		(void)munmap(state->own_mut_bar, VIRTIOD_BAR_SIZE);
	state->own_mut_bar = NULL;
	if (state->own_bar_fd >= 0)
		(void)close(state->own_bar_fd);
	if (state->own_dma_fd >= 0)
		(void)close(state->own_dma_fd);
	if (state->own_event_fd >= 0)
		(void)close(state->own_event_fd);
	state->own_bar_fd = -1;
	state->own_dma_fd = -1;
	state->own_event_fd = -1;
}

static void
virtiod_state_fini(struct virtiod_state *state)
{

	virtiod_generation_fini(state);
	if (state->own_provider_fd >= 0)
		(void)close(state->own_provider_fd);
	virtiod_block_close(&state->own_block);
}
