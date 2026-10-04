// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// libnx CallDescr support for portable entrypoints.
//
// On FEATURE_PORTABLE_ENTRYPOINTS platforms, MethodDesc::GetMultiCallableAddrOfCode
// returns the address of the PortableEntryPoint *struct*, which is data in the
// loader heap. Native platforms call through asm stubs (calldescrworkerarm64.S)
// that jump to the target directly, so the PEP must be resolved first:
//   - PEP with real native code -> call the native entrypoint.
//   - PEP without native code  -> run the method in the interpreter, marshalling
//     the CallDescrData argument registers + stack slots into interp layout.
//

#include "common.h"
#include "callhelpers.h"
#include "precode_portable.hpp"
#include "interpexec.h"

extern "C" int nx_fakemmap_owns(const void* p);

void ExecuteInterpretedMethodWithArgs(TADDR targetIp, int8_t* args, size_t argSize, void* retBuff, PCODE callerIp);

// Returns true if the call was fully handled (interpreted) or if pTarget was
// resolved to a real native entrypoint (proceed with the normal asm worker).
// Returns false only when the target is not a portable entrypoint at all.
bool LibnxCallDescrResolveTarget(CallDescrData* pCallDescrData)
{
    if (!nx_fakemmap_owns((void*)pCallDescrData->pTarget))
        return false; // a real code pointer, leave alone

    if (PortableEntryPoint::HasNativeEntryPoint(pCallDescrData->pTarget))
    {
        pCallDescrData->pTarget = (PCODE)PortableEntryPoint::GetActualCode(pCallDescrData->pTarget);
        return true; // target fixed; continue with asm worker
    }

    MethodDesc* pMD = PortableEntryPoint::GetMethodDesc(pCallDescrData->pTarget);
    _ASSERTE(pMD != NULL);

    InterpByteCodeStart* targetIp = pMD->GetInterpreterCode();
    if (targetIp == NULL)
    {
        GCX_PREEMP();
        (void)pMD->DoPrestub(NULL /* MethodTable */, CallerGCMode::Coop);
        targetIp = pMD->GetInterpreterCode();
    }
    if (targetIp == NULL)
    {
        // No interpreter code and no native code: cannot call.
        EEPOLICY_HANDLE_FATAL_ERROR(COR_E_EXECUTIONENGINE);
    }

    // Marshal args: interp stack layout is 8-byte slots: argument registers
    // x0-x7 first, then the stack slots from pSrc.
    size_t numSlots = NUM_ARGUMENT_REGISTERS + pCallDescrData->numStackSlots;
    int8_t* args = (int8_t*)malloc(numSlots * 8);
    _ASSERTE(args != NULL);

#ifdef CALLDESCR_ARGREGS
    if (pCallDescrData->pArgumentRegisters != NULL)
        memcpy(args, pCallDescrData->pArgumentRegisters->x, NUM_ARGUMENT_REGISTERS * 8);
    else
        memset(args, 0, NUM_ARGUMENT_REGISTERS * 8);
#endif
    if (pCallDescrData->numStackSlots > 0)
        memcpy(args + NUM_ARGUMENT_REGISTERS * 8, pCallDescrData->pSrc, pCallDescrData->numStackSlots * 8);

    void* retBuff = &pCallDescrData->returnValue;

    ExecuteInterpretedMethodWithArgs((TADDR)targetIp, args, numSlots * 8, retBuff, (PCODE)&LibnxCallDescrResolveTarget);

    free(args);
    return true;
}
