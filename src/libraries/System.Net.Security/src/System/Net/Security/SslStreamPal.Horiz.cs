// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.ComponentModel;
using System.Security.Authentication.ExtendedProtection;
using System.Threading;
using System.Threading.Tasks;

namespace System.Net.Security
{
    // Horizon's ssl service runs the whole connection (see SafeDeleteSslContext),
    // so SslStream hands the handshake, reads and writes over to the context and
    // never asks for single records to be processed. The service only implements
    // the client side, and verifies the server's certificate itself.
    internal static class SslStreamPal
    {
        public static Exception GetException(SecurityStatusPal status)
        {
            return status.Exception ?? new Win32Exception((int)status.ErrorCode);
        }

        internal const bool StartMutualAuthAsAnonymous = false;
        internal const bool CanEncryptEmptyMessage = false;
        internal const bool CanGenerateCustomAlerts = false;

        public static void VerifyPackageInfo()
        {
        }

        internal static bool ShouldUseAsyncSecurityContext(SslAuthenticationOptions sslAuthenticationOptions)
        {
            return sslAuthenticationOptions.IsClient;
        }

        public static bool IsAsyncSecurityContext(SafeDeleteSslContext _)
        {
            return true;
        }

        internal static Task<Exception?> AsyncHandshakeAsync(ref SafeDeleteSslContext? context, SslStream stream, CancellationToken cancellationToken)
        {
            try
            {
                context = new SafeDeleteSslContext(stream);
                return context.HandshakeAsync(cancellationToken);
            }
            catch (Exception e)
            {
                return Task.FromResult<Exception?>(e);
            }
        }

        internal static ValueTask<int> AsyncReadAsync(SafeDeleteSslContext securityContext, Memory<byte> buffer, CancellationToken cancellationToken)
        {
            return securityContext.ReadAsync(buffer, cancellationToken);
        }

        internal static Task AsyncWriteAsync(SafeDeleteSslContext securityContext, ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken)
        {
            return securityContext.WriteAsync(buffer, cancellationToken);
        }

        public static SecurityStatusPal SelectApplicationProtocol(
            SafeFreeCredentials? credentialsHandle,
            SafeDeleteSslContext? context,
            SslAuthenticationOptions sslAuthenticationOptions,
            ReadOnlySpan<byte> clientProtocols)
        {
            throw new PlatformNotSupportedException(nameof(SelectApplicationProtocol));
        }

        public static ProtocolToken AcceptSecurityContext(
            ref SafeFreeCredentials? credential,
            ref SafeDeleteSslContext? context,
            ReadOnlySpan<byte> inputBuffer,
            out int consumed,
            SslAuthenticationOptions sslAuthenticationOptions)
        {
            throw new PlatformNotSupportedException(SR.net_nego_server_not_supported);
        }

        public static ProtocolToken InitializeSecurityContext(
            ref SafeFreeCredentials? credential,
            ref SafeDeleteSslContext? context,
            string? targetName,
            ReadOnlySpan<byte> inputBuffer,
            out int consumed,
            SslAuthenticationOptions sslAuthenticationOptions)
        {
            // Clients always run on the service
            throw new PlatformNotSupportedException();
        }

        public static ProtocolToken Renegotiate(
            ref SafeFreeCredentials? credentialsHandle,
            ref SafeDeleteSslContext? context,
            SslAuthenticationOptions sslAuthenticationOptions)
        {
            throw new PlatformNotSupportedException();
        }

        public static SafeFreeCredentials? AcquireCredentialsHandle(SslAuthenticationOptions _1, bool _2)
        {
            return null;
        }

        public static ProtocolToken EncryptMessage(SafeDeleteSslContext securityContext, ReadOnlyMemory<byte> input, int headerSize, int trailerSize)
        {
            throw new PlatformNotSupportedException();
        }

        public static SecurityStatusPal DecryptMessage(SafeDeleteSslContext securityContext, Span<byte> buffer, out int offset, out int count)
        {
            throw new PlatformNotSupportedException();
        }

        public static ChannelBinding? QueryContextChannelBinding(SafeDeleteContext securityContext, ChannelBindingKind attribute)
        {
            // The service exposes neither the certificate nor the Finished messages
            return null;
        }

        public static void QueryContextStreamSizes(SafeDeleteContext? securityContext, out StreamSizes streamSizes)
        {
            streamSizes = StreamSizes.Default;
        }

        public static void QueryContextConnectionInfo(SafeDeleteSslContext securityContext, ref SslConnectionInfo connectionInfo)
        {
            connectionInfo.UpdateSslConnectionInfo(securityContext);
        }

        public static bool TryUpdateClintCertificate(SafeFreeCredentials? _1, SafeDeleteSslContext? _2, SslAuthenticationOptions _3)
        {
            return false;
        }

        public static SecurityStatusPal ApplyAlertToken(SafeDeleteContext? securityContext, TlsAlertType alertType, TlsAlertMessage alertMessage)
        {
            // The service sends its own alerts
            return new SecurityStatusPal(SecurityStatusPalErrorCode.OK);
        }

        public static SecurityStatusPal ApplyShutdownToken(SafeDeleteSslContext securityContext)
        {
            // The service sends close_notify when the connection is closed
            return new SecurityStatusPal(SecurityStatusPalErrorCode.OK);
        }
    }
}
