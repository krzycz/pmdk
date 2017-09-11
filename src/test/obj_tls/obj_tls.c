/*
 * Copyright 2017, Intel Corporation
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
 * obj_tls.c -- unit test for TLS destructors
 */
#include "unittest.h"
#include <pthread.h>

#define LAYOUT "TLS"
#define NTHREADS 4

static PMEMobjpool *Pop;
static os_thread_t Threads[NTHREADS];

static pthread_barrier_t Barrier1;
static pthread_barrier_t Barrier2;

static void
do_alloc_free(void)
{
	PMEMoid oid;

	int ret = pmemobj_alloc(Pop, &oid, 64, 0, NULL, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTne(pmemobj_direct(oid), NULL);

	pmemobj_free(&oid);
	//UT_ASSERTeq(oid, OID_NULL);
}

static void *
thread_func(void *arg)
{
	do_alloc_free();
	pthread_barrier_wait(&Barrier1);
	pthread_barrier_wait(&Barrier2);
	return NULL;
}

static void
create_threads(void)
{
	for (int i = 0; i < NTHREADS; i++)
		PTHREAD_CREATE(&Threads[i], NULL, thread_func, NULL);
	pthread_barrier_wait(&Barrier1);
}

static void
join_threads(void)
{
	pthread_barrier_wait(&Barrier2);
	for (int i = 0; i < NTHREADS; i++)
		PTHREAD_JOIN(Threads[i], NULL);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_tls");

	if (argc != 3)
		UT_FATAL("usage: %s path scenario", argv[0]);

	Pop = pmemobj_create(argv[1], LAYOUT, 10 * PMEMOBJ_MIN_POOL,
			S_IWUSR | S_IRUSR);
	if (Pop == NULL)
		UT_FATAL("!pmemobj_create");

	pthread_barrier_init(&Barrier1, NULL, NTHREADS + 1);
	pthread_barrier_init(&Barrier2, NULL, NTHREADS + 1);

	switch (argv[2][0]) {
	case 'a':
		/* main thread does not use the pool */
		create_threads();
		join_threads();
		pmemobj_close(Pop);
		break;

	case 'b':
		/* main thread uses the pool */
		do_alloc_free();
		create_threads();
		join_threads();
		pmemobj_close(Pop);
		break;

	case 'c':
		/* main thread does not use the pool */
		create_threads();
		pmemobj_close(Pop);
		join_threads();
		break;

	case 'd':
		/* main thread uses the pool */
		do_alloc_free();
		create_threads();
		pmemobj_close(Pop);
		join_threads();
		break;

	default:
		UT_FATAL("wrong argument");
	}

	pthread_barrier_destroy(&Barrier1);
	pthread_barrier_destroy(&Barrier2);

	DONE(NULL);
}
