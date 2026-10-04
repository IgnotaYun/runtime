// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx (Horizon OS) does not support creating two virtual mappings of the
// same physical memory, so W^X double mapping is unavailable. All of these
// entry points report failure, which makes the runtime fall back to plain
// RWX mappings (our nx_mmap shim ignores W^X restrictions).

#include <stddef.h>
#include <stdint.h>
#include "minipal.h"

bool VMToOSInterface::CreateDoubleMemoryMapper(void** pHandle, size_t *pMaxExecutableCodeSize)
{
    *pHandle = NULL;
    *pMaxExecutableCodeSize = 0;
    return false;
}

void VMToOSInterface::DestroyDoubleMemoryMapper(void *mapperHandle)
{
}

void* VMToOSInterface::ReserveDoubleMappedMemory(void *mapperHandle, size_t offset, size_t size, const void *rangeStart, const void* rangeEnd)
{
    return NULL;
}

void *VMToOSInterface::CommitDoubleMappedMemory(void* pStart, size_t size, bool isExecutable)
{
    return NULL;
}

bool VMToOSInterface::ReleaseDoubleMappedMemory(void *mapperHandle, void* pStart, size_t offset, size_t size)
{
    return false;
}

void* VMToOSInterface::GetRWMapping(void *mapperHandle, void* pStart, size_t offset, size_t size)
{
    return NULL;
}

bool VMToOSInterface::ReleaseRWMapping(void* pStart, size_t size)
{
    return false;
}

bool VMToOSInterface::AllocateThunksFromTemplateRespectsStartAddress()
{
    return false;
}

void* VMToOSInterface::CreateTemplate(void* pImageTemplate, size_t templateSize, void (*codePageGenerator)(uint8_t* pageBase, uint8_t* pageBaseRX, size_t size))
{
    return NULL;
}

void* VMToOSInterface::AllocateThunksFromTemplate(void* pTemplate, size_t templateSize, void* pStart, void (*dataPageGenerator)(uint8_t* pageBase, size_t size))
{
    return NULL;
}

bool VMToOSInterface::FreeThunksFromTemplate(void* thunks, size_t templateSize)
{
    return false;
}
