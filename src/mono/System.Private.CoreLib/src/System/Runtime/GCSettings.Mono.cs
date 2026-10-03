// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Runtime
{
    public partial class GCSettings
    {
        public static bool IsServerGC => false;

        private static GCLatencyMode GetGCLatencyMode() => GCLatencyMode.Batch;

        private static SetLatencyModeStatus SetGCLatencyMode(GCLatencyMode newLatencyMode)
        {
#if TARGET_LIBNX
            // Games (Monocle/FNA engine) set SustainedLowLatency during startup.
            // mono/sgen only supports Batch; pretend we accepted it rather than
            // throwing PlatformNotSupportedException.
            if (newLatencyMode == GCLatencyMode.SustainedLowLatency)
                return SetLatencyModeStatus.Succeeded;
#endif
            if (newLatencyMode != GCLatencyMode.Batch)
                throw new PlatformNotSupportedException();

            return SetLatencyModeStatus.Succeeded;
        }

        private static GCLargeObjectHeapCompactionMode GetLOHCompactionMode() => GCLargeObjectHeapCompactionMode.Default;

        private static void SetLOHCompactionMode(GCLargeObjectHeapCompactionMode newLOHCompactionMode)
        {
            if (newLOHCompactionMode != GCLargeObjectHeapCompactionMode.Default)
                throw new PlatformNotSupportedException();
        }
    }
}
