#include "rollback_driver.h"

#include <string.h>

bool mdkr_rollback_driver_init(
    MdkrRollbackDriver *driver, MdkrNetInputHistory *inputs,
    MdkrRollbackRing *snapshots, MdkrRollbackEventJournal *events,
    MdkrRollbackSimulateFn simulate, void *context, uint32_t first_tick,
    unsigned max_resimulation_ticks) {
    if (driver == NULL || inputs == NULL || snapshots == NULL || simulate == NULL ||
        max_resimulation_ticks == 0u || max_resimulation_ticks > 16u ||
        !mdkr_rollback_ring_has(snapshots, first_tick - 1u)) return false;
    memset(driver, 0, sizeof(*driver));
    driver->inputs = inputs;
    driver->snapshots = snapshots;
    driver->events = events;
    driver->simulate = simulate;
    driver->context = context;
    driver->current_tick = first_tick;
    driver->max_resimulation_ticks = (uint8_t)max_resimulation_ticks;
    return true;
}

static bool resimulate_if_dirty(MdkrRollbackDriver *driver) {
    uint32_t dirty;
    uint32_t tick;
    uint32_t depth;
    if (!mdkr_net_input_take_dirty(driver->inputs, &dirty)) return true;
    depth = driver->current_tick - dirty;
    if (depth == 0u) return true;
    if (depth > driver->max_resimulation_ticks) {
        driver->error = MDKR_ROLLBACK_DRIVER_RESIM_LIMIT;
        return false;
    }
    if (!mdkr_rollback_ring_restore(driver->snapshots, dirty - 1u, true)) {
        driver->error = MDKR_ROLLBACK_DRIVER_SNAPSHOT_MISSING;
        return false;
    }
    if (driver->events != NULL) mdkr_rollback_events_begin_rewrite(driver->events, dirty);
    for (tick = dirty; tick != driver->current_tick; tick++) {
        MdkrNetInputSet set;
        if (!mdkr_net_input_for_tick(driver->inputs, tick, &set)) {
            driver->error = MDKR_ROLLBACK_DRIVER_INPUT_UNAVAILABLE;
            if (driver->events != NULL) mdkr_rollback_events_force_clear(driver->events);
            return false;
        }
        if (!driver->simulate(driver->context, tick, &set, true)) {
            driver->error = MDKR_ROLLBACK_DRIVER_SIMULATION_FAILED;
            if (driver->events != NULL) mdkr_rollback_events_force_clear(driver->events);
            return false;
        }
        if (!mdkr_rollback_ring_capture(driver->snapshots, tick)) {
            driver->error = MDKR_ROLLBACK_DRIVER_CAPTURE_FAILED;
            if (driver->events != NULL) mdkr_rollback_events_force_clear(driver->events);
            return false;
        }
        driver->stats.resimulated_ticks++;
    }
    if (driver->events != NULL) mdkr_rollback_events_end_rewrite(driver->events);
    driver->stats.rollback_count++;
    if (depth > driver->stats.deepest_rollback) driver->stats.deepest_rollback = depth;
    return true;
}

bool mdkr_rollback_driver_advance(MdkrRollbackDriver *driver) {
    MdkrNetInputSet set;
    if (driver == NULL || driver->error != MDKR_ROLLBACK_DRIVER_OK) return false;
    mdkr_net_input_set_current_tick(driver->inputs, driver->current_tick);
    if (!resimulate_if_dirty(driver)) return false;
    if (!mdkr_net_input_for_tick(driver->inputs, driver->current_tick, &set)) {
        driver->error = MDKR_ROLLBACK_DRIVER_INPUT_UNAVAILABLE;
        return false;
    }
    if (!driver->simulate(driver->context, driver->current_tick, &set, false)) {
        driver->error = MDKR_ROLLBACK_DRIVER_SIMULATION_FAILED;
        return false;
    }
    if (!mdkr_rollback_ring_capture(driver->snapshots, driver->current_tick)) {
        driver->error = MDKR_ROLLBACK_DRIVER_CAPTURE_FAILED;
        return false;
    }
    driver->current_tick++;
    driver->stats.authored_ticks++;
    mdkr_net_input_set_current_tick(driver->inputs, driver->current_tick);
    return true;
}

bool mdkr_rollback_driver_reconcile(MdkrRollbackDriver *driver) {
    if (driver == NULL || driver->error != MDKR_ROLLBACK_DRIVER_OK) return false;
    mdkr_net_input_set_current_tick(driver->inputs, driver->current_tick);
    return resimulate_if_dirty(driver);
}
