#include "controller_mapping.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *message, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        failures++;
    }
}

static void press(MdkrControllerDigitalState *state,
                  MdkrControllerSource source) {
    memset(state, 0, sizeof(*state));
    state->active[source] = 1;
}

int main(void) {
    MdkrVideoConfig config;
    MdkrControllerDigitalState state;
    static const uint16_t expected_defaults[MDKR_CONTROLLER_SOURCE_COUNT] = {
        MDKR_N64_A, MDKR_N64_B, MDKR_N64_B, MDKR_N64_CU, MDKR_N64_START,
        0, 0, MDKR_N64_L, MDKR_N64_R,
        MDKR_N64_DU, MDKR_N64_DD, MDKR_N64_DL, MDKR_N64_DR,
        MDKR_N64_Z, MDKR_N64_Z,
        MDKR_N64_CU, MDKR_N64_CD, MDKR_N64_CL, MDKR_N64_CR,
    };

    mdkr_video_config_defaults(&config);

    for (int source = 0; source < MDKR_CONTROLLER_SOURCE_COUNT; ++source) {
        char message[80];
        press(&state, (MdkrControllerSource)source);
        snprintf(message, sizeof(message),
                 "normalized source %d preserves its old default", source);
        expect(message,
               mdkr_controller_mapped_buttons(&config, &state) ==
                   expected_defaults[source]);
    }

    press(&state, MDKR_CONTROLLER_SOURCE_A);
    expect("A defaults to N64 A",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_A);
    press(&state, MDKR_CONTROLLER_SOURCE_B);
    expect("B defaults to N64 B",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_B);
    press(&state, MDKR_CONTROLLER_SOURCE_X);
    expect("X preserves alternate N64 B binding",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_B);
    press(&state, MDKR_CONTROLLER_SOURCE_LEFT_TRIGGER);
    expect("left trigger preserves alternate N64 Z binding",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_Z);
    press(&state, MDKR_CONTROLLER_SOURCE_RIGHT_TRIGGER);
    expect("right trigger defaults to N64 Z",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_Z);
    press(&state, MDKR_CONTROLLER_SOURCE_Y);
    expect("Y defaults to C-up",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_CU);
    press(&state, MDKR_CONTROLLER_SOURCE_RIGHT_STICK_DOWN);
    expect("right stick down defaults to C-down",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_CD);
    press(&state, MDKR_CONTROLLER_SOURCE_LEFT_STICK);
    expect("stick clicks default to unbound",
           mdkr_controller_mapped_buttons(&config, &state) == 0);

    expect("face button remap validates",
           mdkr_video_config_set(&config, MDKR_INPUT_CONTROLLER_A, "r",
                                 MDKR_VIDEO_SOURCE_RUNTIME));
    press(&state, MDKR_CONTROLLER_SOURCE_A);
    expect("face button remap reaches requested N64 action",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_R);
    expect("unbound remap validates",
           mdkr_video_config_set(&config, MDKR_INPUT_CONTROLLER_A, "none",
                                 MDKR_VIDEO_SOURCE_RUNTIME));
    expect("unbound source produces no N64 input",
           mdkr_controller_mapped_buttons(&config, &state) == 0);

    memset(&state, 0, sizeof(state));
    state.active[MDKR_CONTROLLER_SOURCE_A] = 1;
    state.active[MDKR_CONTROLLER_SOURCE_DPAD_UP] = 1;
    expect("simultaneous sources combine",
           mdkr_controller_mapped_buttons(&config, &state) == MDKR_N64_DU);

    expect("rumble defaults enabled", mdkr_controller_rumble_enabled(&config));
    expect("rumble defaults preserve full amplitude",
           mdkr_controller_rumble_strength(&config) == UINT16_MAX);
    expect("light profile validates",
           mdkr_video_config_set(&config, MDKR_INPUT_RUMBLE_PROFILE, "light",
                                 MDKR_VIDEO_SOURCE_RUNTIME));
    expect("light profile lowers amplitude",
           mdkr_controller_rumble_strength(&config) == 22937u);
    expect("active light request reaches the selected amplitude",
           mdkr_controller_rumble_output_strength(&config, 1) == 22937u);
    expect("stopped request always resolves to zero amplitude",
           mdkr_controller_rumble_output_strength(&config, 0) == 0u);
    expect("balanced profile validates",
           mdkr_video_config_set(&config, MDKR_INPUT_RUMBLE_PROFILE, "balanced",
                                 MDKR_VIDEO_SOURCE_RUNTIME));
    expect("balanced profile is between light and strong",
           mdkr_controller_rumble_strength(&config) == 42598u);
    expect("rumble disable validates",
           mdkr_video_config_set(&config, MDKR_INPUT_RUMBLE_ENABLED, "0",
                                 MDKR_VIDEO_SOURCE_RUNTIME));
    expect("rumble disable reaches host policy",
           !mdkr_controller_rumble_enabled(&config));
    expect("disabled rumble mutes an active game request",
           mdkr_controller_rumble_output_strength(&config, 1) == 0u);

    if (failures)
        return 1;
    printf("controller mapping policies passed\n");
    return 0;
}
