/* SPDX-License-Identifier: MIT */
/*
 * DragonFly io_uring UAPI: the frozen submission/completion queue entry
 * layouts, opcode and flag namespace.  Layouts are byte-compatible with the
 * upstream Linux io_uring UAPI so io_uring programs recompile unchanged.
 */
#ifndef DF_IO_URING_H
#define DF_IO_URING_H

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
typedef int32_t  __kernel_rwf_t;

/* Submission Queue Entry, 64 bytes. */
struct io_uring_sqe {
	__u8	opcode;
	__u8	flags;
	__u16	ioprio;
	__s32	fd;
	union {
		__u64	off;
		__u64	addr2;
		struct {
			__u32	cmd_op;
			__u32	__pad1;
		};
	};
	union {
		__u64	addr;
		__u64	splice_off_in;
		struct {
			__u32	level;
			__u32	optname;
		};
	};
	__u32	len;
	union {
		__kernel_rwf_t	rw_flags;
		__u32	fsync_flags;
		__u16	poll_events;
		__u32	poll32_events;
		__u32	sync_range_flags;
		__u32	msg_flags;
		__u32	timeout_flags;
		__u32	accept_flags;
		__u32	cancel_flags;
		__u32	open_flags;
		__u32	statx_flags;
		__u32	fadvise_advice;
		__u32	splice_flags;
		__u32	rename_flags;
		__u32	unlink_flags;
		__u32	hardlink_flags;
		__u32	xattr_flags;
		__u32	msg_ring_flags;
		__u32	uring_cmd_flags;
		__u32	waitid_flags;
		__u32	futex_flags;
		__u32	install_fd_flags;
		__u32	nop_flags;
		__u32	pipe_flags;
	};
	__u64	user_data;
	union {
		__u16	buf_index;
		__u16	buf_group;
	} __attribute__((packed));
	__u16	personality;
	union {
		__s32	splice_fd_in;
		__u32	file_index;
		__u32	zcrx_ifq_idx;
		__u32	optlen;
		struct {
			__u16	addr_len;
			__u16	__pad3[1];
		};
	};
	union {
		struct {
			__u64	addr3;
			__u64	__pad2[1];
		};
		struct {
			__u64	attr_ptr;
			__u64	attr_type_mask;
		};
		__u64	optval;
		__u8	cmd[0];
	};
};

enum io_uring_sqe_flags_bit {
	IOSQE_FIXED_FILE_BIT,
	IOSQE_IO_DRAIN_BIT,
	IOSQE_IO_LINK_BIT,
	IOSQE_IO_HARDLINK_BIT,
	IOSQE_ASYNC_BIT,
	IOSQE_BUFFER_SELECT_BIT,
	IOSQE_CQE_SKIP_SUCCESS_BIT,
};

#define IOSQE_FIXED_FILE	(1U << IOSQE_FIXED_FILE_BIT)
#define IOSQE_IO_DRAIN		(1U << IOSQE_IO_DRAIN_BIT)
#define IOSQE_IO_LINK		(1U << IOSQE_IO_LINK_BIT)
#define IOSQE_IO_HARDLINK	(1U << IOSQE_IO_HARDLINK_BIT)
#define IOSQE_ASYNC		(1U << IOSQE_ASYNC_BIT)
#define IOSQE_BUFFER_SELECT	(1U << IOSQE_BUFFER_SELECT_BIT)
#define IOSQE_CQE_SKIP_SUCCESS	(1U << IOSQE_CQE_SKIP_SUCCESS_BIT)

#define IORING_SETUP_IOPOLL	(1U << 0)
#define IORING_SETUP_SQPOLL	(1U << 1)
#define IORING_SETUP_SQ_AFF	(1U << 2)
#define IORING_SETUP_CQSIZE	(1U << 3)
#define IORING_SETUP_CLAMP	(1U << 4)
#define IORING_SETUP_ATTACH_WQ	(1U << 5)
#define IORING_SETUP_R_DISABLED	(1U << 6)
#define IORING_SETUP_SUBMIT_ALL	(1U << 7)

enum io_uring_op {
	IORING_OP_NOP,
	IORING_OP_READV,
	IORING_OP_WRITEV,
	IORING_OP_FSYNC,
	IORING_OP_READ_FIXED,
	IORING_OP_WRITE_FIXED,
	IORING_OP_POLL_ADD,
	IORING_OP_POLL_REMOVE,
	IORING_OP_SYNC_FILE_RANGE,
	IORING_OP_SENDMSG,
	IORING_OP_RECVMSG,
	IORING_OP_TIMEOUT,
	IORING_OP_TIMEOUT_REMOVE,
	IORING_OP_ACCEPT,
	IORING_OP_ASYNC_CANCEL,
	IORING_OP_LINK_TIMEOUT,
	IORING_OP_CONNECT,
	IORING_OP_FALLOCATE,
	IORING_OP_OPENAT,
	IORING_OP_CLOSE,
	IORING_OP_FILES_UPDATE,
	IORING_OP_STATX,
	IORING_OP_READ,
	IORING_OP_WRITE,
	IORING_OP_FADVISE,
	IORING_OP_MADVISE,
	IORING_OP_SEND,
	IORING_OP_RECV,
	IORING_OP_OPENAT2,
	IORING_OP_EPOLL_CTL,
	IORING_OP_SPLICE,
	IORING_OP_PROVIDE_BUFFERS,
	IORING_OP_REMOVE_BUFFERS,
	IORING_OP_TEE,
	IORING_OP_SHUTDOWN,
	IORING_OP_RENAMEAT,
	IORING_OP_UNLINKAT,
	IORING_OP_MKDIRAT,
	IORING_OP_SYMLINKAT,
	IORING_OP_LINKAT,
	IORING_OP_MSG_RING,
	IORING_OP_FSETXATTR,
	IORING_OP_SETXATTR,
	IORING_OP_FGETXATTR,
	IORING_OP_GETXATTR,
	IORING_OP_SOCKET,
	IORING_OP_URING_CMD,
	IORING_OP_SEND_ZC,
	IORING_OP_SENDMSG_ZC,
	IORING_OP_READ_MULTISHOT,
	IORING_OP_WAITID,
	IORING_OP_FUTEX_WAIT,
	IORING_OP_FUTEX_WAKE,
	IORING_OP_FUTEX_WAITV,
	IORING_OP_FIXED_FD_INSTALL,
	IORING_OP_FTRUNCATE,
	IORING_OP_BIND,
	IORING_OP_LISTEN,
	IORING_OP_RECV_ZC,
	IORING_OP_EPOLL_WAIT,
	IORING_OP_READV_FIXED,
	IORING_OP_WRITEV_FIXED,
	IORING_OP_PIPE,
	IORING_OP_NOP128,
	IORING_OP_URING_CMD128,
	IORING_OP_LAST,
};

/* Completion Queue Entry, 16 bytes. */
struct io_uring_cqe {
	__u64	user_data;
	__s32	res;
	__u32	flags;
	__u64	big_cqe[];
};

#define IORING_CQE_F_BUFFER		(1U << 0)
#define IORING_CQE_F_MORE		(1U << 1)
#define IORING_CQE_F_SOCK_NONEMPTY	(1U << 2)
#define IORING_CQE_F_NOTIF		(1U << 3)

#ifdef __cplusplus
}
#endif

#endif
