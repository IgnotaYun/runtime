// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// mmap for horiz.
//
// Horizon has no mmap. The application reserves a range of memory at startup and
// hands it to horiz_fakemmap_init; mappings are carved out of it in 64 KB pages
// tracked by a bitmap that lives at the end of the range. The memory is always
// backed, so reserve and commit are the same thing and mprotect has no effect.
// Releasing the contents of a range (decommit, MADV_DONTNEED) zeroes it.
//
// Files cannot be mapped: there would be no way to share the memory with the
// file or with other mappings of it, so callers read files instead.

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "horiz_host.h"

#define HORIZ_MMAP_PAGE_SIZE (64 * 1024)

// Two bits per 64 KB page track both occupancy and allocation boundaries so
// partial prefix/suffix unmaps (such as aligned GC reservations) never free a
// live page while full unmaps still release sub-64-KB allocations cleanly.
#define PAGE_ALLOCATED 0x1u
#define PAGE_END       0x2u

static uint8_t* s_pageTable;
static uintptr_t s_heapStart;
static size_t s_totalPages;
static size_t s_usedPages;
static pthread_mutex_t s_pageTableLock = PTHREAD_MUTEX_INITIALIZER;

void horiz_fakemmap_init(size_t memoryStart, size_t memoryEnd)
{
    s_pageTable = NULL;
    s_heapStart = 0;
    s_totalPages = 0;
    s_usedPages = 0;

    if (memoryStart == 0 || memoryEnd <= memoryStart)
        return;

    uintptr_t alignedStart = (memoryStart + (HORIZ_MMAP_PAGE_SIZE - 1)) & ~(uintptr_t)(HORIZ_MMAP_PAGE_SIZE - 1);
    if (alignedStart < memoryStart || memoryEnd <= alignedStart)
        return;

    size_t span = memoryEnd - alignedStart;
    size_t maxPages = span / HORIZ_MMAP_PAGE_SIZE;
    if (maxPages == 0)
        return;

    // The page table (2 bits per 64 KB page) lives at the end of the range itself.
    size_t pageTableSize = (maxPages + 3) / 4;
    if (span <= pageTableSize || (span - pageTableSize) < HORIZ_MMAP_PAGE_SIZE)
        return;

    s_pageTable = (uint8_t*)(memoryEnd - pageTableSize);
    memset(s_pageTable, 0, pageTableSize);

    s_heapStart = alignedStart;
    s_totalPages = (span - pageTableSize) / HORIZ_MMAP_PAGE_SIZE;
}

int horiz_fakemmap_owns(const void* address)
{
    uintptr_t value = (uintptr_t)address;
    return s_heapStart != 0 && value >= s_heapStart && value < s_heapStart + s_totalPages * HORIZ_MMAP_PAGE_SIZE;
}

size_t horiz_fakemmap_size(void)
{
    return s_totalPages * HORIZ_MMAP_PAGE_SIZE;
}

size_t horiz_fakemmap_used(void)
{
    return s_usedPages * HORIZ_MMAP_PAGE_SIZE;
}

static uint8_t GetPageFlags(size_t page)
{
    return (uint8_t)((s_pageTable[page / 4] >> ((page % 4) * 2)) & 0x3u);
}

static void SetPageFlags(size_t page, uint8_t flags)
{
    size_t shift = (page % 4) * 2;
    s_pageTable[page / 4] = (uint8_t)((s_pageTable[page / 4] & ~(0x3u << shift)) | ((flags & 0x3u) << shift));
}

static bool IsPageAllocated(size_t page)
{
    return (GetPageFlags(page) & PAGE_ALLOCATED) != 0;
}

static bool IsPageEnd(size_t page)
{
    return (GetPageFlags(page) & PAGE_END) != 0;
}

static bool IsAllocationStart(size_t page)
{
    return IsPageAllocated(page) && (page == 0 || !IsPageAllocated(page - 1) || IsPageEnd(page - 1));
}

static void* AllocRange(size_t size)
{
    if (s_pageTable == NULL || size == 0)
        return NULL;

    size_t numPages = (size + HORIZ_MMAP_PAGE_SIZE - 1) / HORIZ_MMAP_PAGE_SIZE;
    if (numPages == 0 || numPages > s_totalPages)
        return NULL;

    size_t start = 0;
    size_t count = 0;

    pthread_mutex_lock(&s_pageTableLock);
    for (size_t i = 0; i < s_totalPages; i++)
    {
        if (IsPageAllocated(i))
        {
            count = 0;
            continue;
        }

        if (count == 0)
            start = i;

        if (++count == numPages)
        {
            for (size_t j = 0; j + 1 < numPages; j++)
                SetPageFlags(start + j, PAGE_ALLOCATED);
            SetPageFlags(start + numPages - 1, PAGE_ALLOCATED | PAGE_END);

            s_usedPages += numPages;
            pthread_mutex_unlock(&s_pageTableLock);
            return (void*)(s_heapStart + start * HORIZ_MMAP_PAGE_SIZE);
        }
    }
    pthread_mutex_unlock(&s_pageTableLock);
    return NULL;
}

// Releases a whole mapping, or trims complete 64 KB prefix/suffix pages from a
// mapping (such as aligned virtual reservations). Sub-page prefixes or suffixes
// leave their shared 64 KB page allocated so live memory is never reclaimed.
static int FreeRange(void* address, size_t size)
{
    if (s_pageTable == NULL || !horiz_fakemmap_owns(address) || size == 0)
        return -1;

    uintptr_t heapEnd = s_heapStart + s_totalPages * HORIZ_MMAP_PAGE_SIZE;
    if (size > heapEnd - (uintptr_t)address)
        return -1;

    uintptr_t offset = (uintptr_t)address - s_heapStart;
    size_t addrPage = offset / HORIZ_MMAP_PAGE_SIZE;

    pthread_mutex_lock(&s_pageTableLock);

    if (!IsPageAllocated(addrPage))
    {
        pthread_mutex_unlock(&s_pageTableLock);
        return -1;
    }

    size_t allocStart = addrPage;
    while (!IsAllocationStart(allocStart))
        allocStart--;

    size_t allocEnd = addrPage;
    while (allocEnd + 1 < s_totalPages && IsPageAllocated(allocEnd + 1) && !IsPageEnd(allocEnd))
        allocEnd++;

    if (!IsPageEnd(allocEnd))
    {
        pthread_mutex_unlock(&s_pageTableLock);
        return -1;
    }

    size_t allocPages = allocEnd - allocStart + 1;
    int status = -1;

    if ((offset % HORIZ_MMAP_PAGE_SIZE) == 0 && addrPage == allocStart)
    {
        size_t numPagesUp = (size + HORIZ_MMAP_PAGE_SIZE - 1) / HORIZ_MMAP_PAGE_SIZE;
        if (numPagesUp == allocPages)
        {
            for (size_t p = allocStart; p <= allocEnd; p++)
                SetPageFlags(p, 0);

            s_usedPages -= allocPages;
            status = 0;
        }
        else if (numPagesUp < allocPages)
        {
            size_t numPagesDown = size / HORIZ_MMAP_PAGE_SIZE;
            for (size_t p = allocStart; p < allocStart + numPagesDown; p++)
                SetPageFlags(p, 0);

            s_usedPages -= numPagesDown;
            status = 0;
        }
    }
    else
    {
        size_t tailEndPage = ((offset + size + HORIZ_MMAP_PAGE_SIZE - 1) / HORIZ_MMAP_PAGE_SIZE) - 1;
        if (tailEndPage == allocEnd)
        {
            size_t tailStart = (offset + HORIZ_MMAP_PAGE_SIZE - 1) / HORIZ_MMAP_PAGE_SIZE;
            if (tailStart <= allocEnd)
            {
                for (size_t p = tailStart; p <= allocEnd; p++)
                    SetPageFlags(p, 0);

                s_usedPages -= allocEnd - tailStart + 1;
                if (tailStart > allocStart)
                    SetPageFlags(tailStart - 1, PAGE_ALLOCATED | PAGE_END);
            }
            status = 0;
        }
    }

    pthread_mutex_unlock(&s_pageTableLock);
    return status;
}

// Drop the contents of a range that is already mapped, the way the kernel does
// for a fixed anonymous mapping over existing pages (the GC's decommit) and for
// MADV_DONTNEED. The pages stay allocated: the caller recommits the same range.
static int DropContents(void* address, size_t length)
{
    if (address == NULL || length == 0)
        return 0;

    if (!horiz_fakemmap_owns(address) || !horiz_fakemmap_owns((uint8_t*)address + length - 1))
        return -1;

    memset(address, 0, length);
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot;
    (void)offset;

    if (fd >= 0)
    {
        // Like a file system that does not support mapping
        errno = ENODEV;
        return MAP_FAILED;
    }

    if ((flags & MAP_FIXED) != 0)
    {
        // A fixed anonymous mapping inside the range replaces memory that is
        // already ours. Anything else cannot be placed at a fixed address.
        if ((flags & MAP_ANONYMOUS) != 0 && DropContents(addr, length) == 0)
            return addr;

        errno = ENOTSUP;
        return MAP_FAILED;
    }

    // Without MAP_FIXED the address is only a hint.
    void* ptr = AllocRange(length);
    if (ptr == NULL)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    memset(ptr, 0, length);
    return ptr;
}

int munmap(void* addr, size_t length)
{
    if (addr == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    // Whole mappings are freed immediately; prefix/suffix trims only free
    // complete 64 KB pages outside the live portion.
    FreeRange(addr, length);
    return 0;
}

int mprotect(void* addr, size_t length, int prot)
{
    (void)addr;
    (void)length;
    (void)prot;
    return 0;
}

int madvise(void* addr, size_t length, int advice)
{
    if (advice == MADV_DONTNEED || advice == MADV_FREE)
        DropContents(addr, length);

    return 0;
}

int posix_madvise(void* addr, size_t length, int advice)
{
    if (advice == POSIX_MADV_DONTNEED)
        DropContents(addr, length);

    return 0;
}

int msync(void* addr, size_t length, int flags)
{
    (void)flags;

    if (length == 0)
        return 0;

    if (!horiz_fakemmap_owns(addr) || !horiz_fakemmap_owns((uint8_t*)addr + length - 1))
    {
        errno = ENOMEM;
        return -1;
    }

    // No mapping is backed by a file, so there is nothing to write back
    return 0;
}
