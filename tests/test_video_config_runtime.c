/*
 * ROM/GPU-free transaction tests for video_config_runtime.c.
 *
 * The test runs in a private temporary working directory because native video
 * settings intentionally live beside the executable as mdkr64.ini. It proves
 * atomic rewrite semantics, unknown-key preservation, CLI locking, live
 * publication, and restart staging without touching a player's real config.
 */
#include "display_config.h"
#include "audio_volume.h"
#include "config_ini.h"
#include "mod_music.h"
#include "mod_texture_store.h"
#include "present_sched.h"
#include "test_platform_compat.h"
#include "video_config.h"

#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* video_config_runtime links the shared packaged-path policy. This ROM-free
 * test stays deliberately SDL-free and never enters packaged mode. */
char *SDL_GetPrefPath(const char *organization, const char *application) {
    (void)organization;
    (void)application;
    return NULL;
}

void SDL_free(void *memory) {
    (void)memory;
}

float g_pcRenderScale;
int g_pcMsaaSamples;
int g_pcTextureAnisotropy;
int g_pcMipmaps;
int g_pcHiresText;
int g_pcRemasterFX;
int g_pcGradePresets;
int g_pcTonemap;
int g_pcPerPixelLight;
int g_pcSunShadow;
float g_pcSunShadowBias;
float g_pcSunShadowUmbra;

static char s_aspect[32];
static char s_fov[32];
static int s_widescreen;
static int s_failures;

int mdkr_display_set_widescreen(const char *value) {
    s_widescreen = value != NULL && !strcmp(value, "1");
    return value != NULL;
}

int mdkr_display_set_aspect(const char *value) {
    if (value == NULL || strlen(value) >= sizeof(s_aspect)) return 0;
    snprintf(s_aspect, sizeof(s_aspect), "%s", value);
    return 1;
}

int mdkr_display_set_gameplay_fov(const char *value) {
    if (value == NULL || strlen(value) >= sizeof(s_fov)) return 0;
    snprintf(s_fov, sizeof(s_fov), "%s", value);
    return 1;
}

float mdkr_display_forced_aspect(void) { return 0.0f; }
int mdkr_display_widescreen_enabled(void) { return s_widescreen; }

/*
 * present_sched.c is deliberately NOT linked into this test (it pulls
 * sim_sched.c/presentation_snapshot.c/platform_os.c, the same reason
 * display_config.c isn't linked either) -- these stubs stand in for it, same
 * pattern as mdkr_display_set_aspect above, so mdkr_video_config_publish()'s
 * push into present_sched's cached state is observable without the real
 * scheduler.
 */
static char s_presentFrameLimit[32] = "(never called)";
static char s_presentSmoothing[32] = "(never called)";
static char s_presentTearing[32] = "(never called)";
static int s_presentFrameLimitCalls;
static int s_presentSmoothingCalls;
static int s_presentTearingCalls;
static const char *const s_config_env_names[] = {
    "MDKR_REMASTER_FX", "MDKR_WIDESCREEN", "MDKR_ASPECT",
    "MDKR_RENDER_SCALE", "MDKR_MSAA", "MDKR_ANISOTROPY",
    "MDKR_MIPMAPS", "MDKR_TEXTURE_PACK", "MDKR_FOV",
    "MDKR_VIDEO_MODE", "MDKR_PRESENT_RATE", "MDKR_PRESENT_SMOOTHING",
    "MDKR_ALLOW_TEARING",
    "MDKR_MASTER_VOLUME", "MDKR_MUSIC_VOLUME", "MDKR_EFFECTS_VOLUME",
    "MDKR_WINDOW_MODE", "MDKR_RUMBLE_ENABLED", "MDKR_RUMBLE_PROFILE",
    "MDKR_CONTROLLER_A", "MDKR_CONTROLLER_B", "MDKR_CONTROLLER_X",
    "MDKR_CONTROLLER_Y", "MDKR_CONTROLLER_START",
    "MDKR_CONTROLLER_LEFT_STICK", "MDKR_CONTROLLER_RIGHT_STICK",
    "MDKR_CONTROLLER_LEFT_SHOULDER", "MDKR_CONTROLLER_RIGHT_SHOULDER",
    "MDKR_CONTROLLER_DPAD_UP", "MDKR_CONTROLLER_DPAD_DOWN",
    "MDKR_CONTROLLER_DPAD_LEFT", "MDKR_CONTROLLER_DPAD_RIGHT",
    "MDKR_CONTROLLER_LEFT_TRIGGER", "MDKR_CONTROLLER_RIGHT_TRIGGER",
    "MDKR_CONTROLLER_RIGHT_STICK_UP", "MDKR_CONTROLLER_RIGHT_STICK_DOWN",
    "MDKR_CONTROLLER_RIGHT_STICK_LEFT", "MDKR_CONTROLLER_RIGHT_STICK_RIGHT",
    "MDKR_VIDEO_CONFIG_PATH",
};

void mdkr_present_set_frame_limit(const char *value) {
    s_presentFrameLimitCalls++;
    if (value != NULL) {
        snprintf(s_presentFrameLimit, sizeof(s_presentFrameLimit), "%s", value);
    }
}

void mdkr_present_set_motion_smoothing(const char *value) {
    s_presentSmoothingCalls++;
    if (value != NULL) {
        snprintf(s_presentSmoothing, sizeof(s_presentSmoothing), "%s", value);
    }
}

void mdkr_present_set_allow_tearing(const char *value) {
    s_presentTearingCalls++;
    if (value != NULL) {
        snprintf(s_presentTearing, sizeof(s_presentTearing), "%s", value);
    }
}

/*
 * Content.PacksEnabled's two receivers. mod_texture_store.c and mod_music.c are
 * not linked here for the same reason display_config.c and present_sched.c are
 * not -- one drags stb_image, mod_registry and mod_source in, the other dr_wav
 * -- so these stand in, and the real headers are included above so a signature
 * that drifts fails to compile rather than silently going unpublished.
 *
 * They start at 1, which is what the real modules default to, so a publish()
 * that never touched them would leave the observation below unchanged and the
 * "packs off" assertion would fail rather than pass by accident.
 */
static int s_modTexturesEnabled = 1;
static int s_modMusicEnabled = 1;

void mdkr_mod_texture_set_enabled(bool enabled) {
    s_modTexturesEnabled = enabled ? 1 : 0;
}

bool mdkr_mod_texture_enabled(void) {
    return s_modTexturesEnabled != 0;
}

void mdkr_mod_music_set_enabled(int enabled) {
    s_modMusicEnabled = enabled != 0;
}

int mdkr_mod_music_enabled(void) {
    return s_modMusicEnabled;
}

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_complete_config(
    const char *label, const MdkrVideoConfig *config) {
    char assertion[160];

    snprintf(assertion, sizeof(assertion), "%s has valid mode", label);
    expect(assertion, config != NULL &&
        config->mode >= MDKR_VIDEO_MODE_PURE &&
        config->mode <= MDKR_VIDEO_MODE_CUSTOM);
    if (config == NULL) {
        return;
    }
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; key++) {
        const MdkrVideoSchema *schema =
            mdkr_video_schema((MdkrVideoKey)key);
        const MdkrVideoValue *value = &config->values[key];
        snprintf(assertion, sizeof(assertion),
                 "%s key %d has schema", label, key);
        expect(assertion, schema != NULL);
        snprintf(assertion, sizeof(assertion),
                 "%s key %d has valid source", label, key);
        expect(assertion,
               value->source >= MDKR_VIDEO_SOURCE_DEFAULT &&
               value->source <= MDKR_VIDEO_SOURCE_CLI);
        snprintf(assertion, sizeof(assertion),
                 "%s key %d text is terminated", label, key);
        expect(assertion,
               memchr(value->text, '\0', sizeof(value->text)) != NULL);
        if (schema != NULL && schema->type != MDKR_VIDEO_TYPE_STRING) {
            snprintf(assertion, sizeof(assertion),
                     "%s key %d number is finite/in range", label, key);
            expect(assertion, isfinite(value->number) &&
                value->number >= schema->min && value->number <= schema->max);
        }
    }
}

static int write_initial_config(void) {
    FILE *f = fopen("mdkr64.ini", "wb");
    const char *text =
        "[Future]\n"
        "OwnedByNewerBuild=keep-me\n"
        "\n"
        "[Video]\n"
        "Mode=restored\n"
        "Aspect=16:10\n"
        "GameplayFOV=authored\n"
        "Mipmaps=1\n";
    if (f == NULL) return 0;
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int read_config(char *out, size_t capacity) {
    FILE *f = fopen("mdkr64.ini", "rb");
    size_t n;
    if (f == NULL || capacity == 0) return 0;
    n = fread(out, 1, capacity - 1, f);
    out[n] = '\0';
    fclose(f);
    return 1;
}

static int read_file_bytes(const char *path, unsigned char *out,
                           size_t capacity, size_t *out_size) {
    FILE *file;
    size_t count;
    if (path == NULL || out == NULL || out_size == NULL || capacity == 0u) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    count = fread(out, 1, capacity, file);
    if (ferror(file) || fclose(file) != 0) return 0;
    *out_size = count;
    return 1;
}

static int write_file_bytes(const char *path, const unsigned char *bytes,
                            size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (fwrite(bytes, 1, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int config_has_entry(const char *text, const char *key,
                            const char *value) {
    ConfigIniEntry entries[128];
    int count = 0;
    if (text == NULL || key == NULL || value == NULL ||
        !config_ini_parse(text, entries,
                          (int)(sizeof(entries) / sizeof(entries[0])),
                          &count)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!strcmp(entries[i].key, key) && !strcmp(entries[i].value, value)) {
            return 1;
        }
    }
    return 0;
}

static int no_config_staging_files(const char *directory) {
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int clean = dir != NULL;
    if (dir == NULL) return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!strncmp(entry->d_name, "mdkr64.ini.tmp.", 15)) clean = 0;
    }
    closedir(dir);
    return clean;
}

static void remove_config_artifacts(const char *directory) {
    char path[2300];
    DIR *dir;
    struct dirent *entry;
    snprintf(path, sizeof(path), "%s/mdkr64.ini", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/mdkr64.ini.lock", directory);
    unlink(path);
    dir = opendir(directory);
    if (dir == NULL) return;
    while ((entry = readdir(dir)) != NULL) {
        if (!strncmp(entry->d_name, "mdkr64.ini.tmp.", 15)) {
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            unlink(path);
        }
    }
    closedir(dir);
}

static int run_primary_case(void) {
    char temporary[2048];
    char original[2048];
    char text[32768];
    char *argv[] = {
        "mdkr-video-runtime-test",
        "--video-set",
        "Video.Mipmaps=0",
    };
    char *engine_argv[] = {
        "mdkr-video-runtime-test",
        "--video-set",
        "Video.FrameLimit=240",
    };
    expect("temporary directory created",
           mdkr_test_make_temp_directory(
               temporary, sizeof(temporary), "mdkr-video-runtime"));
    if (s_failures != 0) return 1;
    expect("original cwd captured", getcwd(original, sizeof(original)) != NULL);
    expect("entered temporary directory", chdir(temporary) == 0);
    for (size_t i = 0;
         i < sizeof(s_config_env_names) / sizeof(s_config_env_names[0]); i++) {
        (void)mdkr_test_env_unset(s_config_env_names[i]);
    }
    {
        char config_path[2300];
        snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
        expect("explicit video config path override set",
               mdkr_test_env_set(
                   "MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    }
    expect("initial config written", write_initial_config());

    mdkr_video_config_init(3, argv);
    mdkr_video_config_publish();
    expect("file mode resolved", mdkr_video_config_current()->mode ==
                                 MDKR_VIDEO_MODE_RESTORED);
    expect("file aspect published", !strcmp(s_aspect, "16:10"));
    expect("authored FOV published", !strcmp(s_fov, "authored"));
    expect("CLI mipmap value effective", g_pcMipmaps == 0);
    expect("restored grade is disabled",
           g_pcGradePresets == 0 && g_pcTonemap == 0);
    {
        MdkrAudioVolumeState volume;
        mdkr_audio_volume_snapshot(&volume);
        expect("audio defaults publish at authored unity",
               volume.master == 100 && volume.music == 100 &&
               volume.effects == 100);
    }
    expect("CLI setting reports locked",
           mdkr_video_config_runtime_locked(MDKR_VIDEO_MIPMAPS));
    expect("locked setting rejected",
           mdkr_video_config_runtime_set(MDKR_VIDEO_MIPMAPS, "1") ==
               MDKR_VIDEO_RUNTIME_LOCKED);

    expect("master volume applies live",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "55") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("audio-only edit keeps presentation preset",
           mdkr_video_config_desired()->mode == MDKR_VIDEO_MODE_RESTORED);
    {
        MdkrAudioVolumeState volume;
        mdkr_audio_volume_snapshot(&volume);
        expect("master volume reached output policy", volume.master == 55);
    }
    expect("music preview applies without changing config",
           mdkr_audio_config_runtime_preview(MDKR_AUDIO_MUSIC_VOLUME, 12));
    {
        MdkrAudioVolumeState volume;
        mdkr_audio_volume_snapshot(&volume);
        expect("audible preview reached output policy", volume.music == 12);
        expect("preview did not mutate persisted desired state",
               mdkr_video_config_desired()
                       ->values[MDKR_AUDIO_MUSIC_VOLUME].number == 100.0f);
    }
    mdkr_audio_config_runtime_cancel_preview();
    {
        MdkrAudioVolumeState volume;
        mdkr_audio_volume_snapshot(&volume);
        expect("cancel restored committed music", volume.music == 100);
    }
    expect("original game sliders persist atomically",
           mdkr_audio_config_runtime_set_game_levels(128, 64) ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("game slider mapping is rounded to percentages",
           mdkr_video_config_current()->values[MDKR_AUDIO_MUSIC_VOLUME].number ==
                   50.0f &&
               mdkr_video_config_current()
                       ->values[MDKR_AUDIO_EFFECTS_VOLUME].number == 25.0f);

    expect("rumble disable applies live",
           mdkr_video_config_runtime_set(MDKR_INPUT_RUMBLE_ENABLED, "0") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("controller button remap applies live",
           mdkr_video_config_runtime_set(MDKR_INPUT_CONTROLLER_A, "r") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("window mode preference applies live in config transaction",
           mdkr_video_config_runtime_set(MDKR_WINDOW_MODE, "fullscreen") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("comfort edits keep presentation preset",
           mdkr_video_config_desired()->mode == MDKR_VIDEO_MODE_RESTORED);

    expect("aspect applies live",
           mdkr_video_config_runtime_set(MDKR_VIDEO_ASPECT, "21:9") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live aspect reached display policy", !strcmp(s_aspect, "21:9"));
    expect("individual edit marks custom mode",
           mdkr_video_config_desired()->mode == MDKR_VIDEO_MODE_CUSTOM);
    expect("supersampling applies live",
           mdkr_video_config_runtime_set(MDKR_VIDEO_RENDER_SCALE, "3") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live supersampling published", g_pcRenderScale == 3.0f);
    expect("FOV applies live",
           mdkr_video_config_runtime_set(MDKR_VIDEO_GAMEPLAY_FOV, "75") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live FOV reached display policy", !strcmp(s_fov, "75"));

    /*
     * FrameLimit/MotionSmoothing were untouched by the file, CLI or env layer
     * above (write_initial_config never sets them), so the two boot-time
     * publish() calls so far (mdkr_video_config_init/publish above, plus every
     * runtime_set's internal republish) must NOT have pushed into
     * present_sched: both stayed schema-DEFAULT-sourced. This is the guard
     * that keeps a raw diagnostic MDKR_PRESENT_RATE=30/120 untouched by config
     * (tests/check_presentation_matrix.py arm B) -- see video_config_runtime.c.
     */
    expect("frame limit not pushed while still DEFAULT-sourced",
           s_presentFrameLimitCalls == 0);
    expect("motion smoothing not pushed while still DEFAULT-sourced",
           s_presentSmoothingCalls == 0);
    expect("tearing opt-in not pushed while still DEFAULT-sourced",
           s_presentTearingCalls == 0);

    /*
     * FrameLimit/MotionSmoothing/AllowTearing are SCOPE_LIVE with the
     * PRESENTATION apply domain. NO DOMAIN IS REGISTERED IN THIS PROCESS --
     * there is no engine here to defer to -- so the contract under test is the
     * un-deferred one: publish() pushes them inline exactly as it always did,
     * and the setter reports LIVE. That is not a weaker case than the deferred
     * one; it is the case that keeps --video-set, the launcher's pre-Play
     * panel, and every ROM-free run on the path they have always taken. The
     * deferred path has its own case (run_deferred_apply_case) below.
     */
    expect("frame limit applies live",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "240") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live frame limit reached present_sched",
           s_presentFrameLimitCalls == 1 &&
           !strcmp(s_presentFrameLimit, "240"));
    expect("a live frame limit raises no restart",
           !mdkr_video_config_restart_pending());
    expect("motion smoothing applies live",
           mdkr_video_config_runtime_set(
               MDKR_VIDEO_MOTION_SMOOTHING, "interpolate") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live motion smoothing reached present_sched",
           s_presentSmoothingCalls == 1 &&
           !strcmp(s_presentSmoothing, "interpolate"));
    expect("tearing opt-in applies live",
           mdkr_video_config_runtime_set(MDKR_VIDEO_ALLOW_TEARING, "on") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("live tearing opt-in reached present_sched",
           s_presentTearingCalls == 1 && !strcmp(s_presentTearing, "on"));
    expect("live tearing opt-in is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_ALLOW_TEARING].text, "on"));
    /* ...and the applied values are also what a restart would resolve. */
    expect("live frame limit is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_FRAME_LIMIT].text, "240"));
    expect("live motion smoothing is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                   "interpolate"));

    /*
     * Content.PacksEnabled is SCOPE_LIVE for the same reason the three above
     * are: publish() carries it to a receiver. Before it did, the key was
     * declared live and reached nobody, so the checkbox did nothing until the
     * next launch. BOTH directions are asserted -- an implementation that only
     * published "off" would leave a player unable to switch their packs back
     * on without relaunching, which is the worse half of the same defect.
     */
    expect("custom content switches off live",
           mdkr_video_config_runtime_set(MDKR_CONTENT_PACKS_ENABLED, "0") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("switching custom content off reached textures and music",
           !mdkr_mod_texture_enabled() && !mdkr_mod_music_enabled());
    expect("custom content switches back on live",
           mdkr_video_config_runtime_set(MDKR_CONTENT_PACKS_ENABLED, "1") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("switching custom content on reached textures and music",
           mdkr_mod_texture_enabled() && mdkr_mod_music_enabled());
    expect("switching custom content raises no restart",
           !mdkr_video_config_restart_pending());

    /*
     * The presentation-pace quick choice: ONE call that must move BOTH keys.
     * The counters are what make "together" checkable -- a Smooth press that
     * pushed only the frame limit would leave the run half-converted and every
     * value-based assertion below would still pass on the half that moved.
     */
    {
        const int limitBefore = s_presentFrameLimitCalls;
        const int smoothingBefore = s_presentSmoothingCalls;
        expect("smooth quick choice applies live",
               mdkr_video_config_runtime_set_presentation_pace(
                   MDKR_PRESENTATION_PACE_SMOOTH) ==
                   MDKR_VIDEO_RUNTIME_LIVE);
        expect("smooth quick choice pushed BOTH pacing keys once",
               s_presentFrameLimitCalls == limitBefore + 1 &&
               s_presentSmoothingCalls == smoothingBefore + 1 &&
               !strcmp(s_presentFrameLimit, "display") &&
               !strcmp(s_presentSmoothing, "interpolate"));
        expect("smooth quick choice reads back as smooth",
               mdkr_video_presentation_pace(mdkr_video_config_desired()) ==
                   MDKR_PRESENTATION_PACE_SMOOTH);
        expect("original quick choice applies live",
               mdkr_video_config_runtime_set_presentation_pace(
                   MDKR_PRESENTATION_PACE_ORIGINAL) ==
                   MDKR_VIDEO_RUNTIME_LIVE);
        expect("original quick choice restored the authored pair",
               !strcmp(mdkr_video_config_desired()
                           ->values[MDKR_VIDEO_FRAME_LIMIT].text,
                       "original") &&
               !strcmp(mdkr_video_config_desired()
                           ->values[MDKR_VIDEO_MOTION_SMOOTHING].text, "off") &&
               mdkr_video_presentation_pace(mdkr_video_config_desired()) ==
                   MDKR_PRESENTATION_PACE_ORIGINAL);
        expect("custom is refused: it has no expansion to write",
               mdkr_video_config_runtime_set_presentation_pace(
                   MDKR_PRESENTATION_PACE_CUSTOM) ==
                   MDKR_VIDEO_RUNTIME_INVALID);
        /* Restore what the rest of the case expects to find. */
        expect("frame limit restored for the remaining assertions",
               mdkr_video_config_runtime_set(
                   MDKR_VIDEO_FRAME_LIMIT, "240") ==
                   MDKR_VIDEO_RUNTIME_LIVE);
        expect("motion smoothing restored for the remaining assertions",
               mdkr_video_config_runtime_set(
                   MDKR_VIDEO_MOTION_SMOOTHING, "interpolate") ==
                   MDKR_VIDEO_RUNTIME_LIVE);
    }

    /*
     * Camera.Obstruction is SCOPE_LEVEL. With no CAMERA applier registered
     * there is no level boundary to wait for either, so it behaves as a plain
     * live key here -- the value is stored and the boot-time seam
     * (MDKR_CAMERA_OBSTRUCTION) remains what the camera runtime reads. Only the
     * key's own domain is legal: an unparsed policy is a rejected VALUE, not a
     * setting that quietly stores.
     */
    expect("camera obstruction stores without a restart",
           mdkr_video_config_runtime_set(
               MDKR_VIDEO_CAMERA_OBSTRUCTION, "modern") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("stored camera obstruction is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
                   "modern"));
    expect("camera obstruction rejects an unknown policy",
           mdkr_video_config_runtime_set(
               MDKR_VIDEO_CAMERA_OBSTRUCTION, "modren") ==
               MDKR_VIDEO_RUNTIME_INVALID);
    expect("a rejected policy leaves the staged value alone",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
                   "modern"));

    expect("remaster effects stage for restart",
           mdkr_video_config_runtime_set(MDKR_VIDEO_REMASTER_FX, "1") ==
               MDKR_VIDEO_RUNTIME_RESTART);
    expect("active remaster effects unchanged", g_pcRemasterFX == 0);
    expect("restart is pending", mdkr_video_config_restart_pending());
    expect("mode transaction rejects partial CLI override",
           mdkr_video_config_runtime_set(MDKR_VIDEO_MODE, "pure") ==
               MDKR_VIDEO_RUNTIME_LOCKED);

    expect("persisted config readable", read_config(text, sizeof(text)));
    expect("unknown key round-tripped",
           strstr(text, "OwnedByNewerBuild=keep-me") != NULL);
    expect("custom mode persisted", strstr(text, "Mode=custom") != NULL);
    expect("aspect persisted", strstr(text, "Aspect=21:9") != NULL);
    expect("FOV persisted", strstr(text, "GameplayFOV=75") != NULL);
    expect("live render scale persisted", strstr(text, "RenderScale=3") != NULL);
    expect("rumble disable persisted", strstr(text, "RumbleEnabled=0") != NULL);
    expect("controller remap persisted", strstr(text, "ControllerA=r") != NULL);
    expect("window mode persisted", config_has_entry(
               text, "Window.Mode", "fullscreen"));
    expect("camera obstruction persisted", config_has_entry(
               text, "Camera.Obstruction", "modern"));
    expect("temporary CLI value not baked into config",
           strstr(text, "Mipmaps=1") != NULL);
    expect("all unique atomic temporary files removed",
           no_config_staging_files("."));

    /* The native Restore controller defaults action builds this exact bounded
     * multi-key transaction. Exercise all mappings and both rumble fields as
     * one commit so a partial restore can never become a UI-only assumption. */
    {
        MdkrVideoConfig defaults;
        MdkrVideoRuntimeChange changes[MDKR_VIDEO_KEY_COUNT];
        char values[MDKR_VIDEO_KEY_COUNT][MDKR_VIDEO_STRING_MAX];
        int count = 0;
        mdkr_video_config_defaults(&defaults);
        for (int key = MDKR_INPUT_FIRST_KEY; key <= MDKR_INPUT_LAST_KEY; key++) {
            const MdkrVideoSchema *schema =
                mdkr_video_schema((MdkrVideoKey)key);
            if (schema->type == MDKR_VIDEO_TYPE_STRING) {
                snprintf(values[count], sizeof(values[count]), "%s",
                         defaults.values[key].text);
            } else {
                snprintf(values[count], sizeof(values[count]), "%d",
                         (int)defaults.values[key].number);
            }
            changes[count].key = (MdkrVideoKey)key;
            changes[count].value = values[count];
            count++;
        }
        expect("controller defaults restore is one live transaction",
               mdkr_video_config_runtime_set_many(changes, count) ==
                   MDKR_VIDEO_RUNTIME_LIVE);
        expect("controller defaults restore covers mappings and rumble",
               mdkr_video_config_current()
                       ->values[MDKR_INPUT_RUMBLE_ENABLED].number == 1.0f &&
                   !strcmp(mdkr_video_config_current()
                               ->values[MDKR_INPUT_RUMBLE_PROFILE].text,
                           "strong") &&
                   !strcmp(mdkr_video_config_current()
                               ->values[MDKR_INPUT_CONTROLLER_A].text,
                           "a") &&
                   !strcmp(mdkr_video_config_current()
                               ->values[MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT]
                               .text,
                           "c_right"));
    }

    /*
     * The launcher and engine share this process. A normal second init must
     * stay inert, while the explicit one-shot handoff promotes staged restart
     * settings and engine CLI overrides before the presentation policy latches.
     */
    expect("launcher-to-engine handoff completed",
           mdkr_video_config_handoff_to_engine(3, engine_argv));
    expect_complete_config("active handoff config",
                           mdkr_video_config_current());
    expect_complete_config("desired handoff config",
                           mdkr_video_config_desired());
    expect("handed-off frame limit is active",
           !strcmp(mdkr_video_config_current()
                       ->values[MDKR_VIDEO_FRAME_LIMIT].text, "240"));
    expect("handed-off frame limit is desired",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_FRAME_LIMIT].text, "240"));
    expect("engine CLI retained highest precedence",
           mdkr_video_config_current()
                   ->values[MDKR_VIDEO_FRAME_LIMIT].source ==
               MDKR_VIDEO_SOURCE_CLI);
    expect("handoff clears restart pending",
           !mdkr_video_config_restart_pending());

    {
        /* Once PER PUBLISH, which is the property that matters -- a publish
         * that pushed twice, or not at all, would both be bugs. Counting from a
         * mark rather than from zero because the live edits earlier in this
         * case legitimately pushed too; the old absolute count only worked
         * while the key was RESTART-scoped and could not be pushed before
         * here. */
        const int before = s_presentFrameLimitCalls;
        mdkr_video_config_init(3, argv); /* remains idempotent after handoff */
        mdkr_video_config_publish();
        expect("240 frame limit reached present scheduler once",
               s_presentFrameLimitCalls == before + 1 &&
               !strcmp(s_presentFrameLimit, "240"));
    }
    expect("one-shot handoff rejects a second transition",
           !mdkr_video_config_handoff_to_engine(3, engine_argv));
    expect("completed engine session rearms one future handoff",
           mdkr_video_config_engine_session_complete());
    expect("engine-session completion cannot be repeated",
           !mdkr_video_config_engine_session_complete());
    expect("next engine session receives one handoff",
           mdkr_video_config_handoff_to_engine(3, engine_argv));
    expect("next engine session still rejects a duplicate handoff",
           !mdkr_video_config_handoff_to_engine(3, engine_argv));

    expect("returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("primary config artifacts cleaned", rmdir(temporary) == 0);

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("all video_config runtime tests passed\n");
    return 0;
}

/* --------------------------------------------------------------------------
 *  Deferred apply (video_config.h). The ROM-free half of the live-toggle gate.
 *
 * WHAT THIS CASE OWNS. The MECHANISM: that a registered domain defers, that the
 * deferral is exact about which boundary, that the value is not published to
 * its receiver one instant before that boundary, and that a LEVEL key's live
 * config keeps telling the truth while it waits. What it deliberately does NOT
 * own is whether the resulting engine state is correct -- that needs a ROM and
 * a running pacer, and tests/check_live_toggle_settings.py is where it lives.
 * ------------------------------------------------------------------------ */

static int s_presentationApplies;
static int s_cameraApplies;

static void fake_presentation_apply(void) {
    s_presentationApplies++;
    /* The real applier's step 4. Standing in for it here is what lets this case
     * assert that the push happens at the BOUNDARY and not at the setter. */
    mdkr_video_config_push_presentation();
}

static void fake_camera_apply(void) {
    s_cameraApplies++;
}

static int run_deferred_apply_case(void) {
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    char *argv[] = { "mdkr-video-runtime-test" };

    s_failures = 0;
    expect("deferred-apply temporary directory created",
           mdkr_test_make_temp_directory(
               temporary, sizeof(temporary), "mdkr-deferred-apply-runtime"));
    if (s_failures != 0) return 1;
    expect("deferred-apply original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("deferred-apply entered temporary directory", chdir(temporary) == 0);
    for (size_t i = 0;
         i < sizeof(s_config_env_names) / sizeof(s_config_env_names[0]); i++) {
        (void)mdkr_test_env_unset(s_config_env_names[i]);
    }
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("deferred-apply config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);

    mdkr_video_config_init(1, argv);
    mdkr_video_config_publish();

    s_presentFrameLimitCalls = 0;
    s_presentSmoothingCalls = 0;
    s_presentTearingCalls = 0;
    s_presentationApplies = 0;
    s_cameraApplies = 0;

    /* Nothing is registered yet, so nothing is pending and a boundary is a
     * no-op. This is the state every headless run stays in forever. */
    expect("no boundary work before registration",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 0 &&
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LEVEL) == 0);

    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_PRESENTATION,
                                     fake_presentation_apply);
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_CAMERA,
                                     fake_camera_apply);

    /* Registration alone must not make a boundary do work. pending_scope's
     * zero-initialized value is SCOPE_LIVE, so a domain that did not clear it
     * on registration would fire here -- an apply nobody asked for, on a value
     * nobody changed. */
    expect("registration alone stages nothing",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 0 &&
           s_presentationApplies == 0);

    /* --- The presentation domain: LIVE, deferred to the frame boundary --- */
    expect("deferred frame limit is accepted",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "120") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    /* THE HAZARD ASSERTION. present_sched must not have been touched by the
     * setter's own call stack: in the engine that stack is an ImGui draw,
     * possibly inside a presentation subloop that is replaying a retained
     * display list captured under the OLD policy. */
    expect("a deferred frame limit does not reach present_sched at set time",
           s_presentFrameLimitCalls == 0);
    expect("the frame boundary is pending",
           mdkr_video_config_apply_is_pending(MDKR_VIDEO_SCOPE_LIVE));
    /* ...and the LEVEL boundary is NOT, because a boundary services only the
     * domains that named it. */
    expect("the level boundary is not pending for a LIVE key",
           !mdkr_video_config_apply_is_pending(MDKR_VIDEO_SCOPE_LEVEL));
    expect("a level boundary does not flush a LIVE domain",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LEVEL) == 0 &&
           s_presentationApplies == 0 && s_presentFrameLimitCalls == 0);

    expect("the frame boundary applies the presentation domain",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 1);
    expect("the applier is what pushed into present_sched",
           s_presentationApplies == 1 && s_presentFrameLimitCalls == 1 &&
           !strcmp(s_presentFrameLimit, "120"));
    expect("a serviced boundary is no longer pending",
           !mdkr_video_config_apply_is_pending(MDKR_VIDEO_SCOPE_LIVE) &&
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 0);

    /* Two keys of one domain in one transaction are ONE apply, not two. The
     * ordered teardown/rebuild is expensive and, more to the point, running it
     * twice would invalidate the replay history the first pass just rebuilt. */
    {
        const MdkrVideoRuntimeChange changes[2] = {
            { MDKR_VIDEO_MOTION_SMOOTHING, "interpolate" },
            { MDKR_VIDEO_ALLOW_TEARING, "on" },
        };
        expect("a two-key presentation transaction is accepted",
               mdkr_video_config_runtime_set_many(changes, 2) ==
                   MDKR_VIDEO_RUNTIME_LIVE);
        expect("neither key reached present_sched at set time",
               s_presentSmoothingCalls == 0 && s_presentTearingCalls == 0);
        expect("two keys of one domain are one apply",
               mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 1 &&
               s_presentationApplies == 2);
        expect("both keys reached present_sched from that one apply",
               s_presentSmoothingCalls == 1 && s_presentTearingCalls == 1 &&
               !strcmp(s_presentSmoothing, "interpolate") &&
               !strcmp(s_presentTearing, "on"));
    }

    /*
     * TWO EDITS BEFORE ONE BOUNDARY. A settings combobox can be moved twice in
     * a frame, and the boundary must apply the LATEST choice -- the same
     * newest-intent-wins rule AppWindow_requestMode already follows. The apply
     * still happens once, because the ordered teardown/rebuild is per domain
     * and not per edit.
     */
    s_presentFrameLimitCalls = 0;
    expect("the first of two frame-limit edits is accepted",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "144") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("the second replaces it",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "60") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("neither reached present_sched before the boundary",
           s_presentFrameLimitCalls == 0);
    expect("two edits are still one apply",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 1);
    expect("the boundary applied the newest choice, not the first",
           s_presentFrameLimitCalls == 1 &&
           !strcmp(s_presentFrameLimit, "60"));

    /* --- The camera domain: LEVEL, deferred to a level load ---
     *
     * The staged direction is observe -> modern, the opt-in, because that is
     * the change a player makes again: the authored camera is the default, so
     * staging "observe" against the default would stage nothing at all. */
    expect("deferred camera obstruction is accepted",
           mdkr_video_config_runtime_set(
               MDKR_VIDEO_CAMERA_OBSTRUCTION, "modern") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("the level boundary is pending",
           mdkr_video_config_apply_is_pending(MDKR_VIDEO_SCOPE_LEVEL));
    /*
     * THE DIRECTION THAT MATTERS. A frame boundary must not apply a LEVEL
     * domain early: that would be precisely the mid-race camera cut the scope
     * decision exists to prevent, and it would be invisible in play because the
     * only symptom is a camera that jumps once.
     */
    expect("a frame boundary does not apply the camera domain",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE) == 0 &&
           s_cameraApplies == 0);
    /*
     * ...and while it waits, the live config still reports the policy the
     * engine is actually running. This is what the settings panel reads to say
     * "now: Authored", and a staged value copied in early would make it lie.
     */
    expect("the live config still holds the running policy",
           !strcmp(mdkr_video_config_current()
                       ->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
                   "observe"));
    expect("the desired config holds the player's choice",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
                   "modern"));
    /* A LEVEL key must not raise the restart notice; that is the whole point. */
    expect("a staged LEVEL key raises no restart",
           !mdkr_video_config_restart_pending());

    /* Camera.Comfort shares the domain, so the two keys must reach the engine
     * through one boundary rather than two -- one apply, both values. */
    expect("deferred camera comfort is accepted",
           mdkr_video_config_runtime_set(
               MDKR_VIDEO_CAMERA_COMFORT, "reduced") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("comfort waits at the same boundary",
           mdkr_video_config_apply_is_pending(MDKR_VIDEO_SCOPE_LEVEL) &&
           !strcmp(mdkr_video_config_current()
                       ->values[MDKR_VIDEO_CAMERA_COMFORT].text,
                   "authored"));

    expect("the level boundary applies the camera domain",
           mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LEVEL) == 1 &&
           s_cameraApplies == 1);
    expect("the applied policy is now the live config",
           !strcmp(mdkr_video_config_current()
                       ->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text,
                   "modern"));
    expect("one apply carried both camera keys",
           !strcmp(mdkr_video_config_current()
                       ->values[MDKR_VIDEO_CAMERA_COMFORT].text,
                   "reduced"));

    /* Unregistering returns the domain to the inline path rather than leaving
     * a value staged for a boundary that no longer exists. */
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_PRESENTATION, NULL);
    s_presentFrameLimitCalls = 0;
    expect("an unregistered domain applies inline again",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "90") ==
               MDKR_VIDEO_RUNTIME_LIVE &&
           s_presentFrameLimitCalls == 1 &&
           !strcmp(s_presentFrameLimit, "90"));
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_CAMERA, NULL);

    expect("deferred-apply returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("deferred-apply artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d deferred-apply failure(s)\n", s_failures);
        return 1;
    }
    printf("Deferred apply tests passed\n");
    return 0;
}

static int run_pure_comfort_case(void) {
    char temporary[2048];
    char original[2048];
    char text[32768];
    char config_path[2300];
    char *argv[] = { "mdkr-video-runtime-test", "--pure" };

    s_failures = 0;
    expect("pure-comfort temporary directory created",
           mdkr_test_make_temp_directory(
               temporary, sizeof(temporary), "mdkr-pure-comfort-runtime"));
    if (s_failures != 0) return 1;
    expect("pure-comfort original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("pure-comfort entered temporary directory", chdir(temporary) == 0);
    for (size_t i = 0;
         i < sizeof(s_config_env_names) / sizeof(s_config_env_names[0]); i++) {
        (void)mdkr_test_env_unset(s_config_env_names[i]);
    }
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("pure-comfort config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    expect("pure-comfort initial config written", write_initial_config());

    mdkr_video_config_init(2, argv);
    mdkr_video_config_publish();
    expect("explicit Pure session is presentation read-only",
           mdkr_video_config_is_readonly());
    expect("Pure audio comfort edit applies live",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "44") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("Pure controller comfort edit applies live",
           mdkr_video_config_runtime_set(MDKR_INPUT_CONTROLLER_A, "r") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("Pure rumble comfort edit applies live",
           mdkr_video_config_runtime_set(MDKR_INPUT_RUMBLE_ENABLED, "0") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("Pure rumble profile edit applies live",
           mdkr_video_config_runtime_set(MDKR_INPUT_RUMBLE_PROFILE, "light") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("Pure window comfort edit applies live",
           mdkr_video_config_runtime_set(MDKR_WINDOW_MODE, "fullscreen") ==
               MDKR_VIDEO_RUNTIME_LIVE);
    expect("Pure comfort persistence is readable",
           read_config(text, sizeof(text)));
    expect("Pure comfort persistence keeps prior mode",
           strstr(text, "Mode=restored") != NULL &&
               strstr(text, "Mode=pure") == NULL);
    expect("Pure comfort persistence keeps prior framing",
           strstr(text, "Aspect=16:10") != NULL &&
               strstr(text, "GameplayFOV=authored") != NULL);
    expect("Pure comfort persistence keeps prior fidelity",
           strstr(text, "Mipmaps=1") != NULL);
    expect("Pure comfort persistence writes audio value",
           strstr(text, "MasterVolume=44") != NULL);
    expect("Pure persistence writes controller comfort value",
           strstr(text, "ControllerA=r") != NULL);
    expect("Pure persistence writes rumble comfort value",
           strstr(text, "RumbleEnabled=0") != NULL);
    expect("Pure persistence writes rumble profile comfort value",
           strstr(text, "RumbleProfile=light") != NULL);
    expect("Pure persistence writes window comfort value",
           config_has_entry(text, "Window.Mode", "fullscreen"));
    expect("Pure comfort persistence keeps unknown settings",
           strstr(text, "OwnedByNewerBuild=keep-me") != NULL);
    expect("Pure session remains active after comfort edits",
           mdkr_video_config_current()->mode == MDKR_VIDEO_MODE_PURE);
    /*
     * The comfort writes above rebuild the staged config from the file while
     * holding the lock. The preset rank a --pure invocation established has to
     * survive that rebuild, or the session silently stops being the oracle
     * reference it was launched as. Every assertion below failed before the
     * invocation-rank rescue: the desired config resolved back to the file's
     * `restored`, which unlocked presentation and made the pending-restart
     * banner offer to "apply" values the player never chose.
     */
    expect("Pure staged mode survives an unrelated comfort write",
           mdkr_video_config_desired()->mode == MDKR_VIDEO_MODE_PURE &&
               mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_MODE].source ==
                   MDKR_VIDEO_SOURCE_PRESET);
    expect("Pure preset values survive an unrelated comfort write",
           mdkr_video_config_desired()
                   ->values[MDKR_VIDEO_MIPMAPS].number == 0.0f &&
               mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_MIPMAPS].source ==
                   MDKR_VIDEO_SOURCE_PRESET);
    expect("Pure session is still presentation read-only after comfort writes",
           mdkr_video_config_is_readonly() &&
               mdkr_video_config_runtime_locked(MDKR_VIDEO_MIPMAPS) &&
               mdkr_video_config_runtime_locked(MDKR_VIDEO_MODE));
    expect("Pure comfort writes raise no phantom restart-required",
           mdkr_video_config_restart_pending() == 0);

    expect("pure-comfort returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("pure-comfort config artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d pure-comfort failure(s)\n", s_failures);
        return 1;
    }
    printf("Pure comfort persistence tests passed\n");
    return 0;
}

static int run_corrupt_handoff_case(void) {
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    char *argv[] = { "mdkr-video-runtime-test" };
    char *engine_argv[] = {
        "mdkr-video-runtime-test", "--video-set", "Video.FrameLimit=120",
    };
    FILE *file;
    char byte = 'x';
    long size;

    s_failures = 0;
    expect("corrupt-handoff temporary directory created",
           mdkr_test_make_temp_directory(temporary, sizeof(temporary),
                                         "mdkr-corrupt-video-runtime"));
    if (s_failures != 0) return 1;
    expect("corrupt-handoff original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("corrupt-handoff entered temporary directory", chdir(temporary) == 0);
    for (size_t i = 0;
         i < sizeof(s_config_env_names) / sizeof(s_config_env_names[0]); i++) {
        (void)mdkr_test_env_unset(s_config_env_names[i]);
    }
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("corrupt-handoff config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    file = fopen("mdkr64.ini", "wb");
    expect("corrupt-handoff oversized config opened", file != NULL);
    if (file != NULL) {
        for (int i = 0; i < 32768; i++) {
            if (fwrite(&byte, 1, 1, file) != 1) break;
        }
        expect("corrupt-handoff oversized config written", fclose(file) == 0);
    }
    mdkr_video_config_init(1, argv);
    expect("corrupt config does not block engine handoff",
           mdkr_video_config_handoff_to_engine(3, engine_argv));
    expect_complete_config("corrupt handoff config", mdkr_video_config_current());
    expect("corrupt handoff retains engine CLI", !strcmp(
        mdkr_video_config_current()->values[MDKR_VIDEO_FRAME_LIMIT].text, "120"));
    expect("corrupt config refuses mutation rather than overwriting it",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "44") ==
               MDKR_VIDEO_RUNTIME_SAVE_FAILED);
    file = fopen("mdkr64.ini", "rb");
    size = -1;
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fclose(file);
    }
    expect("corrupt config remains byte-count unchanged", size == 32768);
    expect("corrupt-handoff returned to original cwd", chdir(original) == 0);
    expect("corrupt config leaves no staging file", no_config_staging_files(temporary));
    remove_config_artifacts(temporary);
    expect("corrupt-handoff config artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d corrupt-handoff failure(s)\n", s_failures);
        return 1;
    }
    printf("Corrupt config handoff tests passed\n");
    return 0;
}

static int run_embedded_nul_case(void) {
    static const unsigned char fixture[] =
        "[Video]\nMode=restored\n\0[Video]\nMasterVolume=1\n";
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    unsigned char before[sizeof(fixture)];
    unsigned char after[sizeof(fixture)];
    size_t before_size = 0u;
    size_t after_size = 0u;
    char *argv[] = { "mdkr-video-runtime-test" };

    s_failures = 0;
    expect("embedded-NUL temporary directory created",
           mdkr_test_make_temp_directory(temporary, sizeof(temporary),
                                         "mdkr-nul-video-runtime"));
    if (s_failures != 0) return 1;
    expect("embedded-NUL original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("embedded-NUL entered temporary directory", chdir(temporary) == 0);
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("embedded-NUL config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    expect("embedded-NUL fixture written",
           write_file_bytes("mdkr64.ini", fixture, sizeof(fixture) - 1u));
    expect("embedded-NUL fixture captured",
           read_file_bytes("mdkr64.ini", before, sizeof(before), &before_size));
    mdkr_video_config_init(1, argv);
    expect("embedded-NUL config refuses mutation",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "44") ==
               MDKR_VIDEO_RUNTIME_SAVE_FAILED);
    expect("embedded-NUL fixture remains readable",
           read_file_bytes("mdkr64.ini", after, sizeof(after), &after_size));
    expect("embedded-NUL config remains byte-for-byte unchanged",
           after_size == before_size && memcmp(after, before, before_size) == 0);
    expect("embedded-NUL returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("embedded-NUL artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d embedded-NUL failure(s)\n", s_failures);
        return 1;
    }
    printf("Embedded-NUL video config tests passed\n");
    return 0;
}

static int run_durability_case(void) {
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    char text[32768];
    char *argv[] = { "mdkr-video-runtime-test" };

    s_failures = 0;
    expect("durability temporary directory created",
           mdkr_test_make_temp_directory(temporary, sizeof(temporary),
                                         "mdkr-durable-video-runtime"));
    if (s_failures != 0) return 1;
    expect("durability original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("durability entered temporary directory", chdir(temporary) == 0);
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("durability config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    expect("durability initial config written", write_initial_config());
    mdkr_video_config_init(1, argv);
    mdkr_video_test_force_directory_sync_failure(1);
    expect("runtime outcome helper rejects actual save failures",
           !mdkr_video_runtime_result_applied(MDKR_VIDEO_RUNTIME_SAVE_FAILED));
    expect("directory-sync uncertainty is distinct from durable success",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "44") ==
               MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED);
    expect("runtime outcome helper accepts visible unconfirmed changes",
           mdkr_video_runtime_result_applied(
               MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED));
    mdkr_video_test_force_directory_sync_failure(0);
    expect("unconfirmed replacement remains visibly readable",
           read_config(text, sizeof(text)) &&
               config_has_entry(text, "Audio.MasterVolume", "44"));
    expect("unconfirmed live value remains active",
           mdkr_video_config_current()
                   ->values[MDKR_AUDIO_MASTER_VOLUME].number == 44.0f &&
               mdkr_video_config_desired()
                   ->values[MDKR_AUDIO_MASTER_VOLUME].number == 44.0f);
    expect("durability returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("durability artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d durability failure(s)\n", s_failures);
        return 1;
    }
    printf("Video config durability tests passed\n");
    return 0;
}

static void write_launcher_interleaving_commit(void) {
    static const char text[] =
        "[Future]\nOwnedByOtherProcess=keep-me\n\n"
        "[Video]\nMode=restored\n\n"
        "[Window]\nMode=windowed\n\n[Audio]\nMasterVolume=37\n";
    FILE *file = fopen("mdkr64.ini", "wb");
    if (file == NULL || fwrite(text, 1, strlen(text), file) != strlen(text) ||
        fclose(file) != 0) {
        fprintf(stderr, "FAIL launcher interleaving writer\n");
        s_failures++;
    }
}

static int run_launcher_merge_case(void) {
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    char text[32768];
    char *argv[] = {
        "mdkr-video-runtime-test", "--video-launch-set", "Window.Mode=fullscreen",
        "--video-launch-persist"
    };

    s_failures = 0;
    expect("launcher-merge temporary directory created",
           mdkr_test_make_temp_directory(temporary, sizeof(temporary),
                                         "mdkr-launcher-merge"));
    if (s_failures != 0) return 1;
    expect("launcher-merge original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("launcher-merge entered temporary directory", chdir(temporary) == 0);
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("launcher-merge config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    expect("launcher-merge initial config written", write_initial_config());
    /* This deterministic interleaving commits another process's complete
     * replacement after launcher startup read but before its lock acquisition. */
    mdkr_video_test_set_launcher_persist_hook(write_launcher_interleaving_commit);
    mdkr_video_config_init(4, argv);
    mdkr_video_test_set_launcher_persist_hook(NULL);
    expect("launcher merge retained independent committed audio edit",
           read_config(text, sizeof(text)) &&
               config_has_entry(text, "Audio.MasterVolume", "37"));
    expect("launcher merge persisted its own requested key",
           config_has_entry(text, "Window.Mode", "fullscreen"));
    expect("launcher merge uses launcher intent for concurrently changed key",
           !config_has_entry(text, "Window.Mode", "windowed"));
    expect("launcher merge retained unknown concurrent key",
           strstr(text, "OwnedByOtherProcess=keep-me") != NULL);
    expect("launcher-merge returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("launcher-merge artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d launcher-merge failure(s)\n", s_failures);
        return 1;
    }
    printf("Launcher merge transaction tests passed\n");
    return 0;
}

/*
 * A browser-launcher session that was NOT asked to persist. Its choices are
 * invocation-only in exactly the way a preset flag is: authoritative for this
 * run, never a durable player decision. An unrelated in-game settings write
 * must therefore keep them staged and keep them out of the file -- the two
 * halves of the same rule, proven in both directions here.
 */
static int run_launcher_session_case(void) {
    char temporary[2048];
    char original[2048];
    char config_path[2300];
    char text[32768];
    char *argv[] = {
        "mdkr-video-runtime-test",
        "--video-launch-mode", "remastered",
    };

    s_failures = 0;
    expect("launcher-session temporary directory created",
           mdkr_test_make_temp_directory(temporary, sizeof(temporary),
                                         "mdkr-launcher-session"));
    if (s_failures != 0) return 1;
    expect("launcher-session original cwd captured",
           getcwd(original, sizeof(original)) != NULL);
    expect("launcher-session entered temporary directory",
           chdir(temporary) == 0);
    for (size_t i = 0;
         i < sizeof(s_config_env_names) / sizeof(s_config_env_names[0]); i++) {
        (void)mdkr_test_env_unset(s_config_env_names[i]);
    }
    snprintf(config_path, sizeof(config_path), "%s/mdkr64.ini", temporary);
    expect("launcher-session config path override set",
           mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH", config_path, 1) == 0);
    expect("launcher-session initial config written", write_initial_config());

    mdkr_video_config_init(3, argv);
    mdkr_video_config_publish();
    expect("launcher mode is active without --video-launch-persist",
           mdkr_video_config_current()->mode == MDKR_VIDEO_MODE_REMASTERED &&
               mdkr_video_config_current()
                       ->values[MDKR_VIDEO_MODE].source ==
                   MDKR_VIDEO_SOURCE_LAUNCHER);
    expect("a launcher Pure/Remastered choice is not a read-only session",
           !mdkr_video_config_is_readonly());
    expect("unrelated comfort write applies live",
           mdkr_video_config_runtime_set(MDKR_AUDIO_MASTER_VOLUME, "44") ==
               MDKR_VIDEO_RUNTIME_LIVE);

    expect("launcher-ranked mode survives an unrelated settings write",
           mdkr_video_config_desired()->mode == MDKR_VIDEO_MODE_REMASTERED &&
               mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_MODE].source ==
                   MDKR_VIDEO_SOURCE_LAUNCHER);
    expect("launcher-ranked preset values survive that write",
           mdkr_video_config_desired()
                   ->values[MDKR_VIDEO_REMASTER_FX].number == 1.0f &&
               mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_REMASTER_FX].source ==
                   MDKR_VIDEO_SOURCE_LAUNCHER);
    expect("an unpersisted launcher session raises no phantom restart",
           mdkr_video_config_restart_pending() == 0);

    expect("launcher-session persistence is readable",
           read_config(text, sizeof(text)));
    expect("the comfort value the player changed is written",
           config_has_entry(text, "Audio.MasterVolume", "44"));
    expect("an unpersisted launcher mode is never baked to disk",
           config_has_entry(text, "Video.Mode", "restored") &&
               !config_has_entry(text, "Video.Mode", "remastered"));
    expect("an unpersisted launcher preset value is never baked to disk",
           !config_has_entry(text, "Video.RemasterFX", "1"));
    expect("launcher-session keeps unknown settings",
           strstr(text, "OwnedByNewerBuild=keep-me") != NULL);

    /* The same key, chosen deliberately in-game, outranks the launcher seed and
     * IS durable. Without this the "never bake" rule would be indistinguishable
     * from "this key can never be saved". */
    expect("an in-game change to a launcher-seeded key applies",
           mdkr_video_runtime_result_applied(
               mdkr_video_config_runtime_set(MDKR_VIDEO_MODE, "restored")));
    expect("launcher-session persistence re-read",
           read_config(text, sizeof(text)));
    expect("a deliberate in-game mode change is written",
           config_has_entry(text, "Video.Mode", "restored"));

    expect("launcher-session returned to original cwd", chdir(original) == 0);
    remove_config_artifacts(temporary);
    expect("launcher-session artifacts cleaned", rmdir(temporary) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d launcher-session failure(s)\n", s_failures);
        return 1;
    }
    printf("Launcher session invocation-rank tests passed\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--deferred-apply-case")) {
        return run_deferred_apply_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--pure-comfort-case")) {
        return run_pure_comfort_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--corrupt-handoff-case")) {
        return run_corrupt_handoff_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--embedded-nul-case")) {
        return run_embedded_nul_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--durability-case")) {
        return run_durability_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--launcher-merge-case")) {
        return run_launcher_merge_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--launcher-session-case")) {
        return run_launcher_session_case();
    }
    return run_primary_case();
}
