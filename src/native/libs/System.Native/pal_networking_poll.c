// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "pal_networking_poll.h"

#if HAVE_POLL_SOCKET_EVENT_PORT

#include "pal_errno.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

//
// The managed SocketAsyncEngine is written against edge-triggered epoll and
// kqueue: it is told once that a socket became readable or writable, and is
// told again only after that readiness went away and came back. poll(2) is
// level triggered, so the port remembers what it reported for each socket
// and leaves those events out of the following polls.
//
// The port cannot see a socket's readiness drop and come back between two
// polls, which is what wakes an operation that ran into EAGAIN. Reported
// events are therefore re-armed:
//
//  * Read and error events PollReadRearmMs after they were reported. A
//    socket that was read until EAGAIN polls as not readable, so re-arming
//    these is cheap and only reports data that actually arrived.
//  * Write events when a poll with no timeout finds the socket not writable
//    (its send buffer filled up), or PollWriteRepeatMs after they were
//    reported in case the buffer filled up and drained between two samples.
//    Connected sockets are writable most of the time, so they are not
//    re-reported on every round.
//
// A registration is replaced when its descriptor is closed and the number is
// reused. Every registration has a generation, and results that a wait
// collected for a previous registration are dropped.
//

#define PollPortBase       0x40000000 // Not a descriptor: indexes s_pollPorts
#define PollPortCount      32         // One port per SocketAsyncEngine
#define PollPortMaxEntries 256        // Sockets registered with one port
#define PollWaitSliceMs    10         // Refreshes the registrations while waiting
#define PollReadRearmMs    10
#define PollWriteRepeatMs  250

static const int32_t ReadEvents = SocketEvents_SA_READ | SocketEvents_SA_READCLOSE | SocketEvents_SA_CLOSE | SocketEvents_SA_ERROR;

typedef struct
{
    int fd;
    int32_t interest;  // SocketEvents the managed side registered
    int32_t reported;  // SocketEvents reported and not re-armed yet
    uintptr_t data;
    uint64_t generation;
    int64_t readRearmMs;
    int64_t writeRearmMs;
} PollPortEntry;

typedef struct
{
    bool inUse;
    int count;
    uint64_t nextGeneration;
    PollPortEntry entries[PollPortMaxEntries];
} PollPort;

static PollPort s_pollPorts[PollPortCount];
static pthread_mutex_t s_pollPortsLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_pollPortsCond;
static pthread_once_t s_pollPortsOnce = PTHREAD_ONCE_INIT;
#if HAVE_PTHREAD_CONDATTR_SETCLOCK && HAVE_CLOCK_MONOTONIC
static bool s_pollPortsCondMonotonic;
#endif

static void InitPollPortsCond(void)
{
#if HAVE_PTHREAD_CONDATTR_SETCLOCK && HAVE_CLOCK_MONOTONIC
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) == 0)
    {
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
            pthread_cond_init(&s_pollPortsCond, &attr) == 0)
        {
            s_pollPortsCondMonotonic = true;
            pthread_condattr_destroy(&attr);
            return;
        }
        pthread_condattr_destroy(&attr);
    }
#endif
    int rv = pthread_cond_init(&s_pollPortsCond, NULL);
    assert(rv == 0);
    (void)rv;
}

static int64_t GetMilliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static PollPort* GetPollPort(int32_t port)
{
    int index = port - PollPortBase;
    if (index < 0 || index >= PollPortCount || !s_pollPorts[index].inUse)
    {
        return NULL;
    }

    return &s_pollPorts[index];
}

static int FindEntry(PollPort* port, int fd)
{
    for (int i = 0; i < port->count; i++)
    {
        if (port->entries[i].fd == fd)
        {
            return i;
        }
    }

    return -1;
}

static void RemoveEntry(PollPort* port, int index)
{
    port->entries[index] = port->entries[port->count - 1];
    port->count--;
}

static short GetPollEvents(int32_t socketEvents)
{
    short pollEvents = 0;
    if ((socketEvents & SocketEvents_SA_READ) != 0)
    {
        pollEvents |= POLLIN;
    }
    if ((socketEvents & SocketEvents_SA_WRITE) != 0)
    {
        pollEvents |= POLLOUT;
    }

    return pollEvents;
}

static int32_t GetSocketEvents(short pollEvents)
{
    int32_t socketEvents = 0;
    if ((pollEvents & POLLIN) != 0)
    {
        socketEvents |= SocketEvents_SA_READ;
    }
    if ((pollEvents & POLLOUT) != 0)
    {
        socketEvents |= SocketEvents_SA_WRITE;
    }
    if ((pollEvents & ~(POLLIN | POLLOUT)) != 0)
    {
        // POLLERR, POLLHUP and anything else poll(2) reports without being asked
        // for: report the socket as closed, the managed side picks up the error.
        socketEvents |= SocketEvents_SA_CLOSE | SocketEvents_SA_ERROR;
    }

    return socketEvents;
}

// Drops descriptors that no longer exist. libnx/libounce's poll(2) fails as a whole
// with EBADF if any descriptor is gone.
static void PruneClosedDescriptors(PollPort* port)
{
    for (int i = 0; i < port->count; )
    {
        struct pollfd probe = { .fd = port->entries[i].fd, .events = 0, .revents = 0 };
        int rv;
        while ((rv = poll(&probe, 1, 0)) < 0 && errno == EINTR);
        if (rv < 0 || (probe.revents & POLLNVAL) != 0)
        {
            RemoveEntry(port, i);
            continue;
        }

        i++;
    }
}

// Re-arms the reported events that are due, and returns the time until the
// next entry is due (at most PollWaitSliceMs). Called with the lock held.
static int RearmReportedEvents(PollPort* port, int64_t now, bool* hasPendingRearm)
{
    struct pollfd samples[PollPortMaxEntries];
    int sampleEntries[PollPortMaxEntries];
    int sampleCount = 0;
    int64_t nextRearmDelta = INT64_MAX;

    for (int i = 0; i < port->count; i++)
    {
        PollPortEntry* entry = &port->entries[i];

        if ((entry->reported & ReadEvents) != 0)
        {
            if (now >= entry->readRearmMs)
            {
                entry->reported &= ~ReadEvents;
            }
            else
            {
                int64_t delta = entry->readRearmMs - now;
                if (delta < nextRearmDelta)
                {
                    nextRearmDelta = delta;
                }
            }
        }

        if ((entry->reported & SocketEvents_SA_WRITE) != 0)
        {
            if (now >= entry->writeRearmMs)
            {
                entry->reported &= ~SocketEvents_SA_WRITE;
            }
            else
            {
                samples[sampleCount].fd = entry->fd;
                samples[sampleCount].events = POLLOUT;
                samples[sampleCount].revents = 0;
                sampleEntries[sampleCount] = i;
                sampleCount++;
            }
        }
    }

    if (sampleCount > 0)
    {
        int rv;
        while ((rv = poll(samples, (nfds_t)sampleCount, 0)) < 0 && errno == EINTR);
        if (rv >= 0)
        {
            for (int s = 0; s < sampleCount; s++)
            {
                PollPortEntry* entry = &port->entries[sampleEntries[s]];
                if ((samples[s].revents & POLLOUT) == 0)
                {
                    // The send buffer filled up: the next writability is new
                    entry->reported &= ~SocketEvents_SA_WRITE;
                }
                else
                {
                    int64_t delta = entry->writeRearmMs - now;
                    if (delta < nextRearmDelta)
                    {
                        nextRearmDelta = delta;
                    }
                }
            }
        }
        else
        {
            for (int s = 0; s < sampleCount; s++)
            {
                int64_t delta = port->entries[sampleEntries[s]].writeRearmMs - now;
                if (delta < nextRearmDelta)
                {
                    nextRearmDelta = delta;
                }
            }
        }
    }

    *hasPendingRearm = (nextRearmDelta != INT64_MAX);
    int64_t timeout = PollWaitSliceMs;
    if (nextRearmDelta < timeout)
    {
        timeout = nextRearmDelta;
    }
    if (timeout < 0)
    {
        timeout = 0;
    }

    return (int)timeout;
}

int32_t PollSocketEventPort_Create(int32_t* port)
{
    assert(port != NULL);

    pthread_once(&s_pollPortsOnce, InitPollPortsCond);
    pthread_mutex_lock(&s_pollPortsLock);
    for (int i = 0; i < PollPortCount; i++)
    {
        if (!s_pollPorts[i].inUse)
        {
            s_pollPorts[i].inUse = true;
            s_pollPorts[i].count = 0;
            s_pollPorts[i].nextGeneration = 0;
            pthread_mutex_unlock(&s_pollPortsLock);
            *port = PollPortBase + i;
            return Error_SUCCESS;
        }
    }
    pthread_mutex_unlock(&s_pollPortsLock);

    *port = -1;
    return Error_EMFILE;
}

int32_t PollSocketEventPort_Close(int32_t port)
{
    pthread_mutex_lock(&s_pollPortsLock);
    PollPort* pollPort = GetPollPort(port);
    if (pollPort != NULL)
    {
        pollPort->inUse = false;
        pollPort->count = 0;
        pthread_cond_broadcast(&s_pollPortsCond);
    }
    pthread_mutex_unlock(&s_pollPortsLock);

    return pollPort != NULL ? Error_SUCCESS : Error_EBADF;
}

int32_t PollSocketEventPort_TryChangeRegistration(int32_t port, int32_t socket, SocketEvents newEvents, uintptr_t data)
{
    int32_t result = Error_SUCCESS;

    pthread_mutex_lock(&s_pollPortsLock);
    PollPort* pollPort = GetPollPort(port);
    if (pollPort == NULL)
    {
        pthread_mutex_unlock(&s_pollPortsLock);
        return Error_EBADF;
    }

    int index = FindEntry(pollPort, socket);
    if (newEvents == SocketEvents_SA_NONE)
    {
        if (index >= 0)
        {
            RemoveEntry(pollPort, index);
            pthread_cond_broadcast(&s_pollPortsCond);
        }
    }
    else
    {
        if (index < 0 && pollPort->count < PollPortMaxEntries)
        {
            index = pollPort->count++;
        }

        if (index >= 0)
        {
            PollPortEntry* entry = &pollPort->entries[index];
            entry->fd = socket;
            entry->interest = (int32_t)newEvents;
            entry->reported = 0;
            entry->data = data;
            entry->generation = ++pollPort->nextGeneration;
            pthread_cond_broadcast(&s_pollPortsCond);
        }
        else
        {
            result = Error_ENOMEM;
        }
    }

    pthread_mutex_unlock(&s_pollPortsLock);
    return result;
}

void PollSocketEventPort_CloseDescriptor(int32_t fd)
{
    bool removed = false;
    pthread_mutex_lock(&s_pollPortsLock);
    for (int p = 0; p < PollPortCount; p++)
    {
        PollPort* pollPort = &s_pollPorts[p];
        if (pollPort->inUse)
        {
            int index = FindEntry(pollPort, fd);
            if (index >= 0)
            {
                RemoveEntry(pollPort, index);
                removed = true;
            }
        }
    }
    if (removed)
    {
        pthread_cond_broadcast(&s_pollPortsCond);
    }
    pthread_mutex_unlock(&s_pollPortsLock);
}

int32_t PollSocketEventPort_Wait(int32_t port, SocketEvent* buffer, int32_t* count)
{
    assert(buffer != NULL);
    assert(count != NULL);
    assert(*count >= 0);

    struct pollfd fds[PollPortMaxEntries];
    uint64_t fdGenerations[PollPortMaxEntries];

    for (;;)
    {
        int fdCount = 0;
        int timeout;
        bool hasPendingRearm = false;

        pthread_mutex_lock(&s_pollPortsLock);
        PollPort* pollPort = GetPollPort(port);
        if (pollPort == NULL)
        {
            pthread_mutex_unlock(&s_pollPortsLock);
            *count = 0;
            return Error_EBADF;
        }

        timeout = RearmReportedEvents(pollPort, GetMilliseconds(), &hasPendingRearm);

        for (int i = 0; i < pollPort->count; i++)
        {
            PollPortEntry* entry = &pollPort->entries[i];
            int32_t pending = entry->interest & ~entry->reported;
            if ((entry->reported & SocketEvents_SA_ERROR) != 0 || GetPollEvents(pending) == 0)
            {
                // Nothing left to report until the entry is re-armed. poll(2)
                // reports errors and hang-ups without being asked, so leave it
                // out instead of asking for no events.
                continue;
            }

            fds[fdCount].fd = entry->fd;
            fds[fdCount].events = GetPollEvents(pending);
            fds[fdCount].revents = 0;
            fdGenerations[fdCount] = entry->generation;
            fdCount++;
        }

        if (fdCount == 0)
        {
            if (!hasPendingRearm)
            {
                pthread_cond_wait(&s_pollPortsCond, &s_pollPortsLock);
            }
            else if (timeout > 0)
            {
                struct timespec ts;
#if HAVE_PTHREAD_CONDATTR_SETCLOCK && HAVE_CLOCK_MONOTONIC
                clock_gettime(s_pollPortsCondMonotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
#else
                clock_gettime(CLOCK_REALTIME, &ts);
#endif
                ts.tv_sec += timeout / 1000;
                ts.tv_nsec += (long)(timeout % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L)
                {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&s_pollPortsCond, &s_pollPortsLock, &ts);
            }
            pthread_mutex_unlock(&s_pollPortsLock);
            continue;
        }
        pthread_mutex_unlock(&s_pollPortsLock);

        int rv;
        while ((rv = poll(fds, (nfds_t)fdCount, timeout)) < 0 && errno == EINTR);
        if (rv < 0)
        {
            if (errno != EBADF && errno != EINVAL)
            {
                *count = 0;
                return SystemNative_ConvertErrorPlatformToPal(errno);
            }

            pthread_mutex_lock(&s_pollPortsLock);
            pollPort = GetPollPort(port);
            if (pollPort != NULL)
            {
                PruneClosedDescriptors(pollPort);
            }
            pthread_mutex_unlock(&s_pollPortsLock);
            continue;
        }

        if (rv == 0)
        {
            continue;
        }

        int32_t produced = 0;
        int64_t now = GetMilliseconds();

        pthread_mutex_lock(&s_pollPortsLock);
        pollPort = GetPollPort(port);
        if (pollPort == NULL)
        {
            pthread_mutex_unlock(&s_pollPortsLock);
            *count = 0;
            return Error_EBADF;
        }

        for (int f = 0; f < fdCount && produced < *count; f++)
        {
            if (fds[f].revents == 0)
            {
                continue;
            }

            // The registrations may have changed while the wait was in flight
            int index = FindEntry(pollPort, fds[f].fd);
            if (index < 0 || pollPort->entries[index].generation != fdGenerations[f])
            {
                continue;
            }

            PollPortEntry* entry = &pollPort->entries[index];
            if ((fds[f].revents & POLLNVAL) != 0)
            {
                RemoveEntry(pollPort, index);
                continue;
            }

            int32_t ready = GetSocketEvents(fds[f].revents);
            if ((ready & SocketEvents_SA_ERROR) != 0)
            {
                // Persistent condition: report everything once, then wait for the re-arm
                ready |= entry->interest;
            }
            else
            {
                ready &= entry->interest & ~entry->reported;
            }

            if (ready == 0)
            {
                continue;
            }

            buffer[produced].Data = entry->data;
            buffer[produced].Events = (SocketEvents)ready;
            produced++;

            entry->reported |= ready;
            if ((ready & ReadEvents) != 0)
            {
                entry->readRearmMs = now + PollReadRearmMs;
            }
            if ((ready & SocketEvents_SA_WRITE) != 0)
            {
                entry->writeRearmMs = now + PollWriteRepeatMs;
            }
        }
        pthread_mutex_unlock(&s_pollPortsLock);

        if (produced > 0)
        {
            *count = produced;
            return Error_SUCCESS;
        }
    }
}

#endif // HAVE_POLL_SOCKET_EVENT_PORT
