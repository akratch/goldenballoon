/**
 * controller_mapping.h — pure normalized-controller -> N64 button policy.
 *
 * SDL owns device-specific joystick mappings. This layer begins after SDL has
 * normalized a pad into named controls and applies the player's durable
 * Input.Controller* choices. It deliberately has no SDL dependency, so exact
 * default/remapped behavior and rumble profiles can be unit-tested without a
 * window or physical controller.
 */
#ifndef MDKR64_CONTROLLER_MAPPING_H
#define MDKR64_CONTROLLER_MAPPING_H

#include "video_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Must match game/include/PR/os_cont.h CONT_* values. */
#define MDKR_N64_A     0x8000u
#define MDKR_N64_B     0x4000u
#define MDKR_N64_Z     0x2000u
#define MDKR_N64_START 0x1000u
#define MDKR_N64_DU    0x0800u
#define MDKR_N64_DD    0x0400u
#define MDKR_N64_DL    0x0200u
#define MDKR_N64_DR    0x0100u
#define MDKR_N64_L     0x0020u
#define MDKR_N64_R     0x0010u
#define MDKR_N64_CU    0x0008u
#define MDKR_N64_CD    0x0004u
#define MDKR_N64_CL    0x0002u
#define MDKR_N64_CR    0x0001u

typedef enum MdkrControllerSource {
    MDKR_CONTROLLER_SOURCE_A = 0,
    MDKR_CONTROLLER_SOURCE_B,
    MDKR_CONTROLLER_SOURCE_X,
    MDKR_CONTROLLER_SOURCE_Y,
    MDKR_CONTROLLER_SOURCE_START,
    MDKR_CONTROLLER_SOURCE_LEFT_STICK,
    MDKR_CONTROLLER_SOURCE_RIGHT_STICK,
    MDKR_CONTROLLER_SOURCE_LEFT_SHOULDER,
    MDKR_CONTROLLER_SOURCE_RIGHT_SHOULDER,
    MDKR_CONTROLLER_SOURCE_DPAD_UP,
    MDKR_CONTROLLER_SOURCE_DPAD_DOWN,
    MDKR_CONTROLLER_SOURCE_DPAD_LEFT,
    MDKR_CONTROLLER_SOURCE_DPAD_RIGHT,
    MDKR_CONTROLLER_SOURCE_LEFT_TRIGGER,
    MDKR_CONTROLLER_SOURCE_RIGHT_TRIGGER,
    MDKR_CONTROLLER_SOURCE_RIGHT_STICK_UP,
    MDKR_CONTROLLER_SOURCE_RIGHT_STICK_DOWN,
    MDKR_CONTROLLER_SOURCE_RIGHT_STICK_LEFT,
    MDKR_CONTROLLER_SOURCE_RIGHT_STICK_RIGHT,
    MDKR_CONTROLLER_SOURCE_COUNT
} MdkrControllerSource;

typedef struct MdkrControllerDigitalState {
    uint8_t active[MDKR_CONTROLLER_SOURCE_COUNT];
} MdkrControllerDigitalState;

/* Config key that owns one normalized source, or MDKR_VIDEO_KEY_COUNT. */
MdkrVideoKey mdkr_controller_binding_key(MdkrControllerSource source);

/* OR every active source's configured N64 destination into one pad bitfield. */
uint16_t     mdkr_controller_mapped_buttons(
    const MdkrVideoConfig            *config,
    const MdkrControllerDigitalState *state);

/* Resolved haptic preferences. Strength is the SDL 0..UINT16_MAX amplitude. */
int      mdkr_controller_rumble_enabled(const MdkrVideoConfig *config);
uint16_t mdkr_controller_rumble_strength(const MdkrVideoConfig *config);
/* Final amplitude for one game motor request. A muted request resolves to zero
 * without changing whether the connected device is reported as capable. */
uint16_t mdkr_controller_rumble_output_strength(
    const MdkrVideoConfig *config,
    int                    requested);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CONTROLLER_MAPPING_H */
