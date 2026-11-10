// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// dlfcn for horiz.
//
// Horizon has no dynamic loader. The runtime is linked statically into the
// application, which resolves native libraries from its own symbol tables
// through horiz_host_dlopen and horiz_host_dlsym.

#include <dlfcn.h>
#include <stddef.h>

#include "horiz_host.h"

__attribute__((weak)) void* horiz_host_dlopen(const char* name)
{
    (void)name;
    return NULL;
}

__attribute__((weak)) void* horiz_host_dlsym(void* handle, const char* symbol)
{
    (void)handle;
    (void)symbol;
    return NULL;
}

void* dlopen(const char* filename, int flags)
{
    (void)flags;
    return horiz_host_dlopen(filename);
}

void* dlsym(void* handle, const char* symbol)
{
    return horiz_host_dlsym(handle, symbol);
}

int dlclose(void* handle)
{
    (void)handle;
    return 0;
}

char* dlerror(void)
{
    return (char*)"dynamic loading is not supported on horiz";
}

int dladdr(const void* addr, Dl_info* info)
{
    // All code belongs to the application image, whose path the homebrew
    // loader passes as argv[0]. Emulators that load the image directly pass no
    // arguments; use the placeholder that minipal_getexepath reports then.
    extern int __system_argc;
    extern char** __system_argv;

    (void)addr;
    if (info == NULL)
        return 0;

    info->dli_fname = (__system_argc > 0 && __system_argv[0] != NULL && __system_argv[0][0] != '\0')
        ? __system_argv[0]
        : "/application.nro";
    info->dli_fbase = NULL;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}
