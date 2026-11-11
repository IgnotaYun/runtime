// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

internal static partial class Interop
{
    internal static partial class Sys
    {
        // TLS over Horizon's ssl service
        internal enum HorizSslStatus
        {
            OK = 0,
            WouldBlock = 1,
            Error = 2,
        }

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslCreate", StringMarshalling = StringMarshalling.Utf8)]
        internal static unsafe partial HorizSslStatus HorizSslCreate(
            int socket,
            int sslVersion,
            string? hostName,
            byte* alpn,
            int alpnLength,
            out SafeHorizSslHandle connection,
            out uint result);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslDestroy")]
        internal static partial void HorizSslDestroy(IntPtr connection);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslHandshake")]
        internal static partial HorizSslStatus HorizSslHandshake(SafeHorizSslHandle connection, out uint result);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslRead")]
        internal static unsafe partial HorizSslStatus HorizSslRead(SafeHorizSslHandle connection, byte* buffer, int length, out int read, out uint result);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslWrite")]
        internal static unsafe partial HorizSslStatus HorizSslWrite(SafeHorizSslHandle connection, byte* buffer, int length, out int written, out uint result);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslGetCipherInfo")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static unsafe partial bool HorizSslGetCipherInfo(SafeHorizSslHandle connection, byte* cipher, int cipherLength, byte* protocol, int protocolLength);

        [LibraryImport(Libraries.SystemNative, EntryPoint = "SystemNative_HorizSslGetAlpnProtocol")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static unsafe partial bool HorizSslGetAlpnProtocol(SafeHorizSslHandle connection, byte* buffer, int bufferLength, out int length);
    }
}

namespace Microsoft.Win32.SafeHandles
{
    internal sealed class SafeHorizSslHandle : SafeHandle
    {
        public SafeHorizSslHandle() : base(IntPtr.Zero, ownsHandle: true)
        {
        }

        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            Interop.Sys.HorizSslDestroy(handle);
            SetHandle(IntPtr.Zero);
            return true;
        }
    }
}
