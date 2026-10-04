// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// libnx reverse (native->managed) thunks for UnmanagedCallersOnly methods.
//
// Reuses the WASI generated thunk table (callhelpers-reverse.cpp): those
// thunks are plain C functions that marshal arguments and call
// ExecuteInterpretedMethodFromUnmanaged, so they work on arm64 as-is.
// The lookup machinery below mirrors vm/wasm/helpers.cpp.
//

#include "common.h"
#include "../wasm/callhelpers.hpp"
#include "precode_portable.hpp"

extern "C" void libnx_revthunk_log(const char* src, int found);

// Defined below; declared here for the anonymous-namespace LookupThunk.
static const struct ReverseThunkMapEntry* LibnxLookupExtraThunk(const char* source);


namespace
{
    ULONG GetHashCode(MethodDesc* pMD, SString& strSource)
    {
        _ASSERTE(pMD != nullptr);

        // the key is in the form $"{MethodName}#{Method.GetParameters().Length}:{AssemblyName}:{Namespace}:{TypeName}";
        const char* pszNamespace = nullptr;
        const char* pszName = pMD->GetMethodTable()->GetFullyQualifiedNameInfo(&pszNamespace);
        MetaSig sig(pMD);
        strSource.Printf("%s#%d:%s:%s:%s",
            pMD->GetName(),
            sig.NumFixedArgs(),
            pMD->GetAssembly()->GetSimpleName(),
            pszNamespace != nullptr ? pszNamespace : "",
            pszName);

        return strSource.Hash();
    }

    struct ReverseThunkMapKey
    {
        ULONG HashCode;
        const char* Source;
    };

    class ReverseThunkHashTraits : public NoRemoveSHashTraits<DefaultSHashTraits<const ReverseThunkMapEntry*>>
    {
    public:
        typedef ReverseThunkMapKey key_t;

        static key_t GetKey(element_t e)
        {
            LIMITED_METHOD_CONTRACT;
            return { e->hashCode, e->Source };
        }
        static BOOL Equals(key_t k1, key_t k2)
        {
            LIMITED_METHOD_CONTRACT;
            return (k1.HashCode == k2.HashCode) && strcmp(k1.Source, k2.Source) == 0;
        }
        static count_t Hash(key_t k)
        {
            LIMITED_METHOD_CONTRACT;
            return k.HashCode;
        }
    };

    typedef SHash<ReverseThunkHashTraits> ReverseThunkHash;
    ReverseThunkHash* reverseThunkCache = nullptr;

    ReverseThunkHash* CreateReverseThunkHashTable()
    {
        ReverseThunkHash* newTable = new ReverseThunkHash();
        newTable->Reallocate(g_ReverseThunksCount * ReverseThunkHash::s_density_factor_denominator / ReverseThunkHash::s_density_factor_numerator + 1);
        for (size_t i = 0; i < g_ReverseThunksCount; i++)
        {
            newTable->Add(&g_ReverseThunks[i]);
        }

        ReverseThunkHash **ppCache = &reverseThunkCache;
        if (InterlockedCompareExchangeT(ppCache, newTable, nullptr) != nullptr)
        {
            // Another thread won the race, discard ours
            delete newTable;
        }
        return *ppCache;
    }

    const ReverseThunkMapValue* LookupThunk(MethodDesc* pMD)
    {
        ReverseThunkHash* table = VolatileLoad(&reverseThunkCache);

        if (table == nullptr)
        {
            table = CreateReverseThunkHashTable();
        }

        SString source;
        ULONG hashCode = GetHashCode(pMD, source);
        ReverseThunkMapKey key = { hashCode, source.GetUTF8() };
        const ReverseThunkMapEntry* entry = table->Lookup(key);
        if (entry == nullptr)
            entry = LibnxLookupExtraThunk(source.GetUTF8());
        libnx_revthunk_log(source.GetUTF8(), entry != nullptr);
        return entry != nullptr ? &entry->value : nullptr;
    }
}

// --- Supplementary thunks missing from the generated WASI table --------------

extern "C" void LookupUnmanagedCallersOnlyMethodByName(const char* fullQualifiedTypeName, const char* methodName, MethodDesc** ppMD);
extern "C" void ExecuteInterpretedMethodFromUnmanaged(MethodDesc* pMD, int8_t* args, size_t argSize, int8_t* ret, PCODE callerIp);

static MethodDesc* MD_EventPipeEventProvider_Callback = nullptr;
static void Call_EventPipeEventProvider_Callback(void* a0, int32_t a1, uint8_t a2, int64_t a3, int64_t a4, void* a5, void* a6)
{
    int64_t args[7] = { (int64_t)a0, (int64_t)a1, (int64_t)a2, a3, a4, (int64_t)a5, (int64_t)a6 };
    if (!MD_EventPipeEventProvider_Callback)
        LookupUnmanagedCallersOnlyMethodByName("System.Diagnostics.Tracing.EventPipeEventProvider, System.Private.CoreLib", "Callback", &MD_EventPipeEventProvider_Callback);
    ExecuteInterpretedMethodFromUnmanaged(MD_EventPipeEventProvider_Callback, (int8_t*)args, sizeof(args), nullptr, (PCODE)&Call_EventPipeEventProvider_Callback);
}

static MethodDesc* MD_ConsolePal_InvalidateTerminalSettings = nullptr;
static void Call_ConsolePal_InvalidateTerminalSettings(void)
{
    if (!MD_ConsolePal_InvalidateTerminalSettings)
        LookupUnmanagedCallersOnlyMethodByName("System.ConsolePal, System.Console", "InvalidateTerminalSettings", &MD_ConsolePal_InvalidateTerminalSettings);
    ExecuteInterpretedMethodFromUnmanaged(MD_ConsolePal_InvalidateTerminalSettings, nullptr, 0, nullptr, (PCODE)&Call_ConsolePal_InvalidateTerminalSettings);
}

static const ReverseThunkMapEntry g_LibnxExtraThunks[] =
{
    { 0, "Callback#7:System.Private.CoreLib:System.Diagnostics.Tracing:EventPipeEventProvider", { &MD_EventPipeEventProvider_Callback, (void*)&Call_EventPipeEventProvider_Callback } },
    { 0, "InvalidateTerminalSettings#0:System.Console:System:ConsolePal", { &MD_ConsolePal_InvalidateTerminalSettings, (void*)&Call_ConsolePal_InvalidateTerminalSettings } },
};

static const ReverseThunkMapEntry* LibnxLookupExtraThunk(const char* source)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_LibnxExtraThunks); i++)
    {
        if (strcmp(source, g_LibnxExtraThunks[i].Source) == 0)
            return &g_LibnxExtraThunks[i];
    }
    return nullptr;
}

void* GetUnmanagedCallersOnlyThunk(MethodDesc* pMD)
{
    STANDARD_VM_CONTRACT;
    _ASSERTE(pMD != NULL);
    _ASSERTE(pMD->HasUnmanagedCallersOnlyAttribute());

    const ReverseThunkMapValue* value = LookupThunk(pMD);
    if (value == NULL)
    {
        // Thunk table covers CoreLib's UnmanagedCallersOnly methods; anything
        // else (e.g. app-defined UCO) has no entry. Fail hard: callers assume success.
        _ASSERTE(!"GetUnmanagedCallersOnlyThunk: no thunk for method");
        return NULL;
    }

    // Update the target method if not already set.
    _ASSERTE(value->Target != NULL);
    if (NULL == (*value->Target))
        *value->Target = pMD;

    _ASSERTE((*value->Target) == pMD);
    _ASSERTE(value->EntryPoint != NULL);
    return value->EntryPoint;
}
