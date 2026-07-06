/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly native nvkm per-file scheduler.
 */

#include "nvkm_sched.h"

#include <sys/systm.h>
#include <sys/thread2.h>
#include <linux/slab.h>

struct nvkm_sched_dep {
	TAILQ_ENTRY(nvkm_sched_dep) job_link;
	TAILQ_ENTRY(nvkm_sched_dep) dag_link;
	struct dma_fence *fence;
	struct nvkm_job *job;
};

TAILQ_HEAD(nvkm_sched_dag_waiter_list, nvkm_sched_dep);

struct nvkm_sched_dag_node {
	RB_ENTRY(nvkm_sched_dag_node) link;
	struct dma_fence *fence;
	struct nvkm_sched_dag_waiter_list waiters;
};

static int nvkm_sched_dag_node_cmp(struct nvkm_sched_dag_node *a,
    struct nvkm_sched_dag_node *b);
RB_PROTOTYPE_STATIC(nvkm_sched_dag_tree, nvkm_sched_dag_node, link,
    nvkm_sched_dag_node_cmp);
RB_GENERATE_STATIC(nvkm_sched_dag_tree, nvkm_sched_dag_node, link,
    nvkm_sched_dag_node_cmp);

static bool
nvkm_sched_stop_requested(struct nvkm_sched *sched)
{
	return (atomic_fetchadd_int(&sched->stop_requested, 0) != 0);
}

static int
nvkm_sched_dag_node_cmp(struct nvkm_sched_dag_node *a,
    struct nvkm_sched_dag_node *b)
{
	if ((uintptr_t)a->fence < (uintptr_t)b->fence)
		return (-1);
	if ((uintptr_t)a->fence > (uintptr_t)b->fence)
		return (1);
	return (0);
}

static struct nvkm_sched_dag_node *
nvkm_sched_dag_find_locked(struct nvkm_sched *sched, struct dma_fence *fence)
{
	struct nvkm_sched_dag_node key;

	key.fence = fence;
	return (RB_FIND(nvkm_sched_dag_tree, &sched->dag, &key));
}

static struct nvkm_sched_dag_node *
nvkm_sched_dag_ensure_locked(struct nvkm_sched *sched, struct dma_fence *fence)
{
	struct nvkm_sched_dag_node *node;

	node = nvkm_sched_dag_find_locked(sched, fence);
	if (node != NULL)
		return (node);

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (node == NULL)
		return (NULL);
	node->fence = fence;
	TAILQ_INIT(&node->waiters);
	RB_INSERT(nvkm_sched_dag_tree, &sched->dag, node);
	return (node);
}

static bool
nvkm_sched_job_is_parked_locked(struct nvkm_sched *sched, struct nvkm_job *job)
{
	struct nvkm_job *iter;

	TAILQ_FOREACH(iter, &sched->parked_jobs, sched_link) {
		if (iter == job)
			return (true);
	}
	return (false);
}

static void
nvkm_sched_job_remove_dep_locked(struct nvkm_sched *sched,
    struct nvkm_job *job, struct nvkm_sched_dep *dep)
{
	struct nvkm_sched_dag_node *node;

	node = nvkm_sched_dag_find_locked(sched, dep->fence);
	if (node != NULL) {
		TAILQ_REMOVE(&node->waiters, dep, dag_link);
		if (TAILQ_EMPTY(&node->waiters)) {
			RB_REMOVE(nvkm_sched_dag_tree, &sched->dag, node);
			kfree(node);
		}
	}
	TAILQ_REMOVE(&job->dep_fences, dep, job_link);
	dma_fence_put(dep->fence);
	kfree(dep);
}

static void
nvkm_sched_job_remove_all_deps_locked(struct nvkm_sched *sched,
    struct nvkm_job *job)
{
	struct nvkm_sched_dep *dep;

	while ((dep = TAILQ_FIRST(&job->dep_fences)) != NULL)
		nvkm_sched_job_remove_dep_locked(sched, job, dep);
}

static bool
nvkm_sched_job_has_dep(struct nvkm_job *job, struct dma_fence *fence)
{
	struct nvkm_sched_dep *dep;

	TAILQ_FOREACH(dep, &job->dep_fences, job_link) {
		if (dep->fence == fence)
			return (true);
	}
	return (false);
}

static void
nvkm_sched_move_parked_to_pollable_locked(struct nvkm_sched *sched,
    struct nvkm_job *job)
{
	if (!nvkm_sched_job_is_parked_locked(sched, job))
		return;
	TAILQ_REMOVE(&sched->parked_jobs, job, sched_link);
	TAILQ_INSERT_TAIL(&sched->pollable_jobs, job, sched_link);
	wakeup(sched);
}

static int
nvkm_sched_queue_event(struct nvkm_sched *sched, enum nvkm_sched_event_type type,
    struct dma_fence *fence, int result, int gfp)
{
	struct nvkm_sched_event *event;

	if (sched == NULL || fence == NULL)
		return (-EINVAL);

	event = kzalloc(sizeof(*event), gfp);
	if (event == NULL)
		return (-ENOMEM);
	event->type = type;
	event->fence = dma_fence_get(fence);
	event->result = result;

	lwkt_gettoken(&sched->token);
	TAILQ_INSERT_TAIL(&sched->events, event, link);
	wakeup(sched);
	lwkt_reltoken(&sched->token);
	return (0);
}

/*
 * nvkm_job_init()
 *
 * Ownership:
 *   Initializes caller-owned job storage.  The job takes a reference to
 *   done_fence when one is supplied.
 *
 * Lifetime:
 *   The caller owns the job until nvkm_sched_enqueue() succeeds.  After
 *   enqueue, only the scheduler may destroy it.
 *
 * Threading:
 *   Called before the job becomes visible to the scheduler.
 */
void
nvkm_job_init(struct nvkm_job *job,
    struct nvkm_job_future (*poll)(struct nvkm_sched *, struct nvkm_job *),
    struct dma_fence *done_fence)
{
	memset(job, 0, sizeof(*job));
	job->poll = poll;
	job->done_fence = dma_fence_get(done_fence);
	TAILQ_INIT(&job->dep_fences);
}

/*
 * nvkm_job_add_dep()
 *
 * Ownership:
 *   Borrows fence and stores one scheduler-owned reference if the dependency is
 *   unsignaled and not already present in the job.
 *
 * Lifetime:
 *   Dependencies live until the scheduler observes the fence completion or the
 *   caller tears down an unqueued job with nvkm_job_fini().
 *
 * Threading:
 *   Called before enqueue while the ioctl path still owns the job.
 */
int
nvkm_job_add_dep(struct nvkm_job *job, struct dma_fence *fence)
{
	struct nvkm_sched_dep *dep;

	if (job == NULL || fence == NULL)
		return (0);
	if (dma_fence_is_signaled(fence))
		return (0);
	if (nvkm_sched_job_has_dep(job, fence))
		return (0);

	dep = kzalloc(sizeof(*dep), GFP_KERNEL);
	if (dep == NULL)
		return (-ENOMEM);
	dep->fence = dma_fence_get(fence);
	dep->job = job;
	TAILQ_INSERT_TAIL(&job->dep_fences, dep, job_link);
	return (0);
}

/*
 * nvkm_job_fini()
 *
 * Ownership:
 *   Releases common references owned by an unqueued job or by the scheduler at
 *   terminal completion.
 *
 * Lifetime:
 *   The concrete job memory remains owned by the caller; this helper only tears
 *   down common fence references.
 *
 * Threading:
 *   The caller must ensure the job is not visible to scheduler lists.
 */
void
nvkm_job_fini(struct nvkm_job *job)
{
	struct nvkm_sched_dep *dep;

	if (job == NULL)
		return;
	while ((dep = TAILQ_FIRST(&job->dep_fences)) != NULL) {
		TAILQ_REMOVE(&job->dep_fences, dep, job_link);
		dma_fence_put(dep->fence);
		kfree(dep);
	}
	dma_fence_put(job->done_fence);
	job->done_fence = NULL;
}

/*
 * nvkm_sched_wake()
 *
 * Ownership:
 *   Borrows sched.  The caller must ensure the owning drm_file still keeps the
 *   scheduler storage alive.
 *
 * Lifetime:
 *   Safe from ioctl, interrupt, and scheduler contexts after nvkm_sched_start()
 *   and until nvkm_sched_run() returns.  Stop only requests teardown; it
 *   does not join the scheduler thread.
 *
 * Threading:
 *   MP-safe.  The scheduler token serializes callers with the scheduler loop;
 *   sched itself is the sleep channel.
 */
void
nvkm_sched_wake(struct nvkm_sched *sched)
{
	if (sched == NULL)
		return;

	lwkt_gettoken(&sched->token);
	wakeup(sched);
	lwkt_reltoken(&sched->token);
}

/*
 * nvkm_sched_wake_job()
 *
 * Ownership:
 *   Borrows sched and fence.  The queued event owns its own fence reference.
 *
 * Lifetime:
 *   Used by job-private external events, such as EXEC interrupt harvest, after
 *   the job has already been enqueued.
 *
 * Threading:
 *   MP-safe.  Allocation is GFP_ATOMIC so ithread paths do not sleep.
 */
void
nvkm_sched_wake_job(struct nvkm_sched *sched, struct dma_fence *fence,
    int result)
{
	(void)nvkm_sched_queue_event(sched, NVKM_COMPLETE_JOB, fence, result,
	    GFP_ATOMIC);
}

/*
 * nvkm_sched_fence_complete()
 *
 * Ownership:
 *   Borrows sched and fence.  The queued event owns its own fence reference.
 *
 * Lifetime:
 *   Used for exact fence DAG completion, including scheduler-to-scheduler IPI
 *   bridges and common done_fence propagation.
 *
 * Threading:
 *   MP-safe.  Allocation is GFP_ATOMIC so external wake sources do not sleep.
 */
void
nvkm_sched_fence_complete(struct nvkm_sched *sched, struct dma_fence *fence,
    int result)
{
	(void)nvkm_sched_queue_event(sched, NVKM_COMPLETE_FENCE, fence, result,
	    GFP_ATOMIC);
}

static void
nvkm_sched_process_job_event_locked(struct nvkm_sched *sched,
    struct nvkm_sched_event *event)
{
	struct nvkm_job *job;

	TAILQ_FOREACH(job, &sched->parked_jobs, sched_link) {
		if (job->done_fence != event->fence)
			continue;
		if (event->result != 0 && job->result == 0)
			job->result = event->result;
		nvkm_sched_move_parked_to_pollable_locked(sched, job);
		return;
	}
}

static void
nvkm_sched_process_fence_event_locked(struct nvkm_sched *sched,
    struct nvkm_sched_event *event)
{
	struct nvkm_sched_dag_node *node;
	struct nvkm_sched_dep *dep;
	struct nvkm_job *job;

	node = nvkm_sched_dag_find_locked(sched, event->fence);
	if (node == NULL)
		return;

	RB_REMOVE(nvkm_sched_dag_tree, &sched->dag, node);
	while ((dep = TAILQ_FIRST(&node->waiters)) != NULL) {
		TAILQ_REMOVE(&node->waiters, dep, dag_link);
		job = dep->job;
		TAILQ_REMOVE(&job->dep_fences, dep, job_link);
		dma_fence_put(dep->fence);
		kfree(dep);

		if (event->result != 0 && job->result == 0)
			job->result = event->result;
		if (event->result != 0)
			nvkm_sched_job_remove_all_deps_locked(sched, job);
		if (TAILQ_EMPTY(&job->dep_fences))
			nvkm_sched_move_parked_to_pollable_locked(sched, job);
	}
	kfree(node);
}

static void
nvkm_sched_process_events_locked(struct nvkm_sched *sched)
{
	struct nvkm_sched_event *event;

	while ((event = TAILQ_FIRST(&sched->events)) != NULL) {
		TAILQ_REMOVE(&sched->events, event, link);
		switch (event->type) {
		case NVKM_COMPLETE_JOB:
			nvkm_sched_process_job_event_locked(sched, event);
			break;
		case NVKM_COMPLETE_FENCE:
			nvkm_sched_process_fence_event_locked(sched, event);
			break;
		}
		dma_fence_put(event->fence);
		kfree(event);
	}
}

static void
nvkm_sched_complete_job(struct nvkm_sched *sched, struct nvkm_job *job,
    int result)
{
	struct dma_fence *done_fence;

	done_fence = dma_fence_get(job->done_fence);
	nvkm_job_fini(job);
	kfree(job);

	if (done_fence == NULL)
		return;
	if (!dma_fence_is_signaled(done_fence)) {
		if (result != 0)
			dma_fence_set_error(done_fence, result);
		(void)dma_fence_signal(done_fence);
	}
	nvkm_sched_fence_complete(sched, done_fence, result);
	dma_fence_put(done_fence);
}

static bool
nvkm_sched_poll_one_locked(struct nvkm_sched *sched)
{
	struct nvkm_job_future future;
	struct nvkm_job *job;
	int result;

	job = TAILQ_FIRST(&sched->pollable_jobs);
	if (job == NULL)
		return (false);

	TAILQ_REMOVE(&sched->pollable_jobs, job, sched_link);
	sched->polling_job = job;
	lwkt_reltoken(&sched->token);

	if (job->poll != NULL) {
		future = job->poll(sched, job);
	} else {
		future.ready = true;
		future.result = -EINVAL;
	}

	lwkt_gettoken(&sched->token);
	sched->polling_job = NULL;
	result = future.result != 0 ? future.result : job->result;
	if (future.ready) {
		lwkt_reltoken(&sched->token);
		nvkm_sched_complete_job(sched, job, result);
		lwkt_gettoken(&sched->token);
	} else {
		if (future.result != 0 && job->result == 0)
			job->result = future.result;
		TAILQ_INSERT_TAIL(&sched->parked_jobs, job, sched_link);
	}

	return (true);
}

static void
nvkm_sched_cancel_jobs_locked(struct nvkm_sched *sched, int result)
{
	struct nvkm_job *job, *next;

	for (job = TAILQ_FIRST(&sched->parked_jobs); job != NULL;
	    job = next) {
		next = TAILQ_NEXT(job, sched_link);
		TAILQ_REMOVE(&sched->parked_jobs, job, sched_link);
		nvkm_sched_job_remove_all_deps_locked(sched, job);
		if (job->result == 0)
			job->result = result;
		TAILQ_INSERT_TAIL(&sched->pollable_jobs, job, sched_link);
	}
	TAILQ_FOREACH(job, &sched->pollable_jobs, sched_link) {
		if (job->result == 0)
			job->result = result;
	}
}

/*
 * nvkm_sched_make_pollable_from_poll()
 *
 * Ownership:
 *   Borrows job, which must already be owned by this scheduler.
 *
 * Lifetime:
 *   Intended for timeline-local waiters while the owning scheduler LWKT is
 *   inside another job's poll method.
 *
 * Threading:
 *   Not an MP-safe wake API.  It may be called only from the owning scheduler
 *   poll context.  The helper takes sched->token and verifies that the target
 *   job is still parked and has no unsatisfied DAG dependencies.
 */
bool
nvkm_sched_make_pollable_from_poll(struct nvkm_sched *sched,
    struct nvkm_job *job)
{
	bool moved = false;

	if (sched == NULL || job == NULL)
		return (false);

	lwkt_gettoken(&sched->token);
	if (sched->polling_job != NULL &&
	    nvkm_sched_job_is_parked_locked(sched, job) &&
	    TAILQ_EMPTY(&job->dep_fences)) {
		nvkm_sched_move_parked_to_pollable_locked(sched, job);
		moved = true;
	}
	lwkt_reltoken(&sched->token);
	return (moved);
}

/*
 * nvkm_sched_enqueue()
 *
 * Ownership:
 *   Takes ownership of job on success.  On failure the caller still owns the job
 *   and must call nvkm_job_fini() plus free the concrete object.
 *
 * Lifetime:
 *   Called after the ioctl path has published ABI-visible output fences but
 *   before it returns to userspace or waits for synchronous VM_BIND completion.
 *
 * Threading:
 *   MP-safe.  The scheduler token protects DAG insertion and queue ownership.
 */
int
nvkm_sched_enqueue(struct nvkm_sched *sched, struct nvkm_job *job)
{
	struct nvkm_sched_dep *dep;
	struct nvkm_sched_dag_node *node;
	int err = 0;

	if (sched == NULL || job == NULL)
		return (-EINVAL);

	lwkt_gettoken(&sched->token);
	if (nvkm_sched_stop_requested(sched)) {
		lwkt_reltoken(&sched->token);
		return (-ENODEV);
	}

	TAILQ_FOREACH(dep, &job->dep_fences, job_link) {
		node = nvkm_sched_dag_ensure_locked(sched, dep->fence);
		if (node == NULL) {
			err = -ENOMEM;
			break;
		}
		TAILQ_INSERT_TAIL(&node->waiters, dep, dag_link);
	}
	if (err != 0) {
		TAILQ_FOREACH(dep, &job->dep_fences, job_link) {
			node = nvkm_sched_dag_find_locked(sched, dep->fence);
			if (node != NULL) {
				TAILQ_REMOVE(&node->waiters, dep, dag_link);
				if (TAILQ_EMPTY(&node->waiters)) {
					RB_REMOVE(nvkm_sched_dag_tree, &sched->dag,
					    node);
					kfree(node);
				}
			}
		}
		lwkt_reltoken(&sched->token);
		return (err);
	}

	TAILQ_INSERT_TAIL(&sched->parked_jobs, job, sched_link);
	if (TAILQ_EMPTY(&job->dep_fences)) {
		if (job->done_fence != NULL) {
			lwkt_reltoken(&sched->token);
			nvkm_sched_wake_job(sched, job->done_fence, 0);
			return (0);
		}
		nvkm_sched_move_parked_to_pollable_locked(sched, job);
	}
	wakeup(sched);
	lwkt_reltoken(&sched->token);
	return (0);
}

/*
 * nvkm_sched_start()
 *
 * Ownership:
 *   Initializes caller-owned scheduler storage embedded in nvkm_drm_file.
 *
 * Lifetime:
 *   Must run before the drm_file is published through file_priv->driver_priv.
 *   The caller must keep scheduler storage alive until nvkm_sched_run()
 *   returns.  Stop is asynchronous and only starts that drain.
 *
 * Threading:
 *   Initializes scheduler state.  The VM owner creates the LWKT because it
 *   owns the post-run VM teardown.
 */
int
nvkm_sched_start(struct nvkm_sched *sched)
{
	if (sched == NULL)
		return (-EINVAL);

	lwkt_token_init(&sched->token, "nvkm-sched");
	RB_INIT(&sched->dag);
	TAILQ_INIT(&sched->parked_jobs);
	TAILQ_INIT(&sched->pollable_jobs);
	TAILQ_INIT(&sched->events);
	sched->polling_job = NULL;
	sched->stop_requested = 0;
	sched->thread = NULL;

	return (0);
}

/*
 * nvkm_sched_stop()
 *
 * Ownership:
 *   Borrows sched.  The owning VM retains scheduler storage until
 *   nvkm_sched_run() returns.
 *
 * Lifetime:
 *   Requests scheduler teardown.  The scheduler thread drains jobs and then
 *   returns to the VM-owned runner.
 *
 * Threading:
 *   MP-safe and non-blocking.  Publishes stop_requested atomically and wakes
 *   sched, which is the scheduler sleep channel.
 */
void
nvkm_sched_stop(struct nvkm_sched *sched)
{
	if (sched == NULL)
		return;

	atomic_set_int(&sched->stop_requested, 1);
	wakeup(sched);
}

void
nvkm_sched_run(struct nvkm_sched *sched)
{
	lwkt_gettoken(&sched->token);
	for (;;) {
		nvkm_sched_process_events_locked(sched);
		if (nvkm_sched_stop_requested(sched))
			nvkm_sched_cancel_jobs_locked(sched, -ECANCELED);
		while (nvkm_sched_poll_one_locked(sched))
			nvkm_sched_process_events_locked(sched);

		KKASSERT(sched->polling_job == NULL);
		if (nvkm_sched_stop_requested(sched)) {
			if (TAILQ_EMPTY(&sched->parked_jobs) &&
			    TAILQ_EMPTY(&sched->pollable_jobs))
				break;
		}

		tsleep_interlock(sched, 0);
		(void)tsleep(sched, PINTERLOCKED, "nvksch", hz / 10);
	}

	lwkt_reltoken(&sched->token);
	lwkt_token_uninit(&sched->token);
}
