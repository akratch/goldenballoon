#include "input_tick_queue.h"

#include <stdio.h>
#include <string.h>

#define A_BUTTON UINT16_C(0x8000)
#define B_BUTTON UINT16_C(0x4000)

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static MdkrInputTickQueue fresh_queue(void) {
    MdkrInputTickQueue queue;
    MdkrInputSample initial[MDKR_INPUT_PORTS];
    memset(initial, 0, sizeof(initial));
    initial[0].present = true;
    mdkr_input_tick_queue_init(&queue, initial);
    return queue;
}

static MdkrInputSample sample(
    uint16_t buttons, int stick_x, int stick_y, int present) {
    MdkrInputSample value;
    value.buttons = buttons;
    value.stick_x = (int8_t)stick_x;
    value.stick_y = (int8_t)stick_y;
    value.present = present != 0;
    return value;
}

static void test_ordinary_edges(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 1, sample(A_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 1, output);
    expect("press reaches its target tick", output[0].buttons == A_BUTTON);
    mdkr_input_tick_queue_consume(&queue, 2, output);
    expect("held button remains held", output[0].buttons == A_BUTTON);
    mdkr_input_tick_queue_capture(&queue, 0, 3, sample(0, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 3, output);
    expect("release reaches its target tick", output[0].buttons == 0);
}

static void test_tap_stretch(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 4, sample(A_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_capture(&queue, 0, 4, sample(0, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 4, output);
    expect("between-tick tap is visible for one tick",
           output[0].buttons == A_BUTTON);
    mdkr_input_tick_queue_consume(&queue, 5, output);
    expect("stretched tap releases on following tick",
           output[0].buttons == 0);
    expect("tap stretch is observable", queue.stats.stretched_edges != 0u);
}

static void test_independent_buttons_and_ports(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(
        &queue, 0, 1, sample(A_BUTTON | B_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_capture(&queue, 0, 1, sample(B_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_capture(&queue, 1, 1, sample(A_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 1, output);
    expect("one button can tap while another holds",
           output[0].buttons == (A_BUTTON | B_BUTTON));
    expect("second port publishes independently",
           output[1].present && output[1].buttons == A_BUTTON);
    mdkr_input_tick_queue_consume(&queue, 2, output);
    expect("only the tapped button releases",
           output[0].buttons == B_BUTTON);
}

static void test_analog_policy(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];
    unsigned index;

    for (index = 0; index < 34u; index++) {
        mdkr_input_tick_queue_capture(
            &queue, 0, 7, sample(0, (int)index - 17, 17 - (int)index, 1));
    }
    mdkr_input_tick_queue_consume(&queue, 7, output);
    expect("latest analog sample wins within a tick",
           output[0].stick_x == 16 && output[0].stick_y == -16);
    expect("same-tick analog samples coalesce",
           queue.stats.analog_coalesced >= 32u);
}

static void test_lifecycle(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 1, sample(A_BUTTON, 50, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 1, output);
    mdkr_input_tick_queue_capture(&queue, 0, 2, sample(0, 0, 0, 0));
    mdkr_input_tick_queue_consume(&queue, 2, output);
    expect("disconnect neutralizes held input",
           !output[0].present && output[0].buttons == 0 &&
           output[0].stick_x == 0);
    mdkr_input_tick_queue_capture(&queue, 0, 3, sample(0, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 3, output);
    expect("neutral reconnect has no phantom press",
           output[0].present && output[0].buttons == 0);
}

static void test_target_order_and_catchup(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 3, sample(A_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 1, output);
    expect("future catch-up input stays pending", output[0].buttons == 0);
    mdkr_input_tick_queue_consume(&queue, 2, output);
    expect("future input still waits for logical interval",
           output[0].buttons == 0);
    mdkr_input_tick_queue_consume(&queue, 3, output);
    expect("catch-up input lands on assigned ticket",
           output[0].buttons == A_BUTTON);
    mdkr_input_tick_queue_capture(&queue, 0, 2, sample(0, 0, 0, 1));
    expect("out-of-order target is normalized",
           queue.stats.reordered_targets == 1u);
}

static void test_overflow_fails_neutral(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];
    unsigned edge;

    for (edge = 0; edge < MDKR_INPUT_EDGE_CAPACITY + 2u; edge++) {
        mdkr_input_tick_queue_capture(
            &queue, 0, 10,
            sample((edge & 1u) == 0u ? A_BUTTON : 0, 0, 0, 1));
    }
    mdkr_input_tick_queue_consume(&queue, 10, output);
    expect("overflow neutralizes instead of sticking a button",
           output[0].buttons == 0);
    expect("overflow policy is observable",
           queue.stats.overflow_neutralizations == 1u);
    expect("overflow leaves queue bounded",
           mdkr_input_tick_queue_pending_edges(&queue, 0) <=
               MDKR_INPUT_EDGE_CAPACITY);
}

int main(void) {
    test_ordinary_edges();
    test_tap_stretch();
    test_independent_buttons_and_ports();
    test_analog_policy();
    test_lifecycle();
    test_target_order_and_catchup();
    test_overflow_fails_neutral();
    if (s_failures != 0) {
        fprintf(stderr, "%d input-tick-queue test(s) failed\n", s_failures);
        return 1;
    }
    puts("input tick queue: PASS");
    return 0;
}
