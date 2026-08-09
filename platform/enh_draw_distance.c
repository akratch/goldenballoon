/** enh_draw_distance.c — see enh_draw_distance.h. */
#include <stdio.h>
#include <stdlib.h>

#include "enh_draw_distance.h"
#include "video_config.h"

/* Host frame ordinal, for the [DRAWDIST] diagnostic only. Declared in
 * platform/platform_os.h; taken locally the way enh_speedometer.c already takes
 * it, so this stays clear of the SDL/window surface that header also carries. */
extern int g_frameCounter;

/* Latched for the frame being drawn. Exactly 1.0f means "authored", and every
 * caller tests for that rather than for "the setting is 100", so a future
 * source for the scale cannot accidentally leave the authored path behind. */
static float s_scale = 1.0f;
static int s_lodBias = MDKR_LOD_BIAS_AUTHORED;

/* The tally belongs to the frame it was collected on, and it can only be
 * printed once that frame has finished drawing — which is the next call to
 * begin_frame. Remembering the ordinal here rather than reading g_frameCounter
 * at print time is what keeps the [DRAWDIST] row labelled with the frame it
 * actually describes, and a test that captures one frame and reads that row
 * depends on the two being the same frame. */
static int s_tallyFrame;
static int s_authoredDraws;
static int s_extendedDraws;
static int s_lodShifts;

static int drawdist_trace_armed(void) {
    static int armed = -1;

    if (armed < 0) {
        const char *setting = getenv("MDKR_DRAWDIST_TRACE");
        armed = (setting != NULL && setting[0] == '1') ? 1 : 0;
    }
    return armed;
}

static int clamp_percent(int percent) {
    if (percent < MDKR_DRAW_DISTANCE_MIN_PERCENT) {
        return MDKR_DRAW_DISTANCE_MIN_PERCENT;
    }
    if (percent > MDKR_DRAW_DISTANCE_MAX_PERCENT) {
        return MDKR_DRAW_DISTANCE_MAX_PERCENT;
    }
    return percent;
}

void mdkr_enh_draw_distance_begin_frame(void) {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    int percent;
    int bias;

    /*
     * The presentation-depth tool hook rides this call, and only this call,
     * because of WHERE the call is: tracks.c invokes begin_frame() at the top
     * of render_scene(), immediately after camera_obstruction_presentation_
     * begin() opens the presentation scope and before any viewport authors a
     * lens from the record the fixed-tick finalizer just latched. Nothing else
     * in platform/ is called in that window, and a tool that substitutes
     * outside it either has its substitution overwritten by the next tick's
     * latch (anything before the tick) or arrives after the display list has
     * already been walked (anything at swap time).
     *
     * Declared locally rather than by including app_overlay_hooks.h -- that
     * header pulls the app-shell seam header in for AppOverlayHooks, and this
     * file needs one void(void) symbol. game/src/thread3_main.c extern's
     * mdkr_render_census_pre() at its call site for the same reason.
     *
     * Nothing is registered on any CLI invocation, so this is one null compare
     * per drawn frame there.
     */
    {
        extern void platformPresentationHook(void);
        platformPresentationHook();
    }

    if (drawdist_trace_armed() &&
        (s_authoredDraws != 0 || s_extendedDraws != 0)) {
        printf("[DRAWDIST] frame=%d scale=%.2f lodBias=%d authored=%d "
               "extended=%d drawn=%d lodShifted=%d\n",
               s_tallyFrame, (double) s_scale, s_lodBias, s_authoredDraws,
               s_extendedDraws, s_authoredDraws + s_extendedDraws,
               s_lodShifts);
        fflush(stdout);
    }
    s_tallyFrame = g_frameCounter;
    s_authoredDraws = 0;
    s_extendedDraws = 0;
    s_lodShifts = 0;

    percent = clamp_percent((int) config->values[MDKR_ENH_DRAW_DISTANCE].number);
    /* The authored default is the bottom of the schema's range, so the common
     * case produces the literal 1.0f rather than 100/100. */
    s_scale = percent == MDKR_DRAW_DISTANCE_MIN_PERCENT
                  ? 1.0f
                  : (float) percent / (float) MDKR_DRAW_DISTANCE_MIN_PERCENT;

    bias = (int) config->values[MDKR_ENH_LOD_BIAS].number;
    if (bias < MDKR_LOD_BIAS_AUTHORED || bias > MDKR_LOD_BIAS_MAX) {
        bias = MDKR_LOD_BIAS_AUTHORED;
    }
    s_lodBias = bias;
}

float mdkr_enh_draw_distance_scale(void) { return s_scale; }

static int clamp_model(int modelIndex, int firstModel, int lastModel) {
    if (modelIndex < firstModel) {
        return firstModel;
    }
    if (modelIndex > lastModel) {
        return lastModel;
    }
    return modelIndex;
}

int mdkr_enh_lod_bias_apply(int modelIndex, int firstModel, int lastModel) {
    int authored;
    int biased;

    /* Return early rather than subtracting zero. At the authored setting this
     * function must not be able to hand back a different index than the caller
     * already had, and "subtracting 0 is the identity" is a claim about
     * arithmetic; "the caller's own value came straight back" is not a claim at
     * all. */
    if (s_lodBias == MDKR_LOD_BIAS_AUTHORED) {
        return modelIndex;
    }
    authored = clamp_model(modelIndex, firstModel, lastModel);
    biased = clamp_model(modelIndex - s_lodBias, firstModel, lastModel);
    if (biased != authored && drawdist_trace_armed()) {
        s_lodShifts++;
    }
    return biased;
}

void mdkr_enh_draw_distance_count(int authored, int extended) {
    if (!drawdist_trace_armed()) {
        return;
    }
    s_authoredDraws += authored;
    s_extendedDraws += extended;
}
