/* Bounded host-input to authoritative-tick adapter. */
#ifndef MDKR_INPUT_TICK_QUEUE_H
#define MDKR_INPUT_TICK_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_INPUT_PORTS 4u
#define MDKR_INPUT_BUTTON_BITS 16u
#define MDKR_INPUT_EDGE_CAPACITY 64u
#define MDKR_INPUT_SAMPLE_CAPACITY 64u

typedef struct MdkrInputSample {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    bool present;
} MdkrInputSample;

typedef struct MdkrInputTransition {
    uint64_t target_tick;
    uint8_t value;
} MdkrInputTransition;

typedef struct MdkrInputTransitionLane {
    MdkrInputTransition entries[MDKR_INPUT_EDGE_CAPACITY];
    uint8_t head;
    uint8_t count;
} MdkrInputTransitionLane;

typedef struct MdkrInputAnalogSample {
    uint64_t target_tick;
    int8_t stick_x;
    int8_t stick_y;
} MdkrInputAnalogSample;

typedef struct MdkrInputAnalogLane {
    MdkrInputAnalogSample entries[MDKR_INPUT_SAMPLE_CAPACITY];
    uint8_t head;
    uint8_t count;
} MdkrInputAnalogLane;

typedef struct MdkrInputPortQueue {
    MdkrInputTransitionLane buttons[MDKR_INPUT_BUTTON_BITS];
    MdkrInputTransitionLane presence;
    MdkrInputAnalogLane analog;
    MdkrInputSample observed;
    MdkrInputSample published;
    uint64_t last_target_tick;
    bool force_neutral;
    /* Overflow deliberately publishes one neutral ticket. The next complete
     * host snapshot must then be compared against that neutral baseline even
     * when the physical controller never released its held state. */
    bool resync_pending;
} MdkrInputPortQueue;

typedef struct MdkrInputQueueStats {
    uint64_t captures;
    uint64_t ticks;
    uint64_t button_edges;
    uint64_t stretched_edges;
    uint64_t analog_samples;
    uint64_t analog_coalesced;
    uint64_t presence_edges;
    uint64_t overflow_neutralizations;
    uint64_t reordered_targets;
    uint64_t max_button_depth;
    uint64_t max_analog_depth;
} MdkrInputQueueStats;

typedef struct MdkrInputTickQueue {
    MdkrInputPortQueue ports[MDKR_INPUT_PORTS];
    MdkrInputQueueStats stats;
} MdkrInputTickQueue;

/* Seed all ports without creating edges. */
void mdkr_input_tick_queue_init(
    MdkrInputTickQueue *queue,
    const MdkrInputSample initial[MDKR_INPUT_PORTS]);

/* Capture one complete host-side port snapshot for a logical ticket. */
void mdkr_input_tick_queue_capture(
    MdkrInputTickQueue *queue, unsigned port, uint64_t target_tick,
    MdkrInputSample sample);

/* Publish exactly one DKR-visible sample per port for this ticket. */
void mdkr_input_tick_queue_consume(
    MdkrInputTickQueue *queue, uint64_t tick,
    MdkrInputSample output[MDKR_INPUT_PORTS]);

unsigned mdkr_input_tick_queue_pending_edges(
    const MdkrInputTickQueue *queue, unsigned port);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_INPUT_TICK_QUEUE_H */
