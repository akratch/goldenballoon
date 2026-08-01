#include "input_consumption_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_TRACE_PORTS 4u
#define INPUT_HASH_OFFSET UINT64_C(14695981039346656037)
#define INPUT_HASH_PRIME  UINT64_C(1099511628211)

typedef struct InputTraceSample {
    uint16_t held;
    uint16_t pressed;
    uint16_t released;
    int8_t stick_x;
    int8_t stick_y;
    bool present;
} InputTraceSample;

static InputTraceSample s_samples[INPUT_TRACE_PORTS];
static int s_enabled = -1;
static int64_t s_perturb_tick = -1;
static uint64_t s_ticks;
static uint64_t s_active_ticks;
static uint64_t s_pressed_edges;
static uint64_t s_released_edges;
static uint64_t s_presence_changes;
static uint8_t s_last_presence;
static bool s_presence_valid;

bool input_consumption_trace_enabled(void) {
    if (s_enabled < 0) {
        const char *value = getenv("MDKR_INPUT_HASH");
        const char *perturb = getenv("MDKR_TEST_INPUT_PERTURB");
        char *end = NULL;
        s_enabled = value != NULL && value[0] != '\0' &&
                    strcmp(value, "0") != 0;
        if (perturb != NULL && perturb[0] != '\0') {
            long long parsed = strtoll(perturb, &end, 10);
            if (end != perturb && *end == '\0' && parsed >= 0) {
                s_perturb_tick = (int64_t)parsed;
            }
        }
    }
    return s_enabled != 0;
}

void input_consumption_trace_observe(
    unsigned port, uint16_t held, uint16_t pressed, uint16_t released,
    int8_t stick_x, int8_t stick_y, bool present) {
    if (!input_consumption_trace_enabled() || port >= INPUT_TRACE_PORTS) {
        return;
    }
    s_samples[port].held = held;
    s_samples[port].pressed = pressed;
    s_samples[port].released = released;
    s_samples[port].stick_x = stick_x;
    s_samples[port].stick_y = stick_y;
    s_samples[port].present = present;
}

static void hash_u32(uint64_t *hash, uint32_t value) {
    unsigned byte;
    for (byte = 0; byte < 4u; byte++) {
        *hash ^= (uint8_t)(value >> (byte * 8u));
        *hash *= INPUT_HASH_PRIME;
    }
}

static unsigned popcount_u16(uint16_t value) {
    unsigned count = 0;
    while (value != 0u) {
        value &= (uint16_t)(value - 1u);
        count++;
    }
    return count;
}

void input_consumption_trace_tick(uint64_t tick) {
    uint64_t hash = INPUT_HASH_OFFSET;
    uint8_t presence = 0;
    unsigned port;
    bool active = false;
    if (!input_consumption_trace_enabled()) {
        return;
    }
    hash_u32(&hash, (uint32_t)tick);
    hash_u32(&hash, (uint32_t)(tick >> 32));
    for (port = 0; port < INPUT_TRACE_PORTS; port++) {
        const InputTraceSample *sample = &s_samples[port];
        hash_u32(&hash, sample->held);
        hash_u32(&hash, sample->pressed);
        hash_u32(&hash, sample->released);
        hash_u32(&hash, (uint8_t)sample->stick_x);
        hash_u32(&hash, (uint8_t)sample->stick_y);
        hash_u32(&hash, sample->present ? 1u : 0u);
        if (sample->present) {
            presence |= (uint8_t)(1u << port);
        }
        if (sample->held != 0u || sample->pressed != 0u ||
            sample->released != 0u || sample->stick_x != 0 ||
            sample->stick_y != 0) {
            active = true;
        }
        s_pressed_edges += (uint64_t)popcount_u16(sample->pressed);
        s_released_edges += (uint64_t)popcount_u16(sample->released);
    }
    if (s_perturb_tick >= 0 && tick == (uint64_t)s_perturb_tick) {
        hash_u32(&hash, UINT32_C(0x494e5054));
    }
    if (s_presence_valid && presence != s_last_presence) {
        s_presence_changes++;
    }
    s_presence_valid = true;
    s_last_presence = presence;
    s_ticks++;
    if (active) {
        s_active_ticks++;
    }
    fprintf(stderr,
            "[INPUTHASH] v=1 tick=%" PRIu64 " hash=%016" PRIx64
            " p1=%u,%04x,%04x,%04x,%d,%d"
            " p2=%u,%04x,%04x,%04x,%d,%d"
            " p3=%u,%04x,%04x,%04x,%d,%d"
            " p4=%u,%04x,%04x,%04x,%d,%d\n",
            tick, hash,
            s_samples[0].present, s_samples[0].held,
            s_samples[0].pressed, s_samples[0].released,
            s_samples[0].stick_x, s_samples[0].stick_y,
            s_samples[1].present, s_samples[1].held,
            s_samples[1].pressed, s_samples[1].released,
            s_samples[1].stick_x, s_samples[1].stick_y,
            s_samples[2].present, s_samples[2].held,
            s_samples[2].pressed, s_samples[2].released,
            s_samples[2].stick_x, s_samples[2].stick_y,
            s_samples[3].present, s_samples[3].held,
            s_samples[3].pressed, s_samples[3].released,
            s_samples[3].stick_x, s_samples[3].stick_y);
    fflush(stderr);
}

void input_consumption_trace_summary(void) {
    if (!input_consumption_trace_enabled()) {
        return;
    }
    fprintf(stderr,
            "[INPUTHASH-SUMMARY] v=1 ticks=%" PRIu64
            " active=%" PRIu64 " pressed=%" PRIu64
            " released=%" PRIu64 " presence=%" PRIu64 "\n",
            s_ticks, s_active_ticks, s_pressed_edges,
            s_released_edges, s_presence_changes);
    fflush(stderr);
}
