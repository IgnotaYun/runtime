// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// FlushProcessWriteBuffers for horiz.
//
// Horizon has neither membarrier nor an mprotect that sends IPIs. Instead, every
// other thread known to the PAL is paused and resumed with svcSetThreadActivity.
// Pausing a thread that is running makes the kernel interrupt its core and wait
// until the thread has been switched out, so the thread has left user mode, which
// is a full memory barrier for it. A thread that is blocked in a system call is
// only marked as paused, but it is already outside of user mode and synchronizes
// with the kernel's scheduler before it can return to it.
//
// The cost is two system calls per thread, plus the wait for the interrupted cores
// to switch the threads out, under the process lock. This is far more expensive
// than membarrier, but the runtime only flushes at the start of a suspension and in
// a few rarely taken paths.
//
// A thread that something else (e.g. the application) has paused cannot be paused
// again and is left alone; it is not running, so it needs no barrier. Conversely,
// pausing a thread fails while a flush has it paused.
//
// The horiz mirror uses the upstream membarrier path (thread/process.cpp).

#include "pal/dbgmsg.h"
#include "pal/palinternal.h"
#include "pal/process.h"
#include "pal/thread.hpp"
#include "../thread/procprivate.hpp"

#include <minipal/mutex.h>

// libnx/libounce headers only get C linkage through <switch.h>.
extern "C" {
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
}

#include "horiz_host.h"

SET_DEFAULT_DEBUG_CHANNEL(THREAD); // some headers have code with asserts, so do this first

using namespace CorUnix;

// The lock of the thread list (thread/process.cpp). Taken directly rather than
// with PROCProcessLock, which creates PAL thread data for unknown threads.
extern minipal_mutex g_csProcess;

namespace
{
    // The thread exited (or is exiting) since it was added to the thread list
    bool IsThreadGone(Result rc)
    {
        return rc == KERNELRESULT(InvalidState) ||
            rc == KERNELRESULT(InvalidHandle) ||
            rc == KERNELRESULT(ThreadTerminating);
    }

    void FlushThreadWriteBuffers(Handle thread)
    {
        Result rc = svcSetThreadActivity(thread, ThreadActivity_Paused);
        if (R_FAILED(rc))
        {
            // InvalidState also covers a thread that is already paused
            _ASSERTE(IsThreadGone(rc));
            return;
        }

        rc = svcSetThreadActivity(thread, ThreadActivity_Runnable);
        if (R_FAILED(rc) && !IsThreadGone(rc))
        {
            fprintf(stderr, "FATAL ERROR: Failed to resume thread 0x%x after a flush (0x%x)\n", thread, rc);
            PROCAbort();
        }
    }
}

extern "C" void horiz_flush_process_write_buffers(void)
{
    Handle self = threadGetCurHandle();

    // The list lock keeps the threads in the list from finishing their exit, so
    // their handles stay open, and serializes flushes, so that two flushing threads
    // cannot pause each other.
    minipal_mutex_enter(&g_csProcess);

    for (CPalThread* thread = pGThreadList; thread != NULL; thread = thread->GetNext())
    {
        // Threads that have not started yet have no handle
        Handle handle = thread->GetHorizHandle();
        if (handle != INVALID_HANDLE && handle != self)
        {
            FlushThreadWriteBuffers(handle);
        }
    }

    minipal_mutex_leave(&g_csProcess);
}
