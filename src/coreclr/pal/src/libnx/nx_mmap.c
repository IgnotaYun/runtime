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
#include <unistd.h>

#define NX_MMAP_PAGE_SIZE (64 * 1024)

static uint8_t* page_table;
static uintptr_t heap_start = 0;
static size_t total_pages = 0;
static size_t used_pages = 0;
static pthread_mutex_t page_table_lock = PTHREAD_MUTEX_INITIALIZER;

#include <stdio.h>
#include <switch.h>

// Provided by the NRO when it is present (clrhost's io_util.c): routes these
// lines into the SD card log as well as svcOutputDebugString. Weak so the PAL
// still links into hosts that do not have it (the linuxnx mirror, the AOT and
// interpreter flavors).
__attribute__((weak)) void nx_log_line(const char *line);

static void nx_log_emit(const char *buffer)
{
    if (nx_log_line)
    {
        nx_log_line(buffer);
        return;
    }

    svcOutputDebugString(buffer, strlen(buffer));
}

void nx_fakemmap_init(size_t memory_start, size_t memory_end)
{
    {
        char buf[96];
        int n = snprintf(buf, sizeof(buf), "nxmmap: init start=%zx end=%zx\n", memory_start, memory_end);
        if (n > 0) nx_log_emit(buf);
    }
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

int nx_fakemmap_owns(const void* p)
{
    return heap_start && (size_t)p >= heap_start && (size_t)p < heap_start + total_pages * NX_MMAP_PAGE_SIZE;
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

// Unmapping a range that is not exactly a range we handed out is the classic way
// an allocator corrupts itself: the pages go back on the free list while their
// contents are still live, and the next allocation lands on top of them. glibc's
// free never does that to this shim, but newlib's memalign/aligned_alloc path and
// anything that adjusts a pointer inside a mapping can, so the callers are
// checked here instead of trusted. A rejected release is reported rather than
// silently ignored: a live log line is the only way to spot it, since the
// corruption it would cause shows up somewhere else entirely.
static unsigned bad_unmap_calls;

static void free_range(void* addr, size_t size)
{
    if (!page_table || !addr || !size)
        return;

    size_t num_pages = (size + NX_MMAP_PAGE_SIZE - 1) / NX_MMAP_PAGE_SIZE;
    int start = page_index(addr);

    pthread_mutex_lock(&page_table_lock);

    bool interior = ((uintptr_t)addr != heap_start + (uintptr_t)start * NX_MMAP_PAGE_SIZE) || (start < 0);
    bool inrange = (start >= 0) && ((size_t)start + num_pages <= total_pages);
    bool allset = inrange;

    for (int i = 0; allset && i < (int)num_pages; i++)
    {
        allset = is_page_set(start + i);
    }

    if (interior || !inrange || !allset)
    {
        if ((bad_unmap_calls++ < 8) || (bad_unmap_calls % 256) == 0)
        {
            char buf[160];
            int n = snprintf(buf, sizeof(buf),
                             "nxmmap: refusing unmap of %zu bytes at %p (%s%s%s, #%u)\n",
                             size, addr,
                             interior ? "not a mapping start" : "",
                             inrange ? "" : " outside the region",
                             allset ? "" : " pages not allocated",
                             bad_unmap_calls);
            if (n > 0) nx_log_emit(buf);
        }
        pthread_mutex_unlock(&page_table_lock);
        return;
    }

    for (size_t i = 0; i < num_pages && (size_t)(start + i) < total_pages; i++)
    {
        set_page(start + (int)i, 0);
        used_pages--;
    }
    pthread_mutex_unlock(&page_table_lock);
}

static void nxlog(const char* what, size_t a, size_t b)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "nxmmap: %s %zu %zu (used=%zuMB)\n", what, a, b, nx_fakemmap_used()/1024/1024);
    if (n > 0) nx_log_emit(buf);
}

// Ranges the kernel would have dropped and re-zeroed for us. Rate limited: the
// GC does this every time a heap segment or region is trimmed, which is far too
// often to log all of it, but a run's first few calls plus a periodic count are
// enough to see whether the path is live.
static unsigned decommit_calls;
static size_t decommit_bytes;

static void nxlog_decommit(const char* what, void* address, size_t length)
{
    decommit_calls++;
    decommit_bytes += length;

    if (decommit_calls <= 4 || (decommit_calls % 256) == 0)
    {
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "nxmmap: %s %zu bytes at %p (#%u, %zuMB total)\n",
                         what, length, address, decommit_calls, decommit_bytes / 1048576);
        if (n > 0) nx_log_emit(buf);
    }
}

// Drop the contents of a range that is already ours. This is what the kernel
// does for the two calls below, and the fake region has to answer the same way:
//
//   * gcenv.unix.cpp's GCToOSInterface::VirtualDecommit maps the range with
//     MAP_FIXED|MAP_ANON (PROT_NONE) to decommit it, and explicitly says why:
//     "GC depends on re-committed pages to be zeroed-out". The matching commit
//     is mprotect(PROT_READ|WRITE), which is a no-op here.
//   * madvise(MADV_DONTNEED / MADV_FREE) tells the kernel the contents are
//     expendable, and MADV_DONTNEED promises zeroes on the next access.
//
// The pages must stay allocated: the GC re-commits the same addresses later, so
// handing them back to the allocator on decommit would let another allocation
// take them. Reserve == commit already meant the pages are ours; all a decommit
// can mean here is that the bytes go away.
static int nx_fakemmap_drop_contents(void* address, size_t length)
{
    if (address == NULL || length == 0)
        return 0;

    if (!nx_fakemmap_owns(address) || !nx_fakemmap_owns((uint8_t*)address + length - 1))
        return -1;

    memset(address, 0, length);
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot; (void)flags;

    if (addr != NULL)
    {
        // A page-fixed anonymous mapping of a range inside the fake region is a
        // decommit/reset of memory we already own, not a new mapping: drop the
        // contents in place and report success so the caller's later commit sees
        // zeroes, which is what it is written against. Anything else fixed -
        // file-backed, or outside the region - still cannot be satisfied here.
        if ((fd < 0) && ((flags & (MAP_FIXED | MAP_ANON)) == (MAP_FIXED | MAP_ANON)) &&
            (nx_fakemmap_drop_contents(addr, length) == 0))
        {
            nxlog_decommit("decommit mmap", addr, length);
            return addr;
        }

        errno = ENOTSUP;
        return MAP_FAILED;
    }

    // Page-align the length up; the allocator page-granular.
    void* ptr = alloc_range(length);
    if (!ptr)
    {
        nxlog("FAIL mmap size", length, 0);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (length > 1024*1024)
    {
        nxlog("mmap big", length, (size_t)ptr);
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "  caller=%p\n", __builtin_return_address(0));
        if (n > 0) nx_log_emit(buf);
    }
    memset(ptr, 0, length);

    if (fd >= 0)
    {
        // File-backed mapping: read the contents in. The fake region is real
        // memory anyway, so a copy is equivalent to demand paging here.
        size_t done = 0;
        while (done < length)
        {
            ssize_t got = pread(fd, (char*)ptr + done, length - done, offset + (off_t)done);
            if (got < 0)
            {
                if (errno == EINTR)
                    continue;
                free_range(ptr, length);
                return MAP_FAILED;
            }
            if (got == 0)
                break; // EOF: rest stays zero, like a real mapping past EOF
            done += (size_t)got;
        }
    }
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
    if ((advice == MADV_DONTNEED) || (advice == MADV_FREE))
    {
        if (nx_fakemmap_drop_contents(addr, length) == 0)
        {
            nxlog_decommit("madvise drop", addr, length);
        }
    }

    return 0;
}

int posix_madvise(void* addr, size_t length, int advice)
{
    if (advice == POSIX_MADV_DONTNEED)
    {
        if (nx_fakemmap_drop_contents(addr, length) == 0)
        {
            nxlog_decommit("posix_madvise drop", addr, length);
        }
    }

    return 0;
}
