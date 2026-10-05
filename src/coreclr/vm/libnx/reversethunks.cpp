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
extern "C" void svcOutputDebugString(const char*, size_t);

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

// ---------------------------------------------------------------------------
// Marshal.GetFunctionPointerForDelegate support.
//
// On portable-entrypoint platforms there is no runtime code generation, so
// UMEntryThunks cannot exist. libnx instead hands out one of a fixed pool of
// precompiled native thunks for the simple delegate shapes native libraries
// actually request (currently: void(void*) callbacks, e.g. FNA3D_LogFunc).
// The thunk indexes into a slot table holding a strong handle to the delegate.

#include "interpexec.h"
#include "comdelegate.h"

namespace
{
    constexpr int kDelegateThunkCount = 16;
    OBJECTHANDLE g_delegateThunkTargets[kDelegateThunkCount] = {0};
    MethodDesc* g_delegateThunkInvoke[kDelegateThunkCount] = {0};
    int g_delegateThunkAlloc = 0;

    int64_t LibnxDelegateThunkDispatch(int slot, int64_t a0, int64_t a1, int64_t a2, int64_t a3)
    {
        CONTRACTL
        {
            THROWS;
            GC_TRIGGERS;
            MODE_ANY;
        }
        CONTRACTL_END;

        OBJECTHANDLE h = g_delegateThunkTargets[slot];
        _ASSERTE(h != NULL);

        if (GetThreadNULLOk() == NULL)
        {
            // Native thread the runtime has never seen; no Thread object, no
            // interp context. Nothing sane to do.
            const char msg[] = "libnx: delegate thunk on unattached thread\n";
            svcOutputDebugString(msg, sizeof(msg) - 1);
            return 0;
        }

        Object* target;
        MethodDesc* md;
        MethodDesc* pInvoke;
        {
            // Object access (ObjectFromHandle, delegate fields) must happen in
            // cooperative GC mode (debug OBJECTREF validation enforces it).
            GCX_COOP();
            OBJECTREF del = ObjectFromHandle(h);
            DelegateObject* d = (DelegateObject*)OBJECTREFToObject(del);
            OBJECTREF targetRef = d->GetTarget();
            target = OBJECTREFToObject(targetRef);
            // Static-method delegates keep the real target in _methodPtrAux.
            PCODE pc = d->GetMethodPtrAux() != (PCODE)0 ? d->GetMethodPtrAux() : d->GetMethodPtr();
            md = PortableEntryPoint::GetMethodDesc(pc);
            _ASSERTE(md != NULL);
            pInvoke = g_delegateThunkInvoke[slot];
        }

        int64_t args[5];
        int n = 0;
        // Only prepend the delegate target for instance-method delegates.
        // On this platform delegates over static methods can carry a non-NULL
        // _target (observed with FNA's static PInvoke-callback delegates:
        // _target holds a heap object, not a bound first argument), and
        // prepending it shifts every native argument one slot, corrupting the
        // call (KeyNotFoundException in FNA3D.INTERNAL_Read).
        if (target != NULL && !md->IsStatic())
            args[n++] = (int64_t)target;
        bool returnsVoid = true;
        if (pInvoke != NULL)
        {
            MetaSig invokeSig(pInvoke);
            UINT numArgs = invokeSig.NumFixedArgs();
            const int64_t nativeArgs[4] = { a0, a1, a2, a3 };
            for (UINT i = 0; i < numArgs && i < 4; i++)
                args[n++] = nativeArgs[i];
            returnsVoid = invokeSig.IsReturnTypeVoid() == TRUE;
        }

        int64_t ret = 0;
        // ExecuteInterpretedMethodFromUnmanaged expects preemptive mode on
        // entry (it calls DoPrestub with CallerGCMode::Preemptive); the
        // GCX_COOP above has unwound by here. Under gcConservative nothing
        // moves, so holding raw object pointers across the call is safe.
        ExecuteInterpretedMethodFromUnmanaged(md, (int8_t*)args, n * sizeof(int64_t),
            returnsVoid ? nullptr : (int8_t*)&ret, (PCODE)&LibnxDelegateThunkDispatch);
        return ret;
    }
}

// One thunk per slot and arity: the slot index must be a compile-time
// constant. All thunks return int64 in x0, which is ABI-compatible with the
// void/i4/u4/pointer returns native callers actually use.
#define LIBNX_DELEGATE_THUNK(i) \
    extern "C" int64_t LibnxDelegateThunk##i(void* a0) { return LibnxDelegateThunkDispatch(i, (int64_t)a0, 0, 0, 0); }

LIBNX_DELEGATE_THUNK(0)  LIBNX_DELEGATE_THUNK(1)  LIBNX_DELEGATE_THUNK(2)  LIBNX_DELEGATE_THUNK(3)
LIBNX_DELEGATE_THUNK(4)  LIBNX_DELEGATE_THUNK(5)  LIBNX_DELEGATE_THUNK(6)  LIBNX_DELEGATE_THUNK(7)
LIBNX_DELEGATE_THUNK(8)  LIBNX_DELEGATE_THUNK(9)  LIBNX_DELEGATE_THUNK(10) LIBNX_DELEGATE_THUNK(11)
LIBNX_DELEGATE_THUNK(12) LIBNX_DELEGATE_THUNK(13) LIBNX_DELEGATE_THUNK(14) LIBNX_DELEGATE_THUNK(15)

#define LIBNX_DELEGATE_THUNK_A2(i) \
    extern "C" int64_t LibnxDelegateThunkA2_##i(void* a0, int64_t a1) { return LibnxDelegateThunkDispatch(i, (int64_t)a0, a1, 0, 0); }
#define LIBNX_DELEGATE_THUNK_A3(i) \
    extern "C" int64_t LibnxDelegateThunkA3_##i(void* a0, void* a1, int64_t a2) { return LibnxDelegateThunkDispatch(i, (int64_t)a0, (int64_t)a1, a2, 0); }

LIBNX_DELEGATE_THUNK_A2(0)  LIBNX_DELEGATE_THUNK_A2(1)  LIBNX_DELEGATE_THUNK_A2(2)  LIBNX_DELEGATE_THUNK_A2(3)
LIBNX_DELEGATE_THUNK_A2(4)  LIBNX_DELEGATE_THUNK_A2(5)  LIBNX_DELEGATE_THUNK_A2(6)  LIBNX_DELEGATE_THUNK_A2(7)
LIBNX_DELEGATE_THUNK_A2(8)  LIBNX_DELEGATE_THUNK_A2(9)  LIBNX_DELEGATE_THUNK_A2(10) LIBNX_DELEGATE_THUNK_A2(11)
LIBNX_DELEGATE_THUNK_A2(12) LIBNX_DELEGATE_THUNK_A2(13) LIBNX_DELEGATE_THUNK_A2(14) LIBNX_DELEGATE_THUNK_A2(15)

LIBNX_DELEGATE_THUNK_A3(0)  LIBNX_DELEGATE_THUNK_A3(1)  LIBNX_DELEGATE_THUNK_A3(2)  LIBNX_DELEGATE_THUNK_A3(3)
LIBNX_DELEGATE_THUNK_A3(4)  LIBNX_DELEGATE_THUNK_A3(5)  LIBNX_DELEGATE_THUNK_A3(6)  LIBNX_DELEGATE_THUNK_A3(7)
LIBNX_DELEGATE_THUNK_A3(8)  LIBNX_DELEGATE_THUNK_A3(9)  LIBNX_DELEGATE_THUNK_A3(10) LIBNX_DELEGATE_THUNK_A3(11)
LIBNX_DELEGATE_THUNK_A3(12) LIBNX_DELEGATE_THUNK_A3(13) LIBNX_DELEGATE_THUNK_A3(14) LIBNX_DELEGATE_THUNK_A3(15)

static void* const g_delegateThunksA1[kDelegateThunkCount] = {
    (void*)&LibnxDelegateThunk0,  (void*)&LibnxDelegateThunk1,
    (void*)&LibnxDelegateThunk2,  (void*)&LibnxDelegateThunk3,
    (void*)&LibnxDelegateThunk4,  (void*)&LibnxDelegateThunk5,
    (void*)&LibnxDelegateThunk6,  (void*)&LibnxDelegateThunk7,
    (void*)&LibnxDelegateThunk8,  (void*)&LibnxDelegateThunk9,
    (void*)&LibnxDelegateThunk10, (void*)&LibnxDelegateThunk11,
    (void*)&LibnxDelegateThunk12, (void*)&LibnxDelegateThunk13,
    (void*)&LibnxDelegateThunk14, (void*)&LibnxDelegateThunk15,
};

static void* const g_delegateThunksA2[kDelegateThunkCount] = {
    (void*)&LibnxDelegateThunkA2_0,  (void*)&LibnxDelegateThunkA2_1,
    (void*)&LibnxDelegateThunkA2_2,  (void*)&LibnxDelegateThunkA2_3,
    (void*)&LibnxDelegateThunkA2_4,  (void*)&LibnxDelegateThunkA2_5,
    (void*)&LibnxDelegateThunkA2_6,  (void*)&LibnxDelegateThunkA2_7,
    (void*)&LibnxDelegateThunkA2_8,  (void*)&LibnxDelegateThunkA2_9,
    (void*)&LibnxDelegateThunkA2_10, (void*)&LibnxDelegateThunkA2_11,
    (void*)&LibnxDelegateThunkA2_12, (void*)&LibnxDelegateThunkA2_13,
    (void*)&LibnxDelegateThunkA2_14, (void*)&LibnxDelegateThunkA2_15,
};

static void* const g_delegateThunksA3[kDelegateThunkCount] = {
    (void*)&LibnxDelegateThunkA3_0,  (void*)&LibnxDelegateThunkA3_1,
    (void*)&LibnxDelegateThunkA3_2,  (void*)&LibnxDelegateThunkA3_3,
    (void*)&LibnxDelegateThunkA3_4,  (void*)&LibnxDelegateThunkA3_5,
    (void*)&LibnxDelegateThunkA3_6,  (void*)&LibnxDelegateThunkA3_7,
    (void*)&LibnxDelegateThunkA3_8,  (void*)&LibnxDelegateThunkA3_9,
    (void*)&LibnxDelegateThunkA3_10, (void*)&LibnxDelegateThunkA3_11,
    (void*)&LibnxDelegateThunkA3_12, (void*)&LibnxDelegateThunkA3_13,
    (void*)&LibnxDelegateThunkA3_14, (void*)&LibnxDelegateThunkA3_15,
};

// Returns a native function pointer that invokes the delegate, or NULL if the
// delegate shape is not supported by the pool.
extern "C" void* LibnxGetDelegateCallback(OBJECTREF delegateObj)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    DelegateObject* d = (DelegateObject*)OBJECTREFToObject(delegateObj);

    // _methodPtrAux is set for delegates over static methods (the _methodPtr
    // is a shuffle thunk; aux holds the real target). Multicast delegates are
    // not supported by the pool.
    // Multicast delegates are not supported by the pool (not detected here;
    // FNA-style single callbacks only).

    MethodTable* pMT = d->GetMethodTable();
    MethodDesc* pInvokeMeth = COMDelegate::FindDelegateInvokeMethod(pMT);
    MetaSig sig(pInvokeMeth);

    // Supported shapes: up to 3 pointer-sized scalar arguments, void or
    // pointer-sized integer return. Covers FNA3D log callbacks and the
    // FNA3D_Image_Load read/skip/eof stream callbacks.
    if (sig.IsReturnTypeVoid() == FALSE)
    {
        CorElementType retEt = sig.GetReturnType();
        switch (retEt)
        {
        case ELEMENT_TYPE_I:
        case ELEMENT_TYPE_U:
        case ELEMENT_TYPE_I4:
        case ELEMENT_TYPE_U4:
        case ELEMENT_TYPE_I8:
        case ELEMENT_TYPE_U8:
        case ELEMENT_TYPE_PTR:
        case ELEMENT_TYPE_FNPTR:
        case ELEMENT_TYPE_BOOLEAN:
            break;
        default:
        {
            char nb[96];
            int nn = snprintf(nb, sizeof(nb), "libnx: delegate cb: unsupported return type %d\n", (int)retEt);
            svcOutputDebugString(nb, nn);
            return NULL;
        }
        }
    }
    UINT numArgs = sig.NumFixedArgs();
    if (numArgs > 3)
    {
        svcOutputDebugString("libnx: delegate cb: too many args\n", 39);
        return NULL;
    }
    for (UINT i = 0; i < numArgs; i++)
    {
        CorElementType et = sig.NextArg();
        switch (et)
        {
        case ELEMENT_TYPE_I:
        case ELEMENT_TYPE_U:
        case ELEMENT_TYPE_I8:
        case ELEMENT_TYPE_U8:
        case ELEMENT_TYPE_PTR:
        case ELEMENT_TYPE_FNPTR:
        case ELEMENT_TYPE_I4:
        case ELEMENT_TYPE_U4:
            break;
        default:
        {
            char nb[96];
            int nn = snprintf(nb, sizeof(nb), "libnx: delegate cb: unsupported arg type %d\n", (int)et);
            svcOutputDebugString(nb, nn);
            return NULL;
        }
        }
    }

    // Weak handles so that slots for short-lived delegates (e.g. per-texture
    // FNA3D_Image_Load callbacks) are reclaimed after collection and reused;
    // long-lived callbacks (loggers) stay rooted by their callers as the
    // GetFunctionPointerForDelegate contract requires.
    int slot = -1;
    // Dedup: marshalling the same delegate object again must return the same
    // thunk instead of leaking a slot. PInvoke stubs call this every time the
    // PInvoke runs (e.g. FNA's static read/skip/eof delegates once per
    // texture); without dedup the pool exhausts after a handful of calls.
    Object* pDelegateRaw = OBJECTREFToObject(delegateObj);
    for (int i = 0; i < g_delegateThunkAlloc && i < kDelegateThunkCount; i++)
    {
        if (g_delegateThunkTargets[i] != NULL &&
            OBJECTREFToObject(ObjectFromHandle(g_delegateThunkTargets[i])) == pDelegateRaw)
        {
            slot = i;
            break;
        }
    }
    if (slot >= 0)
    {
        if (numArgs == 3)
            return g_delegateThunksA3[slot];
        if (numArgs == 2)
            return g_delegateThunksA2[slot];
        return g_delegateThunksA1[slot];
    }
    for (int i = 0; i < g_delegateThunkAlloc; i++)
    {
        if (g_delegateThunkTargets[i] != NULL && ObjectFromHandle(g_delegateThunkTargets[i]) == NULL)
        {
            DestroyWeakHandle(g_delegateThunkTargets[i]);
            g_delegateThunkTargets[i] = NULL;
            slot = i;
            break;
        }
    }
    if (slot < 0)
        slot = InterlockedIncrement(&g_delegateThunkAlloc) - 1;
    if (slot >= kDelegateThunkCount)
    {
        svcOutputDebugString("libnx: delegate cb: thunk pool exhausted\n", 45);
        return NULL;
    }

    g_delegateThunkInvoke[slot] = pInvokeMeth;
    g_delegateThunkTargets[slot] = CreateWeakHandle(GetAppDomain()->GetHandleStore(), delegateObj);
    if (numArgs == 3)
        return g_delegateThunksA3[slot];
    if (numArgs == 2)
        return g_delegateThunksA2[slot];
    return g_delegateThunksA1[slot];
}
