#include "host_frame_driver.h"
#include "tick_dispatch_gate.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIELD_UNITS UINT64_C(1000000000)
#define MAX_TICKS 5000u

static int s_failures;

typedef struct Trace {
    uint64_t rows[MAX_TICKS];
    size_t count;
} Trace;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_next_tick_dispatch_exit_gate(void) {
    expect("issued ticket permits the next authoritative pass",
           mdkr_next_tick_dispatch_allowed(false, true));
    expect("missing ticket cannot dispatch the next pass",
           !mdkr_next_tick_dispatch_allowed(false, false));
    expect("exit request cannot dispatch the next pass",
           !mdkr_next_tick_dispatch_allowed(true, true));
}

static uint64_t mix(uint64_t state, uint64_t tick) {
    state ^= tick + UINT64_C(0x9e3779b97f4a7c15) +
             (state << 6) + (state >> 2);
    state ^= state >> 30;
    state *= UINT64_C(0xbf58476d1ce4e5b9);
    state ^= state >> 27;
    state *= UINT64_C(0x94d049bb133111eb);
    return state ^ (state >> 31);
}

static void drain(HostFrameDriver *driver, Trace *trace) {
    while (host_frame_driver_take_tick(driver)) {
        uint64_t state = trace->count == 0
            ? UINT64_C(0x444b523634)
            : trace->rows[trace->count - 1];
        if (trace->count >= MAX_TICKS) {
            expect("trace capacity", 0);
            return;
        }
        trace->rows[trace->count] = mix(state, trace->count);
        trace->count++;
    }
}

static void check_alpha(const char *name, HostFrameDriver *driver) {
    uint64_t num = 0, den = 0;
    host_frame_driver_alpha(driver, &num, &den);
    if (den == 0 || num >= den) {
        fprintf(stderr, "FAIL %s alpha=%llu/%llu\n", name,
                (unsigned long long)num, (unsigned long long)den);
        s_failures++;
    }
}

static Trace run_integer_rate(unsigned rate, unsigned seconds) {
    HostFrameDriver driver;
    Trace trace;
    uint64_t phase = 0;
    const uint64_t units_per_second = 60u * FIELD_UNITS;
    unsigned frame;

    memset(&trace, 0, sizeof(trace));
    host_frame_driver_init(&driver, 60u, 2u);
    for (frame = 0; frame < rate * seconds; frame++) {
        uint64_t units;
        phase += units_per_second;
        units = phase / rate;
        phase %= rate;
        (void)host_frame_driver_advance_units(&driver, units, false);
        check_alpha("integer rate", &driver);
        drain(&driver, &trace);
        check_alpha("integer rate drained", &driver);
    }
    expect("integer schedule ends without ticket debt",
           host_frame_driver_pending_ticks(&driver) == 0u);
    expect("integer schedule ends without time residue",
           driver.clock.accumulator_units == 0u);
    expect("integer schedule issued every clock tick",
           host_frame_driver_clock_ticks(&driver) ==
               host_frame_driver_issued_ticks(&driver));
    return trace;
}

static void expect_same(const char *name, const Trace *expected,
                        const Trace *actual) {
    if (expected->count != actual->count ||
        memcmp(expected->rows, actual->rows,
               expected->count * sizeof(expected->rows[0])) != 0) {
        fprintf(stderr, "FAIL %s expected=%zu actual=%zu\n",
                name, expected->count, actual->count);
        s_failures++;
    }
}

static Trace run_irregular(unsigned seconds) {
    static const uint64_t parts[] = {
        UINT64_C(1), UINT64_C(1999999999), UINT64_C(17000000000),
        UINT64_C(3), UINT64_C(40999999997)
    };
    HostFrameDriver driver;
    Trace trace;
    unsigned second, part;

    memset(&trace, 0, sizeof(trace));
    host_frame_driver_init(&driver, 60u, 2u);
    for (second = 0; second < seconds; second++) {
        for (part = 0; part < sizeof(parts) / sizeof(parts[0]); part++) {
            (void)host_frame_driver_advance_units(
                &driver, parts[part], false);
            check_alpha("irregular", &driver);
            drain(&driver, &trace);
        }
    }
    return trace;
}

static Trace run_uncapped_like(unsigned seconds) {
    HostFrameDriver driver;
    Trace trace;
    uint64_t total = 0;
    uint64_t target = (uint64_t)seconds * 60u * FIELD_UNITS;
    unsigned frame = 0;

    memset(&trace, 0, sizeof(trace));
    host_frame_driver_init(&driver, 60u, 2u);
    while (total < target) {
        /* Deterministic 700--1700 microsecond host opportunities. */
        uint64_t ns = UINT64_C(700000) +
            (uint64_t)((frame * 48271u) % 1000001u);
        uint64_t units = ns * 60u;
        if (units > target - total) {
            units = target - total;
        }
        total += units;
        (void)host_frame_driver_advance_units(&driver, units, false);
        check_alpha("uncapped-like", &driver);
        drain(&driver, &trace);
        frame++;
    }
    return trace;
}

static void test_schedule_matrix(void) {
    static const unsigned rates[] = {
        30u, 50u, 60u, 90u, 120u, 144u, 165u, 240u
    };
    const unsigned seconds = 120u;
    Trace baseline = run_integer_rate(rates[0], seconds);
    unsigned index;

    expect("baseline has 30 fixed ticks per second",
           baseline.count == 30u * seconds);
    for (index = 1; index < sizeof(rates) / sizeof(rates[0]); index++) {
        Trace other = run_integer_rate(rates[index], seconds);
        char label[64];
        (void)snprintf(label, sizeof(label), "%u Hz schedule", rates[index]);
        expect_same(label, &baseline, &other);
    }
    {
        Trace irregular = run_irregular(seconds);
        Trace uncapped = run_uncapped_like(seconds);
        expect_same("alternating irregular schedule", &baseline, &irregular);
        expect_same("uncapped-like schedule", &baseline, &uncapped);
    }
}

static void test_5994_and_pal(void) {
    HostFrameDriver driver;
    uint64_t phase = 0;
    uint64_t ticks = 0;
    unsigned frame;

    host_frame_driver_init(&driver, 60u, 2u);
    /* 60000/1001 Hz for exactly 1001 seconds. */
    for (frame = 0; frame < 60000u; frame++) {
        uint64_t units;
        phase += 60u * FIELD_UNITS * UINT64_C(1001);
        units = phase / 60000u;
        phase %= 60000u;
        (void)host_frame_driver_advance_units(&driver, units, false);
        while (host_frame_driver_take_tick(&driver)) {
            ticks++;
        }
    }
    expect("59.94-like long run has exact tick total", ticks == 30030u);
    expect("59.94-like long run has zero residue",
           driver.clock.accumulator_units == 0u);

    host_frame_driver_init(&driver, 50u, 2u);
    ticks = 0;
    for (frame = 0; frame < 50u * 3600u; frame++) {
        (void)host_frame_driver_advance_units(
            &driver, FIELD_UNITS, false);
        while (host_frame_driver_take_tick(&driver)) {
            ticks++;
        }
    }
    expect("PAL hour is exactly 25 Hz", ticks == 25u * 3600u);
}

static void test_debt_and_rebase(void) {
    HostFrameDriver driver;
    uint64_t num = 1, den = 0;

    host_frame_driver_init(&driver, 60u, 2u);
    expect("six-tick burst becomes six tickets",
           host_frame_driver_advance_units(
               &driver, 12u * FIELD_UNITS, false) == 6u);
    expect("burst debt retained",
           host_frame_driver_pending_ticks(&driver) == 6u);
    expect("host clock cannot resample while ticket debt remains",
           host_frame_driver_advance_fields(&driver, 2u, false) == 0u &&
           host_frame_driver_pending_ticks(&driver) == 6u &&
           driver.stats.blocked_advances == 1u);
    host_frame_driver_alpha(&driver, &num, &den);
    expect("alpha is zero under debt", num == 0u && den == 2u * FIELD_UNITS);
    expect("first debt ticket available", host_frame_driver_take_tick(&driver));
    expect("ordinary debt is not discarded",
           host_frame_driver_pending_ticks(&driver) == 5u);

    expect("suspension starts a fresh one-tick timeline",
           host_frame_driver_advance_units(
               &driver, 2u * FIELD_UNITS, true) == 1u);
    expect("pre-suspend debt retired and one new ticket remains",
           host_frame_driver_pending_ticks(&driver) == 1u);
    expect("driver records suspension rebase", driver.stats.rebases == 1u);
    expect("fresh ticket can run", host_frame_driver_take_tick(&driver));
    host_frame_driver_alpha(&driver, &num, &den);
    expect("alpha is exact zero after fresh tick", num == 0u);

    host_frame_driver_init(&driver, 60u, 1u);
    expect("explicit enhanced cadence keeps one-field tickets",
           host_frame_driver_advance_fields(&driver, 1u, false) == 1u &&
           host_frame_driver_fields_per_tick(&driver) == 1u &&
           host_frame_driver_take_tick(&driver));

    host_frame_driver_init(&driver, 60u, 0u);
    expect("invalid zero-width cadence falls back to authored width",
           host_frame_driver_fields_per_tick(&driver) == 2u);
    host_frame_driver_init(&driver, 60u, 1000u);
    expect("oversized cadence falls back to authored width",
           host_frame_driver_fields_per_tick(&driver) == 2u);
}

int main(void) {
    test_next_tick_dispatch_exit_gate();
    test_schedule_matrix();
    test_5994_and_pal();
    test_debt_and_rebase();
    if (s_failures != 0) {
        fprintf(stderr, "%d host-frame-driver test(s) failed\n", s_failures);
        return 1;
    }
    puts("host frame driver: PASS");
    return 0;
}
