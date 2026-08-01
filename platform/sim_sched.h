/* Fixed-authority frame clock and accumulator (spec §5, Phase 1). */
#ifndef MDKR_SIM_SCHED_H
#define MDKR_SIM_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SimSchedStats {
    unsigned long long ticks;
    unsigned long long catchup_steps;
    unsigned long long render_skips_owed;
    unsigned long long rebases;
} SimSchedStats;

typedef struct SimSched {
    unsigned field_hz;                   /* 60 NTSC/MPAL, 50 PAL */
    unsigned fields_per_tick;            /* 2 original, 1 explicit enhanced */
    unsigned long long tick_units;       /* fields_per_tick x 1e9 */
    unsigned long long accumulator_units; /* ns x field_hz residue */
    SimSchedStats stats;
} SimSched;

void sim_sched_init(SimSched *sched, unsigned field_hz);
void sim_sched_init_fields(
    SimSched *sched, unsigned field_hz, unsigned fields_per_tick);
void sim_sched_rebase(SimSched *sched);
/* Returns authoritative steps due for this host frame (0..max_steps;
 * max_steps == 0 means unbounded). */
unsigned sim_sched_advance(
    SimSched *sched, unsigned long long elapsed_ns, unsigned max_steps);
/*
 * Same, but in the accumulator's own exact units (ns x field_hz; one source
 * field is 1e9). sim_sched_advance is this function with
 * `elapsed_ns * field_hz`.
 *
 * A caller whose clock is a whole number of SOURCE VI FIELDS — the pacer's
 * committed field count (platform_sdl_min.c) is exactly that — must use this
 * entry point rather than converting to nanoseconds first: one field is
 * 1e9/field_hz seconds, which is not an integral nanosecond count at 60 Hz,
 * so the ns round trip loses 20 units per field and the accumulator would
 * drift off the tick boundary it is supposed to land on exactly. One field is
 * 1e9 units, always, with no remainder.
 */
unsigned sim_sched_advance_units(
    SimSched *sched, unsigned long long units, unsigned max_steps);
void sim_sched_alpha(
    const SimSched *sched,
    unsigned long long *numerator,
    unsigned long long *denominator);
int sim_sched_has_debt(const SimSched *sched);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_SIM_SCHED_H */
