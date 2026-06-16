/*
 * RwFenceSet — usage-classed (READ/WRITE) fence container.
 *
 * Replaces reservation_object for content/VM fence tracking (see
 * nvkm-core-spec.md §7). Leaf lock: a native DragonFly struct spinlock
 * (NOT the linuxkpi spinlock_t, which is a sleepable lockmgr). The lock is
 * only ever held for short, non-sleeping critical sections; all blocking
 * waits happen after the lock is dropped.
 *
 * Two buckets:
 *   writes  ~ old "exclusive" fences
 *   reads   ~ old "shared" fences
 * Wait rules (snapshot intent):
 *   RW_READ  (a new reader) waits all writes
 *   RW_WRITE (a new writer) waits all writes + reads
 *
 * Each add() drops already-signalled fences and any same-context fence with
 * a lower seqno (it is subsumed by the newer one), so a bucket holds at most
 * one fence per fence-context — i.e. ~one per channel. A small fixed inline
 * array therefore suffices and no allocation is ever done under the lock.
 */
#ifndef NVKM_RWFENCE_H
#define NVKM_RWFENCE_H

#include <sys/param.h>
#include <sys/spinlock.h>
#include <linux/dma-fence.h>

enum rw_usage {
	RW_READ = 0,
	RW_WRITE = 1,
};

/* Per-bucket inline capacity. Bound is "distinct in-flight fence contexts on
 * one set", which is the number of channels with unsignalled work — tiny. */
#define RWFS_BUCKET_CAP 16

struct rw_fence_set {
	struct spinlock lock;			/* L1, leaf, non-sleeping */
	struct dma_fence *writes[RWFS_BUCKET_CAP];
	struct dma_fence *reads[RWFS_BUCKET_CAP];
	uint32_t writes_len;
	uint32_t reads_len;
	uint64_t overflow_count;		/* diagnostics: bucket-full drops */
};

void rwfs_init(struct rw_fence_set *set);
void rwfs_fini(struct rw_fence_set *set);

/* Record an in-flight access. Borrows f; takes its own reference. */
void rwfs_add(struct rw_fence_set *set, struct dma_fence *f, enum rw_usage usage);

/* Collect the fences a new accessor with @intent must wait on into @out
 * (each returned with a reference held). Returns the count written; if the
 * snapshot needs more than @cap slots it returns the required count and
 * writes nothing (caller grows and retries). */
uint32_t rwfs_snapshot(struct rw_fence_set *set, enum rw_usage intent,
    struct dma_fence **out, uint32_t cap);

/* Snapshot then block (outside the lock) on each fence. Returns the
 * remaining jiffies (>0), 0 on timeout, or -ERESTARTSYS if interrupted. */
long rwfs_wait(struct rw_fence_set *set, enum rw_usage intent, bool intr,
    long timeout);

/* Self-test: validates add/snapshot/prune/subsume logic with stub fences.
 * Returns 0 on success, negative on failure; logs detail via kprintf. */
int rwfs_selftest(void);

#endif /* NVKM_RWFENCE_H */
