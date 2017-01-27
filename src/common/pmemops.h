/*
 * Copyright 2016-2017, Intel Corporation
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

#ifndef LIBPMEMOBJ_PMEMOPS_H
#define LIBPMEMOBJ_PMEMOPS_H 1

#include <stddef.h>
#include <stdint.h>

/*
 * for each replica - pmem or nonpmem
 */
typedef int (*persist_local_fn)(const void *, size_t);
typedef int (*flush_local_fn)(const void *, size_t);
typedef int (*drain_local_fn)(void);
typedef void *(*memcpy_local_fn)(void *, const void *, size_t);
typedef void *(*memset_local_fn)(void *, int, size_t);

struct rep_pmem_ops {
	persist_local_fn persist;
	flush_local_fn flush;
	drain_local_fn drain;
	memcpy_local_fn memcpy;
	memset_local_fn memset;
};

/*
 * used by master replica only (set) - point to funcs w/ or w/o replication
 */
typedef int (*persist_fn)(const void *base, const void *dest, size_t);
typedef int (*flush_fn)(const void *base, const void *, size_t);
typedef int (*drain_fn)(const void *base);
typedef void *(*memcpy_fn)(const void *base, void *dest, const void *src,
		size_t len);
typedef void *(*memset_fn)(const void *base, void *dest, int c, size_t len);

struct set_pmem_ops {
	/* for 'master' replica: with or without data replication */
	persist_fn persist;	/* persist function */
	flush_fn flush;		/* flush function */
	drain_fn drain;		/* drain function */
	memcpy_fn memcpy_persist; /* persistent memcpy function */
	memset_fn memset_persist; /* persistent memset function */
	memcpy_fn memcpy_nodrain; /* memcpy function */
	memset_fn memset_nodrain; /* memset function */
};

struct pmem_ops {
	struct set_pmem_ops ops;

	void *base;		/* base address of master replica */
	size_t pool_size;
};

/* for remote replicas only */
typedef int (*remote_read_fn)(void *ctx, uintptr_t base, void *dest, void *addr,
		size_t length);

typedef void *(*remote_persist_fn)(void *ctx, const void *addr, size_t len,
		unsigned lane);

struct rep_rpmem_ops {
	remote_persist_fn persist;
	remote_read_fn read;

	void *ctx;
	uintptr_t base;	/* base address of remote replica */
};

#endif
