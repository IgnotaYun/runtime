// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Horizon has no dynamic loader and newlib has no <dlfcn.h>. The runtime is
// linked statically into the application; the PAL implements these functions
// on top of a symbol table that the application registers.

#ifndef HORIZ_DLFCN_H
#define HORIZ_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY    0x1
#define RTLD_NOW     0x2
#define RTLD_GLOBAL  0x4
#define RTLD_LOCAL   0x0
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT    ((void*)-1)

typedef struct
{
    const char* dli_fname;
    void*       dli_fbase;
    const char* dli_sname;
    void*       dli_saddr;
} Dl_info;

void* dlopen(const char* filename, int flags);
void* dlsym(void* handle, const char* symbol);
int   dlclose(void* handle);
char* dlerror(void);
int   dladdr(const void* addr, Dl_info* info);

#ifdef __cplusplus
}
#endif

#endif // HORIZ_DLFCN_H
