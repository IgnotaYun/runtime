// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx/libounce defines AF_UNIX, but Horizon has no UNIX domain sockets and libnx/libounce has
// no <sys/un.h>. Declare the address structure with its BSD layout so that
// code which handles socket addresses generically compiles.

#ifndef HORIZ_SYS_UN_H
#define HORIZ_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un
{
    unsigned char sun_len;
    sa_family_t   sun_family;
    char          sun_path[104];
};

#endif // HORIZ_SYS_UN_H
