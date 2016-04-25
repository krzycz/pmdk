/*
 * Copyright 2014-2016, Intel Corporation
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
 * util_os.c -- general utilities with OS-specific implementation (Windows)
 */

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <elf.h>
#include <link.h>

#include "util.h"
#include "out.h"

/*
 * set of macros for determining the alignment descriptor
 */
#define	DESC_MASK		((1 << ALIGNMENT_DESC_BITS) - 1)
#define	alignment_of(t)		offsetof(struct { char c; t x; }, x)
#define	alignment_desc_of(t)	(((uint64_t)alignment_of(t) - 1) & DESC_MASK)
#define	alignment_desc()\
(alignment_desc_of(char)	<<  0 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(short)	<<  1 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(int)		<<  2 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(long)	<<  3 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(long long)	<<  4 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(size_t)	<<  5 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(off_t)	<<  6 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(float)	<<  7 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(double)	<<  8 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(long double)	<<  9 * ALIGNMENT_DESC_BITS) |\
(alignment_desc_of(void *)	<< 10 * ALIGNMENT_DESC_BITS)

/*
 * util_map_hint -- determine hint address for mmap()
 */
char *
util_map_hint(size_t len, size_t req_align)
{
	LOG(3, "len %zu req_align %zu", len, req_align);

	/* XXX - no support yet */
	LOG(4, "hint %p", NULL);
	return NULL;
}

/*
 * util_tmpfile --  (internal) create the temporary file
 */
int
util_tmpfile(const char *dir, const char *templ)
{
	LOG(3, "dir \"%s\" template \"%s\"", dir, templ);

	int oerrno;
	int fd = -1;

	char *fullname = alloca(strlen(dir) + sizeof (templ));

#if 1

	(void) strcpy(fullname, dir);
	(void) strcat(fullname, templ);

	sigset_t set, oldset;
	sigfillset(&set);
	(void) sigprocmask(SIG_BLOCK, &set, &oldset);

	/* XXX */
	/* mode_t prev_umask = umask(S_IRWXG | S_IRWXO); */

	fd = mkstemp(fullname);

	/* XXX */
	/* umask(prev_umask); */

	if (fd < 0) {
		ERR("!mkstemp");
		goto err;
	}

	(void) unlink(fullname);
	(void) sigprocmask(SIG_SETMASK, &oldset, NULL);
	LOG(3, "unlinked file is \"%s\"", fullname);

#else

	UINT fileNo;
	FILE_DISPOSITION_INFO fileDisp;

	fileNo = GetTempFileNameA(dir, "umem", 0, (LPSTR)&fullName[0]);

	if (fileNo == 0) {
		ERR("!mkstemp");
		goto err;
	}

	fd = util_file_create((const char *)&fullName, size, size);

	if (fd == INVALID_HANDLE_VALUE) {
		ERR("!mkstemp");
		goto err;
	}

	fileDisp.DeleteFile = TRUE;

	if (!SetFileInformationByHandle((HANDLE) fd,
				FileDispositionInfo,
				&fileDisp,
				sizeof (fileDisp))) {

		/*
		 * let's preserve the last error across the close and delete
		 */
		int oerrno = GetLastError();

		CloseHandle((HANDLE) fd);
		DeleteFileA((LPCSTR) &fullName);
		fd = INVALID_HANDLE_VALUE;
		SetLastError(oerrno);

		ERR("!mkstemp");
		goto err;
	}

#endif

	return fd;

err:
	oerrno = errno;
	(void) sigprocmask(SIG_SETMASK, &oldset, NULL);
	if (fd != -1)
		(void) close(fd);
	errno = oerrno;
	return -1;
}

/*
 * util_get_arch_flags -- get architecture identification flags
 */
int
util_get_arch_flags(struct arch_flags *arch_flags)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);

	arch_flags->e_machine = si.wProcessorArchitecture;
	arch_flags->ei_class = 0; /* XXX - si.dwProcessorType */
	arch_flags->ei_data = 0;
	arch_flags->alignment_desc = alignment_desc();

	return 0;
}
