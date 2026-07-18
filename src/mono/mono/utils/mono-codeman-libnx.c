#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>
#include <mono/utils/mono-os-mutex.h>
#include <mono/utils/mono-tls.h>

#include "mono-logger-internals.h"

#include "mono-codeman-libnx.h"

static mono_mutex_t g_jit_list_mutex;
static JitAreaNode* g_jit_list = NULL;

JitAreaNode* nx_jit_new(size_t size)
{
	Jit native;
	
	Result rc = jitCreate(&native, size);
	if (R_FAILED(rc))
	{
		mono_trace_error(MONO_TRACE_DIAGNOSTICS, "jitCreate failed %x size=%lx", rc, size);
		return NULL;
	}

	// JitType_CodeMemory is special. While switch is R^X, when using a CodeMemory object we can have two different views of the memory, one is writeable and the other is executable, at the same time.
	// In fact, regardless of what the jit API looks like, we can write and execute to this memory without calling the switch to writeable/executable functions.
	// THe only downside is that we need to propagate the concept of the two views to the client.
	if (native.type != JitType_CodeMemory)
	{
		jitClose(&native);
		mono_trace_error(MONO_TRACE_DIAGNOSTICS, "jitCreate failed, only JitType_CodeMemory is supported. Type was %d", native.type);
		return NULL;
	}	

	JitAreaNode* area = g_new0 (JitAreaNode, 1);
	area->nativeJit = native;
	area->next = NULL;

	mono_os_mutex_lock(&g_jit_list_mutex);
	area->next = g_jit_list;
	g_jit_list = area;
	mono_os_mutex_unlock(&g_jit_list_mutex);
	
	mono_trace(G_LOG_LEVEL_DEBUG, MONO_TRACE_DIAGNOSTICS, "Cretaed JIT area %p req=%lx size=%lx rw=%p rx=%p", area, size, native.size, native.rw_addr, native.rx_addr);

	return area;	
}

static void nxlogf(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	svcOutputDebugString(buffer, strlen(buffer));
}

static void nx_jit_free_internal(JitAreaNode* node, bool useMonoTracing)
{
	if (g_jit_list == node)
		g_jit_list = node->next;
	else
	{
		JitAreaNode* prev = g_jit_list;
		while (prev && prev->next != node)
			prev = prev->next;
		
		if (prev)
			prev->next = node->next;
	}

	if (useMonoTracing)
		mono_trace(G_LOG_LEVEL_DEBUG, MONO_TRACE_DIAGNOSTICS, "Freeing JIT area %p size=%lx rw=%p rx=%p", node, node->nativeJit.size, node->nativeJit.rw_addr, node->nativeJit.rx_addr);
	else
		nxlogf("Freeing JIT area %p size=%lx rw=%p rx=%p", node, node->nativeJit.size, node->nativeJit.rw_addr, node->nativeJit.rx_addr);
	
	jitClose(&node->nativeJit);
	g_free(node);
}

void nx_jit_free(JitAreaNode* node)
{
	if (!node)
		return;

	mono_os_mutex_lock(&g_jit_list_mutex);
	nx_jit_free_internal(node, true);
	mono_os_mutex_unlock(&g_jit_list_mutex);
}

JitAreaNode* nx_jit_find_area(void* address, JIT_AREA_LOOKUP type, ptrdiff_t* offset)
{
	JitAreaNode* node = NULL;
	mono_os_mutex_lock(&g_jit_list_mutex);	

	for (node = g_jit_list; node; node = node->next)
	{
		if (type == JIT_AREA_LOOKUP_BY_RX || type == JIT_AREA_LOOKUP_ANY) 
		{
			intptr_t start = (intptr_t)node->nativeJit.rx_addr;
			intptr_t end = start + node->nativeJit.size;
			intptr_t addr = (intptr_t)address;

			if (addr >= start && addr < end)
			{
				if (offset) *offset = addr - start;
				break;			
			}
		}

		if (type == JIT_AREA_LOOKUP_BY_RW || type == JIT_AREA_LOOKUP_ANY) 
		{
			intptr_t start = (intptr_t)node->nativeJit.rw_addr;
			intptr_t end = start + node->nativeJit.size;
			intptr_t addr = (intptr_t)address;

			if (addr >= start && addr < end) 
			{
				if (offset) *offset = addr - start;
				break;			
			}
		}
	}

	mono_os_mutex_unlock(&g_jit_list_mutex);
	return node;
}

void nx_jit_flush_cache(JitAreaNode* node)
{
	// First, flush the write cache for the writeable view
	armDCacheFlush(node->nativeJit.rw_addr, node->nativeJit.size);
	// Then, flush the instruction cache for the executable view
    armDCacheFlush(node->nativeJit.rx_addr, node->nativeJit.size);
}

void nx_jit_flush_cache_by_address(void* address)
{
	JitAreaNode* node = nx_jit_find_area(address, JIT_AREA_LOOKUP_ANY, NULL);
	if (!node)
	{
		mono_trace_error(MONO_TRACE_DIAGNOSTICS, "nx_jit_flush_cache_by_address: node not found for address %p", address);
		return;
	}

	nx_jit_flush_cache(node);
}

// This function might be called from the embedder when closing the runtime.
// This can also happen after mono_jit_cleanup, so we need to be careful not to call any mono functions here.
void mono_nx_jit_force_dispose(void) 
{
	nxlogf("mono_nx_jit_force_dispose enter");

	while (g_jit_list)
	{
		nxlogf("mono_nx_jit_force_dispose loop");
		JitAreaNode* node = g_jit_list;
		nx_jit_free_internal(node, false);
	}

	nxlogf("mono_nx_jit_force_dispose finished");
}