#include "net_input.h"

#include <string.h>

static MdkrInputSample neutral(void) {
    const MdkrInputSample value = {0u, 0, 0, true};
    return value;
}

static bool same_sample(MdkrInputSample left, MdkrInputSample right) {
    return left.buttons == right.buttons && left.stick_x == right.stick_x &&
        left.stick_y == right.stick_y && left.present == right.present;
}

bool mdkr_net_tick_after(uint32_t candidate, uint32_t reference) {
    const uint32_t distance = candidate - reference;
    return distance != 0u && distance < 0x80000000u;
}

static bool tick_before(uint32_t candidate, uint32_t reference) {
    return mdkr_net_tick_after(reference, candidate);
}

static MdkrNetInputCell *cell_for(
    MdkrNetInputHistory *history, uint32_t tick, bool create) {
    MdkrNetInputCell *cell = &history->cells[tick % MDKR_NET_INPUT_CAPACITY];
    if (!cell->occupied || cell->tick != tick) {
        if (!create) return NULL;
        memset(cell, 0, sizeof(*cell));
        cell->tick = tick;
        cell->occupied = true;
    }
    return cell;
}

void mdkr_net_input_init(MdkrNetInputHistory *history, uint32_t first_tick) {
    if (history == NULL) return;
    memset(history, 0, sizeof(*history));
    history->first_tick = first_tick;
    history->current_tick = first_tick;
    history->confirmed_through = first_tick - 1u;
}

void mdkr_net_input_set_current_tick(MdkrNetInputHistory *history, uint32_t tick) {
    if (history == NULL) return;
    if (tick == history->current_tick || mdkr_net_tick_after(tick, history->current_tick)) {
        history->current_tick = tick;
    }
}

static bool too_old(const MdkrNetInputHistory *history, uint32_t tick) {
    const uint32_t oldest = history->current_tick - (MDKR_NET_INPUT_CAPACITY - 1u);
    return tick_before(tick, oldest);
}

static bool too_far_future(const MdkrNetInputHistory *history, uint32_t tick) {
    return mdkr_net_tick_after(tick, history->current_tick + MDKR_NET_INPUT_MAX_FUTURE);
}

MdkrNetInputSubmitResult mdkr_net_input_submit(
    MdkrNetInputHistory *history, unsigned slot, uint32_t tick,
    const MdkrInputSample *sample) {
    MdkrNetInputCell *cell;
    MdkrInputSample prior;
    uint8_t prior_status;
    if (history == NULL || sample == NULL || slot >= MDKR_NET_INPUT_SLOTS) {
        return MDKR_NET_SUBMIT_INVALID_SLOT;
    }
    if (sample->stick_x < -80 || sample->stick_x > 80 ||
        sample->stick_y < -80 || sample->stick_y > 80) {
        return MDKR_NET_SUBMIT_CONFLICT;
    }
    if (too_old(history, tick)) return MDKR_NET_SUBMIT_TOO_OLD;
    if (too_far_future(history, tick)) return MDKR_NET_SUBMIT_TOO_FAR_FUTURE;
    cell = cell_for(history, tick, true);
    prior = cell->samples[slot];
    prior_status = cell->status[slot];
    if (prior_status == MDKR_NET_INPUT_RECEIVED) {
        return same_sample(prior, *sample)
            ? MDKR_NET_SUBMIT_DUPLICATE : MDKR_NET_SUBMIT_CONFLICT;
    }
    cell->samples[slot] = *sample;
    cell->status[slot] = MDKR_NET_INPUT_RECEIVED;
    if (prior_status == MDKR_NET_INPUT_PREDICTED && !same_sample(prior, *sample) &&
        (tick == history->current_tick || tick_before(tick, history->current_tick))) {
        uint32_t future = tick + 1u;
        while (future == history->current_tick || tick_before(future, history->current_tick)) {
            MdkrNetInputCell *dependent = cell_for(history, future, false);
            if (dependent != NULL) {
                if (dependent->status[slot] == MDKR_NET_INPUT_RECEIVED) break;
                if (dependent->status[slot] == MDKR_NET_INPUT_PREDICTED) {
                    dependent->status[slot] = MDKR_NET_INPUT_ABSENT;
                    dependent->samples[slot] = neutral();
                }
            }
            if (future == history->current_tick) break;
            future++;
        }
        if (!history->have_dirty || tick_before(tick, history->earliest_dirty_tick)) {
            history->earliest_dirty_tick = tick;
            history->have_dirty = true;
        }
        return MDKR_NET_SUBMIT_CORRECTED;
    }
    return MDKR_NET_SUBMIT_ACCEPTED;
}

static MdkrInputSample prediction(
    MdkrNetInputHistory *history, unsigned slot, uint32_t tick) {
    uint32_t distance;
    for (distance = 1u; distance < MDKR_NET_INPUT_CAPACITY; distance++) {
        const uint32_t prior_tick = tick - distance;
        MdkrNetInputCell *prior = cell_for(history, prior_tick, false);
        if (prior != NULL && prior->status[slot] != MDKR_NET_INPUT_ABSENT) {
            return prior->samples[slot];
        }
    }
    return neutral();
}

bool mdkr_net_input_for_tick(
    MdkrNetInputHistory *history, uint32_t tick, MdkrNetInputSet *out) {
    MdkrNetInputCell *cell;
    unsigned slot;
    if (history == NULL || out == NULL || too_old(history, tick) ||
        too_far_future(history, tick)) return false;
    cell = cell_for(history, tick, true);
    out->tick = tick;
    out->all_received = true;
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        if (cell->status[slot] == MDKR_NET_INPUT_ABSENT) {
            cell->samples[slot] = prediction(history, slot, tick);
            cell->status[slot] = MDKR_NET_INPUT_PREDICTED;
        }
        out->samples[slot] = cell->samples[slot];
        out->status[slot] = cell->status[slot];
        if (cell->status[slot] != MDKR_NET_INPUT_RECEIVED) out->all_received = false;
    }
    return true;
}

bool mdkr_net_input_copy_existing(
    const MdkrNetInputHistory *history, uint32_t tick, MdkrNetInputSet *out) {
    const MdkrNetInputCell *cell;
    unsigned slot;
    if (history == NULL || out == NULL ||
        mdkr_net_tick_after(tick, history->current_tick)) {
        return false;
    }
    cell = &history->cells[tick % MDKR_NET_INPUT_CAPACITY];
    if (!cell->occupied || cell->tick != tick) return false;
    out->tick = tick;
    out->all_received = true;
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        if (cell->status[slot] == MDKR_NET_INPUT_ABSENT) return false;
        out->samples[slot] = cell->samples[slot];
        out->status[slot] = cell->status[slot];
        if (cell->status[slot] != MDKR_NET_INPUT_RECEIVED) {
            out->all_received = false;
        }
    }
    return true;
}

bool mdkr_net_input_rebuild_predictions(
    MdkrNetInputHistory *history, uint32_t first_tick, uint32_t last_tick) {
    MdkrNetInputHistory rebuilt;
    uint32_t tick;
    if (history == NULL || mdkr_net_tick_after(first_tick, last_tick) ||
        mdkr_net_tick_after(last_tick, history->current_tick) ||
        too_old(history, first_tick)) return false;
    rebuilt = *history;
    tick = first_tick;
    for (;;) {
        MdkrNetInputCell *cell = cell_for(&rebuilt, tick, false);
        unsigned slot;
        if (cell == NULL) return false;
        for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
            if (cell->status[slot] == MDKR_NET_INPUT_PREDICTED) {
                cell->status[slot] = MDKR_NET_INPUT_ABSENT;
                cell->samples[slot] = neutral();
            }
        }
        if (tick == last_tick) break;
        tick++;
    }
    tick = first_tick;
    for (;;) {
        MdkrNetInputSet ignored;
        if (!mdkr_net_input_for_tick(&rebuilt, tick, &ignored)) return false;
        if (tick == last_tick) break;
        tick++;
    }
    *history = rebuilt;
    return true;
}

bool mdkr_net_input_confirm_through(
    MdkrNetInputHistory *history, uint32_t tick) {
    uint32_t cursor;
    if (history == NULL || mdkr_net_tick_after(tick, history->current_tick)) return false;
    cursor = history->have_confirmed ? history->confirmed_through + 1u
                                     : history->first_tick;
    while (cursor == tick || tick_before(cursor, tick)) {
        MdkrNetInputCell *cell = cell_for(history, cursor, false);
        unsigned slot;
        if (cell == NULL) return false;
        for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
            if (cell->status[slot] != MDKR_NET_INPUT_RECEIVED) return false;
        }
        if (cursor == tick) break;
        cursor++;
    }
    history->confirmed_through = tick;
    history->have_confirmed = true;
    return true;
}

bool mdkr_net_input_take_dirty(
    MdkrNetInputHistory *history, uint32_t *out_tick) {
    if (history == NULL || out_tick == NULL || !history->have_dirty) return false;
    *out_tick = history->earliest_dirty_tick;
    history->have_dirty = false;
    return true;
}
