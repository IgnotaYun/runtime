// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Output of the horiz bring-up diagnostics (horiz_diag.h).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "horiz_diag.h"
#include "horiz_host.h"

#ifdef TARGET_HORIZ_MIRROR
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#include <unistd.h>
#else
#include <switch/kernel/svc.h>
#endif

__attribute__((weak)) void horiz_host_diag_write(const char* message, size_t length)
{
#ifdef TARGET_HORIZ_MIRROR
    ssize_t written = write(STDERR_FILENO, message, length);
    (void)written;
#else
    svcOutputDebugString(message, length);
#endif
}

void horiz_diag_printf(const char* format, ...)
{
    char buffer[256];
    int length = snprintf(buffer, sizeof(buffer), "horiz: ");

    va_list args;
    va_start(args, format);
    int message = vsnprintf(buffer + length, sizeof(buffer) - length - 1, format, args);
    va_end(args);

    if (message < 0)
        return;

    length += message;
    if (length > (int)sizeof(buffer) - 2)
        length = (int)sizeof(buffer) - 2;

    buffer[length++] = '\n';
    buffer[length] = '\0';
    horiz_host_diag_write(buffer, (size_t)length);
}

#ifdef TARGET_HORIZ_MIRROR

// The mirror leaves hardware exceptions fatal, as they are on the device.
// Report where the fault happened before the process dies.
static void FaultLogger(int signalNumber, siginfo_t* info, void* context)
{
    const void* pc = NULL;
#if defined(__aarch64__)
    pc = (const void*)((ucontext_t*)context)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (const void*)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
#endif

    void* writable;
    size_t available;
    horiz_diag_printf("fault: signal %d at %p, pc %p%s%s", signalNumber, info->si_addr, pc,
        horiz_fakemmap_owns(info->si_addr) ? ", address in the mmap range" : "",
        minipal_horiz_get_write_alias((void*)pc, &writable, &available) ? ", pc in the code arena" : "");

    // SA_RESETHAND restored the default action: the instruction faults again
    // and terminates the process.
}

void horiz_diag_install_fault_logger(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = FaultLogger;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);

    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}

#endif // TARGET_HORIZ_MIRROR
