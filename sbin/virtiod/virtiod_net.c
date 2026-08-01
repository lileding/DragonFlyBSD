/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Modern virtio-net-pci provider for a DragonFly TAP interface.  Queue
 * direction and packet handling follow the BSD-2-Clause bhyve virtio-net
 * implementation, adapted to the dfvmm provider ABI.
 */
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/vmm_pcie_abi.h>

#include "virtiod.h"

#define VIRTIOD_NET_BAR_SIZE 0x8000U
#define VIRTIOD_NET_COMMON_OFFSET 0x1000U
#define VIRTIOD_NET_NOTIFY_OFFSET 0x2000U
#define VIRTIOD_NET_ISR_OFFSET 0x3000U
#define VIRTIOD_NET_DEVICE_OFFSET 0x4000U

#define VIRTIOD_NET_CAP_COMMON_CFG 1U
#define VIRTIOD_NET_CAP_NOTIFY_CFG 2U
#define VIRTIOD_NET_CAP_ISR_CFG 3U
#define VIRTIOD_NET_CAP_DEVICE_CFG 4U

#define VIRTIOD_NET_F_VERSION_1 0x00000001U
#define VIRTIOD_NET_F_MAC (1U << 5)
#define VIRTIOD_NET_F_STATUS (1U << 16)
#define VIRTIOD_NET_STATUS_DRIVER_OK 0x04U
#define VIRTIOD_NET_STATUS_NEEDS_RESET 0x40U
#define VIRTIOD_NET_MSI_NO_VECTOR 0xffffU
#define VIRTIOD_NET_QUEUE_RX 0U
#define VIRTIOD_NET_QUEUE_TX 1U
#define VIRTIOD_NET_QUEUE_COUNT 2U
#define VIRTIOD_NET_MSIX_COUNT 3U
#define VIRTIOD_NET_MAX_FRAME 65536U

struct virtiod_net_common_config {
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

struct virtiod_net_config {
	uint8_t mac[6];
	uint16_t le_status;
} __attribute__((__packed__));

struct virtiod_net_header {
	uint8_t flags;
	uint8_t gso_type;
	uint16_t le_hdr_len;
	uint16_t le_gso_size;
	uint16_t le_csum_start;
	uint16_t le_csum_offset;
	uint16_t le_num_buffers;
} __attribute__((__packed__));

struct virtiod_net_queue_config {
	uint16_t le_size;
	uint16_t le_msix_vector;
	uint16_t le_enable;
	uint16_t le_notify_off;
	uint64_t le_desc;
	uint64_t le_driver;
	uint64_t le_device;
};

struct virtiod_net_state {
	int own_provider_fd;
	int own_bar_fd;
	int own_dma_fd;
	struct virtiod_tap own_tap;
	struct vmm_pcie_abi_start own_start;
	struct vmm_pcie_abi_registered own_registered;
	struct virtiod_dma_segment own_dma[VIRTIOD_MAX_DMA_SEGMENTS];
	struct virtiod_vring own_queue[VIRTIOD_NET_QUEUE_COUNT];
	struct virtiod_net_queue_config
	    own_mut_queue_config[VIRTIOD_NET_QUEUE_COUNT];
	struct virtiod_net_common_config own_mut_common;
	struct virtiod_net_config own_mut_config;
	uint8_t *own_mut_bar;
	uint32_t mut_driver_features[2];
	uint8_t mut_isr;
	uint8_t mut_last_device_status;
	unsigned int mut_dma_count;
	int mut_queue_ready[VIRTIOD_NET_QUEUE_COUNT];
	int mut_needs_reset;
	int mut_rx_queue_logged;
	int mut_rx_available_logged;
	int mut_rx_tap_logged;
	int mut_rx_completion_logged;
};

static void virtiod_net_build_register(struct vmm_pcie_abi_register *,
    uint64_t);
static int virtiod_net_receive_start(int, struct vmm_pcie_abi_start *);
static int virtiod_net_receive_registered(int,
    const struct vmm_pcie_abi_start *, struct vmm_pcie_abi_registered *,
    int *, int *);
static int virtiod_net_send_stopped(const struct virtiod_net_state *);
static int virtiod_net_send_msix(const struct virtiod_net_state *, uint16_t);
static int virtiod_net_map_dma(struct virtiod_net_state *);
static void virtiod_net_unmap_dma(struct virtiod_net_state *);
static void virtiod_net_initialize_bar(struct virtiod_net_state *);
static int virtiod_net_sync_queues(struct virtiod_net_state *);
static void virtiod_net_capture_queue(struct virtiod_net_state *, unsigned int);
static void virtiod_net_restore_queue(struct virtiod_net_state *, unsigned int);
static int virtiod_net_process_tx(struct virtiod_net_state *);
static int virtiod_net_process_rx(struct virtiod_net_state *);
static int virtiod_net_complete(struct virtiod_net_state *, unsigned int,
    const struct virtiod_chain *, uint32_t);
static int virtiod_net_interrupt_disabled(const struct virtiod_net_state *,
    unsigned int);
static int virtiod_net_handle_mmio(struct virtiod_net_state *,
    const struct vmm_pcie_abi_mmio *);
static int virtiod_net_send_mmio_response(const struct virtiod_net_state *,
    const struct vmm_pcie_abi_mmio *, uint64_t, int);
static int virtiod_net_parse_mac(const char *, uint8_t[6]);
static int virtiod_net_iov_skip(const struct virtiod_chain *, size_t,
    struct iovec *, int *);
static size_t virtiod_net_iov_limit(struct iovec *, int, size_t);
static int virtiod_net_header_valid(const struct vmm_pcie_abi_header *,
    uint16_t, size_t);
static void virtiod_net_state_fini(struct virtiod_net_state *);

int
virtiod_net_main(int argc, char **argv)
{
	struct vmm_pcie_abi_register register_message;
	struct virtiod_net_state state;
	char provider_path[1024];
	int error;

	if (argc != 3)
		errno = EINVAL, err(1, "usage: virtiod net DEVICE_DIR TAP MAC");
	memset(&state, 0, sizeof(state));
	state.own_provider_fd = -1;
	state.own_bar_fd = -1;
	state.own_dma_fd = -1;
	state.own_tap.own_fd = -1;
	error = virtiod_net_parse_mac(argv[2], state.own_mut_config.mac);
	if (error != 0)
		errno = error, err(1, "parse MAC %s", argv[2]);
	error = virtiod_tap_open(&state.own_tap, argv[1]);
	if (error != 0)
		errno = error, err(1, "open TAP %s", argv[1]);
	if (snprintf(provider_path, sizeof(provider_path), "%s/provider", argv[0])
	    >= (int)sizeof(provider_path))
		errno = ENAMETOOLONG, err(1, "provider path");
	state.own_provider_fd = open(provider_path, O_RDWR);
	if (state.own_provider_fd < 0)
		err(1, "open %s", provider_path);
	error = virtiod_net_receive_start(state.own_provider_fd, &state.own_start);
	if (error != 0)
		errno = error, err(1, "recv START");
	virtiod_net_build_register(&register_message,
	    le64toh(state.own_start.header.le_sequence));
	if (send(state.own_provider_fd, &register_message, sizeof(register_message),
	    0) != sizeof(register_message))
		err(1, "send REGISTER");
	error = virtiod_net_receive_registered(state.own_provider_fd,
	    &state.own_start, &state.own_registered, &state.own_bar_fd,
	    &state.own_dma_fd);
	if (error != 0)
		errno = error, err(1, "recv REGISTERED");
	state.own_mut_bar = mmap(NULL, VIRTIOD_NET_BAR_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, state.own_bar_fd, 0);
	if (state.own_mut_bar == MAP_FAILED)
		err(1, "mmap BAR");
	virtiod_net_initialize_bar(&state);
	error = virtiod_net_map_dma(&state);
	if (error != 0)
		errno = error, err(1, "mmap DMA");
	printf("virtiod: ready net=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
	    argv[1], state.own_mut_config.mac[0], state.own_mut_config.mac[1],
	    state.own_mut_config.mac[2], state.own_mut_config.mac[3],
	    state.own_mut_config.mac[4], state.own_mut_config.mac[5]);
	if (fflush(stdout) != 0)
		err(1, "flush ready");
	for (;;) {
		struct pollfd pollfd;
		size_t rx_length;
		int rx_ready;
		int result;

		error = virtiod_net_sync_queues(&state);
		if (error != 0) {
			warnx("virtio-net queue configuration failed: %s",
			    strerror(error));
			__atomic_fetch_or(&state.own_mut_common.device_status,
			    VIRTIOD_NET_STATUS_NEEDS_RESET, __ATOMIC_RELEASE);
			state.mut_needs_reset = 1;
			memset(state.mut_queue_ready, 0, sizeof(state.mut_queue_ready));
		}
		if (state.mut_queue_ready[VIRTIOD_NET_QUEUE_TX]) {
			error = virtiod_net_process_tx(&state);
			if (error != 0) {
				warnx("virtio-net transmit failed: %s", strerror(error));
				__atomic_fetch_or(&state.own_mut_common.device_status,
				    VIRTIOD_NET_STATUS_NEEDS_RESET, __ATOMIC_RELEASE);
				state.mut_needs_reset = 1;
			}
		}
		rx_ready = state.mut_queue_ready[VIRTIOD_NET_QUEUE_RX] ?
		    virtiod_vring_has_available(&state.own_queue[VIRTIOD_NET_QUEUE_RX]) : 0;
		if (rx_ready < 0)
			rx_ready = 0;
		if (rx_ready != 0) {
			if (!state.mut_rx_available_logged) {
				printf("virtiod: net RX descriptor available\n");
				(void)fflush(stdout);
				state.mut_rx_available_logged = 1;
			}
			rx_length = 0;
			error = virtiod_tap_pending(&state.own_tap, &rx_length);
			if (error != 0)
				errno = error, err(1, "receive TAP frame");
			if (rx_length != 0) {
				if (!state.mut_rx_tap_logged) {
					printf("virtiod: net TAP frame bytes=%zu\n", rx_length);
					(void)fflush(stdout);
					state.mut_rx_tap_logged = 1;
				}
				error = virtiod_net_process_rx(&state);
				if (error != 0)
					errno = error, err(1, "receive TAP frame");
			}
		}
		memset(&pollfd, 0, sizeof(pollfd));
		pollfd.fd = state.own_provider_fd;
		pollfd.events = POLLIN | POLLHUP | POLLERR;
		result = poll(&pollfd, 1, 1);
		if (result < 0)
			err(1, "poll provider");
		if ((pollfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			union {
				struct vmm_pcie_abi_stop stop;
				struct vmm_pcie_abi_mmio mmio;
			} message;
			ssize_t size;

			size = recv(state.own_provider_fd, &message, sizeof(message), 0);
			if (size < 0)
				err(1, "recv provider");
			if (size == sizeof(message.stop) && virtiod_net_header_valid(
			    &message.stop.header, VMM_PCIE_ABI_MSG_STOP,
			    sizeof(message.stop)))
				break;
			if (size == sizeof(message.mmio) && virtiod_net_header_valid(
			    &message.mmio.header, VMM_PCIE_ABI_MSG_MMIO_REQUEST,
			    sizeof(message.mmio))) {
				error = virtiod_net_handle_mmio(&state, &message.mmio);
				if (error != 0)
					errno = error, err(1, "handle MMIO");
				continue;
			}
			errno = EPROTO;
			err(1, "provider message");
		}
	}
	virtiod_net_unmap_dma(&state);
	error = virtiod_net_send_stopped(&state);
	if (error != 0)
		errno = error, err(1, "send STOPPED");
	virtiod_net_state_fini(&state);
	return 0;
}

static void
virtiod_net_build_register(struct vmm_pcie_abi_register *message,
    uint64_t generation)
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
	message->le_device_id = htole16(0x1041);
	message->le_subsystem_vendor_id = htole16(0x1af4);
	message->le_subsystem_device_id = htole16(0x0001);
	message->le_class_code = htole32(0x020000);
	message->revision = 1;
	message->le_msix_vectors = htole16(VIRTIOD_NET_MSIX_COUNT);
	message->bar[0].le_size = htole64(VIRTIOD_NET_BAR_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_64BIT);
	message->bar_range_count = 6;
	message->bar_range[0].le_size = htole64(0x1000);
	message->bar_range[0].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->bar_range[1].le_offset = htole64(VIRTIOD_NET_COMMON_OFFSET);
	message->bar_range[1].le_size = htole64(0x1000);
	message->bar_range[1].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[2].le_offset = htole64(VIRTIOD_NET_NOTIFY_OFFSET);
	message->bar_range[2].le_size = htole64(0x1000);
	message->bar_range[2].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->bar_range[3].le_offset = htole64(VIRTIOD_NET_ISR_OFFSET);
	message->bar_range[3].le_size = htole64(0x1000);
	message->bar_range[3].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[4].le_offset = htole64(VIRTIOD_NET_DEVICE_OFFSET);
	message->bar_range[4].le_size = htole64(0x1000);
	message->bar_range[4].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[5].le_offset = htole64(0x5000);
	message->bar_range[5].le_size = htole64(VIRTIOD_NET_BAR_SIZE - 0x5000);
	message->bar_range[5].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->vendor_cap_count = 4;
	cap = &message->vendor_cap[0];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_NET_CAP_COMMON_CFG;
	cap->bytes[5] = VIRTIOD_NET_COMMON_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_NET_COMMON_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_net_common_config);
	cap = &message->vendor_cap[1];
	cap->length = 20;
	cap->bytes[0] = VIRTIOD_NET_CAP_NOTIFY_CFG;
	cap->bytes[5] = VIRTIOD_NET_NOTIFY_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_NET_NOTIFY_OFFSET >> 8;
	/* virtio_pci_cap.length: the complete direct notify BAR page. */
	cap->bytes[9] = 0;
	cap->bytes[10] = 0x10;
	cap->bytes[13] = 4;
	cap = &message->vendor_cap[2];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_NET_CAP_ISR_CFG;
	cap->bytes[5] = VIRTIOD_NET_ISR_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_NET_ISR_OFFSET >> 8;
	cap->bytes[9] = 1;
	cap = &message->vendor_cap[3];
	cap->length = 16;
	cap->bytes[0] = VIRTIOD_NET_CAP_DEVICE_CFG;
	cap->bytes[5] = VIRTIOD_NET_DEVICE_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_NET_DEVICE_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_net_config);
}

static int
virtiod_net_receive_start(int fd, struct vmm_pcie_abi_start *message)
{
	ssize_t size;

	size = recv(fd, message, sizeof(*message), 0);
	if (size != sizeof(*message))
		return size < 0 ? errno : EPROTO;
	if (!virtiod_net_header_valid(&message->header, VMM_PCIE_ABI_MSG_START,
	    sizeof(*message)) || le32toh(message->le_dma_segment_count) == 0 ||
	    le32toh(message->le_dma_segment_count) > VIRTIOD_MAX_DMA_SEGMENTS)
		return EPROTO;
	return 0;
}

static int
virtiod_net_receive_registered(int fd, const struct vmm_pcie_abi_start *start,
    struct vmm_pcie_abi_registered *message, int *bar_fdp, int *dma_fdp)
{
	char control[CMSG_SPACE(sizeof(int) * 2)];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr socket_message;
	int fds[2];
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
	    (socket_message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
		return size < 0 ? errno : EPROTO;
	if (!virtiod_net_header_valid(&message->header, VMM_PCIE_ABI_MSG_REGISTERED,
	    sizeof(*message)) || message->header.le_sequence !=
	    start->header.le_sequence || le32toh(message->le_bar_fd_mask) != 1 ||
	    (le32toh(message->header.le_flags) &
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY) == 0)
		return EPROTO;
	cmsg = CMSG_FIRSTHDR(&socket_message);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len !=
	    CMSG_LEN(sizeof(fds)) || CMSG_NXTHDR(&socket_message, cmsg) != NULL)
		return EPROTO;
	memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
	if (bar_fdp == NULL || dma_fdp == NULL || fds[0] < 0 || fds[1] < 0)
		return EPROTO;
	*bar_fdp = fds[0];
	*dma_fdp = fds[1];
	return 0;
}

static int
virtiod_net_send_stopped(const struct virtiod_net_state *state)
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
virtiod_net_send_msix(const struct virtiod_net_state *state, uint16_t vector)
{
	struct vmm_pcie_abi_msix message;

	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_MSIX);
	message.header.le_size = htole32(sizeof(message));
	message.le_device_id = state->own_registered.le_device_id;
	message.le_attachment_generation = state->own_registered.le_attachment_generation;
	message.le_vector = htole16(vector);
	return send(state->own_provider_fd, &message, sizeof(message), 0) ==
	    sizeof(message) ? 0 : errno;
}

static int
virtiod_net_map_dma(struct virtiod_net_state *state)
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
virtiod_net_unmap_dma(struct virtiod_net_state *state)
{
	unsigned int i;

	for (i = 0; i < state->mut_dma_count; i++) {
		if (state->own_dma[i].own_mut_bytes != NULL)
			(void)munmap(state->own_dma[i].own_mut_bytes,
			    state->own_dma[i].imm_size);
		state->own_dma[i].own_mut_bytes = NULL;
	}
	state->mut_dma_count = 0;
	memset(state->mut_queue_ready, 0, sizeof(state->mut_queue_ready));
}

static void
virtiod_net_initialize_bar(struct virtiod_net_state *state)
{
	unsigned int i;

	__atomic_store_n(&state->own_mut_common.le_msix_config,
	    htole16(VIRTIOD_NET_MSI_NO_VECTOR), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_num_queues,
	    htole16(VIRTIOD_NET_QUEUE_COUNT), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_queue_size,
	    htole16(VIRTIOD_QUEUE_SIZE), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_queue_msix_vector,
	    htole16(VIRTIOD_NET_MSI_NO_VECTOR), __ATOMIC_RELEASE);
	for (i = 0; i < VIRTIOD_NET_QUEUE_COUNT; i++) {
		state->own_mut_queue_config[i].le_size = htole16(VIRTIOD_QUEUE_SIZE);
		state->own_mut_queue_config[i].le_msix_vector =
		    htole16(VIRTIOD_NET_MSI_NO_VECTOR);
		state->own_mut_queue_config[i].le_notify_off = htole16(i);
	}
	virtiod_net_restore_queue(state, VIRTIOD_NET_QUEUE_RX);
	state->own_mut_config.le_status = htole16(1);
}

static int
virtiod_net_sync_queues(struct virtiod_net_state *state)
{
	struct virtiod_net_common_config *common;
	uint32_t selector;
	uint32_t features;
	unsigned int queue;
	uint8_t status;
	int error;

	common = &state->own_mut_common;
	status = __atomic_load_n(&common->device_status, __ATOMIC_ACQUIRE);
	if (status == 0 && state->mut_last_device_status != 0) {
		memset(state->mut_driver_features, 0, sizeof(state->mut_driver_features));
		memset(state->own_queue, 0, sizeof(state->own_queue));
		memset(state->mut_queue_ready, 0, sizeof(state->mut_queue_ready));
		state->mut_rx_queue_logged = 0;
		state->mut_rx_available_logged = 0;
			state->mut_rx_tap_logged = 0;
			state->mut_rx_completion_logged = 0;
		memset(state->own_mut_queue_config, 0,
		    sizeof(state->own_mut_queue_config));
		for (queue = 0; queue < VIRTIOD_NET_QUEUE_COUNT; queue++) {
			state->own_mut_queue_config[queue].le_size =
			    htole16(VIRTIOD_QUEUE_SIZE);
			state->own_mut_queue_config[queue].le_msix_vector =
			    htole16(VIRTIOD_NET_MSI_NO_VECTOR);
			state->own_mut_queue_config[queue].le_notify_off = htole16(queue);
		}
		state->mut_needs_reset = 0;
	}
	state->mut_last_device_status = status;
	if (state->mut_needs_reset)
		return 0;
	selector = le32toh(__atomic_load_n(&common->le_device_feature_select,
	    __ATOMIC_ACQUIRE));
	features = 0;
	if (selector == 0)
		features = VIRTIOD_NET_F_MAC | VIRTIOD_NET_F_STATUS;
	else if (selector == 1)
		features = VIRTIOD_NET_F_VERSION_1;
	__atomic_store_n(&common->le_device_feature, htole32(features),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_num_queues, htole16(VIRTIOD_NET_QUEUE_COUNT),
	    __ATOMIC_RELEASE);
	queue = le16toh(__atomic_load_n(&common->le_queue_select,
	    __ATOMIC_ACQUIRE));
	if (queue < VIRTIOD_NET_QUEUE_COUNT) {
		virtiod_net_restore_queue(state, queue);
	}
	if ((state->mut_driver_features[0] & ~(VIRTIOD_NET_F_MAC |
	    VIRTIOD_NET_F_STATUS)) != 0 || (state->mut_driver_features[1] &
	    ~VIRTIOD_NET_F_VERSION_1) != 0)
		return EPROTO;
	if ((status & VIRTIOD_NET_STATUS_DRIVER_OK) == 0)
		return 0;
	if ((state->mut_driver_features[1] & VIRTIOD_NET_F_VERSION_1) == 0 ||
	    queue >= VIRTIOD_NET_QUEUE_COUNT)
		return EPROTO;
	for (queue = 0; queue < VIRTIOD_NET_QUEUE_COUNT; queue++) {
		const struct virtiod_net_queue_config *config;

		config = &state->own_mut_queue_config[queue];
		if (le16toh(config->le_enable) == 0 ||
		    state->mut_queue_ready[queue])
			continue;
		if (le16toh(config->le_size) != VIRTIOD_QUEUE_SIZE)
			return EPROTO;
		error = virtiod_vring_configure(&state->own_queue[queue], state->own_dma,
		    state->mut_dma_count, VIRTIOD_QUEUE_SIZE,
		    le64toh(config->le_desc), le64toh(config->le_driver),
		    le64toh(config->le_device));
		if (error != 0)
			return error;
		state->mut_queue_ready[queue] = 1;
		if (queue == VIRTIOD_NET_QUEUE_RX && !state->mut_rx_queue_logged) {
			printf("virtiod: net RX queue ready size=%u\n",
			    (unsigned int)le16toh(config->le_size));
			(void)fflush(stdout);
			state->mut_rx_queue_logged = 1;
		}
	}
	return 0;
}

static void
virtiod_net_capture_queue(struct virtiod_net_state *state, unsigned int queue)
{
	struct virtiod_net_queue_config *config;
	struct virtiod_net_common_config *common;

	if (queue >= VIRTIOD_NET_QUEUE_COUNT)
		return;
	config = &state->own_mut_queue_config[queue];
	common = &state->own_mut_common;
	config->le_size = common->le_queue_size;
	config->le_msix_vector = common->le_queue_msix_vector;
	config->le_enable = common->le_queue_enable;
	config->le_notify_off = common->le_queue_notify_off;
	config->le_desc = common->le_queue_desc;
	config->le_driver = common->le_queue_driver;
	config->le_device = common->le_queue_device;
}

static void
virtiod_net_restore_queue(struct virtiod_net_state *state, unsigned int queue)
{
	const struct virtiod_net_queue_config *config;
	struct virtiod_net_common_config *common;

	if (queue >= VIRTIOD_NET_QUEUE_COUNT)
		return;
	config = &state->own_mut_queue_config[queue];
	common = &state->own_mut_common;
	common->le_queue_size = config->le_size;
	common->le_queue_msix_vector = config->le_msix_vector;
	common->le_queue_enable = config->le_enable;
	common->le_queue_notify_off = config->le_notify_off;
	common->le_queue_desc = config->le_desc;
	common->le_queue_driver = config->le_driver;
	common->le_queue_device = config->le_device;
}

static int
virtiod_net_process_tx(struct virtiod_net_state *state)
{
	struct virtiod_chain chain;
	struct iovec iov[VIRTIOD_MAX_CHAIN];
	int iov_count;
	int error;

	for (;;) {
		ssize_t length;
		unsigned int i;

		error = virtiod_vring_pop(&state->own_queue[VIRTIOD_NET_QUEUE_TX],
		    &chain);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
		for (i = 0; i < chain.mut_iov_count; i++) {
			if ((chain.own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) != 0)
				return EPROTO;
		}
		error = virtiod_net_iov_skip(&chain, sizeof(struct virtiod_net_header),
		    iov, &iov_count);
		if (error != 0)
			return error;
		length = virtiod_tap_writev(&state->own_tap, iov, iov_count);
		if (length < 0 && errno != EWOULDBLOCK)
			return errno;
		error = virtiod_net_complete(state, VIRTIOD_NET_QUEUE_TX, &chain,
		    length > 0 ? (uint32_t)length : 0);
		if (error != 0)
			return error;
	}
}

static int
virtiod_net_process_rx(struct virtiod_net_state *state)
{
	struct virtiod_chain chain;
	struct iovec iov[VIRTIOD_MAX_CHAIN];
	struct virtiod_net_header header;
	int iov_count;
	int error;

	error = virtiod_vring_pop(&state->own_queue[VIRTIOD_NET_QUEUE_RX], &chain);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;
	for (unsigned int i = 0; i < chain.mut_iov_count; i++) {
		if ((chain.own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) == 0)
			return EPROTO;
	}
	if (chain.own_mut_iov[0].iov_len < sizeof(header))
		return EPROTO;
	error = virtiod_net_iov_skip(&chain, sizeof(header), iov, &iov_count);
	if (error != 0)
		return error;
	memset(&header, 0, sizeof(header));
	header.le_num_buffers = htole16(1);
	memcpy(chain.own_mut_iov[0].iov_base, &header, sizeof(header));
	(void)virtiod_net_iov_limit(iov, iov_count, VIRTIOD_NET_MAX_FRAME);
	ssize_t length = virtiod_tap_readv(&state->own_tap, iov, iov_count);
	if (length < 0 && errno != EWOULDBLOCK)
		return errno;
	return virtiod_net_complete(state, VIRTIOD_NET_QUEUE_RX, &chain,
	    length > 0 ? (uint32_t)length + sizeof(header) : 0);
}

static int
virtiod_net_complete(struct virtiod_net_state *state, unsigned int queue,
    const struct virtiod_chain *chain, uint32_t length)
{
	uint16_t vector;
	int error;

	error = virtiod_vring_complete(&state->own_queue[queue], chain, length);
	if (error != 0)
		return error;
	state->mut_isr = 1;
	if (virtiod_net_interrupt_disabled(state, queue)) {
		if (queue == VIRTIOD_NET_QUEUE_RX &&
		    !state->mut_rx_completion_logged) {
			printf("virtiod: net RX completion irq=disabled\n");
			(void)fflush(stdout);
			state->mut_rx_completion_logged = 1;
		}
		return 0;
	}
	vector = le16toh(state->own_mut_queue_config[queue].le_msix_vector);
	if (vector == VIRTIOD_NET_MSI_NO_VECTOR) {
		if (queue == VIRTIOD_NET_QUEUE_RX &&
		    !state->mut_rx_completion_logged) {
			printf("virtiod: net RX completion irq=no-vector\n");
			(void)fflush(stdout);
			state->mut_rx_completion_logged = 1;
		}
		return 0;
	}
	if (vector >= VIRTIOD_NET_MSIX_COUNT)
		return EPROTO;
	if (queue == VIRTIOD_NET_QUEUE_RX && !state->mut_rx_completion_logged) {
		printf("virtiod: net RX completion msix=%u\n", vector);
		(void)fflush(stdout);
		state->mut_rx_completion_logged = 1;
	}
	return virtiod_net_send_msix(state, vector);
}

static int
virtiod_net_interrupt_disabled(const struct virtiod_net_state *state,
    unsigned int queue)
{
	uint16_t *available;
	void *pointer;
	int error;

	error = virtiod_dma_translate(state->own_dma, state->mut_dma_count,
	    state->own_queue[queue].mut_avail_gpa, sizeof(*available), &pointer);
	if (error != 0)
		return 0;
	available = pointer;
	return (le16toh(__atomic_load_n(available, __ATOMIC_ACQUIRE)) & 1U) != 0;
}

static int
virtiod_net_handle_mmio(struct virtiod_net_state *state,
    const struct vmm_pcie_abi_mmio *request)
{
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
	else if (offset >= VIRTIOD_NET_COMMON_OFFSET &&
	    offset - VIRTIOD_NET_COMMON_OFFSET <= sizeof(state->own_mut_common) &&
	    size <= sizeof(state->own_mut_common) -
	    (offset - VIRTIOD_NET_COMMON_OFFSET)) {
		uint8_t *bytes = (uint8_t *)(void *)&state->own_mut_common;

		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0) {
			uint32_t selector;
			uint16_t queue = le16toh(state->own_mut_common.le_queue_select);

			if (offset == VIRTIOD_NET_COMMON_OFFSET + 22U && size == 2)
				virtiod_net_capture_queue(state, queue);
			memcpy(bytes + offset - VIRTIOD_NET_COMMON_OFFSET, &value, size);
			queue = le16toh(state->own_mut_common.le_queue_select);
			if (offset == VIRTIOD_NET_COMMON_OFFSET + 22U && size == 2)
				virtiod_net_restore_queue(state, queue);
			else
				virtiod_net_capture_queue(state, queue);
			if (offset == VIRTIOD_NET_COMMON_OFFSET + 12U && size == 4) {
				selector = le32toh(state->own_mut_common.le_driver_feature_select);
				if (selector < 2)
					state->mut_driver_features[selector] = le32toh(
					    state->own_mut_common.le_driver_feature);
			}
		}
		error = virtiod_net_sync_queues(state);
		if (error != 0) {
			warnx("virtio-net common config failed: %s", strerror(error));
			state->mut_needs_reset = 1;
			state->own_mut_common.device_status |= VIRTIOD_NET_STATUS_NEEDS_RESET;
		}
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) == 0) {
			value = 0;
			memcpy(&value, bytes + offset - VIRTIOD_NET_COMMON_OFFSET, size);
		}
	} else if (offset == VIRTIOD_NET_ISR_OFFSET && size == 1) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else {
			value = state->mut_isr;
			state->mut_isr = 0;
		}
	} else if (offset >= VIRTIOD_NET_DEVICE_OFFSET &&
	    offset - VIRTIOD_NET_DEVICE_OFFSET <= sizeof(state->own_mut_config) &&
	    size <= sizeof(state->own_mut_config) -
	    (offset - VIRTIOD_NET_DEVICE_OFFSET)) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else {
			value = 0;
			memcpy(&value, (uint8_t *)(void *)&state->own_mut_config +
			    offset - VIRTIOD_NET_DEVICE_OFFSET, size);
		}
	} else
		error = EINVAL;
	return virtiod_net_send_mmio_response(state, request, value, error);
}

static int
virtiod_net_send_mmio_response(const struct virtiod_net_state *state,
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
virtiod_net_parse_mac(const char *text, uint8_t mac[6])
{
	unsigned int i;

	if (text == NULL || mac == NULL || strlen(text) != 17)
		return EINVAL;
	for (i = 0; i < 6; i++) {
		unsigned int high;
		unsigned int low;
		char a = text[i * 3U];
		char b = text[i * 3U + 1U];

		if ((i != 5 && text[i * 3U + 2U] != ':') ||
		    !((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f') ||
		    (a >= 'A' && a <= 'F')) || !((b >= '0' && b <= '9') ||
		    (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F')))
			return EINVAL;
		if (a <= '9')
			high = (unsigned int)(a - '0');
		else
			high = (unsigned int)((a | 0x20) - 'a') + 10U;
		if (b <= '9')
			low = (unsigned int)(b - '0');
		else
			low = (unsigned int)((b | 0x20) - 'a') + 10U;
		mac[i] = (uint8_t)((high << 4) | low);
	}
	if ((mac[0] & 1U) != 0 || (mac[0] & 2U) == 0)
		return EINVAL;
	return 0;
}

static int
virtiod_net_iov_skip(const struct virtiod_chain *chain, size_t skip,
    struct iovec *result, int *result_count)
{
	unsigned int i;
	int count;

	if (chain == NULL || result == NULL || result_count == NULL)
		return EINVAL;
	count = 0;
	for (i = 0; i < chain->mut_iov_count; i++) {
		struct iovec item = chain->own_mut_iov[i];

		if (skip >= item.iov_len) {
			skip -= item.iov_len;
			continue;
		}
		item.iov_base = (uint8_t *)item.iov_base + skip;
		item.iov_len -= skip;
		skip = 0;
		result[count++] = item;
	}
	if (skip != 0 || count == 0)
		return EPROTO;
	*result_count = count;
	return 0;
}

static size_t
virtiod_net_iov_limit(struct iovec *iov, int count, size_t limit)
{
	size_t total;
	int i;

	total = 0;
	for (i = 0; i < count; i++) {
		if (iov[i].iov_len > limit - total) {
			iov[i].iov_len = limit - total;
			return limit;
		}
		total += iov[i].iov_len;
		if (total == limit)
			return total;
	}
	return total;
}

static int
virtiod_net_header_valid(const struct vmm_pcie_abi_header *header,
    uint16_t type, size_t size)
{

	return header != NULL && le32toh(header->le_magic) == VMM_PCIE_ABI_MAGIC &&
	    le16toh(header->le_version) == VMM_PCIE_ABI_VERSION &&
	    le16toh(header->le_type) == type && le32toh(header->le_size) == size;
}

static void
virtiod_net_state_fini(struct virtiod_net_state *state)
{

	if (state->own_mut_bar != NULL && state->own_mut_bar != MAP_FAILED)
		(void)munmap(state->own_mut_bar, VIRTIOD_NET_BAR_SIZE);
	if (state->own_bar_fd >= 0)
		(void)close(state->own_bar_fd);
	if (state->own_dma_fd >= 0)
		(void)close(state->own_dma_fd);
	if (state->own_provider_fd >= 0)
		(void)close(state->own_provider_fd);
	virtiod_tap_close(&state->own_tap);
}
