/*
 * Copyright 2014-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj.c -- transactional object store implementation
 */
#include <limits.h>

#include "valgrind_internal.h"
//#include "libpmem.h"
#include "ctree.h"
#include "cuckoo.h"
#include "list.h"
#include "mmap.h"
#include "obj.h"

#include "pmemops.h"
#include "set.h"
#include "sync.h"
#include "tx.h"

static struct cuckoo *pools_ht; /* hash table used for searching by UUID */
static struct ctree *pools_tree; /* tree used for searching by address */

struct pool_hdr_template Obj_ht = {
	OBJ_HDR_SIG,
	OBJ_FORMAT_MAJOR,
	OBJ_FORMAT_COMPAT,
	OBJ_FORMAT_INCOMPAT,
	OBJ_FORMAT_RO_COMPAT,
	PMEMOBJ_MIN_POOL,
	ARCH_FLAGS_NULL, /* arch flags  - initialized by lib ctor */
};

int _pobj_cache_invalidate;

#ifndef _WIN32

__thread struct _pobj_pcache _pobj_cached_pool;

#else /* _WIN32 */

/*
 * XXX - this is a temporary implementation
 *
 * Seems like we could still use TLS and simply substitute "__thread" with
 * "__declspec(thread)", however it's not clear if it would work correctly
 * with Windows DLL's.
 * Need to verify that once we have the multi-threaded tests ported.
 */

struct _pobj_pcache {
	PMEMobjpool *pop;
	uint64_t uuid_lo;
	int invalidate;
};

static pthread_once_t Cached_pool_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t Cached_pool_key;

/*
 * _Cached_pool_key_alloc -- (internal) allocate pool cache pthread key
 */
static void
_Cached_pool_key_alloc(void)
{
	int pth_ret = pthread_key_create(&Cached_pool_key, free);
	if (pth_ret)
		FATAL("!pthread_key_create");
}

/*
 * pmemobj_direct -- returns the direct pointer of an object
 */
void *
pmemobj_direct(PMEMoid oid)
{
	if (oid.off == 0 || oid.pool_uuid_lo == 0)
		return NULL;

	struct _pobj_pcache *pcache = pthread_getspecific(Cached_pool_key);
	if (pcache == NULL) {
		pcache = malloc(sizeof(struct _pobj_pcache));
		int ret = pthread_setspecific(Cached_pool_key, pcache);
		if (ret)
			FATAL("!pthread_setspecific");
	}

	if (_pobj_cache_invalidate != pcache->invalidate ||
	    pcache->uuid_lo != oid.pool_uuid_lo) {
		pcache->invalidate = _pobj_cache_invalidate;

		if ((pcache->pop = pmemobj_pool_by_oid(oid)) == NULL) {
			pcache->uuid_lo = 0;
			return NULL;
		}

		pcache->uuid_lo = oid.pool_uuid_lo;
	}

	return (void *)((uintptr_t)pcache->pop + oid.off);
}

#endif /* _WIN32 */

/*
 * obj_pool_init -- (internal) allocate global structs holding all opened pools
 *
 * This is invoked on a first call to pmemobj_open() or pmemobj_create().
 * Memory is released in library destructor.
 */
static void
obj_pool_init(void)
{
	LOG(3, NULL);

	if (pools_ht)
		return;

	pools_ht = cuckoo_new();
	if (pools_ht == NULL)
		FATAL("!cuckoo_new");

	pools_tree = ctree_new();
	if (pools_tree == NULL)
		FATAL("!ctree_new");
}

/*
 * pmemobj_oid -- return a PMEMoid based on the virtual address
 *
 * If the address does not belong to any pool OID_NULL is returned.
 */
PMEMoid
pmemobj_oid(const void *addr)
{
	PMEMobjpool *pop = pmemobj_pool_by_ptr(addr);
	if (pop == NULL)
		return OID_NULL;

	PMEMoid oid = {pop->uuid_lo, (uintptr_t)addr - (uintptr_t)pop};
	return oid;
}

/*
 * User may decide to map all pools with MAP_PRIVATE flag using
 * PMEMOBJ_COW environment variable.
 */
static int Open_cow;

#ifdef USE_VG_MEMCHECK
/*
 * obj_vg_register -- register object in valgrind
 */
int
obj_vg_register(uint64_t off, void *arg)
{
	PMEMobjpool *pop = arg;
	struct oob_header *oobh = OBJ_OFF_TO_PTR(pop, off);

	VALGRIND_DO_MAKE_MEM_DEFINED(oobh, sizeof(*oobh));

	uint64_t obj_off = off + OBJ_OOB_SIZE;
	void *obj_ptr = OBJ_OFF_TO_PTR(pop, obj_off);

	size_t obj_size = OBJ_IS_ROOT(oobh) ?
		OBJ_ROOT_SIZE(oobh) :
		palloc_usable_size(&pop->heap, obj_off) - OBJ_OOB_SIZE;

	VALGRIND_DO_MEMPOOL_ALLOC(pop->heap.layout, obj_ptr, obj_size);
	VALGRIND_DO_MAKE_MEM_DEFINED(obj_ptr, obj_size);

	return 0;
}
#endif

/*
 * obj_init -- initialization of obj
 *
 * Called by constructor.
 */
void
obj_init(void)
{
	LOG(3, NULL);

	//COMPILE_ERROR_ON(sizeof(struct pmemobjpool) !=
	//	POOL_HDR_SIZE + POOL_DESC_SIZE);

#ifdef USE_COW_ENV
	char *env = getenv("PMEMOBJ_COW");
	if (env)
		Open_cow = atoi(env);
#endif

#ifdef _WIN32
	/* XXX - temporary implementation (see above) */
	pthread_once(&Cached_pool_key_once, _Cached_pool_key_alloc);
#endif

	lane_info_boot();

	util_remote_init();
}

/*
 * obj_fini -- cleanup of obj
 *
 * Called by destructor.
 */
void
obj_fini(void)
{
	LOG(3, NULL);

	if (pools_ht)
		cuckoo_delete(pools_ht);
	if (pools_tree)
		ctree_delete(pools_tree);
	lane_info_destroy();
	util_remote_fini();
}

#ifdef USE_VG_MEMCHECK
/*
 * Arbitrary value. When there's more undefined regions than MAX_UNDEFS, it's
 * not worth reporting everything - developer should fix the code.
 */
#define MAX_UNDEFS 1000

/*
 * obj_vg_check_no_undef -- (internal) check whether there are any undefined
 *				regions
 */
static void
obj_vg_check_no_undef(struct pmemobjpool *pop)
{
	LOG(4, "pop %p", pop);

	struct {
		void *start, *end;
	} undefs[MAX_UNDEFS];
	int num_undefs = 0;

	VALGRIND_DO_DISABLE_ERROR_REPORTING;
	char *addr_start = pop->addr;
	char *addr_end = addr_start + pop->size;

	while (addr_start < addr_end) {
		char *noaccess = (char *)VALGRIND_CHECK_MEM_IS_ADDRESSABLE(
					addr_start, addr_end - addr_start);
		if (noaccess == NULL)
			noaccess = addr_end;

		while (addr_start < noaccess) {
			char *undefined =
				(char *)VALGRIND_CHECK_MEM_IS_DEFINED(
					addr_start, noaccess - addr_start);

			if (undefined) {
				addr_start = undefined;

#ifdef VALGRIND_CHECK_MEM_IS_UNDEFINED
				addr_start = (char *)
					VALGRIND_CHECK_MEM_IS_UNDEFINED(
					addr_start, noaccess - addr_start);
				if (addr_start == NULL)
					addr_start = noaccess;
#else
				while (addr_start < noaccess &&
						VALGRIND_CHECK_MEM_IS_DEFINED(
								addr_start, 1))
					addr_start++;
#endif

				if (num_undefs < MAX_UNDEFS) {
					undefs[num_undefs].start = undefined;
					undefs[num_undefs].end = addr_start - 1;
					num_undefs++;
				}
			} else
				addr_start = noaccess;
		}

#ifdef VALGRIND_CHECK_MEM_IS_UNADDRESSABLE
		addr_start = (char *)VALGRIND_CHECK_MEM_IS_UNADDRESSABLE(
				addr_start, addr_end - addr_start);
		if (addr_start == NULL)
			addr_start = addr_end;
#else
		while (addr_start < addr_end &&
				(char *)VALGRIND_CHECK_MEM_IS_ADDRESSABLE(
						addr_start, 1) == addr_start)
			addr_start++;
#endif
	}
	VALGRIND_DO_ENABLE_ERROR_REPORTING;

	if (num_undefs) {
		/*
		 * How to resolve this error:
		 * If it's part of the free space Valgrind should be told about
		 * it by VALGRIND_DO_MAKE_MEM_NOACCESS request. If it's
		 * allocated - initialize it or use VALGRIND_DO_MAKE_MEM_DEFINED
		 * request.
		 */

		VALGRIND_PRINTF("Part of the pool is left in undefined state on"
				" boot. This is pmemobj's bug.\nUndefined"
				" regions: [pool address: %p]\n", pop);
		for (int i = 0; i < num_undefs; ++i)
			VALGRIND_PRINTF("   [%p, %p]\n", undefs[i].start,
					undefs[i].end);
		if (num_undefs == MAX_UNDEFS)
			VALGRIND_PRINTF("   ...\n");

		/* Trigger error. */
		VALGRIND_CHECK_MEM_IS_DEFINED(undefs[0].start, 1);
	}
}

/*
 * obj_vg_boot -- (internal) notify Valgrind about pool objects
 */
static void
obj_vg_boot(struct pmemobjpool *pop)
{
	if (!On_valgrind)
		return;

	LOG(4, "pop %p", pop);

	if (getenv("PMEMOBJ_VG_CHECK_UNDEF"))
		obj_vg_check_no_undef(pop);
}

#endif

/*
 * obj_boot -- (internal) boots the pmemobj pool
 */
static int
obj_boot(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if ((errno = lane_boot(pop)) != 0) {
		ERR("!lane_boot");
		return errno;
	}

	if ((errno = lane_recover_and_section_boot(pop)) != 0) {
		ERR("!lane_recover_and_section_boot");
		return errno;
	}

	return 0;
}

/*
 * pmemobj_descr_create -- (internal) create obj pool descriptor
 */
static int
obj_descr_create(PMEMobjpool *pop, const char *layout, size_t poolsize)
{
	LOG(3, "pop %p layout %s poolsize %zu", pop, layout, poolsize);

	ASSERTeq(poolsize % Pagesize, 0);

	/* opaque info lives at the beginning of mapped memory pool */
	void *dscp = (void *)((uintptr_t)pop +
				sizeof(struct pool_hdr));

	/* create the persistent part of pool's descriptor */
	memset(dscp, 0, OBJ_DSC_P_SIZE);
	if (layout)
		strncpy(pop->layout, layout, PMEMOBJ_MAX_LAYOUT - 1);
	struct pool_set *set = pop->set;

	/* initialize run_id, it will be incremented later */
	pop->run_id = 0;
	pmemops_persist(set, &pop->run_id, sizeof(pop->run_id));

	pop->lanes_offset = OBJ_LANES_OFFSET;
	pop->nlanes = OBJ_NLANES;
	pop->root_offset = 0;

	/* zero all lanes */
	void *lanes_layout = (void *)((uintptr_t)pop + pop->lanes_offset);
	pmemops_memset_persist(set, lanes_layout, 0,
				pop->nlanes * sizeof(struct lane_layout));

	pop->heap_offset = pop->lanes_offset +
		pop->nlanes * sizeof(struct lane_layout);
	pop->heap_offset = (pop->heap_offset + Pagesize - 1) & ~(Pagesize - 1);
	pop->heap_size = poolsize - pop->heap_offset;

	/* initialize heap prior to storing the checksum */
	errno = palloc_init((char *)pop + pop->heap_offset, pop->heap_size,
			set);
	if (errno != 0) {
		ERR("!palloc_init");
		return -1;
	}

	util_checksum(dscp, OBJ_DSC_P_SIZE, &pop->checksum, 1);

	/* store the persistent part of pool's descriptor (2kB) */
	pmemops_persist(set, dscp, OBJ_DSC_P_SIZE);

	return 0;
}

#if 0
/*
 * obj_descr_check -- (internal) validate obj pool descriptor
 */
static int
obj_descr_check(PMEMobjpool *pop, const char *layout, size_t poolsize)
{
	LOG(3, "pop %p layout %s poolsize %zu", pop, layout, poolsize);

	void *dscp = (void *)((uintptr_t)pop + sizeof(struct pool_hdr));

#if 0
	if (pop->set->remote) { /* XXX */
		/* read remote descriptor */
		if (obj_read_remote(pop->rpp, pop->set->r_ops.base, dscp,
				dscp, OBJ_DSC_P_SIZE)) {
			ERR("!obj_read_remote");
			return -1;
		}

		/*
		 * Set size of the replica to the pool size (required minimum).
		 * This condition is checked while opening the remote pool.
		 */
		pop->size = poolsize;
	}
#endif

	if (!util_checksum(dscp, OBJ_DSC_P_SIZE, &pop->checksum, 0)) {
		ERR("invalid checksum of pool descriptor");
		errno = EINVAL;
		return -1;
	}

	if (layout &&
	    strncmp(pop->layout, layout, PMEMOBJ_MAX_LAYOUT)) {
		ERR("wrong layout (\"%s\"), "
			"pool created with layout \"%s\"",
			layout, pop->layout);
		errno = EINVAL;
		return -1;
	}

	if (pop->size < poolsize) {
		ERR("replica size smaller than pool size: %zu < %zu",
			pop->size, poolsize);
		errno = EINVAL;
		return -1;
	}

	if (pop->heap_offset + pop->heap_size != poolsize) {
		ERR("heap size does not match pool size: %zu != %zu",
			pop->heap_offset + pop->heap_size, poolsize);
		errno = EINVAL;
		return -1;
	}

	if (pop->heap_offset % Pagesize ||
	    pop->heap_size % Pagesize) {
		ERR("unaligned heap: off %ju, size %zu",
			pop->heap_offset, pop->heap_size);
		errno = EINVAL;
		return -1;
	}

	return 0;
}
#endif

#if 0
/*
 * redo_log_check_offset -- (internal) check if offset is valid
 */
static int
redo_log_check_offset(void *ctx, uint64_t offset)
{
	PMEMobjpool *pop = ctx;
	return OBJ_OFF_IS_VALID(pop, offset);
}
#endif

/*
 * obj_runtime_init -- (internal) initialize runtime part of the pool header
 */
static int
obj_runtime_init(PMEMobjpool *pop, int rdonly, int boot, unsigned nlanes)
{
	LOG(3, "pop %p rdonly %d boot %d", pop, rdonly, boot);
	struct pool_set *set = pop->set;

	/* run_id is made unique by incrementing the previous value */
	pop->run_id += 2;
	if (pop->run_id == 0)
		pop->run_id += 2;
	pmemops_persist(set, &pop->run_id, sizeof(pop->run_id));

	/*
	 * Use some of the memory pool area for run-time info.  This
	 * run-time state is never loaded from the file, it is always
	 * created here, so no need to worry about byte-order.
	 */
	pop->rdonly = rdonly;

	pop->uuid_lo = pmemobj_get_uuid_lo(pop);

	pop->lanes_desc.runtime_nlanes = nlanes;

	if (boot) {
		if ((errno = obj_boot(pop)) != 0)
			return -1;

#ifdef USE_VG_MEMCHECK
		if (On_valgrind) {
			/* mark unused part of the pool as not accessible */
			void *end = palloc_heap_end(&pop->heap);
			VALGRIND_DO_MAKE_MEM_NOACCESS(end,
					(void *)pop + pop->size - end);
		}
#endif

		obj_pool_init();

		if ((errno = cuckoo_insert(pools_ht, pop->uuid_lo, pop)) != 0) {
			ERR("!cuckoo_insert");
			return -1;
		}

		if ((errno = ctree_insert(pools_tree, (uint64_t)pop, pop->size))
				!= 0) {
			ERR("!ctree_insert");
			return -1;
		}
	}

	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use. It is not considered an error if this fails.
	 */
	SET_RANGE_NONE(set, pop->addr, sizeof(struct pool_hdr));

	return 0;
}

/*
 * pmemobj_create -- create a transactional memory pool (set)
 */
PMEMobjpool *
pmemobj_create(const char *path, const char *layout, size_t poolsize,
		mode_t mode)
{
	LOG(3, "path %s layout %s poolsize %zu mode %o",
			path, layout, poolsize, mode);

	PMEMobjpool *pop;
	struct pool_set *set;

	/* check length of layout */
	if (layout && (strlen(layout) >= PMEMOBJ_MAX_LAYOUT)) {
		ERR("Layout too long");
		errno = EINVAL;
		return NULL;
	}

	/*
	 * A number of lanes available at runtime equals the lowest value
	 * from all reported by remote replicas hosts. In the single host mode
	 * the runtime number of lanes is equal to the total number of lanes
	 * available in the pool.
	 */
	unsigned runtime_nlanes = OBJ_NLANES;

	if (util_pool_create(&set, path, poolsize, &Obj_ht, &runtime_nlanes,
			REPLICAS_ENABLED) != 0) {
		LOG(2, "cannot create pool or pool set");
		return NULL;
	}

	ASSERT(set->nreplicas > 0);

	/* pop is master replica from now on */
	pop = set->p_ops.base;
	pop->set = set;

	/* create pool descriptor */
	if (obj_descr_create(pop, layout, set->poolsize) != 0) {
		LOG(2, "creation of pool descriptor failed");
		goto err;
	}

	/* initialize runtime parts - lanes, obj stores, ... */
	if (obj_runtime_init(pop, 0, 1 /* boot */, runtime_nlanes) != 0) {
		ERR("pool initialization failed");
		goto err;
	}

	if (util_poolset_chmod(set, mode))
		goto err;

	util_poolset_fdclose(set);

	LOG(3, "pop %p", pop);

	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	//if (set->remote)
	//	obj_cleanup_remote(pop);
	util_poolset_close(set, 1);
	errno = oerrno;
	return NULL;
}

/*
 * obj_check_basic_local -- (internal) basic pool consistency check
 *                              of a local replica
 */
static int
obj_check_basic_local(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	ASSERTeq(pop->set->remote, 0);

	int consistent = 1;

	if (pop->run_id % 2) {
		ERR("invalid run_id %ju", pop->run_id);
		consistent = 0;
	}

	if ((errno = lane_check(pop)) != 0) {
		LOG(2, "!lane_check");
		consistent = 0;
	}

	errno = palloc_heap_check((char *)pop + pop->heap_offset,
			pop->heap_size);
	if (errno != 0) {
		LOG(2, "!heap_check");
		consistent = 0;
	}

	return consistent;
}

#if 0
/*
 * obj_check_basic_remote -- (internal) basic pool consistency check
 *                               of a remote replica
 */
static int
obj_check_basic_remote(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	ASSERTne(pop->set->rmeote, 0);

	int consistent = 1;

	/* read pop->run_id */
	if (obj_read_remote(pop->rpp, pop->remote_base, &pop->run_id,
			&pop->run_id, sizeof(pop->run_id))) {
		ERR("!obj_read_remote");
		return -1;
	}

	if (pop->run_id % 2) {
		ERR("invalid run_id %ju", pop->run_id);
		consistent = 0;
	}

	/* XXX add lane_check_remote */

	errno = palloc_heap_check_remote((char *)pop + pop->heap_offset,
			pop->heap_size, &pop->p_ops.remote);
	if (errno != 0) {
		LOG(2, "!heap_check_remote");
		consistent = 0;
	}

	return consistent;
}
#endif

/*
 * obj_check_basic -- (internal) basic pool consistency check
 *
 * Used to check if all the replicas are consistent prior to pool recovery.
 */
#if 0
static int
obj_check_basic(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if (!pop->set->remote)
		return obj_check_basic_local(pop);
	else
		return obj_check_basic_remote(pop);
}
#endif

static int
obj_check_basic(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	return obj_check_basic_local(pop);
}

/*
 * obj_open_common -- open a transactional memory pool (set)
 *
 * This routine does all the work, but takes a cow flag so internal
 * calls can map a read-only pool if required.
 */
static PMEMobjpool *
obj_open_common(const char *path, const char *layout, int cow, int boot)
{
	LOG(3, "path %s layout %s cow %d", path, layout, cow);

	PMEMobjpool *pop = NULL;
	struct pool_set *set;

	/*
	 * A number of lanes available at runtime equals the lowest value
	 * from all reported by remote replicas hosts. In the single host mode
	 * the runtime number of lanes is equal to the total number of lanes
	 * available in the pool.
	 */
	unsigned runtime_nlanes = OBJ_NLANES;

	if (util_pool_open(&set, path, cow, &Obj_ht, &runtime_nlanes) != 0) {
		LOG(2, "cannot open pool or pool set");
		return NULL;
	}

	ASSERT(set->nreplicas > 0);

	/* read-only mode is not supported in libpmemobj */
	if (set->rdonly) {
		ERR("read-only mode is not supported");
		errno = EINVAL;
		goto err;
	}

	/* pop is master replica from now on */
	pop = set->p_ops.base;
	pop->set = set;

	set->poolsize = pop->heap_offset + pop->heap_size;

	if (boot) {
		/* check consistency of 'master' replica */
		if (obj_check_basic(pop) == 0) {
			goto err;
		}
	}

#if 0
	/*
	 * If there is more than one replica, check if all of them are
	 * consistent (recoverable).
	 * On success, choose any replica and copy entire lanes (redo logs)
	 * to all the other replicas to synchronize them.
	 */
	if (set->nreplicas > 1) {
		PMEMobjpool *rep;
		for (unsigned r = 0; r < set->nreplicas; r++) {
			rep = set->replica[r]->part[0].addr;
			if (obj_check_basic(rep) == 0) {
				ERR("inconsistent replica #%u", r);
				goto err;
			}
		}

		/* copy lanes */
		void *src = (void *)((uintptr_t)pop + pop->lanes_offset);
		size_t len = pop->nlanes * sizeof(struct lane_layout);

		for (unsigned r = 1; r < set->nreplicas; r++) {
			rep = set->replica[r]->part[0].addr;
			void *dst = (void *)((uintptr_t)rep +
						pop->lanes_offset);
			if (rep->rpp == NULL) {
				rep->memcpy_persist_local(dst, src, len);
			} else {
				if (rep->persist_remote(rep, dst, len,
						RLANE_DEFAULT) == NULL)
					obj_handle_remote_persist_error(pop);
			}
		}
	}
#endif

	/*
	 * before runtime initialization lanes are unavailable, remote persists
	 * should use RLANE_DEFAULT
	 */
	pop->lanes_desc.runtime_nlanes = 0;

#ifdef USE_VG_MEMCHECK
	pop->vg_boot = boot;
#endif
	/* initialize runtime parts - lanes, obj stores, ... */
	if (obj_runtime_init(pop, 0, boot, runtime_nlanes) != 0) {
		ERR("pool initialization failed");
		goto err;
	}

#ifdef USE_VG_MEMCHECK
	if (boot)
		obj_vg_boot(pop);
#endif

	util_poolset_fdclose(set);

	LOG(3, "pop %p", pop);

	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	//if (set->remote)
	//	obj_cleanup_remote(pop);
	util_poolset_close(set, 0);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobj_open -- open a transactional memory pool
 */
PMEMobjpool *
pmemobj_open(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	return obj_open_common(path, layout, Open_cow, 1);
}

/*
 * obj_pool_cleanup -- (internal) cleanup the pool and unmap
 */
static void
obj_pool_cleanup(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	palloc_heap_cleanup(&pop->heap);

	lane_cleanup(pop);

	/* unmap all the replicas */
	//obj_replicas_cleanup(pop->set); /* moved to util_poolset_close */
	util_poolset_close(pop->set, 0);
}

/*
 * pmemobj_close -- close a transactional memory pool
 */
void
pmemobj_close(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	_pobj_cache_invalidate++;

	if (cuckoo_remove(pools_ht, pop->uuid_lo) != pop) {
		ERR("cuckoo_remove");
	}

	if (ctree_remove(pools_tree, (uint64_t)pop, 1) != (uint64_t)pop) {
		ERR("ctree_remove");
	}

#ifndef _WIN32

	if (_pobj_cached_pool.pop == pop) {
		_pobj_cached_pool.pop = NULL;
		_pobj_cached_pool.uuid_lo = 0;
	}

#else /* _WIN32 */

	struct _pobj_pcache *pcache = pthread_getspecific(Cached_pool_key);
	if (pcache != NULL) {
		if (pcache->pop == pop) {
			pcache->pop = NULL;
			pcache->uuid_lo = 0;
		}
	}

#endif /* _WIN32 */

	obj_pool_cleanup(pop);
}

/*
 * pmemobj_check -- transactional memory pool consistency check
 */
int
pmemobj_check(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	PMEMobjpool *pop = obj_open_common(path, layout, 1, 0);
	if (pop == NULL)
		return -1;	/* errno set by pmemobj_open_common() */

	int consistent = 1;

	/*
	 * For replicated pools, basic consistency check is performed
	 * in pmemobj_open_common().
	 */
	if (!pop->set->replica)
		consistent = obj_check_basic(pop);

	if (consistent && (errno = obj_boot(pop)) != 0) {
		LOG(3, "!pmemobj_boot");
		consistent = 0;
	}

	if (consistent) {
		obj_pool_cleanup(pop);
	} else {
		/* unmap all the replicas */
		//obj_replicas_cleanup(pop->set);
		util_poolset_close(pop->set, 0);
	}

	if (consistent)
		LOG(4, "pool consistency check OK");

	return consistent;
}

/*
 * pmemobj_pool_by_oid -- returns the pool handle associated with the oid
 */
PMEMobjpool *
pmemobj_pool_by_oid(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	/* XXX this is a temporary fix, to be fixed properly later */
	if (pools_ht == NULL)
		return NULL;

	return cuckoo_get(pools_ht, oid.pool_uuid_lo);
}

/*
 * pmemobj_pool_by_ptr -- returns the pool handle associated with the address
 */
PMEMobjpool *
pmemobj_pool_by_ptr(const void *addr)
{
	LOG(3, "addr %p", addr);

	/* fast path for transactions */
	PMEMobjpool *pop = tx_get_pop();

	if ((pop != NULL) && OBJ_PTR_FROM_POOL(pop, addr))
		return pop;

	/* XXX this is a temporary fix, to be fixed properly later */
	if (pools_tree == NULL)
		return NULL;

	uint64_t key = (uint64_t)addr;
	size_t pool_size = ctree_find_le(pools_tree, &key);

	if (pool_size == 0)
		return NULL;

	ASSERT((uint64_t)addr >= key);
	uint64_t addr_off = (uint64_t)addr - key;

	if (pool_size <= addr_off)
		return NULL;

	return (PMEMobjpool *)key;
}

/* arguments for constructor_alloc_bytype */
struct carg_bytype {
	type_num_t user_type;
	int zero_init;
	pmemobj_constr constructor;
	void *arg;
};

/*
 * constructor_alloc_bytype -- (internal) constructor for obj_alloc_construct
 */
static int
constructor_alloc_bytype(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	PMEMobjpool *pop = ctx;
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);
	struct pool_set *set = pop->set;

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);
	struct carg_bytype *carg = arg;

	pobj->type_num = carg->user_type;
	pobj->size = 0;
	memset(pobj->unused, 0, sizeof(pobj->unused));

	if (carg->zero_init)
		pmemops_memset_persist(set, ptr, 0, usable_size);

	int ret = 0;
	if (carg->constructor)
		ret = carg->constructor(pop, ptr, carg->arg);

	return ret;
}

/*
 * obj_alloc_construct -- (internal) allocates a new object with constructor
 */
static int
obj_alloc_construct(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
	type_num_t type_num, int zero_init,
	pmemobj_constr constructor,
	void *arg)
{
	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	struct carg_bytype carg;

	carg.user_type = type_num;
	carg.zero_init = zero_init;
	carg.constructor = constructor;
	carg.arg = arg;

	struct redo_log *redo = pmalloc_redo_hold(pop);

	struct operation_context ctx;
	operation_init(&ctx, pop, pop->redo, redo);

	if (oidp)
		operation_add_entry(&ctx, &oidp->pool_uuid_lo, pop->uuid_lo,
				OPERATION_SET);

	int ret = pmalloc_operation(&pop->heap, 0,
			oidp != NULL ? &oidp->off : NULL, size + OBJ_OOB_SIZE,
			constructor_alloc_bytype, &carg, &ctx);

	pmalloc_redo_release(pop);

	return ret;
}

/*
 * pmemobj_alloc -- allocates a new object
 */
int
pmemobj_alloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
	uint64_t type_num, pmemobj_constr constructor, void *arg)
{
	LOG(3, "pop %p oidp %p size %zu type_num %llx constructor %p arg %p",
		pop, oidp, size, (unsigned long long)type_num,
		constructor, arg);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (size == 0) {
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	return obj_alloc_construct(pop, oidp, size, type_num,
			0, constructor, arg);
}

/* arguments for constructor_realloc and constructor_zrealloc */
struct carg_realloc {
	void *ptr;
	size_t old_size;
	size_t new_size;
	int zero_init;
	type_num_t user_type;
	pmemobj_constr constructor;
	void *arg;
};

/*
 * pmemobj_zalloc -- allocates a new zeroed object
 */
int
pmemobj_zalloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		uint64_t type_num)
{
	LOG(3, "pop %p oidp %p size %zu type_num %llx",
			pop, oidp, size, (unsigned long long)type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (size == 0) {
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	return obj_alloc_construct(pop, oidp, size, type_num,
					1, NULL, NULL);
}

/*
 * obj_free -- (internal) free an object
 */
static void
obj_free(PMEMobjpool *pop, PMEMoid *oidp)
{
	ASSERTne(oidp, NULL);

	struct redo_log *redo = pmalloc_redo_hold(pop);

	struct operation_context ctx;
	operation_init(&ctx, pop, pop->redo, redo);

	operation_add_entry(&ctx, &oidp->pool_uuid_lo, 0, OPERATION_SET);

	pmalloc_operation(&pop->heap, oidp->off, &oidp->off, 0, NULL, NULL,
			&ctx);

	pmalloc_redo_release(pop);
}

/*
 * constructor_realloc -- (internal) constructor for pmemobj_realloc
 */
static int
constructor_realloc(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	PMEMobjpool *pop = ctx;
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);
	struct pool_set *set = pop->set;

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_realloc *carg = arg;
	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);

	if (ptr != carg->ptr) {
		pobj->type_num = carg->user_type;
		pobj->size = 0;
	}

	if (!carg->zero_init)
		return 0;

	if (usable_size > carg->old_size) {
		size_t grow_len = usable_size - carg->old_size;
		void *new_data_ptr = (void *)((uintptr_t)ptr + carg->old_size);

		pmemops_memset_persist(set, new_data_ptr, 0, grow_len);
	}

	return 0;
}

/*
 * obj_realloc_common -- (internal) common routine for resizing
 *                          existing objects
 */
static int
obj_realloc_common(PMEMobjpool *pop,
	PMEMoid *oidp, size_t size, type_num_t type_num, int zero_init)
{
	/* if OID is NULL just allocate memory */
	if (OBJ_OID_IS_NULL(*oidp)) {
		/* if size is 0 - do nothing */
		if (size == 0)
			return 0;

		return obj_alloc_construct(pop, oidp, size, type_num,
				zero_init, NULL, NULL);
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	/* if size is 0 just free */
	if (size == 0) {
		obj_free(pop, oidp);
		return 0;
	}

	struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, *oidp);
	type_num_t user_type_old = pobj->type_num;

	struct carg_realloc carg;
	carg.ptr = OBJ_OFF_TO_PTR(pop, oidp->off);
	carg.new_size = size;
	carg.old_size = pmemobj_alloc_usable_size(*oidp);
	carg.user_type = type_num;
	carg.constructor = NULL;
	carg.arg = NULL;
	carg.zero_init = zero_init;

	struct redo_log *redo = pmalloc_redo_hold(pop);

	struct operation_context ctx;
	operation_init(&ctx, pop, pop->redo, redo);

	int ret;
	if (type_num == user_type_old) {
		ret = pmalloc_operation(&pop->heap, oidp->off, &oidp->off,
			size + OBJ_OOB_SIZE,
			constructor_realloc, &carg, &ctx);
	} else {
		operation_add_entry(&ctx, &pobj->type_num, type_num,
				OPERATION_SET);

		ret = pmalloc_operation(&pop->heap, oidp->off, &oidp->off,
			size + OBJ_OOB_SIZE, constructor_realloc, &carg, &ctx);
	}
	pmalloc_redo_release(pop);

	return ret;
}

/*
 * constructor_zrealloc_root -- (internal) constructor for pmemobj_root
 */
static int
constructor_zrealloc_root(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	PMEMobjpool *pop = ctx;
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_realloc *carg = arg;

	VALGRIND_ADD_TO_TX(OOB_HEADER_FROM_PTR(ptr),
		usable_size + OBJ_OOB_SIZE);

	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);

	constructor_realloc(pop, ptr, usable_size, arg);
	if (ptr != carg->ptr) {
		pobj->size = carg->new_size | OBJ_INTERNAL_OBJECT_MASK;
	}

	int ret = 0;
	if (carg->constructor)
		ret = carg->constructor(pop, ptr, carg->arg);

	VALGRIND_REMOVE_FROM_TX(OOB_HEADER_FROM_PTR(ptr),
		carg->new_size + OBJ_OOB_SIZE);

	return ret;
}

/*
 * pmemobj_realloc -- resizes an existing object
 */
int
pmemobj_realloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		uint64_t type_num)
{
	ASSERTne(oidp, NULL);

	LOG(3, "pop %p oid.off 0x%016jx size %zu type_num %lu",
		pop, oidp->off, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));

	return obj_realloc_common(pop, oidp, size, (type_num_t)type_num, 0);
}

/*
 * pmemobj_zrealloc -- resizes an existing object, any new space is zeroed.
 */
int
pmemobj_zrealloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		uint64_t type_num)
{
	ASSERTne(oidp, NULL);

	LOG(3, "pop %p oid.off 0x%016jx size %zu type_num %lu",
		pop, oidp->off, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));

	return obj_realloc_common(pop, oidp, size, (type_num_t)type_num, 1);
}

/* arguments for constructor_strdup */
struct carg_strdup {
	size_t size;
	const char *s;
};

/*
 * constructor_strdup -- (internal) constructor of pmemobj_strdup
 */
static int
constructor_strdup(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_strdup *carg = arg;

	/* copy string */
	pmemops_memcpy_persist(pop->set, ptr, carg->s, carg->size);

	return 0;
}

/*
 * pmemobj_strdup -- allocates a new object with duplicate of the string s.
 */
int
pmemobj_strdup(PMEMobjpool *pop, PMEMoid *oidp, const char *s,
		uint64_t type_num)
{
	LOG(3, "pop %p oidp %p string %s type_num %lu", pop, oidp, s, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (NULL == s) {
		errno = EINVAL;
		return -1;
	}

	struct carg_strdup carg;
	carg.size = (strlen(s) + 1) * sizeof(char);
	carg.s = s;

	return obj_alloc_construct(pop, oidp, carg.size,
		(type_num_t)type_num, 0, constructor_strdup, &carg);
}

/*
 * pmemobj_free -- frees an existing object
 */
void
pmemobj_free(PMEMoid *oidp)
{
	ASSERTne(oidp, NULL);

	LOG(3, "oid.off 0x%016jx", oidp->off);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (oidp->off == 0)
		return;

	PMEMobjpool *pop = pmemobj_pool_by_oid(*oidp);

	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));

	obj_free(pop, oidp);
}

/*
 * pmemobj_alloc_usable_size -- returns usable size of object
 */
size_t
pmemobj_alloc_usable_size(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	if (oid.off == 0)
		return 0;

	PMEMobjpool *pop = pmemobj_pool_by_oid(oid);

	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	return (palloc_usable_size(&pop->heap, oid.off) - OBJ_OOB_SIZE);
}

/*
 * obj_handle_remote_persist_error -- (internal) handle remote persist
 *                                    fatal error
 */
static void
obj_handle_remote_persist_error(PMEMobjpool *pop)
{
	LOG(1, "pop %p", pop);

	ERR("error clean up...");
	obj_pool_cleanup(pop);

	FATAL("Fatal error of remote persist. Aborting...");
}

/*
 * pmemobj_memcpy_persist -- pmemobj version of memcpy
 */
void *
pmemobj_memcpy_persist(PMEMobjpool *pop, void *dest, const void *src,
	size_t len)
{
	LOG(15, "pop %p dest %p src %p len %zu", pop, dest, src, len);

	return pmemops_memcpy_persist(pop->set, dest, src, len);
}

/*
 * pmemobj_memset_persist -- pmemobj version of memset
 */
void *
pmemobj_memset_persist(PMEMobjpool *pop, void *dest, int c, size_t len)
{
	LOG(15, "pop %p dest %p c 0x%02x len %zu", pop, dest, c, len);

	return pmemops_memset_persist(pop->set, dest, c, len);
}

/*
 * pmemobj_persist -- pmemobj version of pmem_persist
 */
void
pmemobj_persist(PMEMobjpool *pop, const void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);

	if (pmemops_persist(pop->set, addr, len) != 0)
		obj_handle_remote_persist_error(pop);
}

/*
 * pmemobj_flush -- pmemobj version of pmem_flush
 */
void
pmemobj_flush(PMEMobjpool *pop, const void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);

	if (pmemops_flush(pop->set, addr, len) != 0)
		obj_handle_remote_persist_error(pop);
}

/*
 * pmemobj_drain -- pmemobj version of pmem_drain
 */
void
pmemobj_drain(PMEMobjpool *pop)
{
	LOG(15, "pop %p", pop);

	pmemops_drain(pop->set);
}

/*
 * pmemobj_type_num -- returns type number of object
 */
uint64_t
pmemobj_type_num(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	ASSERT(!OID_IS_NULL(oid));

	void *ptr = pmemobj_direct(oid);

	struct oob_header *oobh = OOB_HEADER_FROM_PTR(ptr);
	return oobh->type_num;
}

/* arguments for constructor_alloc_root */
struct carg_root {
	size_t size;
	pmemobj_constr constructor;
	void *arg;
};

/*
 * constructor_alloc_root -- (internal) constructor for obj_alloc_root
 */
static int
constructor_alloc_root(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	PMEMobjpool *pop = ctx;
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);
	struct pool_set *set = pop->set;

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	int ret = 0;

	struct oob_header *ro = OOB_HEADER_FROM_PTR(ptr);
	struct carg_root *carg = arg;

	/* temporarily add atomic root allocation to pmemcheck transaction */
	VALGRIND_ADD_TO_TX(ro, OBJ_OOB_SIZE + usable_size);

	if (carg->constructor)
		ret = carg->constructor(pop, ptr, carg->arg);
	else
		pmemops_memset_persist(set, ptr, 0, usable_size);

	ro->type_num = POBJ_ROOT_TYPE_NUM;
	ro->size = carg->size | OBJ_INTERNAL_OBJECT_MASK;

	VALGRIND_REMOVE_FROM_TX(ro, OBJ_OOB_SIZE + usable_size);

	return ret;
}

/*
 * obj_alloc_root -- (internal) allocate root object
 */
static int
obj_alloc_root(PMEMobjpool *pop, size_t size,
	pmemobj_constr constructor, void *arg)
{
	LOG(3, "pop %p size %zu", pop, size);

	struct carg_root carg;

	carg.size = size;
	carg.constructor = constructor;
	carg.arg = arg;

	return pmalloc_construct(pop, &pop->root_offset,
		size + OBJ_OOB_SIZE, constructor_alloc_root, &carg);
}

/*
 * obj_realloc_root -- (internal) reallocate root object
 */
static int
obj_realloc_root(PMEMobjpool *pop, size_t size,
	size_t old_size,
	pmemobj_constr constructor, void *arg)
{
	LOG(3, "pop %p size %zu old_size %zu", pop, size, old_size);

	struct carg_realloc carg;

	carg.ptr = OBJ_OFF_TO_PTR(pop, pop->root_offset);
	carg.old_size = old_size;
	carg.new_size = size;
	carg.user_type = POBJ_ROOT_TYPE_NUM;
	carg.constructor = constructor;
	carg.zero_init = 1;
	carg.arg = arg;

	struct redo_log *redo = pmalloc_redo_hold(pop);

	struct operation_context ctx;
	operation_init(&ctx, pop, pop->redo, redo);

	int ret = pmalloc_operation(&pop->heap, pop->root_offset,
			&pop->root_offset, size + OBJ_OOB_SIZE,
			constructor_zrealloc_root, &carg, &ctx);

	pmalloc_redo_release(pop);

	return ret;
}

/*
 * pmemobj_root_size -- returns size of the root object
 */
size_t
pmemobj_root_size(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if (pop->root_offset) {
		struct oob_header *ro =
			OOB_HEADER_FROM_OFF(pop, pop->root_offset);
		return OBJ_ROOT_SIZE(ro);
	} else
		return 0;
}

/*
 * pmemobj_root_construct -- returns root object
 */
PMEMoid
pmemobj_root_construct(PMEMobjpool *pop, size_t size,
	pmemobj_constr constructor, void *arg)
{
	LOG(3, "pop %p size %zu constructor %p args %p", pop, size, constructor,
		arg);

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return OID_NULL;
	}

	PMEMoid root;

	pmemobj_mutex_lock_nofail(pop, &pop->rootlock);

	if (pop->root_offset == 0)
		obj_alloc_root(pop, size, constructor, arg);
	else {
		size_t old_size = pmemobj_root_size(pop);
		if (size > old_size && obj_realloc_root(pop, size,
				old_size, constructor, arg)) {
			pmemobj_mutex_unlock_nofail(pop, &pop->rootlock);
			LOG(2, "obj_realloc_root failed");
			return OID_NULL;
		}
	}
	root.pool_uuid_lo = pop->uuid_lo;
	root.off = pop->root_offset;

	pmemobj_mutex_unlock_nofail(pop, &pop->rootlock);
	return root;
}

/*
 * pmemobj_root -- returns root object
 */
PMEMoid
pmemobj_root(PMEMobjpool *pop, size_t size)
{
	LOG(3, "pop %p size %zu", pop, size);

	return pmemobj_root_construct(pop, size, NULL, NULL);
}

/*
 * pmemobj_first - returns first object of specified type
 */
PMEMoid
pmemobj_first(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	PMEMoid ret = {0, 0};

	uint64_t off = palloc_first(&pop->heap);
	if (off != 0) {
		ret.off = off + OBJ_OOB_SIZE;
		ret.pool_uuid_lo = pop->uuid_lo;

		struct oob_header *oobh = OOB_HEADER_FROM_OFF(pop, ret.off);
		if (oobh->size & OBJ_INTERNAL_OBJECT_MASK) {
			return pmemobj_next(ret);
		}
	}

	return ret;
}

/*
 * pmemobj_next - returns next object of specified type
 */
PMEMoid
pmemobj_next(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	if (oid.off == 0)
		return OID_NULL;

	PMEMobjpool *pop = pmemobj_pool_by_oid(oid);

	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	PMEMoid ret = {0, 0};
	uint64_t off = palloc_next(&pop->heap, oid.off);
	if (off != 0) {
		ret.off = off + OBJ_OOB_SIZE;
		ret.pool_uuid_lo = pop->uuid_lo;

		struct oob_header *oobh = OOB_HEADER_FROM_OFF(pop, ret.off);
		if (oobh->size & OBJ_INTERNAL_OBJECT_MASK) {
			return pmemobj_next(ret);
		}
	}

	return ret;
}

/*
 * pmemobj_list_insert -- adds object to a list
 */
int
pmemobj_list_insert(PMEMobjpool *pop, size_t pe_offset, void *head,
		    PMEMoid dest, int before, PMEMoid oid)
{
	LOG(3, "pop %p pe_offset %zu head %p dest.off 0x%016jx before %d"
	    " oid.off 0x%016jx",
	    pop, pe_offset, head, dest.off, before, oid.off);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, oid));
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	if (pe_offset >= pop->size) {
		ERR("pe_offset (%lu) too big", pe_offset);
		errno = EINVAL;
		return -1;
	}

	return list_insert(pop, (ssize_t)pe_offset, head, dest, before, oid);
}

/*
 * pmemobj_list_insert_new -- adds new object to a list
 */
PMEMoid
pmemobj_list_insert_new(PMEMobjpool *pop, size_t pe_offset, void *head,
			PMEMoid dest, int before, size_t size,
			uint64_t type_num,
			pmemobj_constr constructor, void *arg)
{
	LOG(3, "pop %p pe_offset %zu head %p dest.off 0x%016jx before %d"
	    " size %zu type_num %lu",
	    pop, pe_offset, head, dest.off, before, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return OID_NULL;
	}

	if (pe_offset >= pop->size) {
		ERR("pe_offset (%lu) too big", pe_offset);
		errno = EINVAL;
		return OID_NULL;
	}

	struct carg_bytype carg;

	carg.user_type = (type_num_t)type_num;
	carg.constructor = constructor;
	carg.arg = arg;
	carg.zero_init = 0;

	PMEMoid retoid = OID_NULL;
	list_insert_new_user(pop,
			pe_offset, head, dest, before,
			size, constructor_alloc_bytype, &carg, &retoid);
	return retoid;
}

/*
 * pmemobj_list_remove -- removes object from a list
 */
int
pmemobj_list_remove(PMEMobjpool *pop, size_t pe_offset, void *head,
		    PMEMoid oid, int free)
{
	LOG(3, "pop %p pe_offset %zu head %p oid.off 0x%016jx free %d",
	    pop, pe_offset, head, oid.off, free);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	if (pe_offset >= pop->size) {
		ERR("pe_offset (%lu) too big", pe_offset);
		errno = EINVAL;
		return -1;
	}

	if (free) {
		return list_remove_free_user(pop, pe_offset, head, &oid);
	} else {
		return list_remove(pop, (ssize_t)pe_offset, head, oid);
	}
}

/*
 * pmemobj_list_move -- moves object between lists
 */
int
pmemobj_list_move(PMEMobjpool *pop, size_t pe_old_offset, void *head_old,
			size_t pe_new_offset, void *head_new,
			PMEMoid dest, int before, PMEMoid oid)
{
	LOG(3, "pop %p pe_old_offset %zu pe_new_offset %zu"
	    " head_old %p head_new %p dest.off 0x%016jx"
	    " before %d oid.off 0x%016jx",
	    pop, pe_old_offset, pe_new_offset,
	    head_old, head_new, dest.off, before, oid.off);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	ASSERT(OBJ_OID_IS_VALID(pop, oid));
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	if (pe_old_offset >= pop->size) {
		ERR("pe_old_offset (%lu) too big", pe_old_offset);
		errno = EINVAL;
		return -1;
	}

	if (pe_new_offset >= pop->size) {
		ERR("pe_new_offset (%lu) too big", pe_new_offset);
		errno = EINVAL;
		return -1;
	}

	return list_move(pop, pe_old_offset, head_old,
				pe_new_offset, head_new,
				dest, before, oid);
}

/*
 * _pobj_debug_notice -- logs notice message if used inside a transaction
 */
void
_pobj_debug_notice(const char *api_name, const char *file, int line)
{
#ifdef DEBUG
	if (pmemobj_tx_stage() != TX_STAGE_NONE) {
		if (file)
			LOG(4, "Notice: non-transactional API"
				" used inside a transaction (%s in %s:%d)",
				api_name, file, line);
		else
			LOG(4, "Notice: non-transactional API"
				" used inside a transaction (%s)", api_name);
	}
#endif /* DEBUG */
}

#ifdef _MSC_VER
/*
 * libpmemobj constructor/destructor functions
 */
MSVC_CONSTR(libpmemobj_init)
MSVC_DESTR(libpmemobj_fini)
#endif
