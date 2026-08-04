/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Modern virtio-vsock-pci provider and the private runv stream broker.
 */
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/vmm_pcie_abi.h>
#include <sys/vmm_vsock_abi.h>

#include "virtiod.h"

#define VIRTIOD_VSOCK_BAR_SIZE		0x8000U
#define VIRTIOD_VSOCK_COMMON_OFFSET	0x1000U
#define VIRTIOD_VSOCK_NOTIFY_OFFSET	0x2000U
#define VIRTIOD_VSOCK_ISR_OFFSET	0x3000U
#define VIRTIOD_VSOCK_DEVICE_OFFSET	0x4000U
#define VIRTIOD_VSOCK_QUEUE_RX		0U
#define VIRTIOD_VSOCK_QUEUE_TX		1U
#define VIRTIOD_VSOCK_QUEUE_EVENT	2U
#define VIRTIOD_VSOCK_QUEUE_COUNT	3U
#define VIRTIOD_VSOCK_MSIX_COUNT	4U
#define VIRTIOD_VSOCK_MSI_NO_VECTOR	0xffffU
#define VIRTIOD_VSOCK_F_VERSION_1	0x00000001U
#define VIRTIOD_VSOCK_STATUS_DRIVER_OK	0x04U
#define VIRTIOD_VSOCK_STATUS_NEEDS_RESET 0x40U
#define VIRTIOD_VSOCK_TYPE_STREAM	1U
#define VIRTIOD_VSOCK_OP_REQUEST	1U
#define VIRTIOD_VSOCK_OP_RESPONSE	2U
#define VIRTIOD_VSOCK_OP_RST		3U
#define VIRTIOD_VSOCK_OP_SHUTDOWN	4U
#define VIRTIOD_VSOCK_OP_RW		5U
#define VIRTIOD_VSOCK_OP_CREDIT_UPDATE 6U
#define VIRTIOD_VSOCK_OP_CREDIT_REQUEST 7U
#define VIRTIOD_VSOCK_SHUTDOWN_RCV	1U
#define VIRTIOD_VSOCK_SHUTDOWN_SEND	2U
#define VIRTIOD_VSOCK_BUFFER_SIZE	65536U
#define VIRTIOD_VSOCK_PAYLOAD_MAX	4096U

struct virtiod_vsock_common_config {
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

struct virtiod_vsock_config {
	uint64_t le_guest_cid;
} __attribute__((__packed__));

struct virtiod_vsock_header {
	uint64_t le_src_cid;
	uint64_t le_dst_cid;
	uint32_t le_src_port;
	uint32_t le_dst_port;
	uint32_t le_len;
	uint16_t le_type;
	uint16_t le_op;
	uint32_t le_flags;
	uint32_t le_buf_alloc;
	uint32_t le_fwd_cnt;
} __attribute__((__packed__));

struct virtiod_vsock_queue_config {
	uint16_t le_size;
	uint16_t le_msix_vector;
	uint16_t le_enable;
	uint16_t le_notify_off;
	uint64_t le_desc;
	uint64_t le_driver;
	uint64_t le_device;
};

struct virtiod_vsock_state;

struct virtiod_vsock_thread {
	struct virtiod_vsock_state *borrow_mut_state;
	unsigned int imm_queue;
};

struct virtiod_vsock_packet {
	TAILQ_ENTRY(virtiod_vsock_packet) entry;
	struct virtiod_vsock_header imm_header;
	size_t imm_length;
	uint8_t own_bytes[];
};

struct virtiod_vsock_bytes {
	TAILQ_ENTRY(virtiod_vsock_bytes) entry;
	size_t mut_offset;
	size_t imm_length;
	uint8_t own_bytes[];
};

enum virtiod_vsock_flow_origin {
	VIRTIOD_VSOCK_FLOW_HOST_CONNECT,
	VIRTIOD_VSOCK_FLOW_GUEST_CONNECT,
};

struct virtiod_vsock_flow {
	TAILQ_ENTRY(virtiod_vsock_flow) entry;
	TAILQ_HEAD(, virtiod_vsock_bytes) own_mut_to_host;
	struct virtiod_vsock_state *borrow_mut_state;
	uint32_t imm_guest_port;
	uint32_t imm_host_port;
	uint64_t imm_guest_cid;
	uint64_t imm_request_sequence;
	uint32_t mut_peer_buf_alloc;
	uint32_t mut_peer_fwd_cnt;
	uint32_t mut_tx_cnt;
	uint32_t mut_rx_fwd_cnt;
	int own_backend_fd;
	int own_return_fd;
	enum virtiod_vsock_flow_origin imm_origin;
	int mut_open;
};

struct virtiod_vsock_listener {
	TAILQ_ENTRY(virtiod_vsock_listener) entry;
	uint32_t imm_port;
	uint64_t mut_accept_sequence;
};

struct virtiod_vsock_broker {
	TAILQ_HEAD(, virtiod_vsock_flow) own_mut_flows;
	TAILQ_HEAD(, virtiod_vsock_listener) own_mut_listeners;
	pthread_mutex_t own_mutex;
	pthread_mutex_t own_send_mutex;
	pthread_t own_thread;
	struct virtiod_vsock_state *weak_mut_state;
	uint32_t mut_next_port;
	int own_control_fd;
	int mut_running;
};

struct virtiod_vsock_state {
	pthread_mutex_t own_mutex;
	pthread_t own_mut_threads[VIRTIOD_VSOCK_QUEUE_COUNT];
	struct virtiod_vsock_thread
	    own_mut_thread_args[VIRTIOD_VSOCK_QUEUE_COUNT];
	struct virtiod_vring own_mut_queue[VIRTIOD_VSOCK_QUEUE_COUNT];
	struct virtiod_vsock_queue_config
	    own_mut_queue_config[VIRTIOD_VSOCK_QUEUE_COUNT];
	TAILQ_HEAD(, virtiod_vsock_packet) own_mut_packets;
	struct virtiod_vsock_broker *borrow_mut_broker;
	struct vmm_pcie_abi_start own_start;
	struct vmm_pcie_abi_registered own_registered;
	struct virtiod_dma_segment own_dma[VIRTIOD_MAX_DMA_SEGMENTS];
	struct virtiod_vsock_common_config own_mut_common;
	struct virtiod_vsock_config own_mut_config;
	uint8_t *own_mut_bar;
	uint32_t mut_driver_features[2];
	unsigned int mut_dma_count;
	uint16_t atomic_mut_msix_vector[VIRTIOD_VSOCK_QUEUE_COUNT];
	uint8_t atomic_mut_isr;
	uint8_t mut_last_device_status;
	int mut_queue_ready[VIRTIOD_VSOCK_QUEUE_COUNT];
	int mut_needs_reset;
	int mut_running;
	unsigned int mut_thread_count;
	int mut_broker_attached;
	int mut_generation_active;
	u_int atomic_mut_driver_ready_logged;
	u_int atomic_mut_first_guest_packet_logged;
	u_int atomic_mut_agent_response_logged;
	int own_provider_fd;
	int own_bar_fd;
	int own_dma_fd;
};

static void virtiod_vsock_build_register(struct vmm_pcie_abi_register *,
    uint64_t);
static int virtiod_vsock_receive_start(int, struct vmm_pcie_abi_start *);
static int virtiod_vsock_receive_registered(int,
    const struct vmm_pcie_abi_start *, struct vmm_pcie_abi_registered *,
    int *, int *);
static int virtiod_vsock_send_stopped(const struct virtiod_vsock_state *);
static int virtiod_vsock_send_msix(const struct virtiod_vsock_state *,
    unsigned int);
static int virtiod_vsock_map_dma(struct virtiod_vsock_state *);
static void virtiod_vsock_unmap_dma(struct virtiod_vsock_state *);
static void virtiod_vsock_initialize_bar(struct virtiod_vsock_state *);
static int virtiod_vsock_sync_queues(struct virtiod_vsock_state *);
static int virtiod_vsock_handle_mmio(struct virtiod_vsock_state *,
    const struct vmm_pcie_abi_mmio *);
static int virtiod_vsock_send_mmio_response(const struct virtiod_vsock_state *,
    const struct vmm_pcie_abi_mmio *, uint64_t, int);
static void *virtiod_vsock_queue_main(void *);
static int virtiod_vsock_process_tx(struct virtiod_vsock_state *);
static int virtiod_vsock_process_rx(struct virtiod_vsock_state *);
static int virtiod_vsock_process_event(struct virtiod_vsock_state *);
static int virtiod_vsock_complete(struct virtiod_vsock_state *, unsigned int,
    const struct virtiod_chain *, uint32_t);
static int virtiod_vsock_interrupt_disabled(const struct virtiod_vsock_state *,
    unsigned int);
static void virtiod_vsock_generation_fini(struct virtiod_vsock_state *);
static void virtiod_vsock_state_fini(struct virtiod_vsock_state *);
static int virtiod_vsock_header_valid(const struct vmm_pcie_abi_header *,
    uint16_t, size_t);
static int virtiod_vsock_chain_read(const struct virtiod_chain *, size_t,
    void *, size_t);
static int virtiod_vsock_chain_write(const struct virtiod_chain *, size_t,
    const void *, size_t);
static size_t virtiod_vsock_chain_writable(const struct virtiod_chain *);
static int virtiod_vsock_broker_attach(struct virtiod_vsock_broker *,
    struct virtiod_vsock_state *);
static void virtiod_vsock_broker_detach(struct virtiod_vsock_broker *,
    struct virtiod_vsock_state *);
static void *virtiod_vsock_broker_main(void *);
static int virtiod_vsock_broker_request(struct virtiod_vsock_broker *,
    const struct vmm_vsock_abi_request *);
static int virtiod_vsock_broker_guest_packet(struct virtiod_vsock_state *,
    const struct virtiod_vsock_header *, const uint8_t *, size_t);
static int virtiod_vsock_broker_take_packet(struct virtiod_vsock_state *,
    struct virtiod_vsock_packet **);
static void virtiod_vsock_broker_poll(struct virtiod_vsock_state *);
static void virtiod_vsock_broker_close_flow_locked(
    struct virtiod_vsock_broker *, struct virtiod_vsock_flow *);
static int virtiod_vsock_broker_queue_packet_locked(
    struct virtiod_vsock_broker *, struct virtiod_vsock_flow *, uint16_t,
    uint32_t, const void *, size_t);
static int virtiod_vsock_broker_send_result(struct virtiod_vsock_broker *,
    uint16_t, uint64_t, uint64_t, uint32_t, int, uint32_t);
static struct virtiod_vsock_flow *virtiod_vsock_broker_find_flow_locked(
    struct virtiod_vsock_broker *, uint64_t, uint32_t, uint32_t);
static struct virtiod_vsock_listener *virtiod_vsock_broker_find_listener_locked(
    struct virtiod_vsock_broker *, uint32_t);
static int virtiod_vsock_broker_set_nonblock(int);

int
virtiod_vsock_broker_init(struct virtiod_vsock_broker **result, int control_fd)
{
	struct virtiod_vsock_broker *broker;
	int error;

	if (result == NULL || control_fd < 0)
		return EINVAL;
	broker = calloc(1, sizeof(*broker));
	if (broker == NULL)
		return ENOMEM;
	TAILQ_INIT(&broker->own_mut_flows);
	TAILQ_INIT(&broker->own_mut_listeners);
	broker->own_control_fd = control_fd;
	broker->mut_next_port = 1024;
	broker->mut_running = 1;
	error = pthread_mutex_init(&broker->own_mutex, NULL);
	if (error != 0) {
		free(broker);
		return error;
	}
	error = pthread_mutex_init(&broker->own_send_mutex, NULL);
	if (error != 0) {
		(void)pthread_mutex_destroy(&broker->own_mutex);
		free(broker);
		return error;
	}
	error = pthread_create(&broker->own_thread, NULL,
	    virtiod_vsock_broker_main, broker);
	if (error != 0) {
		(void)pthread_mutex_destroy(&broker->own_send_mutex);
		(void)pthread_mutex_destroy(&broker->own_mutex);
		free(broker);
		return error;
	}
	*result = broker;
	return 0;
}

void
virtiod_vsock_broker_fini(struct virtiod_vsock_broker *broker)
{

	if (broker == NULL)
		return;
	pthread_mutex_lock(&broker->own_mutex);
	broker->mut_running = 0;
	pthread_mutex_unlock(&broker->own_mutex);
	(void)shutdown(broker->own_control_fd, SHUT_RDWR);
	(void)pthread_join(broker->own_thread, NULL);
	(void)close(broker->own_control_fd);
	(void)pthread_mutex_destroy(&broker->own_send_mutex);
	(void)pthread_mutex_destroy(&broker->own_mutex);
	free(broker);
}

int
virtiod_vsock_run(const struct virtiod_device *device,
    struct virtiod_vsock_broker *broker)
{
	struct vmm_pcie_abi_register register_message;
	struct virtiod_vsock_state state;
	struct pollfd pollfd;
	char provider_path[VIRTIOD_PATH_MAX];
	unsigned int i;
	int error;

	if (device == NULL || broker == NULL ||
	    device->imm_type != VIRTIOD_DEVICE_VSOCK ||
	    device->imm_guest_cid <= VMM_VSOCK_HOST_CID)
		return EINVAL;
	memset(&state, 0, sizeof(state));
	TAILQ_INIT(&state.own_mut_packets);
	state.borrow_mut_broker = broker;
	state.own_provider_fd = -1;
	state.own_bar_fd = -1;
	state.own_dma_fd = -1;
	if ((error = pthread_mutex_init(&state.own_mutex, NULL)) != 0)
		return error;
	if (snprintf(provider_path, sizeof(provider_path), "%s/provider",
	    device->imm_slot_path) >= (int)sizeof(provider_path)) {
		virtiod_vsock_state_fini(&state);
		return ENAMETOOLONG;
	}
	state.own_provider_fd = open(provider_path, O_RDWR);
	if (state.own_provider_fd < 0) {
		error = errno;
		virtiod_vsock_state_fini(&state);
		return error;
	}

restart:
	memset(&state.own_registered, 0, sizeof(state.own_registered));
	memset(state.own_dma, 0, sizeof(state.own_dma));
	memset(state.own_mut_queue, 0, sizeof(state.own_mut_queue));
	memset(state.own_mut_queue_config, 0, sizeof(state.own_mut_queue_config));
	memset(&state.own_mut_common, 0, sizeof(state.own_mut_common));
	memset(state.mut_driver_features, 0, sizeof(state.mut_driver_features));
	__atomic_store_n(&state.atomic_mut_isr, 0, __ATOMIC_RELEASE);
	state.mut_last_device_status = 0;
	state.mut_needs_reset = 0;
	state.mut_running = 0;
	state.mut_thread_count = 0;
	state.mut_broker_attached = 0;
	state.mut_generation_active = 0;
	state.own_mut_config.le_guest_cid = htole64(device->imm_guest_cid);
	error = virtiod_vsock_receive_start(state.own_provider_fd,
	    &state.own_start);
	if (error != 0) {
		virtiod_vsock_state_fini(&state);
		return error == ECONNRESET ? 0 : error;
	}
	virtiod_vsock_build_register(&register_message,
	    le64toh(state.own_start.header.le_sequence));
	if (send(state.own_provider_fd, &register_message,
	    sizeof(register_message), 0) != sizeof(register_message)) {
		error = errno;
		virtiod_vsock_state_fini(&state);
		return error;
	}
	error = virtiod_vsock_receive_registered(state.own_provider_fd,
	    &state.own_start, &state.own_registered, &state.own_bar_fd,
	    &state.own_dma_fd);
	if (error != 0) {
		virtiod_vsock_state_fini(&state);
		return error;
	}
	state.own_mut_bar = mmap(NULL, VIRTIOD_VSOCK_BAR_SIZE,
	    PROT_READ | PROT_WRITE, MAP_SHARED, state.own_bar_fd, 0);
	if (state.own_mut_bar == MAP_FAILED) {
		error = errno;
		state.own_mut_bar = NULL;
		virtiod_vsock_state_fini(&state);
		return error;
	}
	virtiod_vsock_initialize_bar(&state);
	error = virtiod_vsock_map_dma(&state);
	if (error != 0) {
		virtiod_vsock_state_fini(&state);
		return error;
	}
	error = virtiod_vsock_broker_attach(broker, &state);
	if (error != 0) {
		virtiod_vsock_state_fini(&state);
		return error;
	}
	state.mut_broker_attached = 1;
	state.mut_running = 1;
	for (i = 0; i < VIRTIOD_VSOCK_QUEUE_COUNT; i++) {
		state.own_mut_thread_args[i].borrow_mut_state = &state;
		state.own_mut_thread_args[i].imm_queue = i;
		error = pthread_create(&state.own_mut_threads[i], NULL,
		    virtiod_vsock_queue_main, &state.own_mut_thread_args[i]);
		if (error != 0) {
			state.mut_running = 0;
			virtiod_vsock_state_fini(&state);
			return error;
		}
		state.mut_thread_count++;
	}
	state.mut_generation_active = 1;
	fprintf(stderr, "virtiod tsc=%020" PRIu64
	    " vsock provider_ready cid=%" PRIu64 "\n",
	    (uint64_t)__builtin_ia32_rdtsc(),
	    le64toh(state.own_mut_config.le_guest_cid));
	printf("virtiod: ready slot=%s vsock-cid=%ju\n", device->imm_slot_path,
	    (uintmax_t)device->imm_guest_cid);
	if (fflush(stdout) != 0) {
		error = errno;
		virtiod_vsock_state_fini(&state);
		return error;
	}
	pollfd.fd = state.own_provider_fd;
	pollfd.events = POLLIN;
	for (;;) {
		struct vmm_pcie_abi_mmio message;
		ssize_t size;

		(void)poll(&pollfd, 1, 10);
		if ((pollfd.revents & POLLIN) == 0)
			continue;
		size = recv(state.own_provider_fd, &message, sizeof(message), 0);
		if (size == 0)
			break;
		if (size != sizeof(message) || !virtiod_vsock_header_valid(
		    &message.header, VMM_PCIE_ABI_MSG_MMIO_REQUEST, sizeof(message))) {
			error = size < 0 ? errno : EPROTO;
			break;
		}
		error = virtiod_vsock_handle_mmio(&state, &message);
		if (error != 0)
			break;
	}
	virtiod_vsock_generation_fini(&state);
	if (error == 0)
		error = virtiod_vsock_send_stopped(&state);
	if (error != 0) {
		virtiod_vsock_state_fini(&state);
		return error;
	}
	goto restart;
}

static void
virtiod_vsock_build_register(struct vmm_pcie_abi_register *message,
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
	message->le_device_id = htole16(0x1053);
	message->le_subsystem_vendor_id = htole16(0x1af4);
	message->le_subsystem_device_id = htole16(19);
	message->le_class_code = htole32(0x078000);
	message->revision = 1;
	message->le_msix_vectors = htole16(VIRTIOD_VSOCK_MSIX_COUNT);
	message->bar[0].le_size = htole64(VIRTIOD_VSOCK_BAR_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_64BIT);
	message->bar_range_count = 6;
	message->bar_range[0].le_size = htole64(0x1000);
	message->bar_range[0].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->bar_range[1].le_offset = htole64(VIRTIOD_VSOCK_COMMON_OFFSET);
	message->bar_range[1].le_size = htole64(0x1000);
	message->bar_range[1].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[2].le_offset = htole64(VIRTIOD_VSOCK_NOTIFY_OFFSET);
	message->bar_range[2].le_size = htole64(0x1000);
	message->bar_range[2].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->bar_range[3].le_offset = htole64(VIRTIOD_VSOCK_ISR_OFFSET);
	message->bar_range[3].le_size = htole64(0x1000);
	message->bar_range[3].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[4].le_offset = htole64(VIRTIOD_VSOCK_DEVICE_OFFSET);
	message->bar_range[4].le_size = htole64(0x1000);
	message->bar_range[4].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED);
	message->bar_range[5].le_offset = htole64(0x5000);
	message->bar_range[5].le_size = htole64(VIRTIOD_VSOCK_BAR_SIZE - 0x5000);
	message->bar_range[5].le_flags = htole32(VMM_PCIE_ABI_BAR_RANGE_F_DIRECT);
	message->vendor_cap_count = 4;
	cap = &message->vendor_cap[0];
	cap->length = 16;
	cap->bytes[0] = 1;
	cap->bytes[5] = VIRTIOD_VSOCK_COMMON_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_VSOCK_COMMON_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_vsock_common_config);
	cap = &message->vendor_cap[1];
	cap->length = 20;
	cap->bytes[0] = 2;
	cap->bytes[5] = VIRTIOD_VSOCK_NOTIFY_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_VSOCK_NOTIFY_OFFSET >> 8;
	cap->bytes[10] = 0x10;
	cap->bytes[13] = 4;
	cap = &message->vendor_cap[2];
	cap->length = 16;
	cap->bytes[0] = 3;
	cap->bytes[5] = VIRTIOD_VSOCK_ISR_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_VSOCK_ISR_OFFSET >> 8;
	cap->bytes[9] = 1;
	cap = &message->vendor_cap[3];
	cap->length = 16;
	cap->bytes[0] = 4;
	cap->bytes[5] = VIRTIOD_VSOCK_DEVICE_OFFSET & 0xff;
	cap->bytes[6] = VIRTIOD_VSOCK_DEVICE_OFFSET >> 8;
	cap->bytes[9] = sizeof(struct virtiod_vsock_config);
}

static int
virtiod_vsock_receive_start(int fd, struct vmm_pcie_abi_start *message)
{
	ssize_t size;

	size = recv(fd, message, sizeof(*message), 0);
	if (size == 0)
		return ECONNRESET;
	if (size != sizeof(*message))
		return size < 0 ? errno : EPROTO;
	if (!virtiod_vsock_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_START, sizeof(*message)) ||
	    le32toh(message->le_dma_segment_count) == 0 ||
	    le32toh(message->le_dma_segment_count) > VIRTIOD_MAX_DMA_SEGMENTS)
		return EPROTO;
	return 0;
}

static int
virtiod_vsock_receive_registered(int fd,
    const struct vmm_pcie_abi_start *start,
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
	if (!virtiod_vsock_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_REGISTERED, sizeof(*message)) ||
	    message->header.le_sequence != start->header.le_sequence ||
	    le32toh(message->le_bar_fd_mask) != 1 ||
	    le16toh(message->le_msix_vectors) != VIRTIOD_VSOCK_MSIX_COUNT ||
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
virtiod_vsock_send_stopped(const struct virtiod_vsock_state *state)
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
virtiod_vsock_send_msix(const struct virtiod_vsock_state *state,
    unsigned int queue)
{
	struct vmm_pcie_abi_msix message;
	uint16_t vector;

	if (queue >= VIRTIOD_VSOCK_QUEUE_COUNT)
		return EINVAL;
	vector = __atomic_load_n(&state->atomic_mut_msix_vector[queue],
	    __ATOMIC_ACQUIRE);
	if (vector == VIRTIOD_VSOCK_MSI_NO_VECTOR)
		return 0;
	if (vector >= VIRTIOD_VSOCK_MSIX_COUNT)
		return EPROTO;
	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_MSIX);
	message.header.le_size = htole32(sizeof(message));
	message.le_device_id = state->own_registered.le_device_id;
	message.le_attachment_generation =
	    state->own_registered.le_attachment_generation;
	message.le_vector = htole16(vector);
	return send(state->own_provider_fd, &message, sizeof(message), 0) ==
	    sizeof(message) ? 0 : errno;
}

static int
virtiod_vsock_map_dma(struct virtiod_vsock_state *state)
{
	unsigned int i;

	state->mut_dma_count = le32toh(state->own_start.le_dma_segment_count);
	for (i = 0; i < state->mut_dma_count; i++) {
		const struct vmm_pcie_abi_dma_segment *source;
		void *mapping;

		source = &state->own_start.dma_segment[i];
		state->own_dma[i].imm_gpa = le64toh(source->le_gpa);
		state->own_dma[i].imm_size = le64toh(source->le_length);
		if (state->own_dma[i].imm_size == 0 ||
		    (state->own_dma[i].imm_gpa &
		    (VMM_PCIE_ABI_PAGE_SIZE - 1U)) != 0 ||
		    (state->own_dma[i].imm_size &
		    (VMM_PCIE_ABI_PAGE_SIZE - 1U)) != 0)
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
virtiod_vsock_unmap_dma(struct virtiod_vsock_state *state)
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
virtiod_vsock_initialize_bar(struct virtiod_vsock_state *state)
{
	unsigned int i;

	__atomic_store_n(&state->own_mut_common.le_msix_config,
	    htole16(VIRTIOD_VSOCK_MSI_NO_VECTOR), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_num_queues,
	    htole16(VIRTIOD_VSOCK_QUEUE_COUNT), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_queue_size,
	    htole16(VIRTIOD_QUEUE_SIZE), __ATOMIC_RELEASE);
	__atomic_store_n(&state->own_mut_common.le_queue_msix_vector,
	    htole16(VIRTIOD_VSOCK_MSI_NO_VECTOR), __ATOMIC_RELEASE);
	for (i = 0; i < VIRTIOD_VSOCK_QUEUE_COUNT; i++) {
		state->own_mut_queue_config[i].le_size =
		    htole16(VIRTIOD_QUEUE_SIZE);
		state->own_mut_queue_config[i].le_msix_vector =
		    htole16(VIRTIOD_VSOCK_MSI_NO_VECTOR);
		state->own_mut_queue_config[i].le_notify_off = htole16(i);
	}
}

static int
virtiod_vsock_sync_queues(struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_common_config *common;
	uint16_t queue;
	uint32_t selector;
	uint32_t features;
	uint8_t status;
	unsigned int i;
	int error;

	common = &state->own_mut_common;
	status = __atomic_load_n(&common->device_status, __ATOMIC_ACQUIRE);
	if (status == 0 && state->mut_last_device_status != 0) {
		memset(state->mut_driver_features, 0,
		    sizeof(state->mut_driver_features));
		memset(state->own_mut_queue, 0, sizeof(state->own_mut_queue));
		memset(state->mut_queue_ready, 0, sizeof(state->mut_queue_ready));
		for (i = 0; i < VIRTIOD_VSOCK_QUEUE_COUNT; i++) {
			state->own_mut_queue_config[i].le_size =
			    htole16(VIRTIOD_QUEUE_SIZE);
			state->own_mut_queue_config[i].le_msix_vector =
			    htole16(VIRTIOD_VSOCK_MSI_NO_VECTOR);
			state->own_mut_queue_config[i].le_enable = 0;
			state->own_mut_queue_config[i].le_notify_off = htole16(i);
		}
		state->mut_needs_reset = 0;
	}
	state->mut_last_device_status = status;
	if (state->mut_needs_reset)
		return 0;
	selector = le32toh(__atomic_load_n(&common->le_device_feature_select,
	    __ATOMIC_ACQUIRE));
	features = selector == 1 ? VIRTIOD_VSOCK_F_VERSION_1 : 0;
	__atomic_store_n(&common->le_device_feature, htole32(features),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&common->le_num_queues,
	    htole16(VIRTIOD_VSOCK_QUEUE_COUNT), __ATOMIC_RELEASE);
	queue = le16toh(__atomic_load_n(&common->le_queue_select,
	    __ATOMIC_ACQUIRE));
	if (queue >= VIRTIOD_VSOCK_QUEUE_COUNT)
		return EPROTO;
	state->own_mut_queue_config[queue].le_size = common->le_queue_size;
	state->own_mut_queue_config[queue].le_msix_vector =
	    common->le_queue_msix_vector;
	state->own_mut_queue_config[queue].le_enable = common->le_queue_enable;
	state->own_mut_queue_config[queue].le_notify_off =
	    common->le_queue_notify_off;
	state->own_mut_queue_config[queue].le_desc = common->le_queue_desc;
	state->own_mut_queue_config[queue].le_driver = common->le_queue_driver;
	state->own_mut_queue_config[queue].le_device = common->le_queue_device;
	if ((state->mut_driver_features[0] != 0) ||
	    (state->mut_driver_features[1] & ~VIRTIOD_VSOCK_F_VERSION_1) != 0)
		return EPROTO;
	if ((status & VIRTIOD_VSOCK_STATUS_DRIVER_OK) == 0)
		return 0;
	if ((state->mut_driver_features[1] & VIRTIOD_VSOCK_F_VERSION_1) == 0)
		return EPROTO;
	if (__atomic_exchange_n(&state->atomic_mut_driver_ready_logged, 1,
	    __ATOMIC_ACQ_REL) == 0) {
		fprintf(stderr, "virtiod tsc=%020" PRIu64
		    " vsock driver_ok cid=%" PRIu64 "\n",
		    (uint64_t)__builtin_ia32_rdtsc(),
		    le64toh(state->own_mut_config.le_guest_cid));
	}
	for (i = 0; i < VIRTIOD_VSOCK_QUEUE_COUNT; i++) {
		const struct virtiod_vsock_queue_config *config;

		config = &state->own_mut_queue_config[i];
		if (le16toh(config->le_enable) == 0 || state->mut_queue_ready[i])
			continue;
		if (le16toh(config->le_size) != VIRTIOD_QUEUE_SIZE)
			return EPROTO;
		error = virtiod_vring_configure(&state->own_mut_queue[i],
		    state->own_dma, state->mut_dma_count, VIRTIOD_QUEUE_SIZE,
		    le64toh(config->le_desc), le64toh(config->le_driver),
		    le64toh(config->le_device));
		if (error != 0)
			return error;
		__atomic_store_n(&state->atomic_mut_msix_vector[i],
		    le16toh(config->le_msix_vector), __ATOMIC_RELEASE);
		state->mut_queue_ready[i] = 1;
	}
	return 0;
}

static int
virtiod_vsock_handle_mmio(struct virtiod_vsock_state *state,
    const struct vmm_pcie_abi_mmio *request)
{
	uint64_t offset;
	uint64_t value;
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
	pthread_mutex_lock(&state->own_mutex);
	if (le32toh(request->le_bar_index) != 0)
		error = EINVAL;
	else if (offset >= VIRTIOD_VSOCK_COMMON_OFFSET &&
	    offset - VIRTIOD_VSOCK_COMMON_OFFSET <= sizeof(state->own_mut_common) &&
	    size <= sizeof(state->own_mut_common) -
	    (offset - VIRTIOD_VSOCK_COMMON_OFFSET)) {
		uint8_t *bytes;
		uint16_t queue;

		bytes = (uint8_t *)(void *)&state->own_mut_common;
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0) {
			uint32_t selector;

			queue = le16toh(state->own_mut_common.le_queue_select);
			if (queue < VIRTIOD_VSOCK_QUEUE_COUNT) {
				state->own_mut_queue_config[queue].le_size =
				    state->own_mut_common.le_queue_size;
				state->own_mut_queue_config[queue].le_msix_vector =
				    state->own_mut_common.le_queue_msix_vector;
				state->own_mut_queue_config[queue].le_enable =
				    state->own_mut_common.le_queue_enable;
				state->own_mut_queue_config[queue].le_notify_off =
				    state->own_mut_common.le_queue_notify_off;
				state->own_mut_queue_config[queue].le_desc =
				    state->own_mut_common.le_queue_desc;
				state->own_mut_queue_config[queue].le_driver =
				    state->own_mut_common.le_queue_driver;
				state->own_mut_queue_config[queue].le_device =
				    state->own_mut_common.le_queue_device;
			}
			memcpy(bytes + offset - VIRTIOD_VSOCK_COMMON_OFFSET, &value, size);
			selector = le32toh(state->own_mut_common.le_driver_feature_select);
			if (offset == VIRTIOD_VSOCK_COMMON_OFFSET + 12U && size == 4 &&
			    selector < 2)
				state->mut_driver_features[selector] = le32toh(
				    state->own_mut_common.le_driver_feature);
			queue = le16toh(state->own_mut_common.le_queue_select);
			if (queue < VIRTIOD_VSOCK_QUEUE_COUNT) {
				if (offset == VIRTIOD_VSOCK_COMMON_OFFSET + 22U &&
				    size == 2) {
					state->own_mut_common.le_queue_size =
				    state->own_mut_queue_config[queue].le_size;
					state->own_mut_common.le_queue_msix_vector =
				    state->own_mut_queue_config[queue].le_msix_vector;
					state->own_mut_common.le_queue_enable =
				    state->own_mut_queue_config[queue].le_enable;
					state->own_mut_common.le_queue_notify_off =
				    state->own_mut_queue_config[queue].le_notify_off;
					state->own_mut_common.le_queue_desc =
				    state->own_mut_queue_config[queue].le_desc;
					state->own_mut_common.le_queue_driver =
				    state->own_mut_queue_config[queue].le_driver;
					state->own_mut_common.le_queue_device =
				    state->own_mut_queue_config[queue].le_device;
				} else {
					state->own_mut_queue_config[queue].le_size =
					    state->own_mut_common.le_queue_size;
					state->own_mut_queue_config[queue].le_msix_vector =
					    state->own_mut_common.le_queue_msix_vector;
					state->own_mut_queue_config[queue].le_enable =
					    state->own_mut_common.le_queue_enable;
					state->own_mut_queue_config[queue].le_notify_off =
					    state->own_mut_common.le_queue_notify_off;
					state->own_mut_queue_config[queue].le_desc =
					    state->own_mut_common.le_queue_desc;
					state->own_mut_queue_config[queue].le_driver =
					    state->own_mut_common.le_queue_driver;
					state->own_mut_queue_config[queue].le_device =
					    state->own_mut_common.le_queue_device;
				}
			}
		}
		error = virtiod_vsock_sync_queues(state);
		if (error != 0) {
			__atomic_fetch_or(&state->own_mut_common.device_status,
			    VIRTIOD_VSOCK_STATUS_NEEDS_RESET, __ATOMIC_RELEASE);
			state->mut_needs_reset = 1;
		}
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) == 0) {
			queue = le16toh(state->own_mut_common.le_queue_select);
			if (queue < VIRTIOD_VSOCK_QUEUE_COUNT) {
				state->own_mut_common.le_queue_size =
				    state->own_mut_queue_config[queue].le_size;
				state->own_mut_common.le_queue_msix_vector =
				    state->own_mut_queue_config[queue].le_msix_vector;
				state->own_mut_common.le_queue_enable =
				    state->own_mut_queue_config[queue].le_enable;
				state->own_mut_common.le_queue_notify_off =
				    state->own_mut_queue_config[queue].le_notify_off;
				state->own_mut_common.le_queue_desc =
				    state->own_mut_queue_config[queue].le_desc;
				state->own_mut_common.le_queue_driver =
				    state->own_mut_queue_config[queue].le_driver;
				state->own_mut_common.le_queue_device =
				    state->own_mut_queue_config[queue].le_device;
			}
			memcpy(&value, bytes + offset - VIRTIOD_VSOCK_COMMON_OFFSET, size);
		}
	} else if (offset >= VIRTIOD_VSOCK_ISR_OFFSET &&
	    offset - VIRTIOD_VSOCK_ISR_OFFSET == 0 && size == 1) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else
			value = __atomic_exchange_n(&state->atomic_mut_isr, 0,
			    __ATOMIC_ACQ_REL);
	} else if (offset >= VIRTIOD_VSOCK_DEVICE_OFFSET &&
	    offset - VIRTIOD_VSOCK_DEVICE_OFFSET <= sizeof(state->own_mut_config) &&
	    size <= sizeof(state->own_mut_config) -
	    (offset - VIRTIOD_VSOCK_DEVICE_OFFSET)) {
		if ((flags & VMM_PCIE_ABI_MMIO_F_WRITE) != 0)
			error = EROFS;
		else
			memcpy(&value, (const uint8_t *)(const void *)&state->own_mut_config +
			    offset - VIRTIOD_VSOCK_DEVICE_OFFSET, size);
	} else {
		error = EINVAL;
	}
	pthread_mutex_unlock(&state->own_mutex);
	return virtiod_vsock_send_mmio_response(state, request, value, error);
}

static int
virtiod_vsock_send_mmio_response(const struct virtiod_vsock_state *state,
    const struct vmm_pcie_abi_mmio *request, uint64_t value, int error)
{
	struct vmm_pcie_abi_mmio response;

	response = *request;
	response.header.le_type = htole16(VMM_PCIE_ABI_MSG_MMIO_RESPONSE);
	response.le_value = htole64(value);
	response.le_error = htole32((uint32_t)error);
	return send(state->own_provider_fd, &response, sizeof(response), 0) ==
	    sizeof(response) ? 0 : errno;
}

static void *
virtiod_vsock_queue_main(void *argument)
{
	const struct virtiod_vsock_thread *thread;
	struct virtiod_vsock_state *state;
	unsigned int queue;

	thread = argument;
	state = thread->borrow_mut_state;
	queue = thread->imm_queue;
	if (state == NULL || queue >= VIRTIOD_VSOCK_QUEUE_COUNT)
		return NULL;
	for (;;) {
		int error;
		int ready;
		int running;

		pthread_mutex_lock(&state->own_mutex);
		running = state->mut_running;
		ready = running && state->mut_queue_ready[queue];
		pthread_mutex_unlock(&state->own_mutex);
		if (!running)
			break;
		if (!ready) {
			usleep(1000);
			continue;
		}
		if (queue == VIRTIOD_VSOCK_QUEUE_TX)
			error = virtiod_vsock_process_tx(state);
		else if (queue == VIRTIOD_VSOCK_QUEUE_RX)
			error = virtiod_vsock_process_rx(state);
		else
			error = virtiod_vsock_process_event(state);
		if (error != 0) {
			pthread_mutex_lock(&state->own_mutex);
			__atomic_fetch_or(&state->own_mut_common.device_status,
			    VIRTIOD_VSOCK_STATUS_NEEDS_RESET, __ATOMIC_RELEASE);
			state->mut_needs_reset = 1;
			state->mut_queue_ready[queue] = 0;
			pthread_mutex_unlock(&state->own_mutex);
		}
		usleep(1000);
	}
	return NULL;
}

static int
virtiod_vsock_process_tx(struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_header header;
	struct virtiod_chain chain;
	uint8_t *payload;
	size_t length;
	size_t readable;
	unsigned int i;
	int error;

	pthread_mutex_lock(&state->own_mutex);
	error = virtiod_vring_pop(&state->own_mut_queue[VIRTIOD_VSOCK_QUEUE_TX],
	    &chain);
	pthread_mutex_unlock(&state->own_mutex);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;
	readable = 0;
	for (i = 0; i < chain.mut_iov_count; i++) {
		if ((chain.own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) != 0)
			return EPROTO;
		readable += chain.own_mut_iov[i].iov_len;
	}
	if (virtiod_vsock_chain_read(&chain, 0, &header, sizeof(header)) != 0)
		return EPROTO;
	length = le32toh(header.le_len);
	if (length > VIRTIOD_VSOCK_PAYLOAD_MAX ||
	    sizeof(header) + length > readable)
		return EPROTO;
	payload = NULL;
	if (length != 0) {
		payload = malloc(length);
		if (payload == NULL)
			return ENOMEM;
		error = virtiod_vsock_chain_read(&chain, sizeof(header), payload,
		    length);
		if (error != 0) {
			free(payload);
			return error;
		}
	}
	error = virtiod_vsock_broker_guest_packet(state, &header, payload,
	    length);
	free(payload);
	if (error != 0)
		return error;
	pthread_mutex_lock(&state->own_mutex);
	error = virtiod_vsock_complete(state, VIRTIOD_VSOCK_QUEUE_TX, &chain,
	    sizeof(header) + (uint32_t)length);
	pthread_mutex_unlock(&state->own_mutex);
	return error;
}

static int
virtiod_vsock_process_rx(struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_packet *packet;
	struct virtiod_chain chain;
	uint8_t *bytes;
	size_t total;
	int available;
	int error;

	pthread_mutex_lock(&state->own_mutex);
	available = virtiod_vring_has_available(
	    &state->own_mut_queue[VIRTIOD_VSOCK_QUEUE_RX]);
	pthread_mutex_unlock(&state->own_mutex);
	if (available < 0)
		return -available;
	if (available == 0)
		return 0;
	virtiod_vsock_broker_poll(state);
	error = virtiod_vsock_broker_take_packet(state, &packet);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;
	pthread_mutex_lock(&state->own_mutex);
	error = virtiod_vring_pop(&state->own_mut_queue[VIRTIOD_VSOCK_QUEUE_RX],
	    &chain);
	if (error == 0 && chain.mut_readable_count != 0)
		error = EPROTO;
	total = sizeof(packet->imm_header) + packet->imm_length;
	if (error == 0 && virtiod_vsock_chain_writable(&chain) < total)
		error = EPROTO;
	if (error == 0) {
		bytes = malloc(total);
		if (bytes == NULL)
			error = ENOMEM;
		else {
			memcpy(bytes, &packet->imm_header, sizeof(packet->imm_header));
			memcpy(bytes + sizeof(packet->imm_header), packet->own_bytes,
			    packet->imm_length);
			error = virtiod_vsock_chain_write(&chain, 0, bytes, total);
			free(bytes);
		}
	}
	if (error == 0)
		error = virtiod_vsock_complete(state, VIRTIOD_VSOCK_QUEUE_RX,
		    &chain, (uint32_t)total);
	pthread_mutex_unlock(&state->own_mutex);
	free(packet);
	return error;
}

static int
virtiod_vsock_process_event(struct virtiod_vsock_state *state)
{

	(void)state;
	return 0;
}

static int
virtiod_vsock_complete(struct virtiod_vsock_state *state, unsigned int queue,
    const struct virtiod_chain *chain, uint32_t length)
{
	int error;

	error = virtiod_vring_complete(&state->own_mut_queue[queue], chain, length);
	if (error != 0)
		return error;
	__atomic_store_n(&state->atomic_mut_isr, 1, __ATOMIC_RELEASE);
	if (virtiod_vsock_interrupt_disabled(state, queue))
		return 0;
	return virtiod_vsock_send_msix(state, queue);
}

static int
virtiod_vsock_interrupt_disabled(const struct virtiod_vsock_state *state,
    unsigned int queue)
{
	uint16_t *available;
	void *pointer;
	int error;

	error = virtiod_dma_translate(state->own_dma, state->mut_dma_count,
	    state->own_mut_queue[queue].mut_avail_gpa, sizeof(*available), &pointer);
	if (error != 0)
		return 0;
	available = pointer;
	return (le16toh(__atomic_load_n(available, __ATOMIC_ACQUIRE)) & 1U) != 0;
}

static int
virtiod_vsock_chain_read(const struct virtiod_chain *chain, size_t offset,
    void *destination, size_t length)
{
	uint8_t *bytes;
	unsigned int i;

	if (chain == NULL || destination == NULL)
		return EINVAL;
	bytes = destination;
	for (i = 0; i < chain->mut_iov_count && length != 0; i++) {
		const struct iovec *iov;
		size_t part;

		iov = &chain->own_mut_iov[i];
		if ((chain->own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) != 0)
			return EPROTO;
		if (offset >= iov->iov_len) {
			offset -= iov->iov_len;
			continue;
		}
		part = iov->iov_len - offset;
		if (part > length)
			part = length;
		memcpy(bytes, (const uint8_t *)iov->iov_base + offset, part);
		bytes += part;
		length -= part;
		offset = 0;
	}
	return length == 0 ? 0 : EPROTO;
}

static int
virtiod_vsock_chain_write(const struct virtiod_chain *chain, size_t offset,
    const void *source, size_t length)
{
	const uint8_t *bytes;
	unsigned int i;

	if (chain == NULL || source == NULL)
		return EINVAL;
	bytes = source;
	for (i = 0; i < chain->mut_iov_count && length != 0; i++) {
		const struct iovec *iov;
		size_t part;

		iov = &chain->own_mut_iov[i];
		if ((chain->own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) == 0)
			return EPROTO;
		if (offset >= iov->iov_len) {
			offset -= iov->iov_len;
			continue;
		}
		part = iov->iov_len - offset;
		if (part > length)
			part = length;
		memcpy((uint8_t *)iov->iov_base + offset, bytes, part);
		bytes += part;
		length -= part;
		offset = 0;
	}
	return length == 0 ? 0 : EPROTO;
}

static size_t
virtiod_vsock_chain_writable(const struct virtiod_chain *chain)
{
	size_t length;
	unsigned int i;

	length = 0;
	for (i = 0; i < chain->mut_iov_count; i++) {
		if ((chain->own_mut_flags[i] & VIRTIOD_DESC_F_WRITE) != 0)
			length += chain->own_mut_iov[i].iov_len;
	}
	return length;
}

static int
virtiod_vsock_broker_attach(struct virtiod_vsock_broker *broker,
    struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_flow *flow;
	struct virtiod_vsock_flow *next;
	int error;

	pthread_mutex_lock(&broker->own_mutex);
	if (!broker->mut_running || broker->weak_mut_state != NULL)
		error = EBUSY;
	else {
		broker->weak_mut_state = state;
		error = 0;
		for (flow = TAILQ_FIRST(&broker->own_mut_flows); flow != NULL;
		    flow = next) {
			next = TAILQ_NEXT(flow, entry);
			if (flow->borrow_mut_state != NULL)
				continue;
			if (flow->imm_guest_cid !=
			    le64toh(state->own_mut_config.le_guest_cid)) {
				(void)virtiod_vsock_broker_send_result(broker,
				    VMM_VSOCK_ABI_MSG_CONNECTED,
				    flow->imm_request_sequence, flow->imm_guest_cid,
				    EINVAL, -1, flow->imm_guest_port);
				virtiod_vsock_broker_close_flow_locked(broker, flow);
				continue;
			}
			flow->borrow_mut_state = state;
			error = virtiod_vsock_broker_queue_packet_locked(broker, flow,
			    VIRTIOD_VSOCK_OP_REQUEST, 0, NULL, 0);
			if (error != 0) {
				(void)virtiod_vsock_broker_send_result(broker,
				    VMM_VSOCK_ABI_MSG_CONNECTED,
				    flow->imm_request_sequence, flow->imm_guest_cid,
				    error, -1, flow->imm_guest_port);
				virtiod_vsock_broker_close_flow_locked(broker, flow);
			}
		}
		error = 0;
	}
	pthread_mutex_unlock(&broker->own_mutex);
	return error;
}

static void
virtiod_vsock_broker_detach(struct virtiod_vsock_broker *broker,
    struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_flow *flow;
	struct virtiod_vsock_flow *next;

	if (broker == NULL)
		return;
	pthread_mutex_lock(&broker->own_mutex);
	if (broker->weak_mut_state == state)
		broker->weak_mut_state = NULL;
	for (flow = TAILQ_FIRST(&broker->own_mut_flows); flow != NULL;
	    flow = next) {
		next = TAILQ_NEXT(flow, entry);
		if (flow->borrow_mut_state == state)
			virtiod_vsock_broker_close_flow_locked(broker, flow);
	}
	pthread_mutex_unlock(&broker->own_mutex);
	(void)virtiod_vsock_broker_send_result(broker,
	    VMM_VSOCK_ABI_MSG_DEVICE_DOWN, 0, 0, ECONNRESET, -1, 0);
}

static void *
virtiod_vsock_broker_main(void *argument)
{
	struct virtiod_vsock_broker *broker;

	broker = argument;
	for (;;) {
		struct vmm_vsock_abi_request request;
		ssize_t size;
		int running;

		size = recv(broker->own_control_fd, &request, sizeof(request), 0);
		if (size <= 0)
			break;
		pthread_mutex_lock(&broker->own_mutex);
		running = broker->mut_running;
		pthread_mutex_unlock(&broker->own_mutex);
		if (!running)
			break;
		if (size != sizeof(request) ||
		    le32toh(request.header.le_magic) != VMM_VSOCK_ABI_MAGIC ||
		    le16toh(request.header.le_version) != VMM_VSOCK_ABI_VERSION ||
		    le32toh(request.header.le_size) != sizeof(request)) {
			(void)virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_COMPLETE, 0, 0, EPROTO, -1, 0);
			continue;
		}
		(void)virtiod_vsock_broker_request(broker, &request);
	}
	return NULL;
}

static int
virtiod_vsock_broker_request(struct virtiod_vsock_broker *broker,
    const struct vmm_vsock_abi_request *request)
{
	struct virtiod_vsock_state *state;
	struct virtiod_vsock_listener *listener;
	struct virtiod_vsock_flow *flow;
	uint16_t type;
	uint64_t cid;
	uint64_t sequence;
	uint32_t port;
	int error;
	int sockets[2];

	type = le16toh(request->header.le_type);
	cid = le64toh(request->le_cid);
	port = le32toh(request->le_port);
	sequence = le64toh(request->header.le_sequence);
	if (sequence == 0 || port == 0)
		return virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, EINVAL, -1, port);
	pthread_mutex_lock(&broker->own_mutex);
	state = broker->weak_mut_state;
	if (state == NULL) {
		pthread_mutex_unlock(&broker->own_mutex);
		return virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, ENXIO, -1, port);
	}
	if (type == VMM_VSOCK_ABI_MSG_CONNECT) {
		if ((state != NULL && cid !=
		    le64toh(state->own_mut_config.le_guest_cid)) ||
		    socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
			error = state != NULL && cid !=
			    le64toh(state->own_mut_config.le_guest_cid) ?
			    EINVAL : errno;
			pthread_mutex_unlock(&broker->own_mutex);
			return virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_CONNECTED, sequence, cid, error, -1, port);
		}
		flow = calloc(1, sizeof(*flow));
		if (flow == NULL) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			pthread_mutex_unlock(&broker->own_mutex);
			return virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_CONNECTED, sequence, cid, ENOMEM, -1, port);
		}
		error = virtiod_vsock_broker_set_nonblock(sockets[1]);
		if (error != 0) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			free(flow);
			pthread_mutex_unlock(&broker->own_mutex);
			return virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_CONNECTED, sequence, cid, error, -1, port);
		}
		TAILQ_INIT(&flow->own_mut_to_host);
		flow->borrow_mut_state = state;
		flow->imm_guest_cid = cid;
		flow->imm_guest_port = port;
		flow->imm_host_port = broker->mut_next_port++;
		flow->imm_request_sequence = sequence;
		flow->own_backend_fd = sockets[1];
		flow->own_return_fd = sockets[0];
		flow->imm_origin = VIRTIOD_VSOCK_FLOW_HOST_CONNECT;
		TAILQ_INSERT_TAIL(&broker->own_mut_flows, flow, entry);
		if (state != NULL) {
			error = virtiod_vsock_broker_queue_packet_locked(broker, flow,
			    VIRTIOD_VSOCK_OP_REQUEST, 0, NULL, 0);
		} else {
			error = 0;
		}
		if (error != 0)
			virtiod_vsock_broker_close_flow_locked(broker, flow);
		pthread_mutex_unlock(&broker->own_mutex);
		return error;
	}
	if (cid != VMM_VSOCK_HOST_CID) {
		pthread_mutex_unlock(&broker->own_mutex);
		return virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, EINVAL, -1, port);
	}
	listener = virtiod_vsock_broker_find_listener_locked(broker, port);
	if (type == VMM_VSOCK_ABI_MSG_LISTEN) {
		if (listener != NULL)
			error = EADDRINUSE;
		else {
			listener = calloc(1, sizeof(*listener));
			if (listener == NULL)
				error = ENOMEM;
			else {
				listener->imm_port = port;
				TAILQ_INSERT_TAIL(&broker->own_mut_listeners, listener, entry);
				error = 0;
			}
		}
		pthread_mutex_unlock(&broker->own_mutex);
		return virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, error, -1, port);
	}
	if (type == VMM_VSOCK_ABI_MSG_UNLISTEN) {
		if (listener == NULL)
			error = ENOENT;
		else {
			TAILQ_REMOVE(&broker->own_mut_listeners, listener, entry);
			free(listener);
			error = 0;
		}
		pthread_mutex_unlock(&broker->own_mutex);
		return virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, error, -1, port);
	}
	if (type == VMM_VSOCK_ABI_MSG_ACCEPT) {
		if (listener == NULL || listener->mut_accept_sequence != 0)
			error = listener == NULL ? ENOENT : EBUSY;
		else {
			TAILQ_FOREACH(flow, &broker->own_mut_flows, entry) {
				if (flow->borrow_mut_state == state &&
				    flow->imm_origin == VIRTIOD_VSOCK_FLOW_GUEST_CONNECT &&
				    flow->imm_host_port == port && flow->own_return_fd >= 0)
					break;
			}
			if (flow == NULL) {
				listener->mut_accept_sequence = sequence;
				error = 0;
			} else {
				error = virtiod_vsock_broker_send_result(broker,
				    VMM_VSOCK_ABI_MSG_ACCEPTED, sequence,
				    VMM_VSOCK_HOST_CID, 0, flow->own_return_fd, port);
				if (error == 0) {
					(void)close(flow->own_return_fd);
					flow->own_return_fd = -1;
				}
			}
		}
		pthread_mutex_unlock(&broker->own_mutex);
		if (error != 0)
			return virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_ACCEPTED, sequence, cid, error, -1, port);
		return 0;
	}
	pthread_mutex_unlock(&broker->own_mutex);
	return virtiod_vsock_broker_send_result(broker,
	    VMM_VSOCK_ABI_MSG_COMPLETE, sequence, cid, EINVAL, -1, port);
}

static int
virtiod_vsock_broker_guest_packet(struct virtiod_vsock_state *state,
    const struct virtiod_vsock_header *header, const uint8_t *payload,
    size_t length)
{
	struct virtiod_vsock_broker *broker;
	struct virtiod_vsock_listener *listener;
	struct virtiod_vsock_flow *flow;
	struct virtiod_vsock_bytes *bytes;
	uint16_t op;
	uint64_t source_cid;
	uint64_t destination_cid;
	uint32_t source_port;
	uint32_t destination_port;
	uint64_t sequence;
	int sockets[2];
	int error;

	if (state == NULL || header == NULL ||
	    le16toh(header->le_type) != VIRTIOD_VSOCK_TYPE_STREAM ||
	    le32toh(header->le_len) != length)
		return EPROTO;
	broker = state->borrow_mut_broker;
	source_cid = le64toh(header->le_src_cid);
	destination_cid = le64toh(header->le_dst_cid);
	source_port = le32toh(header->le_src_port);
	destination_port = le32toh(header->le_dst_port);
	op = le16toh(header->le_op);
	if (__atomic_exchange_n(&state->atomic_mut_first_guest_packet_logged, 1,
	    __ATOMIC_ACQ_REL) == 0) {
		fprintf(stderr, "virtiod tsc=%020" PRIu64
		    " vsock guest_packet op=%u src=%" PRIu64 ":%u dst=%" PRIu64
		    ":%u\n", (uint64_t)__builtin_ia32_rdtsc(), op, source_cid,
		    source_port,
		    destination_cid, destination_port);
	}
	if (source_cid != le64toh(state->own_mut_config.le_guest_cid) ||
	    destination_cid != VMM_VSOCK_HOST_CID || source_port == 0 ||
	    destination_port == 0)
		return EPROTO;
	pthread_mutex_lock(&broker->own_mutex);
	flow = virtiod_vsock_broker_find_flow_locked(broker, source_cid,
	    source_port, destination_port);
	if (op == VIRTIOD_VSOCK_OP_REQUEST) {
		listener = virtiod_vsock_broker_find_listener_locked(broker,
		    destination_port);
		if (flow != NULL || listener == NULL) {
			struct virtiod_vsock_flow reject;

			memset(&reject, 0, sizeof(reject));
			reject.borrow_mut_state = state;
			reject.imm_guest_cid = source_cid;
			reject.imm_guest_port = source_port;
			reject.imm_host_port = destination_port;
			error = virtiod_vsock_broker_queue_packet_locked(broker, &reject,
			    VIRTIOD_VSOCK_OP_RST, 0, NULL, 0);
			pthread_mutex_unlock(&broker->own_mutex);
			return error;
		}
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
			error = errno;
			pthread_mutex_unlock(&broker->own_mutex);
			return error;
		}
		error = virtiod_vsock_broker_set_nonblock(sockets[1]);
		if (error != 0) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			pthread_mutex_unlock(&broker->own_mutex);
			return error;
		}
		flow = calloc(1, sizeof(*flow));
		if (flow == NULL) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			pthread_mutex_unlock(&broker->own_mutex);
			return ENOMEM;
		}
		TAILQ_INIT(&flow->own_mut_to_host);
		flow->borrow_mut_state = state;
		flow->imm_guest_cid = source_cid;
		flow->imm_guest_port = source_port;
		flow->imm_host_port = destination_port;
		flow->own_backend_fd = sockets[1];
		flow->own_return_fd = sockets[0];
		flow->imm_origin = VIRTIOD_VSOCK_FLOW_GUEST_CONNECT;
		flow->mut_open = 1;
		TAILQ_INSERT_TAIL(&broker->own_mut_flows, flow, entry);
		error = virtiod_vsock_broker_queue_packet_locked(broker, flow,
		    VIRTIOD_VSOCK_OP_RESPONSE, 0, NULL, 0);
		if (listener->mut_accept_sequence != 0) {
			sequence = listener->mut_accept_sequence;
			listener->mut_accept_sequence = 0;
			(void)virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_ACCEPTED, sequence, VMM_VSOCK_HOST_CID,
			    error, flow->own_return_fd, destination_port);
			if (error == 0) {
				(void)close(flow->own_return_fd);
				flow->own_return_fd = -1;
			}
		}
		pthread_mutex_unlock(&broker->own_mutex);
		return error;
	}
	if (flow == NULL) {
		struct virtiod_vsock_flow reject;

		memset(&reject, 0, sizeof(reject));
		reject.borrow_mut_state = state;
		reject.imm_guest_cid = source_cid;
		reject.imm_guest_port = source_port;
		reject.imm_host_port = destination_port;
		error = virtiod_vsock_broker_queue_packet_locked(broker, &reject,
		    VIRTIOD_VSOCK_OP_RST, 0, NULL, 0);
		pthread_mutex_unlock(&broker->own_mutex);
		return error;
	}
	flow->mut_peer_buf_alloc = le32toh(header->le_buf_alloc);
	flow->mut_peer_fwd_cnt = le32toh(header->le_fwd_cnt);
	if (op == VIRTIOD_VSOCK_OP_RESPONSE &&
	    flow->imm_origin == VIRTIOD_VSOCK_FLOW_HOST_CONNECT) {
		if (source_port == 1024 &&
		    __atomic_exchange_n(&state->atomic_mut_agent_response_logged, 1,
		    __ATOMIC_ACQ_REL) == 0) {
			fprintf(stderr, "virtiod tsc=%020" PRIu64
			    " vsock agent_response cid=%" PRIu64 " port=%u\n",
			    (uint64_t)__builtin_ia32_rdtsc(), source_cid, source_port);
		}
		flow->mut_open = 1;
		sequence = flow->imm_request_sequence;
		flow->imm_request_sequence = 0;
		error = virtiod_vsock_broker_send_result(broker,
		    VMM_VSOCK_ABI_MSG_CONNECTED, sequence, source_cid, 0,
		    flow->own_return_fd, flow->imm_host_port);
		if (error == 0) {
			(void)close(flow->own_return_fd);
			flow->own_return_fd = -1;
		}
		pthread_mutex_unlock(&broker->own_mutex);
		return error;
	}
	if (op == VIRTIOD_VSOCK_OP_RST) {
		if (flow->imm_request_sequence != 0)
			(void)virtiod_vsock_broker_send_result(broker,
			    VMM_VSOCK_ABI_MSG_CONNECTED, flow->imm_request_sequence,
			    source_cid, ECONNREFUSED, -1, source_port);
		virtiod_vsock_broker_close_flow_locked(broker, flow);
		pthread_mutex_unlock(&broker->own_mutex);
		return 0;
	}
	if (op == VIRTIOD_VSOCK_OP_SHUTDOWN) {
		(void)shutdown(flow->own_backend_fd, SHUT_RDWR);
		pthread_mutex_unlock(&broker->own_mutex);
		return 0;
	}
	if (op == VIRTIOD_VSOCK_OP_CREDIT_REQUEST) {
		error = virtiod_vsock_broker_queue_packet_locked(broker, flow,
		    VIRTIOD_VSOCK_OP_CREDIT_UPDATE, 0, NULL, 0);
		pthread_mutex_unlock(&broker->own_mutex);
		return error;
	}
	if (op != VIRTIOD_VSOCK_OP_RW || !flow->mut_open || length == 0) {
		pthread_mutex_unlock(&broker->own_mutex);
		return op == VIRTIOD_VSOCK_OP_CREDIT_UPDATE ? 0 : EPROTO;
	}
	bytes = malloc(sizeof(*bytes) + length);
	if (bytes == NULL) {
		pthread_mutex_unlock(&broker->own_mutex);
		return ENOMEM;
	}
	bytes->mut_offset = 0;
	bytes->imm_length = length;
	memcpy(bytes->own_bytes, payload, length);
	TAILQ_INSERT_TAIL(&flow->own_mut_to_host, bytes, entry);
	pthread_mutex_unlock(&broker->own_mutex);
	return 0;
}

static int
virtiod_vsock_broker_take_packet(struct virtiod_vsock_state *state,
    struct virtiod_vsock_packet **result)
{
	struct virtiod_vsock_broker *broker;
	struct virtiod_vsock_packet *packet;

	if (state == NULL || result == NULL)
		return EINVAL;
	broker = state->borrow_mut_broker;
	pthread_mutex_lock(&broker->own_mutex);
	packet = TAILQ_FIRST(&state->own_mut_packets);
	if (packet != NULL)
		TAILQ_REMOVE(&state->own_mut_packets, packet, entry);
	pthread_mutex_unlock(&broker->own_mutex);
	if (packet == NULL)
		return ENOENT;
	*result = packet;
	return 0;
}

static void
virtiod_vsock_broker_poll(struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_broker *broker;
	struct virtiod_vsock_flow *flow;

	broker = state->borrow_mut_broker;
	pthread_mutex_lock(&broker->own_mutex);
	TAILQ_FOREACH(flow, &broker->own_mut_flows, entry) {
		struct virtiod_vsock_bytes *bytes;
		uint32_t available;
		uint32_t in_flight;
		ssize_t length;

		if (flow->borrow_mut_state != state || !flow->mut_open)
			continue;
		bytes = TAILQ_FIRST(&flow->own_mut_to_host);
		if (bytes != NULL) {
			length = write(flow->own_backend_fd,
			    bytes->own_bytes + bytes->mut_offset,
			    bytes->imm_length - bytes->mut_offset);
			if (length > 0) {
				bytes->mut_offset += (size_t)length;
				flow->mut_rx_fwd_cnt += (uint32_t)length;
				if (bytes->mut_offset == bytes->imm_length) {
					TAILQ_REMOVE(&flow->own_mut_to_host, bytes, entry);
					free(bytes);
				}
				(void)virtiod_vsock_broker_queue_packet_locked(broker,
				    flow, VIRTIOD_VSOCK_OP_CREDIT_UPDATE, 0, NULL, 0);
			} else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				(void)virtiod_vsock_broker_queue_packet_locked(broker,
				    flow, VIRTIOD_VSOCK_OP_RST, 0, NULL, 0);
				flow->mut_open = 0;
			}
		}
		in_flight = flow->mut_tx_cnt - flow->mut_peer_fwd_cnt;
		available = in_flight >= flow->mut_peer_buf_alloc ? 0 :
		    flow->mut_peer_buf_alloc - in_flight;
		if (available == 0)
			continue;
		if (available > VIRTIOD_VSOCK_PAYLOAD_MAX)
			available = VIRTIOD_VSOCK_PAYLOAD_MAX;
		uint8_t buffer[VIRTIOD_VSOCK_PAYLOAD_MAX];
		length = read(flow->own_backend_fd, buffer, available);
		if (length > 0) {
			flow->mut_tx_cnt += (uint32_t)length;
			(void)virtiod_vsock_broker_queue_packet_locked(broker, flow,
			    VIRTIOD_VSOCK_OP_RW, 0, buffer, (size_t)length);
		} else if (length == 0) {
			(void)virtiod_vsock_broker_queue_packet_locked(broker, flow,
			    VIRTIOD_VSOCK_OP_SHUTDOWN, VIRTIOD_VSOCK_SHUTDOWN_RCV |
			    VIRTIOD_VSOCK_SHUTDOWN_SEND, NULL, 0);
			flow->mut_open = 0;
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			(void)virtiod_vsock_broker_queue_packet_locked(broker, flow,
			    VIRTIOD_VSOCK_OP_RST, 0, NULL, 0);
			flow->mut_open = 0;
		}
	}
	pthread_mutex_unlock(&broker->own_mutex);
}

static void
virtiod_vsock_broker_close_flow_locked(struct virtiod_vsock_broker *broker,
    struct virtiod_vsock_flow *flow)
{
	struct virtiod_vsock_bytes *bytes;

	TAILQ_REMOVE(&broker->own_mut_flows, flow, entry);
	while ((bytes = TAILQ_FIRST(&flow->own_mut_to_host)) != NULL) {
		TAILQ_REMOVE(&flow->own_mut_to_host, bytes, entry);
		free(bytes);
	}
	if (flow->own_backend_fd >= 0)
		(void)close(flow->own_backend_fd);
	if (flow->own_return_fd >= 0)
		(void)close(flow->own_return_fd);
	free(flow);
}

static int
virtiod_vsock_broker_queue_packet_locked(struct virtiod_vsock_broker *broker,
    struct virtiod_vsock_flow *flow, uint16_t operation, uint32_t flags,
    const void *payload, size_t length)
{
	struct virtiod_vsock_packet *packet;
	struct virtiod_vsock_state *state;

	if (broker == NULL || flow == NULL || length > VIRTIOD_VSOCK_PAYLOAD_MAX ||
	    (length != 0 && payload == NULL))
		return EINVAL;
	state = flow->borrow_mut_state;
	if (state == NULL || broker->weak_mut_state != state)
		return ENXIO;
	packet = calloc(1, sizeof(*packet) + length);
	if (packet == NULL)
		return ENOMEM;
	packet->imm_header.le_src_cid = htole64(VMM_VSOCK_HOST_CID);
	packet->imm_header.le_dst_cid = htole64(flow->imm_guest_cid);
	packet->imm_header.le_src_port = htole32(flow->imm_host_port);
	packet->imm_header.le_dst_port = htole32(flow->imm_guest_port);
	packet->imm_header.le_len = htole32((uint32_t)length);
	packet->imm_header.le_type = htole16(VIRTIOD_VSOCK_TYPE_STREAM);
	packet->imm_header.le_op = htole16(operation);
	packet->imm_header.le_flags = htole32(flags);
	packet->imm_header.le_buf_alloc = htole32(VIRTIOD_VSOCK_BUFFER_SIZE);
	packet->imm_header.le_fwd_cnt = htole32(flow->mut_rx_fwd_cnt);
	packet->imm_length = length;
	if (length != 0)
		memcpy(packet->own_bytes, payload, length);
	TAILQ_INSERT_TAIL(&state->own_mut_packets, packet, entry);
	return 0;
}

static int
virtiod_vsock_broker_send_result(struct virtiod_vsock_broker *broker,
    uint16_t type, uint64_t sequence, uint64_t cid, uint32_t status,
    int fd, uint32_t port)
{
	char control[CMSG_SPACE(sizeof(fd))];
	struct vmm_vsock_abi_result result;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr message;
	ssize_t size;

	memset(&result, 0, sizeof(result));
	result.header.le_magic = htole32(VMM_VSOCK_ABI_MAGIC);
	result.header.le_version = htole16(VMM_VSOCK_ABI_VERSION);
	result.header.le_type = htole16(type);
	result.header.le_size = htole32(sizeof(result));
	result.header.le_sequence = htole64(sequence);
	result.le_cid = htole64(cid);
	result.le_port = htole32(port);
	result.le_status = htole32(status);
	memset(&message, 0, sizeof(message));
	iov.iov_base = &result;
	iov.iov_len = sizeof(result);
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	if (fd >= 0) {
		memset(control, 0, sizeof(control));
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	}
	pthread_mutex_lock(&broker->own_send_mutex);
	size = sendmsg(broker->own_control_fd, &message, 0);
	pthread_mutex_unlock(&broker->own_send_mutex);
	return size == sizeof(result) ? 0 : size < 0 ? errno : EPROTO;
}

static struct virtiod_vsock_flow *
virtiod_vsock_broker_find_flow_locked(struct virtiod_vsock_broker *broker,
    uint64_t cid, uint32_t guest_port, uint32_t host_port)
{
	struct virtiod_vsock_flow *flow;

	TAILQ_FOREACH(flow, &broker->own_mut_flows, entry) {
		if (flow->imm_guest_cid == cid && flow->imm_guest_port == guest_port &&
		    flow->imm_host_port == host_port)
			return flow;
	}
	return NULL;
}

static struct virtiod_vsock_listener *
virtiod_vsock_broker_find_listener_locked(struct virtiod_vsock_broker *broker,
    uint32_t port)
{
	struct virtiod_vsock_listener *listener;

	TAILQ_FOREACH(listener, &broker->own_mut_listeners, entry) {
		if (listener->imm_port == port)
			return listener;
	}
	return NULL;
}

static int
virtiod_vsock_broker_set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return errno;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : errno;
}

static void
virtiod_vsock_generation_fini(struct virtiod_vsock_state *state)
{
	struct virtiod_vsock_packet *packet;
	unsigned int i;

	if (!state->mut_generation_active && !state->mut_broker_attached &&
	    state->own_mut_bar == NULL && state->own_bar_fd < 0 &&
	    state->own_dma_fd < 0)
		return;
	pthread_mutex_lock(&state->own_mutex);
	state->mut_running = 0;
	pthread_mutex_unlock(&state->own_mutex);
	for (i = 0; i < state->mut_thread_count; i++) {
		(void)pthread_join(state->own_mut_threads[i], NULL);
	}
	state->mut_thread_count = 0;
	if (state->mut_broker_attached) {
		virtiod_vsock_broker_detach(state->borrow_mut_broker, state);
		state->mut_broker_attached = 0;
	}
	while ((packet = TAILQ_FIRST(&state->own_mut_packets)) != NULL) {
		TAILQ_REMOVE(&state->own_mut_packets, packet, entry);
		free(packet);
	}
	virtiod_vsock_unmap_dma(state);
	if (state->own_mut_bar != NULL) {
		(void)munmap(state->own_mut_bar, VIRTIOD_VSOCK_BAR_SIZE);
		state->own_mut_bar = NULL;
	}
	if (state->own_bar_fd >= 0) {
		(void)close(state->own_bar_fd);
		state->own_bar_fd = -1;
	}
	if (state->own_dma_fd >= 0) {
		(void)close(state->own_dma_fd);
		state->own_dma_fd = -1;
	}
	state->mut_generation_active = 0;
}

static void
virtiod_vsock_state_fini(struct virtiod_vsock_state *state)
{

	if (state == NULL)
		return;
	virtiod_vsock_generation_fini(state);
	if (state->own_provider_fd >= 0)
		(void)close(state->own_provider_fd);
	state->own_provider_fd = -1;
	(void)pthread_mutex_destroy(&state->own_mutex);
}

static int
virtiod_vsock_header_valid(const struct vmm_pcie_abi_header *header,
    uint16_t type, size_t size)
{

	return header != NULL && le32toh(header->le_magic) == VMM_PCIE_ABI_MAGIC &&
	    le16toh(header->le_version) == VMM_PCIE_ABI_VERSION &&
	    le16toh(header->le_type) == type && le32toh(header->le_size) == size;
}
