// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx portability stubs: debugger/funceval frames and the wasm-style
// portable thunk helpers that don't exist for arm64+portable-entrypoints.
// Modeled on vm/wasm/helpers.cpp stubs. None of these should be reached at
// runtime (no debugger, no R2R, IL-only); assert if they are.

#include "common.h"
#include "frames.h"
#include "threads.h"
#include "dllimportcallback.h"

#define LIBNX_PORTABILITY_ASSERT(message) \
    PORTABILITY_ASSERT(message)

// --- FuncEvalFrame (debugger func eval; never used without a debugger) ------

unsigned FuncEvalFrame::GetFrameAttribs_Impl(void)
{
    LIBNX_PORTABILITY_ASSERT("FuncEvalFrame::GetFrameAttribs_Impl is not implemented on libnx");
    return 0;
}

TADDR FuncEvalFrame::GetReturnAddressPtr_Impl()
{
    LIBNX_PORTABILITY_ASSERT("FuncEvalFrame::GetReturnAddressPtr_Impl is not implemented on libnx");
    return 0;
}

void FuncEvalFrame::UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats)
{
    LIBNX_PORTABILITY_ASSERT("FuncEvalFrame::UpdateRegDisplay_Impl is not implemented on libnx");
}

// --- Portable thunk helpers (wasm-only upstream) -----------------------------

void* GetPortableEntryPointToInterpreterThunk(MethodDesc *pMD)
{
    // No pregenerated thunks on libnx; callers treat nullptr as "not available".
    return nullptr;
}

void* GetUnmanagedCallersOnlyThunk(MethodDesc* pMD)
{
    LIBNX_PORTABILITY_ASSERT("GetUnmanagedCallersOnlyThunk is not implemented on libnx");
    return nullptr;
}

void InitializePendingThunkResolutionLock()
{
    LIMITED_METHOD_CONTRACT;
}

// TheUMEntryPrestubWorker: dllimportcallback.cpp is compiled out under
// FEATURE_PORTABLE_ENTRYPOINTS, but asmhelpers.S's TheUMEntryPrestub still
// references it. Declared extern "C" in dllimportcallback.h.
class UMEntryThunkData; // real class is behind !FEATURE_PORTABLE_ENTRYPOINTS
extern "C" PCODE TheUMEntryPrestubWorker(UMEntryThunkData* pUMEntryThunkData)
{
    LIBNX_PORTABILITY_ASSERT("TheUMEntryPrestubWorker is not implemented on libnx");
    return 0;
}
