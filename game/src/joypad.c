#include "joypad.h"
#include "game.h"
#include "menu.h"
#include "save_layout.h"
#include "thread3_main.h"
#ifdef NATIVE_PORT
#include <string.h>

#include "input_consumption_trace.h"
#endif

/* PORTABILITY (Windows/MinGW): OSContStatus has a field named `errno`, and the
 * Windows CRT defines `errno` as an object-like macro (see PR/os_cont.h), so
 * `gControllerStatus[0].errno` below would expand mid-expression. This TU never
 * touches the real C errno, so drop the macro for the whole file — after every
 * system header has been pulled in, since MinGW's own <stdlib.h> chain assigns
 * to errno. */
#ifdef _WIN32
#undef errno
#endif

s32 sNoControllerPluggedIn =
    FALSE; // Looks to be a boolean for whether a controller is plugged in. FALSE if plugged in, and TRUE if not.
u16 gButtonMask = 0xFFFF; // Used when anti-cheat/anti-tamper has failed in level_global_init()

OSMesgQueue sSIMesgQueue;
OSMesg sSIMesgBuf;
OSMesg gSIMesg;
OSContStatus gControllerStatus[MAXCONTROLLERS];
OSContPad gControllerCurrData[MAXCONTROLLERS];
OSContPad gControllerPrevData[MAXCONTROLLERS];
u16 gControllerButtonsPressed[MAXCONTROLLERS];
u16 gControllerButtonsReleased[MAXCONTROLLERS];
u8 sPlayerID[16];

/**
 * Return the serial interface message queue.
 * Official name: joyMessageQ
 */
OSMesgQueue *si_mesg(void) {
    return &sSIMesgQueue;
}

/**
 * Initialise the player controllers, and return the status when finished.
 * Official name: joyInit
 */
s32 input_init(void) {
    UNUSED s32 *temp1;
    u8 bitpattern;
    UNUSED s32 *temp2;

    osCreateMesgQueue(&sSIMesgQueue, &sSIMesgBuf, 1);
    osSetEventMesg(OS_EVENT_SI, &sSIMesgQueue, gSIMesg);
    osContInit(&sSIMesgQueue, &bitpattern, gControllerStatus);
    osContStartReadData(&sSIMesgQueue);
    input_assign_players();

    sNoControllerPluggedIn = FALSE;

    if ((bitpattern & CONT_ABSOLUTE) && (!(gControllerStatus[0].errno & CONT_NO_RESPONSE_ERROR))) {
        return CONTROLLER_EXISTS;
    }

    if (!bitpattern) {} // Fakematch

    sNoControllerPluggedIn = TRUE;

    return CONTROLLER_MISSING;
}

/**
 * Reads arg0 for a set of flags on whether to read, write, or erase any save data.
 * Also reads the latest inputs from the controllers, and sets their values.
 * Official name: joyRead
 */
s32 input_update(s32 saveDataFlags, s32 updateRate) {
    Settings **allSaves;
    OSMesg unusedMsg;
    Settings *settings;
    s32 i;

    if (osRecvMesg(&sSIMesgQueue, &unusedMsg, OS_MESG_NOBLOCK) == 0) {
        // Back up old controller data
        for (i = 0; i < MAXCONTROLLERS; i++) {
            gControllerPrevData[i] = gControllerCurrData[i];
        }
        osContGetReadData(gControllerCurrData);
        if (saveDataFlags != 0) {
            settings = get_settings();
            if (SAVE_DATA_FLAG_READ_EEPROM_INDEX(saveDataFlags)) {
                read_eeprom_data(settings, SAVE_DATA_FLAG_READ_EEPROM_INDEX(saveDataFlags));
            }
            if (saveDataFlags & SAVE_DATA_FLAG_READ_ALL_SAVE_DATA) {
                allSaves = get_all_save_files_ptr();
                for (i = 0; i < NUMBER_OF_SAVE_FILES; i++) {
                    read_save_file(i, allSaves[i]);
                }
            }
            if (saveDataFlags & SAVE_DATA_FLAG_READ_SAVE_DATA) {
                read_save_file(SAVE_DATA_FLAG_READ_SAVE_FILE_NUMBER(saveDataFlags), settings);
            }
            if (SAVE_DATA_FLAG_WRITE_EEPROM_INDEX(saveDataFlags)) {
                write_eeprom_data(settings, SAVE_DATA_FLAG_WRITE_EEPROM_INDEX(saveDataFlags));
            }
            if (saveDataFlags & SAVE_DATA_FLAG_WRITE_SAVE_DATA) {
                Settings *writeSource = take_write_save_file_source();
                if (writeSource != NULL) {
                    write_save_data(
                        SAVE_DATA_FLAG_WRITE_SAVE_FILE_NUMBER(saveDataFlags),
                        writeSource);
                } else {
                    stubbed_printf(
                        "WARNING : Save write requested without a source\n");
                }
            }
            if (saveDataFlags & SAVE_DATA_FLAG_ERASE_SAVE_DATA) {
                erase_save_file(SAVE_DATA_FLAG_WRITE_SAVE_FILE_NUMBER(saveDataFlags), settings);
            }
            //!@bug: These next two if statements check the same bits
            // as the ones used to set the save file number to read from.
            if (saveDataFlags & SAVE_DATA_FLAG_READ_EEPROM_SETTINGS) {
                read_eeprom_settings(get_eeprom_settings_pointer());
            }
            if (saveDataFlags & SAVE_DATA_FLAG_WRITE_EEPROM_SETTINGS) {
                write_eeprom_settings(get_eeprom_settings_pointer());
            }
            // Reset all flags
            saveDataFlags = 0;
        }
        rumble_update(updateRate);
        osContStartReadData(&sSIMesgQueue);
    }
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (sNoControllerPluggedIn) {
            gControllerCurrData[i].button = 0;
        }
        // XOR the diff between the last read of the controller data with the current read to see what buttons have been
        // pushed and released.
        gControllerButtonsPressed[i] =
            ((gControllerCurrData[i].button ^ gControllerPrevData[i].button) & gControllerCurrData[i].button) &
            gButtonMask;
        gControllerButtonsReleased[i] =
            ((gControllerCurrData[i].button ^ gControllerPrevData[i].button) & gControllerPrevData[i].button) &
            gButtonMask;
#ifdef NATIVE_PORT
        INPUT_CONSUMPTION_TRACE(
            (unsigned)i, gControllerCurrData[i].button,
            gControllerButtonsPressed[i], gControllerButtonsReleased[i],
            gControllerCurrData[i].stick_x, gControllerCurrData[i].stick_y,
            (gControllerCurrData[i].errno & CONT_NO_RESPONSE_ERROR) == 0);
#endif
    }
    return saveDataFlags;
}

/**
 * Set the first 4 player ID's to the controller numbers, so players can input in the menus after boot.
 * Official name: joyResetMap
 */
void input_assign_players(void) {
    s32 i;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        sPlayerID[i] = i;
    }
}

/**
 * Assign the first four player ID's to the index of the connected players.
 * Assign the next four player ID's to the index of the players who are not connected.
 * Official name: joyCreateMap
 */
void charselect_assign_players(s8 *activePlayers) {
    s32 i;
    s32 temp = 0;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (activePlayers[i]) {
            sPlayerID[temp++] = i;
        }
    }
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (!activePlayers[i]) {
            sPlayerID[temp++] = i;
        }
    }
}

/**
 * Returns the id of the selected index.
 * Official name: joyGetController
 */
u8 input_player_id(s32 player) {
    return sPlayerID[player];
}

/**
 * Swaps the ID's of the first two indexes.
 * This applies in 2 player adventure, so that player 2 can control the car in the overworld.
 */
void input_swap_id(void) {
    u8 tempID = sPlayerID[0];
    sPlayerID[0] = sPlayerID[1];
    sPlayerID[1] = tempID;
}

/**
 * Returns the buttons that are currently pressed down on the controller.
 * Official name: joyGetButtons
 */
u16 input_held(s32 player) {
    return gControllerCurrData[sPlayerID[player]].button;
}

/**
 * Returns the buttons that are newly pressed during that frame.
 * NOTE: This was a u16, but we only got a match in menu_ghost_data_loop when it was a u32 for some reason
 * Official name: joyGetPressed
 */
u32 input_pressed(s32 player) {
    return gControllerButtonsPressed[sPlayerID[player]];
}

/**
 * Returns the buttons that are no longer pressed in that frame.
 * Official name: joyGetReleased
 */
u16 input_released(s32 player) {
    return gControllerButtonsReleased[sPlayerID[player]];
}

/**
 * Clamps the X joystick axis of the selected player to 70 and returns it.
 * Official name: joyGetStickX
 */
s32 input_clamp_stick_x(s32 player) {
    return input_clamp_stick_mag(gControllerCurrData[sPlayerID[player]].stick_x);
}

/**
 * Clamps the Y joystick axis of the selected player to 70 and returns it.
 * Official name: joyGetStickY
 */
s32 input_clamp_stick_y(s32 player) {
    return input_clamp_stick_mag(gControllerCurrData[sPlayerID[player]].stick_y);
}

/**
 * Keeps the joysticks axis reads no higher than 70 (of a possible 127 or -128)
 * Will also pull the reading towards the centre.
 */
s8 input_clamp_stick_mag(s8 stickMag) {
    if (stickMag < JOYSTICK_DEADZONE && stickMag > -JOYSTICK_DEADZONE) {
        return 0;
    }
    if (stickMag > 0) {
        stickMag -= JOYSTICK_DEADZONE;
        if (stickMag > JOYSTICK_MAX_RANGE) {
            stickMag = JOYSTICK_MAX_RANGE;
        }
    } else {
        stickMag += JOYSTICK_DEADZONE;
        if (stickMag < -JOYSTICK_MAX_RANGE) {
            stickMag = -JOYSTICK_MAX_RANGE;
        }
    }
    return stickMag;
}

/**
 * Used when anti-cheat/anti-tamper has failed in level_global_init()
 * Official Name: joySetSecurity
 */
void drm_disable_input(void) {
    gButtonMask = 0;
}

#ifdef NATIVE_PORT
void input_rollback_capture(MdkrInputSample out[MDKR_INPUT_PORTS]) {
    unsigned port;
    if (out == NULL) return;
    for (port = 0u; port < MDKR_INPUT_PORTS; port++) {
        out[port] = (MdkrInputSample){
            gControllerCurrData[port].button,
            gControllerCurrData[port].stick_x,
            gControllerCurrData[port].stick_y,
            (gControllerCurrData[port].errno & CONT_NO_RESPONSE_ERROR) == 0};
    }
}

void input_rollback_apply(const MdkrInputSample input[MDKR_INPUT_PORTS]) {
    unsigned port;
    if (input == NULL) return;
    for (port = 0u; port < MDKR_INPUT_PORTS; port++) {
        gControllerPrevData[port] = gControllerCurrData[port];
        gControllerCurrData[port].button =
            input[port].present ? input[port].buttons : 0u;
        gControllerCurrData[port].stick_x =
            input[port].present ? input[port].stick_x : 0;
        gControllerCurrData[port].stick_y =
            input[port].present ? input[port].stick_y : 0;
        gControllerCurrData[port].errno =
            input[port].present ? 0 : CONT_NO_RESPONSE_ERROR;
        gControllerButtonsPressed[port] =
            ((gControllerCurrData[port].button ^
              gControllerPrevData[port].button) &
             gControllerCurrData[port].button) & gButtonMask;
        gControllerButtonsReleased[port] =
            ((gControllerCurrData[port].button ^
              gControllerPrevData[port].button) &
             gControllerPrevData[port].button) & gButtonMask;
    }
}

static void input_rollback_write_pad(
    OSContPad *pad, const MdkrInputSample *sample) {
    memset(pad, 0, sizeof(*pad));
    pad->button = sample->present ? sample->buttons : 0u;
    pad->stick_x = sample->present ? sample->stick_x : 0;
    pad->stick_y = sample->present ? sample->stick_y : 0;
    pad->errno = sample->present ? 0 : CONT_NO_RESPONSE_ERROR;
}

void input_rollback_apply_from_previous(
    const MdkrInputSample input[MDKR_INPUT_PORTS],
    const MdkrInputSample previous[MDKR_INPUT_PORTS]) {
    unsigned port;
    if (input == NULL || previous == NULL) return;
    for (port = 0u; port < MDKR_INPUT_PORTS; port++) {
        input_rollback_write_pad(&gControllerPrevData[port], &previous[port]);
        input_rollback_write_pad(&gControllerCurrData[port], &input[port]);
        input_rollback_compute_edges(
            &input[port], &previous[port], gButtonMask,
            &gControllerButtonsPressed[port],
            &gControllerButtonsReleased[port]);
    }
}

s32 input_rollback_state_spans(
    MdkrInputRollbackSpan spans[MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT]) {
    size_t count = 0u;
#define ADD_INPUT_STATE(value)                                      \
    do {                                                            \
        spans[count++] = (MdkrInputRollbackSpan){                   \
            &(value), sizeof(value)};                               \
    } while (0)
    if (spans == NULL) return FALSE;
    ADD_INPUT_STATE(sNoControllerPluggedIn);
    ADD_INPUT_STATE(gButtonMask);
    ADD_INPUT_STATE(gControllerStatus);
    ADD_INPUT_STATE(gControllerCurrData);
    ADD_INPUT_STATE(gControllerPrevData);
    ADD_INPUT_STATE(gControllerButtonsPressed);
    ADD_INPUT_STATE(gControllerButtonsReleased);
    ADD_INPUT_STATE(sPlayerID);
#undef ADD_INPUT_STATE
    return count == MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT;
}
#endif
