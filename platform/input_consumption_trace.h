/* Pointer-free trace of the controller sample consumed by each game tick. */
#ifndef MDKR_INPUT_CONSUMPTION_TRACE_H
#define MDKR_INPUT_CONSUMPTION_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool input_consumption_trace_enabled(void);
void input_consumption_trace_observe(
    unsigned port, uint16_t held, uint16_t pressed, uint16_t released,
    int8_t stick_x, int8_t stick_y, bool present);
void input_consumption_trace_tick(uint64_t tick);
void input_consumption_trace_summary(void);

#define INPUT_CONSUMPTION_TRACE(port, held, pressed, released, sx, sy, present) \
    do {                                                                         \
        if (input_consumption_trace_enabled()) {                                 \
            input_consumption_trace_observe(                                    \
                (port), (held), (pressed), (released), (sx), (sy), (present));  \
        }                                                                        \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* MDKR_INPUT_CONSUMPTION_TRACE_H */
