// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

#include "pal_compiler.h"
#include "pal_types.h"

// TLS over Horizon's ssl service. The service runs TLS itself, on a socket
// that it is handed: SslStream gives it the socket of its NetworkStream.

typedef struct HorizSslConnection HorizSslConnection;

typedef enum
{
    HorizSslStatus_OK = 0,
    HorizSslStatus_WouldBlock = 1,
    HorizSslStatus_Error = 2,
} HorizSslStatus;

// Creates a connection on the socket. sslVersion is an SslVersion bitmask. The
// server certificate is verified against the system trust store, and its name
// against hostName when it is not empty. alpn is a list of {length, name}
// entries and may be empty. On failure, result is the service's Result.
PALEXPORT int32_t SystemNative_HorizSslCreate(int32_t socket,
                                              int32_t sslVersion,
                                              const char* hostName,
                                              const uint8_t* alpn,
                                              int32_t alpnLength,
                                              HorizSslConnection** connection,
                                              uint32_t* result);

PALEXPORT void SystemNative_HorizSslDestroy(HorizSslConnection* connection);

// The connection is non-blocking: these return HorizSslStatus_WouldBlock when the
// service waits for data from the peer, or for room to send.
PALEXPORT int32_t SystemNative_HorizSslHandshake(HorizSslConnection* connection, uint32_t* result);

// A read of 0 bytes means that the peer closed the connection.
PALEXPORT int32_t SystemNative_HorizSslRead(HorizSslConnection* connection, uint8_t* buffer, int32_t length, int32_t* read, uint32_t* result);

PALEXPORT int32_t SystemNative_HorizSslWrite(HorizSslConnection* connection, const uint8_t* buffer, int32_t length, int32_t* written, uint32_t* result);

// The negotiated cipher suite and protocol, as the service names them.
PALEXPORT int32_t SystemNative_HorizSslGetCipherInfo(HorizSslConnection* connection, char* cipher, int32_t cipherLength, char* protocol, int32_t protocolLength);

// The negotiated application protocol; length is 0 when there is none.
PALEXPORT int32_t SystemNative_HorizSslGetAlpnProtocol(HorizSslConnection* connection, uint8_t* buffer, int32_t bufferLength, int32_t* length);
