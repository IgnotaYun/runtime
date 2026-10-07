// Cumulative perf counters for the libnx port (cheap: 2 atomic adds per call).
// Defined in the host (native/clrhost/shimsrc/nx_stats.c); dumped periodically
// by the host's monitor thread.
#pragma once
#include <stdint.h>

extern "C" uint64_t nxstat_ticks(void);              // libnx armGetSystemTick
extern "C" uint64_t nxstat_ticks_per_sec(void);
extern "C" volatile uint64_t nxstat_pinvoke_count, nxstat_pinvoke_ticks;
extern "C" volatile uint64_t nxstat_invoke_managed_count, nxstat_invoke_managed_ticks;
extern "C" const char* volatile nxstat_phase;

struct NxStatScope
{
    uint64_t t0;
    volatile uint64_t* sum;
    NxStatScope(volatile uint64_t* counter, volatile uint64_t* ticks) : sum(ticks)
    {
        if (counter) ++*counter;
        t0 = nxstat_ticks();
    }
    ~NxStatScope() { *sum += nxstat_ticks() - t0; }
};
#define NXSTAT_SCOPE(cnt, sum) NxStatScope _nxs_##__LINE__(cnt, sum)
