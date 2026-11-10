// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "unixasmmacros.inc"
#include "horiz_host.h"

// Where a thread resumes when PAL_HorizHandleUserException (exception-horiz.cpp)
// takes over its hardware exception. The handler left
//
//   x0 = the exception state, copied to the thread's stack, sp == x0
//
// and a frame record {fp, pc} of the faulting frame immediately above the
// state (at sp + PAL_HORIZ_EXCEPTION_STATE_SIZE), so that the faulting frame
// unwinds as the caller of this one.
LEAF_ENTRY PAL_HorizExceptionTrampoline, _TEXT
        .cfi_def_cfa sp, PAL_HORIZ_EXCEPTION_STATE_SIZE + 16
        .cfi_offset x29, -16
        .cfi_offset x30, -8
        add     x29, sp, #PAL_HORIZ_EXCEPTION_STATE_SIZE
        .cfi_def_cfa x29, 16
        bl      C_FUNC(PAL_HorizDispatchException)
        // PAL_HorizDispatchException does not return
        brk     #0
LEAF_END PAL_HorizExceptionTrampoline, _TEXT
