#ifndef __MONO_CODEMAN_LIBNX_H__
#define __MONO_CODEMAN_LIBNX_H__

#include <mono/utils/mono-publib.h>
#include <switch.h>

typedef struct _JitAreaNode {
	struct _JitAreaNode* next;
	
	// By convention, the main address is the RX address
	Jit nativeJit;
} JitAreaNode;

JitAreaNode* nx_jit_new(size_t size);
void nx_jit_free(JitAreaNode* node);

typedef enum {
	JIT_AREA_LOOKUP_BY_RX,
	JIT_AREA_LOOKUP_BY_RW,
	JIT_AREA_LOOKUP_ANY, // best not to use this to avoid confusion
} JIT_AREA_LOOKUP;

JitAreaNode* nx_jit_find_area(void* address, JIT_AREA_LOOKUP type, ptrdiff_t* offset);

void nx_jit_flush_cache(JitAreaNode* node);
void nx_jit_flush_cache_by_address(void* address);

#endif /* __MONO_CODEMAN_LIBNX_H__ */

