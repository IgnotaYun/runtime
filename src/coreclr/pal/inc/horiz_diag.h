// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Bring-up diagnostics for horiz.
//
// Horizon has no debugger that can attach to the runtime, so a few markers
// (startup stages, GC phases, use of the code arena) help to place a hang or
// a crash. They are compiled in with CLR_CMAKE_HORIZ_DIAGNOSTICS=ON and are
// written to horiz_host_diag_write (horiz_host.h).

#ifndef __HORIZ_DIAG_H__
#define __HORIZ_DIAG_H__

#ifdef HORIZ_DIAGNOSTICS

#ifdef __cplusplus
extern "C"
#endif
void horiz_diag_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

#define HORIZ_DIAG(...) horiz_diag_printf(__VA_ARGS__)

#ifdef TARGET_HORIZ_MIRROR
// Logs the hardware exceptions that the mirror does not translate
#ifdef __cplusplus
extern "C"
#endif
void horiz_diag_install_fault_logger(void);
#endif

#else // HORIZ_DIAGNOSTICS

#define HORIZ_DIAG(...) ((void)0)

#endif // HORIZ_DIAGNOSTICS

#endif // __HORIZ_DIAG_H__
