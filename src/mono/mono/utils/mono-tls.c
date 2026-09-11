/**
 * \file
 * Low-level TLS support
 *
 * Thread local variables that are accessed both from native and managed code
 * are defined here and should be accessed only through this APIs
 *
 * Copyright 2013 Xamarin, Inc (http://www.xamarin.com)
 */

#include "mono-tls.h"
#include "mono-tls-inline.h"

#ifdef MONO_KEYWORD_THREAD

/* Runtime offset detection */
#if defined(TARGET_AMD64) && !defined(TARGET_MACH) && !defined(HOST_WIN32) /* __thread likely not tested on mac/win */

#if defined(PIC)
// This only works if libmono is linked into the application
#define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  __asm ("movq " #var "@GOTTPOFF(%%rip), %0" : "=r" (foo)); offset = foo; } while (0)
#else
#define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  __asm ("movq $" #var "@TPOFF, %0" : "=r" (foo)); offset = foo; } while (0)
#endif

#elif defined(TARGET_X86) && !defined(TARGET_MACH) && !defined(HOST_WIN32) && defined(__GNUC__)

#if defined(PIC)
#define MONO_THREAD_VAR_OFFSET(var,offset) do { int tmp; __asm ("call 1f; 1: popl %0; addl $_GLOBAL_OFFSET_TABLE_+[.-1b], %0; movl " #var "@gotntpoff(%0), %1" : "=r" (tmp), "=r" (offset)); } while (0)
#else
#define MONO_THREAD_VAR_OFFSET(var,offset) __asm ("movl $" #var "@ntpoff, %0" : "=r" (offset))
#endif

#elif defined(TARGET_ARM64) && !defined(PIC) && !defined(HOST_WIN32)

#define MONO_THREAD_VAR_OFFSET(var,offset) \
	__asm ( "mov %0, #0\n add %0, %0, #:tprel_hi12:" #var "\n add %0, %0, #:tprel_lo12_nc:" #var "\n" \
		: "=r" (offset))

#elif defined(TARGET_ARM) && defined(__ARM_EABI__) && !defined(PIC)

#define MONO_THREAD_VAR_OFFSET(var,offset) __asm ("     ldr     %0, 1f; b 2f; 1: .word " #var "(tpoff); 2:" : "=r" (offset))

#elif defined(TARGET_S390X)
# if defined(__PIC__)
#  if !defined(__PIE__)
// This only works if libmono is linked into the application
#   define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  				\
						void *x = &var;					\
						__asm__ ("ear   %%r1,%%a0\n"			\
							 "sllg  %%r1,%%r1,32\n"			\
							 "ear   %%r1,%%a1\n"			\
							 "lgr   %0,%1\n"			\
							 "sgr   %0,%%r1\n"			\
							: "=r" (foo) : "r" (x)			\
							: "1", "cc");				\
						offset = foo; } while (0)
#  elif __PIE__ == 1
#   define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  					\
						__asm__ ("lg	%0," #var "@GOTNTPOFF(%%r12)\n\t"	\
							 : "=r" (foo));					\
						offset = foo; } while (0)
#  elif __PIE__ == 2
#   define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  				\
						__asm__ ("larl	%%r1," #var "@INDNTPOFF\n\t"	\
							 "lg	%0,0(%%r1)\n\t"			\
							 : "=r" (foo) :				\
							 : "1", "cc");				\
						offset = foo; } while (0)
#  endif
# else
#  define MONO_THREAD_VAR_OFFSET(var,offset) do { guint64 foo;  			\
						__asm__ ("larl  %%r1,0f\n\t"		\
							 "j     0f\n\t"			\
							 "0:.quad " #var "@NTPOFF\n"	\
							 "1:\n\t"			\
							 "lg    %0,0(%%r1)\n\t"		\
							: "=r" (foo) : : "1");		\
						offset = foo; } while (0)
# endif

#elif defined (TARGET_RISCV) && !defined (PIC)

#define MONO_THREAD_VAR_OFFSET(var, offset) \
	do { \
		guint32 temp; \
		__asm__ ( \
			"lui %0, %%tprel_hi(" #var ")\n" \
			"add %0, %0, tp, %%tprel_add(" #var ")\n" \
			"addi %0, %0, %%tprel_lo(" #var ")\n" \
			: "=r" (temp) \
		); \
		offset = temp; \
	} while (0)

#else

#define MONO_THREAD_VAR_OFFSET(var,offset) (offset) = -1

#endif

// Tls variables for each MonoTlsKey.
// These are extern instead of static for inexplicable C++ compatibility.
//
MONO_KEYWORD_THREAD MonoInternalThread *mono_tls_thread MONO_TLS_FAST;
MONO_KEYWORD_THREAD MonoJitTlsData     *mono_tls_jit_tls MONO_TLS_FAST;
MONO_KEYWORD_THREAD MonoDomain         *mono_tls_domain MONO_TLS_FAST;
MONO_KEYWORD_THREAD SgenThreadInfo     *mono_tls_sgen_thread_info MONO_TLS_FAST;
MONO_KEYWORD_THREAD MonoLMF           **mono_tls_lmf_addr MONO_TLS_FAST;

#elif defined(DISABLE_THREADS)

MonoInternalThread *mono_tls_thread;
MonoJitTlsData     *mono_tls_jit_tls;
MonoDomain         *mono_tls_domain;
SgenThreadInfo     *mono_tls_sgen_thread_info;
MonoLMF           **mono_tls_lmf_addr;

#else

#if defined(TARGET_AMD64) && (defined(TARGET_MACH) || defined(HOST_WIN32))
#define MONO_THREAD_VAR_OFFSET(key,offset) (offset) = (gint32)key
#elif defined(TARGET_X86) && (defined(TARGET_MACH) || defined(HOST_WIN32))
#define MONO_THREAD_VAR_OFFSET(key,offset) (offset) = (gint32)key
#else
#define MONO_THREAD_VAR_OFFSET(var,offset) (offset) = -1
#endif

MonoNativeTlsKey mono_tls_key_thread;
MonoNativeTlsKey mono_tls_key_jit_tls;
MonoNativeTlsKey mono_tls_key_domain;
MonoNativeTlsKey mono_tls_key_sgen_thread_info;
MonoNativeTlsKey mono_tls_key_lmf_addr;

#endif

gint32 mono_tls_offsets [TLS_KEY_NUM];

void
mono_tls_init_gc_keys (void)
{
#ifdef MONO_KEYWORD_THREAD
	MONO_THREAD_VAR_OFFSET (mono_tls_sgen_thread_info, mono_tls_offsets [TLS_KEY_SGEN_THREAD_INFO]);
#elif defined(DISABLE_THREADS)
#else
	mono_native_tls_alloc (&mono_tls_key_sgen_thread_info, NULL);
	MONO_THREAD_VAR_OFFSET (mono_tls_key_sgen_thread_info, mono_tls_offsets [TLS_KEY_SGEN_THREAD_INFO]);
#endif
}

void
mono_tls_init_runtime_keys (void)
{
#ifdef MONO_KEYWORD_THREAD
	MONO_THREAD_VAR_OFFSET (mono_tls_thread, mono_tls_offsets [TLS_KEY_THREAD]);
	MONO_THREAD_VAR_OFFSET (mono_tls_jit_tls, mono_tls_offsets [TLS_KEY_JIT_TLS]);
	MONO_THREAD_VAR_OFFSET (mono_tls_domain, mono_tls_offsets [TLS_KEY_DOMAIN]);
	MONO_THREAD_VAR_OFFSET (mono_tls_lmf_addr, mono_tls_offsets [TLS_KEY_LMF_ADDR]);
#elif defined(DISABLE_THREADS)
#else
	mono_native_tls_alloc (&mono_tls_key_thread, NULL);
	MONO_THREAD_VAR_OFFSET (mono_tls_key_thread, mono_tls_offsets [TLS_KEY_THREAD]);
	mono_native_tls_alloc (&mono_tls_key_jit_tls, NULL);
	MONO_THREAD_VAR_OFFSET (mono_tls_key_jit_tls, mono_tls_offsets [TLS_KEY_JIT_TLS]);
	mono_native_tls_alloc (&mono_tls_key_domain, NULL);
	MONO_THREAD_VAR_OFFSET (mono_tls_key_domain, mono_tls_offsets [TLS_KEY_DOMAIN]);
	mono_native_tls_alloc (&mono_tls_key_lmf_addr, NULL);
	MONO_THREAD_VAR_OFFSET (mono_tls_key_lmf_addr, mono_tls_offsets [TLS_KEY_LMF_ADDR]);
#endif
}

// Some references are from AOT and cannot be inlined.

G_EXTERN_C MonoInternalThread *mono_tls_get_thread_extern (void)
{
	return mono_tls_get_thread ();
}

G_EXTERN_C MonoJitTlsData *mono_tls_get_jit_tls_extern (void)
{
	return mono_tls_get_jit_tls ();
}

G_EXTERN_C MonoDomain *mono_tls_get_domain_extern (void)
{
	return mono_tls_get_domain ();
}

G_EXTERN_C SgenThreadInfo *mono_tls_get_sgen_thread_info_extern (void)
{
	return mono_tls_get_sgen_thread_info ();
}

G_EXTERN_C MonoLMF **mono_tls_get_lmf_addr_extern (void)
{
	return mono_tls_get_lmf_addr ();
}

#if HOST_LIBNX

#include <switch.h>
#include "mono-logger-internals.h"

// libnx has a limit of 12 TLS slot, mono needs more.
// Here we "multiplex" a single TLS slot to workaround this limitation

#define TLS_EMU_MAX_KEYS 32
#define KEY(i) (1ULL << (i))

typedef void (*TlsDestructor)(void*);

static Mutex g_tls_mutex = INVALID_HANDLE;
static s32 g_fake_tls_key = -1;
static u64 g_my_key_bitmap;
static TlsDestructor g_destructors[TLS_EMU_MAX_KEYS];

typedef struct
{
	u64 bitmap;
	void* items[TLS_EMU_MAX_KEYS];
	bool being_destroyed;
} emulated_tls_data;

static void tls_thread_destructor(void* data)
{
	if (!data)
		return;
	
	mono_trace_message (MONO_TRACE_DIAGNOSTICS, "Running fake TLS destructor for %p\n", data);

	emulated_tls_data* tls = (emulated_tls_data*)data;
	for (int i = 0; i < TLS_EMU_MAX_KEYS; i++)
	{
		// If this key is not valid, or has been freed, skip it
		// The call to a destructor may free other keys so we need to check this every time
		if (!(g_my_key_bitmap & KEY(i)))
			continue;

		// If this thread does not use this key, skip it
		if (!(tls->bitmap & KEY(i)))
			continue;

		if (g_destructors[i])
		{
			void* value = tls->items[i];
			tls->items[i] = NULL;
			tls->bitmap &= ~KEY(i);
			g_destructors[i](value);

			mono_trace_message (MONO_TRACE_DIAGNOSTICS, "Ran g_destructors[%d] for value %p\n", i, value);

			if (tls->bitmap & KEY(i))
				g_error("TLS destructor for key %d re-set the value, which is not supported", i);
		}
	}
}	

static emulated_tls_data* fake_tls_get(void)
{
	emulated_tls_data* ptr = threadTlsGet(g_fake_tls_key);	
	if (ptr)
		return ptr;

	ptr = calloc(1, sizeof(emulated_tls_data));
	g_assert(ptr);

	threadTlsSet(g_fake_tls_key, ptr);

	mono_trace_message (MONO_TRACE_DIAGNOSTICS, "Allocated fake TLS for %p\n", ptr);
	return ptr;
}

int mono_native_tls_alloc (MonoNativeTlsKey *key, void *destructor)
{
	mutexLock(&g_tls_mutex);

	if (g_fake_tls_key == -1) 
	{
		g_fake_tls_key = threadTlsAlloc(tls_thread_destructor);
		if (g_fake_tls_key == -1)
			g_error("Failed to initialize fake_tls");
	}

	u64 bitmap = g_my_key_bitmap;
	
	int index = __builtin_ffs(~bitmap) - 1;
	if (index < 0 || index >= TLS_EMU_MAX_KEYS) 
		g_error("Out of TLS slots");

    g_my_key_bitmap = bitmap | KEY(index);
	g_destructors[index] = (TlsDestructor)destructor;
	
	*key = index;

	mutexUnlock(&g_tls_mutex);

	return 1;
}

void mono_native_tls_free (MonoNativeTlsKey key)
{	
	mutexLock(&g_tls_mutex);
	if (key < 0 || key >= TLS_EMU_MAX_KEYS)
		g_error("mono_native_tls_free called with Invalid TLS key");

	u64 bitmap = g_my_key_bitmap;
	if (!(bitmap & KEY(key)))
		g_error("mono_native_tls_free called with free TLS key");
        
    g_my_key_bitmap = bitmap & ~KEY(key);	
	g_destructors[key] = NULL;

	mutexUnlock(&g_tls_mutex);
}

static void ensure_key_valid(MonoNativeTlsKey key)
{
	if (key < 0 || key >= TLS_EMU_MAX_KEYS)
		g_error("Invalid TLS key");

	if (!(g_my_key_bitmap & KEY(key)))
		g_error("TLS key already freed");
}

int mono_native_tls_set_value (MonoNativeTlsKey key, gpointer value)
{
	ensure_key_valid(key);

	emulated_tls_data* data = fake_tls_get();
	data->items[key] = value;
	data->bitmap |= KEY(key);
	return 1;
}

void* mono_native_tls_get_value (MonoNativeTlsKey key)
{
	ensure_key_valid(key);

	emulated_tls_data* data = fake_tls_get();
	return data->items[key];
}

#endif