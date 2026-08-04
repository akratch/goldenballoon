/** controller_mapping.c — see controller_mapping.h. */
#include "controller_mapping.h"

#include <string.h>

static const MdkrVideoKey s_binding_keys[MDKR_CONTROLLER_SOURCE_COUNT] = {
    [MDKR_CONTROLLER_SOURCE_A] = MDKR_INPUT_CONTROLLER_A,
    [MDKR_CONTROLLER_SOURCE_B] = MDKR_INPUT_CONTROLLER_B,
    [MDKR_CONTROLLER_SOURCE_X] = MDKR_INPUT_CONTROLLER_X,
    [MDKR_CONTROLLER_SOURCE_Y] = MDKR_INPUT_CONTROLLER_Y,
    [MDKR_CONTROLLER_SOURCE_START] = MDKR_INPUT_CONTROLLER_START,
    [MDKR_CONTROLLER_SOURCE_LEFT_STICK] = MDKR_INPUT_CONTROLLER_LEFT_STICK,
    [MDKR_CONTROLLER_SOURCE_RIGHT_STICK] = MDKR_INPUT_CONTROLLER_RIGHT_STICK,
    [MDKR_CONTROLLER_SOURCE_LEFT_SHOULDER] =
        MDKR_INPUT_CONTROLLER_LEFT_SHOULDER,
    [MDKR_CONTROLLER_SOURCE_RIGHT_SHOULDER] =
        MDKR_INPUT_CONTROLLER_RIGHT_SHOULDER,
    [MDKR_CONTROLLER_SOURCE_DPAD_UP] = MDKR_INPUT_CONTROLLER_DPAD_UP,
    [MDKR_CONTROLLER_SOURCE_DPAD_DOWN] = MDKR_INPUT_CONTROLLER_DPAD_DOWN,
    [MDKR_CONTROLLER_SOURCE_DPAD_LEFT] = MDKR_INPUT_CONTROLLER_DPAD_LEFT,
    [MDKR_CONTROLLER_SOURCE_DPAD_RIGHT] = MDKR_INPUT_CONTROLLER_DPAD_RIGHT,
    [MDKR_CONTROLLER_SOURCE_LEFT_TRIGGER] = MDKR_INPUT_CONTROLLER_LEFT_TRIGGER,
    [MDKR_CONTROLLER_SOURCE_RIGHT_TRIGGER] =
        MDKR_INPUT_CONTROLLER_RIGHT_TRIGGER,
    [MDKR_CONTROLLER_SOURCE_RIGHT_STICK_UP] =
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_UP,
    [MDKR_CONTROLLER_SOURCE_RIGHT_STICK_DOWN] =
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_DOWN,
    [MDKR_CONTROLLER_SOURCE_RIGHT_STICK_LEFT] =
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_LEFT,
    [MDKR_CONTROLLER_SOURCE_RIGHT_STICK_RIGHT] =
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT,
};

MdkrVideoKey mdkr_controller_binding_key(MdkrControllerSource source) {
    if ((int)source < 0 || source >= MDKR_CONTROLLER_SOURCE_COUNT) {
        return MDKR_VIDEO_KEY_COUNT;
    }
    return s_binding_keys[source];
}

static uint16_t action_bit(const char *action) {
    if (action == NULL || !strcmp(action, "none"))
        return 0;
    if (!strcmp(action, "a"))
        return MDKR_N64_A;
    if (!strcmp(action, "b"))
        return MDKR_N64_B;
    if (!strcmp(action, "z"))
        return MDKR_N64_Z;
    if (!strcmp(action, "start"))
        return MDKR_N64_START;
    if (!strcmp(action, "dpad_up"))
        return MDKR_N64_DU;
    if (!strcmp(action, "dpad_down"))
        return MDKR_N64_DD;
    if (!strcmp(action, "dpad_left"))
        return MDKR_N64_DL;
    if (!strcmp(action, "dpad_right"))
        return MDKR_N64_DR;
    if (!strcmp(action, "l"))
        return MDKR_N64_L;
    if (!strcmp(action, "r"))
        return MDKR_N64_R;
    if (!strcmp(action, "c_up"))
        return MDKR_N64_CU;
    if (!strcmp(action, "c_down"))
        return MDKR_N64_CD;
    if (!strcmp(action, "c_left"))
        return MDKR_N64_CL;
    if (!strcmp(action, "c_right"))
        return MDKR_N64_CR;
    return 0;
}

uint16_t
mdkr_controller_mapped_buttons(const MdkrVideoConfig *config,
                               const MdkrControllerDigitalState *state) {
    uint16_t buttons = 0;
    if (config == NULL || state == NULL)
        return 0;

    for (int source = 0; source < MDKR_CONTROLLER_SOURCE_COUNT; source++) {
        const MdkrVideoKey key = s_binding_keys[source];
        if (state->active[source]) {
            buttons |= action_bit(config->values[key].text);
        }
    }
    return buttons;
}

int mdkr_controller_rumble_enabled(const MdkrVideoConfig *config) {
    return config != NULL &&
           config->values[MDKR_INPUT_RUMBLE_ENABLED].number != 0.0f;
}

uint16_t mdkr_controller_rumble_strength(const MdkrVideoConfig *config) {
    const char *profile;
    if (config == NULL)
        return UINT16_MAX;
    profile = config->values[MDKR_INPUT_RUMBLE_PROFILE].text;
    if (!strcmp(profile, "light"))
        return (uint16_t)22937u; /* 35% */
    if (!strcmp(profile, "balanced"))
        return (uint16_t)42598u; /* 65% */
    return UINT16_MAX;           /* strong/default */
}

uint16_t mdkr_controller_rumble_output_strength(const MdkrVideoConfig *config,
                                                int requested) {
    return requested && mdkr_controller_rumble_enabled(config)
               ? mdkr_controller_rumble_strength(config)
               : 0u;
}
