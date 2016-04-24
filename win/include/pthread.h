/*
 * Copyright 2015-2016, Intel Corporation
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
 * pthread.h -- (imperfect) POSIX threads for Windows
 */

#pragma once

#include <stdint.h>


#define	pthread_t int
#define	pthread_attr_t int
#define	pthread_once_t long
#define	pthread_key_t DWORD

#define	PTHREAD_ONCE_INIT 0

int pthread_once(pthread_once_t *o, void (*func)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);



#define	USE_WIN_SRWLOCK

typedef struct {
	unsigned attr;
	CRITICAL_SECTION lock;
} pthread_mutex_t;

#ifdef USE_WIN_SRWLOCK

typedef struct {
	unsigned attr;
	SRWLOCK lock;
} pthread_rwlock_t;

#else

#define	pthread_rwlock_t pthread_mutex_t

#endif

typedef struct {
	unsigned attr;
	CONDITION_VARIABLE cond;
} pthread_cond_t;


#define	pthread_mutexattr_t int
#define	pthread_rwlockattr_t int
#define	pthread_condattr_t int


/* Mutex types */
enum
{
	PTHREAD_MUTEX_NORMAL = 0,
	PTHREAD_MUTEX_RECURSIVE = 1,
	PTHREAD_MUTEX_ERRORCHECK = 2,
	PTHREAD_MUTEX_DEFAULT = PTHREAD_MUTEX_NORMAL
};

/* RWlock types */
enum
{
	PTHREAD_RWLOCK_PREFER_READER = 0,
	PTHREAD_RWLOCK_PREFER_WRITER = 1,
	PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE = 2,
	PTHREAD_RWLOCK_DEFAULT = PTHREAD_RWLOCK_PREFER_READER
};



int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);

int pthread_mutexattr_gettype(const pthread_mutexattr_t *restrict attr,
	int *restrict type);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);



int pthread_mutex_init(pthread_mutex_t *restrict mutex,
	const pthread_mutexattr_t *restrict attr);
int pthread_mutex_destroy(pthread_mutex_t *restrict mutex);
int pthread_mutex_lock(pthread_mutex_t *restrict mutex);
int pthread_mutex_trylock(pthread_mutex_t *restrict mutex);
int pthread_mutex_unlock(pthread_mutex_t *restrict mutex);

/* XXX - non POSIX */
int pthread_mutex_timedlock(pthread_mutex_t *restrict mutex,
	const struct timespec *abstime);

#ifdef USE_WIN_SRWLOCK

int pthread_rwlock_init(pthread_rwlock_t *restrict rwlock,
	const pthread_rwlockattr_t *restrict attr);
int pthread_rwlock_destroy(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *restrict rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *restrict rwlock,
	const struct timespec *abstime);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *restrict rwlock,
	const struct timespec *abstime);

#else

#define	pthread_rwlock_init pthread_mutex_init
#define	pthread_rwlock_destroy pthread_mutex_destroy
#define	pthread_rwlock_rdlock pthread_mutex_lock
#define	pthread_rwlock_tryrdlock pthread_mutex_trylock
#define	pthread_rwlock_trywrlock pthread_mutex_trylock
#define	pthread_rwlock_unlock pthread_mutex_unlock
#define	pthread_rwlock_wrlock pthread_mutex_lock
#define	pthread_rwlock_timedrdlock pthread_mutex_timedlock
#define	pthread_rwlock_timedwrlock pthread_mutex_timedlock

#endif


int pthread_cond_init(pthread_cond_t *restrict cond,
	const pthread_condattr_t *restrict attr);
int pthread_cond_destroy(pthread_cond_t *restrict cond);
int pthread_cond_broadcast(pthread_cond_t *restrict cond);
int pthread_cond_signal(pthread_cond_t *restrict cond);
int pthread_cond_timedwait(pthread_cond_t *restrict cond,
	pthread_mutex_t *restrict mutex, const struct timespec *abstime);
int pthread_cond_wait(pthread_cond_t *restrict cond,
	pthread_mutex_t *restrict mutex);
