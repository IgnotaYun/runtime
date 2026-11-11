// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Security.Authentication;

namespace System.Net.Security
{
    internal partial struct SslConnectionInfo
    {
        public void UpdateSslConnectionInfo(SafeDeleteSslContext context)
        {
            SslProtocols protocol = SslProtocols.None;

            if (context.GetCipherInfo(out string? cipher, out string? version))
            {
                protocol = SafeDeleteSslContext.ParseProtocol(version);

                if (Enum.TryParse(cipher, out TlsCipherSuite cipherSuite))
                {
                    MapCipherSuite(cipherSuite);
                }
            }

            // Not every version of the service reports the connection, and SslStream
            // takes a connection without a protocol for one that is not established.
            // Report the highest protocol that the connection was allowed to use.
            Protocol = (int)(protocol != SslProtocols.None ? protocol : context.MaxProtocol);

            ApplicationProtocol = context.GetApplicationProtocol();
        }
    }
}
