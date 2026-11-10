// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Debugging APIs for horiz (debug/debug.cpp on other Unix systems).
//
// Horizon has no ptrace and no signals: other threads' contexts cannot be read,
// and memory cannot be probed without faulting.

#include "pal/dbgmsg.h"
#include "pal/palinternal.h"
#include "pal/debug.h"

#include <string.h>

// libnx/libounce headers only get C linkage through <switch.h>.
extern "C" {
#include <switch/kernel/svc.h>
}

#include "horiz_host.h"

SET_DEFAULT_DEBUG_CHANNEL(DEBUG); // some headers have code with asserts, so do this first

extern "C" void DBG_DebugBreak_End();

BOOL
PALAPI
FlushInstructionCache(
    IN HANDLE hProcess,
    IN LPCVOID lpBaseAddress,
    IN SIZE_T dwSize)
{
    if (lpBaseAddress == NULL)
        return TRUE;

    // Code is written through the writable alias of the code arena, so the data
    // cache has to be cleaned at that address.
    if (!minipal_horiz_flush_instruction_cache(const_cast<void*>(lpBaseAddress), dwSize))
    {
        __builtin___clear_cache((char*)lpBaseAddress, (char*)lpBaseAddress + dwSize);
    }

    return TRUE;
}

VOID
PALAPI
OutputDebugStringA(
    IN LPCSTR lpOutputString)
{
    if (lpOutputString != NULL)
    {
        svcOutputDebugString(lpOutputString, strlen(lpOutputString));
    }
}

VOID
PALAPI
OutputDebugStringW(
    IN LPCWSTR lpOutputString)
{
    if (lpOutputString == NULL)
        return;

    int length = WideCharToMultiByte(CP_UTF8, 0, lpOutputString, -1, NULL, 0, NULL, NULL);
    if (length <= 0)
        return;

    char* buffer = (char*)malloc(length);
    if (buffer == NULL)
        return;

    if (WideCharToMultiByte(CP_UTF8, 0, lpOutputString, -1, buffer, length, NULL, NULL) > 0)
    {
        svcOutputDebugString(buffer, strlen(buffer));
    }

    free(buffer);
}

VOID
PALAPI
DebugBreak(
    VOID)
{
    DBG_DebugBreak();
}

BOOL
IsInDebugBreak(void *addr)
{
    return (addr >= (void *)DBG_DebugBreak) && (addr <= (void *)DBG_DebugBreak_End);
}

BOOL
PALAPI
GetThreadContext(
    IN HANDLE hThread,
    IN OUT LPCONTEXT lpContext)
{
    ASSERT("GetThreadContext is not supported on horiz\n");
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL
PALAPI
PAL_ProbeMemory(
    PVOID pBuffer,
    DWORD cbBuffer,
    BOOL fWriteAccess)
{
    // There is no way to detect an invalid access without taking it.
    return TRUE;
}
