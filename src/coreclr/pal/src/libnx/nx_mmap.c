// Fake mmap for libnx, ported from mono's mono-mmap-libnx.c (exelix11).
// 1-bit page state over a VA region reserved by the host at startup.
// Reserve == commit: everything is real memory; mprotect is a no-op except
// MADV-style zeroing handled in madvise.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h> // the shim header

#define NX_MMAP_PAGE_SIZE (64 * 1024)

static uint8_t* page_table;
static uintptr_t heap_start = 0;
static size_t total_pages = 0;
static size_t used_pages = 0;
static pthread_mutex_t page_table_lock = PTHREAD_MUTEX_INITIALIZER;

void nx_fakemmap_init(size_t memory_start, size_t memory_end)
{
    page_table = NULL;
    heap_start = 0;
    total_pages = 0;
    used_pages = 0;

    if (!memory_start || memory_end <= memory_start)
        return;

    // The page table lives at the end of the region itself.
    size_t tmp_heap_size = memory_end - memory_start;
    size_t tmp_pages = tmp_heap_size / NX_MMAP_PAGE_SIZE + 1;
    size_t page_table_size = (tmp_pages + 7) / 8;
    page_table = (uint8_t*)(memory_end - page_table_size);
    memset(page_table, 0, page_table_size);

    size_t total_memory = (memory_end - page_table_size) - memory_start;
    heap_start = memory_start;
    total_pages = total_memory / NX_MMAP_PAGE_SIZE;
}

size_t nx_fakemmap_used(void)
{
    return used_pages * NX_MMAP_PAGE_SIZE;
}

static int page_index(void* ptr)
{
    uintptr_t offset = (uintptr_t)ptr - heap_start;
    return (int)(offset / NX_MMAP_PAGE_SIZE);
}

static int is_page_set(int page)
{
    return (page_table[page / 8] & (1 << (page % 8))) != 0;
}

static void set_page(int page, int value)
{
    if (value)
        page_table[page / 8] |= (uint8_t)(1 << (page % 8));
    else
        page_table[page / 8] &= (uint8_t)~(1 << (page % 8));
}

static void* alloc_range(size_t size)
{
    if (!page_table || !size)
        return NULL;

    size_t num_pages = (size + NX_MMAP_PAGE_SIZE - 1) / NX_MMAP_PAGE_SIZE;
    size_t start = 0, count = 0;

    pthread_mutex_lock(&page_table_lock);
    for (size_t i = 0; i < total_pages; i++)
    {
        if (!is_page_set((int)i))
        {
            if (count == 0)
                start = i;
            count++;
            if (count == num_pages)
            {
                for (size_t j = 0; j < num_pages; j++)
                    set_page((int)(start + j), 1);
                used_pages += num_pages;
                pthread_mutex_unlock(&page_table_lock);
                return (void*)(heap_start + start * NX_MMAP_PAGE_SIZE);
            }
        }
        else
        {
            count = 0;
        }
    }
    pthread_mutex_unlock(&page_table_lock);
    return NULL;
}

static void free_range(void* addr, size_t size)
{
    if (!page_table || !addr || !size)
        return;

    size_t num_pages = (size + NX_MMAP_PAGE_SIZE - 1) / NX_MMAP_PAGE_SIZE;
    int start = page_index(addr);

    pthread_mutex_lock(&page_table_lock);
    for (size_t i = 0; i < num_pages && (size_t)(start + i) < total_pages; i++)
    {
        set_page(start + (int)i, 0);
        used_pages--;
    }
    pthread_mutex_unlock(&page_table_lock);
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot; (void)flags; (void)fd; (void)offset;

    if (addr != NULL)
    {
        // MAP_FIXED-style requests cannot be satisfied from the managed region.
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    // Page-align the length up; the allocator page-granular.
    void* ptr = alloc_range(length);
    if (!ptr)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(ptr, 0, length);
    return ptr;
}

int munmap(void* addr, size_t length)
{
    if (!addr)
    {
        errno = EINVAL;
        return -1;
    }
    free_range(addr, length);
    return 0;
}

int mprotect(void* addr, size_t length, int prot)
{
    // Memory is always RW in the fake-mmap region. PROT_EXEC is refused by
    // the platform for userland homebrew regardless.
    (void)addr; (void)length; (void)prot;
    return 0;
}

int madvise(void* addr, size_t length, int advice)
{
    (void)addr; (void)length; (void)advice;
    return 0;
}

int posix_madvise(void* addr, size_t length, int advice)
{
    (void)addr; (void)length; (void)advice;
    return 0;
}
