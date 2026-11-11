// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "pal_config.h"
#include "pal_ssl_horiz.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch/result.h>
#include <switch/services/ssl.h>
// socket.h uses NifmRequest without including nifm.h
#include <switch/services/nifm.h>
#include <switch/runtime/devices/socket.h>

struct HorizSslConnection
{
    SslContext context;
    SslConnection connection;
    // The duplicate of the socket that the service hands back
    int serviceSocket;
};

#define HORIZ_SSL_MODULE 123
#define HORIZ_SSL_WOULD_BLOCK 204

static int32_t ToStatus(Result rc, uint32_t* result)
{
    *result = rc;

    if (R_SUCCEEDED(rc))
        return HorizSslStatus_OK;

    if (R_MODULE(rc) == HORIZ_SSL_MODULE && R_DESCRIPTION(rc) == HORIZ_SSL_WOULD_BLOCK)
        return HorizSslStatus_WouldBlock;

    return HorizSslStatus_Error;
}

int32_t SystemNative_HorizSslCreate(int32_t socket,
                                    int32_t sslVersion,
                                    const char* hostName,
                                    const uint8_t* alpn,
                                    int32_t alpnLength,
                                    HorizSslConnection** connection,
                                    uint32_t* result)
{
    *connection = NULL;

    // The service is reference counted, every connection holds it
    Result rc = sslInitialize(3);
    if (R_FAILED(rc))
        return ToStatus(rc, result);

    HorizSslConnection* c = (HorizSslConnection*)calloc(1, sizeof(HorizSslConnection));
    if (c == NULL)
    {
        sslExit();
        return ToStatus(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory), result);
    }

    c->serviceSocket = -1;

    rc = sslCreateContext(&c->context, (u32)sslVersion);
    if (R_FAILED(rc))
    {
        free(c);
        sslExit();
        return ToStatus(rc, result);
    }

    rc = sslContextCreateConnection(&c->context, &c->connection);
    if (R_FAILED(rc))
    {
        sslContextClose(&c->context);
        free(c);
        sslExit();
        return ToStatus(rc, result);
    }

    // The socket stays the caller's: it is still waited on, and closed, through
    // the caller's descriptor
    rc = sslConnectionSetOption(&c->connection, SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(rc))
    {
        SystemNative_HorizSslDestroy(c);
        return ToStatus(rc, result);
    }

    int serviceSocket = socketSslConnectionSetSocketDescriptor(&c->connection, socket);
    if (serviceSocket >= 0)
    {
        c->serviceSocket = serviceSocket;
    }
    else if (errno != ENOENT)
    {
        rc = socketGetLastResult();
        SystemNative_HorizSslDestroy(c);
        return ToStatus(R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_IoError), result);
    }

    size_t hostNameLength = hostName != NULL ? strlen(hostName) : 0;
    if (hostNameLength != 0)
    {
        rc = sslConnectionSetHostName(&c->connection, hostName, (u32)hostNameLength);
    }
    else
    {
        // Without a name only the chain and the dates can be verified, which
        // the service refuses unless the default verification is skipped. The
        // option does not exist before 5.0.0, which has no such requirement.
        sslConnectionSetOption(&c->connection, SslOptionType_SkipDefaultVerify, true);
    }

    if (R_SUCCEEDED(rc))
    {
        u32 verify = SslVerifyOption_PeerCa | SslVerifyOption_DateCheck;
        if (hostNameLength != 0)
            verify |= SslVerifyOption_HostName;

        rc = sslConnectionSetVerifyOption(&c->connection, verify);
    }

    if (R_SUCCEEDED(rc))
        rc = sslConnectionSetIoMode(&c->connection, SslIoMode_NonBlocking);

    if (R_SUCCEEDED(rc) && alpnLength > 0)
    {
        rc = sslConnectionSetOption(&c->connection, SslOptionType_EnableAlpn, true);
        if (R_SUCCEEDED(rc))
            rc = sslConnectionSetNextAlpnProto(&c->connection, alpn, (u32)alpnLength);
    }

    if (R_FAILED(rc))
    {
        SystemNative_HorizSslDestroy(c);
        return ToStatus(rc, result);
    }

    *connection = c;
    return ToStatus(0, result);
}

void SystemNative_HorizSslDestroy(HorizSslConnection* connection)
{
    if (connection == NULL)
        return;

    // The service requires its duplicate to be closed first
    if (connection->serviceSocket >= 0)
        close(connection->serviceSocket);

    sslConnectionClose(&connection->connection);
    sslContextClose(&connection->context);
    free(connection);
    sslExit();
}

int32_t SystemNative_HorizSslHandshake(HorizSslConnection* connection, uint32_t* result)
{
    return ToStatus(sslConnectionDoHandshake(&connection->connection, NULL, NULL, NULL, 0), result);
}

int32_t SystemNative_HorizSslRead(HorizSslConnection* connection, uint8_t* buffer, int32_t length, int32_t* read, uint32_t* result)
{
    u32 size = 0;
    int32_t status = ToStatus(sslConnectionRead(&connection->connection, buffer, (u32)length, &size), result);
    *read = status == HorizSslStatus_OK ? (int32_t)size : 0;
    return status;
}

int32_t SystemNative_HorizSslWrite(HorizSslConnection* connection, const uint8_t* buffer, int32_t length, int32_t* written, uint32_t* result)
{
    u32 size = 0;
    int32_t status = ToStatus(sslConnectionWrite(&connection->connection, buffer, (u32)length, &size), result);
    *written = status == HorizSslStatus_OK ? (int32_t)size : 0;
    return status;
}

int32_t SystemNative_HorizSslGetCipherInfo(HorizSslConnection* connection, char* cipher, int32_t cipherLength, char* protocol, int32_t protocolLength)
{
    SslCipherInfo info;
    memset(&info, 0, sizeof(info));

    // Not every version of the service reports it
    if (cipherLength <= 0 || protocolLength <= 0 || R_FAILED(sslConnectionGetCipherInfo(&connection->connection, &info)))
        return 0;

    strncpy(cipher, info.cipher, (size_t)cipherLength - 1);
    cipher[cipherLength - 1] = '\0';
    strncpy(protocol, info.protocol_version, (size_t)protocolLength - 1);
    protocol[protocolLength - 1] = '\0';
    return 1;
}

int32_t SystemNative_HorizSslGetAlpnProtocol(HorizSslConnection* connection, uint8_t* buffer, int32_t bufferLength, int32_t* length)
{
    SslAlpnProtoState state = SslAlpnProtoState_NoSupport;
    u32 size = 0;

    *length = 0;
    if (R_FAILED(sslConnectionGetNextAlpnProto(&connection->connection, &state, &size, buffer, (u32)bufferLength)))
        return 0;

    if ((state == SslAlpnProtoState_Negotiated || state == SslAlpnProtoState_Selected) && size <= (u32)bufferLength)
        *length = (int32_t)size;

    return 1;
}
