/*
 * Copyright (c) 2005 Jeffrey M. Hsu.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Jeffrey M. Hsu.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/sys/kern/kern_objcache.c,v 1.14 2006/12/17 19:28:30 dillon Exp $
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/globaldata.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/objcache.h>
#include <sys/spinlock.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/spinlock2.h>

static MALLOC_DEFINE(M_OBJCACHE, "objcache", "Object Cache");
static MALLOC_DEFINE(M_OBJMAG, "objcache magazine", "Object Cache Magazine");

#define	INITIAL_MAG_CAPACITY	256

struct magazine {
	int			 rounds;
	int			 capacity;
	int			 cleaning;
	SLIST_ENTRY(magazine)	 nextmagazine;
	void			*objects[];
};

SLIST_HEAD(magazinelist, magazine);

/*
 * per-cluster cache of magazines
 *
 * All fields in this structure are protected by the spinlock.
 */
struct magazinedepot {
	/*
	 * The per-cpu object caches only exchanges completely full or
	 * completely empty magazines with the depot layer, so only have
	 * to cache these two types of magazines.
	 */
	struct magazinelist	fullmagazines;
	struct magazinelist	emptymagazines;
	int			magcapacity;

	/* protect this structure */
	struct spinlock		spin;

	/* magazines not yet allocated towards limit */
	int			unallocated_objects;

	/* infrequently used fields */
	int			waiting;	/* waiting for another cpu to
						 * return a full magazine to
						 * the depot */
	int			contested;	/* depot contention count */
};

/*
 * per-cpu object cache
 * All fields in this structure are protected by crit_enter().
 */
struct percpu_objcache {
	struct magazine	*loaded_magazine;	/* active magazine */
	struct magazine	*previous_magazine;	/* backup magazine */

	/* statistics */
	int		gets_cumulative;	/* total calls to get */
	int		gets_null;		/* objcache_get returned NULL */
	int		puts_cumulative;	/* total calls to put */
	int		puts_othercluster;	/* returned to other cluster */

	/* infrequently used fields */
	int		waiting;	/* waiting for a thread on this cpu to
					 * return an obj to the per-cpu cache */
};

/* only until we have NUMA cluster topology information XXX */
#define MAXCLUSTERS 1
#define myclusterid 0
#define CLUSTER_OF(obj) 0

/*
 * Two-level object cache consisting of NUMA cluster-level depots of
 * fully loaded or completely empty magazines and cpu-level caches of
 * individual objects.
 */
struct objcache {
	char			*name;

	/* object constructor and destructor from blank storage */
	objcache_ctor_fn	*ctor;
	objcache_dtor_fn	*dtor;
	void			*private;

	/* interface to underlying allocator */
	objcache_alloc_fn	*alloc;
	objcache_free_fn	*free;
	void			*allocator_args;

	SLIST_ENTRY(objcache)	oc_next;

	/* NUMA-cluster level caches */
	struct magazinedepot	depot[MAXCLUSTERS];

	struct percpu_objcache	cache_percpu[];		/* per-cpu caches */
};

static struct spinlock objcachelist_spin;
static SLIST_HEAD(objcachelist, objcache) allobjcaches;

static struct magazine *
mag_alloc(int capacity)
{
	struct magazine *mag;

	mag = kmalloc(__offsetof(struct magazine, objects[capacity]),
			M_OBJMAG, M_INTWAIT | M_ZERO);
	mag->capacity = capacity;
	mag->rounds = 0;
	mag->cleaning = 0;
	return (mag);
}

/*
 * Utility routine for objects that don't require any de-construction.
 */

static void
null_dtor(void *obj, void *private)
{
	/* do nothing */
}

static boolean_t
null_ctor(void *obj, void *private, int ocflags)
{
	return TRUE;
}

/*
 * Create an object cache.
 */
struct objcache *
objcache_create(const char *name, int cluster_limit, int mag_capacity,
		objcache_ctor_fn *ctor, objcache_dtor_fn *dtor, void *private,
		objcache_alloc_fn *alloc, objcache_free_fn *free,
		void *allocator_args)
{
	struct objcache *oc;
	struct magazinedepot *depot;
	int cpuid;

	/* allocate object cache structure */
	oc = kmalloc(__offsetof(struct objcache, cache_percpu[ncpus]),
		    M_OBJCACHE, M_WAITOK | M_ZERO);
	oc->name = kstrdup(name, M_TEMP);
	oc->ctor = ctor ? ctor : null_ctor;
	oc->dtor = dtor ? dtor : null_dtor;
	oc->private = private;
	oc->free = free;
	oc->allocator_args = allocator_args;

	/* initialize depots */
	depot = &oc->depot[0];

	spin_init(&depot->spin);
	SLIST_INIT(&depot->fullmagazines);
	SLIST_INIT(&depot->emptymagazines);

	if (mag_capacity == 0)
		mag_capacity = INITIAL_MAG_CAPACITY;
	depot->magcapacity = mag_capacity;

	/*
	 * The cluster_limit must be sufficient to have three magazines per
	 * cpu.
	 */
	if (cluster_limit == 0) {
		depot->unallocated_objects = -1;
	} else {
		if (cluster_limit < mag_capacity * ncpus * 3)
			cluster_limit = mag_capacity * ncpus * 3;
		depot->unallocated_objects = cluster_limit;
	}
	oc->alloc = alloc;

	/* initialize per-cpu caches */
	for (cpuid = 0; cpuid < ncpus; cpuid++) {
		struct percpu_objcache *cache_percpu = &oc->cache_percpu[cpuid];

		cache_percpu->loaded_magazine = mag_alloc(mag_capacity);
		cache_percpu->previous_magazine = mag_alloc(mag_capacity);
	}
	spin_lock_wr(&objcachelist_spin);
	SLIST_INSERT_HEAD(&allobjcaches, oc, oc_next);
	spin_unlock_wr(&objcachelist_spin);

	return (oc);
}

struct objcache *
objcache_create_simple(malloc_type_t mtype, size_t objsize)
{
	struct objcache_malloc_args *margs;
	struct objcache *oc;

	margs = kmalloc(sizeof(*margs), M_OBJCACHE, M_WAITOK|M_ZERO);
	margs->objsize = objsize;
	margs->mtype = mtype;
	oc = objcache_create(mtype->ks_shortdesc, 0, 0,
			     NULL, NULL, NULL,
			     objcache_malloc_alloc, objcache_malloc_free,
			     margs);
	return (oc);
}

#define MAGAZINE_EMPTY(mag)	(mag->rounds == 0)
#define MAGAZINE_NOTEMPTY(mag)	(mag->rounds != 0)
#define MAGAZINE_FULL(mag)	(mag->rounds == mag->capacity)

#define	swap(x, y)	({ struct magazine *t = x; x = y; y = t; })

/*
 * Get an object from the object cache.
 *
 * WARNING!  ocflags are only used when we have to go to the underlying
 * allocator, so we cannot depend on flags such as M_ZERO.
 */
void *
objcache_get(struct objcache *oc, int ocflags)
{
	struct percpu_objcache *cpucache = &oc->cache_percpu[mycpuid];
	struct magazine *loadedmag;
	struct magazine *emptymag;
	void *obj;
	struct magazinedepot *depot;

	KKASSERT((ocflags & M_ZERO) == 0);
	crit_enter();
	++cpucache->gets_cumulative;

retry:
	/*
	 * Loaded magazine has an object.  This is the hot path.
	 * It is lock-free and uses a critical section to block
	 * out interrupt handlers on the same processor.
	 */
	loadedmag = cpucache->loaded_magazine;
	if (MAGAZINE_NOTEMPTY(loadedmag)) {
		obj = loadedmag->objects[--loadedmag->rounds];
		crit_exit();
		return (obj);
	}

	/* Previous magazine has an object. */
	if (MAGAZINE_NOTEMPTY(cpucache->previous_magazine)) {
		KKASSERT(cpucache->previous_magazine->cleaning +
			 cpucache->loaded_magazine->cleaning == 0);
		swap(cpucache->loaded_magazine, cpucache->previous_magazine);
		loadedmag = cpucache->loaded_magazine;
		obj = loadedmag->objects[--loadedmag->rounds];
		crit_exit();
		return (obj);
	}

	/*
	 * Both magazines empty.  Get a full magazine from the depot and
	 * move one of the empty ones to the depot.
	 *
	 * Obtain the depot spinlock.
	 *
	 * NOTE: Beyond this point, M_* flags are handled via oc->alloc()
	 */
	depot = &oc->depot[myclusterid];
	spin_lock_wr(&depot->spin);

	/*
	 * Recheck the cpucache after obtaining the depot spinlock.  This
	 * shouldn't be necessary now but don't take any chances.
	 */
	if (MAGAZINE_NOTEMPTY(cpucache->loaded_magazine) ||
	    MAGAZINE_NOTEMPTY(cpucache->previous_magazine)
	) {
		spin_unlock_wr(&depot->spin);
		goto retry;
	}

	/* Check if depot has a full magazine. */
	if (!SLIST_EMPTY(&depot->fullmagazines)) {
		emptymag = cpucache->previous_magazine;
		cpucache->previous_magazine = cpucache->loaded_magazine;
		cpucache->loaded_magazine = SLIST_FIRST(&depot->fullmagazines);
		SLIST_REMOVE_HEAD(&depot->fullmagazines, nextmagazine);

		/*
		 * Return emptymag to the depot.
		 */
		KKASSERT(MAGAZINE_EMPTY(emptymag));
		SLIST_INSERT_HEAD(&depot->emptymagazines,
				  emptymag, nextmagazine);
		spin_unlock_wr(&depot->spin);
		goto retry;
	}

	/*
	 * The depot does not have any non-empty magazines.  If we have
	 * not hit our object limit we can allocate a new object using
	 * the back-end allocator.
	 *
	 * note: unallocated_objects can be initialized to -1, which has
	 * the effect of removing any allocation limits.
	 */
	if (depot->unallocated_objects) {
		--depot->unallocated_objects;
		spin_unlock_wr(&depot->spin);
		crit_exit();

		obj = oc->alloc(oc->allocator_args, ocflags);
		if (obj) {
			if (oc->ctor(obj, oc->private, ocflags))
				return (obj);
			oc->free(obj, oc->allocator_args);
			spin_lock_wr(&depot->spin);
			++depot->unallocated_objects;
			spin_unlock_wr(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
			obj = NULL;
		}
		if (obj == NULL) {
			crit_enter();
			/*
			 * makes debugging easier when gets_cumulative does
			 * not include gets_null.
			 */
			++cpucache->gets_null;
			--cpucache->gets_cumulative;
			crit_exit();
		}
		return(obj);
	}

	/*
	 * Otherwise block if allowed to.
	 */
	if ((ocflags & (M_WAITOK|M_NULLOK)) == M_WAITOK) {
		++cpucache->waiting;
		++depot->waiting;
		msleep(depot, &depot->spin, 0, "objcache_get", 0);
		--cpucache->waiting;
		--depot->waiting;
		spin_unlock_wr(&depot->spin);
		goto retry;
	}

	/*
	 * Otherwise fail
	 */
	++cpucache->gets_null;
	--cpucache->gets_cumulative;
	crit_exit();
	spin_unlock_wr(&depot->spin);
	return (NULL);
}

/*
 * Wrapper for malloc allocation routines.
 */
void *
objcache_malloc_alloc(void *allocator_args, int ocflags)
{
	struct objcache_malloc_args *alloc_args = allocator_args;

	return (kmalloc(alloc_args->objsize, alloc_args->mtype,
		       ocflags & OC_MFLAGS));
}

void
objcache_malloc_free(void *obj, void *allocator_args)
{
	struct objcache_malloc_args *alloc_args = allocator_args;

	kfree(obj, alloc_args->mtype);
}

/*
 * Wrapper for allocation policies that pre-allocate at initialization time
 * and don't do run-time allocation.
 */
void *
objcache_nop_alloc(void *allocator_args, int ocflags)
{
	return (NULL);
}

void
objcache_nop_free(void *obj, void *allocator_args)
{
}

/*
 * Return an object to the object cache.
 */
void
objcache_put(struct objcache *oc, void *obj)
{
	struct percpu_objcache *cpucache = &oc->cache_percpu[mycpuid];
	struct magazine *loadedmag;
	struct magazinedepot *depot;

	crit_enter();
	++cpucache->puts_cumulative;

	if (CLUSTER_OF(obj) != myclusterid) {
#ifdef notyet
		/* use lazy IPI to send object to owning cluster XXX todo */
		++cpucache->puts_othercluster;
		crit_exit();
		return;
#endif
	}

retry:
	/*
	 * Free slot available in loaded magazine.  This is the hot path.
	 * It is lock-free and uses a critical section to block out interrupt
	 * handlers on the same processor.
	 */
	loadedmag = cpucache->loaded_magazine;
	if (!MAGAZINE_FULL(loadedmag)) {
		loadedmag->objects[loadedmag->rounds++] = obj;
		if (cpucache->waiting)
			wakeup_mycpu(&oc->depot[myclusterid]);
		crit_exit();
		return;
	}

	/*
	 * Current magazine full, but previous magazine has room.  XXX
	 */
	if (!MAGAZINE_FULL(cpucache->previous_magazine)) {
		KKASSERT(cpucache->previous_magazine->cleaning +
			 cpucache->loaded_magazine->cleaning == 0);
		swap(cpucache->loaded_magazine, cpucache->previous_magazine);
		loadedmag = cpucache->loaded_magazine;
		loadedmag->objects[loadedmag->rounds++] = obj;
		if (cpucache->waiting)
			wakeup_mycpu(&oc->depot[myclusterid]);
		crit_exit();
		return;
	}

	/*
	 * Both magazines full.  Get an empty magazine from the depot and
	 * move a full loaded magazine to the depot.  Even though the
	 * magazine may wind up with space available after we block on
	 * the spinlock, we still cycle it through to avoid the non-optimal
	 * corner-case.
	 *
	 * Obtain the depot spinlock.
	 */
	depot = &oc->depot[myclusterid];
	spin_lock_wr(&depot->spin);

	/*
	 * If an empty magazine is available in the depot, cycle it
	 * through and retry.
	 */
	if (!SLIST_EMPTY(&depot->emptymagazines)) {
		KKASSERT(cpucache->previous_magazine->cleaning +
			 cpucache->loaded_magazine->cleaning == 0);
		loadedmag = cpucache->previous_magazine;
		cpucache->previous_magazine = cpucache->loaded_magazine;
		cpucache->loaded_magazine = SLIST_FIRST(&depot->emptymagazines);
		SLIST_REMOVE_HEAD(&depot->emptymagazines, nextmagazine);

		/*
		 * Return loadedmag to the depot.  Due to blocking it may
		 * not be entirely full and could even be empty.
		 */
		if (MAGAZINE_EMPTY(loadedmag)) {
			SLIST_INSERT_HEAD(&depot->emptymagazines,
					  loadedmag, nextmagazine);
			spin_unlock_wr(&depot->spin);
		} else {
			SLIST_INSERT_HEAD(&depot->fullmagazines,
					  loadedmag, nextmagazine);
			spin_unlock_wr(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
		}
		goto retry;
	}

	/*
	 * An empty mag is not available.  This is a corner case which can
	 * occur due to cpus holding partially full magazines.  Do not try
	 * to allocate a mag, just free the object.
	 */
	++depot->unallocated_objects;
	spin_unlock_wr(&depot->spin);
	if (depot->waiting)
		wakeup(depot);
	crit_exit();
	oc->dtor(obj, oc->private);
	oc->free(obj, oc->allocator_args);
}

/*
 * The object is being put back into the cache, but the caller has
 * indicated that the object is not in any shape to be reused and should
 * be dtor'd immediately.
 */
void
objcache_dtor(struct objcache *oc, void *obj)
{
	struct magazinedepot *depot;

	depot = &oc->depot[myclusterid];
	spin_lock_wr(&depot->spin);
	++depot->unallocated_objects;
	spin_unlock_wr(&depot->spin);
	if (depot->waiting)
		wakeup(depot);
	oc->dtor(obj, oc->private);
	oc->free(obj, oc->allocator_args);
}

/*
 * Deallocate all objects in a magazine and free the magazine if requested.
 * The magazine must already be disassociated from the depot.
 *
 * Must be called with a critical section held when called with a per-cpu
 * magazine.  The magazine may be indirectly modified during the loop.
 *
 * The number of objects freed is returned.
 */
static int
mag_purge(struct objcache *oc, struct magazine *mag, int freeit)
{
	int count;
	void *obj;

	count = 0;
	++mag->cleaning;
	while (mag->rounds) {
		obj = mag->objects[--mag->rounds];
		oc->dtor(obj, oc->private);		/* MAY BLOCK */
		oc->free(obj, oc->allocator_args);	/* MAY BLOCK */
		++count;

		/*
		 * Cycle for interrupts
		 */
		if ((count & 15) == 0) {
			crit_exit();
			crit_enter();
		}
	}
	--mag->cleaning;
	if (freeit)
		kfree(mag, M_OBJMAG);
	return(count);
}

/*
 * Disassociate zero or more magazines from a magazine list associated with
 * the depot, update the depot, and move the magazines to a temporary
 * list.
 *
 * The caller must check the depot for waiters and wake it up, typically
 * after disposing of the magazines this function loads onto the temporary
 * list.
 */
static void
maglist_disassociate(struct magazinedepot *depot, struct magazinelist *maglist,
		     struct magazinelist *tmplist, boolean_t purgeall)
{
	struct magazine *mag;

	while ((mag = SLIST_FIRST(maglist)) != NULL) {
		SLIST_REMOVE_HEAD(maglist, nextmagazine);
		SLIST_INSERT_HEAD(tmplist, mag, nextmagazine);
		depot->unallocated_objects += mag->rounds;
	}
}
			
/*
 * Deallocate all magazines and their contents from the passed temporary
 * list.  The magazines have already been accounted for by their depots.
 *
 * The total number of rounds freed is returned.  This number is typically
 * only used to determine whether a wakeup on the depot is needed or not.
 */
static int
maglist_purge(struct objcache *oc, struct magazinelist *maglist)
{
	struct magazine *mag;
	int count = 0;

	/*
	 * can't use SLIST_FOREACH because blocking releases the depot
	 * spinlock 
	 */
	while ((mag = SLIST_FIRST(maglist)) != NULL) {
		SLIST_REMOVE_HEAD(maglist, nextmagazine);
		count += mag_purge(oc, mag, TRUE);
	}
	return(count);
}

/*
 * De-allocates all magazines on the full and empty magazine lists.
 *
 * Because this routine is called with a spinlock held, the magazines
 * can only be disassociated and moved to a temporary list, not freed.
 *
 * The caller is responsible for freeing the magazines.
 */
static void
depot_disassociate(struct magazinedepot *depot, struct magazinelist *tmplist)
{
	maglist_disassociate(depot, &depot->fullmagazines, tmplist, TRUE);
	maglist_disassociate(depot, &depot->emptymagazines, tmplist, TRUE);
}

#ifdef notneeded
void
objcache_reclaim(struct objcache *oc)
{
	struct percpu_objcache *cache_percpu = &oc->cache_percpu[myclusterid];
	struct magazinedepot *depot = &oc->depot[myclusterid];
	struct magazinelist tmplist;
	int count;

	SLIST_INIT(&tmplist);
	crit_enter();
	count = mag_purge(oc, cache_percpu->loaded_magazine, FALSE);
	count += mag_purge(oc, cache_percpu->previous_magazine, FALSE);
	crit_exit();

	spin_lock_wr(&depot->spin);
	depot->unallocated_objects += count;
	depot_disassociate(depot, &tmplist);
	spin_unlock_wr(&depot->spin);
	count += maglist_purge(oc, &tmplist);
	if (count && depot->waiting)
		wakeup(depot);
}
#endif

/*
 * Try to free up some memory.  Return as soon as some free memory is found.
 * For each object cache on the reclaim list, first try the current per-cpu
 * cache, then the full magazine depot.
 */
boolean_t
objcache_reclaimlist(struct objcache *oclist[], int nlist, int ocflags)
{
	struct objcache *oc;
	struct percpu_objcache *cpucache;
	struct magazinedepot *depot;
	struct magazinelist tmplist;
	int i, count;

	SLIST_INIT(&tmplist);

	for (i = 0; i < nlist; i++) {
		oc = oclist[i];
		cpucache = &oc->cache_percpu[mycpuid];
		depot = &oc->depot[myclusterid];

		crit_enter();
		count = mag_purge(oc, cpucache->loaded_magazine, FALSE);
		if (count == 0)
			count += mag_purge(oc, cpucache->previous_magazine, FALSE);
		crit_exit();
		if (count > 0) {
			spin_lock_wr(&depot->spin);
			depot->unallocated_objects += count;
			spin_unlock_wr(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
			return (TRUE);
		}
		crit_exit();
		spin_lock_wr(&depot->spin);
		maglist_disassociate(depot, &depot->fullmagazines,
				     &tmplist, FALSE);
		spin_unlock_wr(&depot->spin);
		count = maglist_purge(oc, &tmplist);
		if (count > 0) {
			if (depot->waiting)
				wakeup(depot);
			return (TRUE);
		}
	}
	return (FALSE);
}

/*
 * Destroy an object cache.  Must have no existing references.
 */
void
objcache_destroy(struct objcache *oc)
{
	struct percpu_objcache *cache_percpu;
	struct magazinedepot *depot;
	int clusterid, cpuid;
	struct magazinelist tmplist;

	SLIST_INIT(&tmplist);
	for (clusterid = 0; clusterid < MAXCLUSTERS; clusterid++) {
		depot = &oc->depot[clusterid];
		spin_lock_wr(&depot->spin);
		depot_disassociate(depot, &tmplist);
		spin_unlock_wr(&depot->spin);
	}
	maglist_purge(oc, &tmplist);

	for (cpuid = 0; cpuid < ncpus; cpuid++) {
		cache_percpu = &oc->cache_percpu[cpuid];

		mag_purge(oc, cache_percpu->loaded_magazine, TRUE);
		mag_purge(oc, cache_percpu->previous_magazine, TRUE);
		cache_percpu->loaded_magazine = NULL;
		cache_percpu->previous_magazine = NULL;
		/* don't bother adjusting depot->unallocated_objects */
	}

	kfree(oc->name, M_TEMP);
	kfree(oc, M_OBJCACHE);
}

#if 0
/*
 * Populate the per-cluster depot with elements from a linear block
 * of memory.  Must be called for individually for each cluster.
 * Populated depots should not be destroyed.
 */
void
objcache_populate_linear(struct objcache *oc, void *base, int nelts, int size)
{
	char *p = base;
	char *end = (char *)base + (nelts * size);
	struct magazinedepot *depot = &oc->depot[myclusterid];
	struct magazine *emptymag = mag_alloc(depot->magcapcity);

	while (p < end) {
		emptymag->objects[emptymag->rounds++] = p;
		if (MAGAZINE_FULL(emptymag)) {
			spin_lock_wr(&depot->spin);
			SLIST_INSERT_HEAD(&depot->fullmagazines, emptymag,
					  nextmagazine);
			depot->unallocated_objects += emptymag->rounds;
			spin_unlock_wr(&depot->spin);
			if (depot->waiting)
				wakeup(depot);
			emptymag = mag_alloc(depot->magcapacity);
		}
		p += size;
	}
	if (MAGAZINE_EMPTY(emptymag)) {
		mag_purge(oc, emptymag, TRUE);
	} else {
		spin_lock_wr(&depot->spin);
		SLIST_INSERT_HEAD(&depot->fullmagazines, emptymag,
				  nextmagazine);
		depot->unallocated_objects += emptymag->rounds;
		spin_unlock_wr(&depot->spin);
		if (depot->waiting)
			wakeup(depot);
		emptymag = mag_alloc(depot->magcapacity);
	}
}
#endif

#if 0
/*
 * Check depot contention once a minute.
 * 2 contested locks per second allowed.
 */
static int objcache_rebalance_period;
static const int objcache_contention_rate = 120;
static struct callout objcache_callout;

#define MAXMAGSIZE 512

/*
 * Check depot contention and increase magazine size if necessary.
 */
static void
objcache_timer(void *dummy)
{
	struct objcache *oc;
	struct magazinedepot *depot;
	struct magazinelist tmplist;

	XXX we need to detect when an objcache is destroyed out from under
	    us XXX

	SLIST_INIT(&tmplist);

	spin_lock_wr(&objcachelist_spin);
	SLIST_FOREACH(oc, &allobjcaches, oc_next) {
		depot = &oc->depot[myclusterid];
		if (depot->magcapacity < MAXMAGSIZE) {
			if (depot->contested > objcache_contention_rate) {
				spin_lock_wr(&depot->spin);
				depot_disassociate(depot, &tmplist);
				depot->magcapacity *= 2;
				spin_unlock_wr(&depot->spin);
				printf("objcache_timer: increasing cache %s"
				       " magsize to %d, contested %d times\n",
				    oc->name, depot->magcapacity,
				    depot->contested);
			}
			depot->contested = 0;
		}
		spin_unlock_wr(&objcachelist_spin);
		if (maglist_purge(oc, &tmplist) > 0 && depot->waiting)
			wakeup(depot);
		spin_lock_wr(&objcachelist_spin);
	}
	spin_unlock_wr(&objcachelist_spin);

	callout_reset(&objcache_callout, objcache_rebalance_period,
		      objcache_timer, NULL);
}

#endif

static void
objcache_init(void)
{
	spin_init(&objcachelist_spin);
#if 0
	callout_init(&objcache_callout);
	objcache_rebalance_period = 60 * hz;
	callout_reset(&objcache_callout, objcache_rebalance_period,
		      objcache_timer, NULL);
#endif
}
SYSINIT(objcache, SI_SUB_CPU, SI_ORDER_ANY, objcache_init, 0);
