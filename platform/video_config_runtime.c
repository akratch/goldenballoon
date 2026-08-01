/**
 * video_config_runtime.c — the impure half of the video config module.
 *
 * Everything that touches the filesystem, the environment, or the rest of the
 * program lives here. video_config.c stays free of all three so
 * tests/test_video_config.c can link the pure core on its own — without the
 * g_pc* render globals, without display_config, without a GPU. Merging these
 * two files back together would quietly break that seam.
 */
#include "video_config.h"

#include "display_config.h"
#include "fast3d/gfx_mipgen.h"
#include "fast3d/gfx_shadow_cascade.h"
#include "fast3d/gfx_uniforms.h"
#include "present_sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#else
#include <emscripten/emscripten.h>
#endif

/* --------------------------------------------------------------------------
 *  Runtime layer — the only part that touches the filesystem or environment.
 * ------------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
#define MDKR_VIDEO_INI_PATH   "/save/mdkr64.ini"
#else
#define MDKR_VIDEO_INI_PATH   "mdkr64.ini"
#endif
#define MDKR_VIDEO_INI_TMP    MDKR_VIDEO_INI_PATH ".tmp"
#define MDKR_VIDEO_INI_MAX    128
#define MDKR_VIDEO_INI_TEXT_MAX 32768

static MdkrVideoConfig s_video;
static MdkrVideoConfig s_desired_video;
static ConfigIniEntry s_file_entries[MDKR_VIDEO_INI_MAX];
static int s_file_entry_count;
static int s_video_initialized;
static int mdkr_video_write_config(const MdkrVideoConfig *config);

#ifdef __EMSCRIPTEN__
EM_JS(void, mdkr_video_schedule_persist, (), {
    const persist = (typeof Module.__mdkrPersist === "function")
        ? Module.__mdkrPersist({reason: "video-config", urgent: true})
        : new Promise((resolve, reject) => {
            FS.syncfs(false, error => error ? reject(error) : resolve());
        });
    Promise.resolve(persist).catch(error => {
        if (typeof Module.__mdkrPersistFailed === "function") {
            Module.__mdkrPersistFailed(String(
                error && error.message ? error.message : error));
        }
    });
});
#endif

static const char *mdkr_video_getenv(const char *name) {
    return getenv(name);
}

/*
 * Launcher persistence happens during config initialization, before main_pc.c
 * performs its normal argument-validation pass. Validate the complete launcher
 * layer first so a malformed internal argument cannot partially rewrite a
 * previously valid file on the way to main()'s exit-2 error.
 */
static int mdkr_video_launcher_args_valid(int argc, char *const *argv) {
    MdkrVideoConfig validation;

    mdkr_video_config_defaults(&validation);
    for (int i = 1; i < argc && argv != NULL; i++) {
        if (!strcmp(argv[i], "--video-launch-mode")) {
            int mode;
            if (i + 1 >= argc) {
                return 0;
            }
            mode = mdkr_video_mode_from_name(argv[++i]);
            if (mode < MDKR_VIDEO_MODE_PURE ||
                mode > MDKR_VIDEO_MODE_REMASTERED ||
                !mdkr_video_config_apply_preset_from(
                    &validation, (MdkrVideoMode) mode,
                    MDKR_VIDEO_SOURCE_LAUNCHER)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--video-launch-set")) {
            const char *pair;
            const char *eq;
            char name[MDKR_VIDEO_NAME_MAX];
            size_t length;
            MdkrVideoKey key;

            if (i + 1 >= argc) {
                return 0;
            }
            pair = argv[++i];
            eq = strchr(pair, '=');
            length = eq != NULL ? (size_t) (eq - pair) : 0;
            if (length == 0 || length >= sizeof(name)) {
                return 0;
            }
            memcpy(name, pair, length);
            name[length] = '\0';
            key = mdkr_video_key_from_name(name);
            if (key == MDKR_VIDEO_KEY_COUNT ||
                !mdkr_video_config_set(&validation, key, eq + 1,
                                       MDKR_VIDEO_SOURCE_LAUNCHER)) {
                return 0;
            }
        }
    }
    return 1;
}

void mdkr_video_config_init(int argc, char *const *argv) {
    int count = 0;
    int launcher_persist = 0;
    char text[MDKR_VIDEO_INI_TEXT_MAX];
    FILE *f;

    if (s_video_initialized) {
        return;
    }
    s_video_initialized = 1;
    mdkr_video_config_defaults(&s_video);

    f = fopen(MDKR_VIDEO_INI_PATH, "rb");
    if (f != NULL) {
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        text[n] = '\0';
        fclose(f);
        if (!config_ini_parse(text, s_file_entries, MDKR_VIDEO_INI_MAX, &count)) {
            fprintf(stderr,
                    "[video] %s has more than %d entries; extra keys ignored\n",
                    MDKR_VIDEO_INI_PATH, MDKR_VIDEO_INI_MAX);
        }
    }
    s_file_entry_count = count;

    mdkr_video_config_resolve(&s_video, s_file_entries, count,
                              mdkr_video_getenv, argc, argv);
    s_desired_video = s_video;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--video-launch-persist")) {
            launcher_persist = 1;
            break;
        }
    }
    if (launcher_persist) {
        if (!mdkr_video_launcher_args_valid(argc, argv)) {
            fprintf(stderr,
                    "[video] invalid launcher settings; existing config left unchanged\n");
        } else if (!mdkr_video_write_config(&s_desired_video)) {
            fprintf(stderr,
                    "[video] launcher settings are active but could not be persisted\n");
        }
    }
}

const MdkrVideoConfig *mdkr_video_config_current(void) {
    return &s_video;
}

const MdkrVideoConfig *mdkr_video_config_desired(void) {
    return &s_desired_video;
}

int mdkr_video_config_is_readonly(void) {
    return mdkr_video_config_readonly_for(&s_video);
}

/* --------------------------------------------------------------------------
 *  Publication — the resolved config reaches the rest of the program here.
 * ------------------------------------------------------------------------ */

/*
 * Diagnostic float override for one of the two hardcoded shadow constants.
 *
 * Both were tuned by measurement (bias against DKR's terrain facet size, umbra
 * against its baked vertex occlusion) and both are the kind of constant whose
 * tuning has to be re-measurable later — the umbra already moved once, from
 * 0.48 to 0.62, because a playthrough said the shadows were too heavy. A raw
 * env seam keeps that A/B one command away without putting a second, redundant
 * knob next to Video.WorldShadows in the options screen. Out-of-range or
 * unparseable text leaves the production value untouched.
 */
static float mdkr_video_float_override(const char *name, float fallback,
                                       float low, float high) {
    const char *raw = mdkr_video_getenv(name);
    char *end;
    double parsed;

    if (raw == NULL || raw[0] == '\0') {
        return fallback;
    }
    parsed = strtod(raw, &end);
    if (end == raw || *end != '\0' || !(parsed >= low) || !(parsed <= high)) {
        fprintf(stderr, "[video] ignoring invalid %s=%s\n", name, raw);
        return fallback;
    }
    return (float) parsed;
}

void mdkr_video_config_publish(void) {
    const MdkrVideoConfig *c = &s_video;
    const char *world_shadow =
        c->values[MDKR_VIDEO_WORLD_SHADOWS].text;
    const char *world_finish_off =
        mdkr_video_getenv("MDKR_TEST_WORLD_FINISH_OFF");
    int world_finish;

    g_pcRemasterFX        = (int) c->values[MDKR_VIDEO_REMASTER_FX].number;
    world_finish =
        g_pcRemasterFX && world_finish_off == NULL;
    g_pcGradePresets      = world_finish;
    g_pcTonemap           = world_finish;
    /* RL-5 is part of the Remastered art pass, not an independent fidelity
     * knob. Pure/Restored therefore cannot accidentally enable its shader IDs. */
    g_pcPerPixelLight     = g_pcRemasterFX;
    /*
     * WD-6 production policy: real maps are part of Remastered, so the world
     * shadow pass still cannot outlive RemasterFX. Within Remastered the player
     * now chooses (Video.WorldShadows); MDKR_WORLD_SHADOW is that key's env
     * name, so the historical "0"/"1" diagnostic spellings still resolve here,
     * canonicalised by the schema.
     */
    g_pcSunShadow =
        g_pcRemasterFX && strcmp(world_shadow, "off") != 0;
    /*
     * Not a resolution request — the cascade planner owns resolution and varies
     * it live with the split-screen view count (2048 at 1P, 1024 at 2-4P). This
     * publishes the 1P budget so a consumer reading the global agrees with the
     * plan instead of contradicting it. It was a hardcoded 2048 that nothing in
     * either backend read.
     */
    g_pcSunShadowRes =
        (int) gfx_shadow_budget_for_views(1).resolution;
    /*
     * WORLD-unit comparison bias. Both receivers divide this by the planned
     * light z-span at upload, so acne/peter-panning behavior no longer
     * depends on how much caster depth a stage happens to span (the old
     * 0.0015 NDC constant silently grew several-fold when planning learned
     * to cover the whole static cache). ~8 world units clears DKR's terrain
     * facets at the clamped 29–55° sun without visibly detaching kart
     * contact shadows (kart bodies are ~40–60 units).
     */
    g_pcSunShadowBias =
        mdkr_video_float_override("MDKR_SHADOW_BIAS", 8.0f, 0.0f, 4000.0f);
    /*
     * Shadowed pixels are multiplied by the umbra factor after RL-5 lighting.
     * DKR's baked vertex colour already carries authored occlusion, so a deep
     * umbra double-darkens and reads as heavy splotches on bright arcade art.
     * 0.62 (38% attenuation) keeps shadows legible without crushing; the
     * original 0.48 measured as the dominant "shadows degrade the UX" factor
     * in the 2026-07-29 playthrough review.
     *
     * "soft" is the same measurement taken one step further for players who
     * still read it as heavy: 0.78 (22% attenuation) on the same maps and the
     * same cascades. It is a strength choice, not a quality tier — nothing
     * about the shadow itself gets coarser, so no value of this key can make
     * the image noisier than the default does.
     */
    g_pcSunShadowUmbra = mdkr_video_float_override(
        "MDKR_SHADOW_UMBRA",
        strcmp(world_shadow, "soft") == 0 ? 0.78f : 0.62f,
        0.0f, 1.0f);
    g_pcRenderScale       =       c->values[MDKR_VIDEO_RENDER_SCALE].number;
    g_pcMsaaSamples       = (int) c->values[MDKR_VIDEO_MSAA].number;
    g_pcTextureAnisotropy = (int) c->values[MDKR_VIDEO_ANISOTROPY].number;
    g_pcMipmaps           = (int) c->values[MDKR_VIDEO_MIPMAPS].number;

    /*
     * Widescreen and aspect are display_config's state, not ours. We are the
     * single resolution point; it stays the single owner. Its setters take a
     * string and lazily self-initialize, so ordering here is safe.
     *
     * main() publishes BEFORE parsing --aspect/--widescreen/--legacy-stretch,
     * so those flags land on top of the mode preset. That is deliberate: they
     * are CLI-rank and must beat a preset.
     */
    mdkr_display_set_widescreen(
        c->values[MDKR_VIDEO_WIDESCREEN].number != 0.0f ? "1" : "0");
    if (c->values[MDKR_VIDEO_ASPECT].text[0] != '\0') {
        mdkr_display_set_aspect(c->values[MDKR_VIDEO_ASPECT].text);
    }
    if (c->values[MDKR_VIDEO_GAMEPLAY_FOV].text[0] != '\0') {
        mdkr_display_set_gameplay_fov(
            c->values[MDKR_VIDEO_GAMEPLAY_FOV].text);
    }

    /*
     * Video.FrameLimit / Video.MotionSmoothing (spec §11) push into
     * present_sched's cached MDKR_PRESENT_RATE/MDKR_PRESENT_SMOOTHING state --
     * see present_sched.h's "Wave C" note for why a plain setenv-and-forget
     * would not be LIVE. The push happens ONLY when this key resolved from
     * something other than the schema default: pushing unconditionally on
     * every publish() (including the very first one, at boot, before any game
     * frame runs) would overwrite the present scheduler's direct getenv result
     * with this key's DEFAULT "original". The environment is normally resolved
     * by the schema too, but keeping the default-source guard preserves the
     * diagnostic seam and makes "unset" semantically distinct from an explicit
     * `original`. tests/check_presentation_matrix.py's arm B remains the
     * regression for that precedence boundary.
     */
    if (c->values[MDKR_VIDEO_FRAME_LIMIT].source != MDKR_VIDEO_SOURCE_DEFAULT) {
        mdkr_present_set_frame_limit(c->values[MDKR_VIDEO_FRAME_LIMIT].text);
    }
    if (c->values[MDKR_VIDEO_MOTION_SMOOTHING].source !=
        MDKR_VIDEO_SOURCE_DEFAULT) {
        mdkr_present_set_motion_smoothing(
            c->values[MDKR_VIDEO_MOTION_SMOOTHING].text);
    }
}

static int mdkr_video_values_equal(const MdkrVideoValue *a,
                                   const MdkrVideoValue *b,
                                   MdkrVideoType type) {
    if (type == MDKR_VIDEO_TYPE_STRING) {
        return strcmp(a->text, b->text) == 0;
    }
    return a->number == b->number;
}

static const ConfigIniEntry *mdkr_video_last_file_entry(MdkrVideoKey key) {
    const ConfigIniEntry *found = NULL;
    for (int i = 0; i < s_file_entry_count; i++) {
        if (mdkr_video_key_from_name(s_file_entries[i].key) == key) {
            found = &s_file_entries[i];
        }
    }
    return found;
}

static int mdkr_video_append_entry(ConfigIniEntry *entries, int *count,
                                   const char *key, const char *value) {
    if (entries == NULL || count == NULL || key == NULL || value == NULL ||
        *count < 0 || *count >= MDKR_VIDEO_INI_MAX ||
        strlen(key) >= sizeof(entries[*count].key) ||
        strlen(value) >= sizeof(entries[*count].value)) {
        return 0;
    }
    snprintf(entries[*count].key, sizeof(entries[*count].key), "%s", key);
    snprintf(entries[*count].value, sizeof(entries[*count].value), "%s", value);
    (*count)++;
    return 1;
}

static int mdkr_video_build_persisted_entries(
    const MdkrVideoConfig *config,
    ConfigIniEntry entries[MDKR_VIDEO_INI_MAX],
    int *out_count) {
    int count = 0;

    if (config == NULL || entries == NULL || out_count == NULL) {
        return 0;
    }

    /* Unknown settings are owned by their producer and round-trip unchanged.
     * Known settings are emitted once below, eliminating ambiguous duplicates. */
    for (int i = 0; i < s_file_entry_count; i++) {
        if (mdkr_video_key_from_name(s_file_entries[i].key) ==
            MDKR_VIDEO_KEY_COUNT) {
            if (!mdkr_video_append_entry(entries, &count,
                                         s_file_entries[i].key,
                                         s_file_entries[i].value)) {
                return 0;
            }
        }
    }

    /* Mode is deliberately first: readers can establish its preset and then
     * layer the explicit values below independent of the original file order. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
            const MdkrVideoSchema *schema = mdkr_video_schema((MdkrVideoKey) i);
            const MdkrVideoValue *value = &config->values[i];
            const ConfigIniEntry *prior;
            char number[64];
            const char *serialized;

            if ((pass == 0) != (i == MDKR_VIDEO_MODE)) {
                continue;
            }
            if (value->source > MDKR_VIDEO_SOURCE_RUNTIME) {
                /* Never bake a temporary environment/CLI override into disk. */
                prior = mdkr_video_last_file_entry((MdkrVideoKey) i);
                if (prior == NULL) {
                    continue;
                }
                serialized = prior->value;
            } else if (schema->type == MDKR_VIDEO_TYPE_STRING) {
                serialized = value->text;
            } else if (schema->type == MDKR_VIDEO_TYPE_INT) {
                snprintf(number, sizeof(number), "%d", (int) value->number);
                serialized = number;
            } else {
                snprintf(number, sizeof(number), "%.9g", (double) value->number);
                serialized = number;
            }
            if (!mdkr_video_append_entry(entries, &count, schema->name,
                                         serialized)) {
                return 0;
            }
        }
    }
    *out_count = count;
    return 1;
}

static int mdkr_video_write_config(const MdkrVideoConfig *config) {
    ConfigIniEntry entries[MDKR_VIDEO_INI_MAX];
    char text[MDKR_VIDEO_INI_TEXT_MAX];
    int count = 0;
    FILE *f;

    if (!mdkr_video_build_persisted_entries(config, entries, &count) ||
        !config_ini_serialize(entries, count, text, sizeof(text))) {
        fprintf(stderr, "[video] config is too large to save safely\n");
        return 0;
    }
#ifdef __EMSCRIPTEN__
    if (mkdir("/save", 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "[video] could not create /save: %s\n", strerror(errno));
        return 0;
    }
#endif
    f = fopen(MDKR_VIDEO_INI_TMP, "wb");
    if (f == NULL) {
        fprintf(stderr, "[video] could not open %s: %s\n",
                MDKR_VIDEO_INI_TMP, strerror(errno));
        return 0;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text) ||
        fflush(f) != 0) {
        fprintf(stderr, "[video] could not write %s: %s\n",
                MDKR_VIDEO_INI_TMP, strerror(errno));
        fclose(f);
        remove(MDKR_VIDEO_INI_TMP);
        return 0;
    }
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0) {
#else
    if (fsync(fileno(f)) != 0) {
#endif
        fprintf(stderr, "[video] could not synchronize %s: %s\n",
                MDKR_VIDEO_INI_TMP, strerror(errno));
        fclose(f);
        remove(MDKR_VIDEO_INI_TMP);
        return 0;
    }
#endif
    if (fclose(f) != 0) {
        fprintf(stderr, "[video] could not close %s: %s\n",
                MDKR_VIDEO_INI_TMP, strerror(errno));
        remove(MDKR_VIDEO_INI_TMP);
        return 0;
    }
#ifdef _WIN32
    if (!MoveFileExA(MDKR_VIDEO_INI_TMP, MDKR_VIDEO_INI_PATH,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "[video] could not replace %s (Windows error %lu)\n",
                MDKR_VIDEO_INI_PATH, (unsigned long) GetLastError());
        remove(MDKR_VIDEO_INI_TMP);
        return 0;
    }
#else
    if (rename(MDKR_VIDEO_INI_TMP, MDKR_VIDEO_INI_PATH) != 0) {
        fprintf(stderr, "[video] could not replace %s: %s\n",
                MDKR_VIDEO_INI_PATH, strerror(errno));
        remove(MDKR_VIDEO_INI_TMP);
        return 0;
    }
#endif
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    {
        int directory = open(".", O_RDONLY);
        if (directory >= 0) {
            if (fsync(directory) != 0) {
                fprintf(stderr,
                        "[video] warning: could not synchronize config directory: %s\n",
                        strerror(errno));
            }
            close(directory);
        }
    }
#elif defined(__EMSCRIPTEN__)
    mdkr_video_schedule_persist();
#endif

    memcpy(s_file_entries, entries, (size_t) count * sizeof(entries[0]));
    s_file_entry_count = count;
    return 1;
}

static int mdkr_video_mode_locked(void) {
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        if (s_desired_video.values[i].source > MDKR_VIDEO_SOURCE_RUNTIME) {
            return 1;
        }
    }
    return 0;
}

int mdkr_video_config_runtime_locked(MdkrVideoKey key) {
    if ((int) key < 0 || key >= MDKR_VIDEO_KEY_COUNT ||
        mdkr_video_config_is_readonly()) {
        return 1;
    }
    if (key == MDKR_VIDEO_MODE) {
        return mdkr_video_mode_locked();
    }
    return s_desired_video.values[key].source > MDKR_VIDEO_SOURCE_RUNTIME;
}

MdkrVideoRuntimeResult mdkr_video_config_runtime_set_many(
    const MdkrVideoRuntimeChange *changes,
    int change_count) {
    MdkrVideoConfig candidate;
    int includes_mode = 0;
    int requires_restart = 0;

    if (!s_video_initialized || changes == NULL ||
        change_count <= 0 || change_count > MDKR_VIDEO_KEY_COUNT) {
        return MDKR_VIDEO_RUNTIME_INVALID;
    }
    for (int i = 0; i < change_count; i++) {
        const MdkrVideoSchema *schema = mdkr_video_schema(changes[i].key);
        if (schema == NULL || changes[i].value == NULL) {
            return MDKR_VIDEO_RUNTIME_INVALID;
        }
        if (mdkr_video_config_runtime_locked(changes[i].key)) {
            return MDKR_VIDEO_RUNTIME_LOCKED;
        }
        includes_mode |= changes[i].key == MDKR_VIDEO_MODE;
        requires_restart |= schema->scope == MDKR_VIDEO_SCOPE_RESTART;
    }

    candidate = s_desired_video;
    for (int i = 0; i < change_count; i++) {
        if (!mdkr_video_config_set(&candidate, changes[i].key, changes[i].value,
                                   MDKR_VIDEO_SOURCE_RUNTIME)) {
            return MDKR_VIDEO_RUNTIME_INVALID;
        }
    }
    if (!includes_mode) {
        (void) mdkr_video_config_set(&candidate, MDKR_VIDEO_MODE, "custom",
                                     MDKR_VIDEO_SOURCE_RUNTIME);
    }
    if (!mdkr_video_write_config(&candidate)) {
        return MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    }

    s_desired_video = candidate;
    if (!includes_mode) {
        for (int i = 0; i < change_count; i++) {
            const MdkrVideoSchema *schema = mdkr_video_schema(changes[i].key);
            if (schema->scope == MDKR_VIDEO_SCOPE_LIVE) {
                (void) mdkr_video_config_set(&s_video, changes[i].key,
                                             changes[i].value,
                                             MDKR_VIDEO_SOURCE_RUNTIME);
            }
        }
        (void) mdkr_video_config_set(&s_video, MDKR_VIDEO_MODE, "custom",
                                     MDKR_VIDEO_SOURCE_RUNTIME);
        mdkr_video_config_publish();
    }
    return requires_restart ? MDKR_VIDEO_RUNTIME_RESTART
                            : MDKR_VIDEO_RUNTIME_LIVE;
}

MdkrVideoRuntimeResult mdkr_video_config_runtime_set(MdkrVideoKey key,
                                                     const char *value) {
    const MdkrVideoRuntimeChange change = { key, value };
    return mdkr_video_config_runtime_set_many(&change, 1);
}

int mdkr_video_config_restart_pending(void) {
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const MdkrVideoSchema *schema = mdkr_video_schema((MdkrVideoKey) i);
        if (i != MDKR_VIDEO_MODE && schema->scope == MDKR_VIDEO_SCOPE_RESTART &&
            !mdkr_video_values_equal(&s_video.values[i],
                                     &s_desired_video.values[i],
                                     schema->type)) {
            return 1;
        }
    }
    return 0;
}

static const char *mdkr_video_source_name(MdkrVideoSource source) {
    switch (source) {
        case MDKR_VIDEO_SOURCE_DEFAULT: return "default";
        case MDKR_VIDEO_SOURCE_FILE:    return MDKR_VIDEO_INI_PATH;
        case MDKR_VIDEO_SOURCE_PRESET:  return "preset";
        case MDKR_VIDEO_SOURCE_LAUNCHER:return "browser launcher";
        case MDKR_VIDEO_SOURCE_RUNTIME: return "in-game";
        case MDKR_VIDEO_SOURCE_ENV:     return "env";
        case MDKR_VIDEO_SOURCE_CLI:     return "CLI";
    }
    return "?";
}

void mdkr_video_config_report(void) {
    static const char *const mode_name[] = {
        "pure", "restored", "remastered", "custom"
    };
    const MdkrVideoConfig *c = &s_video;
    float effective_aspect;

    printf("[video] mode=%s%s\n", mode_name[c->mode],
           mdkr_video_config_is_readonly() ? " (config read-only)" : "");
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey) i);
        if (s->type == MDKR_VIDEO_TYPE_STRING) {
            printf("  %-30s %-12s [%s]\n", s->name,
                   c->values[i].text[0] != '\0' ? c->values[i].text : "(none)",
                   mdkr_video_source_name(c->values[i].source));
        } else {
            printf("  %-30s %-12g [%s]\n", s->name, (double) c->values[i].number,
                   mdkr_video_source_name(c->values[i].source));
        }
    }

    /*
     * Effective display state, which the --aspect/--widescreen flags can move
     * after we publish. Printing both makes an override visible instead of
     * leaving the report quietly disagreeing with what renders.
     */
    effective_aspect = mdkr_display_forced_aspect();
    printf("  %-30s widescreen=%d aspect=", "(effective display)",
           mdkr_display_widescreen_enabled());
    if (effective_aspect > 0.0f) {
        printf("%.4f\n", (double) effective_aspect);
    } else {
        printf("auto\n");
    }
}
