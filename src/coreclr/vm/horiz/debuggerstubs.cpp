// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// horiz has no debugger support, so the debugger (debug/ee) is not built.
// Definitions that the rest of the runtime expects from it.

#include "common.h"
#include "frames.h"

DacGlobals g_dacTable;

void DacGlobals::Initialize()
{
    LIMITED_METHOD_CONTRACT;
}

// Referenced by the cDAC contract descriptor
ULONG CLRJitAttachState = 0;
void* g_pDebugger = nullptr;

unsigned FuncEvalFrame::GetFrameAttribs_Impl(void)
{
    PORTABILITY_ASSERT("FuncEvalFrame::GetFrameAttribs_Impl is not implemented on horiz");
    return 0;
}

TADDR FuncEvalFrame::GetReturnAddressPtr_Impl()
{
    PORTABILITY_ASSERT("FuncEvalFrame::GetReturnAddressPtr_Impl is not implemented on horiz");
    return 0;
}

void FuncEvalFrame::UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats)
{
    PORTABILITY_ASSERT("FuncEvalFrame::UpdateRegDisplay_Impl is not implemented on horiz");
}
