/*
 * host_sched_probe.c -- host scheduling counters for frame-time diagnostics.
 * See host_sched_probe.h. Kept out of platform_sdl_min.c on purpose: the Mach
 * headers and the ROM SDK headers that file pulls in do not mix.
 */
#include "host_sched_probe.h"

#include <string.h>

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

/* proc_pid_rusage reports its times in Mach absolute-time units, not
 * nanoseconds (125/3 ns per unit on Apple silicon, 1/1 on Intel). */
static uint64_t host_abs_to_ns(uint64_t units) {
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0u) {
        if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
            timebase.denom == 0u) {
            timebase.numer = 1u;
            timebase.denom = 1u;
        }
    }
    return units / timebase.denom * timebase.numer +
           units % timebase.denom * timebase.numer / timebase.denom;
}

void mdkr_host_sched_sample(MdkrHostSchedSample *out) {
    struct rusage_info_v4 info;
    struct rusage usage;
    thread_extended_info_data_t thread;
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->thread_priority = -1;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4,
                        (rusage_info_t *)&info) == 0) {
        out->valid = true;
        out->cpu_ns = host_abs_to_ns(info.ri_user_time + info.ri_system_time);
        out->runnable_ns = host_abs_to_ns(info.ri_runnable_time);
        out->low_qos_cpu_ns = host_abs_to_ns(
            info.ri_cpu_time_qos_utility + info.ri_cpu_time_qos_background +
            info.ri_cpu_time_qos_maintenance);
        out->pageins = info.ri_pageins;
    }
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        out->valid = true;
        out->voluntary_switches = (uint64_t)usage.ru_nvcsw;
        out->involuntary_switches = (uint64_t)usage.ru_nivcsw;
    }
    /* pthread_mach_thread_np adds no port reference, unlike
     * mach_thread_self(), so there is nothing to deallocate. */
    if (thread_info(pthread_mach_thread_np(pthread_self()),
                    THREAD_EXTENDED_INFO, (thread_info_t)&thread,
                    &count) == KERN_SUCCESS) {
        out->valid = true;
        out->thread_priority = thread.pth_curpri;
    }
}

#elif !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/resource.h>

void mdkr_host_sched_sample(MdkrHostSchedSample *out) {
    struct rusage usage;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->thread_priority = -1;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        out->valid = true;
        out->cpu_ns =
            ((uint64_t)usage.ru_utime.tv_sec + (uint64_t)usage.ru_stime.tv_sec) *
                UINT64_C(1000000000) +
            ((uint64_t)usage.ru_utime.tv_usec +
             (uint64_t)usage.ru_stime.tv_usec) * UINT64_C(1000);
        out->pageins = (uint64_t)usage.ru_majflt;
        out->voluntary_switches = (uint64_t)usage.ru_nvcsw;
        out->involuntary_switches = (uint64_t)usage.ru_nivcsw;
    }
}

#else

void mdkr_host_sched_sample(MdkrHostSchedSample *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->thread_priority = -1;
}

#endif
