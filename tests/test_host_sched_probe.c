/*
 * host_sched_probe: the counters the frame-wait rows difference must be real
 * running totals in the units the rows print (nanoseconds), or a long frame's
 * "off-CPU" figure is noise. On macOS proc_pid_rusage reports Mach units
 * (125/3 ns on Apple silicon), which is the conversion this pins.
 */
#include "host_sched_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    } else {
        printf("ok %s\n", name);
    }
}

#if defined(__APPLE__) || defined(__linux__)
static uint64_t thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* Spin on this thread for `ns` of its own CPU time. */
static void burn(uint64_t ns) {
    const uint64_t start = thread_cpu_ns();
    volatile uint64_t sink = 0;
    while (thread_cpu_ns() - start < ns) {
        for (int i = 0; i < 1000; i++) sink += (uint64_t)i;
    }
}
#endif

int main(void) {
    MdkrHostSchedSample before;
    MdkrHostSchedSample after;

    mdkr_host_sched_sample(NULL); /* must not crash */
    mdkr_host_sched_sample(&before);
#if defined(__APPLE__) || defined(__linux__)
    {
        const uint64_t thread_before = thread_cpu_ns();
        const uint64_t wall_before = monotonic_ns();
        uint64_t thread_spent;
        uint64_t wall_spent;
        uint64_t cpu_spent;
        struct timespec nap = { 0, 20000000 }; /* 20 ms */

        burn(UINT64_C(200000000)); /* 200 ms of this thread's CPU */
        nanosleep(&nap, NULL);
        mdkr_host_sched_sample(&after);
        thread_spent = thread_cpu_ns() - thread_before;
        wall_spent = monotonic_ns() - wall_before;

        expect("a sample is read on this host", before.valid && after.valid);
        expect("cpu time is a running total", after.cpu_ns >= before.cpu_ns);
        cpu_spent = after.cpu_ns - before.cpu_ns;
        /* In nanoseconds: the process ran at least this thread's 200 ms
         * (Mach units read as nanoseconds would be 1/41.7 of it on Apple
         * silicon), and no more than a handful of cores' worth of wall time
         * (a doubled conversion would be 41.7 times too much). */
        expect("process cpu covers the thread's burn",
               cpu_spent + UINT64_C(5000000) >= thread_spent);
        expect("process cpu is not over-converted",
               cpu_spent <= wall_spent * UINT64_C(16));
        expect("counters never run backwards",
               after.pageins >= before.pageins &&
                   after.voluntary_switches >= before.voluntary_switches &&
                   after.involuntary_switches >= before.involuntary_switches &&
                   after.low_qos_cpu_ns >= before.low_qos_cpu_ns);
#if defined(__APPLE__)
        /* Runnable time counts running too, so it can only lag CPU time by
         * accounting granularity, and it must be in the same unit. */
        expect("runnable time is a running total",
               after.runnable_ns >= before.runnable_ns);
        expect("runnable time covers the burn",
               after.runnable_ns - before.runnable_ns + UINT64_C(5000000) >=
                   thread_spent);
        expect("runnable time is not over-converted",
               after.runnable_ns - before.runnable_ns <=
                   wall_spent * UINT64_C(16));
        expect("thread priority is a Mach priority",
               after.thread_priority >= 0 && after.thread_priority <= 127);
#else
        expect("no thread priority where the host does not report one",
               after.thread_priority == -1);
#endif
    }
#else
    mdkr_host_sched_sample(&after);
    expect("no sample on this host", !before.valid && !after.valid &&
                                         after.thread_priority == -1);
#endif
    if (s_failures != 0) {
        fprintf(stderr, "%d FAIL\n", s_failures);
        return 1;
    }
    printf("host_sched_probe: all ok\n");
    return 0;
}
