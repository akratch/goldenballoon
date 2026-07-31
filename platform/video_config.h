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
 *   restored    the original art direction at modern fidelity. Changes
 *               sharpness and stability, never look.
 *   remastered  the full art-directed remaster. Default.
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
    MDKR_VIDEO_KEY_COUNT
} MdkrVideoKey;

typedef enum MdkrVideoType {
    MDKR_VIDEO_TYPE_INT = 0,
    MDKR_VIDEO_TYPE_FLOAT,
    MDKR_VIDEO_TYPE_STRING
} MdkrVideoType;

/* Whether a change takes effect immediately or needs a relaunch. */
typedef enum MdkrVideoScope {
    MDKR_VIDEO_SCOPE_LIVE = 0,
    MDKR_VIDEO_SCOPE_RESTART
} MdkrVideoScope;

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
 * A Pure session never rewrites the ini, so debugging against the oracle
 * reference cannot destroy a user's Remastered setup.
 */
int mdkr_video_config_readonly_for(const MdkrVideoConfig *config);

/* --- Runtime API (reads the real ini, environment and argv) --- */

void mdkr_video_config_init(int argc, char *const *argv);
const MdkrVideoConfig *mdkr_video_config_current(void);
const MdkrVideoConfig *mdkr_video_config_desired(void);
int mdkr_video_config_is_readonly(void);

typedef enum MdkrVideoRuntimeResult {
    MDKR_VIDEO_RUNTIME_INVALID = 0,
    MDKR_VIDEO_RUNTIME_LIVE,
    MDKR_VIDEO_RUNTIME_RESTART,
    MDKR_VIDEO_RUNTIME_LOCKED,
    MDKR_VIDEO_RUNTIME_SAVE_FAILED
} MdkrVideoRuntimeResult;

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
int mdkr_video_config_restart_pending(void);
int mdkr_video_config_runtime_locked(MdkrVideoKey key);

/*
 * Writes the resolved values into the g_pc* globals the fast3d backends read
 * and into display_config's widescreen/aspect state. Safe to call repeatedly;
 * LIVE-scope settings take effect on the next frame.
 */
void mdkr_video_config_publish(void);

/* --video-list: prints every setting, its value, and which layer set it. */
void mdkr_video_config_report(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_VIDEO_CONFIG_H */
