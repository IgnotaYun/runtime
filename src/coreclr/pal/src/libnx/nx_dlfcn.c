// libnx dlfcn: no dynamic loading. The runtime is statically linked into the
// host NRO; dlopen/dlsym are forwarded to the host application, which owns the
// symbol tables (see native/clrhost). Weak defaults make everything fail so
// the PAL treats libraries as missing.
#include <stddef.h>
#include <dlfcn.h> // the shim header

static const char* err = "dynamic loading is not supported on libnx";

// Implemented by the host application. Weak defaults: nothing resolves.
__attribute__((weak)) void* nx_host_dlopen(const char* name)
{
    (void)name;
    return NULL;
}

__attribute__((weak)) void* nx_host_dlsym(void* handle, const char* symbol)
{
    (void)handle; (void)symbol;
    return NULL;
}

void* dlopen(const char* filename, int flags)
{
    (void)flags;
    return nx_host_dlopen(filename);
}

void* dlsym(void* handle, const char* symbol)
{
    return nx_host_dlsym(handle, symbol);
}

int dlclose(void* handle)
{
    (void)handle;
    return 0;
}

char* dlerror(void)
{
    return (char*)err;
}

int dladdr(const void* addr, Dl_info* info)
{
    (void)addr; (void)info;
    return 0;
}
