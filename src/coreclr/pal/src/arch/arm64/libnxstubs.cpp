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
#include "pal/thread.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
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

/* context capture/throw come from the real arm64 asm (context2.S,
   exceptionhelper.S) */

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

// ---------------------------------------------------------------------------
// libc functions newlib-for-Switch does not provide
// ---------------------------------------------------------------------------

extern "C" int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

extern "C" unsigned int sleep(unsigned int seconds);
extern "C" unsigned int alarm(unsigned int seconds);

extern "C" int pause(void)
{
    // No signals on libnx; block "forever" (nobody will wake us).
    for (;;)
        sleep(3600);
}

extern "C" int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;
    return -1;
}

extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (memptr == NULL || (alignment & (alignment - 1)) != 0 || alignment < sizeof(void*))
        return EINVAL;
    *memptr = aligned_alloc(alignment, size);
    return *memptr ? 0 : ENOMEM;
}

// pread/pwrite: not in newlib's set for Switch. Sequential seek+op; single
// process, so the fd-offset race does not exist.
extern "C" ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved == (off_t)-1)
        return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t ret = read(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
    return ret;
}

extern "C" ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved == (off_t)-1)
        return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t ret = write(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
    return ret;
}

extern "C" long sysconf(int name)
{
    // Only the values the runtime actually asks for; anything else fails.
    // _SC_NPROCESSORS_ONLN: 3 cores are usable by apps on Horizon (core 3 is
    // reserved for the OS).
    const int _SC_PAGESIZE_ = 8;         // newlib values (sys/unistd.h)
    const int _SC_NPROCESSORS_CONF_ = 9;
    const int _SC_NPROCESSORS_ONLN_ = 10;
    if (name == _SC_NPROCESSORS_ONLN_ || name == _SC_NPROCESSORS_CONF_)
        return 3;
    if (name == _SC_PAGESIZE_)
        return 4096;
    errno = EINVAL;
    return -1;
}

extern "C" int timespec_get(struct timespec *ts, int base)
{
    // TIME_UTC == 1
    if (base != 1)
        return 0;
    if (clock_gettime(CLOCK_REALTIME, ts) != 0)
        return 0;
    return base;
}

extern "C" pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    errno = ECHILD; // no child processes on libnx
    return -1;
}


// PAL_ProbeMemory: pal/src/debug/debug.cpp (which provides it on other unixes)
// is excluded for libnx. With no signals there is no graceful probe; report
// everything readable (matches the "trust the pointer" behavior of the wasm
// in-bounds check).
#include "pal/palinternal.h"

BOOL
PALAPI
PAL_ProbeMemory(
    PVOID pBuffer,
    DWORD cbBuffer,
    BOOL fWriteAccess)
{
    (void)pBuffer; (void)cbBuffer; (void)fWriteAccess;
    return TRUE;
}

// CorDBGetInterface lives in debug/ee (not built on libnx). The debugger is
// never used; provide the symbol so ceemain.cpp links. Returning an error
// makes InitializeDebugger abort cleanly if it is ever called.
struct DebugInterface;
extern "C" int CorDBGetInterface(DebugInterface** rcInterface)
{
    if (rcInterface) *rcInterface = NULL;
    return (int)0x80004005; // E_FAIL
}

// InjectActivationInternal: signal.cpp is excluded for libnx. Activation
// injection (thread hijacking for GC suspension) needs signals; not available.
CorUnix::PAL_ERROR InjectActivationInternal(CorUnix::CPalThread* pThread)
{
    return ERROR_NOT_SUPPORTED;
}
