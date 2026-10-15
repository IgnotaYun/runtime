// PAL entry points that exist on the POSIX hosts but have no meaning on libnx.
//
// Keep this file short and the reasons specific: each entry is here because some
// runtime feature is compiled in for this target and links against it.

#include "pal.h"

// The perf map (FEATURE_PERFMAP) can be told to ignore the profiling signal.
// libnx delivers no signals at all, so there is nothing to ignore; the call is
// only made when DOTNET_PerfMapIgnoreSignal is set, but the symbol has to exist
// for the link.
//
// Weak: pal/src/exception/signal.cpp defines a real one for the hosts that pull
// it in - the linuxnx mirror does, and without this the two definitions collide
// at link time. The real one wins where it is linked, which is the correct
// behaviour for a host process that does have signals; the NRO keeps this stub
// because it never pulls signal.cpp (libnxstubs.cpp covers those entry points).
__attribute__((weak)) void PALAPI PAL_IgnoreProfileSignal(int signalNum)
{
    (void)signalNum;
}
