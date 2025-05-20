
#include <config.h>

#ifdef HOST_LIBNX
#include <switch.h>

#include <stddef.h>
#include <errno.h>

#include "mono-mmap.h"
#include "mono-mmap-internals.h"
#include "mono-proclib.h"
#include <mono/utils/mono-threads.h>
#include <mono/utils/atomic.h>
#include <mono/utils/mono-counters.h>

void* __libnx_alloc(size_t size);
void* __libnx_aligned_alloc(size_t alignment, size_t size);
void __libnx_free(void* p);

// posix_memalign compatible function
int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (memptr == NULL) {
        return EINVAL;
    }

    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        // Alignment must be a power of two and greater than zero
        return EINVAL;
    }

    *memptr = __libnx_aligned_alloc(alignment, size);
    if (*memptr == NULL) {
        return ENOMEM;
    }

    return 0;
}

int
mono_pagesize (void)
{
	return 4096;
}

int
mono_valloc_granule (void)
{
	return mono_pagesize ();
}

void*
mono_valloc (void *addr, size_t length, int flags, MonoMemAccountType type)
{
	g_assert (addr == NULL);
	return mono_valloc_aligned (length, mono_pagesize (), flags, type);
}

void*
mono_valloc_aligned (size_t size, size_t alignment, int flags, MonoMemAccountType type)
{
	void *res = NULL;
	if (posix_memalign (&res, alignment, size))
		return NULL;

	memset (res, 0, size);
	return res;
}

#define HAVE_VALLOC_ALIGNED

int
mono_vfree (void *addr, size_t length, MonoMemAccountType type)
{
	__libnx_free (addr);
	return 0;
}

int
mono_mprotect (void *addr, size_t length, int flags)
{
	if (flags & MONO_MMAP_DISCARD) {
		memset (addr, 0, length);
	}
	return 0;
}

#endif
