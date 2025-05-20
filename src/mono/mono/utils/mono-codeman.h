/**
 * \file
 */

#ifndef __MONO_CODEMAN_H__
#define __MONO_CODEMAN_H__

#include <mono/utils/mono-publib.h>

typedef struct _MonoCodeManager MonoCodeManager;

#define MONO_CODE_MANAGER_CALLBACKS \
	MONO_CODE_MANAGER_CALLBACK (void, chunk_new, (void *chunk, int size)) \
	MONO_CODE_MANAGER_CALLBACK (void, chunk_destroy, (void *chunk)) \

typedef struct {

#undef MONO_CODE_MANAGER_CALLBACK
#define MONO_CODE_MANAGER_CALLBACK(ret, name, sig) ret (*name) sig;

    MONO_CODE_MANAGER_CALLBACKS

} MonoCodeManagerCallbacks;

MonoCodeManager* mono_code_manager_new     (void);
MonoCodeManager* mono_code_manager_new_dynamic (void);
MonoCodeManager* mono_code_manager_new_aot (void);
void             mono_code_manager_destroy (MonoCodeManager *cman);
void             mono_code_manager_invalidate (MonoCodeManager *cman);
void             mono_code_manager_set_read_only (MonoCodeManager *cman);

void*            mono_code_manager_reserve_align (MonoCodeManager *cman, int size, int alignment);

void*            mono_code_manager_reserve (MonoCodeManager *cman, int size);
void             mono_code_manager_commit  (MonoCodeManager *cman, void *data, int size, int newsize);
int              mono_code_manager_size    (MonoCodeManager *cman, int *used_size);
void             mono_code_manager_init (gboolean no_exec);
void             mono_code_manager_cleanup (void);
void             mono_code_manager_install_callbacks (const MonoCodeManagerCallbacks* callbacks);

/* find the extra block allocated to resolve branches close to code */
typedef int    (*MonoCodeManagerFunc)      (void *data, int csize, int size, void *user_data);
void            mono_code_manager_foreach  (MonoCodeManager *cman, MonoCodeManagerFunc func, void *user_data);

void mono_codeman_enable_write (void);
void mono_codeman_disable_write (void);

// On libnx, we can only allocate W^X memory so we need to manualy switch permissions every time we need to write new code
// However, the same memory region is mapped at two different addresses, one for executing and one for writing.
// Codeman clients do not have this assumption so for compatibility we provide a way to transparently switch between the two.
// the _ex functions take the current address held by the client and return the address that should be used to write or execute to.
// This is very hacky and could cause issues if the client is not careful (eg, copying the address before calling the function)
// These functions work in conjunction with the new MINI_xyz_CODEGEN_EX macros
guint8* mono_codeman_enable_write_ex (void* exec_address, const char* trace_line);
guint8* mono_codeman_disable_write_ex (void* write_address, const char* trace_line);

guint8* mono_codeman_find_write_address (void* exec_address, const char* trace_line);
guint8* mono_codeman_find_exec_address (void* write_address, const char* trace_line);

#endif /* __MONO_CODEMAN_H__ */

