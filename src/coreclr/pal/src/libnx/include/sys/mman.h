// libnx mmap shim: newlib for Switch has no <sys/mman.h>. The PAL expects
// POSIX mmap semantics; provided by nx_mmap.c over a pre-reserved VA region
// (the host application initializes it via nx_fakemmap_init at startup).
#ifndef LIBNX_SYS_MMAN_H
#define LIBNX_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#if defined(__linux__)
// linuxnx profiling mirror: use the real glibc declarations; nx_mmap.c still
// provides the (fake-mmap) definitions so we profile the same code paths.
#include_next <sys/mman.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FIXED     0x10

#define MAP_FAILED ((void*)-1)

#define MADV_DONTNEED 4
#define MADV_FREE     8

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   madvise(void* addr, size_t length, int advice);

#define POSIX_MADV_DONTNEED 4
int   posix_madvise(void* addr, size_t length, int advice);

#ifdef __cplusplus
}
#endif

#endif // __linux__

#ifdef __cplusplus
extern "C" {
#endif

// Host-side initialization: reserves [memory_start, memory_end) for fake-mmap
// allocations. Must be called before coreclr_initialize.
void nx_fakemmap_init(size_t memory_start, size_t memory_end);
// Current bytes allocated through the fake mmap (for diagnostics).
size_t nx_fakemmap_used(void);

#ifdef __cplusplus
}
#endif

#endif // LIBNX_SYS_MMAN_H

// Used by the PAL's mmap wrapper on Apple; meaningless on Horizon.
#ifndef PROT_CHECK
#define PROT_CHECK 0
#endif
