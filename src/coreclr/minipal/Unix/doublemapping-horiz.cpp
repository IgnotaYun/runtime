// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Double mapped executable memory for horiz, backed by codeslab_span.
//
// CoreCLR's ExecutableAllocator requires a single contiguous RX/RW virtual
// address span for relative branches and offset arithmetic, while Horizon OS
// provides only a tiny per-process slab pool (~10) of KCodeMemory objects.
// codeslab_span reserves the full contiguous RX and RW spans up front and backs
// fixed ChunkSize slices lazily via Horizon CodeMemory SVCs (or memfd on the
// Linux TARGET_HORIZ_MIRROR build).

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <codeslab/codeslab.h>
#include "minipal.h"
#include "horiz_host.h"

namespace
{
    const size_t ChunkSize = 64 * 1024 * 1024;
    const size_t DefaultReserveSize = 512 * 1024 * 1024;

    using DoubleMapper = codeslab_span;
    DoubleMapper* g_mapper;

    size_t GetReserveSize()
    {
        size_t size = DefaultReserveSize;

        const char* value = getenv("DOTNET_HorizCodeArenaReserve");
        if (value != nullptr && value[0] != '\0')
        {
            char* end;
            unsigned long long parsed = strtoull(value, &end, 16);
            if (*end == '\0' && parsed != 0 && parsed <= SIZE_MAX - ChunkSize)
                size = static_cast<size_t>(parsed);
        }

        return (size + ChunkSize - 1) & ~(ChunkSize - 1);
    }

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

    bool GetOffsetInView(const DoubleMapper* mapper, const uint8_t* base, const void* address, size_t size, size_t* offset)
    {
        size_t mapperSize = codeslab_span_size(mapper);
        uintptr_t value = reinterpret_cast<uintptr_t>(address);
        uintptr_t start = reinterpret_cast<uintptr_t>(base);
        if (value < start || size > mapperSize || value - start > mapperSize - size)
            return false;

        *offset = value - start;
        return true;
    }

    bool IsValidRange(DoubleMapper* mapper, void* pStart, size_t offset, size_t size)
    {
        if (mapper == nullptr)
            return false;

        size_t mapperSize = codeslab_span_size(mapper);
        uint8_t* rx = static_cast<uint8_t*>(codeslab_span_rx_base(mapper));
        return offset <= mapperSize && size <= mapperSize - offset && pStart == rx + offset;
    }
}

bool VMToOSInterface::CreateDoubleMemoryMapper(void** pHandle, size_t* pMaxExecutableCodeSize)
{
    codeslab_span_config cfg{};
    cfg.name = "code arena";
    cfg.reserve_size = GetReserveSize();
    cfg.chunk_size = ChunkSize;

    DoubleMapper* mapper = codeslab_span_create(&cfg);
    if (mapper == nullptr)
        return false;

    g_mapper = mapper;
    *pHandle = mapper;
    *pMaxExecutableCodeSize = codeslab_span_size(mapper);
    return true;
}

void VMToOSInterface::DestroyDoubleMemoryMapper(void* mapperHandle)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (mapper == nullptr)
        return;

    if (g_mapper == mapper)
        g_mapper = nullptr;

    codeslab_span_destroy(mapper);
}

void* VMToOSInterface::ReserveDoubleMappedMemory(void* mapperHandle, size_t offset, size_t size, const void* rangeStart, const void* rangeEnd)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (mapper == nullptr)
        return nullptr;

    size_t mapperSize = codeslab_span_size(mapper);
    if (offset > mapperSize || size > mapperSize - offset)
        return nullptr;

    uint8_t* rx = static_cast<uint8_t*>(codeslab_span_rx_base(mapper));
    void* result = rx + offset;
    if (!IsInRange(result, size, rangeStart, rangeEnd))
        return nullptr;

    return result;
}

void* VMToOSInterface::CommitDoubleMappedMemory(void* pStart, size_t size, bool isExecutable)
{
    (void)isExecutable;

    DoubleMapper* mapper = g_mapper;
    size_t offset;
    if (mapper == nullptr || !GetOffsetInView(mapper, static_cast<uint8_t*>(codeslab_span_rx_base(mapper)), pStart, size, &offset))
        return nullptr;

    return codeslab_span_commit(mapper, offset, size) == 0 ? pStart : nullptr;
}

bool VMToOSInterface::ReleaseDoubleMappedMemory(void* mapperHandle, void* pStart, size_t offset, size_t size)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (!IsValidRange(mapper, pStart, offset, size))
        return false;

    return codeslab_span_zero_backed(mapper, offset, size) == 0;
}

void* VMToOSInterface::GetRWMapping(void* mapperHandle, void* pStart, size_t offset, size_t size)
{
    DoubleMapper* mapper = static_cast<DoubleMapper*>(mapperHandle);
    if (!IsValidRange(mapper, pStart, offset, size))
        return nullptr;

    return static_cast<uint8_t*>(codeslab_span_rw_base(mapper)) + offset;
}

bool VMToOSInterface::ReleaseRWMapping(void* pStart, size_t size)
{
    DoubleMapper* mapper = g_mapper;
    size_t offset;
    return mapper != nullptr && GetOffsetInView(mapper, static_cast<uint8_t*>(codeslab_span_rw_base(mapper)), pStart, size, &offset);
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
    return g_mapper != nullptr && codeslab_span_write_alias(g_mapper, address, writable, available) == 0;
}

bool minipal_horiz_flush_instruction_cache(void* address, size_t size)
{
    return g_mapper != nullptr && codeslab_span_flush(g_mapper, address, size) == 0;
}

bool minipal_horiz_get_code_arena_info(size_t* reserved, size_t* backed, size_t* chunks)
{
    codeslab_span_stats stats;
    if (g_mapper == nullptr || codeslab_span_get_stats(g_mapper, &stats) != 0)
        return false;

    if (reserved != nullptr)
        *reserved = stats.reserved_bytes;
    if (backed != nullptr)
        *backed = stats.backed_bytes;
    if (chunks != nullptr)
        *chunks = stats.backed_chunks;
    return true;
}
