#include "input_tick_queue.h"

#include <string.h>

static bool sample_equal(MdkrInputSample left, MdkrInputSample right) {
    return left.buttons == right.buttons &&
           left.stick_x == right.stick_x &&
           left.stick_y == right.stick_y &&
           left.present == right.present;
}

static void clear_port_lanes(MdkrInputPortQueue *port) {
    unsigned bit;
    for (bit = 0; bit < MDKR_INPUT_BUTTON_BITS; bit++) {
        port->buttons[bit].head = 0;
        port->buttons[bit].count = 0;
    }
    port->presence.head = 0;
    port->presence.count = 0;
    port->analog.head = 0;
    port->analog.count = 0;
}

static void force_port_neutral(
    MdkrInputTickQueue *queue, MdkrInputPortQueue *port) {
    clear_port_lanes(port);
    port->observed.buttons = 0;
    port->observed.stick_x = 0;
    port->observed.stick_y = 0;
    port->force_neutral = true;
    port->resync_pending = true;
    queue->stats.overflow_neutralizations++;
}

static bool transition_push(
    MdkrInputTransitionLane *lane, uint64_t target_tick, uint8_t value) {
    unsigned tail;
    if (lane->count >= MDKR_INPUT_EDGE_CAPACITY) {
        return false;
    }
    tail = ((unsigned)lane->head + (unsigned)lane->count) %
           MDKR_INPUT_EDGE_CAPACITY;
    lane->entries[tail].target_tick = target_tick;
    lane->entries[tail].value = value;
    lane->count++;
    return true;
}

static bool transition_pop_due(
    MdkrInputTransitionLane *lane, uint64_t tick, uint8_t *value,
    bool *stretched) {
    MdkrInputTransition *entry;
    if (lane->count == 0u) {
        return false;
    }
    entry = &lane->entries[lane->head];
    if (entry->target_tick > tick) {
        return false;
    }
    if (value != NULL) {
        *value = entry->value;
    }
    lane->head = (uint8_t)(((unsigned)lane->head + 1u) %
                           MDKR_INPUT_EDGE_CAPACITY);
    lane->count--;
    if (stretched != NULL) {
        *stretched = lane->count != 0u &&
            lane->entries[lane->head].target_tick <= tick;
    }
    return true;
}

static void analog_push(
    MdkrInputTickQueue *queue, MdkrInputAnalogLane *lane,
    uint64_t target_tick, int8_t stick_x, int8_t stick_y) {
    unsigned tail;
    if (lane->count != 0u) {
        tail = ((unsigned)lane->head + (unsigned)lane->count - 1u) %
               MDKR_INPUT_SAMPLE_CAPACITY;
        if (lane->entries[tail].target_tick == target_tick) {
            lane->entries[tail].stick_x = stick_x;
            lane->entries[tail].stick_y = stick_y;
            queue->stats.analog_coalesced++;
            return;
        }
    }
    if (lane->count >= MDKR_INPUT_SAMPLE_CAPACITY) {
        /* Analog is a sampled value, not an edge stream: replacing the oldest
         * sample is the documented bounded policy and preserves the newest
         * value for each future tick. */
        lane->head = (uint8_t)(((unsigned)lane->head + 1u) %
                               MDKR_INPUT_SAMPLE_CAPACITY);
        lane->count--;
        queue->stats.analog_coalesced++;
    }
    tail = ((unsigned)lane->head + (unsigned)lane->count) %
           MDKR_INPUT_SAMPLE_CAPACITY;
    lane->entries[tail].target_tick = target_tick;
    lane->entries[tail].stick_x = stick_x;
    lane->entries[tail].stick_y = stick_y;
    lane->count++;
    queue->stats.analog_samples++;
    if (lane->count > queue->stats.max_analog_depth) {
        queue->stats.max_analog_depth = lane->count;
    }
}

void mdkr_input_tick_queue_init(
    MdkrInputTickQueue *queue,
    const MdkrInputSample initial[MDKR_INPUT_PORTS]) {
    unsigned port;
    memset(queue, 0, sizeof(*queue));
    for (port = 0; port < MDKR_INPUT_PORTS; port++) {
        MdkrInputSample sample = { 0, 0, 0, false };
        if (initial != NULL) {
            sample = initial[port];
        }
        if (!sample.present) {
            sample.buttons = 0;
            sample.stick_x = 0;
            sample.stick_y = 0;
        }
        queue->ports[port].observed = sample;
        queue->ports[port].published = sample;
    }
}

void mdkr_input_tick_queue_capture(
    MdkrInputTickQueue *queue, unsigned port_index, uint64_t target_tick,
    MdkrInputSample sample) {
    MdkrInputPortQueue *port;
    uint16_t changed;
    unsigned bit;

    if (queue == NULL || port_index >= MDKR_INPUT_PORTS) {
        return;
    }
    port = &queue->ports[port_index];
    queue->stats.captures++;
    if (target_tick < port->last_target_tick) {
        target_tick = port->last_target_tick;
        queue->stats.reordered_targets++;
    }
    port->last_target_tick = target_tick;
    if (!sample.present) {
        sample.buttons = 0;
        sample.stick_x = 0;
        sample.stick_y = 0;
    }
    if (!port->resync_pending && sample_equal(sample, port->observed)) {
        return;
    }

    /* A disconnect is an immediate fail-safe neutralization. It cannot leave
     * held controls behind or replay queued presses after reconnect. */
    if (port->observed.present && !sample.present) {
        clear_port_lanes(port);
        port->force_neutral = true;
        if (!transition_push(&port->presence, target_tick, 0u)) {
            force_port_neutral(queue, port);
            return;
        }
        queue->stats.presence_edges++;
        port->observed = sample;
        port->resync_pending = false;
        return;
    }

    if (sample.present != port->observed.present) {
        if (!transition_push(
                &port->presence, target_tick, sample.present ? 1u : 0u)) {
            force_port_neutral(queue, port);
            return;
        }
        queue->stats.presence_edges++;
    }

    changed = (uint16_t)(sample.buttons ^ port->observed.buttons);
    for (bit = 0; bit < MDKR_INPUT_BUTTON_BITS; bit++) {
        const uint16_t mask = (uint16_t)(1u << bit);
        MdkrInputTransitionLane *lane;
        if ((changed & mask) == 0u) {
            continue;
        }
        lane = &port->buttons[bit];
        if (!transition_push(
                lane, target_tick, (sample.buttons & mask) != 0u)) {
            force_port_neutral(queue, port);
            return;
        }
        queue->stats.button_edges++;
        if (lane->count > queue->stats.max_button_depth) {
            queue->stats.max_button_depth = lane->count;
        }
    }
    if (sample.stick_x != port->observed.stick_x ||
        sample.stick_y != port->observed.stick_y) {
        analog_push(
            queue, &port->analog, target_tick,
            sample.stick_x, sample.stick_y);
    }
    port->observed = sample;
    port->resync_pending = false;
}

static void consume_port(
    MdkrInputTickQueue *queue, MdkrInputPortQueue *port, uint64_t tick) {
    unsigned bit;
    uint8_t value;
    bool stretched;

    if (port->force_neutral) {
        port->published.buttons = 0;
        port->published.stick_x = 0;
        port->published.stick_y = 0;
        port->force_neutral = false;
    }
    if (transition_pop_due(
            &port->presence, tick, &value, &stretched)) {
        port->published.present = value != 0u;
        if (!port->published.present) {
            port->published.buttons = 0;
            port->published.stick_x = 0;
            port->published.stick_y = 0;
        }
        if (stretched) {
            queue->stats.stretched_edges++;
        }
    }
    /* Presence transitions are stretched one per logical ticket just like
     * button edges. If this ticket publishes the disconnected edge, preserve
     * reconnect controls in their lanes for the ticket that publishes the
     * reconnect instead of consuming them invisibly while the port is absent. */
    if (!port->published.present) {
        port->published.buttons = 0;
        port->published.stick_x = 0;
        port->published.stick_y = 0;
        return;
    }
    for (bit = 0; bit < MDKR_INPUT_BUTTON_BITS; bit++) {
        const uint16_t mask = (uint16_t)(1u << bit);
        if (!transition_pop_due(
                &port->buttons[bit], tick, &value, &stretched)) {
            continue;
        }
        if (value != 0u) {
            port->published.buttons |= mask;
        } else {
            port->published.buttons &= (uint16_t)~mask;
        }
        if (stretched) {
            queue->stats.stretched_edges++;
        }
    }
    while (port->analog.count != 0u) {
        MdkrInputAnalogSample *sample =
            &port->analog.entries[port->analog.head];
        if (sample->target_tick > tick) {
            break;
        }
        port->published.stick_x = sample->stick_x;
        port->published.stick_y = sample->stick_y;
        port->analog.head = (uint8_t)(((unsigned)port->analog.head + 1u) %
                                      MDKR_INPUT_SAMPLE_CAPACITY);
        port->analog.count--;
    }
    if (!port->published.present) {
        port->published.buttons = 0;
        port->published.stick_x = 0;
        port->published.stick_y = 0;
    }
}

void mdkr_input_tick_queue_consume(
    MdkrInputTickQueue *queue, uint64_t tick,
    MdkrInputSample output[MDKR_INPUT_PORTS]) {
    unsigned port;
    if (queue == NULL) {
        return;
    }
    queue->stats.ticks++;
    for (port = 0; port < MDKR_INPUT_PORTS; port++) {
        consume_port(queue, &queue->ports[port], tick);
        if (output != NULL) {
            output[port] = queue->ports[port].published;
        }
    }
}

unsigned mdkr_input_tick_queue_pending_edges(
    const MdkrInputTickQueue *queue, unsigned port) {
    unsigned bit;
    unsigned total = 0;
    if (queue == NULL || port >= MDKR_INPUT_PORTS) {
        return 0;
    }
    total += queue->ports[port].presence.count;
    for (bit = 0; bit < MDKR_INPUT_BUTTON_BITS; bit++) {
        total += queue->ports[port].buttons[bit].count;
    }
    return total;
}
