// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// libnx/libounce defines struct iovec for its socket API, but newlib has no <sys/uio.h>.
// Neither provides readv or writev.

#ifndef HORIZ_SYS_UIO_H
#define HORIZ_SYS_UIO_H

#include <sys/_iovec.h>

#endif // HORIZ_SYS_UIO_H
