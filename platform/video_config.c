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
        "Superseded by Content.PacksEnabled and the mods folder. Kept so an "
        "existing settings file still parses; it has no runtime effect.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_CONTENT_PACKS_ENABLED] = {
        "Content.PacksEnabled", "MDKR_CONTENT_PACKS",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Custom content",
        "Apply installed content packs. Tab switches them off and back on "
        "while you play, so you can compare against the original.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_CONTENT_PACK_DISABLED] = {
        "Content.PackDisabled", "MDKR_CONTENT_PACK_DISABLED",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Skipped packs",
        "Comma-separated pack names to leave uninstalled. Set from the "
        "Content list rather than typed.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_ENH_SPEEDOMETER] = {
        "Enhancements.Speedometer", "MDKR_ENH_SPEEDOMETER",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 2.0f,
        "Speedometer",
        "Show your current speed. 0 off, 1 miles per hour, 2 kilometres per "
        "hour. Changes only how the game looks.",
        MDKR_VIDEO_CAT_INTERFACE
    },
    [MDKR_ENH_DRAW_DISTANCE] = {
        "Enhancements.DrawDistance", "MDKR_ENH_DRAW_DISTANCE",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 100.0f, 400.0f,
        "Draw distance",
        "How far ahead scenery is drawn, as a percentage of the authored "
        "distance. Changes only how the game looks.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_ENH_LOD_BIAS] = {
        "Enhancements.LodBias", "MDKR_ENH_LOD_BIAS",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 2.0f,
        "Model detail",
        "0 keeps the authored detail switching. 1 and 2 hold higher-detail "
        "models further out. Changes only how the game looks.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_APP_UPDATE_CHECK] = {
        "App.UpdateCheck", "MDKR_UPDATE_CHECK",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Check for updates",
        "Look once a day for a newer release and show a notice in the "
        "launcher. Nothing is downloaded or installed.",
        MDKR_VIDEO_CAT_INTERFACE
    },
    [MDKR_ENH_AI_DIFFICULTY] = {
        "Enhancements.AIDifficulty", "MDKR_ENH_AI_DIFFICULTY",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Opponent skill",
        "authored races the opponents as they were written. hard and brutal "
        "make them faster. Changes how the game plays.",
        MDKR_VIDEO_CAT_PACING
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
     * SCOPE_LIVE with an apply DOMAIN, which is a different thing from the
     * plain SCOPE_LIVE Video.Widescreen / Video.Aspect / Video.RenderScale
     * carry. Those are complete when publish() returns. This one is not, and
     * the reason is the pair of latches this key's receivers hold:
     *
     *   - platform_sdl_min.c's present_pace_lazy_init() resolves the present
     *     policy ONCE, on the first platform_present_subloop_fields() call,
     *     into file-static deadline state.
     *   - gfx_pc_dkr.c's gfx_start_frame()/gfx_end_frame() capture the replay's
     *     walk-entry state (dkr_walk_entry_*) and freeze the shadow matrix
     *     registry only when present_sched_replay_armed() is true, and
     *     present_sched.c arms the presentation snapshot store behind a
     *     one-shot (s_snapshot_forced).
     *
     * Writing the new value straight from the setter is unsafe in BOTH
     * directions, and this is the hazard the deferred apply exists to close.
     * Engaging late is dead cost: the freeze/snapshot work starts happening but
     * the subloop never runs, because the platform policy was already latched
     * inactive. Disengaging late is worse than dead: the subloop is still
     * latched ON, while present_sched_replay_armed() has gone false, so
     * gfx_start_frame stops refreshing dkr_walk_entry_* -- which
     * gfx_dkr_replay_invalidate() is the only thing that ever clears -- and
     * gfx_dkr_replay_walk() goes on memcpy'ing a STALE segment table
     * (gfx_pc_dkr.c's replay branch) into the live HLE state. Stale segment
     * bases are wild pointers.
     *
     * WHAT MAKES IT SAFE NOW. Nothing here is bypassed; the latches are still
     * latches, and they are re-latched rather than mutated. The setter only
     * marks MDKR_VIDEO_APPLY_PRESENTATION pending. The engine's host-frame
     * boundary (stubs_dkr.c's osRecvMesg video-queue branch, entered after the
     * previous authoritative pass and its whole subloop have completed, and
     * before the next tick's platform_present_subloop_fields() call) then runs
     * platform_present_config_apply() as ONE ordered step:
     * gfx_dkr_replay_invalidate() first -- so no walk-entry state survives the
     * change in either direction -- then the snapshot stage reset, then the
     * policy push, then a full re-init of the pacer state machine and the
     * backend's present mode. There is no host opportunity at which a replay
     * can observe a walk entry captured under the other policy, because the
     * only thing that can observe one is the subloop, and the subloop is not
     * running at that boundary.
     */
    [MDKR_VIDEO_FRAME_LIMIT] = {
        "Video.FrameLimit", "MDKR_PRESENT_RATE",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Frame limit",
        "original presents each authored image once. display follows the "
        "monitor, display-margin sits a few Hz under it, a number sets a "
        "native cap, and uncapped removes the native "
        "software cap (the browser maps both display-margin and uncapped to "
        "display). display-margin is for a variable-refresh display: staying "
        "just below the top of its range keeps it adapting to the game instead "
        "of falling back to a fixed refresh, and it re-reads the rate if you "
        "move the window to another monitor. A cap of 40 is a good "
        "battery-friendly choice on a handheld whose display runs at 40 or "
        "120 Hz. Rates above your "
        "display's refresh need a display connection that can drop an image it "
        "has not shown yet; where the system does not offer one, they present "
        "at your display's refresh instead, unless Allow tearing is on. Pair a "
        "rate above original with Motion smoothing = Interpolated for unique "
        "in-between images. Gameplay remains on its fixed original cadence. "
        "A European (50 Hz) ROM paces unevenly under original: its authored "
        "image lasts 40 ms, which is not a whole number of 60 Hz refreshes, so "
        "original alternates between holding it for two and for three and the "
        "motion looks uneven. display with interpolate removes that without "
        "changing game speed, music pitch, or timers. "
        "Takes effect on the next frame; you can change it while you play.",
        MDKR_VIDEO_CAT_PACING
    },
    /*
     * LIVE with the same PRESENTATION domain as Video.FrameLimit above, and
     * that is not merely inherited: present_sched_replay_armed() short-circuits
     * to false when smoothing is off, so a run with MotionSmoothing=off never
     * arms the snapshot store, never captures dkr_walk_entry_* and never
     * freezes the registry. Flipping it to interpolate would otherwise drive
     * the subloop into a replay with no frozen registry and no snapshot pair --
     * every intermediate frame silently identical to the tick's own -- and the
     * reverse flip lands in the stale-walk-entry hazard FrameLimit's note
     * describes.
     *
     * Both of those are properties of the state at the moment of the flip, and
     * both are answered by the same ordered apply: the boundary invalidates the
     * replay history and re-arms the snapshot one-shot, so the first tick after
     * an off->interpolate change starts capturing again from nothing, and the
     * first tick after interpolate->off leaves nothing behind that a later
     * re-arm could inherit. gfx_dkr_replay_walk() additionally refuses outright
     * while dkr_walk_entry_valid is false, so the window between the boundary
     * and the next real walk cannot replay at all -- it holds the authored
     * image, which is exactly what smoothing=off looks like anyway.
     */
    [MDKR_VIDEO_MOTION_SMOOTHING] = {
        "Video.MotionSmoothing", "MDKR_PRESENT_SMOOTHING",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Motion smoothing",
        "interpolate draws presentation-only in-between images from adjacent "
        "authored tasks. Cameras, objects, and scrolling surfaces such as "
        "waterfalls, water, and lava all move in step. It does not advance "
        "physics, AI, timers, audio, or input more often. off presents only the "
        "game's authored images. Takes effect on the next frame; you can change "
        "it while you play.",
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
    /*
     * SCOPE_LEVEL, in the CAMERA domain -- and the choice of LEVEL over LIVE is
     * the one scope decision here that is not forced by a safety argument, so
     * it is worth being exact about which argument it IS.
     *
     * The setting was RESTART because game/src/camera_obstruction_runtime.c
     * resolved MDKR_CAMERA_OBSTRUCTION with no way for anything but a relaunch
     * to change what it read; the launcher exports the resolved value onto that
     * same variable before engine entry (platform/app/engine_boot.cpp), so a
     * value set here and a value set in the environment reach the runtime
     * through one seam. That seam now has a setter beside it
     * (camera_obstruction_runtime_set_policy) and the env read is the fallback,
     * so "relaunch the whole app" is no longer the honest answer.
     *
     * NOT LIVE, for two reasons, neither of which is memory safety:
     *
     *   1. The obstruction runtime's only proven re-init is
     *      camera_obstruction_runtime_reset(), and every existing caller of it
     *      is a level boundary (thread3_main.c, before cam_init()). Reusing it
     *      there costs nothing and inherits a path the camera suite already
     *      covers. Mid-race it would zero live state -- presentation_depth,
     *      viewport_slot_valid, last_physical_slot_by_viewport -- whose
     *      invariants are maintained across the render's begin/end scope, and
     *      the value of proving that at a new boundary is nil, because:
     *   2. A policy flip is a HARD CUT of the rendered eye. The resolver's
     *      whole retract/recovery design (RELEASE_HOLD_TICKS, the transition
     *      cut census, the discontinuity flag) exists so the corrected camera
     *      never teleports, and it has no path that fades between two
     *      policies. A level load is the one moment the game already cuts the
     *      camera, so applying there is not a compromise -- it is the only
     *      moment the change is invisible.
     *
     * The player-facing cost of LEVEL over LIVE is a menu transition, and the
     * thing that actually mattered -- that choosing a camera should not mean
     * quitting the app -- is delivered either way.
     */
    [MDKR_VIDEO_CAMERA_OBSTRUCTION] = {
        "Camera.Obstruction", "MDKR_CAMERA_OBSTRUCTION",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LEVEL, 0.0f, 0.0f,
        "Camera",
        "Authored is the camera the game writes, unchanged, and it is what "
        "you get unless you change this. Keep the camera out of walls pulls "
        "it in front of walls, doors, and anything else solid that would come "
        "between it and your racer. Only the picture moves either way: "
        "handling, results, ghosts, and saves are identical. Takes effect at "
        "the next race, so the camera never jumps mid-corner. In a config "
        "file or the environment the two values are observe and modern.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    /*
     * Off by default: every frame limit hands the backend a vblank-synchronized
     * queue, and this is the only control that gives that up for latency.
     *
     * SCOPE_LIVE in the PRESENTATION domain, for a reason of its own rather
     * than by inheritance from the two pacing keys above. The claim it used to
     * carry -- that the display connection is set up once at launch -- was only
     * ever half true, and the half that was true stopped being true in M3:
     *
     *   - The WebGPU swapchain does bake its present mode at surface
     *     configuration, but gfx_webgpu_request_surface_reconfigure() already
     *     exists to re-rank it, because a display change invalidates the same
     *     ranking for reasons that have nothing to do with this key.
     *   - The GL swap interval is not fixed at context adoption at all.
     *     sdl_apply_gl_present_policy() is a plain SDL_GL_SetSwapInterval call
     *     that an existing context accepts at any time; it was simply only ever
     *     called once.
     *
     * Both are re-applied by the domain's boundary applier. Tearing rides with
     * the pacing keys rather than beside them because the effective present
     * mode is a function of policy AND tearing together (platform_sdl_min.c's
     * platform_present_display_quantum_units consults both), so re-ranking one
     * without the other would leave the pacer projecting onto a vblank the
     * backend is no longer waiting for.
     */
    [MDKR_VIDEO_ALLOW_TEARING] = {
        "Video.AllowTearing", "MDKR_ALLOW_TEARING",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Allow tearing (lowest latency)",
        "Shows finished frames without waiting for the display, where the "
        "system allows it. That is the lowest input delay, and the picture may "
        "show a seam across it while things are moving. Leave this off unless "
        "you are chasing latency and prefer the seam. If your display has a "
        "variable refresh rate, leave it off: the display already adapts to "
        "the game, and turning this on gives that up for a seam you do not "
        "need. Frame limit = Just under display is the setting that suits "
        "those displays. Takes effect on the next "
        "frame; you can change it while you play.",
        MDKR_VIDEO_CAT_PACING
    },
    /*
     * Every cartridge carries the same text and font assets regardless of
     * region -- pal.v80 and us.v80 are byte-identical across every asset
     * section (docs/ROM_REVISIONS.md), so a US disc already holds working
     * German menus and subtitles, just under a retail menu that never lists
     * them. "all" offers every language the running disc's assets carry, and
     * is the default: it is non-interfering (no gameplay, save, or ghost data
     * changes) and simply shows a player more of what their disc already has.
     * "authentic" is the opt-out for a player who wants their own disc's
     * retail menu back.
     *
     * SCOPE_LIVE: game/src/menu.c's menu_language_has_german() re-reads the
     * resolved config every time the player moves the language selector, so
     * nothing needs to be latched at boot.
     */
    [MDKR_VIDEO_MENU_LANGUAGES] = {
        "Gameplay.MenuLanguages", "MDKR_MENU_LANGUAGES",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 0.0f,
        "Menu languages",
        "Every cartridge holds the same on-disc translations, in every "
        "language, no matter which region it was sold in. By default the "
        "in-game language selector offers all of them. Authentic restores "
        "your disc's own retail menu -- the language list its region shipped "
        "with, nothing added.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
    /*
     * SCOPE_LEVEL, in the CAMERA domain, riding the applier Camera.Obstruction
     * already installed (game/src/camera_obstruction_runtime.c's
     * camera_obstruction_runtime_apply_config). Both keys are read by the same
     * runtime and both are cleared by the same level-boundary reset, so this
     * key adds a value to an existing seam rather than a second one.
     *
     * NOT LIVE, and for a weaker reason than Camera.Obstruction's: a comfort
     * flip is not a hard cut of the eye -- the vertical filter converges and
     * the recovery rate is re-read every tick, so mid-race application would
     * be merely a visible softening rather than a teleport. It is LEVEL
     * because the runtime's proven re-init is still only
     * camera_obstruction_runtime_reset(), the per-slot filter state this key
     * adds is cleared by exactly that reset, and buying a new mid-race
     * boundary for a setting a player changes once is not worth proving.
     *
     * PRESENTATION-ONLY, on the same terms as the camera it softens. Both
     * effects live entirely inside the obstruction runtime's sidecar: the
     * vertical filter is applied to the resolver's *desired* eye, before the
     * solve, so the published pose is still proven clear of geometry; and the
     * recovery rate only bounds EXPANSION, never retraction. Neither writes
     * Camera, gCameras, or any simulation state -- which is not a style
     * preference here, because platform/sim_hash.c hashes
     * gCameras[PLAYER_FOUR].trans and shakeMagnitude as authoritative (the
     * 3P/T.T. camera feeds next-tick object sort and LOD). Softening the
     * authored shake where the game writes it -- racer.c's
     * `trans.y_position += y_velocity + shakeMagnitude` -- would therefore
     * change the simulation, so this key does not go near it.
     */
    [MDKR_VIDEO_CAMERA_COMFORT] = {
        "Camera.Comfort", "MDKR_CAMERA_COMFORT",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_LEVEL, 0.0f, 0.0f,
        "Camera motion",
        "Reduced motion calms the camera: it smooths out the vertical shake "
        "from bumps, landings and explosions, and eases the camera back out "
        "more gently after it has squeezed past a wall. Authored is the "
        "default and leaves every bit of that motion exactly as the game "
        "wrote it. This changes the picture only -- handling, results, "
        "ghosts, and saves are identical -- and it applies to the "
        "keep-out-of-walls camera, so it has nothing to soften if you have "
        "chosen the original camera. Takes effect at the next race. In a "
        "config file or the environment the two values are authored and "
        "reduced.",
        MDKR_VIDEO_CAT_PRESENTATION
    },
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

/*
 * The domain table, kept here rather than as a schema column so the invariant
 * below can be stated as code. A key that names a domain is making a claim
 * about a specific receiver's re-init seam, and there are four of them; a
 * fifth would be a design decision, not a table edit.
 */
MdkrVideoApplyDomain mdkr_video_key_apply_domain(MdkrVideoKey key) {
    switch (key) {
        case MDKR_VIDEO_FRAME_LIMIT:
        case MDKR_VIDEO_MOTION_SMOOTHING:
        case MDKR_VIDEO_ALLOW_TEARING:
            return MDKR_VIDEO_APPLY_PRESENTATION;
        case MDKR_VIDEO_CAMERA_OBSTRUCTION:
        case MDKR_VIDEO_CAMERA_COMFORT:
            return MDKR_VIDEO_APPLY_CAMERA;
        default:
            return MDKR_VIDEO_APPLY_NONE;
    }
}

int mdkr_video_key_is_audio(MdkrVideoKey key) {
    return key == MDKR_AUDIO_MASTER_VOLUME ||
           key == MDKR_AUDIO_MUSIC_VOLUME ||
           key == MDKR_AUDIO_EFFECTS_VOLUME;
}

int mdkr_video_key_is_input(MdkrVideoKey key) {
    return key >= MDKR_INPUT_FIRST_KEY && key <= MDKR_INPUT_LAST_KEY;
}

int mdkr_video_key_is_content(MdkrVideoKey key) {
    return key == MDKR_CONTENT_PACKS_ENABLED ||
           key == MDKR_CONTENT_PACK_DISABLED;
}

int mdkr_video_key_is_enhancement(MdkrVideoKey key) {
    return key == MDKR_ENH_SPEEDOMETER || key == MDKR_ENH_DRAW_DISTANCE ||
           key == MDKR_ENH_LOD_BIAS || key == MDKR_ENH_AI_DIFFICULTY;
}

int mdkr_video_key_is_player_comfort(MdkrVideoKey key) {
    /*
     * Content and enhancement keys join audio, input and window mode here for
     * the same reason those three are exempt: Pure/Restored/Remastered are
     * art-direction presets, and none of these is art direction. A player who
     * installed a pack or turned the speedometer on chose that deliberately,
     * and switching presentation mode to compare two looks must not silently
     * undo it. Without this exemption every preset switch would re-pin them to
     * the preset table's zero row, which for Enhancements.DrawDistance is not
     * even inside its own 100..400 range.
     */
    return mdkr_video_key_is_audio(key) || mdkr_video_key_is_input(key) ||
           mdkr_video_key_is_content(key) ||
           mdkr_video_key_is_enhancement(key) ||
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
    [MDKR_VIDEO_CAMERA_OBSTRUCTION] = {  0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_AUDIO_MASTER_VOLUME] = {    100.0f,   100.0f,     100.0f },
    [MDKR_AUDIO_MUSIC_VOLUME] = {     100.0f,   100.0f,     100.0f },
    [MDKR_AUDIO_EFFECTS_VOLUME] = {   100.0f,   100.0f,     100.0f },
    [MDKR_VIDEO_MENU_LANGUAGES] = {     0.0f,     0.0f,       0.0f }, /* string; see below */
    [MDKR_VIDEO_CAMERA_COMFORT] = {     0.0f,     0.0f,       0.0f }, /* string; see below */
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
    /*
     * Never pinned by any preset, for the FrameLimit/MotionSmoothing reason
     * rather than the Audio one: this is presentation, so it belongs in the
     * preset table and is subject to Pure's read-only rule, but which of the
     * two cameras a player is racing with is their standing choice rather than
     * an art-direction one, and no mode a player picks may switch it behind
     * them -- in either direction.
     * mdkr_video_config_defaults() seeds "observe" for all three; a resolved
     * value survives every preset switch untouched.
     */
    [MDKR_VIDEO_CAMERA_OBSTRUCTION] = { NULL, NULL, NULL },
    /* Never pinned by any preset, for the Camera.Obstruction reason and one
     * more: a reduced-motion choice is an accessibility setting, and no visual
     * preset may reach in and undo it. defaults() seeds "authored". */
    [MDKR_VIDEO_CAMERA_COMFORT] = { NULL, NULL, NULL },
    /* Never pinned, for the FrameLimit reason: a latency preference is not an
     * art direction. */
    [MDKR_VIDEO_ALLOW_TEARING] = { NULL, NULL, NULL },
    /* Never pinned by any preset, for the Camera.Obstruction reason: which
     * languages the selector lists is a player's standing choice, not a
     * property of the visual preset they happen to be in.
     * mdkr_video_config_defaults() seeds "all"; a resolved value survives
     * every preset switch untouched. */
    [MDKR_VIDEO_MENU_LANGUAGES] = { NULL, NULL, NULL },
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
    /*
     * The authored camera is the default in every mode. Correcting it is a
     * deliberate choice a player makes once, not a property of the
     * presentation preset they happen to be in -- that is why neither camera
     * is pinned by Pure, Restored or Remastered above.
     *
     * This seeded "modern" for one wave and was seeded back on 2026-08-07,
     * when device acceptance rejected the corrected camera as a default.
     */
    snprintf(
        config->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
        sizeof(config->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text),
        "%s", "observe");
    /* Authored motion is the default: comfort is an accessibility opt-in, and
     * nothing a player did not ask for may soften the camera the game
     * writes. */
    snprintf(
        config->values[MDKR_VIDEO_CAMERA_COMFORT].text,
        sizeof(config->values[MDKR_VIDEO_CAMERA_COMFORT].text),
        "%s", "authored");
    snprintf(
        config->values[MDKR_VIDEO_ALLOW_TEARING].text,
        sizeof(config->values[MDKR_VIDEO_ALLOW_TEARING].text),
        "%s", "off");
    /*
     * All is the default in every mode: every cartridge already carries every
     * language's text and fonts, so showing the full list is non-interfering
     * (no gameplay, save, or ghost data changes) and simply surfaces what the
     * disc already has. Authentic is the opt-out for a player who wants their
     * own disc's retail menu back.
     */
    snprintf(
        config->values[MDKR_VIDEO_MENU_LANGUAGES].text,
        sizeof(config->values[MDKR_VIDEO_MENU_LANGUAGES].text),
        "%s", "all");

    /*
     * Content packs apply when they are installed. The default is ON rather
     * than off because installing a pack IS the opt-in -- an installed pack
     * that silently does nothing until a second setting is found is the most
     * common modding support question there is. With no mods directory the
     * whole path is inert, so this default costs nothing to a player who has
     * never heard of packs.
     */
    config->values[MDKR_CONTENT_PACKS_ENABLED].number = 1.0f;

    /*
     * Looking for a newer release defaults to ON, but the notice it produces
     * is a single line in the launcher and nothing is ever downloaded or
     * installed. A player who does not want the request makes at all sets this
     * to 0, and then no request is made -- not a request whose result is
     * discarded.
     */
    config->values[MDKR_APP_UPDATE_CHECK].number = 1.0f;

    /*
     * Enhancements default to the authored game. DrawDistance is seeded
     * explicitly because its 100..400 range does not contain the zero the
     * preset table would otherwise leave here, and a value outside its own
     * schema range is exactly the kind of quiet invalid state that survives
     * until something far away divides by it.
     */
    config->values[MDKR_ENH_SPEEDOMETER].number = 0.0f;
    config->values[MDKR_ENH_DRAW_DISTANCE].number = 100.0f;
    config->values[MDKR_ENH_LOD_BIAS].number = 0.0f;
    snprintf(
        config->values[MDKR_ENH_AI_DIFFICULTY].text,
        sizeof(config->values[MDKR_ENH_AI_DIFFICULTY].text),
        "%s", "authored");

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

static int mdkr_video_validate_motion_smoothing(const char *value) {
    return mdkr_video_ci_equal(value, "off") ||
           mdkr_video_ci_equal(value, "interpolate");
}

int mdkr_video_frame_limit_canonical(const char *value, char *out, size_t cap) {
    MdkrPresentPolicy policy;

    if (value == NULL || out == NULL || cap == 0 ||
        !mdkr_present_policy_parse(value, &policy)) {
        return 0;
    }
    switch (policy.kind) {
        case MDKR_PRESENT_DISPLAY:
            snprintf(out, cap, "display");
            return 1;
        case MDKR_PRESENT_DISPLAY_MARGIN:
            snprintf(out, cap, "display-margin");
            return 1;
        case MDKR_PRESENT_UNCAPPED:
            snprintf(out, cap, "uncapped");
            return 1;
        case MDKR_PRESENT_CAPPED:
            snprintf(out, cap, "%u", policy.rate);
            return 1;
        case MDKR_PRESENT_ORIGINAL:
        default:
            snprintf(out, cap, "original");
            return 1;
    }
}

/* --- Presentation pace (see video_config.h) ------------------------------ */

int mdkr_video_presentation_pace_values(MdkrPresentationPace pace,
                                        const char **frame_limit,
                                        const char **motion_smoothing) {
    const char *limit;
    const char *smoothing;

    switch (pace) {
        case MDKR_PRESENTATION_PACE_ORIGINAL:
            limit = "original";
            smoothing = "off";
            break;
        case MDKR_PRESENTATION_PACE_SMOOTH:
            limit = "display";
            smoothing = "interpolate";
            break;
        case MDKR_PRESENTATION_PACE_CUSTOM:
        default:
            return 0;
    }
    if (frame_limit != NULL) *frame_limit = limit;
    if (motion_smoothing != NULL) *motion_smoothing = smoothing;
    return 1;
}

MdkrPresentationPace mdkr_video_presentation_pace(
    const MdkrVideoConfig *config) {
    static const MdkrPresentationPace kPaces[] = {
        MDKR_PRESENTATION_PACE_ORIGINAL, MDKR_PRESENTATION_PACE_SMOOTH
    };

    if (config == NULL) {
        return MDKR_PRESENTATION_PACE_CUSTOM;
    }
    for (size_t i = 0; i < sizeof(kPaces) / sizeof(kPaces[0]); i++) {
        const char *limit = NULL;
        const char *smoothing = NULL;
        if (!mdkr_video_presentation_pace_values(kPaces[i], &limit,
                                                 &smoothing)) {
            continue;
        }
        /* Case-insensitively, because the comparison must agree with the
         * validators rather than with however the value happened to be typed;
         * mdkr_video_config_set canonicalises both keys, so in practice this
         * only matters for a config assembled by hand in a test. */
        if (mdkr_video_ci_equal(config->values[MDKR_VIDEO_FRAME_LIMIT].text,
                                limit) &&
            mdkr_video_ci_equal(
                config->values[MDKR_VIDEO_MOTION_SMOOTHING].text, smoothing)) {
            return kPaces[i];
        }
    }
    return MDKR_PRESENTATION_PACE_CUSTOM;
}

const char *mdkr_video_presentation_pace_name(MdkrPresentationPace pace) {
    switch (pace) {
        case MDKR_PRESENTATION_PACE_ORIGINAL: return "original";
        case MDKR_PRESENTATION_PACE_SMOOTH:   return "smooth";
        case MDKR_PRESENTATION_PACE_CUSTOM:
        default:                              return "custom";
    }
}

int mdkr_video_presentation_pace_from_name(const char *name) {
    if (name == NULL) {
        return -1;
    }
    if (mdkr_video_ci_equal(name, "original")) {
        return (int) MDKR_PRESENTATION_PACE_ORIGINAL;
    }
    if (mdkr_video_ci_equal(name, "smooth")) {
        return (int) MDKR_PRESENTATION_PACE_SMOOTH;
    }
    return -1;
}

/* Two words for the player, and the "0"/"1" spellings so the env name doubles
 * as the diagnostic seam the backends already read. */
const char *mdkr_video_allow_tearing_canonical(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    if (value[0] == '\0' || mdkr_video_ci_equal(value, "off") ||
        mdkr_video_ci_equal(value, "0")) {
        return "off";
    }
    if (mdkr_video_ci_equal(value, "on") || mdkr_video_ci_equal(value, "1")) {
        return "on";
    }
    return NULL;
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

/*
 * Two player-facing words, nothing else, for Gameplay.MenuLanguages' reason:
 * this key is new, so there is no older diagnostic spelling to stay compatible
 * with. Empty is rejected rather than folded into "authored" -- an explicit
 * MDKR_CAMERA_COMFORT that does not parse should fail the same way as any
 * other unrecognised value.
 */
const char *mdkr_video_camera_comfort_canonical(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    if (mdkr_video_ci_equal(value, "authored")) {
        return "authored";
    }
    if (mdkr_video_ci_equal(value, "reduced")) {
        return "reduced";
    }
    return NULL;
}

/*
 * Two of these four words are a player's choice and two are diagnostics.
 * "observe" and "modern" are what the settings UI offers and what the ini and
 * the options screen ever show. "legacy" and "center-ray" are arms of the
 * MDKR_CAMERA_OBSTRUCTION seam this key inherits: an A/B gate that sets the
 * environment must keep resolving, so they are accepted and returned verbatim
 * rather than folded into either player-facing state, which would make the
 * config claim a policy the camera runtime is not running. Returns NULL for
 * anything unrecognised, including the empty string -- unlike the world-shadow
 * seam, an empty MDKR_CAMERA_OBSTRUCTION means "unset" to the runtime, and
 * spelling that as a stored value would invent a state the runtime has no
 * word for.
 */
const char *mdkr_video_camera_obstruction_canonical(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    if (mdkr_video_ci_equal(value, "observe")) {
        return "observe";
    }
    if (mdkr_video_ci_equal(value, "modern")) {
        return "modern";
    }
    if (mdkr_video_ci_equal(value, "legacy")) {
        return "legacy";
    }
    if (mdkr_video_ci_equal(value, "center-ray")) {
        return "center-ray";
    }
    return NULL;
}

/*
 * Two player-facing words, nothing else. Unlike the world-shadow and camera
 * seams, this key has no pre-existing diagnostic spelling to stay compatible
 * with -- it is new -- so there is no reason to accept anything but its own
 * two states. Empty is rejected rather than folded into "authentic": an
 * explicit MDKR_MENU_LANGUAGES that does not parse should fail the same way
 * as any other unrecognised value, not silently resolve to the default.
 */
const char *mdkr_video_menu_languages_canonical(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    if (mdkr_video_ci_equal(value, "authentic")) {
        return "authentic";
    }
    if (mdkr_video_ci_equal(value, "all")) {
        return "all";
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
        if (key == MDKR_VIDEO_ALLOW_TEARING) {
            const char *canonical = mdkr_video_allow_tearing_canonical(value);
            if (canonical == NULL) {
                return 0;
            }
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_CAMERA_OBSTRUCTION) {
            const char *canonical =
                mdkr_video_camera_obstruction_canonical(value);
            if (canonical == NULL) {
                return 0;
            }
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_CAMERA_COMFORT) {
            const char *canonical = mdkr_video_camera_comfort_canonical(value);
            if (canonical == NULL) {
                return 0;
            }
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_MENU_LANGUAGES) {
            const char *canonical = mdkr_video_menu_languages_canonical(value);
            if (canonical == NULL) {
                return 0;
            }
            snprintf(slot->text, sizeof(slot->text), "%s", canonical);
            slot->source = source;
            return 1;
        }
        if (key == MDKR_VIDEO_FRAME_LIMIT) {
            char canonical[MDKR_VIDEO_STRING_MAX];
            if (!mdkr_video_frame_limit_canonical(value, canonical,
                                                  sizeof(canonical))) {
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
