/** enh_speedometer.c — see enh_speedometer.h. */
/* <ultra64.h> first: PR/os_libc.h redeclares sprintf, which the host's
 * fortified <stdio.h> has already turned into a macro if it got in ahead. */
#include <ultra64.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "camera.h" /* SCREEN_HEIGHT */
#include "font.h"
/* macros.h routes sqrtf to __builtin_sqrtf, which is the square root
 * hud_speedometre() takes. The readout has to agree with the needle to the
 * last bit, so it must not quietly get libm's instead. */
#include "macros.h"
#include "objects.h"
#include "racer.h"
#include "textures_sprites.h" /* rendermode_reset */

#include "enh_speedometer.h"
#include "video_config.h"

/* Host frame ordinal, for the [SPEEDO] diagnostic only. Declared in
 * platform/platform_os.h; taken locally the way mdkr_adventure.c and
 * hasm_stubs_temp.c already take it, so this stays clear of the SDL/window
 * surface that header also carries. */
extern int g_frameCounter;

/* The authored needle is driven by four times the racer's ground speed and
 * saturates at 100; the dial face it points at is printed 0..150. See the
 * header for why those two constants, and not a hand-picked "feels right"
 * scale, are the ones the readout uses. */
#define SPEEDOMETER_NEEDLE_PER_UNIT 4.0f
#define SPEEDOMETER_NEEDLE_MAX      100.0f
#define SPEEDOMETER_FACE_PER_NEEDLE 1.5f
#define SPEEDOMETER_KM_PER_MILE     1.609344f

/* Bottom-left, clear of every authored element: the weapon panel stops well
 * above this line, the banana and lap counters are along the top, and the
 * minimap and the authored dial are bottom-RIGHT. Nothing the game already
 * draws shares these rows, which is what lets check_enh_speedometer.py assert
 * the whole rest of the frame is untouched. */
#define SPEEDOMETER_X 10
#define SPEEDOMETER_Y (SCREEN_HEIGHT - 16)

s32 mdkr_enh_speedometer_mode(void) {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    s32 mode = (s32) config->values[MDKR_ENH_SPEEDOMETER].number;

    if (mode < MDKR_SPEEDOMETER_OFF || mode > MDKR_SPEEDOMETER_KMH) {
        return MDKR_SPEEDOMETER_OFF;
    }
    return mode;
}

f32 mdkr_enh_speedometer_sample(void) {
    Object *obj = get_racer_object_by_port(PLAYER_ONE);
    const Object_Racer *racer;
    f32 x;
    f32 y;
    f32 z;

    if (obj == NULL || obj->racer == NULL) {
        return 0.0f;
    }
    racer = obj->racer;
    x = obj->x_velocity;
    z = obj->z_velocity;
    /* The plane is the one vehicle whose climb is speed a player would call
     * speed, which is exactly the split hud_speedometre() already makes. */
    if (racer->vehicleID == VEHICLE_PLANE) {
        y = obj->y_velocity;
        return sqrtf((x * x) + (y * y) + (z * z));
    }
    return sqrtf((x * x) + (z * z));
}

s32 mdkr_enh_speedometer_reading(s32 mode, f32 sample) {
    f32 needle;
    f32 face;

    if (mode != MDKR_SPEEDOMETER_MPH && mode != MDKR_SPEEDOMETER_KMH) {
        return 0;
    }
    /* Written as "not greater than" so a NaN velocity reads 0 rather than
     * propagating into the cast, which would be undefined. */
    if (!(sample > 0.0f)) {
        return 0;
    }
    needle = sample * SPEEDOMETER_NEEDLE_PER_UNIT;
    if (needle > SPEEDOMETER_NEEDLE_MAX) {
        needle = SPEEDOMETER_NEEDLE_MAX;
    }
    face = needle * SPEEDOMETER_FACE_PER_NEEDLE;
    if (mode == MDKR_SPEEDOMETER_KMH) {
        face *= SPEEDOMETER_KM_PER_MILE;
    }
    return (s32) (face + 0.5f);
}

const char *mdkr_enh_speedometer_unit(s32 mode) {
    switch (mode) {
    case MDKR_SPEEDOMETER_MPH:
        return "MPH";
    case MDKR_SPEEDOMETER_KMH:
        /* "KPH", not "KM/H": the fun font's solidus carries almost no advance,
         * so the slash lands on top of the M and the unit reads as a smudge.
         * Three characters also keeps both modes the same width, so the
         * readout does not jump when a player switches units. */
        return "KPH";
    default:
        return "";
    }
}

s32 mdkr_enh_speedometer_text(char *out, size_t capacity, s32 mode,
                              s32 reading) {
    const char *unit = mdkr_enh_speedometer_unit(mode);
    int written;

    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (unit[0] == '\0') {
        return 0;
    }
    written = snprintf(out, capacity, "%d %s", (int) reading, unit);
    if (written < 0 || (size_t) written >= capacity) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* One line per drawn readout, for tests that need the number the player is
 * looking at without reading it back out of the pixels. Off unless
 * MDKR_SPEEDO_TRACE=1, so a normal run's log is unchanged. */
static void speedometer_trace(s32 mode, f32 sample, s32 reading) {
    static int armed = -1;

    if (armed < 0) {
        const char *setting = getenv("MDKR_SPEEDO_TRACE");
        armed = (setting != NULL && setting[0] == '1') ? 1 : 0;
    }
    if (!armed) {
        return;
    }
    printf("[SPEEDO] frame=%d mode=%d countdown=%d units=%.6f shown=%d "
           "unit=%s\n",
           g_frameCounter, (int) mode, (int) get_race_countdown(),
           (double) sample, (int) reading, mdkr_enh_speedometer_unit(mode));
    fflush(stdout);
}

void mdkr_enh_speedometer_draw(Gfx **displayList) {
    char text[MDKR_SPEEDOMETER_TEXT_MAX];
    s32 mode = mdkr_enh_speedometer_mode();
    f32 sample;
    s32 reading;

    if (displayList == NULL || mode == MDKR_SPEEDOMETER_OFF) {
        return;
    }
    sample = mdkr_enh_speedometer_sample();
    reading = mdkr_enh_speedometer_reading(mode, sample);
    if (!mdkr_enh_speedometer_text(text, sizeof text, mode, reading)) {
        return;
    }

    /* Same two-pass drop shadow the port's other in-race caption uses, so the
     * readout stays legible over both sky and track. */
    set_text_font(ASSET_FONTS_FUNFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 180);
    draw_text(displayList, SPEEDOMETER_X + 1, SPEEDOMETER_Y + 1, text,
              ALIGN_TOP_LEFT);
    set_text_colour(255, 224, 96, 0, 235);
    draw_text(displayList, SPEEDOMETER_X, SPEEDOMETER_Y, text, ALIGN_TOP_LEFT);

    /* render_text_string() already resets the render mode, but it leaves the
     * primitive colour at the caption's opacity. Put the HUD default back so
     * nothing drawn after this point can inherit it: the whole claim of this
     * enhancement is that the rest of the frame is byte-identical. */
    rendermode_reset(displayList);
    gDPSetPrimColor((*displayList)++, 0, 0, 255, 255, 255, 255);

    speedometer_trace(mode, sample, reading);
}
