// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// POSIX functions that newlib declares but neither newlib nor libnx/libounce implements,
// and that CoreCLR (PAL, GC, minipal) and System.Native link against.
//
// Hosts normally provide these: on Horizon the application owns libc. This copy
// is only built with -DCLR_CMAKE_HORIZ_LIBC_SHIMS=ON, for hosts that do not.
// Enabling it together with a host that provides them is a duplicate-symbol
// link error.
//
// Not covered here: named mutexes and cross-process shared memory. The *at()
// functions below only accept AT_FDCWD or an absolute path, so the PAL's shared
// memory directory setup fails cleanly instead of pretending to work.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <switch/kernel/svc.h>
#include <switch/result.h>

#define LIBC_SHIMS_PAGE_SIZE 4096

// ---------------------------------------------------------------------------
// System configuration
// ---------------------------------------------------------------------------

static long allowed_core_count(void)
{
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask != 0)
        return __builtin_popcountll(mask);

    // Applications get cores 0-2; core 3 belongs to the system.
    return 3;
}

static long process_memory_pages(InfoType type)
{
    u64 value = 0;
    if (R_FAILED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)))
    {
        errno = EINVAL;
        return -1;
    }
    return (long)(value / LIBC_SHIMS_PAGE_SIZE);
}

long sysconf(int name)
{
    switch (name)
    {
        case _SC_PAGESIZE:
            return LIBC_SHIMS_PAGE_SIZE;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:
            return allowed_core_count();
        case _SC_PHYS_PAGES:
            return process_memory_pages(InfoType_TotalMemorySize);
        case _SC_AVPHYS_PAGES:
        {
            long total = process_memory_pages(InfoType_TotalMemorySize);
            long used = process_memory_pages(InfoType_UsedMemorySize);
            return (total < 0 || used < 0 || used > total) ? -1 : total - used;
        }
        case _SC_CLK_TCK:
            return 100;
        case _SC_GETPW_R_SIZE_MAX:
            // Indeterminate: callers fall back to their own buffer size.
            return -1;
        default:
            errno = EINVAL;
            return -1;
    }
}

int getpagesize(void)
{
    return LIBC_SHIMS_PAGE_SIZE;
}

// ---------------------------------------------------------------------------
// Users, sessions and processes
// ---------------------------------------------------------------------------

// Horizon has one application process and no POSIX users or sessions.

uid_t geteuid(void)
{
    return 0;
}

pid_t getsid(pid_t pid)
{
    return pid == 0 ? getpid() : pid;
}

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result)
{
    (void)uid;
    (void)pwd;
    (void)buf;
    (void)buflen;
    *result = NULL;
    return ENOENT;
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ECHILD;
    return -1;
}

// ---------------------------------------------------------------------------
// File system
// ---------------------------------------------------------------------------

int flock(int fd, int operation)
{
    // No other process can hold a conflicting lock.
    (void)fd;
    (void)operation;
    return 0;
}

void sync(void)
{
    // fsdev commits on close/fsync; there is no global flush.
}

int mkfifo(const char* path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOTSUP;
    return -1;
}

static bool at_path_supported(int dirfd, const char* path)
{
    if (dirfd == AT_FDCWD || (path != NULL && (path[0] == '/' || strchr(path, ':') != NULL)))
        return true;

    // Directory descriptors are not tracked, so a path relative to one
    // cannot be resolved.
    errno = ENOTSUP;
    return false;
}

int openat(int dirfd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    if (!at_path_supported(dirfd, path))
        return -1;
    return open(path, flags, mode);
}

int mkdirat(int dirfd, const char* path, mode_t mode)
{
    if (!at_path_supported(dirfd, path))
        return -1;
    return mkdir(path, mode);
}

int unlinkat(int dirfd, const char* path, int flags)
{
    if (!at_path_supported(dirfd, path))
        return -1;
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

// ---------------------------------------------------------------------------
// pipe(2)
// ---------------------------------------------------------------------------

// Horizon has no kernel pipes. The PAL uses them to wake threads within the
// process, so an in-process byte queue is enough. Both ends are newlib file
// descriptors on a "pipe" devoptab device, so read(), write() and close() reach
// them through the normal libsysbase dispatch without being overridden.

#define PIPE_BUFFER_SIZE 4096

typedef struct
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool readOpen;
    bool writeOpen;
    size_t head;
    size_t count;
    unsigned char data[PIPE_BUFFER_SIZE];
} pipe_state;

typedef struct
{
    pipe_state* pipe;
    bool isWriteEnd;
} pipe_end;

static int pipe_close_r(struct _reent* r, void* fd)
{
    (void)r;
    pipe_end* end = (pipe_end*)fd;
    pipe_state* p = end->pipe;

    pthread_mutex_lock(&p->lock);
    if (end->isWriteEnd)
        p->writeOpen = false;
    else
        p->readOpen = false;
    bool last = !p->readOpen && !p->writeOpen;
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);

    if (last)
    {
        pthread_cond_destroy(&p->changed);
        pthread_mutex_destroy(&p->lock);
        free(p);
    }
    return 0;
}

static ssize_t pipe_read_r(struct _reent* r, void* fd, char* ptr, size_t len)
{
    pipe_end* end = (pipe_end*)fd;
    if (end->isWriteEnd)
    {
        r->_errno = EBADF;
        return -1;
    }

    pipe_state* p = end->pipe;
    pthread_mutex_lock(&p->lock);
    while (p->count == 0 && p->writeOpen)
        pthread_cond_wait(&p->changed, &p->lock);

    size_t n = len < p->count ? len : p->count;
    for (size_t i = 0; i < n; i++)
        ptr[i] = (char)p->data[(p->head + i) % PIPE_BUFFER_SIZE];
    p->head = (p->head + n) % PIPE_BUFFER_SIZE;
    p->count -= n;

    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
    return (ssize_t)n; // 0 at end of file
}

static ssize_t pipe_write_r(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    pipe_end* end = (pipe_end*)fd;
    if (!end->isWriteEnd)
    {
        r->_errno = EBADF;
        return -1;
    }

    pipe_state* p = end->pipe;
    size_t written = 0;
    pthread_mutex_lock(&p->lock);
    while (written < len)
    {
        while (p->count == PIPE_BUFFER_SIZE && p->readOpen)
            pthread_cond_wait(&p->changed, &p->lock);

        if (!p->readOpen)
        {
            pthread_mutex_unlock(&p->lock);
            if (written != 0)
                return (ssize_t)written;
            r->_errno = EPIPE;
            return -1;
        }

        while (written < len && p->count < PIPE_BUFFER_SIZE)
        {
            p->data[(p->head + p->count) % PIPE_BUFFER_SIZE] = (unsigned char)ptr[written++];
            p->count++;
        }
        pthread_cond_broadcast(&p->changed);
    }
    pthread_mutex_unlock(&p->lock);
    return (ssize_t)written;
}

static const devoptab_t s_pipeDevice = {
    .name = "pipe",
    .structSize = sizeof(pipe_end),
    .close_r = pipe_close_r,
    .write_r = pipe_write_r,
    .read_r = pipe_read_r,
};

static pthread_once_t s_pipeDeviceOnce = PTHREAD_ONCE_INIT;
static int s_pipeDeviceId = -1;

static void register_pipe_device(void)
{
    s_pipeDeviceId = AddDevice(&s_pipeDevice);
}

static int alloc_pipe_end(pipe_state* p, bool isWriteEnd)
{
    int fd = __alloc_handle(s_pipeDeviceId);
    if (fd < 0)
        return -1;

    pipe_end* end = (pipe_end*)__get_handle(fd)->fileStruct;
    end->pipe = p;
    end->isWriteEnd = isWriteEnd;
    return fd;
}

int pipe(int fds[2])
{
    pthread_once(&s_pipeDeviceOnce, register_pipe_device);
    if (s_pipeDeviceId < 0)
    {
        errno = ENFILE;
        return -1;
    }

    pipe_state* p = (pipe_state*)calloc(1, sizeof(*p));
    if (p == NULL)
    {
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->changed, NULL);
    p->readOpen = true;
    p->writeOpen = true;

    int readFd = alloc_pipe_end(p, false);
    if (readFd < 0)
    {
        pthread_cond_destroy(&p->changed);
        pthread_mutex_destroy(&p->lock);
        free(p);
        errno = EMFILE;
        return -1;
    }

    int writeFd = alloc_pipe_end(p, true);
    if (writeFd < 0)
    {
        p->writeOpen = false;
        close(readFd); // last open end: frees the pipe
        errno = EMFILE;
        return -1;
    }

    fds[0] = readFd;
    fds[1] = writeFd;
    return 0;
}
