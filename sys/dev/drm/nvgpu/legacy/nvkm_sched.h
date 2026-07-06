/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly native nvkm per-file scheduler.
 */

#ifndef _NVKM_SCHED_H_
#define _NVKM_SCHED_H_

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/thread.h>
#include <sys/tree.h>

#include <linux/dma-fence.h>

struct nvkm_job;
struct nvkm_sched;
struct nvkm_sched_dag_node;
struct nvkm_sched_dep;
struct nvkm_sched_event;

RB_HEAD(nvkm_sched_dag_tree, nvkm_sched_dag_node);
TAILQ_HEAD(nvkm_sched_dep_list, nvkm_sched_dep);
TAILQ_HEAD(nvkm_sched_job_list, nvkm_job);
TAILQ_HEAD(nvkm_sched_event_list, nvkm_sched_event);


struct nvkm_job_future {
	bool ready;
	int result;
};

struct nvkm_job {
	TAILQ_ENTRY(nvkm_job) sched_link;
	struct nvkm_job_future (*poll)(struct nvkm_sched *sched,
	    struct nvkm_job *job);
	struct dma_fence *done_fence;
	struct nvkm_sched_dep_list dep_fences;
	int result;
};

enum nvkm_sched_event_type {
	NVKM_COMPLETE_FENCE,
	NVKM_COMPLETE_JOB,
};

struct nvkm_sched_event {
	TAILQ_ENTRY(nvkm_sched_event) link;
	enum nvkm_sched_event_type type;
	struct dma_fence *fence;
	int result;
};

struct nvkm_sched {
	struct lwkt_token token;
	struct thread *thread;
	struct nvkm_sched_dag_tree dag;
	struct nvkm_sched_job_list parked_jobs;
	struct nvkm_sched_job_list pollable_jobs;
	struct nvkm_job *polling_job;
	struct nvkm_sched_event_list events;
	u_int stop_requested;
};
void nvkm_job_init(struct nvkm_job *job,
    struct nvkm_job_future (*poll)(struct nvkm_sched *, struct nvkm_job *),
    struct dma_fence *done_fence);
int nvkm_job_add_dep(struct nvkm_job *job, struct dma_fence *fence);
void nvkm_job_fini(struct nvkm_job *job);

int nvkm_sched_start(struct nvkm_sched *sched);
void nvkm_sched_run(struct nvkm_sched *sched);
void nvkm_sched_stop(struct nvkm_sched *sched);
void nvkm_sched_wake(struct nvkm_sched *sched);
int nvkm_sched_enqueue(struct nvkm_sched *sched, struct nvkm_job *job);
void nvkm_sched_wake_job(struct nvkm_sched *sched, struct dma_fence *fence,
    int result);
void nvkm_sched_fence_complete(struct nvkm_sched *sched,
    struct dma_fence *fence, int result);
bool nvkm_sched_make_pollable_from_poll(struct nvkm_sched *sched,
    struct nvkm_job *job);

#endif /* _NVKM_SCHED_H_ */
