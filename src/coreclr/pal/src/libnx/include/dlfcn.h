// libnx dlfcn shim: no dynamic loading on Switch homebrew. All symbols resolve
// to failure/no-op; actual symbol resolution is done by the host via the
// dl-shim table (same approach as the mono port).
#ifndef LIBNX_DLFCN_H
#define LIBNX_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_GLOBAL 0x4
#define RTLD_LOCAL  0x0
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

#endif // LIBNX_DLFCN_H
