// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

#include "pal_config.h"
#include "pal_networking.h"

// A socket event port on top of poll(2), for systems that have neither epoll
// nor kqueue but whose poll(2) works on sockets (horiz).
#if !HAVE_EPOLL && !HAVE_KQUEUE && HAVE_SYS_POLL_H && defined(TARGET_HORIZ)
#define HAVE_POLL_SOCKET_EVENT_PORT 1
#else
#define HAVE_POLL_SOCKET_EVENT_PORT 0
#endif

#if HAVE_POLL_SOCKET_EVENT_PORT

int32_t PollSocketEventPort_Create(int32_t* port);

int32_t PollSocketEventPort_Close(int32_t port);

int32_t PollSocketEventPort_TryChangeRegistration(int32_t port, int32_t socket, SocketEvents newEvents, uintptr_t data);

int32_t PollSocketEventPort_Wait(int32_t port, SocketEvent* buffer, int32_t* count);

// Drops the registrations of a descriptor that is being closed. epoll and
// kqueue do this by themselves; a table of descriptors has to be told.
void PollSocketEventPort_CloseDescriptor(int32_t fd);

#endif // HAVE_POLL_SOCKET_EVENT_PORT
