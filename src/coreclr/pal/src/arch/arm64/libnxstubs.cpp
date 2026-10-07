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

extern "C" bool minipal_libnx_flush_instruction_cache(void*, size_t);

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
#include <unwind.h>

static _Unwind_Reason_Code LibnxBtCb(struct _Unwind_Context* ctx, void* arg)
{
    int* i = (int*)arg;
    if (*i >= 14) return _URC_END_OF_STACK;
    void* ip = (void*)_Unwind_GetIP(ctx);
    if (ip)
    {
        char buf[96];
        int n = snprintf(buf, sizeof(buf), "  ubt[%d]=%p", *i, ip);
        svcOutputDebugString(buf, n);
        (*i)++;
    }
    return _URC_NO_REASON;
}

extern "C" void LibnxDebugBreakImpl(void* caller, void* fp)
{
    char buf[160];
    int n0 = snprintf(buf, sizeof(buf), "  dbgbrk caller=%p", caller);
    svcOutputDebugString(buf, n0);
    int i = 0;
    _Unwind_Backtrace(LibnxBtCb, &i);
    svcOutputDebugString("\n", 1);
    // Do not trap: the guest BRK re-enters the PAL signal path and recurses
    // DebugBreak until the stack is gone. Exit the process instead.
    svcExitProcess();
    for (;;) { }
}

// Naked entry so x30 (true caller) and x29 are sampled before any prologue.
__asm__(
    ".globl DebugBreak\n"
    ".type DebugBreak, %function\n"
    "DebugBreak:\n"
    "    .cfi_startproc\n"
    "    mov x0, x30\n"
    "    mov x1, x29\n"
    "    b LibnxDebugBreakImpl\n"
    "    .cfi_endproc\n"
);

extern "C" VOID PALAPI
OutputDebugStringA(IN LPCSTR lpOutputString)
{
    if (lpOutputString)
    {
        svcOutputDebugString(lpOutputString, strlen(lpOutputString));
    }
}

extern "C" VOID PALAPI
OutputDebugStringW(IN LPCWSTR lpOutputString)
{
    if (!lpOutputString) return;
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, lpOutputString, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0)
    {
        svcOutputDebugString(buf, n - 1);
    }
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
    if (minipal_libnx_flush_instruction_cache(const_cast<void*>(lpBaseAddress), dwSize))
        return TRUE;
    __builtin___clear_cache((char*)lpBaseAddress, (char*)lpBaseAddress + dwSize);
    return TRUE;
}

#if !defined(LINUXNX_MIRROR)
// Everything in this block overrides/stubs libc functions that glibc already
// provides; excluded from the linuxnx profiling mirror.
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

// Emulated in-process pipes. libnx has no kernel pipes; the PAL only needs
// them for CREATE_SUSPENDED thread startup (1-byte wakeup), so a small
// static table with condvar blocking is sufficient. Pipe fds live in a
// reserved range; read/write/close/fcntl below route them here and forward
// everything else to newlib's reentrant syscalls.
#define NX_PIPE_FD_BASE 10000
#define NX_MAX_PIPES 16
#define NX_PIPE_BUF 256

struct NxPipe
{
    bool used, read_open, write_open;
    uint8_t buf[NX_PIPE_BUF];
    size_t head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
};

static NxPipe g_nxPipes[NX_MAX_PIPES];
static pthread_mutex_t g_nxPipeAllocLock = PTHREAD_MUTEX_INITIALIZER;

static inline bool nx_is_pipe_fd(int fd) { return fd >= NX_PIPE_FD_BASE && fd < NX_PIPE_FD_BASE + 2 * NX_MAX_PIPES; }
static inline NxPipe* nx_pipe_for(int fd) { return &g_nxPipes[(fd - NX_PIPE_FD_BASE) / 2]; }

extern "C" int pipe(int fds[2])
{
    pthread_mutex_lock(&g_nxPipeAllocLock);
    for (int i = 0; i < NX_MAX_PIPES; i++)
    {
        if (!g_nxPipes[i].used)
        {
            NxPipe* p = &g_nxPipes[i];
            p->used = true;
            p->read_open = p->write_open = true;
            p->head = p->tail = p->count = 0;
            pthread_mutex_init(&p->mtx, nullptr);
            pthread_cond_init(&p->cond, nullptr);
            pthread_mutex_unlock(&g_nxPipeAllocLock);
            fds[0] = NX_PIPE_FD_BASE + 2 * i;
            fds[1] = NX_PIPE_FD_BASE + 2 * i + 1;
            return 0;
        }
    }
    pthread_mutex_unlock(&g_nxPipeAllocLock);
    errno = EMFILE;
    return -1;
}

extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (memptr == NULL || (alignment & (alignment - 1)) != 0 || alignment < sizeof(void*))
        return EINVAL;
    *memptr = aligned_alloc(alignment, size);
    return *memptr ? 0 : ENOMEM;
}

// pread/pwrite are provided by the host (native/shared/io_shims.c), which
// implements them on top of newlib's reentrant syscalls.

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
#endif // !LINUXNX_MIRROR


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

// Temporary EE startup stage logging for bring-up debugging (declared in
// vm/libnx context, called from ceemain.cpp).
// On hardware svcOutputDebugString is invisible without a debugger, so also
// mirror to the SD log file that clrhost opens. Flushed per write: the next
// line may never come (crash).
extern "C" void libnx_log_to_file(const char* buf, size_t n)
{
    static FILE* s_logFile;
    if (!s_logFile)
        s_logFile = fopen("/switch/clrhost/clrhost.log", "a");
    if (s_logFile)
    {
        fwrite(buf, 1, n, s_logFile);
        fflush(s_logFile);
    }
}

// Route every svcOutputDebugString below through the file mirror.
extern "C" void nx_svcOutputDebugString_real(const char*, size_t);
__asm__(".set nx_svcOutputDebugString_real, svcOutputDebugString");
static void svc_log_mirror(const char* buf, size_t n)
{
    nx_svcOutputDebugString_real(buf, n);
    libnx_log_to_file(buf, n);
}
#define svcOutputDebugString svc_log_mirror

extern "C" void LibnxStartupLog(const char* stage)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "EE: %s\n", stage);
    svcOutputDebugString(buf, (size_t)n);
}

extern "C" void libnx_pal_log_stack(const char* what, void* a, size_t b)
{
    // Per-call/per-frame trace from GC crawls, stackwalk and the interpreter
    // calli path: each line costs an svc + SD fwrite/fflush, which dominated
    // boot time (GFX LOAD 10+ min on emu). Opt in via NX_TRACE_CALLS=1.
    static int enabled = -1;
    if (enabled < 0)
{
        const char* e = getenv("NX_TRACE_CALLS");
        enabled = (e && e[0] == '1') ? 1 : 0;
}
    if (!enabled)
        return;

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "palstack: %s %p %zu\n", what, a, b);
    svcOutputDebugString(buf, (size_t)n);
}

#if !defined(LINUXNX_MIRROR)
// fd routing for emulated pipes ------------------------------------------------

#include <reent.h>
#include <sys/unistd.h>

extern "C" ssize_t read(int fd, void* buf, size_t n)
{
    if (!nx_is_pipe_fd(fd))
        return _read_r(_REENT, fd, buf, n);

    NxPipe* p = nx_pipe_for(fd);
    if (!p->used || (fd - NX_PIPE_FD_BASE) % 2 != 0 || !p->read_open)
    {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&p->mtx);
    while (p->count == 0)
    {
        if (!p->write_open)
        {
            pthread_mutex_unlock(&p->mtx);
            return 0;
        }
        pthread_cond_wait(&p->cond, &p->mtx);
    }
    size_t got = 0;
    uint8_t* out = (uint8_t*)buf;
    while (got < n && p->count > 0)
    {
        out[got++] = p->buf[p->head];
        p->head = (p->head + 1) % NX_PIPE_BUF;
        p->count--;
    }
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
    return (ssize_t)got;
}

extern "C" ssize_t write(int fd, const void* buf, size_t n)
{
    if (!nx_is_pipe_fd(fd))
    {
        // stdout/stderr: also mirror to svcOutputDebugString so managed
        // Console output is visible under emulators/debuggers.
        if ((fd == 1 || fd == 2) && buf && n > 0)
            svcOutputDebugString((const char*)buf, n);
        // After consoleInit, fd 1 routes to libnx's con_write, whose cursor
        // state and framebuffer drawing are not thread-safe. Managed Console
        // output and runtime logging (minipal_log_write) both land here on
        // different threads, and the race makes consoleDrawChar write outside
        // the framebuffer allocation, corrupting the newlib heap. Serialize
        // stdio writes.
        static pthread_mutex_t s_stdioWriteLock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&s_stdioWriteLock);
        ssize_t ret = _write_r(_REENT, fd, buf, n);
        pthread_mutex_unlock(&s_stdioWriteLock);
        return ret;
    }

    NxPipe* p = nx_pipe_for(fd);
    if (!p->used || (fd - NX_PIPE_FD_BASE) % 2 != 1 || !p->write_open)
    {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&p->mtx);
    if (!p->read_open)
    {
        pthread_mutex_unlock(&p->mtx);
        errno = EPIPE;
        return -1;
    }
    const uint8_t* in = (const uint8_t*)buf;
    size_t put = 0;
    while (put < n)
    {
        while (p->count == NX_PIPE_BUF)
        {
            if (!p->read_open)
            {
                pthread_mutex_unlock(&p->mtx);
                errno = EPIPE;
                return put ? (ssize_t)put : -1;
            }
            pthread_cond_wait(&p->cond, &p->mtx);
        }
        p->buf[p->tail] = in[put++];
        p->tail = (p->tail + 1) % NX_PIPE_BUF;
        p->count++;
    }
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
    return (ssize_t)put;
}

extern "C" int close(int fd)
{
    if (!nx_is_pipe_fd(fd))
        return _close_r(_REENT, fd);

    NxPipe* p = nx_pipe_for(fd);
    if (!p->used)
    {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&p->mtx);
    if ((fd - NX_PIPE_FD_BASE) % 2 == 0)
        p->read_open = false;
    else
        p->write_open = false;
    bool done = !p->read_open && !p->write_open;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
    if (done)
    {
        pthread_mutex_destroy(&p->mtx);
        pthread_cond_destroy(&p->cond);
        p->used = false;
    }
    return 0;
}
#endif // !LINUXNX_MIRROR

extern "C" void libnx_interp_log(const char* name)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "interp: %s\n", name ? name : "?");
    svcOutputDebugString(buf, n);
}

extern "C" void libnx_revthunk_log(const char* src, int found)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "revthunk: %s -> %s\n", src ? src : "?", found ? "hit" : "MISS");
    svcOutputDebugString(buf, n);
}

extern "C" void nx_gc_phase(const char* phase)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "gc: %s\n", phase);
    svcOutputDebugString(buf, (size_t)n);
}
