// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace System.Net
{
    // Horizon's ssl service verifies the server's certificate during the
    // handshake, against the system's trust store, and certificates cannot be
    // loaded on horiz. SslStream therefore never sees the remote certificate.
    internal static partial class CertificateValidationPal
    {
        internal static SslPolicyErrors VerifyCertificateProperties(
            SafeDeleteContext _ /*securityContext*/,
            X509Chain _1 /*chain*/,
            X509Certificate2? _2 /*remoteCertificate*/,
            bool _3 /*checkCertName*/,
            bool _4 /*isServer*/,
            string? _5 /*hostName*/)
        {
            return SslPolicyErrors.RemoteCertificateNotAvailable;
        }

#pragma warning disable IDE0060 // The shared code passes these by name
        private static X509Certificate2? GetRemoteCertificate(
            SafeDeleteContext? securityContext,
            bool retrieveChainCertificates,
            ref X509Chain? chain,
            X509ChainPolicy? chainPolicy)
#pragma warning restore IDE0060
        {
            return null;
        }

        // Check if the local certificate has been sent to the peer during the handshake.
        internal static bool IsLocalCertificateUsed(SafeFreeCredentials? _, SafeDeleteContext? _1)
        {
            return false;
        }

        //
        // Used only by client SSL code, never returns null.
        //
        internal static string[] GetRequestCertificateAuthorities(SafeDeleteContext _)
        {
            return Array.Empty<string>();
        }

        private static X509Store OpenStore(StoreLocation storeLocation)
        {
            X509Store store = new X509Store(StoreName.My, storeLocation);
            store.Open(OpenFlags.ReadOnly);
            return store;
        }
    }
}
