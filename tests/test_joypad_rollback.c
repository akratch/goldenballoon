/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "PR/os_cont.h"
#include "game/src/joypad.h"

static void expect_edges(
    MdkrInputSample current, MdkrInputSample previous, u16 mask,
    u16 expected_pressed, u16 expected_released) {
    u16 pressed = 0xFFFFu;
    u16 released = 0xFFFFu;
    input_rollback_compute_edges(
        &current, &previous, mask, &pressed, &released);
    assert(pressed == expected_pressed);
    assert(released == expected_released);
}

int main(void) {
    const MdkrInputSample absent = {0u, 0, 0, false};
    const MdkrInputSample a = {A_BUTTON, 20, -10, true};
    const MdkrInputSample ab = {A_BUTTON | B_BUTTON, 20, -10, true};
    const MdkrInputSample b = {B_BUTTON, 20, -10, true};

    expect_edges(a, absent, 0xFFFFu, A_BUTTON, 0u);
    expect_edges(absent, a, 0xFFFFu, 0u, A_BUTTON);
    expect_edges(ab, a, 0xFFFFu, B_BUTTON, 0u);
    expect_edges(b, ab, 0xFFFFu, 0u, A_BUTTON);
    expect_edges(ab, a, A_BUTTON, 0u, 0u);
    expect_edges(absent, absent, 0xFFFFu, 0u, 0u);

    input_rollback_compute_edges(&a, &b, 0xFFFFu, NULL, NULL);
    puts("test_joypad_rollback: PASS");
    return 0;
}
