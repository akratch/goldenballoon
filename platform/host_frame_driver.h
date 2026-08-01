/*
 * Exact host-time to authoritative-tick driver.
 *
 * The platform feeds elapsed time once per host opportunity. The driver owns
 * the SimSched clock, turns elapsed time into fixed-size tickets, retains
 * ordinary catch-up debt, rebases suspension gaps, and exposes an alpha that
 * can never extrapolate. Presentation code may decide how many images to draw;
 * it cannot manufacture or resize a gameplay tick.
 */
#ifndef MDKR_HOST_FRAME_DRIVER_H
#define MDKR_HOST_FRAME_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HostFrameDriverStats {
    uint64_t advances;
    uint64_t blocked_advances;
    uint64_t clock_ticks;
    uint64_t issued_tickets;
    uint64_t max_pending_tickets;
    uint64_t rebases;
} HostFrameDriverStats;

typedef struct HostFrameDriver {
    SimSched clock;
    uint64_t pending_tickets;
    HostFrameDriverStats stats;
} HostFrameDriver;

void host_frame_driver_init(
    HostFrameDriver *driver, unsigned field_hz, unsigned fields_per_tick);

/*
 * Add exact clock units (one source field == 1e9 units). A caller must drain
 * all issued tickets before sampling the host clock again; an attempted
 * non-rebase advance while tickets remain is rejected and recorded. This
 * makes the queue bounded by SimSched's one-sample suspension window. When
 * `rebase` is true, all pre-suspension residue and debt is retired before
 * `units` starts a fresh timeline. Returns the number of newly due tickets.
 */
unsigned host_frame_driver_advance_units(
    HostFrameDriver *driver, uint64_t units, bool rebase);

/* Convenience for an integral count of source fields. */
unsigned host_frame_driver_advance_fields(
    HostFrameDriver *driver, unsigned fields, bool rebase);

/* Consume one exact fixed-step ticket. False means the simulation must wait. */
bool host_frame_driver_take_tick(HostFrameDriver *driver);

uint64_t host_frame_driver_pending_ticks(const HostFrameDriver *driver);
uint64_t host_frame_driver_clock_ticks(const HostFrameDriver *driver);
uint64_t host_frame_driver_issued_ticks(const HostFrameDriver *driver);
unsigned host_frame_driver_fields_per_tick(const HostFrameDriver *driver);

/* Exact interpolation fraction. It is forced to 0 while ticket debt exists. */
void host_frame_driver_alpha(
    const HostFrameDriver *driver, uint64_t *numerator, uint64_t *denominator);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_HOST_FRAME_DRIVER_H */
