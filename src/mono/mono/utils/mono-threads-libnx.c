#include <config.h>

#if defined(HOST_LIBNX)

#include <mono/utils/mono-threads.h>
#include <pthread.h>
#include <switch.h>

void
mono_threads_platform_get_stack_bounds (guint8 **staddr, size_t *stsize)
{	
	*staddr = NULL;
	*stsize = (size_t)-1;

	Thread *thread = threadGetSelf();
	g_assert(thread);

	*staddr = (guint8 *)thread->stack_mirror;
	if (!*staddr)
		*staddr = (guint8 *)thread->stack_mem;

	*stsize = thread->stack_sz;
}

guint64
mono_native_thread_os_id_get (void)
{
	return (guint64)threadGetCurHandle();
}

void
mono_threads_suspend_init_signals (void)
{
}

gint
mono_threads_suspend_get_suspend_signal (void)
{
	return -1;
}

gint
mono_threads_suspend_get_restart_signal (void)
{
	return -1;
}

gint
mono_threads_suspend_get_abort_signal (void)
{
	return -1;
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_threads_libnx);

#endif
