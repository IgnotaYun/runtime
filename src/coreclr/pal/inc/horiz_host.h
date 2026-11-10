// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Contract between the horiz runtime and the application that links it.
//
// On Horizon the runtime is linked statically into the application, which owns
// process startup, the heap layout and the native libraries. The application
// configures the runtime through the functions below before it initializes the
// runtime, and may implement the hooks to provide services the OS does not have.

#ifndef __HORIZ_HOST_H__
#define __HORIZ_HOST_H__

// Layout of PAL_HorizExceptionState, for exception entries written in assembly
#define PAL_HORIZ_EXCEPTION_STATE_X         0   // x0..x30
#define PAL_HORIZ_EXCEPTION_STATE_SP        248
#define PAL_HORIZ_EXCEPTION_STATE_PC        256
#define PAL_HORIZ_EXCEPTION_STATE_PSTATE    264
#define PAL_HORIZ_EXCEPTION_STATE_ESR       268
#define PAL_HORIZ_EXCEPTION_STATE_FAR       272
#define PAL_HORIZ_EXCEPTION_STATE_TYPE      280
#define PAL_HORIZ_EXCEPTION_STATE_V         288 // v0..v31
#define PAL_HORIZ_EXCEPTION_STATE_FPCR      800
#define PAL_HORIZ_EXCEPTION_STATE_FPSR      804
#define PAL_HORIZ_EXCEPTION_STATE_SIZE      816

// Smallest stack that PAL_HorizHandleUserException may be called on
#define PAL_HORIZ_EXCEPTION_HANDLER_STACK_SIZE 0x4000

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Implemented by the runtime
//

// Hand the range [memoryStart, memoryEnd) to the PAL's mmap implementation.
// Horizon has no mmap: anonymous mappings are carved out of this range, which
// must be reserved and backed by memory. Call before initializing the runtime.
void horiz_fakemmap_init(size_t memoryStart, size_t memoryEnd);

// Size of the range, the most that can be mapped at once.
size_t horiz_fakemmap_size(void);

// Number of bytes currently mapped from the range.
size_t horiz_fakemmap_used(void);

// Whether the address lies inside the range.
int horiz_fakemmap_owns(const void* address);

// Translate an address in the runtime's executable code arena to its writable
// alias. Returns false for addresses outside the arena or in parts of it that are
// not backed yet. *available is the number of bytes from the address to the end
// of the backed memory.
bool minipal_horiz_get_write_alias(void* address, void** writable, size_t* available);

// Make code written through the writable alias visible to instruction fetch.
// Accepts either alias. Returns false for ranges that are not entirely in backed
// memory of the arena.
bool minipal_horiz_flush_instruction_cache(void* address, size_t size);

// The code arena reserves DOTNET_HorizCodeArenaReserve bytes (hexadecimal, 512 MB
// by default) of address space and backs it in 64 MB chunks, one CodeMemory object
// each, as the runtime commits code. Reports the reserved size, the backed size and
// the number of chunks (CodeMemory objects) in use. Any pointer may be NULL.
// Returns false before the arena is created.
bool minipal_horiz_get_code_arena_info(size_t* reserved, size_t* backed, size_t* chunks);

// FlushProcessWriteBuffers: execute a full memory barrier on every other thread
// known to the runtime, by pausing and resuming each with svcSetThreadActivity.
// Used by the PAL and the GC; costs two system calls per thread. Device only (the
// horiz mirror uses membarrier).
void horiz_flush_process_write_buffers(void);

//
// Hardware exceptions
//
// Horizon has no signals. A fault in user mode re-enters the process at its
// entry point, which libnx/libounce's crt0 forwards to __libnx_exception_entry with
// x0 = the exception type and x1 = the kernel's ThreadExceptionFrameA64, on a
// stack of a few hundred bytes in the process local region. The kernel only
// saved x0-x8, lr, sp, pc and pstate in that frame: x9-x29 and all FP/SIMD
// registers still hold the values of the faulting thread.
// svcReturnFromException(0) resumes the thread with the (possibly modified)
// x0-x8, lr, sp, pc and pstate of the frame; any other result continues with
// the kernel's crash or debugger handling.
//
// The runtime turns faults of managed code at small addresses (null plus a
// field offset) into NullReferenceExceptions when the application's
// __libnx_exception_entry forwards them. The entry must:
//
// 1. Store x9-x29, q0-q31, fpcr and fpsr in a PAL_HorizExceptionState before
//    running any compiled code, and copy x0-x8, lr, sp, pc, pstate, esr and
//    far from the kernel's frame and the type from x0.
// 2. Switch to a stack of at least PAL_HORIZ_EXCEPTION_HANDLER_STACK_SIZE
//    bytes and call PAL_HorizHandleUserException.
// 3. When it returns true, copy x0-x8, x30, sp, pc and pstate from the state
//    to the kernel's frame and call svcReturnFromException(0). The other
//    registers do not need to be restored.
// 4. When it returns false, the state is unchanged and the fault is not the
//    runtime's: restore x9-x29 and the FP/SIMD registers from the state and
//    continue with the application's crash handling.
//
// The application owns __libnx_exception_entry. Applications without one can
// build the runtime's instead (-DCLR_CMAKE_HORIZ_EXCEPTION_ENTRY=ON, see
// pal/src/horiz/exceptionentry-horiz.S), which returns 0xf801 to the kernel for
// the exceptions that the runtime does not handle; they must then not define
// the symbol themselves.
//
// The kernel lets one thread at a time run in exception mode (the others wait),
// so the state and the stack of step 2 may be static. A fault inside the
// handler terminates the process, so the handler only reads the runtime's data
// structures and writes to the faulting thread's stack after checking its
// bounds. It does not raise the exception itself: it copies the state below
// the faulting thread's stack pointer and redirects the thread to a runtime
// function that raises the exception once the thread has left exception mode.

typedef struct PAL_HorizVectorRegister
{
    uint64_t low;
    uint64_t high;
} PAL_HorizVectorRegister;

typedef struct PAL_HorizExceptionState
{
    uint64_t x[31];     // x0..x28, fp (x29), lr (x30)
    uint64_t sp;
    uint64_t pc;
    uint32_t pstate;
    uint32_t esr;
    uint64_t far;
    uint32_t type;      // Exception type passed in x0 (svc::ExceptionType)
    uint32_t reserved0;
    PAL_HorizVectorRegister v[32];
    uint32_t fpcr;
    uint32_t fpsr;
    uint64_t reserved1;
} PAL_HorizExceptionState;

// Handle a hardware exception. Returns true if it is a null reference in
// managed code; the thread is then to be resumed with the updated state, see
// above. Returns false for any other exception, and before the runtime runs
// managed code.
bool PAL_HorizHandleUserException(PAL_HorizExceptionState* state);

// Declare that the application's exception entry calls
// PAL_HorizHandleUserException. Call before initializing the runtime, which
// refuses JitExplicitNullChecks=0 (code that relies on null references
// faulting) unless exceptions reach it.
void PAL_HorizRegisterExceptionEntry(void);

//
// Hooks the application may implement (the runtime has weak defaults)
//

// Native library loading. Horizon has no dynamic loader; the application
// resolves libraries and symbols from tables of statically linked code.
// The defaults fail every request.
void* horiz_host_dlopen(const char* name);
void* horiz_host_dlsym(void* handle, const char* symbol);

// Called when the runtime terminates the process (Environment.Exit and
// unhandled exceptions) so that the application can shut down the services
// it started. Does not return.
void runtime_exit_pal_terminate(int exitCode);

#ifdef __cplusplus
}
#endif

#endif // !__ASSEMBLER__

#endif // __HORIZ_HOST_H__
