// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx: debug/ee (cordbee) is not built, so debug/ee/dactable.cpp (which
// defines the DAC globals table) is unavailable. The debugger is never
// attached on device; provide a zeroed table so ceemain.cpp links and
// DacGlobals::Initialize() is a no-op. Any DAC access would read zeros, which
// is fine because there is no DAC consumer in this build.

#include "common.h"

DacGlobals g_dacTable;

void DacGlobals::Initialize()
{
    LIMITED_METHOD_CONTRACT;
}

// Debugger globals referenced by the cdac contract descriptor; debug/ee is not
// built on libnx, so provide zeroed definitions.
ULONG CLRJitAttachState = 0;
void* g_pDebugger = nullptr;
