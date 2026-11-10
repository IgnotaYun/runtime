// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// DWARF CFI unwinder for horiz (arm64).
//
// Horiz links libgcc, which owns the .eh_frame tables and exports
// _Unwind_Find_FDE, but it has no libunwind, ucontext or dl_iterate_phdr.
// HorizVirtualUnwindCfi evaluates the CFI of the FDE that libgcc finds for a
// PC and restores the caller's registers, like a single unw_step does on the
// other Unix systems.
//
// The unwinder does not depend on the PAL so that it can be tested on its own.
// HorizUnwindContext has the layout of the leading fields of the arm64 CONTEXT.

#ifndef __UNWIND_HORIZ_H__
#define __UNWIND_HORIZ_H__

#include <stdint.h>

struct HorizUnwindContext
{
    uint32_t ContextFlags;
    uint32_t Cpsr;
    uint64_t X[29];
    uint64_t Fp;
    uint64_t Lr;
    uint64_t Sp;
    uint64_t Pc;
    struct { uint64_t Low; uint64_t High; } V[32];
};

// Locations of the callee-saved registers of the caller, as saved by the
// unwound frame. Entries for registers the frame does not save are not touched.
struct HorizUnwindSavedLocations
{
    uint64_t* X19ToX28[10];
    uint64_t* Fp;
    uint64_t* Lr;
    uint64_t* D8ToD15[8];
};

namespace horizcfi
{
    // Unwinds one frame. Unless exactPc is set, context->Pc is a return address
    // and the unwind information of the call instruction before it is used.
    // Returns false, leaving the context untouched, when the PC has no unwind
    // information or its CFI cannot be evaluated.
    bool HorizVirtualUnwindCfi(HorizUnwindContext* context, bool exactPc = false, HorizUnwindSavedLocations* savedLocations = nullptr);
}

#endif // __UNWIND_HORIZ_H__
