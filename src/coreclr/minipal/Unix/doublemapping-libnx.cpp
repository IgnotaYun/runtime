// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx executable memory mapper.
//
// Horizon CodeMemory exposes simultaneous writable and executable aliases at
// different virtual addresses. CoreCLR's ExecutableAllocator already models
// exactly that contract: Reserve returns an RX address and MapRW translates an
// RX subrange to a writable alias. Keep one large arena because the kernel has
// a small CodeMemory-object limit (roughly ten on current firmware).

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include "minipal.h"

#if defined(LINUXNX_MIRROR)
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
extern "C"
{
#include <switch/result.h>
#include <switch/kernel/jit.h>
#include <switch/kernel/svc.h>
#include <switch/arm/cache.h>
}
#include <stdio.h>
#endif

namespace
{
    constexpr size_t LibnxJitArenaSize = 256u * 1024u * 1024u;

    struct LibnxDoubleMapper
    {
        size_t size;
        uint8_t* rw;
        uint8_t* rx;
#if defined(LINUXNX_MIRROR)
        int fd;
#else
        Jit jit;
#endif
    };

    LibnxDoubleMapper* g_mapper;

#if defined(LINUXNX_MIRROR)
    void* LinuxMmap(void* address, size_t size, int protection, int flags, int fd, off_t offset)
    {
        return reinterpret_cast<void*>(syscall(SYS_mmap, address, size, protection, flags, fd, offset));
    }

    int LinuxMunmap(void* address, size_t size)
    {
        return static_cast<int>(syscall(SYS_munmap, address, size));
    }
#endif

    bool RangeContains(const void* start, size_t size, const void* rangeStart, const void* rangeEnd)
    {
        if (rangeStart == nullptr && rangeEnd == nullptr)
            return true;

        // ExecutableAllocator::ReserveAt expresses an exact-address request by
        // passing the same address for both bounds.
        if (rangeStart == rangeEnd)
            return start == rangeStart;

        uintptr_t value = reinterpret_cast<uintptr_t>(start);
        uintptr_t end = value + size;
        uintptr_t low = reinterpret_cast<uintptr_t>(rangeStart);
        uintptr_t high = reinterpret_cast<uintptr_t>(rangeEnd);
        return value >= low && end >= value && end <= high;
    }

    bool TranslateRxToRw(void* rx, size_t size, uint8_t** rw)
    {
        if (g_mapper == nullptr)
            return false;

        uintptr_t address = reinterpret_cast<uintptr_t>(rx);
        uintptr_t base = reinterpret_cast<uintptr_t>(g_mapper->rx);
        if (address < base || size > g_mapper->size || address - base > g_mapper->size - size)
            return false;

        *rw = g_mapper->rw + (address - base);
        return true;
    }
}

bool VMToOSInterface::CreateDoubleMemoryMapper(void** pHandle, size_t* pMaxExecutableCodeSize)
{
    LibnxDoubleMapper* mapper = new (std::nothrow) LibnxDoubleMapper{};
    if (mapper == nullptr)
        return false;

    mapper->size = LibnxJitArenaSize;
#if defined(LINUXNX_MIRROR)
    mapper->fd = static_cast<int>(syscall(SYS_memfd_create, "coreclr-libnx-jit", 0));
    if (mapper->fd < 0 || ftruncate(mapper->fd, mapper->size) != 0)
    {
        if (mapper->fd >= 0)
            close(mapper->fd);
        delete mapper;
        return false;
    }

    // The mirror intentionally links libnx's fake mmap for the rest of the
    // PAL. Bypass that symbol here: these two mappings must be real Linux
    // aliases of the same memfd, not two independent fake-mmap allocations.
    mapper->rw = static_cast<uint8_t*>(LinuxMmap(nullptr, mapper->size, PROT_READ | PROT_WRITE, MAP_SHARED, mapper->fd, 0));
    mapper->rx = static_cast<uint8_t*>(LinuxMmap(nullptr, mapper->size, PROT_READ | PROT_EXEC, MAP_SHARED, mapper->fd, 0));
    if (mapper->rw == MAP_FAILED || mapper->rx == MAP_FAILED)
    {
        if (mapper->rw != MAP_FAILED)
            LinuxMunmap(mapper->rw, mapper->size);
        if (mapper->rx != MAP_FAILED)
            LinuxMunmap(mapper->rx, mapper->size);
        close(mapper->fd);
        delete mapper;
        return false;
    }
#else
    Result rc = jitCreate(&mapper->jit, mapper->size);
    if (R_FAILED(rc) || mapper->jit.type != JitType_CodeMemory)
    {
        if (R_SUCCEEDED(rc))
            jitClose(&mapper->jit);
        delete mapper;
        return false;
    }
    mapper->rw = static_cast<uint8_t*>(jitGetRwAddr(&mapper->jit));
    mapper->rx = static_cast<uint8_t*>(jitGetRxAddr(&mapper->jit));
    if (mapper->rw == nullptr || mapper->rx == nullptr)
    {
        jitClose(&mapper->jit);
        delete mapper;
        return false;
    }

    // Anonymous mmap/memfd backing is initially zero. CodeMemory is backed by
    // caller-owned pages and does not provide that guarantee, while CoreCLR's
    // executable loader heaps require every fresh page to be zero-filled.
    // Initialize the arena once; ReleaseDoubleMappedMemory clears reused
    // ranges without destroying data on repeated Commit calls.
    memset(mapper->rw, 0, mapper->size);
    armDCacheFlush(mapper->rw, mapper->size);

    // Which virtual addresses the guest's own generated code lives in. Without
    // this an invalid-access report from a host is a bare number; with it, a
    // fault address can be placed inside (or outside) the code arenas. Once per
    // boot, so it costs nothing.
    {
        char buffer[160];
        int length = snprintf(
            buffer,
            sizeof(buffer),
            "coreclr jit arena: rw=%p rx=%p size=0x%zx\n",
            static_cast<void*>(mapper->rw),
            static_cast<void*>(mapper->rx),
            mapper->size);

        svcOutputDebugString(buffer, static_cast<size_t>(length));
    }
#endif

    g_mapper = mapper;
    *pHandle = mapper;
    *pMaxExecutableCodeSize = mapper->size;
    return true;
}

void VMToOSInterface::DestroyDoubleMemoryMapper(void* mapperHandle)
{
    LibnxDoubleMapper* mapper = static_cast<LibnxDoubleMapper*>(mapperHandle);
    if (mapper == nullptr)
        return;

#if defined(LINUXNX_MIRROR)
    LinuxMunmap(mapper->rw, mapper->size);
    LinuxMunmap(mapper->rx, mapper->size);
    close(mapper->fd);
#else
    jitClose(&mapper->jit);
#endif
    if (g_mapper == mapper)
        g_mapper = nullptr;
    delete mapper;
}

void* VMToOSInterface::ReserveDoubleMappedMemory(
    void* mapperHandle,
    size_t offset,
    size_t size,
    const void* rangeStart,
    const void* rangeEnd)
{
    LibnxDoubleMapper* mapper = static_cast<LibnxDoubleMapper*>(mapperHandle);
    if (mapper == nullptr || offset > mapper->size || size > mapper->size - offset)
        return nullptr;

    void* result = mapper->rx + offset;
    return RangeContains(result, size, rangeStart, rangeEnd) ? result : nullptr;
}

void* VMToOSInterface::CommitDoubleMappedMemory(void* pStart, size_t size, bool isExecutable)
{
    (void)isExecutable;
    uint8_t* rw;
    return TranslateRxToRw(pStart, size, &rw) ? pStart : nullptr;
}

bool VMToOSInterface::ReleaseDoubleMappedMemory(
    void* mapperHandle,
    void* pStart,
    size_t offset,
    size_t size)
{
    LibnxDoubleMapper* mapper = static_cast<LibnxDoubleMapper*>(mapperHandle);
    if (mapper == nullptr || pStart != mapper->rx + offset || offset > mapper->size || size > mapper->size - offset)
        return false;

    memset(mapper->rw + offset, 0, size);
#if !defined(LINUXNX_MIRROR)
    armDCacheFlush(mapper->rw + offset, size);
    armICacheInvalidate(mapper->rx + offset, size);
#endif
    return true;
}

void* VMToOSInterface::GetRWMapping(
    void* mapperHandle,
    void* pStart,
    size_t offset,
    size_t size)
{
    LibnxDoubleMapper* mapper = static_cast<LibnxDoubleMapper*>(mapperHandle);
    if (mapper == nullptr || pStart != mapper->rx + offset || offset > mapper->size || size > mapper->size - offset)
        return nullptr;
    return mapper->rw + offset;
}

bool VMToOSInterface::ReleaseRWMapping(void* pStart, size_t size)
{
    if (g_mapper == nullptr)
        return false;
    uintptr_t address = reinterpret_cast<uintptr_t>(pStart);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_mapper->rw);
    return address >= base && size <= g_mapper->size && address - base <= g_mapper->size - size;
}

bool VMToOSInterface::AllocateThunksFromTemplateRespectsStartAddress()
{
    return false;
}

void* VMToOSInterface::CreateTemplate(
    void* pImageTemplate,
    size_t templateSize,
    void (*codePageGenerator)(uint8_t* pageBase, uint8_t* pageBaseRX, size_t size))
{
    (void)pImageTemplate;
    (void)templateSize;
    (void)codePageGenerator;
    return nullptr;
}

void* VMToOSInterface::AllocateThunksFromTemplate(
    void* pTemplate,
    size_t templateSize,
    void* pStart,
    void (*dataPageGenerator)(uint8_t* pageBase, size_t size))
{
    (void)pTemplate;
    (void)templateSize;
    (void)pStart;
    (void)dataPageGenerator;
    return nullptr;
}

bool VMToOSInterface::FreeThunksFromTemplate(void* thunks, size_t templateSize)
{
    (void)thunks;
    (void)templateSize;
    return false;
}

// Native detour consumers must patch through the owned RW alias, never mprotect
// the RX view. Return only the remainder of this arena; callers validate size.
extern "C" __attribute__((visibility("default"))) bool minipal_libnx_get_write_alias(void* address, void** writable, size_t* available)
{
    if (g_mapper == nullptr || writable == nullptr || available == nullptr)
        return false;
    uintptr_t value = reinterpret_cast<uintptr_t>(address);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_mapper->rx);
    if (value >= base && value - base < g_mapper->size)
    {
        *writable = g_mapper->rw + (value - base);
        *available = g_mapper->size - (value - base);
        return true;
    }
    base = reinterpret_cast<uintptr_t>(g_mapper->rw);
    if (value >= base && value - base < g_mapper->size)
    {
        *writable = address;
        *available = g_mapper->size - (value - base);
        return true;
    }
    return false;
}

extern "C" __attribute__((visibility("default"))) bool minipal_libnx_flush_instruction_cache(void* rx, size_t size)
{
    uint8_t* rw;
    if (!TranslateRxToRw(rx, size, &rw))
    {
        if (g_mapper == nullptr)
            return false;
        uintptr_t value = reinterpret_cast<uintptr_t>(rx);
        uintptr_t base = reinterpret_cast<uintptr_t>(g_mapper->rw);
        if (value < base || size > g_mapper->size || value - base > g_mapper->size - size)
            return false;
        rw = static_cast<uint8_t*>(rx);
        rx = g_mapper->rx + (value - base);
    }

#if defined(LINUXNX_MIRROR)
    __builtin___clear_cache(reinterpret_cast<char*>(rw), reinterpret_cast<char*>(rw + size));
    __builtin___clear_cache(reinterpret_cast<char*>(rx), reinterpret_cast<char*>(static_cast<uint8_t*>(rx) + size));
#else
    armDCacheFlush(rw, size);
    armICacheInvalidate(rx, size);
#endif
    return true;
}
