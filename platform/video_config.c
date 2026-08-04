/**
 * video_config.c — see video_config.h.
 *
 * Structured like display_config.c: a pure calculation core with no globals or
 * environment reads, wrapped by a thin runtime layer. The split is what lets
 * tests/test_video_config.c assert every precedence rule without a window.
 */
#include "video_config.h"
#include "pacing_policy.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const MdkrVideoSchema s_schema[MDKR_VIDEO_KEY_COUNT] = {
    [MDKR_VIDEO_REMASTER_FX] = {
        "Video.RemasterFX", "MDKR_REMASTER_FX",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 1.0f,
        "Remaster effects",
        "Master switch for look-changing effects. Requires a restart so cached "
        "font, lighting, and post-effect resources change as one coherent mode.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    [MDKR_VIDEO_WIDESCREEN] = {
        "Video.Widescreen", "MDKR_WIDESCREEN",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Widescreen",
        "1 engages the display policy. 0 is the pre-widescreen STRETCH compatibility "
        "path -- it is NOT how Pure gets 4:3; see Video.Aspect.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    [MDKR_VIDEO_ASPECT] = {
        "Video.Aspect", "MDKR_ASPECT",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Aspect ratio",
        "auto follows the window. 4:3 pillarboxes the authored framing (Pure).",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    [MDKR_VIDEO_RENDER_SCALE] = {
        "Video.RenderScale", "MDKR_RENDER_SCALE",
        MDKR_VIDEO_TYPE_FLOAT, MDKR_VIDEO_SCOPE_LIVE, 1.0f, 4.0f,
        "Render scale",
        "Internal supersampling factor. Fidelity only; never changes the look.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_MSAA] = {
        "Video.MSAA", "MDKR_MSAA",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 8.0f,
        "MSAA",
        "Multisample anti-aliasing. Redundant with render scale; off by default.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_ANISOTROPY] = {
        "Video.AnisotropicFiltering", "MDKR_ANISOTROPY",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_RESTART, 1.0f, 16.0f,
        "Anisotropic filtering",
        "1 keeps the N64 3-point filter. Above 1 bypasses it for grazing surfaces. "
        "Requires a restart because shader and sampler caches own this choice.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_MIPMAPS] = {
        "Video.Mipmaps", "MDKR_MIPMAPS",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 1.0f,
        "Mipmaps",
        "Generate mip chains for 3D surfaces. Removes track-surface shimmer. "
        "Requires a restart so already-uploaded textures cannot retain stale chains.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_TEXTURE_PACK] = {
        "Video.TexturePack", "MDKR_TEXTURE_PACK",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Texture pack",
        "Reserved for a future user-supplied HD texture pack. This setting has "
        "no runtime effect yet; stock textures are always used.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_GAMEPLAY_FOV] = {
        "Video.GameplayFOV", "MDKR_FOV",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Gameplay FOV",
        "authored preserves every track's original lens. A number from 20 to 140 "
        "scales gameplay cameras relative to the authored 60-degree reference.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    [MDKR_VIDEO_SIMULATION_CADENCE] = {
        "Gameplay.SimulationCadence", "MDKR_SIMULATION_CADENCE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Simulation cadence",
        "original preserves DKR's authored two-VI-field physics. enhanced keeps "
        "the historical port's one-field simulation and changes gameplay.",
        MDKR_VIDEO_CAT_PACING
    },
    /*
     * FrameLimit/MotionSmoothing (spec §11) are presentation-only settings
     * mapped onto the MDKR_PRESENT_RATE / MDKR_PRESENT_SMOOTHING seams Phase 3
     * Wave B slice 2 already reads (present_sched.c/platform_sdl_min.c).
     * The env name below is deliberately the SAME seam name so a raw
     * diagnostic override and this schema key resolve from the same
     * underlying environment variable. The shared pacing-policy parser accepts
     * `original`, integer caps 30..1000, `display`, and `uncapped`; the platform
     * converts elapsed time into exact sub-field scheduler/audio units, so none
     * of those values is rounded onto the source 50/60 Hz VI grid.
     *
     * SCOPE_RESTART, not SCOPE_LIVE, and the asymmetry with Video.Widescreen /
     * Video.Aspect / Video.RenderScale is deliberate. Both consumers LATCH:
     *
     *   - platform_sdl_min.c's present_pace_lazy_init() resolves the present
     *     policy ONCE, on the first platform_present_subloop_fields() call,
     *     into file-static deadline state. Nothing ever re-resolves it.
     *   - gfx_pc_dkr.c's gfx_start_frame()/gfx_end_frame() capture the replay's
     *     walk-entry state (dkr_walk_entry_*) and freeze the shadow matrix
     *     registry only when present_sched_replay_armed() is true, and
     *     present_sched.c arms the presentation snapshot store behind a
     *     one-shot (s_snapshot_forced).
     *
     * So a "live" change is unsafe in BOTH directions. Engaging late is dead
     * cost: the freeze/snapshot work starts happening but the subloop never
     * runs, because the platform policy was already latched inactive. Disengaging late
     * is worse than dead: the subloop is still latched ON, while
     * present_sched_replay_armed() has gone false, so gfx_start_frame stops
     * refreshing dkr_walk_entry_* -- which gfx_dkr_replay_invalidate() is the
     * only thing that ever clears -- and gfx_dkr_replay_walk() goes on
     * memcpy'ing a STALE segment table (gfx_pc_dkr.c's replay branch) into the
     * live HLE state. Stale segment bases are wild pointers.
     *
     * Making the seams genuinely re-resolvable is Phase 3's live/interactive
     * slice (design doc §6 slice 3), which is the first slice that has a reason
     * to want it. Until then RESTART is the honest scope: the value still
     * resolves normally at boot from file/CLI/env, and mdkr_video_config_publish
     * still pushes it into present_sched there (video_config_runtime.c) -- it
     * just cannot be flipped underneath a running engine.
     */
    [MDKR_VIDEO_FRAME_LIMIT] = {
        "Video.FrameLimit", "MDKR_PRESENT_RATE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Frame limit",
        "original presents each authored image once. display follows the "
        "monitor, a number sets a native cap, and uncapped removes the native "
        "software cap (the browser maps it to display). Pair a rate above "
        "original with Motion smoothing = Interpolated for unique in-between "
        "images. Gameplay remains on its fixed original cadence. Requires a "
        "restart because the host pacer resolves this value once at startup.",
        MDKR_VIDEO_CAT_PACING
    },
    /*
     * RESTART for the same reason as Video.FrameLimit above, and it is not
     * merely inherited: present_sched_replay_armed() short-circuits to false
     * when smoothing is off, so a boot with MotionSmoothing=off never arms the
     * snapshot store, never captures dkr_walk_entry_* and never freezes the
     * registry. Flipping it to interpolate afterwards would drive the subloop
     * into a replay with no frozen registry and no snapshot pair -- every
     * intermediate frame silently identical to the tick's own. The reverse flip
     * lands in the same stale-walk-entry hazard FrameLimit's note describes.
     */
    [MDKR_VIDEO_MOTION_SMOOTHING] = {
        "Video.MotionSmoothing", "MDKR_PRESENT_SMOOTHING",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Motion smoothing",
        "interpolate draws presentation-only in-between images from adjacent "
        "authored tasks. It does not advance physics, AI, timers, audio, or input "
        "more often. off presents only the game's authored images. Requires a "
        "restart because retained replay resources are armed at startup.",
        MDKR_VIDEO_CAT_PACING
    },
    [MDKR_VIDEO_MODE] = {
        "Video.Mode", "MDKR_VIDEO_MODE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Presentation",
        "Pure is the 4:3 reference. Restored is the default original art direction "
        "at modern fidelity. Remastered is an opt-in, work-in-progress presentation "
        "with SDF text, restrained lighting, world shadows, and a bounded finish.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    /*
     * The only player-facing control over the Remastered world-shadow pass.
     *
     * Before this key the feed had no setting at all: shadows arrived bundled
     * inside Video.RemasterFX, so "the remaster shadows degrade the UX" — the
     * one complaint the pre-1.0 playthrough left open — could only be answered
     * by giving up the RemasterFX group: tonemapping, grading, RL-5 lighting and
     * SDF text. The preset's anisotropy and mip chains are orthogonal settings
     * and remain available without RemasterFX. The renderer had already been
     * written as though the
     * setting existed (gfx_opengl.c's off->on latch reset still names
     * "Video.SunShadow"); this is that setting, under its real name.
     *
     * SCOPE_LIVE, and unlike the pacing keys above that is not aspirational.
     * Both receivers re-read g_pcSunShadow per draw and per frame; the shadow
     * depth resources are (re)acquired every frame from the cascade plan's own
     * budget, which ALREADY changes live whenever the split-screen player count
     * does (2048/2 cascades at 1P, 1024/1 at 4P); and both backends already
     * edge-detect an off->on transition to clear the perma-fail latch. Nothing
     * about this value is latched at boot.
     *
     * Values, and why there are three rather than a checkbox:
     *   full  the shipped image, unchanged (38% attenuation under the umbra).
     *   soft  the same maps and the same cascades at 22% attenuation, for
     *         players who read the full-strength umbra as heavy on DKR's flat
     *         arcade art. DKR's vertex colour already bakes occlusion in, so
     *         the shadow pass is always double-darkening to some degree; this
     *         is the knob for how much.
     *   off   no world shadows. Actor blob decals come back, so karts and
     *         objects keep their grounding rather than floating.
     * "1"/"on" and "0" are accepted for the diagnostic seam's historical
     * spellings so every existing A/B gate keeps working unchanged.
     */
    [MDKR_VIDEO_WORLD_SHADOWS] = {
        "Video.WorldShadows", "MDKR_WORLD_SHADOW",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "World shadows",
        "full is the shipped art-directed depth pass. soft keeps the same "
        "shadows at a lighter strength, for art that already bakes its own "
        "occlusion. off restores the original blob shadows under karts and "
        "objects. Part of Remaster effects; inert in Pure and Restored.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_AUDIO_MASTER_VOLUME] = {
        "Audio.MasterVolume", "MDKR_MASTER_VOLUME",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 100.0f,
        "Master volume",
        "Controls everything you hear. The maximum preserves the authored mix "
        "at unity gain; lower values use a smooth perceptual curve.",
        MDKR_VIDEO_CAT_AUDIO
    },
    [MDKR_AUDIO_MUSIC_VOLUME] = {
        "Audio.MusicVolume", "MDKR_MUSIC_VOLUME",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 100.0f,
        "Music",
        "Adjusts DKR's music bus without changing sound effects.",
        MDKR_VIDEO_CAT_AUDIO
    },
    [MDKR_AUDIO_EFFECTS_VOLUME] = {
        "Audio.EffectsVolume", "MDKR_EFFECTS_VOLUME",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 100.0f,
        "Sound effects",
        "Adjusts vehicles, voices, jingles, and effects without changing music.",
        MDKR_VIDEO_CAT_AUDIO
    },
    [MDKR_WINDOW_MODE] = {
        "Window.Mode", "MDKR_WINDOW_MODE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Window mode",
        "Windowed keeps normal desktop decorations. Fullscreen uses the current "
        "desktop resolution without borders or a display-mode switch. F11 or "
        "Alt+Enter toggles the same setting.",
        MDKR_VIDEO_CAT_INTERFACE
    },
    [MDKR_INPUT_RUMBLE_ENABLED] = {
        "Input.RumbleEnabled", "MDKR_RUMBLE_ENABLED",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Rumble",
        "Mutes host controller vibration without making the connected Rumble "
        "Pak disappear from the game.",
        MDKR_VIDEO_CAT_INPUT
    },
    [MDKR_INPUT_RUMBLE_PROFILE] = {
        "Input.RumbleProfile", "MDKR_RUMBLE_PROFILE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Rumble profile",
        "Light and Balanced reduce both controller motors. Strong preserves "
        "the previous full-amplitude host behavior; DKR still controls each "
        "effect's authored pulse timing.",
        MDKR_VIDEO_CAT_INPUT
    },
#define CONTROLLER_BINDING_SCHEMA(KEY, NAME, ENV, LABEL) \
    [KEY] = { \
        NAME, ENV, \
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f, \
        LABEL, \
        "Choose which N64 button this normalized controller input activates. " \
        "None leaves it unbound; the left analog stick remains steering.", \
        MDKR_VIDEO_CAT_INPUT \
    }
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_A, "Input.ControllerA", "MDKR_CONTROLLER_A",
        "A button"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_B, "Input.ControllerB", "MDKR_CONTROLLER_B",
        "B button"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_X, "Input.ControllerX", "MDKR_CONTROLLER_X",
        "X button"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_Y, "Input.ControllerY", "MDKR_CONTROLLER_Y",
        "Y button"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_START, "Input.ControllerStart",
        "MDKR_CONTROLLER_START", "Start button"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_LEFT_STICK, "Input.ControllerLeftStick",
        "MDKR_CONTROLLER_LEFT_STICK", "Left stick click"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_STICK, "Input.ControllerRightStick",
        "MDKR_CONTROLLER_RIGHT_STICK", "Right stick click"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_LEFT_SHOULDER, "Input.ControllerLeftShoulder",
        "MDKR_CONTROLLER_LEFT_SHOULDER", "Left shoulder"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_SHOULDER, "Input.ControllerRightShoulder",
        "MDKR_CONTROLLER_RIGHT_SHOULDER", "Right shoulder"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_DPAD_UP, "Input.ControllerDpadUp",
        "MDKR_CONTROLLER_DPAD_UP", "D-pad up"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_DPAD_DOWN, "Input.ControllerDpadDown",
        "MDKR_CONTROLLER_DPAD_DOWN", "D-pad down"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_DPAD_LEFT, "Input.ControllerDpadLeft",
        "MDKR_CONTROLLER_DPAD_LEFT", "D-pad left"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_DPAD_RIGHT, "Input.ControllerDpadRight",
        "MDKR_CONTROLLER_DPAD_RIGHT", "D-pad right"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_LEFT_TRIGGER, "Input.ControllerLeftTrigger",
        "MDKR_CONTROLLER_LEFT_TRIGGER", "Left trigger"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_TRIGGER, "Input.ControllerRightTrigger",
        "MDKR_CONTROLLER_RIGHT_TRIGGER", "Right trigger"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_UP, "Input.ControllerRightStickUp",
        "MDKR_CONTROLLER_RIGHT_STICK_UP", "Right stick up"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_DOWN,
        "Input.ControllerRightStickDown", "MDKR_CONTROLLER_RIGHT_STICK_DOWN",
        "Right stick down"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_LEFT,
        "Input.ControllerRightStickLeft", "MDKR_CONTROLLER_RIGHT_STICK_LEFT",
        "Right stick left"),
    CONTROLLER_BINDING_SCHEMA(
        MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT,
        "Input.ControllerRightStickRight", "MDKR_CONTROLLER_RIGHT_STICK_RIGHT",
        "Right stick right"),
#undef CONTROLLER_BINDING_SCHEMA
};

const char *mdkr_video_category_name(MdkrVideoCategory category) {
    switch (category) {
        case MDKR_VIDEO_CAT_PRESENTATION: return "Presentation";
        case MDKR_VIDEO_CAT_FIDELITY:     return "Fidelity";
        case MDKR_VIDEO_CAT_PACING:       return "Pacing";
        case MDKR_VIDEO_CAT_AUDIO:        return "Audio";
        case MDKR_VIDEO_CAT_INTERFACE:    return "Interface";
        case MDKR_VIDEO_CAT_INPUT:        return "Controller";
        default:                          return NULL;
    }
}

const MdkrVideoSchema *mdkr_video_schema(MdkrVideoKey key) {
    if ((int) key < 0 || (int) key >= MDKR_VIDEO_KEY_COUNT) {
        return NULL;
    }
    return &s_schema[key];
}

int mdkr_video_key_is_audio(MdkrVideoKey key) {
    return key == MDKR_AUDIO_MASTER_VOLUME ||
           key == MDKR_AUDIO_MUSIC_VOLUME ||
           key == MDKR_AUDIO_EFFECTS_VOLUME;
}

int mdkr_video_key_is_input(MdkrVideoKey key) {
    return key >= MDKR_INPUT_FIRST_KEY && key <= MDKR_INPUT_LAST_KEY;
}

int mdkr_video_key_is_player_comfort(MdkrVideoKey key) {
    return mdkr_video_key_is_audio(key) || mdkr_video_key_is_input(key) ||
           key == MDKR_WINDOW_MODE;
}

static int mdkr_video_ci_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *mdkr_window_mode_canonical(const char *value) {
    if (mdkr_video_ci_equal(value, "windowed") ||
        mdkr_video_ci_equal(value, "window")) {
        return "windowed";
    }
    if (mdkr_video_ci_equal(value, "fullscreen") ||
        mdkr_video_ci_equal(value, "borderless")) {
        return "fullscreen";
    }
    return NULL;
}

const char *mdkr_rumble_profile_canonical(const char *value) {
    if (mdkr_video_ci_equal(value, "light")) return "light";
    if (mdkr_video_ci_equal(value, "balanced") ||
        mdkr_video_ci_equal(value, "medium")) {
        return "balanced";
    }
    if (mdkr_video_ci_equal(value, "strong") ||
        mdkr_video_ci_equal(value, "full")) {
        return "strong";
    }
    return NULL;
}

const char *mdkr_controller_action_canonical(const char *value) {
    static const char *const actions[] = {
        "none", "a", "b", "z", "start", "l", "r",
        "dpad_up", "dpad_down", "dpad_left", "dpad_right",
        "c_up", "c_down", "c_left", "c_right",
    };
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        if (mdkr_video_ci_equal(value, actions[i])) return actions[i];
    }
    return NULL;
}

MdkrVideoKey mdkr_video_key_from_name(const char *name) {
    if (name == NULL) {
        return MDKR_VIDEO_KEY_COUNT;
    }
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        if (mdkr_video_ci_equal(name, s_schema[i].name)) {
            return (MdkrVideoKey) i;
        }
    }
    return MDKR_VIDEO_KEY_COUNT;
}

/*
 * Preset tables. Rows are keys, columns are modes — see the design spec §2.2.
 * Pure reproduces the authored presentation. Restored adds fidelity without
 * changing the art direction. Remastered is the opt-in, work-in-progress home
 * for deliberately look-changing effects on top of Restored.
 */
static const float s_preset[MDKR_VIDEO_KEY_COUNT][3] = {
    /*                                 pure  restored  remastered */
    [MDKR_VIDEO_REMASTER_FX]  = {      0.0f,     0.0f,       1.0f },
    [MDKR_VIDEO_WIDESCREEN]   = {      1.0f,     1.0f,       1.0f },
    [MDKR_VIDEO_ASPECT]       = {      0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_RENDER_SCALE] = {      1.0f,     2.0f,       2.0f },
    [MDKR_VIDEO_MSAA]         = {      0.0f,     0.0f,       0.0f },
    [MDKR_VIDEO_ANISOTROPY]   = {      1.0f,     8.0f,      16.0f },
    [MDKR_VIDEO_MIPMAPS]      = {      0.0f,     1.0f,       1.0f },
    [MDKR_VIDEO_TEXTURE_PACK] = {      0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_GAMEPLAY_FOV] = {       0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_SIMULATION_CADENCE] = { 0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_FRAME_LIMIT]  = {      0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_MOTION_SMOOTHING] = {  0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_MODE]         = {       0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_WORLD_SHADOWS] = {      0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_AUDIO_MASTER_VOLUME] = {    100.0f,   100.0f,     100.0f },
    [MDKR_AUDIO_MUSIC_VOLUME] = {     100.0f,   100.0f,     100.0f },
    [MDKR_AUDIO_EFFECTS_VOLUME] = {   100.0f,   100.0f,     100.0f },
};

/*
 * RenderScale 2 in Restored and Remastered is the supersampling default, and it
 * is ON.
 *
 * A first attempt at this made it opt-in, because switching it on broke seven of
 * the twenty-eight checks and the obvious reading was that anti-aliasing
 * legitimately changes every pixel. That reading was wrong. Two of the seven
 * compare arms that supersampling affects IDENTICALLY -- rom_revision compares
 * ROM byte orders, renderer_backends compares backends -- so neither could be
 * explained by filtering at all. The real cause was a resize-debounce bug that
 * left the scene target oscillating between the output and render sizes
 * (gfx_webgpu.c, PERF-020). With that fixed all seven pass with supersampling
 * on, and the setting ships enabled where it belongs.
 */

/*
 * String-valued presets. NULL means "this mode does not pin the key" (the
 * resolved value survives); a non-NULL string is pinned at PRESET precedence.
 *
 * Video.Widescreen is 1 in EVERY mode, including Pure. Setting it to 0 does not
 * give a 4:3 image — it engages the pre-widescreen path (display_config.c:116)
 * where 4:3 content is STRETCHED to fill the window. Pure gets authentic
 * framing from Aspect=4:3, which pillarboxes undistorted and, because Hor+
 * computes against presentation_aspect, yields exactly the authored FOV.
 */
static const char *const s_preset_text[MDKR_VIDEO_KEY_COUNT][3] = {
    /*                                  pure   restored  remastered */
    [MDKR_VIDEO_ASPECT]       = {      "4:3",   "auto",     "auto" },
    [MDKR_VIDEO_TEXTURE_PACK] = {         "",     NULL,       NULL },
    [MDKR_VIDEO_GAMEPLAY_FOV] = { "authored", "authored", "authored" },
    [MDKR_VIDEO_SIMULATION_CADENCE] = {
        NULL, NULL, NULL
    },
    /*
     * Never pinned by any preset (spec §11 policy): Pure/Restored/Remastered
     * are presentation-mode art-direction presets, and FrameLimit/
     * MotionSmoothing are a presentation-RATE choice orthogonal to them, same
     * reasoning as Gameplay.SimulationCadence just above. A resolved value
     * (file/env/CLI/in-game) survives every preset switch untouched.
     */
    [MDKR_VIDEO_FRAME_LIMIT] = { NULL, NULL, NULL },
    [MDKR_VIDEO_MOTION_SMOOTHING] = { NULL, NULL, NULL },
    [MDKR_VIDEO_MODE]         = {     "pure", "restored", "remastered" },
    /*
     * Pinned in every mode, and "off" in Pure/Restored is the truth rather than
     * a policy: the receiver shader path is only compiled under RemasterFX, so
     * a non-off value in those modes would be a setting that displays a state
     * the image does not have. Remastered pins "full" — the shipped default
     * image is unchanged by this key's arrival, which is the whole point of
     * defaulting it there.
     */
    [MDKR_VIDEO_WORLD_SHADOWS] = {    "off",      "off",       "full" },
};

void mdkr_video_config_defaults(MdkrVideoConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    /* Restored is the safe default everywhere a player first meets the port.
     * Remastered remains an explicit opt-in; visual gates select it directly
     * whenever they exercise Remastered-only features. */
    config->mode = MDKR_VIDEO_MODE_RESTORED;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const char *text = s_preset_text[i][MDKR_VIDEO_MODE_RESTORED];
        config->values[i].number = s_preset[i][MDKR_VIDEO_MODE_RESTORED];
        snprintf(config->values[i].text, sizeof(config->values[i].text),
                 "%s", text != NULL ? text : "");
        config->values[i].source = MDKR_VIDEO_SOURCE_DEFAULT;
    }
    /*
     * Gameplay cadence is intentionally independent of the Remastered visual
     * preset used to seed the other defaults.
     */
    snprintf(
        config->values[MDKR_VIDEO_SIMULATION_CADENCE].text,
        sizeof(config->values[MDKR_VIDEO_SIMULATION_CADENCE].text),
        "%s", "original");
    /*
     * FrameLimit defaults to "original", NOT the "60" the spec §11 example ini
     * shows as its "suggested final semantics". Arbitrary-rate presentation is
     * opt-in: Original is the conservative default that preserves authored
     * visual motion without allocating replay resources. MotionSmoothing also
     * defaults to off; players who choose a higher presentation rate can opt
     * into immutable adjacent-task interpolation explicitly.
     */
    snprintf(
        config->values[MDKR_VIDEO_FRAME_LIMIT].text,
        sizeof(config->values[MDKR_VIDEO_FRAME_LIMIT].text),
        "%s", "original");
    snprintf(
        config->values[MDKR_VIDEO_MOTION_SMOOTHING].text,
        sizeof(config->values[MDKR_VIDEO_MOTION_SMOOTHING].text),
        "%s", "off");

    /* Window/input choices are player comfort, not presentation-mode state.
     * These defaults exactly preserve the pre-remapping SDL behavior, including
     * B/X as alternate N64 B inputs and both triggers as N64 Z. */
#define SET_DEFAULT_TEXT(KEY, VALUE) \
    snprintf(config->values[KEY].text, sizeof(config->values[KEY].text), \
             "%s", VALUE)
    SET_DEFAULT_TEXT(MDKR_WINDOW_MODE, "windowed");
    config->values[MDKR_INPUT_RUMBLE_ENABLED].number = 1.0f;
    SET_DEFAULT_TEXT(MDKR_INPUT_RUMBLE_PROFILE, "strong");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_A, "a");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_B, "b");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_X, "b");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_Y, "c_up");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_START, "start");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_LEFT_STICK, "none");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_STICK, "none");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_LEFT_SHOULDER, "l");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_SHOULDER, "r");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_DPAD_UP, "dpad_up");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_DPAD_DOWN, "dpad_down");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_DPAD_LEFT, "dpad_left");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_DPAD_RIGHT, "dpad_right");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_LEFT_TRIGGER, "z");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_TRIGGER, "z");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_STICK_UP, "c_up");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_STICK_DOWN, "c_down");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_STICK_LEFT, "c_left");
    SET_DEFAULT_TEXT(MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT, "c_right");
#undef SET_DEFAULT_TEXT
}

int mdkr_video_config_apply_preset_from(MdkrVideoConfig *config,
                                        MdkrVideoMode mode,
                                        MdkrVideoSource source) {
    MdkrVideoValue *mode_slot;

    if (config == NULL || (int) mode < 0 || (int) mode > MDKR_VIDEO_MODE_REMASTERED) {
        return 0;
    }
    mode_slot = &config->values[MDKR_VIDEO_MODE];
    if (source < mode_slot->source) {
        return 0;
    }
    config->mode = mode;
    snprintf(mode_slot->text, sizeof(mode_slot->text), "%s",
             s_preset_text[MDKR_VIDEO_MODE][mode]);
    mode_slot->source = source;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const MdkrVideoSchema *s = &s_schema[i];
        char number[64];

        if (i == MDKR_VIDEO_MODE ||
            mdkr_video_key_is_player_comfort((MdkrVideoKey)i)) {
            continue;
        }
        if (s->type == MDKR_VIDEO_TYPE_STRING) {
            const char *text = s_preset_text[i][mode];
            if (text != NULL) {
                (void) mdkr_video_config_set(config, (MdkrVideoKey) i, text, source);
            }
            continue;
        }
        snprintf(number, sizeof(number), "%.9g", (double) s_preset[i][mode]);
        (void) mdkr_video_config_set(config, (MdkrVideoKey) i, number, source);
    }
    return 1;
}

void mdkr_video_config_apply_preset(MdkrVideoConfig *config, MdkrVideoMode mode) {
    (void) mdkr_video_config_apply_preset_from(
        config, mode, MDKR_VIDEO_SOURCE_PRESET);
}

int mdkr_video_mode_from_name(const char *name) {
    if (mdkr_video_ci_equal(name, "pure"))       return MDKR_VIDEO_MODE_PURE;
    if (mdkr_video_ci_equal(name, "restored"))   return MDKR_VIDEO_MODE_RESTORED;
    if (mdkr_video_ci_equal(name, "remastered")) return MDKR_VIDEO_MODE_REMASTERED;
    if (mdkr_video_ci_equal(name, "custom"))     return MDKR_VIDEO_MODE_CUSTOM;
    return -1;
}

static int mdkr_video_parse_number(const char *value, float *out);

static int mdkr_video_validate_aspect(const char *value) {
    char *end;
    double numerator;
    double denominator;

    if (mdkr_video_ci_equal(value, "auto") ||
        mdkr_video_ci_equal(value, "window") ||
        mdkr_video_ci_equal(value, "native")) {
        return 1;
    }
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    numerator = strtod(value, &end);
    if (end == value || !isfinite(numerator)) {
        return 0;
    }
    if (*end == ':' || *end == '/') {
        const char *right = end + 1;
        denominator = strtod(right, &end);
        if (end == right || !isfinite(denominator) || denominator == 0.0) {
            return 0;
        }
        numerator /= denominator;
    }
    while (*end != '\0' && isspace((unsigned char) *end)) {
        end++;
    }
    return *end == '\0' && numerator >= 0.5 && numerator <= 4.0;
}

static int mdkr_video_validate_gameplay_fov(const char *value) {
    float parsed;

    if (mdkr_video_ci_equal(value, "authored") ||
        mdkr_video_ci_equal(value, "default") ||
        mdkr_video_ci_equal(value, "off")) {
        return 1;
    }
    return mdkr_video_parse_number(value, &parsed) &&
           parsed >= 20.0f && parsed <= 140.0f;
}

static int mdkr_video_validate_frame_limit(const char *value) {
    MdkrPresentPolicy policy;
    return mdkr_present_policy_parse(value, &policy);
}

static int mdkr_video_validate_motion_smoothing(const char *value) {
    return mdkr_video_ci_equal(value, "off") ||
           mdkr_video_ci_equal(value, "interpolate");
}

/*
 * World shadows resolve to one of three canonical words, but the key inherits
 * MDKR_WORLD_SHADOW — a seam that predates it and that every existing A/B gate
 * drives with "0"/"1" (and, historically, with an empty string meaning off).
 * Accepting those spellings and CANONICALISING them here is what lets the
 * setting and the diagnostic seam be the same thing: whatever a gate or a user
 * writes, the config, the ini and the options screen all show one of three
 * words. Returns NULL for anything unrecognised.
 */
const char *mdkr_video_world_shadows_canonical(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    if (value[0] == '\0' || mdkr_video_ci_equal(value, "off") ||
        mdkr_video_ci_equal(value, "0")) {
        return "off";
    }
    if (mdkr_video_ci_equal(value, "soft")) {
        return "soft";
    }
    if (mdkr_video_ci_equal(value, "full") || mdkr_video_ci_equal(value, "on") ||
        mdkr_video_ci_equal(value, "1")) {
        return "full";
    }
    return NULL;
}

static int mdkr_video_parse_number(const char *value, float *out) {
    char *end;
    float parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return 0;
    }
    parsed = strtof(value, &end);
    if (end == value) {
        return 0;
    }
    while (*end != '\0' && isspace((unsigned char) *end)) {
        end++;
    }
    if (*end != '\0' || !isfinite(parsed)) {
        return 0;
    }
    *out = parsed;
    return 1;
}

int mdkr_video_config_set(MdkrVideoConfig *config,
                          MdkrVideoKey key,
                          const char *value,
                          MdkrVideoSource source) {
    const MdkrVideoSchema *schema = mdkr_video_schema(key);
    MdkrVideoValue *slot;
    float parsed;

    if (config == NULL || schema == NULL || value == NULL) {
        return 0;
    }
    slot = &config->values[key];
    if (source < slot->source) {
        return 0;
    }

    if (schema->type == MDKR_VIDEO_TYPE_STRING) {
        const char *canonical = NULL;
        if (strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
            return 0;
        }
        if (key == MDKR_WINDOW_MODE) {
            canonical = mdkr_window_mode_canonical(value);
        } else if (key == MDKR_INPUT_RUMBLE_PROFILE) {
            canonical = mdkr_rumble_profile_canonical(value);
        } else if (key >= MDKR_INPUT_CONTROLLER_A &&
                   key <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT) {
            canonical = mdkr_controller_action_canonical(value);
        }
        if (key == MDKR_WINDOW_MODE || key == MDKR_INPUT_RUMBLE_PROFILE ||
            (key >= MDKR_INPUT_CONTROLLER_A &&
             key <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT)) {
            if (canonical == NULL) return 0;
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_MODE) {
            int mode = mdkr_video_mode_from_name(value);
            if (mode < 0) {
                return 0;
            }
            if (mode == MDKR_VIDEO_MODE_CUSTOM) {
                snprintf(slot->text, sizeof(slot->text), "%s", "custom");
                slot->source = source;
                config->mode = MDKR_VIDEO_MODE_CUSTOM;
                return 1;
            }
            return mdkr_video_config_apply_preset_from(
                config, (MdkrVideoMode) mode, source);
        }
        if (key == MDKR_VIDEO_WORLD_SHADOWS) {
            const char *canonical = mdkr_video_world_shadows_canonical(value);
            if (canonical == NULL) {
                return 0;
            }
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_MOTION_SMOOTHING &&
            mdkr_video_validate_motion_smoothing(value)) {
            snprintf(slot->text, sizeof(slot->text), "%s",
                     mdkr_video_ci_equal(value, "interpolate")
                         ? "interpolate" : "off");
            slot->source = source;
            return 1;
        }
        if ((key == MDKR_VIDEO_ASPECT && !mdkr_video_validate_aspect(value)) ||
            (key == MDKR_VIDEO_GAMEPLAY_FOV &&
             !mdkr_video_validate_gameplay_fov(value)) ||
            (key == MDKR_VIDEO_SIMULATION_CADENCE &&
             !mdkr_pacing_cadence_valid(value)) ||
            (key == MDKR_VIDEO_FRAME_LIMIT &&
             !mdkr_video_validate_frame_limit(value)) ||
            (key == MDKR_VIDEO_MOTION_SMOOTHING &&
             !mdkr_video_validate_motion_smoothing(value))) {
            return 0;
        }
        if (strlen(value) >= MDKR_VIDEO_STRING_MAX) {
            return 0;
        }
        snprintf(slot->text, sizeof(slot->text), "%s", value);
        slot->source = source;
        return 1;
    }

    if (!mdkr_video_parse_number(value, &parsed)) {
        return 0;
    }
    if (parsed < schema->min || parsed > schema->max) {
        return 0;
    }
    if (schema->type == MDKR_VIDEO_TYPE_INT && parsed != (float) (int) parsed) {
        return 0;
    }
    slot->number = parsed;
    slot->source = source;
    return 1;
}

/* --------------------------------------------------------------------------
 *  Resolution — pure. See the header for the precedence contract.
 * ------------------------------------------------------------------------ */

void mdkr_video_config_resolve(MdkrVideoConfig *config,
                               const ConfigIniEntry *file_entries,
                               int file_count,
                               MdkrVideoEnvLookup env_lookup,
                               int argc,
                               char *const *argv) {
    if (config == NULL) {
        return;
    }

    /* 1. File. Apply its mode as a base independent of key order, then its
     * individual values at the same rank so custom overrides survive. */
    for (int i = 0; i < file_count && file_entries != NULL; i++) {
        if (mdkr_video_key_from_name(file_entries[i].key) == MDKR_VIDEO_MODE) {
            mdkr_video_config_set(config, MDKR_VIDEO_MODE, file_entries[i].value,
                                  MDKR_VIDEO_SOURCE_FILE);
        }
    }
    for (int i = 0; i < file_count && file_entries != NULL; i++) {
        MdkrVideoKey key = mdkr_video_key_from_name(file_entries[i].key);
        if (key != MDKR_VIDEO_KEY_COUNT && key != MDKR_VIDEO_MODE) {
            mdkr_video_config_set(config, key, file_entries[i].value,
                                  MDKR_VIDEO_SOURCE_FILE);
        }
    }

    /* 2. Preset. The last mode flag on the command line wins. */
    for (int i = 1; i < argc && argv != NULL; i++) {
        int mode = mdkr_video_mode_from_name(
            strncmp(argv[i], "--", 2) == 0 ? argv[i] + 2 : "");
        if (mode >= 0) {
            mdkr_video_config_apply_preset(config, (MdkrVideoMode) mode);
        }
    }

    /* Browser launcher choices sit above a native preset but below an in-game
     * change, environment, or explicit CLI override. Unlike --pure, a launcher
     * Pure choice is not a read-only oracle session. */
    for (int i = 1; i < argc && argv != NULL; i++) {
        if (!strcmp(argv[i], "--video-launch-mode") && i + 1 < argc) {
            int mode = mdkr_video_mode_from_name(argv[++i]);
            if (mode >= MDKR_VIDEO_MODE_PURE &&
                mode <= MDKR_VIDEO_MODE_REMASTERED) {
                mdkr_video_config_apply_preset_from(
                    config, (MdkrVideoMode) mode,
                    MDKR_VIDEO_SOURCE_LAUNCHER);
            }
        } else if (!strcmp(argv[i], "--video-launch-set") && i + 1 < argc) {
            const char *pair = argv[++i];
            const char *eq = strchr(pair, '=');
            char name[MDKR_VIDEO_NAME_MAX];
            size_t length = eq != NULL ? (size_t) (eq - pair) : 0;
            if (length > 0 && length < sizeof(name)) {
                MdkrVideoKey key;
                memcpy(name, pair, length);
                name[length] = '\0';
                key = mdkr_video_key_from_name(name);
                if (key != MDKR_VIDEO_KEY_COUNT) {
                    mdkr_video_config_set(
                        config, key, eq + 1, MDKR_VIDEO_SOURCE_LAUNCHER);
                }
            }
        }
    }

    /* 3. Environment. */
    if (env_lookup != NULL) {
        const char *mode = env_lookup(s_schema[MDKR_VIDEO_MODE].env);
        if (mode != NULL && mode[0] != '\0') {
            mdkr_video_config_set(config, MDKR_VIDEO_MODE, mode,
                                  MDKR_VIDEO_SOURCE_ENV);
        }
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
            const char *v = env_lookup(s_schema[i].env);
            if (i != MDKR_VIDEO_MODE && v != NULL && v[0] != '\0') {
                mdkr_video_config_set(config, (MdkrVideoKey) i, v,
                                      MDKR_VIDEO_SOURCE_ENV);
            }
        }
    }

    /* Direct display flags are CLI-owned too. Resolving them here, in addition
     * to main_pc.c's final validation pass, keeps the menu's displayed value and
     * lock state truthful. */
    for (int i = 1; i < argc && argv != NULL; i++) {
        if (!strcmp(argv[i], "--aspect") && i + 1 < argc) {
            mdkr_video_config_set(config, MDKR_VIDEO_ASPECT, argv[++i],
                                  MDKR_VIDEO_SOURCE_CLI);
        } else if (!strcmp(argv[i], "--fov") && i + 1 < argc) {
            mdkr_video_config_set(config, MDKR_VIDEO_GAMEPLAY_FOV, argv[++i],
                                  MDKR_VIDEO_SOURCE_CLI);
        } else if (!strcmp(argv[i], "--widescreen")) {
            mdkr_video_config_set(config, MDKR_VIDEO_WIDESCREEN, "1",
                                  MDKR_VIDEO_SOURCE_CLI);
        } else if (!strcmp(argv[i], "--legacy-stretch")) {
            mdkr_video_config_set(config, MDKR_VIDEO_WIDESCREEN, "0",
                                  MDKR_VIDEO_SOURCE_CLI);
        }
    }

    /* 4. --video-set Key=Value. */
    for (int i = 1; i + 1 < argc && argv != NULL; i++) {
        const char *pair;
        const char *eq;
        char name[MDKR_VIDEO_NAME_MAX];
        size_t nl;
        MdkrVideoKey key;

        if (strcmp(argv[i], "--video-set") != 0) {
            continue;
        }
        pair = argv[i + 1];
        i++;

        eq = strchr(pair, '=');
        if (eq == NULL) {
            continue;
        }
        nl = (size_t) (eq - pair);
        if (nl == 0 || nl >= sizeof(name)) {
            continue;
        }
        memcpy(name, pair, nl);
        name[nl] = '\0';

        key = mdkr_video_key_from_name(name);
        if (key != MDKR_VIDEO_KEY_COUNT) {
            mdkr_video_config_set(config, key, eq + 1, MDKR_VIDEO_SOURCE_CLI);
        }
    }
}

int mdkr_video_config_readonly_for(const MdkrVideoConfig *config) {
    return (config != NULL && config->mode == MDKR_VIDEO_MODE_PURE &&
            config->values[MDKR_VIDEO_MODE].source == MDKR_VIDEO_SOURCE_PRESET) ? 1 : 0;
}
