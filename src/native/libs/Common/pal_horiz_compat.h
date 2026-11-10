// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Adjustments to the libnx/libounce (Nintendo Switch / Switch 2 homebrew) headers. Included by
// pal_config.h for device builds only; the Linux-hosted horiz mirror uses the
// system headers.

#pragma once

#include <sys/socket.h>
#include <netinet/in.h>

// libnx/libounce passes the socket type to Horizon's BSD service unchanged, and the
// service rejects these flags. Horizon has no exec, so close-on-exec is moot.
#undef SOCK_CLOEXEC
#undef SOCK_NONBLOCK

// The BSD service does not support IPv6 (the runtime reports AF_INET6 as
// unsupported), but libnx/libounce's <netinet/in.h> lacks some of the declarations the
// generic socket code refers to. Values are FreeBSD's, like the other IPV6_
// options libnx/libounce defines.
#ifndef IPV6_RECVPKTINFO
#define IPV6_RECVPKTINFO 36
#endif
#ifndef IPV6_PKTINFO
#define IPV6_PKTINFO 46
#endif
#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT 47
#endif

struct in6_pktinfo
{
    struct in6_addr ipi6_addr;
    unsigned int ipi6_ifindex;
};

struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};

// libnx/libounce has no network interface enumeration
static inline unsigned int if_nametoindex(const char* ifname)
{
    (void)ifname;
    return 0;
}
