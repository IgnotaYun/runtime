// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Hardware exceptions on Horizon (exception/machexception.cpp on macOS).
//
// Horizon has no signals: a fault enters the application's exception entry,
// which calls PAL_HorizHandleUserException with the state of the faulting
// thread (see horiz_host.h). The thread is then in the kernel's exception mode,
// where a second fault terminates the process. Like the macOS PAL, the handler
// does not raise the exception there: it only recognizes null references in
// managed code, copies the state to the faulting thread's stack and resumes the
// thread in PAL_HorizExceptionTrampoline, which builds a CONTEXT and calls
// SEHProcessException in normal mode on the thread's own stack.

#include "pal/dbgmsg.h"
#include "pal/palinternal.h"
#include "pal/context.h"
#include "pal/seh.hpp"
#include "pal/virtual.h"

#include <string.h>

// libnx/libounce headers only get C linkage through <switch.h>.
extern "C" {
#include <switch/kernel/thread.h>
}

#include "horiz_host.h"

SET_DEFAULT_DEBUG_CHANNEL(EXCEPT); // some headers have code with asserts, so do this first

static_assert(offsetof(PAL_HorizExceptionState, x) == PAL_HORIZ_EXCEPTION_STATE_X, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, sp) == PAL_HORIZ_EXCEPTION_STATE_SP, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, pc) == PAL_HORIZ_EXCEPTION_STATE_PC, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, pstate) == PAL_HORIZ_EXCEPTION_STATE_PSTATE, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, esr) == PAL_HORIZ_EXCEPTION_STATE_ESR, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, far) == PAL_HORIZ_EXCEPTION_STATE_FAR, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, type) == PAL_HORIZ_EXCEPTION_STATE_TYPE, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, v) == PAL_HORIZ_EXCEPTION_STATE_V, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, fpcr) == PAL_HORIZ_EXCEPTION_STATE_FPCR, "PAL_HorizExceptionState layout");
static_assert(offsetof(PAL_HorizExceptionState, fpsr) == PAL_HORIZ_EXCEPTION_STATE_FPSR, "PAL_HorizExceptionState layout");
static_assert(sizeof(PAL_HorizExceptionState) == PAL_HORIZ_EXCEPTION_STATE_SIZE, "PAL_HorizExceptionState layout");
static_assert(sizeof(PAL_HorizExceptionState) % 16 == 0, "PAL_HorizExceptionState keeps the stack aligned");

extern "C" void PAL_HorizExceptionTrampoline();

// ESR_ELx exception classes of data aborts from EL0 and EL1
#define ESR_EC_SHIFT                26
#define ESR_EC_DATA_ABORT_LOWER_EL  0x24
#define ESR_EC_DATA_ABORT_SAME_EL   0x25
// Data abort ISS: FAR is not valid, write, fault status code
#define ESR_ISS_FNV                 (1u << 10)
#define ESR_ISS_WNR                 (1u << 6)
#define ESR_ISS_DFSC_MASK           0x3f
#define ESR_ISS_DFSC_ALIGNMENT      0x21

// PSTATE.nRW: the thread was executing AArch32 code
#define PSTATE_NRW                  0x10

// The faulting frame may still use memory below its stack pointer
// (AAPCS64 has no red zone, but leave some room anyway)
#define HORIZ_EXCEPTION_RED_ZONE    128
// Stack the dispatch needs below the copied state, on top of what the faulting
// thread used. A thread without it faults in the guard gap instead, which the
// handler leaves to the application's crash handling.
#define HORIZ_EXCEPTION_MIN_STACK   0x4000

static PHORIZ_MANAGED_FAULT_IP_CHECK s_managedFaultIpCheck = NULL;
static bool s_hostExceptionEntry = false;

// The state of an exception that the runtime did not handle after all. The
// dispatch retries the faulting instruction, and the handler declines the
// fault that follows on the same thread, so that the application's crash
// handling sees it.
static volatile Thread* s_declinedThread = NULL;
static volatile uint64_t s_declinedPc = 0;
static volatile uint64_t s_declinedSp = 0;

VOID
PALAPI
PAL_HorizSetManagedFaultIpCheck(
    IN PHORIZ_MANAGED_FAULT_IP_CHECK check)
{
    s_managedFaultIpCheck = check;
}

#ifdef HORIZ_EXCEPTION_ENTRY
// The runtime's exception entry (exceptionentry-horiz.S), which replaces
// libnx/libounce's weak default
extern "C" void __libnx_exception_entry();
static void (*volatile s_runtimeExceptionEntry)() = __libnx_exception_entry;
#endif

BOOL
PALAPI
PAL_HorizHasExceptionEntry()
{
#ifdef HORIZ_EXCEPTION_ENTRY
    if (s_runtimeExceptionEntry != NULL)
    {
        return TRUE;
    }
#endif

    return s_hostExceptionEntry;
}

void PAL_HorizRegisterExceptionEntry(void)
{
    s_hostExceptionEntry = true;
}

// Runs in exception mode, see horiz_host.h: nothing here may fault, allocate
// or wait on another thread.
bool PAL_HorizHandleUserException(PAL_HorizExceptionState* state)
{
    uint32_t ec = state->esr >> ESR_EC_SHIFT;
    if (ec != ESR_EC_DATA_ABORT_LOWER_EL && ec != ESR_EC_DATA_ABORT_SAME_EL)
    {
        return false;
    }

    if ((state->esr & ESR_ISS_FNV) != 0 || (state->esr & ESR_ISS_DFSC_MASK) == ESR_ISS_DFSC_ALIGNMENT)
    {
        return false;
    }

    if ((state->pstate & PSTATE_NRW) != 0)
    {
        return false;
    }

    // The JIT only omits null checks of accesses below half a page
    // (MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT); the VM reports faults in the whole
    // first page as null references (NULL_AREA_SIZE).
    if (state->far >= GetVirtualPageSize())
    {
        return false;
    }

    // The exception mode keeps the faulting thread's TLS registers, so this is
    // the faulting thread.
    Thread* thread = threadGetSelf();
    if (thread == NULL)
    {
        return false;
    }

    if (thread == s_declinedThread && state->pc == s_declinedPc && state->sp == s_declinedSp)
    {
        s_declinedThread = NULL;
        s_declinedPc = 0;
        s_declinedSp = 0;
        return false;
    }

    PHORIZ_MANAGED_FAULT_IP_CHECK managedFaultIpCheck = s_managedFaultIpCheck;
    if (managedFaultIpCheck == NULL || !managedFaultIpCheck((SIZE_T)state->pc))
    {
        return false;
    }

    uintptr_t stackLimit = (uintptr_t)thread->stack_mirror;
    uintptr_t stackBase = stackLimit + thread->stack_sz;
    uintptr_t sp = (uintptr_t)state->sp;
    if (sp <= stackLimit || sp > stackBase)
    {
        return false;
    }

    // Below the faulting frame: a frame record {fp, pc} for the unwinder and
    // the copy of the state, which the trampoline gets in x0 and sp (and uses
    // to set x29 to sp + sizeof(PAL_HorizExceptionState), since kernel return
    // from exception mode does not restore x29).
    if (sp - stackLimit < HORIZ_EXCEPTION_RED_ZONE + 16 + sizeof(PAL_HorizExceptionState) + HORIZ_EXCEPTION_MIN_STACK)
    {
        return false;
    }

    uintptr_t frameRecord = ((sp - HORIZ_EXCEPTION_RED_ZONE) & ~(uintptr_t)15) - 16;
    uintptr_t savedState = frameRecord - sizeof(PAL_HorizExceptionState);

    ((uint64_t*)frameRecord)[0] = state->x[29];
    ((uint64_t*)frameRecord)[1] = state->pc;
    memcpy((void*)savedState, state, sizeof(PAL_HorizExceptionState));

    state->x[0] = savedState;
    state->sp = savedState;
    state->pc = (uint64_t)PAL_HorizExceptionTrampoline;

    return true;
}

static void BuildExceptionContext(const PAL_HorizExceptionState* state, CONTEXT* context, EXCEPTION_RECORD* exceptionRecord)
{
    memset(context, 0, sizeof(CONTEXT));
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;

    for (int i = 0; i < 29; i++)
    {
        context->X[i] = state->x[i];
    }
    context->Fp = state->x[29];
    context->Lr = state->x[30];
    context->Sp = state->sp;
    context->Pc = state->pc;
    context->Cpsr = state->pstate;

    for (int i = 0; i < 32; i++)
    {
        context->V[i].Low = state->v[i].low;
        context->V[i].High = (LONGLONG)state->v[i].high;
    }
    context->Fpcr = state->fpcr;
    context->Fpsr = state->fpsr;

    memset(exceptionRecord, 0, sizeof(EXCEPTION_RECORD));
    exceptionRecord->ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    exceptionRecord->ExceptionFlags = EXCEPTION_IS_SIGNAL;
    exceptionRecord->ExceptionAddress = (PVOID)state->pc;
    exceptionRecord->NumberParameters = 2;
    exceptionRecord->ExceptionInformation[0] = (state->esr & ESR_ISS_WNR) != 0 ? 1 : 0;
    exceptionRecord->ExceptionInformation[1] = (ULONG_PTR)state->far;
}

__attribute__((noinline))
static bool DispatchExceptionInner(CONTEXT* pContext, EXCEPTION_RECORD* pExRecord)
{
    // As in the macOS PAL_DispatchExceptionInner: PAL_VirtualUnwind finds the
    // faulting context through this local when it unwinds SEHProcessException.
    CONTEXT* volatile contextRecord = pContext;
    g_hardware_exception_context_locvar_offset = (int)((char*)&contextRecord - (char*)__builtin_frame_address(0));

    pContext->ContextFlags |= CONTEXT_EXCEPTION_ACTIVE;
    bool continueExecution;
    {
        PAL_SEHException exception(pExRecord, pContext, true);

        TRACE("PAL_HorizDispatchException(EC %08x EA %p)\n", pExRecord->ExceptionCode, pExRecord->ExceptionAddress);

        continueExecution = SEHProcessException(&exception);
        if (continueExecution)
        {
            // Make a copy of the exception records so that we can free them before restoring the context
            *pContext = *exception.ExceptionPointers.ContextRecord;
            *pExRecord = *exception.ExceptionPointers.ExceptionRecord;
        }

        // The exception records are destroyed by the PAL_SEHException destructor now.
    }

    return continueExecution;
}

// Called by PAL_HorizExceptionTrampoline once the thread has left exception mode.
extern "C" void PAL_HorizDispatchException(PAL_HorizExceptionState* state)
{
    CONTEXT context;
    EXCEPTION_RECORD exceptionRecord;
    BuildExceptionContext(state, &context, &exceptionRecord);

    if (DispatchExceptionInner(&context, &exceptionRecord))
    {
        // Horizon cannot restore every register from user mode: RtlRestoreContext
        // uses x16 and x17 for the jump. The VM only continues after a hardware
        // exception for GC stress and the debugger, neither of which horiz has.
        RtlRestoreContext(&context, &exceptionRecord);
    }

    // The VM does not handle the exception after all. Retry the faulting
    // instruction and let the application's crash handling take the fault,
    // with x16 and x17 holding the faulting sp and pc.
    BuildExceptionContext(state, &context, &exceptionRecord);
    s_declinedSp = context.Sp;
    s_declinedPc = context.Pc;
    s_declinedThread = threadGetSelf();
    RtlRestoreContext(&context, NULL);
}
