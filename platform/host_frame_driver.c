#include "host_frame_driver.h"

#include <string.h>

#define HOST_FRAME_FIELD_UNITS UINT64_C(1000000000)

void host_frame_driver_init(
    HostFrameDriver *driver, unsigned field_hz, unsigned fields_per_tick) {
    memset(driver, 0, sizeof(*driver));
    sim_sched_init_fields(&driver->clock, field_hz, fields_per_tick);
}

unsigned host_frame_driver_advance_units(
    HostFrameDriver *driver, uint64_t units, bool rebase) {
    unsigned due;

    driver->stats.advances++;
    if (!rebase && driver->pending_tickets != 0u) {
        /* The platform must run already-due authored ticks before observing
         * more host time. Refusing the sample makes that ownership rule
         * fail-safe and keeps ticket debt bounded by one clock sample. */
        driver->stats.blocked_advances++;
        return 0u;
    }
    if (rebase) {
        sim_sched_rebase(&driver->clock);
        driver->pending_tickets = 0;
        driver->stats.rebases++;
    }
    due = sim_sched_advance_units(&driver->clock, units, 0u);
    if (due != 0u) {
        /* pending_tickets is zero here unless this was an explicit rebase, in
         * which case it was cleared above. SimSched bounds a single accepted
         * sample to its 30-tick suspension window. */
        driver->pending_tickets = (uint64_t)due;
        driver->stats.clock_ticks += (uint64_t)due;
        if (driver->pending_tickets > driver->stats.max_pending_tickets) {
            driver->stats.max_pending_tickets = driver->pending_tickets;
        }
    }
    return due;
}

unsigned host_frame_driver_advance_fields(
    HostFrameDriver *driver, unsigned fields, bool rebase) {
    return host_frame_driver_advance_units(
        driver, (uint64_t)fields * HOST_FRAME_FIELD_UNITS, rebase);
}

bool host_frame_driver_take_tick(HostFrameDriver *driver) {
    if (driver->pending_tickets == 0u) {
        return false;
    }
    driver->pending_tickets--;
    driver->stats.issued_tickets++;
    return true;
}

uint64_t host_frame_driver_pending_ticks(const HostFrameDriver *driver) {
    return driver->pending_tickets;
}

uint64_t host_frame_driver_clock_ticks(const HostFrameDriver *driver) {
    return driver->stats.clock_ticks;
}

uint64_t host_frame_driver_issued_ticks(const HostFrameDriver *driver) {
    return driver->stats.issued_tickets;
}

unsigned host_frame_driver_fields_per_tick(const HostFrameDriver *driver) {
    return driver->clock.fields_per_tick;
}

void host_frame_driver_alpha(
    const HostFrameDriver *driver, uint64_t *numerator, uint64_t *denominator) {
    unsigned long long num = 0;
    unsigned long long den = 1;

    if (driver->pending_tickets == 0u) {
        sim_sched_alpha(&driver->clock, &num, &den);
    } else {
        den = driver->clock.tick_units;
    }
    if (numerator != NULL) {
        *numerator = (uint64_t)num;
    }
    if (denominator != NULL) {
        *denominator = (uint64_t)den;
    }
}
