// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Double mapped executable memory for horiz.
//
// Horizon has no mmap, but a CodeMemory object (libnx/libounce jitCreate) maps the same
// memory at a writable and at an executable address, which is the double
// mapping the executable allocator works with. The kernel allows only a few
// CodeMemory objects per process, so a single arena is created up front and
// reservations are carved out of it at the offsets the allocator chooses.
//
// The horiz mirror maps a memfd twice instead. It uses the system calls
// directly because mmap is horiz's fake mmap in that build.

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include "minipal.h"
#include "horiz_host.h"

#ifdef TARGET_HORIZ_MIRROR
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
// libnx/libounce headers only get C linkage through <switch.h>.
extern "C"
{
#include <switch/result.h>
#include <switch/kernel/jit.h>
#include <switch/arm/cache.h>
}
#endif

namespace
{
    const size_t ArenaSize = 128 * 1024 * 1024;

    struct DoubleMapper
    {
        size_t size;
        uint8_t* rw;
        uint8_t* rx;
#ifdef TARGET_HORIZ_MIRROR
        int fd;
#else
        Jit jit;
#endif
    };

    DoubleMapper* g_mapper;

#ifdef TARGET_HORIZ_MIRROR
    void* SystemMmap(void* address, size_t size, int protection, int flags, int fd, off_t offset)
    {
        return reinterpret_cast<void*>(syscall(SYS_mmap, address, size, protection, flags, fd, offset));
    }

    void SystemMunmap(void* address, size_t size)
    {
        syscall(SYS_munmap, address, size);
    }
#endif

    bool IsInRange(const void* start, size_t size, const void* rangeStart, const void* rangeEnd)
    {
        if (rangeStart == nullptr && rangeEnd == nullptr)
            return true;

        // A request for an exact address passes it as both bounds
        if (rangeStart == rangeEnd)
            return start == rangeStart;

        uintptr_t value = reinterpret_cast<uintptr_t>(start);
        uintptr_t end = value + size;
        return value >= reinterpret_cast<uintptr_t>(rangeStart) && end >= value && end <= reinterpret_cast<uintptr_t>(rangeEnd);
    }

    // Offset of [address, address + size) in the view at base, if it is inside it
    bool GetOffsetInView(const uint8_t* base, const void* address, size_t size, size_t* offset)
    {
        if (g_mapper == nullptr)
            return false;

        uintptr_t value = reinterpret_cast<uintptr_t>(address);
        uintptr_t start = reinterpret_cast<uintptr_t>(base);
        if (value < start || size > g_mapper->size || value - start > g_mapper->size - size)
            return false;

        *offset = value - start;
        return true;
    }

    bool IsValidRange(DoubleMapper* mapper, void* pStart, size_t offset, size_t size)
    {
        return mapper != nullptr && offset <= mapper->size && size <= mapper->size - offset && pStart == mapper->rx + offset;
    }

    void FlushInstructionCache(uint8_t* rw, uint8_t* rx, size_t size)
    {
#ifdef TARGET_HORIZ_MIRROR
        (void)rw;
        __builtin___clear_cache(reinterpret_cast<char*>(rx), reinterpret_cast<char*>(rx + size));
#else
        armDCacheFlush(rw, size);
        armICacheInvalidate(rx, size);
#endif
    }
}

bool VMToOSInterface::CreateDoubleMemoryMapper(void** pHandle, size_t* pMaxExecutableCodeSize)
{
    DoubleMapper* mapper = new (std::nothrow) DoubleMapper{};
    if (mapper == nullptr)
        return false;

    mapper->size = ArenaSize;

#ifdef TARGET_HORIZ_MIRROR
    mapper->fd = static_cast<int>(syscall(SYS_memfd_create, "doublemapper", 0));
    if (mapper->fd < 0 || ftruncate(mapper->fd, mapper->size) != 0)
    {
        if (mapper->fd >= 0)
            close(mapper->fd);
        delete mapper;
        return false;
    }

    mapper->rw = static_cast<uint8_t*>(SystemMmap(nullptr, mapper->size, PROT_READ | PROT_WRITE, MAP_SHARED, mapper->fd, 0));
    mapper->rx = static_cast<uint8_t*>(SystemMmap(nullptr, mapper->size, PROT_READ | PROT_EXEC, MAP_SHARED, mapper->fd, 0));
    if (mapper->rw == MAP_FAILED || mapper->rx == MAP_FAILED)
    {
        if (mapper->rw != MAP_FAILED)
            SystemMunmap(mapper->rw, mapper->size);
        if (mapper->rx != MAP_FAILED)
            SystemMunmap(mapper->rx, mapper->size);
        close(mapper->fd);
        delete mapper;
        return false;
    }
#else
    Result rc = jitCreate(&mapper->jit, mapper->size);
    if (R_FAILED(rc))
    {
        delete mapper;
        return false;
    }

    // Older firmware falls back to JitType_SetProcessMemoryPermission, which
    // has no simultaneous writable and executable views.
    mapper->rw = static_cast<uint8_t*>(jitGetRwAddr(&mapper->jit));
    mapper->rx = static_cast<uint8_t*>(jitGetRxAddr(&mapper->jit));
    if (mapper->jit.type != JitType_CodeMemory || mapper->rw == nullptr || mapper->rx == nullptr)
    {
        jitClose(&mapper->jit);
        delete mapper;
        return false;
    }

    // Unlike anonymous memory, CodeMemory is not zero filled, and the
    // executable allocator expects fresh memory to be.
    memset(mapper->rw, 0, mapper->size);
    FlushInstructionCache(mapper->rw, mapper->rx, mapper->size);
#endif

    g_mapper = mapper;
    *pHandle = mapper;
    *pMaxExecutableCodeSize = mapper->size;
    return true;
}

void VMToOSInterface::DestroyDoubleMemoryMapper(void* mapperHandle)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (mapper == nullptr)
        return;

#ifdef TARGET_HORIZ_MIRROR
    SystemMunmap(mapper->rw, mapper->size);
    SystemMunmap(mapper->rx, mapper->size);
    close(mapper->fd);
#else
    jitClose(&mapper->jit);
#endif

    if (g_mapper == mapper)
        g_mapper = nullptr;

    delete mapper;
}

void* VMToOSInterface::ReserveDoubleMappedMemory(void* mapperHandle, size_t offset, size_t size, const void* rangeStart, const void* rangeEnd)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (mapper == nullptr || offset > mapper->size || size > mapper->size - offset)
        return nullptr;

    void* result = mapper->rx + offset;
    if (!IsInRange(result, size, rangeStart, rangeEnd))
        return nullptr;

    return result;
}

void* VMToOSInterface::CommitDoubleMappedMemory(void* pStart, size_t size, bool isExecutable)
{
    // The arena is always backed
    (void)isExecutable;

    size_t offset;
    return g_mapper != nullptr && GetOffsetInView(g_mapper->rx, pStart, size, &offset) ? pStart : nullptr;
}

bool VMToOSInterface::ReleaseDoubleMappedMemory(void* mapperHandle, void* pStart, size_t offset, size_t size)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (!IsValidRange(mapper, pStart, offset, size))
        return false;

    // Released memory is reused and must be zero filled again
    memset(mapper->rw + offset, 0, size);
    FlushInstructionCache(mapper->rw + offset, mapper->rx + offset, size);
    return true;
}

void* VMToOSInterface::GetRWMapping(void* mapperHandle, void* pStart, size_t offset, size_t size)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (!IsValidRange(mapper, pStart, offset, size))
        return nullptr;

    return mapper->rw + offset;
}

bool VMToOSInterface::ReleaseRWMapping(void* pStart, size_t size)
{
    // The writable view stays mapped
    size_t offset;
    return g_mapper != nullptr && GetOffsetInView(g_mapper->rw, pStart, size, &offset);
}

bool VMToOSInterface::AllocateThunksFromTemplateRespectsStartAddress()
{
    return false;
}

void* VMToOSInterface::CreateTemplate(void* pImageTemplate, size_t templateSize, void (*codePageGenerator)(uint8_t* pageBase, uint8_t* pageBaseRX, size_t size))
{
    return nullptr;
}

void* VMToOSInterface::AllocateThunksFromTemplate(void* pTemplate, size_t templateSize, void* pStart, void (*dataPageGenerator)(uint8_t* pageBase, size_t size))
{
    return nullptr;
}

bool VMToOSInterface::FreeThunksFromTemplate(void* thunks, size_t templateSize)
{
    return false;
}

bool minipal_horiz_get_write_alias(void* address, void** writable, size_t* available)
{
    if (g_mapper == nullptr || writable == nullptr || available == nullptr)
        return false;

    size_t offset;
    if (GetOffsetInView(g_mapper->rx, address, 1, &offset) || GetOffsetInView(g_mapper->rw, address, 1, &offset))
    {
        *writable = g_mapper->rw + offset;
        *available = g_mapper->size - offset;
        return true;
    }

    return false;
}

bool minipal_horiz_flush_instruction_cache(void* address, size_t size)
{
    if (g_mapper == nullptr)
        return false;

    size_t offset;
    if (!GetOffsetInView(g_mapper->rx, address, size, &offset) && !GetOffsetInView(g_mapper->rw, address, size, &offset))
        return false;

    FlushInstructionCache(g_mapper->rw + offset, g_mapper->rx + offset, size);
    return true;
}
