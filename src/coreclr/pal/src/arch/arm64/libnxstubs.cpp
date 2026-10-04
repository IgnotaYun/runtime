// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// libnx (Nintendo Switch homebrew) PAL stubs.
//
// Modeled on the WASI subset of arch/wasm/stubs.cpp: no POSIX signals, no
// ucontext, no dlfcn/libunwind on libnx. Context capture and virtual unwinding
// are stubbed; C++ EH (throw/catch of PAL_SEHException) works and carries
// exceptions instead. Unlike WASM this is a multithreaded arm64 target.
//

#include "pal/dbgmsg.h"
#include "pal/signal.hpp"
#include "pal/context.h"
#include "pal/seh.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>

#include <switch.h>

SET_DEFAULT_DEBUG_CHANNEL(EXCEPT); // some headers have code with asserts, so do this first

// RESERVED_SEH_BIT is defined file-locally in seh.cpp (internal linkage) and
// isn't exposed in a header, so redefine the value here for libnx.
namespace { const UINT LIBNX_RESERVED_SEH_BIT = 0x800000; }

/* debugbreak */

extern "C" void
DBG_DebugBreak()
{
    __builtin_trap();
}

/* context */

extern "C" void
RtlCaptureContext(OUT PCONTEXT pContextRecord)
{
    // No reliable way to capture the current context on libnx (no signals,
    // no ucontext). Callers must tolerate a zero-filled record.
    memset(pContextRecord, 0, sizeof(*pContextRecord));
}

extern "C" void
CONTEXT_CaptureContext(LPCONTEXT lpContext)
{
    _ASSERT(!"CONTEXT_CaptureContext not implemented on libnx");
}

extern "C" void ThrowExceptionFromContextInternal(CONTEXT* context, PAL_SEHException* ex)
{
    _ASSERT(!"ThrowExceptionFromContextInternal not implemented on libnx");
}

/* unwind */

void ExecuteHandlerOnCustomStack(int code, siginfo_t *siginfo, void *context, size_t sp, SignalHandlerWorkerReturnPoint* returnPoint)
{
    _ASSERT(!"ExecuteHandlerOnCustomStack not implemented on libnx");
}

/* context.cpp equivalents (excluded from the libnx build) */

extern "C" BOOL CONTEXT_GetRegisters(DWORD processId, LPCONTEXT lpContext)
{
    _ASSERT(!"CONTEXT_GetRegisters not implemented on libnx");
    return FALSE;
}

void CONTEXTToNativeContext(CONST CONTEXT *lpContext, native_context_t *native)
{
    _ASSERT(!"CONTEXTToNativeContext not implemented on libnx");
}

void CONTEXTFromNativeContext(const native_context_t *native, LPCONTEXT lpContext,
                              ULONG contextFlags)
{
    _ASSERT(!"CONTEXTFromNativeContext not implemented on libnx");
}

LPVOID GetNativeContextPC(const native_context_t *context)
{
    _ASSERT(!"GetNativeContextPC not implemented on libnx");
    return nullptr;
}

LPVOID GetNativeContextSP(const native_context_t *context)
{
    _ASSERT(!"GetNativeContextSP not implemented on libnx");
    return nullptr;
}

DWORD CONTEXTGetExceptionCodeForSignal(const siginfo_t *siginfo,
                                       const native_context_t *context)
{
    _ASSERT(!"CONTEXTGetExceptionCodeForSignal not implemented on libnx");
    return 0;
}

// WIN32 SEH entry — normally in seh-unwind.cpp which we exclude on libnx.
// Mirrors that file's RaiseException: allocate EXCEPTION_RECORD + CONTEXT,
// fill the record, throw PAL_SEHException (what RtlpRaiseException does on
// Unix). The CONTEXT capture / PAL_VirtualUnwind step is skipped.
extern "C" VOID PALAPI
RaiseException(
    IN DWORD dwExceptionCode,
    IN DWORD dwExceptionFlags,
    IN DWORD nNumberOfArguments,
    IN CONST ULONG_PTR *lpArguments)
{
    if (dwExceptionCode & LIBNX_RESERVED_SEH_BIT)
    {
        dwExceptionCode ^= LIBNX_RESERVED_SEH_BIT;
    }

    if (nNumberOfArguments > EXCEPTION_MAXIMUM_PARAMETERS)
    {
        nNumberOfArguments = EXCEPTION_MAXIMUM_PARAMETERS;
    }

    EXCEPTION_RECORD *exceptionRecord;
    CONTEXT *contextRecord;
    AllocateExceptionRecords(&exceptionRecord, &contextRecord);

    exceptionRecord->ExceptionCode = dwExceptionCode;
    exceptionRecord->ExceptionFlags = dwExceptionFlags;
    exceptionRecord->ExceptionRecord = NULL;
    exceptionRecord->ExceptionAddress = NULL;
    exceptionRecord->NumberParameters = nNumberOfArguments;
    if (nNumberOfArguments)
    {
        memcpy(exceptionRecord->ExceptionInformation, lpArguments,
               nNumberOfArguments * sizeof(ULONG_PTR));
    }

    throw PAL_SEHException(exceptionRecord, contextRecord);
}

// WIN32 debug-output APIs. pal/src/debug/debug.cpp is excluded on libnx.
extern "C" VOID PALAPI
DebugBreak()
{
    __builtin_trap();
}

extern "C" VOID PALAPI
OutputDebugStringA(IN LPCSTR lpOutputString)
{
    if (lpOutputString)
    {
        svcOutputDebugString(lpOutputString, strlen(lpOutputString));
        fputs(lpOutputString, stderr);
    }
}

extern "C" VOID PALAPI
OutputDebugStringW(IN LPCWSTR lpOutputString)
{
    (void)lpOutputString;  // wchar_t* — not converted here; stub only
}

// PAL exception-record allocation — normally in seh-unwind.cpp which we
// exclude on libnx. Both records share a single allocation (mirroring the
// Unix layout) so the matching free is a single free() of the combined
// memory, keyed off the contextRecord pointer.
struct LibnxExceptionRecords
{
    CONTEXT          ContextRecord;
    EXCEPTION_RECORD ExceptionRecord;
};

extern "C" PALIMPORT VOID PALAPI
PAL_FreeExceptionRecords(IN EXCEPTION_RECORD *exceptionRecord, IN CONTEXT *contextRecord)
{
    (void)exceptionRecord;
    // contextRecord is the start of the combined LibnxExceptionRecords allocation.
    free(contextRecord);
}

VOID
AllocateExceptionRecords(EXCEPTION_RECORD** exceptionRecord, CONTEXT** contextRecord)
{
    LibnxExceptionRecords* records = (LibnxExceptionRecords*)calloc(1, sizeof(LibnxExceptionRecords));
    if (records == nullptr)
    {
        abort();
    }
    *contextRecord = &records->ContextRecord;
    *exceptionRecord = &records->ExceptionRecord;
}

extern "C" PALIMPORT BOOL PALAPI
PAL_VirtualUnwind(CONTEXT *context)
{
    (void)context;
    _ASSERT(!"PAL_VirtualUnwind not implemented on libnx");
    return FALSE;
}

// Signal-handling surface — pal/src/exception/signal.cpp is excluded on libnx.
// Stubs return success/no-op so the PAL init/shutdown paths continue.
// These are C++ symbols (declared without extern "C" in pal/src/include/pal/signal.hpp).
BOOL SEHInitializeSignals(CorUnix::CPalThread* pthrCurrent, DWORD flags)
{
    (void)pthrCurrent; (void)flags;
    return TRUE;
}

void SEHCleanupSignals(bool isChildProcess)
{
    (void)isChildProcess;
}

void UnmaskActivationSignal()
{
}

extern "C" VOID PALAPI
PAL_EnableCrashReportBeforeSignalChaining(void)
{
    // No-op: libnx has no signal delivery.
}

// Per-thread context retrieval. Normally implemented via ptrace on Unix.
extern "C" PALIMPORT BOOL PALAPI
GetThreadContext(IN HANDLE hThread, IN OUT LPCONTEXT lpContext)
{
    (void)hThread; (void)lpContext;
    _ASSERT(!"GetThreadContext not implemented on libnx");
    return FALSE;
}

// FlushInstructionCache via the GCC builtin (no separate syscall needed).
extern "C" PALIMPORT BOOL PALAPI
FlushInstructionCache(IN HANDLE hProcess, IN LPCVOID lpBaseAddress, IN SIZE_T dwSize)
{
    (void)hProcess;
    __builtin___clear_cache((char*)lpBaseAddress, (char*)lpBaseAddress + dwSize);
    return TRUE;
}

// PAL references getpwuid_r through PAL_GetUserName-style paths; newlib
// lacks it, so provide a stub.
extern "C" int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)uid; (void)pwd; (void)buf; (void)buflen;
    if (result) *result = nullptr;
    return ENOTSUP;
}

// newlib does not declare pthread_setschedparam; scheduling policy is fixed on
// Horizon anyway. No-op stub.
extern "C" int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
    (void)thread; (void)policy; (void)param;
    return 0;
}

extern "C" int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param)
{
    (void)thread; (void)policy; (void)param;
    return 0;
}
