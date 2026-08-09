/**
 * video_config.h — video/presentation policy for the native port.
 *
 * The pure half of this module (schema, defaults, presets, precedence,
 * resolution) reads no globals and no environment, so tests/test_video_config.c
 * exercises it without a ROM, a window, or a GPU — the same seam
 * display_config.h uses.
 *
 * The impure half (mdkr_video_config_init / _publish / _report) reads the ini
 * file, the environment and argv, then publishes into the g_pc* globals the
 * fast3d backends link against and into display_config's widescreen/aspect
 * state.
 *
 * Three presentation modes:
 *
 *   pure        the original game presented honestly — authentic 4:3 framing at
 *               the authored FOV, zero enhancements. The oracle reference.
 *   restored    the default: original art direction at modern fidelity.
 *               Changes sharpness and stability, never the art direction.
 *   remastered  opt-in, work-in-progress art-directed effects on top of
 *               Restored.
 */
#ifndef MDKR64_VIDEO_CONFIG_H
#define MDKR64_VIDEO_CONFIG_H

#include "config_ini.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_VIDEO_STRING_MAX 1024
#define MDKR_VIDEO_NAME_MAX   64

typedef enum MdkrVideoKey {
    MDKR_VIDEO_REMASTER_FX = 0,
    MDKR_VIDEO_WIDESCREEN,
    MDKR_VIDEO_ASPECT,
    MDKR_VIDEO_RENDER_SCALE,
    MDKR_VIDEO_MSAA,
    MDKR_VIDEO_ANISOTROPY,
    MDKR_VIDEO_MIPMAPS,
    MDKR_VIDEO_TEXTURE_PACK,
    MDKR_VIDEO_GAMEPLAY_FOV,
    MDKR_VIDEO_SIMULATION_CADENCE,
    MDKR_VIDEO_FRAME_LIMIT,
    MDKR_VIDEO_MOTION_SMOOTHING,
    MDKR_VIDEO_MODE,
    /*
     * Appended, not inserted. The enum is the schema table's index and the
     * in-game menu's wheel order; inserting in the middle would renumber the
     * pacing keys another lane owns. New keys go here.
     */
    MDKR_VIDEO_WORLD_SHADOWS,
    /* Audio is stored in the same durable player-settings registry, but is
     * deliberately independent of presentation presets and custom-mode state. */
    MDKR_AUDIO_MASTER_VOLUME,
    MDKR_AUDIO_MUSIC_VOLUME,
    MDKR_AUDIO_EFFECTS_VOLUME,
    /* Native shell and normalized-controller comfort settings. Like Audio,
     * these are independent of Pure/Restored/Remastered presentation presets. */
    MDKR_WINDOW_MODE,
    MDKR_INPUT_RUMBLE_ENABLED,
    MDKR_INPUT_RUMBLE_PROFILE,
    MDKR_INPUT_CONTROLLER_A,
    MDKR_INPUT_CONTROLLER_B,
    MDKR_INPUT_CONTROLLER_X,
    MDKR_INPUT_CONTROLLER_Y,
    MDKR_INPUT_CONTROLLER_START,
    MDKR_INPUT_CONTROLLER_LEFT_STICK,
    MDKR_INPUT_CONTROLLER_RIGHT_STICK,
    MDKR_INPUT_CONTROLLER_LEFT_SHOULDER,
    MDKR_INPUT_CONTROLLER_RIGHT_SHOULDER,
    MDKR_INPUT_CONTROLLER_DPAD_UP,
    MDKR_INPUT_CONTROLLER_DPAD_DOWN,
    MDKR_INPUT_CONTROLLER_DPAD_LEFT,
    MDKR_INPUT_CONTROLLER_DPAD_RIGHT,
    MDKR_INPUT_CONTROLLER_LEFT_TRIGGER,
    MDKR_INPUT_CONTROLLER_RIGHT_TRIGGER,
    MDKR_INPUT_CONTROLLER_RIGHT_STICK_UP,
    MDKR_INPUT_CONTROLLER_RIGHT_STICK_DOWN,
    MDKR_INPUT_CONTROLLER_RIGHT_STICK_LEFT,
    MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT,
    /* Presentation again, appended for the reason above. The input block is
     * bounded by the two macros below rather than by being last, so a
     * non-input key may follow it. */
    MDKR_VIDEO_CAMERA_OBSTRUCTION,
    MDKR_VIDEO_ALLOW_TEARING,
    /* Appended for the same reason as the two keys above. */
    MDKR_VIDEO_MENU_LANGUAGES,
    /*
     * Appended, not slotted beside Camera.Obstruction, for the reason stated
     * at the top of this block: the enum index IS the schema row and the menu
     * wheel order, so grouping a related key by inserting it would renumber
     * every key after it. Camera.Comfort reads as camera state to a player
     * because the settings panel groups by category, not by enum order.
     */
    MDKR_VIDEO_CAMERA_COMFORT,
    /*
     * Content packs. Appended for the reason above. These are not presentation
     * presets and are deliberately never pinned by Pure/Restored/Remastered:
     * a player who installed a pack did so on purpose, and a preset switch
     * must not silently uninstall it.
     */
    MDKR_CONTENT_PACKS_ENABLED,
    MDKR_CONTENT_PACK_DISABLED,
    /*
     * Enhancements. Each one also carries an AUTHORITY CLASS in
     * platform/enhancement_registry.c, and that class — not this enum — is what
     * decides whether the key is allowed to move authoritative state. Adding a
     * key here without a registry row leaves it ungated, which
     * check_enhancement_authority.py is written to catch.
     */
    MDKR_ENH_SPEEDOMETER,
    MDKR_ENH_DRAW_DISTANCE,
    MDKR_ENH_LOD_BIAS,
    MDKR_ENH_AI_DIFFICULTY,
    /* Shell behaviour, appended for the reason above. Not presentation, not
     * gameplay: whether the launcher looks for a newer release. */
    MDKR_APP_UPDATE_CHECK,
    MDKR_VIDEO_KEY_COUNT
} MdkrVideoKey;

#define MDKR_INPUT_FIRST_KEY MDKR_INPUT_RUMBLE_ENABLED
#define MDKR_INPUT_LAST_KEY  MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT

typedef enum MdkrVideoType {
    MDKR_VIDEO_TYPE_INT = 0,
    MDKR_VIDEO_TYPE_FLOAT,
    MDKR_VIDEO_TYPE_STRING
} MdkrVideoType;

/*
 * When a change reaches the running engine.
 *
 * LIVE and LEVEL are both "no relaunch"; they differ only in WHICH boundary
 * the engine is willing to swap the value at, and that difference is a
 * property of the receiving subsystem rather than of the setting's importance.
 *
 *   LIVE     the next host-frame boundary — after the previous present has
 *            retired and before the next tick's pacing decision. Everything
 *            the value feeds is re-derived there, as one ordered apply.
 *   LEVEL    the next level load. For a receiver whose only proven re-init
 *            seam is the one the level boundary already runs, and for which a
 *            mid-race swap would be a visible cut rather than a fade.
 *   RESTART  the value cannot be swapped under a running engine at all.
 *
 * Ordered LIVE < LEVEL < RESTART, so `scope >= MDKR_VIDEO_SCOPE_RESTART` and
 * `scope == MDKR_VIDEO_SCOPE_RESTART` both keep meaning "needs a relaunch".
 */
typedef enum MdkrVideoScope {
    MDKR_VIDEO_SCOPE_LIVE = 0,
    MDKR_VIDEO_SCOPE_LEVEL,
    MDKR_VIDEO_SCOPE_RESTART
} MdkrVideoScope;

/*
 * Which subsystem owns the deferred apply for a key.
 *
 * A LIVE/LEVEL key is one of two kinds. Most of them are published straight
 * into their receiver by mdkr_video_config_publish() and are complete the
 * moment that returns (Video.Widescreen writes display_config; Audio.* writes
 * the mixer). Those carry MDKR_VIDEO_APPLY_NONE.
 *
 * The rest have a receiver that CANNOT accept a write at an arbitrary point in
 * the frame: it owns latched state that has to be torn down and rebuilt as one
 * indivisible step, at a boundary where nothing it owns is in flight. Those
 * name the domain that owns that step. video_config_runtime.c stages the value
 * and the domain's registered callback performs the whole apply at the
 * boundary — never the setter's own call stack, which is an ImGui draw inside
 * somebody else's frame.
 *
 *   PRESENTATION  the host pacer, the presentation replay/subloop, and the
 *                 backend's present mode. Video.FrameLimit,
 *                 Video.MotionSmoothing, Video.AllowTearing.
 *   CAMERA        the camera obstruction sidecar. Camera.Obstruction.
 */
typedef enum MdkrVideoApplyDomain {
    MDKR_VIDEO_APPLY_NONE = 0,
    MDKR_VIDEO_APPLY_PRESENTATION,
    MDKR_VIDEO_APPLY_CAMERA,
    MDKR_VIDEO_APPLY_DOMAIN_COUNT
} MdkrVideoApplyDomain;

/*
 * Precedence rank. Monotonic: a set from a lower source never overwrites a
 * value already established by a higher one. This is what makes
 * "CLI beats env beats in-game beats launcher beats preset beats file" a
 * property a test can assert directly rather than an accident of the order the
 * layers happen to be applied in.
 */
typedef enum MdkrVideoSource {
    MDKR_VIDEO_SOURCE_DEFAULT = 0,
    MDKR_VIDEO_SOURCE_FILE,
    MDKR_VIDEO_SOURCE_PRESET,
    MDKR_VIDEO_SOURCE_LAUNCHER,
    MDKR_VIDEO_SOURCE_RUNTIME,
    MDKR_VIDEO_SOURCE_ENV,
    MDKR_VIDEO_SOURCE_CLI
} MdkrVideoSource;

typedef enum MdkrVideoMode {
    MDKR_VIDEO_MODE_PURE = 0,
    MDKR_VIDEO_MODE_RESTORED,
    MDKR_VIDEO_MODE_REMASTERED,
    MDKR_VIDEO_MODE_CUSTOM
} MdkrVideoMode;

/*
 * Presentation grouping for a settings UI. This lives in the schema rather than
 * in the UI so a new key cannot be added without deciding where a player would
 * look for it — the native app shell (platform/app/ui_settings.cpp) renders one
 * section per category and enumerates the schema, so it never carries its own
 * copy of the key list.
 */
typedef enum MdkrVideoCategory {
    MDKR_VIDEO_CAT_PRESENTATION = 0,  /* mode, framing, FOV                    */
    MDKR_VIDEO_CAT_FIDELITY,          /* resolution, filtering, texture detail */
    MDKR_VIDEO_CAT_PACING,            /* simulation cadence, frame limit       */
    MDKR_VIDEO_CAT_AUDIO,             /* master, music, and effects volume      */
    MDKR_VIDEO_CAT_INTERFACE,         /* native window and shell behavior       */
    MDKR_VIDEO_CAT_INPUT,             /* controller mapping and haptics          */
    MDKR_VIDEO_CAT_COUNT
} MdkrVideoCategory;

/* Human name for a category ("Presentation"). NULL when out of range. */
const char *mdkr_video_category_name(MdkrVideoCategory category);

typedef struct MdkrVideoSchema {
    char name[MDKR_VIDEO_NAME_MAX];   /* "Video.RenderScale" */
    char env[MDKR_VIDEO_NAME_MAX];    /* "MDKR_RENDER_SCALE"  */
    MdkrVideoType type;
    MdkrVideoScope scope;
    float min;
    float max;
    const char *label;                /* player-facing in-game options label */
    const char *help;
    MdkrVideoCategory category;       /* which settings section it belongs in */
} MdkrVideoSchema;

typedef struct MdkrVideoValue {
    float number;                          /* numeric value; ints stored exactly */
    char text[MDKR_VIDEO_STRING_MAX];      /* string value, else "" */
    MdkrVideoSource source;
} MdkrVideoValue;

typedef struct MdkrVideoConfig {
    MdkrVideoMode mode;
    MdkrVideoValue values[MDKR_VIDEO_KEY_COUNT];
} MdkrVideoConfig;

/* --- Pure API (no globals, no environment, no I/O) --- */

const MdkrVideoSchema *mdkr_video_schema(MdkrVideoKey key);

/*
 * The subsystem that owns `key`'s deferred apply, or MDKR_VIDEO_APPLY_NONE
 * when publish() completes the key by itself. Pure, so the mapping is a thing
 * tests/test_video_config.c asserts directly rather than a property of the
 * running engine.
 *
 * INVARIANT: a key with a domain is never SCOPE_RESTART, and a key that is
 * SCOPE_LEVEL always has one. A restart-scoped key has no boundary to be
 * applied at, and a level-scoped key exists precisely because some receiver
 * asked for the level boundary.
 */
MdkrVideoApplyDomain mdkr_video_key_apply_domain(MdkrVideoKey key);

int mdkr_video_key_is_audio(MdkrVideoKey key);
int mdkr_video_key_is_input(MdkrVideoKey key);
int mdkr_video_key_is_content(MdkrVideoKey key);
int mdkr_video_key_is_enhancement(MdkrVideoKey key);
int mdkr_video_key_is_player_comfort(MdkrVideoKey key);

/* Canonical string domains used by both validation and generated controls. */
const char *mdkr_window_mode_canonical(const char *value);
const char *mdkr_rumble_profile_canonical(const char *value);
const char *mdkr_controller_action_canonical(const char *value);

/* Returns MDKR_VIDEO_KEY_COUNT when `name` matches nothing. Case-insensitive. */
MdkrVideoKey mdkr_video_key_from_name(const char *name);

void mdkr_video_config_defaults(MdkrVideoConfig *config);
void mdkr_video_config_apply_preset(MdkrVideoConfig *config, MdkrVideoMode mode);
int mdkr_video_config_apply_preset_from(MdkrVideoConfig *config,
                                        MdkrVideoMode mode,
                                        MdkrVideoSource source);

/* Returns the matching MdkrVideoMode, or -1 when `name` matches nothing. */
int mdkr_video_mode_from_name(const char *name);

/*
 * Canonical spelling for a Video.WorldShadows value ("off", "soft", "full"), or
 * NULL when `value` is not one. The key inherits MDKR_WORLD_SHADOW, a seam that
 * predates it and that the existing A/B gates drive with "0"/"1"/"", so those
 * spellings resolve here too and every layer downstream sees one of three
 * words. Exposed so a UI can offer only what the validator takes.
 */
const char *mdkr_video_world_shadows_canonical(const char *value);

/*
 * Canonical spelling for a Camera.Obstruction value, or NULL when `value` is
 * not one. "observe" and "modern" are the two player-facing states; "legacy"
 * and "center-ray" are the diagnostic arms the MDKR_CAMERA_OBSTRUCTION seam
 * predates this key with, and they pass through unchanged so an A/B gate that
 * drives the environment keeps resolving through the same validator.
 */
const char *mdkr_video_camera_obstruction_canonical(const char *value);

/*
 * Canonical spelling for a Camera.Comfort value ("authored" or "reduced"), or
 * NULL when `value` is neither. "authored" is the default and means the camera
 * moves exactly as the game authored it; "reduced" is the accessibility
 * opt-in. Both spellings are new -- there is no pre-existing diagnostic seam to
 * stay compatible with -- so, like Gameplay.MenuLanguages, nothing else is
 * accepted and the empty string is rejected rather than folded into the
 * default.
 */
const char *mdkr_video_camera_comfort_canonical(const char *value);

/*
 * Canonical spelling for a Video.AllowTearing value ("off" or "on"), or NULL
 * when `value` is neither. "0"/"1" resolve too, so MDKR_ALLOW_TEARING reads the
 * same whether it arrives from the settings screen or a diagnostic run.
 */
const char *mdkr_video_allow_tearing_canonical(const char *value);

/*
 * Canonical spelling for a Video.FrameLimit value, written into `out` (which
 * must be at least MDKR_VIDEO_STRING_MAX). Returns 1 when `value` is one the
 * shared pacing-policy parser accepts and 0 otherwise, leaving `out` untouched.
 *
 * WHY THIS KEY NEEDS ONE AND DID NOT BEFORE. Its domain is half words and half
 * numbers, so it was the one enumerated setting stored exactly as typed:
 * `DISPLAY`, `Display` and `display` all resolved to the same policy but three
 * different strings reached the ini and the options screen, and a combo whose
 * table holds `display` simply showed no selection for the other two. That was
 * survivable while every word was a single token. `display-margin` is not a
 * single token, and a settings screen that cannot recognise its own written
 * value would show the player Custom for a choice they just made.
 *
 * Words fold to their lowercase spelling; a numeric cap folds to its decimal
 * value, so `060` and `60` are the same saved setting.
 */
int mdkr_video_frame_limit_canonical(const char *value, char *out, size_t cap);

/* --- Presentation pace: one choice over two existing keys ----------------- *
 *
 * WHAT IT IS. Nearly every player who wants the modern experience wants the
 * same PAIR of values -- Video.FrameLimit=display with
 * Video.MotionSmoothing=interpolate -- and had to find and combine two controls
 * in two different rows to get it. This is one name for that pair, and one name
 * for the authored pair it replaces.
 *
 * WHAT IT IS NOT: a fourth state. There is no Presentation.Pace key, nothing is
 * persisted under that name, and nothing latches it. It is a VIEW of the two
 * keys, derived on read and expanded on write, so the two keys remain the only
 * source of truth. Everything that already reads them -- the live-apply
 * boundary, the ini, --video-set, the report, every gate -- keeps working with
 * no knowledge that this exists, and a player who sets the two keys
 * individually to a matching pair simply sees the matching quick choice
 * selected.
 *
 * That is also why CUSTOM exists rather than being hidden or corrected. A
 * numeric cap, or display without smoothing, is a perfectly good configuration
 * that this control has no name for, and saying so is the honest answer. Custom
 * is a description, never a value: it can be READ back from the keys and never
 * written to them.
 */
typedef enum MdkrPresentationPace {
    /* The two keys spell a combination no quick choice names. */
    MDKR_PRESENTATION_PACE_CUSTOM = 0,
    /* FrameLimit=original, MotionSmoothing=off. */
    MDKR_PRESENTATION_PACE_ORIGINAL,
    /* FrameLimit=display, MotionSmoothing=interpolate. */
    MDKR_PRESENTATION_PACE_SMOOTH
} MdkrPresentationPace;

/* The quick choice `config`'s two pacing keys currently spell. */
MdkrPresentationPace mdkr_video_presentation_pace(const MdkrVideoConfig *config);

/* "original", "smooth" or "custom" -- the spelling the diagnostic seams and
 * the trace rows use. Never NULL. */
const char *mdkr_video_presentation_pace_name(MdkrPresentationPace pace);

/* Parse "original"/"smooth" case-insensitively. Returns -1 for anything else,
 * INCLUDING "custom": custom is a reading of the keys, not a thing to apply. */
int mdkr_video_presentation_pace_from_name(const char *name);

/*
 * The two key values `pace` expands to. Returns 1 and writes both pointers for
 * ORIGINAL and SMOOTH; returns 0 and writes nothing for CUSTOM.
 */
int mdkr_video_presentation_pace_values(MdkrPresentationPace pace,
                                        const char **frame_limit,
                                        const char **motion_smoothing);

/*
 * Canonical spelling for a Gameplay.MenuLanguages value ("authentic" or
 * "all"), or NULL when `value` is neither. "all" is the default: it offers
 * every language the running ROM's text and font assets carry, regardless of
 * which ones that cartridge's retail menu normally lists. "authentic" is the
 * opt-out, restoring each cartridge's own retail menu exactly as it shipped.
 */
const char *mdkr_video_menu_languages_canonical(const char *value);

/*
 * Applies `value` to `key` if `source` outranks whatever set it last.
 * Returns 1 when applied, 0 when rejected (lower precedence, unparseable, or
 * outside the schema's range). Equal rank is allowed so a live change can
 * re-apply from the same layer.
 */
int mdkr_video_config_set(MdkrVideoConfig *config,
                          MdkrVideoKey key,
                          const char *value,
                          MdkrVideoSource source);

/*
 * Pure resolution: applies file entries, then any --pure/--restored/
 * --remastered preset, browser-launcher seeds, MDKR_* environment values,
 * direct display flags, and --video-set overrides, honoring the precedence
 * ranking throughout. Runtime/in-game values are applied transactionally by
 * the impure API below.
 *
 * `env_lookup` may be NULL, in which case the environment layer is skipped.
 * That is what makes this function testable without touching the real
 * environment.
 */
typedef const char *(*MdkrVideoEnvLookup)(const char *name);

void mdkr_video_config_resolve(MdkrVideoConfig *config,
                               const ConfigIniEntry *file_entries,
                               int file_count,
                               MdkrVideoEnvLookup env_lookup,
                               int argc,
                               char *const *argv);

/*
 * A Pure session never rewrites presentation/gameplay values, so debugging
 * against the oracle reference cannot destroy a user's Remastered setup.
 * Runtime audio, window, controller, and haptic controls are explicit
 * comfort-only exceptions.
 */
int mdkr_video_config_readonly_for(const MdkrVideoConfig *config);

/* --- Runtime API (reads the real ini, environment and argv) --- */

void mdkr_video_config_init(int argc, char *const *argv);

/*
 * Complete the launcher -> engine boundary exactly once in an app process.
 * The launcher initializes this module early so its settings UI can stage
 * restart-scoped changes. Immediately before engine entry, this transaction
 * reloads the saved layer and resolves the engine argv into both the active
 * and desired configs. The engine's later init remains an idempotent no-op.
 *
 * Returns 1 for the one valid handoff, or 0 if initialization has not happened
 * or a handoff was already completed.
 */
int mdkr_video_config_handoff_to_engine(int argc, char *const *argv);
const MdkrVideoConfig *mdkr_video_config_current(void);
const MdkrVideoConfig *mdkr_video_config_desired(void);
int mdkr_video_config_is_readonly(void);

typedef enum MdkrVideoRuntimeResult {
    /* The VALUE was rejected: unparseable, out of the schema's range, or not a
     * member of the key's canonical domain. Strictly "what you asked for is not
     * a legal setting" -- never "there was nowhere to apply it". */
    MDKR_VIDEO_RUNTIME_INVALID = 0,
    MDKR_VIDEO_RUNTIME_LIVE,
    MDKR_VIDEO_RUNTIME_RESTART,
    MDKR_VIDEO_RUNTIME_LOCKED,
    MDKR_VIDEO_RUNTIME_SAVE_FAILED,
    /* The replacement is visible in this process, but the parent-directory
     * durability barrier failed. Callers must not claim it will survive power
     * loss/restart without another successful save. */
    MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED,
    /* App-shell extensions: a safe-boundary window request, a rejected OS
     * transition, and the rare case where persistence failed and the OS also
     * rejected the attempted rollback. The core setter never returns these. */
    MDKR_VIDEO_RUNTIME_PENDING,
    MDKR_VIDEO_RUNTIME_APPLY_FAILED,
    MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED,
    /*
     * Appended, not inserted: these are the three causes MDKR_VIDEO_RUNTIME_INVALID
     * used to carry at once, which made the settings panel tell a player "that
     * value is not valid" when the value was perfectly good and only the window
     * was missing, or when their newer selection had simply replaced an older
     * one. Splitting them is what lets the message match the cause.
     *
     * UNAVAILABLE: there is no window / presentation surface to apply this to.
     *              The value was never examined; retrying later can succeed.
     * SUPERSEDED:  a window-mode request was already queued for the next safe
     *              frame boundary and this one replaced it. The NEWEST intent is
     *              what will be applied -- this is a success-shaped outcome for
     *              the request that was dropped, not a rejection of this one.
     * The core setter never returns these; they come from the app shell.
     */
    MDKR_VIDEO_RUNTIME_UNAVAILABLE,
    MDKR_VIDEO_RUNTIME_SUPERSEDED
} MdkrVideoRuntimeResult;

/* A setting may be applied and visibly replaced while its directory durability
 * remains unconfirmed. Keep success classification centralized so UI, original
 * menus, and window rollback code cannot silently drift when a new outcome is
 * added. */
static inline int mdkr_video_runtime_result_applied(
    MdkrVideoRuntimeResult result) {
    return result == MDKR_VIDEO_RUNTIME_LIVE ||
           result == MDKR_VIDEO_RUNTIME_RESTART ||
           result == MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED;
}

#ifdef MDKR_VIDEO_RUNTIME_TESTING
/* Test-build-only deterministic seams. They are absent from production
 * objects, and let the ROM-free runtime contract exercise post-replace and
 * stale-snapshot failures without environment-variable backdoors. */
void mdkr_video_test_force_directory_sync_failure(int enabled);
void mdkr_video_test_set_launcher_persist_hook(void (*hook)(void));
#endif

typedef struct MdkrVideoRuntimeChange {
    MdkrVideoKey key;
    const char *value;
} MdkrVideoRuntimeChange;

/*
 * Applies one player-authored setting at the menu layer. Environment and CLI
 * values remain authoritative and return LOCKED. LIVE changes are published
 * immediately; RESTART changes are saved coherently for the next launch.
 */
MdkrVideoRuntimeResult mdkr_video_config_runtime_set(MdkrVideoKey key,
                                                     const char *value);
MdkrVideoRuntimeResult mdkr_video_config_runtime_set_many(
    const MdkrVideoRuntimeChange *changes,
    int change_count);

/*
 * Apply a presentation-pace quick choice: exactly the two-key set_many the
 * expansion above describes, in ONE transaction.
 *
 * One transaction is the whole point. Two sequential setters would take the
 * config lock twice, write the file twice, and -- under a running engine --
 * stage two separate boundary applies, so a player switching to Smooth could
 * be shown display-with-smoothing-off for one frame boundary. Passing both
 * changes together makes the pair atomic on disk and gives the presentation
 * domain ONE apply carrying both, which is what the live gate asserts.
 *
 * Returns MDKR_VIDEO_RUNTIME_INVALID for CUSTOM, which has no expansion, and
 * otherwise whatever the underlying transaction returns -- including LOCKED
 * when either key is pinned above RUNTIME rank, in which case NEITHER is
 * written.
 */
MdkrVideoRuntimeResult mdkr_video_config_runtime_set_presentation_pace(
    MdkrPresentationPace pace);

/* Persist DKR's original 0..256 music/effects sliders into the shared 0..100
 * shell settings when the player leaves the original Audio Options screen. */
MdkrVideoRuntimeResult mdkr_audio_config_runtime_set_game_levels(
    unsigned music_level, unsigned effects_level);
/* Audible slider preview without a filesystem transaction. The settings UI
 * saves once on release and cancels the preview if navigation interrupts it. */
int mdkr_audio_config_runtime_preview(MdkrVideoKey key, int percent);
void mdkr_audio_config_runtime_cancel_preview(void);
int mdkr_video_config_restart_pending(void);
int mdkr_video_config_runtime_locked(MdkrVideoKey key);

/*
 * Writes the resolved values into renderer/display state and publishes player
 * audio levels. Safe to call repeatedly; LIVE settings take effect next frame.
 *
 * Keys with an apply domain are the exception, and only once that domain has
 * registered a callback below: publish() then leaves them to the boundary
 * rather than writing a latched receiver from whatever stack called it.
 */
void mdkr_video_config_publish(void);

/* --- Deferred apply -------------------------------------------------------
 *
 * WHY THIS EXISTS. A settings edit arrives from an ImGui draw inside the app
 * shell's overlay, which runs inside the engine's own frame — mid-present for
 * the WebGPU backend, and possibly inside a presentation subloop that is
 * already replaying a retained display list. Several receivers cannot be
 * rewritten there at any price: the present pacer has a latched policy and a
 * deadline grid derived from it, the presentation replay holds walk-entry
 * state (segment table included) that only gfx_dkr_replay_invalidate() clears,
 * and the camera obstruction sidecar holds a validated pose it will otherwise
 * keep reusing. Writing half of that under a running walk is how a stale
 * segment base becomes a wild pointer.
 *
 * THE SHAPE. The setter validates, persists, and updates the config
 * immediately — the player's choice is never in doubt and never lost. What it
 * defers is the hardware/engine-state consequence: it marks the owning domain
 * pending, and the engine calls mdkr_video_config_apply_pending() at the one
 * boundary that domain declared safe. The domain's callback then performs the
 * WHOLE apply as one ordered step. There is no partial state in between:
 * before the boundary everything runs on the old value, after it everything
 * runs on the new one.
 *
 * WHEN NOTHING IS REGISTERED. A domain with no callback is not deferred at
 * all — publish() writes it inline exactly as it always did. That is what
 * keeps the launcher-before-Play phase, --video-set, and every ROM-free test
 * working unchanged: they have no engine to defer to, and a value staged for a
 * boundary nobody services would silently never apply.
 */

/*
 * Register `apply` as `domain`'s boundary applier. Called once by the owning
 * subsystem during engine startup, before any settings UI can exist. Passing
 * NULL unregisters, which is what a shutdown path wants so a late edit cannot
 * call into a torn-down subsystem.
 */
void mdkr_video_config_register_apply(MdkrVideoApplyDomain domain,
                                      void (*apply)(void));

/*
 * Run every domain pending at `boundary`, in domain order, and emit one
 * [SETTINGS-APPLY] row per key naming old and new. Returns the number of
 * domains applied, so a gate can assert that a boundary did work.
 *
 * `boundary` is MDKR_VIDEO_SCOPE_LIVE at the host-frame boundary and
 * MDKR_VIDEO_SCOPE_LEVEL at a level load, and the match is EXACT: a boundary
 * services only the domains that named it. A level load does not opportunis-
 * tically flush a LIVE domain, and — the direction that matters — a host-frame
 * boundary never applies a LEVEL domain early, which would be precisely the
 * mid-race camera cut that made Camera.Obstruction LEVEL in the first place.
 */
int mdkr_video_config_apply_pending(MdkrVideoScope boundary);

/* True when any domain is waiting for `boundary`. Cheap; the engine calls the
 * apply entry point unconditionally and this is what makes that free. */
int mdkr_video_config_apply_is_pending(MdkrVideoScope boundary);

/*
 * Push the resolved pacing keys (Video.FrameLimit / Video.MotionSmoothing /
 * Video.AllowTearing) into present_sched. publish() calls this itself while the
 * presentation domain has no registered applier; once it does, that applier
 * calls it — after invalidating the replay history — and publish() does not.
 * Exposed so there is one implementation rather than two that must agree.
 */
void mdkr_video_config_push_presentation(void);

/* --video-list: prints every setting, its value, and which layer set it. */
void mdkr_video_config_report(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_VIDEO_CONFIG_H */
