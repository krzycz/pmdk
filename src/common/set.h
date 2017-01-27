/*
 * Copyright 2014-2017, Intel Corporation
 * Copyright (c) 2016, Microsoft Corporation. All rights reserved.
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
 * set.h -- internal definitions for set module
 */

#ifndef NVML_SET_H
#define NVML_SET_H 1
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pool_hdr.h"
#include "librpmem.h"
#include "pmemops.h"

/*
 * pool sets & replicas
 */
#define POOLSET_HDR_SIG "PMEMPOOLSET"
#define POOLSET_HDR_SIG_LEN 11	/* does NOT include '\0' */

#define POOLSET_REPLICA_SIG "REPLICA"
#define POOLSET_REPLICA_SIG_LEN 7	/* does NOT include '\0' */

#define POOL_LOCAL 0
#define POOL_REMOTE 1

#define REPLICAS_DISABLED 0
#define REPLICAS_ENABLED 1


struct set_lane_descriptor {
	/*
	 * Number of lanes available at runtime must be <= total number of lanes
	 * available in the pool. Number of lanes can be limited by shortage of
	 * other resources e.g. available RNIC's submission queue sizes.
	 */
	unsigned runtime_nlanes;
	unsigned next_lane_idx;
	uint64_t *lane_locks;
	/* XXX */
	/* struct lane *lane; */
};


struct pool_set_part {
	/* populated by a pool set file parser */
	const char *path;
	size_t filesize;	/* aligned to page size */
	int fd;
	int flags;		/* stores flags used when opening the file */
				/* valid only if fd >= 0 */
	int is_dax;		/* indicates if the part is on device dax */
	int created;		/* indicates newly created (zeroed) file */

	/* util_poolset_open/create */
	void *remote_hdr;	/* allocated header for remote replica */
	void *hdr;		/* base address of header */
	size_t hdrsize;		/* size of the header mapping */
	void *addr;		/* base address of the mapping */
	size_t size;		/* size of the mapping - page aligned */
	int rdonly;		/* is set based on compat features, affects */
				/* the whole poolset */
	uuid_t uuid;
};

struct pool_replica {
	unsigned nparts;
	size_t repsize;		/* total size of all the parts (mappings) */
	int is_pmem;		/* true if all the parts are in PMEM */
	int is_dax;		/* true if replica is on Device DAX */

	void *base;		/* local: base address == part[0]->addr */
				/* remote: ... */

	/* local replica ops: pmem or non-pmem */
	struct rep_pmem_ops p_ops;

	/* remote replica section */
	void *rpp;	/* RPMEMpool opaque handle if it is a remote replica */
	/* XXX */
	/* beginning of the pool's descriptor */
	/* uintptr_t remote_base; */
	char *node_addr;	/* address of a remote node */
	char *pool_desc;	/* descriptor of a poolset */
	struct rep_rpmem_ops r_ops;

	struct pool_set_part part[];
};

struct pool_set {
	unsigned nreplicas;
	uuid_t uuid;
	int rdonly;
	int zeroed;		/* true if all the parts are new files */
	size_t poolsize;	/* the smallest replica size */
	int remote;		/* true if contains a remote replica */

	struct pmem_ops p_ops;	/* rep or norep variants */
	struct set_lane_descriptor lanes_desc;

	struct pool_replica *replica[];
};

struct part_file {
	int is_remote;
	const char *path;	/* not-NULL only for a local part file */
	const char *node_addr;	/* address of a remote node */
	/* poolset descriptor is a pool set file name on a remote node */
	const char *pool_desc;	/* descriptor of a poolset */
};

struct pool_attr {
	const unsigned char *poolset_uuid;	/* pool uuid */
	const unsigned char *first_part_uuid;	/* first part uuid */
	const unsigned char *prev_repl_uuid;	/* prev replica uuid */
	const unsigned char *next_repl_uuid;	/* next replica uuid */
	const unsigned char *user_flags;	/* user flags */
};

#define REP(set, r)\
	((set)->replica[((set)->nreplicas + (r)) % (set)->nreplicas])

#define PART(rep, p)\
	((rep)->part[((rep)->nparts + (p)) % (rep)->nparts])

#define HDR(rep, p)\
	((struct pool_hdr *)(PART(rep, p).hdr))

int util_poolset_parse(struct pool_set **setp, const char *path, int fd);
int util_poolset_read(struct pool_set **setp, const char *path);
int util_poolset_create_set(struct pool_set **setp, const char *path,
	size_t poolsize, size_t minsize);
int util_poolset_open(struct pool_set *set);
void util_poolset_close(struct pool_set *set, int del);
void util_poolset_free(struct pool_set *set);
int util_poolset_chmod(struct pool_set *set, mode_t mode);
void util_poolset_fdclose(struct pool_set *set);
int util_is_poolset_file(const char *path);
int util_poolset_foreach_part(const char *path,
	int (*cb)(struct part_file *pf, void *arg), void *arg);
size_t util_poolset_size(const char *path);

int util_pool_create(struct pool_set **setp, const char *path, size_t poolsize,
	const struct pool_hdr_template *ht, unsigned *nlanes, int can_have_rep);
int util_pool_create_uuids(struct pool_set **setp, const char *path,
	size_t poolsize, const struct pool_hdr_template *ht, unsigned *nlanes,
	int can_have_rep, int remote, struct pool_attr *poolattr);

int util_part_open(struct pool_set_part *part, size_t minsize, int create);
void util_part_fdclose(struct pool_set_part *part);
int util_replica_open(struct pool_set *set, unsigned repidx, int flags);
int util_replica_close(struct pool_set *set, unsigned repidx);
int util_map_part(struct pool_set_part *part, void *addr, size_t size,
	size_t offset, int flags, int rdonly);
int util_unmap_part(struct pool_set_part *part);
int util_unmap_parts(struct pool_replica *rep, unsigned start_index,
	unsigned end_index);
int util_header_create(struct pool_set *set, unsigned repidx, unsigned partidx,
	const struct pool_hdr_template *ht, const unsigned char *prev_repl_uuid,
	const unsigned char *next_repl_uuid);

int util_map_hdr(struct pool_set_part *part, int flags, int rdonly);
int util_unmap_hdr(struct pool_set_part *part);

int util_pool_open_nocheck(struct pool_set *set, int rdonly);
int util_pool_open(struct pool_set **setp, const char *path, int rdonly,
	const struct pool_hdr_template *ht, unsigned *nlanes);
int util_pool_open_remote(struct pool_set **setp, const char *path, int rdonly,
	struct pool_hdr_template *ht,
	unsigned char *poolset_uuid, unsigned char *first_part_uuid,
	unsigned char *prev_repl_uuid, unsigned char *next_repl_uuid);

void util_remote_init(void);
void util_remote_fini(void);

void util_remote_init_lock(void);
void util_remote_destroy_lock(void);
int util_pool_close_remote(RPMEMpool *rpp);
void util_remote_unload(void);
void util_replica_fdclose(struct pool_replica *rep);
int util_poolset_remote_open(struct pool_replica *rep, unsigned repidx,
	size_t minsize, int create, void *pool_addr,
	size_t pool_size, unsigned *nlanes);
int util_remote_load(void);
int util_replica_open_remote(struct pool_set *set, unsigned repidx, int flags);
int util_poolset_remote_replica_open(struct pool_set *set, unsigned repidx,
	size_t minsize, int create, unsigned *nlanes);

extern int (*Rpmem_persist)(RPMEMpool *rpp, size_t offset, size_t length,
								unsigned lane);
extern int (*Rpmem_read)(RPMEMpool *rpp, void *buff, size_t offset,
							size_t length);
extern int (*Rpmem_close)(RPMEMpool *rpp);

extern int (*Rpmem_remove)(const char *target,
		const char *pool_set_name, int flags);


extern struct rep_pmem_ops Pmem_ops;
extern struct rep_pmem_ops Nonpmem_ops;
extern struct rep_pmem_ops Remote_ops;

extern struct set_pmem_ops Rep_ops;
extern struct set_pmem_ops Norep_ops;

void *set_memcpy_nodrain_norep(struct pool_set *set, void *dest,
		const void *src, size_t len);
void *set_memset_nodrain_norep(struct pool_set *set, void *dest,
		int c, size_t len);
void *set_memcpy_persist_norep(struct pool_set *set, void *dest,
		const void *src, size_t len);
void *set_memset_persist_norep(struct pool_set *set, void *dest,
		int c, size_t len);
int set_persist_norep(struct pool_set *set, const void *addr, size_t len);
int set_flush_norep(struct pool_set *set, const void *addr, size_t len);
int set_drain_norep(struct pool_set *set);

void *set_memcpy_nodrain(struct pool_set *set, void *dest,
		const void *src, size_t len);
void *set_memset_nodrain(struct pool_set *set, void *dest, int c, size_t len);
void *set_memcpy_persist(struct pool_set *set, void *dest,
		const void *src, size_t len);
void *set_memset_persist(struct pool_set *set, void *dest, int c, size_t len);
int set_persist(struct pool_set *set, const void *addr, size_t len);
int set_flush(struct pool_set *set, const void *addr, size_t len);
int set_drain(struct pool_set *set);

int set_range_ro(struct pool_set *set, void *addr, size_t len);
int set_range_rw(struct pool_set *set, void *addr, size_t len);
int set_range_none(struct pool_set *set, void *addr, size_t len);

/*
 * macros for micromanaging range protections for the debug version
 */
#ifdef DEBUG

#define _RANGE(set, addr, len, type) do {\
	ASSERT(set_range_##type(set, addr, len) >= 0);\
} while (0)

#else

#define _RANGE(set, addr, len, type) do {} while (0)

#endif

#define SET_RANGE_RO(set, addr, len) _RANGE(set, addr, len, ro)
#define SET_RANGE_RW(set, addr, len) _RANGE(set, addr, len, rw)
#define SET_RANGE_NONE(set, addr, len) _RANGE(set, addr, len, none)


#ifdef _MSC_VER
#define force_inline inline
#else
#define force_inline __attribute__((always_inline)) inline
#endif

static force_inline int
pmemops_persist(const struct pool_set *set, const void *d, size_t s)
{
	return set->p_ops.ops.persist(set, d, s);
}

static force_inline int
pmemops_flush(const struct pool_set *set, const void *d, size_t s)
{
	return set->p_ops.ops.flush(set, d, s);
}

static force_inline void
pmemops_drain(const struct pool_set *set)
{
	set->p_ops.ops.drain(set);
}

static force_inline void *
pmemops_memcpy_persist(const struct pool_set *set, void *dest,
		const void *src, size_t len)
{
	return set->p_ops.ops.memcpy_persist(set, dest, src, len);
}

static force_inline void *
pmemops_memset_persist(const struct pool_set *set, void *dest, int c,
		size_t len)
{
	return set->p_ops.ops.memset_persist(set, dest, c, len);
}


static force_inline void *
pmemops_memcpy_nodrain(const struct pool_set *set, void *dest,
		const void *src, size_t len)
{
	return set->p_ops.ops.memcpy_nodrain(set, dest, src, len);
}

static force_inline void *
pmemops_memset_nodrain(const struct pool_set *set, void *dest, int c,
		size_t len)
{
	return set->p_ops.ops.memset_nodrain(set, dest, c, len);
}

struct set_lane_info {
	struct pool_set *set;
	uint64_t lane_idx;
	unsigned long nest_count;
	struct set_lane_info *prev;
	struct set_lane_info *next;
};

unsigned set_lane_hold(struct pool_set *set);
void set_lane_release(struct pool_set *set);

int set_for_each_replica(struct pool_set *set,
		int (*func_cb)(struct pool_replica *rep, void *args),
		void *args);

#ifdef __cplusplus
}
#endif

#endif
