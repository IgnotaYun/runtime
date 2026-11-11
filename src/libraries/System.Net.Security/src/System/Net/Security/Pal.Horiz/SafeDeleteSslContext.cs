// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.IO;
using System.Net.Security;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

using HorizSslStatus = Interop.Sys.HorizSslStatus;

namespace System.Net
{
    // TLS over Horizon's ssl service. The service runs TLS itself on a socket
    // that it is handed, and cannot work on buffers, so the SslStream's inner
    // stream has to be a NetworkStream. The service reads and writes the socket
    // from then on; SslStream must not use the inner stream itself.
    //
    // Calls into the service do not block. When the service waits for the peer,
    // the context waits for the socket to become readable.
    internal sealed class SafeDeleteSslContext : SafeDeleteContext
    {
        // A TLS record holds at most 16 KB of data
        private const int MaxRecordSize = 16 * 1024;

        private readonly Socket _socket;
        private readonly SafeHorizSslHandle _connection;

        // The protocols that the connection may use, None for the system's choice
        private readonly SslProtocols _enabledProtocols;

        // The highest protocol that the connection may use
        internal SslProtocols MaxProtocol { get; }

        public SafeDeleteSslContext(SslStream stream)
            : base(IntPtr.Zero)
        {
            SslAuthenticationOptions options = stream._sslAuthenticationOptions;

            if (options.EncryptionPolicy != EncryptionPolicy.RequireEncryption)
            {
                throw new PlatformNotSupportedException(SR.Format(SR.net_encryptionpolicy_notsupported, options.EncryptionPolicy));
            }

            if (stream.InnerStream is not NetworkStream networkStream)
            {
                throw new PlatformNotSupportedException(SR.net_ssl_horiz_networkstream_required);
            }

            int sslVersion = GetSslVersion(options.EnabledSslProtocols, out SslProtocols maxProtocol);
            MaxProtocol = maxProtocol;
            _enabledProtocols = options.EnabledSslProtocols;

            _socket = networkStream.Socket;
            _connection = Create((int)_socket.Handle, sslVersion, options.TargetHost, GetAlpnList(options.ApplicationProtocols));
        }

        public override bool IsInvalid => _connection?.IsInvalid ?? true;

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                // The socket belongs to the inner stream
                _connection?.Dispose();
            }

            base.Dispose(disposing);
        }

        internal async Task<Exception?> HandshakeAsync(CancellationToken cancellationToken)
        {
            try
            {
                while (true)
                {
                    HorizSslStatus status = Interop.Sys.HorizSslHandshake(_connection, out uint result);
                    if (status == HorizSslStatus.OK)
                    {
                        return CheckProtocol();
                    }

                    if (status == HorizSslStatus.Error)
                    {
                        return CreateException(result);
                    }

                    await WaitForPeerAsync(cancellationToken).ConfigureAwait(false);
                }
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                return e;
            }
        }

        internal async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            if (buffer.Length == 0)
            {
                // Only wait for data to arrive
                await WaitForPeerAsync(cancellationToken).ConfigureAwait(false);
                return 0;
            }

            while (true)
            {
                // A read of 0 bytes means that the peer closed the connection
                HorizSslStatus status = Read(buffer.Span, out int read, out uint result);
                if (status == HorizSslStatus.OK)
                {
                    return read;
                }

                if (status == HorizSslStatus.Error)
                {
                    throw new IOException(SR.net_io_decrypt, CreateException(result));
                }

                await WaitForPeerAsync(cancellationToken).ConfigureAwait(false);
            }
        }

        internal async Task WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken)
        {
            while (buffer.Length > 0)
            {
                HorizSslStatus status = Write(buffer.Span, out int written, out uint result);
                if (status == HorizSslStatus.Error)
                {
                    throw new IOException(SR.net_io_encrypt, CreateException(result));
                }

                if (status == HorizSslStatus.OK)
                {
                    buffer = buffer.Slice(written);
                }
                else
                {
                    // The socket's send buffer is full. Unlike ReceiveAsync(Empty) (which
                    // peeks 1 byte), SendAsync(Empty) issues a 0-byte send(2) that succeeds
                    // immediately even when the send buffer is full, so back off briefly.
                    await Task.Delay(1, cancellationToken).ConfigureAwait(false);
                }
            }
        }

        // The service takes the versions as a hint: asked for TLS 1.3 only, it
        // was seen to settle on TLS 1.2. Refuse a protocol that was not enabled.
        private AuthenticationException? CheckProtocol()
        {
            if (_enabledProtocols == SslProtocols.None ||
                !GetCipherInfo(out _, out string? version))
            {
                return null;
            }

            SslProtocols protocol = ParseProtocol(version);
            if (protocol == SslProtocols.None || (protocol & _enabledProtocols) != 0)
            {
                return null;
            }

            return new AuthenticationException(SR.Format(SR.net_ssl_horiz_protocol_not_enabled, protocol, _enabledProtocols));
        }

        // The protocol as the service names it
        internal static SslProtocols ParseProtocol(string? version) => version switch
        {
#pragma warning disable SYSLIB0039 // TLS 1.0 and 1.1 are obsolete
            "TLSv1" => SslProtocols.Tls,
            "TLSv1.1" => SslProtocols.Tls11,
#pragma warning restore SYSLIB0039
            "TLSv1.2" => SslProtocols.Tls12,
            "TLSv1.3" => SslProtocols.Tls13,
            _ => SslProtocols.None,
        };

        internal unsafe bool GetCipherInfo(out string? cipher, out string? protocol)
        {
            byte* cipherBuffer = stackalloc byte[64];
            byte* protocolBuffer = stackalloc byte[16];

            if (!Interop.Sys.HorizSslGetCipherInfo(_connection, cipherBuffer, 64, protocolBuffer, 16))
            {
                cipher = null;
                protocol = null;
                return false;
            }

            cipher = Marshal.PtrToStringUTF8((IntPtr)cipherBuffer);
            protocol = Marshal.PtrToStringUTF8((IntPtr)protocolBuffer);
            return true;
        }

        internal unsafe byte[]? GetApplicationProtocol()
        {
            byte* buffer = stackalloc byte[256];
            if (!Interop.Sys.HorizSslGetAlpnProtocol(_connection, buffer, 256, out int length) || length == 0)
                return null;

            return new ReadOnlySpan<byte>(buffer, length).ToArray();
        }

        private static unsafe SafeHorizSslHandle Create(int socket, int sslVersion, string? hostName, byte[]? alpn)
        {
            fixed (byte* alpnBuffer = alpn)
            {
                HorizSslStatus status = Interop.Sys.HorizSslCreate(
                    socket,
                    sslVersion,
                    hostName,
                    alpnBuffer,
                    alpn?.Length ?? 0,
                    out SafeHorizSslHandle connection,
                    out uint result);

                if (status != HorizSslStatus.OK)
                {
                    connection.Dispose();
                    throw CreateException(result);
                }

                return connection;
            }
        }

        private unsafe HorizSslStatus Read(Span<byte> buffer, out int read, out uint result)
        {
            fixed (byte* p = buffer)
            {
                return Interop.Sys.HorizSslRead(_connection, p, Math.Min(buffer.Length, MaxRecordSize), out read, out result);
            }
        }

        private unsafe HorizSslStatus Write(ReadOnlySpan<byte> buffer, out int written, out uint result)
        {
            fixed (byte* p = buffer)
            {
                return Interop.Sys.HorizSslWrite(_connection, p, Math.Min(buffer.Length, MaxRecordSize), out written, out result);
            }
        }

        // Completes when the socket has data, or reached its end
        private async ValueTask WaitForPeerAsync(CancellationToken cancellationToken)
        {
            await _socket.ReceiveAsync(Memory<byte>.Empty, SocketFlags.None, cancellationToken).ConfigureAwait(false);
        }

        private static int GetSslVersion(SslProtocols protocols, out SslProtocols maxProtocol)
        {
            // SslVersion in libnx/libounce's ssl.h
            const int Auto = 1 << 0;
            const int TlsV10 = 1 << 3;
            const int TlsV11 = 1 << 4;
            const int TlsV12 = 1 << 5;
            const int TlsV13 = 1 << 6;

            if (protocols == SslProtocols.None)
            {
                // Auto is TLS 1.0 to 1.2
                maxProtocol = SslProtocols.Tls12;
                return Auto;
            }

            int version = 0;
#pragma warning disable SYSLIB0039 // TLS 1.0 and 1.1 are obsolete
            if ((protocols & SslProtocols.Tls) != 0)
                version |= TlsV10;
            if ((protocols & SslProtocols.Tls11) != 0)
                version |= TlsV11;
#pragma warning restore SYSLIB0039
            if ((protocols & SslProtocols.Tls12) != 0)
                version |= TlsV12;
            if ((protocols & SslProtocols.Tls13) != 0)
                version |= TlsV13;

            if (version == 0)
                throw new PlatformNotSupportedException(SR.Format(SR.net_security_sslprotocol_notsupported, protocols));

            maxProtocol =
                (version & TlsV13) != 0 ? SslProtocols.Tls13 :
                (version & TlsV12) != 0 ? SslProtocols.Tls12 :
#pragma warning disable SYSLIB0039 // TLS 1.0 and 1.1 are obsolete
                (version & TlsV11) != 0 ? SslProtocols.Tls11 :
                SslProtocols.Tls;
#pragma warning restore SYSLIB0039

            return version;
        }

        // The protocols as {length, name} entries
        private static byte[]? GetAlpnList(List<SslApplicationProtocol>? protocols)
        {
            if (protocols == null || protocols.Count == 0)
                return null;

            int length = 0;
            foreach (SslApplicationProtocol protocol in protocols)
            {
                length += 1 + protocol.Protocol.Length;
            }

            byte[] list = new byte[length];
            int offset = 0;
            foreach (SslApplicationProtocol protocol in protocols)
            {
                list[offset++] = (byte)protocol.Protocol.Length;
                protocol.Protocol.Span.CopyTo(list.AsSpan(offset));
                offset += protocol.Protocol.Length;
            }

            return list;
        }

        // Results are shown as module and description, the way the system reports errors
        private static AuthenticationException CreateException(uint result) =>
            new AuthenticationException(SR.Format(SR.net_ssl_horiz_service_error, $"{2000 + (result & 0x1FF)}-{(result >> 9) & 0x1FFF:D4}"));
    }
}
