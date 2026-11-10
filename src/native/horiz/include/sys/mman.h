// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// newlib has no <sys/mman.h>. The PAL implements anonymous mappings on top of
// a virtual address range that the application reserves at startup.

#ifndef HORIZ_SYS_MMAN_H
#define HORIZ_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void*)-1)

#define MADV_DONTNEED 4
#define MADV_FREE     8

#define POSIX_MADV_DONTNEED 4

// No mapping is backed by a file, so msync has nothing to write back.
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   madvise(void* addr, size_t length, int advice);
int   posix_madvise(void* addr, size_t length, int advice);
int   msync(void* addr, size_t length, int flags);

#ifdef __cplusplus
}
#endif

#endif // HORIZ_SYS_MMAN_H
