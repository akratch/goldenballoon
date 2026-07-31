/*
 * ROM/GPU-free transaction tests for video_config_runtime.c.
 *
 * The test runs in a private temporary working directory because native video
 * settings intentionally live beside the executable as mdkr64.ini. It proves
 * atomic rewrite semantics, unknown-key preservation, CLI locking, live
 * publication, and restart staging without touching a player's real config.
 */
#include "display_config.h"
#include "present_sched.h"
#include "video_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
/* The Windows CRT has no POSIX unsetenv(). Assigning an EMPTY value with
 * _putenv is the documented way to REMOVE a variable there — which is what
 * this test needs, since getenv() must come back NULL for the env-override
 * cases to be exercised at all. */
static void mdkr_test_unsetenv(const char *name) {
    char assignment[256];
    snprintf(assignment, sizeof(assignment), "%s=", name);
    (void) _putenv(assignment);
}
#define unsetenv(name) mdkr_test_unsetenv(name)
#endif

float g_pcRenderScale;
int g_pcMsaaSamples;
int g_pcTextureAnisotropy;
int g_pcMipmaps;
int g_pcRemasterFX;
int g_pcGradePresets;
int g_pcTonemap;
int g_pcPerPixelLight;
int g_pcSunShadow;
int g_pcSunShadowRes;
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
static int s_presentFrameLimitCalls;
static int s_presentSmoothingCalls;

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

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
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

int main(void) {
    char temporary[] = "/tmp/mdkr-video-runtime-XXXXXX";
    char original[2048];
    char text[32768];
    char *argv[] = {
        "mdkr-video-runtime-test",
        "--video-set",
        "Video.Mipmaps=0",
    };
    static const char *const env_names[] = {
        "MDKR_REMASTER_FX", "MDKR_WIDESCREEN", "MDKR_ASPECT",
        "MDKR_RENDER_SCALE", "MDKR_MSAA", "MDKR_ANISOTROPY",
        "MDKR_MIPMAPS", "MDKR_TEXTURE_PACK", "MDKR_FOV",
        "MDKR_VIDEO_MODE",
    };

    expect("temporary directory created", mkdtemp(temporary) != NULL);
    if (s_failures != 0) return 1;
    expect("original cwd captured", getcwd(original, sizeof(original)) != NULL);
    expect("entered temporary directory", chdir(temporary) == 0);
    for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++) {
        unsetenv(env_names[i]);
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
    expect("CLI setting reports locked",
           mdkr_video_config_runtime_locked(MDKR_VIDEO_MIPMAPS));
    expect("locked setting rejected",
           mdkr_video_config_runtime_set(MDKR_VIDEO_MIPMAPS, "1") ==
               MDKR_VIDEO_RUNTIME_LOCKED);

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

    /*
     * FrameLimit/MotionSmoothing are SCOPE_RESTART (ship review): the present
     * pacer latches its period on the first present and the replay's
     * walk-entry capture is armed once, so an in-game flip either buys nothing
     * or leaves the already-latched subloop replaying a stale segment table.
     * An in-game edit must therefore STAGE -- write the file, report RESTART,
     * and leave the running engine's cached seam value exactly where it was.
     * The push into present_sched still happens, at boot, from publish().
     */
    expect("frame limit stages for restart",
           mdkr_video_config_runtime_set(MDKR_VIDEO_FRAME_LIMIT, "60") ==
               MDKR_VIDEO_RUNTIME_RESTART);
    expect("staged frame limit did not reach present_sched",
           s_presentFrameLimitCalls == 0);
    expect("frame limit restart is pending",
           mdkr_video_config_restart_pending());
    expect("motion smoothing stages for restart",
           mdkr_video_config_runtime_set(MDKR_VIDEO_MOTION_SMOOTHING, "off") ==
               MDKR_VIDEO_RUNTIME_RESTART);
    expect("staged motion smoothing did not reach present_sched",
           s_presentSmoothingCalls == 0);
    /* ...and the staged values are what a restart would resolve. */
    expect("staged frame limit is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_FRAME_LIMIT].text, "60"));
    expect("staged motion smoothing is in the desired config",
           !strcmp(mdkr_video_config_desired()
                       ->values[MDKR_VIDEO_MOTION_SMOOTHING].text, "off"));

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
    expect("temporary CLI value not baked into config",
           strstr(text, "Mipmaps=1") != NULL);
    expect("atomic temporary file removed", access("mdkr64.ini.tmp", F_OK) != 0);

    expect("returned to original cwd", chdir(original) == 0);
    {
        char path[2300];
        snprintf(path, sizeof(path), "%s/mdkr64.ini", temporary);
        unlink(path);
        snprintf(path, sizeof(path), "%s/mdkr64.ini.tmp", temporary);
        unlink(path);
    }
    rmdir(temporary);

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("all video_config runtime tests passed\n");
    return 0;
}
