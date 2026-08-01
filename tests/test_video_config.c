/*
 * test_video_config.c — ROM-free unit tests for the video config core.
 *
 * The pure half of platform/video_config.c (schema, defaults, presets,
 * precedence, resolution) reads no globals and no environment, so every rule
 * below is exercised without a ROM, a window or a GPU. Same seam
 * tests/test_display_config.c uses.
 */
#include "config_ini.h"
#include "video_config.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_int(const char *name, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-40s actual=%d expected=%d\n", name, actual, expected);
        s_failures++;
    }
}

static void test_schema(void) {
    const MdkrVideoSchema *s = mdkr_video_schema(MDKR_VIDEO_RENDER_SCALE);
    expect_true("render scale schema exists", s != NULL);
    expect_true("render scale name", s != NULL && !strcmp(s->name, "Video.RenderScale"));
    expect_true("render scale env", s != NULL && !strcmp(s->env, "MDKR_RENDER_SCALE"));
    expect_int("render scale type", s ? (int) s->type : -1, MDKR_VIDEO_TYPE_FLOAT);
    expect_int("render scale scope", s ? (int) s->scope : -1, MDKR_VIDEO_SCOPE_LIVE);
    expect_true("render scale min", s != NULL && s->min == 1.0f);
    expect_true("render scale max", s != NULL && s->max == 4.0f);

    expect_int("lookup by name", (int) mdkr_video_key_from_name("Video.Mipmaps"),
               MDKR_VIDEO_MIPMAPS);
    expect_int("lookup is case-insensitive", (int) mdkr_video_key_from_name("video.mipmaps"),
               MDKR_VIDEO_MIPMAPS);
    expect_int("unknown key", (int) mdkr_video_key_from_name("Video.Nope"),
               MDKR_VIDEO_KEY_COUNT);
    expect_int("null key", (int) mdkr_video_key_from_name(NULL), MDKR_VIDEO_KEY_COUNT);
    expect_int("gameplay FOV key", (int) mdkr_video_key_from_name("Video.GameplayFOV"),
               MDKR_VIDEO_GAMEPLAY_FOV);
    expect_int("simulation cadence key",
               (int) mdkr_video_key_from_name("Gameplay.SimulationCadence"),
               MDKR_VIDEO_SIMULATION_CADENCE);
    expect_int("mode key", (int) mdkr_video_key_from_name("Video.Mode"),
               MDKR_VIDEO_MODE);
    expect_int("frame limit key",
               (int) mdkr_video_key_from_name("Video.FrameLimit"),
               MDKR_VIDEO_FRAME_LIMIT);
    expect_int("motion smoothing key",
               (int) mdkr_video_key_from_name("Video.MotionSmoothing"),
               MDKR_VIDEO_MOTION_SMOOTHING);

    /*
     * RESTART, not LIVE. Both seams latch (present_pace_lazy_init's
     * s_presentFields, gfx_start_frame's walk-entry capture gated on
     * present_sched_replay_armed) so a mid-run flip is dead cost engaging and
     * a stale-segment-table replay disengaging. See video_config.c's rows.
     */
    s = mdkr_video_schema(MDKR_VIDEO_FRAME_LIMIT);
    expect_true("frame limit env seam",
                s != NULL && !strcmp(s->env, "MDKR_PRESENT_RATE"));
    expect_int("frame limit scope is RESTART",
               s ? (int) s->scope : -1, MDKR_VIDEO_SCOPE_RESTART);
    s = mdkr_video_schema(MDKR_VIDEO_MOTION_SMOOTHING);
    expect_true("motion smoothing env seam",
                s != NULL && !strcmp(s->env, "MDKR_PRESENT_SMOOTHING"));
    expect_int("motion smoothing scope is RESTART",
               s ? (int) s->scope : -1, MDKR_VIDEO_SCOPE_RESTART);

    expect_true("out-of-range key is NULL", mdkr_video_schema(MDKR_VIDEO_KEY_COUNT) == NULL);
    expect_true("negative key is NULL", mdkr_video_schema((MdkrVideoKey) -1) == NULL);

    /* Every key must have a complete schema entry. */
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const MdkrVideoSchema *e = mdkr_video_schema((MdkrVideoKey) i);
        expect_true("schema entry non-null", e != NULL);
        expect_true("schema name non-empty", e != NULL && e->name[0] != '\0');
        expect_true("schema env non-empty", e != NULL && e->env[0] != '\0');
        expect_true("schema label non-empty", e != NULL && e->label != NULL &&
                                              e->label[0] != '\0');
        expect_true("schema help non-empty", e != NULL && e->help != NULL &&
                                             e->help[0] != '\0');
        expect_true("schema round-trips by name",
                    e != NULL && (int) mdkr_video_key_from_name(e->name) == i);
    }
}

static void test_presets(void) {
    MdkrVideoConfig cfg;

    mdkr_video_config_defaults(&cfg);
    expect_int("default mode is restored", (int) cfg.mode, MDKR_VIDEO_MODE_RESTORED);
    /* Supersampling is on in the enhanced modes and off in Pure, which is the
     * reference presentation. */
    expect_true("supersampling on by default",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 2.0f);
    expect_true("default mode value is explicit",
                !strcmp(cfg.values[MDKR_VIDEO_MODE].text, "restored"));
    expect_true("default FOV is authored",
                !strcmp(cfg.values[MDKR_VIDEO_GAMEPLAY_FOV].text, "authored"));
    expect_true("default simulation cadence is original",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "original"));
    /* spec §11's example ini shows FrameLimit=60 as the eventual shipped
     * default; today only camera-only interpolation has landed (Wave B slice
     * 2), so the conservative default stays "original" until promotion. */
    expect_true("default frame limit is original",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "original"));
    expect_true("default motion smoothing is interpolate",
                !strcmp(cfg.values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                        "interpolate"));
    expect_true("defaults are DEFAULT-sourced",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].source == MDKR_VIDEO_SOURCE_DEFAULT);

    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_PURE);
    expect_true("pure render scale", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 1.0f);
    expect_true("pure msaa", cfg.values[MDKR_VIDEO_MSAA].number == 0.0f);
    expect_true("pure aniso", cfg.values[MDKR_VIDEO_ANISOTROPY].number == 1.0f);
    expect_true("pure mipmaps off", cfg.values[MDKR_VIDEO_MIPMAPS].number == 0.0f);
    expect_true("pure remasterfx off", cfg.values[MDKR_VIDEO_REMASTER_FX].number == 0.0f);
    expect_true("pure pins texture pack empty",
                cfg.values[MDKR_VIDEO_TEXTURE_PACK].text[0] == '\0');
    expect_true("preset marks source",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].source == MDKR_VIDEO_SOURCE_PRESET);

    /* The counter-intuitive pair: Pure keeps the widescreen POLICY engaged and
     * gets authentic framing from the aspect. Widescreen=0 would stretch. */
    expect_true("pure keeps widescreen policy on",
                cfg.values[MDKR_VIDEO_WIDESCREEN].number == 1.0f);
    expect_true("pure pins 4:3 aspect",
                !strcmp(cfg.values[MDKR_VIDEO_ASPECT].text, "4:3"));
    expect_true("pure pins authored FOV",
                !strcmp(cfg.values[MDKR_VIDEO_GAMEPLAY_FOV].text, "authored"));
    expect_true("pure leaves default simulation unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "original"));
    expect_true("pure leaves frame limit unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "original"));
    expect_true("pure leaves motion smoothing unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                        "interpolate"));

    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_RESTORED);
    expect_true("restored widescreen on",
                cfg.values[MDKR_VIDEO_WIDESCREEN].number == 1.0f);
    expect_true("restored aspect follows window",
                !strcmp(cfg.values[MDKR_VIDEO_ASPECT].text, "auto"));
    expect_true("restored render scale", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 2.0f);
    expect_true("restored aniso", cfg.values[MDKR_VIDEO_ANISOTROPY].number == 8.0f);
    expect_true("restored mipmaps on", cfg.values[MDKR_VIDEO_MIPMAPS].number == 1.0f);
    expect_true("restored remasterfx off", cfg.values[MDKR_VIDEO_REMASTER_FX].number == 0.0f);

    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_REMASTERED);
    expect_true("remastered aniso", cfg.values[MDKR_VIDEO_ANISOTROPY].number == 16.0f);
    expect_true("remastered remasterfx on", cfg.values[MDKR_VIDEO_REMASTER_FX].number == 1.0f);
    expect_true("remastered leaves default simulation unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "original"));
    expect_true("remastered leaves frame limit unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "original"));
    expect_true("remastered leaves motion smoothing unchanged",
                !strcmp(cfg.values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                        "interpolate"));

    /* Positive control: a resolved FrameLimit=60/MotionSmoothing=off survives
     * every preset switch, exactly like SimulationCadence=enhanced below --
     * proving "presets never touch these" isn't vacuously true because nothing
     * ever tried to set them to something other than the default. */
    mdkr_video_config_defaults(&cfg);
    expect_int("file may request FrameLimit=60",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "60",
                   MDKR_VIDEO_SOURCE_FILE), 1);
    expect_int("file may request MotionSmoothing=off",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_MOTION_SMOOTHING, "off",
                   MDKR_VIDEO_SOURCE_FILE), 1);
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_PURE);
    expect_true("pure preserves resolved frame limit",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "60"));
    expect_true("pure preserves resolved motion smoothing",
                !strcmp(cfg.values[MDKR_VIDEO_MOTION_SMOOTHING].text, "off"));
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_RESTORED);
    expect_true("restored preserves resolved frame limit",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "60"));
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_REMASTERED);
    expect_true("remastered preserves resolved frame limit",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "60"));
    expect_true("remastered preserves resolved motion smoothing",
                !strcmp(cfg.values[MDKR_VIDEO_MOTION_SMOOTHING].text, "off"));

    mdkr_video_config_defaults(&cfg);
    expect_int("file may request enhanced simulation",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_SIMULATION_CADENCE, "enhanced",
                   MDKR_VIDEO_SOURCE_FILE), 1);
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_RESTORED);
    expect_true("restored preserves enhanced simulation",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "enhanced"));
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_PURE);
    expect_true("pure preserves enhanced simulation",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "enhanced"));
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_REMASTERED);
    expect_true("remastered preserves enhanced simulation",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "enhanced"));

    /* Restored/Remastered do not pin the pack, so a resolved value survives. */
    snprintf(cfg.values[MDKR_VIDEO_TEXTURE_PACK].text,
             sizeof(cfg.values[MDKR_VIDEO_TEXTURE_PACK].text), "%s", "/tmp/pack");
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_RESTORED);
    expect_true("restored leaves the pack alone",
                !strcmp(cfg.values[MDKR_VIDEO_TEXTURE_PACK].text, "/tmp/pack"));
    mdkr_video_config_apply_preset(&cfg, MDKR_VIDEO_MODE_PURE);
    expect_true("pure clears a set pack",
                cfg.values[MDKR_VIDEO_TEXTURE_PACK].text[0] == '\0');

    expect_int("mode by name pure",
               mdkr_video_mode_from_name("pure"), MDKR_VIDEO_MODE_PURE);
    expect_int("mode by name remastered",
               mdkr_video_mode_from_name("REMASTERED"), MDKR_VIDEO_MODE_REMASTERED);
    expect_int("mode by name unknown", mdkr_video_mode_from_name("shiny"), -1);
    expect_int("mode by name custom",
               mdkr_video_mode_from_name("custom"), MDKR_VIDEO_MODE_CUSTOM);
    expect_int("mode by name null", mdkr_video_mode_from_name(NULL), -1);
}

static void test_precedence(void) {
    MdkrVideoConfig cfg;
    mdkr_video_config_defaults(&cfg);

    expect_int("file set applies",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "3",
                                     MDKR_VIDEO_SOURCE_FILE), 1);
    expect_true("file value stored", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 3.0f);

    expect_int("preset overrides file",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "1",
                                     MDKR_VIDEO_SOURCE_PRESET), 1);
    expect_true("preset value stored", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 1.0f);

    expect_int("env overrides preset",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "2",
                                     MDKR_VIDEO_SOURCE_ENV), 1);
    expect_int("cli overrides env",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "4",
                                     MDKR_VIDEO_SOURCE_CLI), 1);
    expect_true("cli value stored", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 4.0f);

    /* The invariant: a lower-ranked source never overwrites a higher one. */
    expect_int("file cannot override cli",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "1",
                                     MDKR_VIDEO_SOURCE_FILE), 0);
    expect_true("cli value survived", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 4.0f);
    expect_int("preset cannot override cli",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "1",
                                     MDKR_VIDEO_SOURCE_PRESET), 0);

    /* Same-rank reapplication is allowed (live changes re-publish). */
    expect_int("same source may reapply",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "3",
                                     MDKR_VIDEO_SOURCE_CLI), 1);

    /* Clamping to schema range. */
    mdkr_video_config_defaults(&cfg);
    expect_int("above max is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "99",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("below min is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_ANISOTROPY, "0",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("garbage is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_MSAA, "banana",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("trailing garbage is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_MSAA, "4x",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("empty is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_MSAA, "",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("fractional int is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_MSAA, "2.5",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("float accepts fractional",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "1.5",
                                     MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("null value is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_MSAA, NULL,
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("bad key is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_KEY_COUNT, "1",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("bad aspect is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_ASPECT, "wide",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("bad aspect ratio is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_ASPECT, "16:0",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("valid aspect ratio applies",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_ASPECT, "21:9",
                                     MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("bad FOV is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_GAMEPLAY_FOV, "141",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("authored FOV applies",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_GAMEPLAY_FOV, "authored",
                                     MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("enhanced simulation applies",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_SIMULATION_CADENCE, "enhanced",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("invalid simulation rejected",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_SIMULATION_CADENCE, "60",
                   MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("frame limit accepts original",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "original",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("frame limit accepts 60",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "60",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("frame limit accepts 120",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "120",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("frame limit accepts display",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "display",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("frame limit accepts uncapped",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "uncapped",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("frame limit rejects a low numeric cap",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "29",
                   MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("frame limit rejects a cap above the policy bound",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_FRAME_LIMIT, "1001",
                   MDKR_VIDEO_SOURCE_CLI), 0);
    expect_int("motion smoothing accepts interpolate",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_MOTION_SMOOTHING, "interpolate",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("motion smoothing accepts off",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_MOTION_SMOOTHING, "off",
                   MDKR_VIDEO_SOURCE_CLI), 1);
    expect_int("motion smoothing rejects on",
               mdkr_video_config_set(
                   &cfg, MDKR_VIDEO_MOTION_SMOOTHING, "on",
                   MDKR_VIDEO_SOURCE_CLI), 0);

    /* Strings bypass numeric range but honor capacity and precedence. */
    expect_int("string set applies",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_TEXTURE_PACK, "/tmp/pack",
                                     MDKR_VIDEO_SOURCE_ENV), 1);
    expect_true("string stored",
                !strcmp(cfg.values[MDKR_VIDEO_TEXTURE_PACK].text, "/tmp/pack"));
    expect_int("string file cannot override env",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_TEXTURE_PACK, "/other",
                                     MDKR_VIDEO_SOURCE_FILE), 0);
    expect_true("string survived",
                !strcmp(cfg.values[MDKR_VIDEO_TEXTURE_PACK].text, "/tmp/pack"));
    expect_int("newline injection is rejected",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_TEXTURE_PACK,
                                     "pack\nVideo.Mipmaps=0",
                                     MDKR_VIDEO_SOURCE_CLI), 0);
}

static void test_ini(void) {
    ConfigIniEntry entries[16];
    ConfigIniEntry again[16];
    char oversized[CONFIG_INI_KEY_MAX + CONFIG_INI_VALUE_MAX + 32];
    int count = 0;
    int again_count = 0;
    char out[2048];

    const char *text =
        "# a comment\n"
        "[Video]\n"
        "RenderScale = 2.0\n"
        "Mipmaps=1\n"
        "\n"
        "; another comment\n"
        "FutureSetting = whatever\n";

    expect_int("parse ok", config_ini_parse(text, entries, 16, &count), 1);
    expect_int("entry count", count, 3);
    expect_true("first key", !strcmp(entries[0].key, "Video.RenderScale"));
    expect_true("first value", !strcmp(entries[0].value, "2.0"));
    expect_true("second key", !strcmp(entries[1].key, "Video.Mipmaps"));
    expect_true("second value", !strcmp(entries[1].value, "1"));
    expect_true("unknown key preserved", !strcmp(entries[2].key, "Video.FutureSetting"));
    expect_true("unknown value preserved", !strcmp(entries[2].value, "whatever"));

    expect_int("serialize ok", config_ini_serialize(entries, count, out, sizeof(out)), 1);

    /* Round-trip must be a fixed point: parse(serialize(x)) == x. */
    expect_int("reparse ok", config_ini_parse(out, again, 16, &again_count), 1);
    expect_int("round-trip count", again_count, count);
    for (int i = 0; i < count && i < again_count; i++) {
        expect_true("round-trip key", !strcmp(again[i].key, entries[i].key));
        expect_true("round-trip value", !strcmp(again[i].value, entries[i].value));
    }

    /* Overflow is reported, not silently truncated. */
    expect_int("too many entries reported",
               config_ini_parse(text, entries, 1, &count), 0);

    /* Degenerate inputs. */
    expect_int("empty text parses to zero", config_ini_parse("", again, 16, &again_count), 1);
    expect_int("empty count", again_count, 0);
    expect_int("null text rejected", config_ini_parse(NULL, again, 16, &again_count), 0);
    expect_int("serialize into tiny buffer fails",
               config_ini_serialize(entries, 3, out, 4), 0);
    expect_int(
        "negative serialize count is rejected",
        config_ini_serialize(entries, -1, out, sizeof(out)),
        0);

    memset(oversized, 'k', sizeof(oversized));
    oversized[sizeof(oversized) - 3] = '=';
    oversized[sizeof(oversized) - 2] = '1';
    oversized[sizeof(oversized) - 1] = '\0';
    expect_int(
        "overlong physical line is rejected",
        config_ini_parse(oversized, again, 16, &again_count),
        0);
    expect_int("overlong line publishes no partial count", again_count, 0);

    memset(oversized, 's', CONFIG_INI_KEY_MAX + 2);
    oversized[0] = '[';
    oversized[CONFIG_INI_KEY_MAX + 1] = ']';
    oversized[CONFIG_INI_KEY_MAX + 2] = '\n';
    oversized[CONFIG_INI_KEY_MAX + 3] = 'K';
    oversized[CONFIG_INI_KEY_MAX + 4] = '=';
    oversized[CONFIG_INI_KEY_MAX + 5] = 'V';
    oversized[CONFIG_INI_KEY_MAX + 6] = '\0';
    expect_int(
        "overlong section is rejected",
        config_ini_parse(oversized, again, 16, &again_count),
        0);

    oversized[0] = 'K';
    oversized[1] = '=';
    memset(oversized + 2, 'v', CONFIG_INI_VALUE_MAX);
    oversized[CONFIG_INI_VALUE_MAX + 2] = '\0';
    expect_int(
        "overlong value is rejected",
        config_ini_parse(oversized, again, 16, &again_count),
        0);

    snprintf(entries[0].key, sizeof(entries[0].key), "Future.Value");
    snprintf(entries[0].value, sizeof(entries[0].value), "kept");
    snprintf(entries[1].key, sizeof(entries[1].key), "RootValue");
    snprintf(entries[1].value, sizeof(entries[1].value), "also-kept");
    expect_int("section-to-root serialize succeeds",
               config_ini_serialize(entries, 2, out, sizeof(out)), 1);
    expect_true("serializer never emits invalid empty section",
                strstr(out, "[]") == NULL);
    expect_int("section-to-root reparses",
               config_ini_parse(out, again, 16, &again_count), 1);
    expect_true("root key survives section transition",
                again_count == 2 && !strcmp(again[0].key, "RootValue") &&
                !strcmp(again[1].key, "Future.Value"));
}

/* Fake environment so the env layer is exercised without touching the real one. */
static const char *fake_env(const char *name) {
    if (name != NULL && !strcmp(name, "MDKR_RENDER_SCALE")) {
        return "3";
    }
    if (name != NULL && !strcmp(name, "MDKR_ASPECT")) {
        return "16:10";
    }
    if (name != NULL && !strcmp(name, "MDKR_SIMULATION_CADENCE")) {
        return "enhanced";
    }
    if (name != NULL && !strcmp(name, "MDKR_PRESENT_RATE")) {
        return "60";
    }
    return NULL;
}

static void test_resolve(void) {
    MdkrVideoConfig cfg;
    ConfigIniEntry file[4];
    char *argv[12];
    int argc;

    snprintf(file[0].key, sizeof(file[0].key), "Video.RenderScale");
    snprintf(file[0].value, sizeof(file[0].value), "3");
    snprintf(file[1].key, sizeof(file[1].key), "Video.Mipmaps");
    snprintf(file[1].value, sizeof(file[1].value), "1");
    snprintf(file[2].key, sizeof(file[2].key), "Video.Mode");
    snprintf(file[2].value, sizeof(file[2].value), "restored");
    snprintf(file[3].key, sizeof(file[3].key), "Video.GameplayFOV");
    snprintf(file[3].value, sizeof(file[3].value), "75");

    /* File only. */
    mdkr_video_config_defaults(&cfg);
    argc = 1; argv[0] = "mdkr64";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_true("file applied", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 3.0f);
    expect_int("file mode applied", (int) cfg.mode, MDKR_VIDEO_MODE_RESTORED);
    expect_true("file custom FOV overlays mode",
                !strcmp(cfg.values[MDKR_VIDEO_GAMEPLAY_FOV].text, "75"));

    /* --pure beats the file. */
    mdkr_video_config_defaults(&cfg);
    argc = 2; argv[0] = "mdkr64"; argv[1] = "--pure";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("pure mode selected", (int) cfg.mode, MDKR_VIDEO_MODE_PURE);
    expect_true("preset beat file", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 1.0f);
    expect_true("pure disables mipmaps", cfg.values[MDKR_VIDEO_MIPMAPS].number == 0.0f);
    expect_true("pure resolves to 4:3",
                !strcmp(cfg.values[MDKR_VIDEO_ASPECT].text, "4:3"));

    /* Env beats the preset. */
    mdkr_video_config_defaults(&cfg);
    argc = 2; argv[0] = "mdkr64"; argv[1] = "--pure";
    mdkr_video_config_resolve(&cfg, file, 4, fake_env, argc, argv);
    expect_true("env beat preset", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 3.0f);
    expect_true("env aspect beat preset",
                !strcmp(cfg.values[MDKR_VIDEO_ASPECT].text, "16:10"));
    expect_true("env simulation beat preset",
                !strcmp(cfg.values[MDKR_VIDEO_SIMULATION_CADENCE].text,
                        "enhanced"));
    /* MDKR_PRESENT_RATE is literally present_sched.c's own seam name (see
     * video_config.c's schema comment): a raw env value it recognizes ("60")
     * resolves through the config layer exactly like any other key, at ENV
     * precedence, even though Pure's preset never pins FrameLimit itself. */
    expect_true("env frame limit resolved from the present_sched seam",
                !strcmp(cfg.values[MDKR_VIDEO_FRAME_LIMIT].text, "60"));
    expect_int("env frame limit marked ENV-sourced",
               cfg.values[MDKR_VIDEO_FRAME_LIMIT].source,
               MDKR_VIDEO_SOURCE_ENV);
    expect_true("env marked",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].source == MDKR_VIDEO_SOURCE_ENV);

    /* --video-set beats env. */
    mdkr_video_config_defaults(&cfg);
    argc = 4;
    argv[0] = "mdkr64"; argv[1] = "--pure";
    argv[2] = "--video-set"; argv[3] = "Video.RenderScale=4";
    mdkr_video_config_resolve(&cfg, file, 4, fake_env, argc, argv);
    expect_int("still pure mode", (int) cfg.mode, MDKR_VIDEO_MODE_PURE);
    expect_true("cli beat env", cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 4.0f);
    expect_true("cli marked",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].source == MDKR_VIDEO_SOURCE_CLI);

    /* Last mode flag wins. */
    mdkr_video_config_defaults(&cfg);
    argc = 3; argv[0] = "mdkr64"; argv[1] = "--pure"; argv[2] = "--restored";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("last mode flag wins", (int) cfg.mode, MDKR_VIDEO_MODE_RESTORED);

    /* Malformed --video-set is ignored, not fatal. */
    mdkr_video_config_defaults(&cfg);
    argc = 3; argv[0] = "mdkr64"; argv[1] = "--video-set"; argv[2] = "nonsense";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("malformed video-set leaves file mode intact",
               (int) cfg.mode, MDKR_VIDEO_MODE_RESTORED);

    /* Pure sessions are read-only for config. */
    mdkr_video_config_defaults(&cfg);
    argc = 2; argv[0] = "mdkr64"; argv[1] = "--pure";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("pure is read-only", mdkr_video_config_readonly_for(&cfg), 1);
    mdkr_video_config_defaults(&cfg);
    argc = 1; argv[0] = "mdkr64";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("non-pure is writable", mdkr_video_config_readonly_for(&cfg), 0);

    /* Browser launcher values persist like user choices but remain below the
     * in-game runtime layer. A launcher Pure choice is not the read-only
     * diagnostic contract of the native --pure flag. */
    mdkr_video_config_defaults(&cfg);
    argc = 5;
    argv[0] = "mdkr64";
    argv[1] = "--video-launch-mode";
    argv[2] = "pure";
    argv[3] = "--video-launch-set";
    argv[4] = "Video.RenderScale=3";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_int("launcher mode selected", (int) cfg.mode, MDKR_VIDEO_MODE_PURE);
    expect_true("launcher render scale selected",
                cfg.values[MDKR_VIDEO_RENDER_SCALE].number == 3.0f);
    expect_int("launcher source recorded",
               cfg.values[MDKR_VIDEO_RENDER_SCALE].source,
               MDKR_VIDEO_SOURCE_LAUNCHER);
    expect_int("launcher Pure remains writable",
               mdkr_video_config_readonly_for(&cfg), 0);
    expect_int("in-game layer beats launcher",
               mdkr_video_config_set(&cfg, MDKR_VIDEO_RENDER_SCALE, "4",
                                     MDKR_VIDEO_SOURCE_RUNTIME), 1);

    /* The legacy direct flags also participate in the schema so the menu sees
     * their effective value and correctly reports them as locked. */
    mdkr_video_config_defaults(&cfg);
    argc = 6;
    argv[0] = "mdkr64";
    argv[1] = "--aspect";
    argv[2] = "21:9";
    argv[3] = "--fov";
    argv[4] = "75";
    argv[5] = "--legacy-stretch";
    mdkr_video_config_resolve(&cfg, file, 4, NULL, argc, argv);
    expect_true("direct aspect resolved in schema",
                !strcmp(cfg.values[MDKR_VIDEO_ASPECT].text, "21:9"));
    expect_true("direct FOV resolved in schema",
                !strcmp(cfg.values[MDKR_VIDEO_GAMEPLAY_FOV].text, "75"));
    expect_true("legacy stretch resolved in schema",
                cfg.values[MDKR_VIDEO_WIDESCREEN].number == 0.0f);
    expect_int("direct aspect is CLI-ranked",
               cfg.values[MDKR_VIDEO_ASPECT].source, MDKR_VIDEO_SOURCE_CLI);
}

int main(void) {
    test_schema();
    test_presets();
    test_precedence();
    test_ini();
    test_resolve();
    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("all video_config tests passed\n");
    return 0;
}
