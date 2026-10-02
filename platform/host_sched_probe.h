#ifndef MDKR_HOST_SCHED_PROBE_H
#define MDKR_HOST_SCHED_PROBE_H

/*
 * Host scheduling evidence for frame-time diagnostics.
 *
 * A frame that took 50 ms while the game thread ran for 2 ms waited on
 * something. These counters say which kind of wait it was:
 *
 *   cpu_ns / runnable_ns   the process's CPU time, and the time its threads
 *                          were runnable (running OR ready to run). Runnable
 *                          growing faster than CPU means a thread was ready
 *                          and the scheduler gave it no core: contention, or a
 *                          priority the host lowered.
 *   low_qos_cpu_ns         CPU billed at utility, background or maintenance
 *                          QoS -- what an App Nap / darwinbg demotion looks
 *                          like from inside the process.
 *   pageins                page-ins from disk (a thread blocked on paging).
 *   voluntary_switches /   context switches: blocked (voluntary) versus
 *   involuntary_switches   preempted (involuntary).
 *   thread_priority        the calling thread's current scheduling priority,
 *                          -1 where the host does not report it.
 *
 * Everything but thread_priority is a process-wide running total; a caller
 * differences two samples. Fields a host cannot supply stay 0 and `valid`
 * says whether anything was read at all. One call costs a few system calls,
 * so it is made only by a diagnostic that asked for it (the frame rows of
 * MDKR_TEXTURE_PACK_TIMING=1, gfx_pc_dkr.c); nothing else calls it.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrHostSchedSample {
    bool     valid;
    uint64_t cpu_ns;
    uint64_t runnable_ns;
    uint64_t low_qos_cpu_ns;
    uint64_t pageins;
    uint64_t voluntary_switches;
    uint64_t involuntary_switches;
    int      thread_priority;
} MdkrHostSchedSample;

void mdkr_host_sched_sample(MdkrHostSchedSample *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_HOST_SCHED_PROBE_H */
