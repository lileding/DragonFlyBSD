/*
 * RwFenceSet — usage-classed (READ/WRITE) fence container.
 * See nvkm-core-spec.md §7 and nvkm_rwfence.h.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/spinlock2.h>

#include <linux/dma-fence.h>

#include "nvkm_rwfence.h"

void
rwfs_init(struct rw_fence_set *set)
{
	spin_init(&set->lock, "nvrwfs");
	set->writes_len = 0;
	set->reads_len = 0;
	set->overflow_count = 0;
}

static void
rwfs_bucket_drop_all(struct dma_fence **bucket, uint32_t *len)
{
	for (uint32_t i = 0; i < *len; i++)
		dma_fence_put(bucket[i]);
	*len = 0;
}

void
rwfs_fini(struct rw_fence_set *set)
{
	spin_lock(&set->lock);
	rwfs_bucket_drop_all(set->writes, &set->writes_len);
	rwfs_bucket_drop_all(set->reads, &set->reads_len);
	spin_unlock(&set->lock);
	spin_uninit(&set->lock);
}

/* Drop already-signalled fences (lockless signalled-bit check, no L3 taken).
 * Caller holds set->lock. */
static void
rwfs_prune(struct dma_fence **bucket, uint32_t *len)
{
	uint32_t i = 0;

	while (i < *len) {
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &bucket[i]->flags)) {
			dma_fence_put(bucket[i]);
			bucket[i] = bucket[--(*len)];	/* swap-remove */
		} else {
			i++;
		}
	}
}

void
rwfs_add(struct rw_fence_set *set, struct dma_fence *f, enum rw_usage usage)
{
	struct dma_fence **bucket;
	uint32_t *len, cap = RWFS_BUCKET_CAP;
	uint32_t i;

	if (f == NULL)
		return;

	spin_lock(&set->lock);
	if (usage == RW_WRITE) {
		bucket = set->writes;
		len = &set->writes_len;
	} else {
		bucket = set->reads;
		len = &set->reads_len;
	}

	rwfs_prune(bucket, len);

	/*
	 * Context-subsume: within a fence context seqnos are monotonic, so a
	 * later fence implies all earlier ones in that context have signalled.
	 * Drop same-context older fences; if a same-context fence already
	 * covers f, f is redundant. This bounds a bucket to one fence per
	 * context (~one per channel).
	 */
	for (i = 0; i < *len; i++) {
		if (bucket[i]->context != f->context)
			continue;
		if (bucket[i]->seqno >= f->seqno) {
			/* existing fence subsumes f */
			spin_unlock(&set->lock);
			return;
		}
		/* f subsumes bucket[i] */
		dma_fence_put(bucket[i]);
		bucket[i] = bucket[--(*len)];
		break;
	}

	if (*len >= cap) {
		/* Should not happen given subsumption; best-effort drop oldest. */
		set->overflow_count++;
		dma_fence_put(bucket[0]);
		bucket[0] = bucket[--(*len)];
	}

	bucket[(*len)++] = dma_fence_get(f);
	spin_unlock(&set->lock);
}

uint32_t
rwfs_snapshot(struct rw_fence_set *set, enum rw_usage intent,
    struct dma_fence **out, uint32_t cap)
{
	uint32_t need, n = 0, i;

	spin_lock(&set->lock);
	rwfs_prune(set->writes, &set->writes_len);
	if (intent == RW_WRITE)
		rwfs_prune(set->reads, &set->reads_len);

	/* RW_READ waits writes; RW_WRITE waits writes + reads. */
	need = set->writes_len + (intent == RW_WRITE ? set->reads_len : 0);
	if (need > cap) {
		spin_unlock(&set->lock);
		return need;
	}

	for (i = 0; i < set->writes_len; i++)
		out[n++] = dma_fence_get(set->writes[i]);
	if (intent == RW_WRITE) {
		for (i = 0; i < set->reads_len; i++)
			out[n++] = dma_fence_get(set->reads[i]);
	}
	spin_unlock(&set->lock);
	return n;
}

long
rwfs_wait(struct rw_fence_set *set, enum rw_usage intent, bool intr,
    long timeout)
{
	struct dma_fence *out[2 * RWFS_BUCKET_CAP];
	uint32_t n, i;
	long ret = timeout;

	/*
	 * A snapshot returns at most writes_len + reads_len fences, both bounded
	 * by RWFS_BUCKET_CAP, so the stack array always fits and the cap is never
	 * exceeded. No dynamic allocation is needed.
	 */
	n = rwfs_snapshot(set, intent, out, 2 * RWFS_BUCKET_CAP);
	for (i = 0; i < n; i++) {
		if (ret > 0)
			ret = dma_fence_wait_timeout(out[i], intr, ret);
		else if (ret == 0)
			(void)dma_fence_wait_timeout(out[i], intr, 0);
		dma_fence_put(out[i]);
	}
	return ret;
}

/* ======================== self-test ======================== */

static const char *
rwfs_test_fence_str(struct dma_fence *f)
{
	return "rwfs-test";
}

static const struct dma_fence_ops rwfs_test_fence_ops = {
	.get_driver_name = rwfs_test_fence_str,
	.get_timeline_name = rwfs_test_fence_str,
	.wait = dma_fence_default_wait,
};

#define RWFS_TEST_REQUIRE(cond, msg) do {				\
	if (!(cond)) {							\
		kprintf("rwfs_selftest: FAIL: %s\n", (msg));		\
		fail = -EINVAL;						\
		goto out;						\
	}								\
} while (0)

int
rwfs_selftest(void)
{
	static spinlock_t fl;			/* spinlock_t == struct lock */
	struct rw_fence_set set;
	struct dma_fence a1, a2, b1;
	struct dma_fence *out[2 * RWFS_BUCKET_CAP];
	uint64_t ctx_a, ctx_b;
	uint32_t n;
	int fail = 0;

	lockinit(&fl, "rwfstf", 0, 0);
	ctx_a = dma_fence_context_alloc(1);
	ctx_b = dma_fence_context_alloc(1);
	dma_fence_init(&a1, &rwfs_test_fence_ops, &fl, ctx_a, 1);
	dma_fence_init(&a2, &rwfs_test_fence_ops, &fl, ctx_a, 2);
	dma_fence_init(&b1, &rwfs_test_fence_ops, &fl, ctx_b, 1);

	rwfs_init(&set);

	/* add WRITE a1, READ b1 */
	rwfs_add(&set, &a1, RW_WRITE);
	rwfs_add(&set, &b1, RW_READ);

	/* a reader waits writes only => {a1} */
	n = rwfs_snapshot(&set, RW_READ, out, 2 * RWFS_BUCKET_CAP);
	RWFS_TEST_REQUIRE(n == 1, "reader snapshot should be 1 (writes only)");
	RWFS_TEST_REQUIRE(out[0] == &a1, "reader snapshot fence should be a1");
	for (uint32_t i = 0; i < n; i++)
		dma_fence_put(out[i]);

	/* a writer waits writes + reads => {a1, b1} */
	n = rwfs_snapshot(&set, RW_WRITE, out, 2 * RWFS_BUCKET_CAP);
	RWFS_TEST_REQUIRE(n == 2, "writer snapshot should be 2 (writes+reads)");
	for (uint32_t i = 0; i < n; i++)
		dma_fence_put(out[i]);

	/* add WRITE a2 (same ctx as a1, later seqno) => a1 subsumed, writes={a2} */
	rwfs_add(&set, &a2, RW_WRITE);
	RWFS_TEST_REQUIRE(set.writes_len == 1, "a2 should subsume a1 in writes");
	RWFS_TEST_REQUIRE(set.writes[0] == &a2, "writes[0] should be a2");

	/* signal a2, then snapshot should prune it => writes empty */
	dma_fence_signal(&a2);
	n = rwfs_snapshot(&set, RW_READ, out, 2 * RWFS_BUCKET_CAP);
	RWFS_TEST_REQUIRE(n == 0, "signalled a2 should be pruned to 0");

	/* wait on an all-signalled set returns timeout unchanged (>0) */
	dma_fence_signal(&b1);
	{
		long r = rwfs_wait(&set, RW_WRITE, false, 5 * hz);
		RWFS_TEST_REQUIRE(r > 0, "wait on signalled set should not time out");
	}

	kprintf("rwfs_selftest: PASS\n");
out:
	rwfs_fini(&set);
	/*
	 * The fences are stack objects holding only their init reference now
	 * (rwfs dropped every reference it took). We deliberately do NOT put
	 * that last reference: dropping to zero would invoke dma_fence_free()
	 * (kfree) on stack memory. Letting them go out of scope at refcount 1
	 * frees nothing and leaks nothing — the storage is the stack frame.
	 */
	return fail;
}
