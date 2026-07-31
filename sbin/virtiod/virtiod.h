/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VIRTIOD_H
#define VIRTIOD_H

#include <sys/types.h>

#include <stdint.h>
#include <sys/uio.h>

#define VIRTIOD_QUEUE_SIZE 128U
#define VIRTIOD_MAX_DMA_SEGMENTS 32U
#define VIRTIOD_MAX_CHAIN 128U

#define VIRTIOD_DESC_F_NEXT 0x0001U
#define VIRTIOD_DESC_F_WRITE 0x0002U
#define VIRTIOD_DESC_F_INDIRECT 0x0004U

struct virtiod_dma_segment {
	uint64_t imm_gpa;
	uint64_t imm_size;
	uint8_t *own_mut_bytes;
};

struct virtiod_vring {
	struct virtiod_dma_segment *borrow_imm_dma;
	unsigned int imm_dma_count;
	uint16_t mut_size;
	uint16_t mut_last_avail;
	uint16_t mut_next_used;
	uint64_t mut_desc_gpa;
	uint64_t mut_avail_gpa;
	uint64_t mut_used_gpa;
};

struct virtiod_chain {
	uint16_t imm_head;
	unsigned int mut_iov_count;
	unsigned int mut_readable_count;
	unsigned int mut_writable_count;
	struct iovec own_mut_iov[VIRTIOD_MAX_CHAIN];
	uint16_t own_mut_flags[VIRTIOD_MAX_CHAIN];
};

struct virtiod_block {
	int own_fd;
	int imm_read_only;
	uint64_t imm_size;
};

#define VIRTIOD_BLK_T_IN 0U
#define VIRTIOD_BLK_T_OUT 1U
#define VIRTIOD_BLK_T_FLUSH 4U

#define VIRTIOD_BLK_S_OK 0U
#define VIRTIOD_BLK_S_IOERR 1U
#define VIRTIOD_BLK_S_UNSUPP 2U

int virtiod_dma_translate(const struct virtiod_dma_segment *, unsigned int,
    uint64_t, size_t, void **);
int virtiod_vring_configure(struct virtiod_vring *,
    struct virtiod_dma_segment *, unsigned int, uint16_t, uint64_t,
    uint64_t, uint64_t);
int virtiod_vring_pop(struct virtiod_vring *, struct virtiod_chain *);
int virtiod_vring_complete(struct virtiod_vring *, const struct virtiod_chain *,
    uint32_t);
int virtiod_block_open(struct virtiod_block *, const char *);
void virtiod_block_close(struct virtiod_block *);
int virtiod_block_request(struct virtiod_block *, uint32_t, uint64_t,
    const struct iovec *, unsigned int, uint8_t *);

#endif /* VIRTIOD_H */
