// libnx dlfcn stubs: no dynamic loading. dlopen always fails; the managed
// runtime is statically linked and resolves native symbols via the host's
// dl-shim table instead.
#include <stddef.h>
#include <dlfcn.h> // the shim header

static const char* err = "dynamic loading is not supported on libnx";

void* dlopen(const char* filename, int flags)
{
    (void)filename; (void)flags;
    return NULL;
}

void* dlsym(void* handle, const char* symbol)
{
    (void)handle; (void)symbol;
    return NULL;
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
