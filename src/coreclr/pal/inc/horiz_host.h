// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Contract between the horiz runtime and the application that links it.
//
// On Horizon the runtime is linked statically into the application, which owns
// process startup, the heap layout and the native libraries. The application
// configures the runtime through the functions below before it initializes the
// runtime, and may implement the hooks to provide services the OS does not have.

#ifndef __HORIZ_HOST_H__
#define __HORIZ_HOST_H__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Implemented by the runtime
//

// Hand the range [memoryStart, memoryEnd) to the PAL's mmap implementation.
// Horizon has no mmap: anonymous mappings are carved out of this range, which
// must be reserved and backed by memory. Call before initializing the runtime.
void horiz_fakemmap_init(size_t memoryStart, size_t memoryEnd);

// Size of the range, the most that can be mapped at once.
size_t horiz_fakemmap_size(void);

// Number of bytes currently mapped from the range.
size_t horiz_fakemmap_used(void);

// Whether the address lies inside the range.
int horiz_fakemmap_owns(const void* address);

// Translate an address in the runtime's executable code arena to its writable
// alias. Returns false for addresses outside the arena or in parts of it that are
// not backed yet. *available is the number of bytes from the address to the end
// of the backed memory.
bool minipal_horiz_get_write_alias(void* address, void** writable, size_t* available);

// Make code written through the writable alias visible to instruction fetch.
// Accepts either alias. Returns false for ranges that are not entirely in backed
// memory of the arena.
bool minipal_horiz_flush_instruction_cache(void* address, size_t size);

// The code arena reserves DOTNET_HorizCodeArenaReserve bytes (hexadecimal, 512 MB
// by default) of address space and backs it in 64 MB chunks, one CodeMemory object
// each, as the runtime commits code. Reports the reserved size, the backed size and
// the number of chunks (CodeMemory objects) in use. Any pointer may be NULL.
// Returns false before the arena is created.
bool minipal_horiz_get_code_arena_info(size_t* reserved, size_t* backed, size_t* chunks);

//
// Hooks the application may implement (the runtime has weak defaults)
//

// Native library loading. Horizon has no dynamic loader; the application
// resolves libraries and symbols from tables of statically linked code.
// The defaults fail every request.
void* horiz_host_dlopen(const char* name);
void* horiz_host_dlsym(void* handle, const char* symbol);

// Called when the runtime terminates the process (Environment.Exit and
// unhandled exceptions) so that the application can shut down the services
// it started. Does not return.
void runtime_exit_pal_terminate(int exitCode);

#ifdef __cplusplus
}
#endif

#endif // __HORIZ_HOST_H__
