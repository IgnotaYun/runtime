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
#ifdef NX_TRACE_CALLS
        libnx_revthunk_log(source.GetUTF8(), entry != nullptr);
#endif
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
// CoreCLR's normal UMEntryThunk path is unavailable on portable-entrypoint
// platforms. libnx provides CodeMemory with simultaneous RW/RX aliases, so
// emit a small AArch64 thunk that supplies a stable per-delegate context to a
// shared interpreter dispatcher while preserving the native x0..x3 arguments.

#include "interpexec.h"
#include "comdelegate.h"
extern "C"
{
#include <switch/result.h>
#include <switch/kernel/jit.h>
#include <switch/arm/cache.h>
}

namespace
{
    struct DelegateThunkContext
    {
        OBJECTHANDLE Target;
        MethodDesc* Invoke;
        void* EntryPoint;
        DelegateThunkContext* Next;
    };

    struct DelegateThunkPage
    {
        Jit Code;
        size_t Used;
        DelegateThunkPage* Next;
    };

    constexpr size_t kDelegateThunkSize = 32;
    constexpr size_t kDelegateThunkPageSize = 0x1000;
    DelegateThunkContext* g_delegateThunks = nullptr;
    DelegateThunkPage* g_delegateThunkPages = nullptr;
    DangerousNonHostedSpinLock g_delegateThunkLock;

    extern "C" int64_t LibnxDelegateThunkDispatch(
        int64_t a0,
        int64_t a1,
        int64_t a2,
        int64_t a3,
        DelegateThunkContext* context)
    {
        CONTRACTL
        {
            THROWS;
            GC_TRIGGERS;
            MODE_ANY;
        }
        CONTRACTL_END;

        OBJECTHANDLE h = context->Target;
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
            pInvoke = context->Invoke;
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

    void* AllocateDelegateThunk(DelegateThunkContext* context)
    {
        LIMITED_METHOD_CONTRACT;

        DelegateThunkPage* page = g_delegateThunkPages;
        if (page == nullptr || page->Used + kDelegateThunkSize > page->Code.size)
        {
            page = new (nothrow) DelegateThunkPage{};
            if (page == nullptr)
                return nullptr;

            Result rc = jitCreate(&page->Code, kDelegateThunkPageSize);
            if (R_FAILED(rc) || page->Code.type != JitType_CodeMemory)
            {
                if (R_SUCCEEDED(rc))
                    jitClose(&page->Code);
                delete page;
                return nullptr;
            }

            page->Next = g_delegateThunkPages;
            g_delegateThunkPages = page;
        }

        uint8_t* rw = static_cast<uint8_t*>(jitGetRwAddr(&page->Code)) + page->Used;
        uint8_t* rx = static_cast<uint8_t*>(jitGetRxAddr(&page->Code)) + page->Used;

        // ldr x4, [pc, #16]       ; per-delegate context
        // ldr x16, [pc, #20]      ; shared dispatcher
        // br  x16
        // nop
        // .quad context
        // .quad dispatcher
        const uint32_t instructions[4] = {
            0x58000084u,
            0x580000B0u,
            0xD61F0200u,
            0xD503201Fu,
        };
        memcpy(rw, instructions, sizeof(instructions));
        const uint64_t literals[2] = {
            reinterpret_cast<uint64_t>(context),
            reinterpret_cast<uint64_t>(&LibnxDelegateThunkDispatch),
        };
        memcpy(rw + sizeof(instructions), literals, sizeof(literals));

        armDCacheFlush(rw, kDelegateThunkSize);
        armICacheInvalidate(rx, kDelegateThunkSize);
        page->Used += kDelegateThunkSize;
        return rx;
    }
}

// Returns a native function pointer that invokes the delegate, or NULL if the
// delegate shape is not supported by the interpreter dispatcher.
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

    // Supported shapes: up to 4 pointer-sized scalar arguments, void or
    // pointer-sized integer return. Covers FNA3D log callbacks and the
    // FNA3D_Image_Load read/skip/eof stream callbacks. Floating-point and
    // aggregate ABI shapes require a signature-aware dispatcher.
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
    if (numArgs > 4)
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
            LPCUTF8 ns, nm;
            pMT->GetMDImport()->GetNameOfTypeDef(pMT->GetTypeDefRid(), &nm, &ns);
            int nn = snprintf(nb, sizeof(nb), "libnx: delegate cb: unsupported arg type %d (delegate %s.%s)\n", (int)et, ns ? ns : "?", nm ? nm : "?");
            svcOutputDebugString(nb, nn);
            return NULL;
        }
        }
    }

    // The native pointer is stable for process lifetime. The handle is weak:
    // Marshal.GetFunctionPointerForDelegate requires the caller to keep the
    // delegate alive for as long as native code can call the pointer.
    DelegateThunkContext* context = new (nothrow) DelegateThunkContext{};
    if (context == nullptr)
        return nullptr;

    context->Target = CreateWeakHandle(GetAppDomain()->GetHandleStore(), delegateObj);
    if (context->Target == NULL)
    {
        delete context;
        return nullptr;
    }

    Object* pDelegateRaw = OBJECTREFToObject(delegateObj);
    DangerousNonHostedSpinLockHolder lockHolder(&g_delegateThunkLock);
    for (DelegateThunkContext* existing = g_delegateThunks; existing != nullptr; existing = existing->Next)
    {
        OBJECTREF existingDelegate = existing->Target != NULL ? ObjectFromHandle(existing->Target) : NULL;
        if (existingDelegate != NULL && OBJECTREFToObject(existingDelegate) == pDelegateRaw)
        {
            DestroyWeakHandle(context->Target);
            delete context;
            return existing->EntryPoint;
        }
    }

    context->Invoke = pInvokeMeth;
    context->EntryPoint = AllocateDelegateThunk(context);
    if (context->EntryPoint == nullptr)
    {
        DestroyWeakHandle(context->Target);
        delete context;
        svcOutputDebugString("libnx: delegate cb: JIT allocation failed\n", 43);
        return nullptr;
    }

    context->Next = g_delegateThunks;
    g_delegateThunks = context;
    return context->EntryPoint;
}
