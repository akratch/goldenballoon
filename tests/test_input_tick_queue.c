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

static void test_same_ticket_reconnect_controls(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 5, sample(0, 0, 0, 0));
    mdkr_input_tick_queue_capture(
        &queue, 0, 5, sample(A_BUTTON, 50, -25, 1));
    mdkr_input_tick_queue_consume(&queue, 5, output);
    expect("same-ticket disconnect publishes one neutral ticket",
           !output[0].present && output[0].buttons == 0 &&
           output[0].stick_x == 0 && output[0].stick_y == 0);
    expect("reconnect controls remain queued while port is absent",
           mdkr_input_tick_queue_pending_edges(&queue, 0) >= 2u);
    mdkr_input_tick_queue_consume(&queue, 6, output);
    expect("active reconnect controls publish with reconnect",
           output[0].present && output[0].buttons == A_BUTTON &&
           output[0].stick_x == 50 && output[0].stick_y == -25);
}

static void test_same_ticket_reconnect_tap(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    mdkr_input_tick_queue_capture(&queue, 0, 9, sample(0, 0, 0, 0));
    mdkr_input_tick_queue_capture(
        &queue, 0, 9, sample(A_BUTTON, 0, 0, 1));
    mdkr_input_tick_queue_capture(&queue, 0, 9, sample(0, 0, 0, 1));
    mdkr_input_tick_queue_consume(&queue, 9, output);
    expect("reconnect tap waits through disconnected ticket",
           !output[0].present && output[0].buttons == 0);
    mdkr_input_tick_queue_consume(&queue, 10, output);
    expect("reconnect tap press is visible after presence returns",
           output[0].present && output[0].buttons == A_BUTTON);
    mdkr_input_tick_queue_consume(&queue, 11, output);
    expect("reconnect tap release follows on the next ticket",
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

    for (edge = 0; edge < MDKR_INPUT_EDGE_CAPACITY + 1u; edge++) {
        mdkr_input_tick_queue_capture(
            &queue, 0, 10,
            sample((edge & 1u) == 0u ? A_BUTTON : 0, 40, -20, 1));
    }
    mdkr_input_tick_queue_consume(&queue, 10, output);
    expect("overflow neutralizes instead of sticking a button",
           output[0].buttons == 0);
    expect("overflow policy is observable",
           queue.stats.overflow_neutralizations == 1u);
    expect("overflow leaves queue bounded",
           mdkr_input_tick_queue_pending_edges(&queue, 0) <=
               MDKR_INPUT_EDGE_CAPACITY);
    expect("overflow requests a complete held-state resync",
           queue.ports[0].resync_pending);

    /* The physical controller is still holding exactly the overflowing sample:
     * no release/repress and no stick movement occurs. The next periodic full
     * capture must reconstruct all lanes after the one neutral ticket. */
    mdkr_input_tick_queue_capture(
        &queue, 0, 11, sample(A_BUTTON, 40, -20, 1));
    mdkr_input_tick_queue_consume(&queue, 11, output);
    expect("identical held button recovers after overflow neutral",
           output[0].present && output[0].buttons == A_BUTTON);
    expect("held analog recovers after overflow neutral",
           output[0].stick_x == 40 && output[0].stick_y == -20);
    expect("successful recovery clears the resync request",
           !queue.ports[0].resync_pending);
}

static void test_presence_overflow_resynchronizes(void) {
    MdkrInputTickQueue queue = fresh_queue();
    MdkrInputSample output[MDKR_INPUT_PORTS];

    /* Build the otherwise rare capacity boundary directly: disconnect policy
     * normally clears stale presence edges, but a bounded queue must still be
     * correct if a reconnect arrives while this lane is full. */
    queue.ports[0].observed = sample(0, 0, 0, 0);
    queue.ports[0].published = sample(0, 0, 0, 0);
    queue.ports[0].presence.count = MDKR_INPUT_EDGE_CAPACITY;
    mdkr_input_tick_queue_capture(
        &queue, 0, 20, sample(B_BUTTON, -35, 25, 1));
    mdkr_input_tick_queue_consume(&queue, 20, output);
    expect("presence overflow publishes an absent neutral ticket",
           !output[0].present && output[0].buttons == 0 &&
           output[0].stick_x == 0 && output[0].stick_y == 0);
    expect("presence overflow requests full resync",
           queue.ports[0].resync_pending);

    mdkr_input_tick_queue_capture(
        &queue, 0, 21, sample(B_BUTTON, -35, 25, 1));
    mdkr_input_tick_queue_consume(&queue, 21, output);
    expect("identical connected state recovers after presence overflow",
           output[0].present);
    expect("presence recovery carries its held button and analog lanes",
           output[0].buttons == B_BUTTON &&
           output[0].stick_x == -35 && output[0].stick_y == 25);
}

int main(void) {
    test_ordinary_edges();
    test_tap_stretch();
    test_independent_buttons_and_ports();
    test_analog_policy();
    test_lifecycle();
    test_same_ticket_reconnect_controls();
    test_same_ticket_reconnect_tap();
    test_target_order_and_catchup();
    test_overflow_fails_neutral();
    test_presence_overflow_resynchronizes();
    if (s_failures != 0) {
        fprintf(stderr, "%d input-tick-queue test(s) failed\n", s_failures);
        return 1;
    }
    puts("input tick queue: PASS");
    return 0;
}
