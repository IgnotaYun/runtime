
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
#include <mono/utils/mono-os-mutex.h>
#include <mono/utils/mono-logger-internals.h>

// Custom mmap-like impl, similar to mono-wasm-pagemgr.c but uses a single pre-allocated buffer
#define MMAP_PAGE_SIZE (64 * 1024)

// These symbols are exported to be called by the host application. Currently the heap memory is stolen from the newlib allocator.
void mono_nx_fakemmap_release(void);
void mono_nx_fakemmap_init(intptr_t memory_start, intptr_t memory_end);

// 1-bit state per page, 0 = free, 1 = allocated
static u8* page_table;
static intptr_t heap_start = 0;
static size_t total_pages = 0;
static size_t total_memory = 0;
static pthread_mutex_t page_table_lock = PTHREAD_MUTEX_INITIALIZER;

void mono_nx_fakemmap_release(void) {
	if (heap_start) {
		page_table = NULL;
		heap_start = 0;
		total_pages = 0;
		total_memory = 0;
	}
}

void mono_nx_fakemmap_init(intptr_t memory_start, intptr_t memory_end)
{
	mono_nx_fakemmap_release();

	g_assert(memory_start);
	g_assert(memory_end);
	g_assert(memory_end > memory_start);

	// This function is meant to be called very early during startup and heap may not be ready.
	// Take the memory we need for the page table from the end of the heap itself.
	// We must not take it from the top because the top is aligned to the correct max alignment that is required by mono.
	// First, approximate how big is the page table.
	size_t tmp_heap_size = memory_end - memory_start;
	size_t tmp_pages = tmp_heap_size / MMAP_PAGE_SIZE + 1; // Round up to page size
	g_assert(tmp_pages > 0);

	size_t page_table_size = (tmp_pages + 7) / 8; // 1 bit per page, round up to nearest byte
	page_table = (u8*)(memory_end - page_table_size);
	memset(page_table, 0, page_table_size);

	memory_end -= page_table_size;
	heap_start = memory_start;

	// The actual amount of pages will be smaller, calculate it here
	total_memory = (memory_end - memory_start) / MMAP_PAGE_SIZE * MMAP_PAGE_SIZE; // Round down to page size
	total_pages = total_memory / MMAP_PAGE_SIZE;
	g_assert(total_pages > 0);

	// Assert that our math is correct and we allocated enough space for the page table
	size_t real_page_table_size = (total_pages + 7) / 8; 
	g_assert(real_page_table_size <= page_table_size);

	// Mono trace is not initialized yet at this point.
	//g_printf("Fake heap initialized with %zu bytes @ %p (%zu total pages, %zu MB page table size)", memory_size, (void*)heap_start, total_pages, page_table_size / 1024 / 1024);
}

static inline int is_page_free(int page_index) {
	return (page_table[page_index / 8] & (1 << (page_index % 8))) == 0;
}

static inline void set_page_state(int page_index, int used)
{
	if (used) {
		g_assert(is_page_free(page_index));
		page_table[page_index / 8] |= (1 << (page_index % 8));
	} else {
		g_assert(!is_page_free(page_index));
		page_table[page_index / 8] &= ~(1 << (page_index % 8));
	}
}

static inline void set_page_group_state(int start_page_index, int num_pages, int used)
{
	while (start_page_index % 8 != 0 && num_pages > 0) {
		set_page_state(start_page_index, used);
		start_page_index++;
		num_pages--;
	}

	while (num_pages >= 8) {
		if (used) {
			g_assert(page_table[start_page_index / 8] == 0x00);
			page_table[start_page_index / 8] = 0xFF;
		} else {
			g_assert(page_table[start_page_index / 8] == 0xff);
			page_table[start_page_index / 8] = 0x00;
		}

		start_page_index += 8;
		num_pages -= 8;
	}

	while (num_pages > 0) {
		set_page_state(start_page_index, used);
		start_page_index++;
		num_pages--;
	}
}

static inline int find_first_page_of(int start_page, int max_page, int state)
{
	if (start_page >= total_pages) return -1;
	if (max_page > total_pages) max_page = total_pages;

	while (start_page < max_page) {
		int page_free = is_page_free(start_page);

		if (state && !page_free) {
			return start_page;
		} else if (!state && page_free) {
			return start_page;
		}
		
		start_page++;
	}

	return -1;
}

static inline int count_consecutive_pages(int start_page_index, int max_pages, int* state) {
	int count = 0;

	if (max_pages == 0) return count;
	if (start_page_index >= total_pages) return count;
	if (start_page_index + max_pages > total_pages) max_pages = total_pages - start_page_index;

	int initial_state = is_page_free(start_page_index) ? 0 : 1;
	if (state) *state = initial_state;

	while (start_page_index % 8 != 0 && max_pages > 0) {
		if (is_page_free(start_page_index) != (initial_state == 0)) 
			return count;

		count++;
		start_page_index++;
		max_pages--;
	}

	while (max_pages >= 8) {
		u8 byte = page_table[start_page_index / 8];
		if (byte != (initial_state ? 0xFF : 0x00)) {
			// Do not return here, we must count how many bits in this byte match the initial state
			break;
		}

		count += 8;
		start_page_index += 8;
		max_pages -= 8;
	}

	while (max_pages > 0) {
		if (is_page_free(start_page_index) != (initial_state == 0)) 
			return count;

		count++;
		start_page_index++;
		max_pages--;
	}

	return count;
}

static void free_range(void* start, size_t length) {
	uintptr_t addr = (uintptr_t)start;
	uintptr_t end = addr + length;

	g_assert(addr >= (uintptr_t)heap_start);
	g_assert(end <= (uintptr_t)heap_start + total_memory);

	size_t start_page = (addr - (uintptr_t)heap_start) / MMAP_PAGE_SIZE;
	size_t page_count = (length + MMAP_PAGE_SIZE - 1) / MMAP_PAGE_SIZE;

	set_page_group_state(start_page, page_count, 0);
}

static int next_aligned_index(int start_index, int alignment) {
	if (alignment == 0) return start_index;
	int remainder = start_index % alignment;
	if (remainder == 0) return start_index;
	return start_index + (alignment - remainder);
}

static void* alloc_range(size_t length, int page_index_align) {
	if (length == 0) return NULL;

	size_t pages_needed = (length + MMAP_PAGE_SIZE - 1) / MMAP_PAGE_SIZE;
	int first_free = find_first_page_of(next_aligned_index(0, page_index_align), total_pages, 0);
	while (first_free >= 0) 
	{
		if (page_index_align == 0 || first_free % page_index_align == 0) 
		{
			int consecutive_pages = count_consecutive_pages(first_free, pages_needed, NULL);

			if (consecutive_pages >= pages_needed) {
				set_page_group_state(first_free, pages_needed, 1);
				return (void*)(heap_start + first_free * MMAP_PAGE_SIZE);
			}

			first_free = find_first_page_of(first_free + consecutive_pages, total_pages, 0);
		}
		else
		{
			first_free = find_first_page_of(next_aligned_index(first_free + 1, page_index_align), total_pages, 0);
		}
	}
	
	mono_trace (G_LOG_LEVEL_ERROR, MONO_TRACE_GC, "Fake heap Failed to allocate %zu bytes, not enough contiguous free pages", length);
	return NULL; 
}

#define BEGIN_CRITICAL_SECTION do { \
	pthread_mutex_lock(&page_table_lock); \
	MonoThreadInfo *__info = mono_thread_info_current_unchecked (); \
	if (__info) __info->inside_critical_region = TRUE;	\

#define END_CRITICAL_SECTION \
	if (__info) __info->inside_critical_region = FALSE;	\
	pthread_mutex_unlock(&page_table_lock); \
} while (0)	\

int
mono_pagesize (void)
{
	// This is also used for GC stack alignment, it's important we don't return the wrong value.
	return 0x1000;
}

int
mono_valloc_granule (void)
{
	return MMAP_PAGE_SIZE;
}

void*
mono_valloc (void *addr, size_t length, int flags, MonoMemAccountType type)
{
	if ((flags & MONO_MMAP_FIXED) && addr)
		return NULL;
	
	void* ptr = NULL;
	BEGIN_CRITICAL_SECTION;
	ptr = alloc_range (length, 0);
	END_CRITICAL_SECTION;

	if (!ptr) return NULL;

	memset (ptr, 0, length);
	mono_account_mem (type, (ssize_t)length);

	return ptr;		
}

void*
mono_valloc_aligned (size_t size, size_t alignment, int flags, MonoMemAccountType type)
{	
	if (alignment & (alignment - 1))
		g_error ("mono_valloc_aligned: alignment %zu is not a power of two", alignment);

	int page_alignment = 0;
	if (alignment > MMAP_PAGE_SIZE)
	{
		if (alignment % MMAP_PAGE_SIZE != 0)
			g_error ("mono_valloc_aligned: alignment %zu is not a multiple of page size %d", alignment, MMAP_PAGE_SIZE);

		page_alignment = alignment / MMAP_PAGE_SIZE;
	}

	// In case alignment is less use alignment = 0, which means single page-aligned allocation

	void* ptr = NULL;
	BEGIN_CRITICAL_SECTION;
	ptr = alloc_range (size, page_alignment);
	END_CRITICAL_SECTION;

	if (!ptr) return NULL;

	// Check the pointer is actually aligned
	if (((uintptr_t)ptr % alignment) != 0)
		g_error ("mono_valloc_aligned: returned pointer %p is not aligned to %zx", ptr, alignment);

	memset (ptr, 0, size);
	mono_account_mem (type, (ssize_t)size);

	return ptr;
}

int
mono_vfree (void *addr, size_t length, MonoMemAccountType type)
{
	BEGIN_CRITICAL_SECTION;
	free_range (addr, length);
	END_CRITICAL_SECTION;
	mono_account_mem (type, -(ssize_t)length);
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
