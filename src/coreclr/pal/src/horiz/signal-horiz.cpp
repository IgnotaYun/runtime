// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Signal handling for horiz (exception/signal.cpp on other Unix systems).
//
// Horizon delivers no POSIX signals: hardware exceptions reach the runtime only
// through the application's exception entry (horiz/exception-horiz.cpp), and
// threads cannot be interrupted by activation injection, so the runtime
// suspends threads at GC polls only.

#include "pal/dbgmsg.h"
#include "pal/palinternal.h"
#include "pal/signal.hpp"
#include "pal/thread.hpp"

SET_DEFAULT_DEBUG_CHANNEL(EXCEPT); // some headers have code with asserts, so do this first

bool g_registered_signal_handlers = false;
bool g_enable_alternate_stack_check = false;

BOOL SEHInitializeSignals(CorUnix::CPalThread *pthrCurrent, DWORD flags)
{
    return TRUE;
}

void SEHCleanupSignals(bool isChildProcess)
{
}

void UnmaskActivationSignal()
{
}

CorUnix::PAL_ERROR InjectActivationInternal(CorUnix::CPalThread* pThread)
{
    return ERROR_NOT_SUPPORTED;
}

void PAL_IgnoreProfileSignal(int signalNum)
{
}
