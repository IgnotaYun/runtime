// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// The runtime's exception entry, for applications that do not provide one
// (-DCLR_CMAKE_HORIZ_EXCEPTION_ENTRY=ON). It replaces libnx/libounce's weak
// __libnx_exception_entry and forwards hardware exceptions to
// PAL_HorizHandleUserException as described in horiz_host.h. Exceptions that
// the runtime does not handle go to the kernel's crash handling (result 0xf801,
// as libnx/libounce's entry does without an exception handler), with the registers of
// the faulting thread.

#include "unixasmmacros.inc"
#include "horiz_host.h"

// ThreadExceptionFrameA64 (switch/arm/thread_context.h)
#define FRAME_X         0
#define FRAME_LR        72
#define FRAME_SP        80
#define FRAME_PC        88
#define FRAME_PSTATE    96
#define FRAME_ESR       108
#define FRAME_FAR       112

#define SVC_RETURN_FROM_EXCEPTION   0x28
#define EXCEPTION_NOT_HANDLED       0xf801

#define STATE_X(n)      (PAL_HORIZ_EXCEPTION_STATE_X + (n) * 8)

        .bss
        .p2align 4
LOCAL_LABEL(ExceptionState):
        .space  PAL_HORIZ_EXCEPTION_STATE_SIZE
        .p2align 4
LOCAL_LABEL(ExceptionStack):
        .space  PAL_HORIZ_EXCEPTION_HANDLER_STACK_SIZE
LOCAL_LABEL(ExceptionStackEnd):

        .text

// x0 = exception type, x1 = ThreadExceptionFrameA64*, sp = the kernel's
// exception stack. x2-x8 are free (their values are in the frame), x9-x29 and
// the FP/SIMD registers are the faulting thread's.
LEAF_ENTRY __libnx_exception_entry, _TEXT
        cbz     x1, LOCAL_LABEL(NotHandled)

        adrp    x2, LOCAL_LABEL(ExceptionState)
        add     x2, x2, :lo12:LOCAL_LABEL(ExceptionState)

        // The registers that the kernel did not save
        str     x9,       [x2, #STATE_X(9)]
        stp     x10, x11, [x2, #STATE_X(10)]
        stp     x12, x13, [x2, #STATE_X(12)]
        stp     x14, x15, [x2, #STATE_X(14)]
        stp     x16, x17, [x2, #STATE_X(16)]
        stp     x18, x19, [x2, #STATE_X(18)]
        stp     x20, x21, [x2, #STATE_X(20)]
        stp     x22, x23, [x2, #STATE_X(22)]
        stp     x24, x25, [x2, #STATE_X(24)]
        stp     x26, x27, [x2, #STATE_X(26)]
        stp     x28, x29, [x2, #STATE_X(28)]

        add     x3, x2, #PAL_HORIZ_EXCEPTION_STATE_V
        stp     q0, q1,   [x3, #0x000]
        stp     q2, q3,   [x3, #0x020]
        stp     q4, q5,   [x3, #0x040]
        stp     q6, q7,   [x3, #0x060]
        stp     q8, q9,   [x3, #0x080]
        stp     q10, q11, [x3, #0x0a0]
        stp     q12, q13, [x3, #0x0c0]
        stp     q14, q15, [x3, #0x0e0]
        stp     q16, q17, [x3, #0x100]
        stp     q18, q19, [x3, #0x120]
        stp     q20, q21, [x3, #0x140]
        stp     q22, q23, [x3, #0x160]
        stp     q24, q25, [x3, #0x180]
        stp     q26, q27, [x3, #0x1a0]
        stp     q28, q29, [x3, #0x1c0]
        stp     q30, q31, [x3, #0x1e0]
        mrs     x4, fpcr
        str     w4, [x2, #PAL_HORIZ_EXCEPTION_STATE_FPCR]
        mrs     x4, fpsr
        str     w4, [x2, #PAL_HORIZ_EXCEPTION_STATE_FPSR]

        // The registers that the kernel saved in the frame
        ldp     x3, x4, [x1, #(FRAME_X + 0x00)]
        stp     x3, x4, [x2, #STATE_X(0)]
        ldp     x3, x4, [x1, #(FRAME_X + 0x10)]
        stp     x3, x4, [x2, #STATE_X(2)]
        ldp     x3, x4, [x1, #(FRAME_X + 0x20)]
        stp     x3, x4, [x2, #STATE_X(4)]
        ldp     x3, x4, [x1, #(FRAME_X + 0x30)]
        stp     x3, x4, [x2, #STATE_X(6)]
        ldr     x3,     [x1, #(FRAME_X + 0x40)]
        str     x3,     [x2, #STATE_X(8)]
        ldr     x3, [x1, #FRAME_LR]
        str     x3, [x2, #STATE_X(30)]
        ldr     x3, [x1, #FRAME_SP]
        str     x3, [x2, #PAL_HORIZ_EXCEPTION_STATE_SP]
        ldr     x3, [x1, #FRAME_PC]
        str     x3, [x2, #PAL_HORIZ_EXCEPTION_STATE_PC]
        ldr     w3, [x1, #FRAME_PSTATE]
        str     w3, [x2, #PAL_HORIZ_EXCEPTION_STATE_PSTATE]
        ldr     w3, [x1, #FRAME_ESR]
        str     w3, [x2, #PAL_HORIZ_EXCEPTION_STATE_ESR]
        ldr     x3, [x1, #FRAME_FAR]
        str     x3, [x2, #PAL_HORIZ_EXCEPTION_STATE_FAR]
        str     w0, [x2, #PAL_HORIZ_EXCEPTION_STATE_TYPE]

        // The kernel's stack has room for a few hundred bytes only
        mov     x19, x1
        mov     x20, sp
        adrp    x3, LOCAL_LABEL(ExceptionStackEnd)
        add     x3, x3, :lo12:LOCAL_LABEL(ExceptionStackEnd)
        mov     sp, x3
        mov     x29, xzr
        mov     x30, xzr

        mov     x0, x2
        bl      C_FUNC(PAL_HorizHandleUserException)

        mov     sp, x20
        mov     x1, x19
        adrp    x2, LOCAL_LABEL(ExceptionState)
        add     x2, x2, :lo12:LOCAL_LABEL(ExceptionState)
        tst     w0, #0xff
        b.eq    LOCAL_LABEL(Restore)

        // Resume the thread where the handler redirected it
        ldp     x3, x4, [x2, #STATE_X(0)]
        stp     x3, x4, [x1, #(FRAME_X + 0x00)]
        ldp     x3, x4, [x2, #STATE_X(2)]
        stp     x3, x4, [x1, #(FRAME_X + 0x10)]
        ldp     x3, x4, [x2, #STATE_X(4)]
        stp     x3, x4, [x1, #(FRAME_X + 0x20)]
        ldp     x3, x4, [x2, #STATE_X(6)]
        stp     x3, x4, [x1, #(FRAME_X + 0x30)]
        ldr     x3,     [x2, #STATE_X(8)]
        str     x3,     [x1, #(FRAME_X + 0x40)]
        ldr     x3, [x2, #STATE_X(30)]
        str     x3, [x1, #FRAME_LR]
        ldr     x3, [x2, #PAL_HORIZ_EXCEPTION_STATE_SP]
        str     x3, [x1, #FRAME_SP]
        ldr     x3, [x2, #PAL_HORIZ_EXCEPTION_STATE_PC]
        str     x3, [x1, #FRAME_PC]
        ldr     w3, [x2, #PAL_HORIZ_EXCEPTION_STATE_PSTATE]
        str     w3, [x1, #FRAME_PSTATE]
        // x9-x29 and the FP/SIMD registers are not restored: the thread
        // resumes in PAL_HorizExceptionTrampoline, which does not use them.
        mov     w0, wzr
        svc     #SVC_RETURN_FROM_EXCEPTION
        b       .

LOCAL_LABEL(Restore):
        // Leave the faulting thread's registers to the crash handling
        add     x3, x2, #PAL_HORIZ_EXCEPTION_STATE_V
        ldp     q0, q1,   [x3, #0x000]
        ldp     q2, q3,   [x3, #0x020]
        ldp     q4, q5,   [x3, #0x040]
        ldp     q6, q7,   [x3, #0x060]
        ldp     q8, q9,   [x3, #0x080]
        ldp     q10, q11, [x3, #0x0a0]
        ldp     q12, q13, [x3, #0x0c0]
        ldp     q14, q15, [x3, #0x0e0]
        ldp     q16, q17, [x3, #0x100]
        ldp     q18, q19, [x3, #0x120]
        ldp     q20, q21, [x3, #0x140]
        ldp     q22, q23, [x3, #0x160]
        ldp     q24, q25, [x3, #0x180]
        ldp     q26, q27, [x3, #0x1a0]
        ldp     q28, q29, [x3, #0x1c0]
        ldp     q30, q31, [x3, #0x1e0]
        ldr     w4, [x2, #PAL_HORIZ_EXCEPTION_STATE_FPCR]
        msr     fpcr, x4
        ldr     w4, [x2, #PAL_HORIZ_EXCEPTION_STATE_FPSR]
        msr     fpsr, x4

        ldr     x9,       [x2, #STATE_X(9)]
        ldp     x10, x11, [x2, #STATE_X(10)]
        ldp     x12, x13, [x2, #STATE_X(12)]
        ldp     x14, x15, [x2, #STATE_X(14)]
        ldp     x16, x17, [x2, #STATE_X(16)]
        ldp     x18, x19, [x2, #STATE_X(18)]
        ldp     x20, x21, [x2, #STATE_X(20)]
        ldp     x22, x23, [x2, #STATE_X(22)]
        ldp     x24, x25, [x2, #STATE_X(24)]
        ldp     x26, x27, [x2, #STATE_X(26)]
        ldp     x28, x29, [x2, #STATE_X(28)]

LOCAL_LABEL(NotHandled):
        mov     w0, #EXCEPTION_NOT_HANDLED
        svc     #SVC_RETURN_FROM_EXCEPTION
        b       .
LEAF_END __libnx_exception_entry, _TEXT
