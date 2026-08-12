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
#include "fs_utf8.h"
#include "audio_volume.h"

#include "display_config.h"
#include "fast3d/gfx_mipgen.h"
#include "fast3d/gfx_font_outline.h"
#include "fast3d/gfx_uniforms.h"
/* The two receivers of Content.PacksEnabled. Taken as headers, not as a
 * platform_os.h round trip: publish() is the single place a LIVE key reaches
 * its receiver, and these are receivers exactly like audio_volume.h below. */
#include "mod_music.h"
#include "mod_texture_store.h"
#include "present_sched.h"
#include "user_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <io.h>
#include <process.h>
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

#define MDKR_VIDEO_INI_MAX    128
#define MDKR_VIDEO_INI_TEXT_MAX 32768
#define MDKR_VIDEO_PATH_MAX   4096

static MdkrVideoConfig s_video;
static MdkrVideoConfig s_desired_video;
static ConfigIniEntry s_file_entries[MDKR_VIDEO_INI_MAX];
static int s_file_entry_count;
static int s_config_read_ok = 1;
static int s_video_initialized;
static int s_engine_handoff_completed;
static char s_video_ini_path[MDKR_VIDEO_PATH_MAX];
static unsigned long s_video_tmp_serial;
typedef enum MdkrVideoWriteResult {
    MDKR_VIDEO_WRITE_FAILED = 0,
    MDKR_VIDEO_WRITE_DURABLE,
    MDKR_VIDEO_WRITE_UNCONFIRMED
} MdkrVideoWriteResult;
static MdkrVideoWriteResult mdkr_video_write_config(const MdkrVideoConfig *config);
static MdkrVideoWriteResult mdkr_video_write_config_unlocked(
    const MdkrVideoConfig *config, int persist_launcher);

/* --------------------------------------------------------------------------
 *  Deferred apply — see video_config.h's "Deferred apply" note for the why.
 * ------------------------------------------------------------------------ */

typedef struct MdkrVideoApplyState {
    void (*apply)(void);
    /* The boundary this domain is waiting for, or -1 for "not waiting". Held
     * as the SCOPE rather than as a bool because one domain services exactly
     * one boundary and the pending record is what tells the engine which. */
    int pending_scope;
} MdkrVideoApplyState;

static MdkrVideoApplyState s_apply[MDKR_VIDEO_APPLY_DOMAIN_COUNT];

/*
 * One staged transition per key, kept ONLY so the boundary can name what it
 * changed. The authoritative new value is never stored here -- it lives in
 * s_desired_video, which the setter already updated, and the boundary commits
 * from there. Two edits before one boundary therefore apply the LATEST choice
 * rather than the first, without this record having to track supersession.
 *
 * `old_text` is what the RUNNING engine had before the FIRST of those edits: it
 * is captured from s_video at stage time and deliberately not refreshed on a
 * re-stage, because "what you are looking at now" does not change just because
 * the player moved the selector twice.
 *
 * The buffers are display-sized, not value-sized. Truncating a 1 KB value into
 * a log row is cosmetic; truncating one into the config would not be, which is
 * exactly why the config is not read back out of here.
 */
typedef struct MdkrVideoApplyRecord {
    char old_text[MDKR_VIDEO_NAME_MAX];
    char new_text[MDKR_VIDEO_NAME_MAX];
    int staged;
} MdkrVideoApplyRecord;

static MdkrVideoApplyRecord s_apply_record[MDKR_VIDEO_KEY_COUNT];

/*
 * A domain is deferred only once its owner has registered. Before that -- the
 * launcher's settings panel before Play, --video-set, every ROM-free test --
 * there is no boundary to defer to, so publish() writes the value inline
 * exactly as it always did. Staging for a boundary nobody services would be a
 * setting that silently never applies, which is the one outcome worse than
 * requiring a restart.
 */
static int mdkr_video_apply_is_deferred(MdkrVideoApplyDomain domain) {
    if (domain <= MDKR_VIDEO_APPLY_NONE ||
        domain >= MDKR_VIDEO_APPLY_DOMAIN_COUNT) {
        return 0;
    }
    return s_apply[domain].apply != NULL;
}

void mdkr_video_config_register_apply(MdkrVideoApplyDomain domain,
                                      void (*apply)(void)) {
    if (domain <= MDKR_VIDEO_APPLY_NONE ||
        domain >= MDKR_VIDEO_APPLY_DOMAIN_COUNT) {
        return;
    }
    s_apply[domain].apply = apply;
    /* Always, not only on unregister: pending_scope's zero-initialized value is
     * MDKR_VIDEO_SCOPE_LIVE, so a domain that did not explicitly clear it here
     * would fire a spurious apply at the first boundary after registration. */
    s_apply[domain].pending_scope = -1;
}

int mdkr_video_config_apply_is_pending(MdkrVideoScope boundary) {
    for (int i = 0; i < MDKR_VIDEO_APPLY_DOMAIN_COUNT; i++) {
        if (s_apply[i].apply != NULL &&
            s_apply[i].pending_scope == (int)boundary) {
            return 1;
        }
    }
    return 0;
}

/*
 * Stage `key` for its domain's boundary. Returns 1 when the value was staged
 * (so the caller must NOT publish it inline) and 0 when the key has no
 * deferred owner and should take the ordinary publish path.
 *
 * `old_text` is read from s_video BEFORE the caller updates it, and only on the
 * first stage. Numeric keys are formatted into the same field: every key in a
 * domain today is a string, but the trace row is a diagnostic contract and
 * should not acquire a hole the first time a numeric key joins one.
 */
static int mdkr_video_apply_stage(MdkrVideoKey key, const char *value) {
    const MdkrVideoApplyDomain domain = mdkr_video_key_apply_domain(key);
    const MdkrVideoSchema *schema = mdkr_video_schema(key);
    MdkrVideoApplyRecord *record;

    if (schema == NULL || !mdkr_video_apply_is_deferred(domain)) {
        return 0;
    }
    record = &s_apply_record[key];
    if (!record->staged) {
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            snprintf(record->old_text, sizeof(record->old_text), "%s",
                     s_video.values[key].text);
        } else {
            snprintf(record->old_text, sizeof(record->old_text), "%.9g",
                     (double)s_video.values[key].number);
        }
    }
    snprintf(record->new_text, sizeof(record->new_text), "%s",
             value != NULL ? value : "");
    record->staged = 1;
    s_apply[domain].pending_scope = (int)schema->scope;
    return 1;
}

/*
 * Commit `domain`'s staged transitions into the live config and announce them.
 *
 * COMMIT AND ANNOUNCE ARE THE SAME STEP because they answer the same question.
 * s_video is "what the running engine has"; the [SETTINGS-APPLY] row is the
 * externally visible statement that it changed. Doing either without the other
 * is how a log and a config disagree.
 *
 * The row is written BEFORE the domain callback runs. If the apply faults, the
 * last thing in the log is the change that was being made, which is the only
 * ordering that helps anyone reading a crash — and it is what lets the toggle
 * soak attribute a fault to a specific transition rather than to a run.
 */
static void mdkr_video_apply_flush(MdkrVideoApplyDomain domain,
                                   MdkrVideoScope boundary) {
    static const char *const kDomainNames[MDKR_VIDEO_APPLY_DOMAIN_COUNT] = {
        "none", "presentation", "camera"
    };
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        const MdkrVideoSchema *schema = mdkr_video_schema((MdkrVideoKey)i);
        if (!s_apply_record[i].staged ||
            mdkr_video_key_apply_domain((MdkrVideoKey)i) != domain) {
            continue;
        }
        /* Make the LIVE config match the DESIRED one for this key, which is
         * the whole meaning of "applied". Idempotent for a LIVE key the setter
         * already copied; load-bearing for a LEVEL key, which the setter
         * deliberately left alone so that until this moment every reader still
         * saw the value the engine was running.
         *
         * From s_desired_video rather than from the record, so two edits before
         * one boundary apply the latest -- and so the value that reaches the
         * config is the one the setter validated, never a display copy of it. A
         * key pinned above RUNTIME rank cannot reach here: the setter refuses it
         * as LOCKED, so nothing is staged for it in the first place. */
        s_video.values[i] = s_desired_video.values[i];
        fprintf(stderr,
                "[SETTINGS-APPLY] domain=%s boundary=%s key=%s old=%s new=%s\n",
                kDomainNames[domain],
                boundary == MDKR_VIDEO_SCOPE_LEVEL ? "level" : "frame",
                schema != NULL ? schema->name : "?",
                s_apply_record[i].old_text, s_apply_record[i].new_text);
        s_apply_record[i].staged = 0;
    }
    fflush(stderr);
}

int mdkr_video_config_apply_pending(MdkrVideoScope boundary) {
    int applied = 0;

    /* Domain order is the enum's, and it is the ordering guarantee callers get:
     * presentation before camera. Presentation is what invalidates the replay
     * history, so a camera apply in the same pass can never publish a sidecar
     * pose into a retained task that is about to be thrown away. Today the two
     * domains service different boundaries and cannot co-occur; the order is
     * stated anyway so that stops being an accident if a third domain lands. */
    for (int i = 0; i < MDKR_VIDEO_APPLY_DOMAIN_COUNT; i++) {
        if (s_apply[i].apply == NULL ||
            s_apply[i].pending_scope != (int)boundary) {
            continue;
        }
        s_apply[i].pending_scope = -1;
        mdkr_video_apply_flush((MdkrVideoApplyDomain)i, boundary);
        s_apply[i].apply();
        applied++;
    }
    return applied;
}

/*
 * Sources this process owns for the lifetime of ONE invocation: the
 * --pure/--restored/--remastered preset flags, the browser launcher's seeds,
 * the environment, and --video-set. Two properties follow from that and both
 * are load-bearing:
 *
 *   - re-resolving the saved file cannot reconstruct them (the rebuilt
 *     candidate in the runtime setter resolves with argc == 0), so they must be
 *     carried across a settings transaction explicitly, and
 *   - none of them is a player's durable choice, so none may be serialized.
 *
 * FILE and RUNTIME are deliberately excluded: both already live in the file the
 * candidate was rebuilt from. The launcher-persist transaction is the single
 * exception -- it exists precisely to promote LAUNCHER values to disk -- and
 * says so with its own flag rather than by widening this predicate.
 */
static int mdkr_video_source_is_invocation(MdkrVideoSource source) {
    return source == MDKR_VIDEO_SOURCE_PRESET ||
           source == MDKR_VIDEO_SOURCE_LAUNCHER ||
           source == MDKR_VIDEO_SOURCE_ENV ||
           source == MDKR_VIDEO_SOURCE_CLI;
}

#ifdef MDKR_VIDEO_RUNTIME_TESTING
static int s_test_force_directory_sync_failure;
static void (*s_test_launcher_persist_hook)(void);

void mdkr_video_test_force_directory_sync_failure(int enabled) {
    s_test_force_directory_sync_failure = enabled != 0;
}

void mdkr_video_test_set_launcher_persist_hook(void (*hook)(void)) {
    s_test_launcher_persist_hook = hook;
}
#endif

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

static const char *mdkr_video_noenv(const char *name) {
    (void)name;
    return NULL;
}

static int mdkr_video_parent_directory_sync(const char *path) {
#ifdef MDKR_VIDEO_RUNTIME_TESTING
    if (s_test_force_directory_sync_failure) {
        errno = EIO;
        return -1;
    }
#endif
    return mdkr_parent_directory_sync_utf8(path);
}

static int mdkr_video_resolve_paths(void) {
    if (!mdkr_user_video_config_path(
            s_video_ini_path, sizeof(s_video_ini_path))) {
        fprintf(stderr, "[video] config path is unavailable or too long\n");
        return 0;
    }
    return 1;
}

static long mdkr_video_process_id(void) {
#if defined(__EMSCRIPTEN__)
    /* The browser runtime has one process; the monotonically increasing
     * staging serial supplies uniqueness without importing POSIX getpid(). */
    return 1;
#elif defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int mdkr_video_open_unique_temp(char output[MDKR_VIDEO_PATH_MAX],
                                       FILE **file) {
    for (unsigned attempt = 0; attempt < 64u; ++attempt) {
        int written = snprintf(output, MDKR_VIDEO_PATH_MAX, "%s.tmp.%ld.%lu",
                               s_video_ini_path, mdkr_video_process_id(),
                               ++s_video_tmp_serial);
        if (written < 0 || (size_t)written >= MDKR_VIDEO_PATH_MAX) {
            fprintf(stderr, "[video] temporary config path is too long\n");
            return 0;
        }
        *file = mdkr_fopen_utf8(output, "wbx");
        if (*file != NULL) return 1;
        if (errno != EEXIST) break;
    }
    fprintf(stderr, "[video] could not create an exclusive staging file beside %s: %s\n",
            s_video_ini_path, strerror(errno));
    return 0;
}

static int mdkr_video_lock_acquire(MdkrFileLock *lock) {
#ifdef __EMSCRIPTEN__
    /* A page has one JS event loop and IDBFS transactions are serialized by the
     * shell. Native instances need an OS lock; the browser has no peer process
     * sharing this in-memory filesystem view. */
    lock->handle = -1;
    return mdkr_video_resolve_paths();
#else
    char lock_path[MDKR_VIDEO_PATH_MAX];
    int written;
    if (!mdkr_video_resolve_paths()) return 0;
    written = snprintf(lock_path, sizeof(lock_path), "%s.lock", s_video_ini_path);
    if (written < 0 || (size_t)written >= sizeof(lock_path) ||
        mdkr_file_lock_acquire_utf8(lock_path, lock) != 0) {
        fprintf(stderr, "[video] could not lock %s: %s\n",
                s_video_ini_path, strerror(errno));
        return 0;
    }
    return 1;
#endif
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

static int mdkr_video_read_config(ConfigIniEntry *entries, int *out_count) {
    char text[MDKR_VIDEO_INI_TEXT_MAX];
    int count = 0;
    FILE *f;

    if (entries == NULL || out_count == NULL || !mdkr_video_resolve_paths()) {
        return 0;
    }
    *out_count = 0;
    f = mdkr_fopen_utf8(s_video_ini_path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) return 1;
        fprintf(stderr, "[video] could not open %s: %s\n",
                s_video_ini_path, strerror(errno));
        return 0;
    }
    {
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        int extra = n == sizeof(text) - 1 ? fgetc(f) : EOF;
        int read_failed = ferror(f);
        if (fclose(f) != 0) read_failed = 1;
        if (read_failed || extra != EOF) {
            fprintf(stderr,
                    "[video] %s is unreadable or exceeds %u bytes; it was left unchanged\n",
                    s_video_ini_path, (unsigned)(sizeof(text) - 1u));
            return 0;
        }
        /* config_ini_parse is deliberately a C-string parser. Treat binary
         * data as damaged instead of parsing a valid prefix and later
         * overwriting an unseen suffix during a settings transaction. */
        if (memchr(text, '\0', n) != NULL) {
            fprintf(stderr,
                    "[video] %s contains a NUL byte; it was left unchanged\n",
                    s_video_ini_path);
            return 0;
        }
        text[n] = '\0';
    }
    if (!config_ini_parse(text, entries, MDKR_VIDEO_INI_MAX, &count)) {
        fprintf(stderr,
                "[video] %s has invalid or excessive configuration entries; it was left unchanged\n",
                s_video_ini_path);
        return 0;
    }
    *out_count = count;
    return 1;
}

void mdkr_video_config_init(int argc, char *const *argv) {
    int launcher_persist = 0;

    if (s_video_initialized) {
        return;
    }
    s_video_initialized = 1;
    mdkr_video_config_defaults(&s_video);

    s_config_read_ok = mdkr_video_read_config(s_file_entries, &s_file_entry_count);

    mdkr_video_config_resolve(&s_video, s_file_entries, s_file_entry_count,
                              mdkr_video_getenv, argc, argv);
    s_desired_video = s_video;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--video-launch-persist")) {
            launcher_persist = 1;
            break;
        }
    }
    if (launcher_persist) {
        if (!s_config_read_ok) {
            fprintf(stderr,
                    "[video] launcher settings were not persisted because the existing config could not be read safely\n");
            return;
        }
        if (!mdkr_video_launcher_args_valid(argc, argv)) {
            fprintf(stderr,
                    "[video] invalid launcher settings; existing config left unchanged\n");
        } else {
#ifdef MDKR_VIDEO_RUNTIME_TESTING
            if (s_test_launcher_persist_hook != NULL) {
                s_test_launcher_persist_hook();
            }
#endif
            const MdkrVideoWriteResult write =
                mdkr_video_write_config(&s_desired_video);
            if (write == MDKR_VIDEO_WRITE_FAILED) {
                fprintf(stderr,
                        "[video] launcher settings are active but could not be persisted\n");
            } else if (write == MDKR_VIDEO_WRITE_UNCONFIRMED) {
                fprintf(stderr,
                        "[video] launcher settings are active and visible, but directory durability was not confirmed\n");
            }
        }
    }
}

int mdkr_video_config_handoff_to_engine(int argc, char *const *argv) {
    MdkrVideoConfig resolved;
    ConfigIniEntry fresh_entries[MDKR_VIDEO_INI_MAX];
    int fresh_entry_count = 0;

    if (!s_video_initialized || s_engine_handoff_completed) {
        return 0;
    }

    if (mdkr_video_read_config(fresh_entries, &fresh_entry_count)) {
        memcpy(s_file_entries, fresh_entries,
               (size_t)fresh_entry_count * sizeof(s_file_entries[0]));
        s_file_entry_count = fresh_entry_count;
    } else {
        /* A damaged preference file must never become a playability failure.
         * Keep the last known-safe startup snapshot (or defaults when startup
         * also failed), layer the engine arguments normally, and leave every
         * future mutation fail-closed until the file is repaired. */
        fprintf(stderr,
                "[video] config reread failed; continuing with safe settings and leaving the file unchanged\n");
        s_config_read_ok = 0;
    }
    /* mdkr_video_config_resolve() layers values by source rank; it deliberately
     * does not manufacture its caller's base object.  The launcher and engine
     * share a process, so this second resolution must begin from the same fully
     * initialized defaults as first boot.  Otherwise indeterminate source ranks
     * can reject file/environment/CLI values and indeterminate strings can be
     * copied into the live runtime configuration. */
    mdkr_video_config_defaults(&resolved);
    mdkr_video_config_resolve(&resolved, s_file_entries, s_file_entry_count,
                              mdkr_video_getenv, argc, argv);
    s_video = resolved;
    s_desired_video = resolved;
    s_engine_handoff_completed = 1;
    return 1;
}

int mdkr_video_config_engine_session_complete(void) {
    if (!s_video_initialized || !s_engine_handoff_completed) {
        return 0;
    }
    s_engine_handoff_completed = 0;
    return 1;
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
    const int packs_enabled =
        c->values[MDKR_CONTENT_PACKS_ENABLED].number != 0.0f;
    int world_finish;

    (void)mdkr_audio_volume_publish(
        (int)c->values[MDKR_AUDIO_MASTER_VOLUME].number,
        (int)c->values[MDKR_AUDIO_MUSIC_VOLUME].number,
        (int)c->values[MDKR_AUDIO_EFFECTS_VOLUME].number);

    /*
     * "Custom content" — the settings checkbox, applied while you play.
     *
     * Ticking or clearing it changes the picture on the next frame, the same
     * way Tab does. The two are the same lever, so the rule between them is
     * the plain one: Tab is a momentary comparison you hold against whatever
     * you have chosen, and choosing again ends the comparison. If you have
     * pressed Tab to look at the original and then you change a setting, the
     * game goes back to showing you what your settings say — because the
     * settings screen is where you say what you want, and a screen that
     * silently kept overriding you would be lying about its own checkbox.
     *
     * A player who wanted the comparison back presses Tab again; a player who
     * did not gets exactly what the box in front of them shows. This is also
     * why the settings path does NOT copy Tab's "do nothing when no pack is
     * installed" guard. That guard exists so a keypress with nothing to
     * compare stays silent; here there is nothing to stay silent about, and
     * skipping the write would leave the running game disagreeing with the
     * setting the moment anything is installed. Both calls below are already
     * no-ops when the value has not moved, and both are inert with no pack.
     *
     * ON->OFF and OFF->ON are BOTH live for textures, because
     * platform_content_packs_init() scans mods/ and binds the decoded-texture
     * store unconditionally: the setting has never gated the scan, only
     * whether the store answers. So turning it back on has something to turn
     * on, and the flip costs a cache generation rather than a rescan.
     *
     * Music is honoured from the next piece of music onwards rather than
     * instantly, and mod_music.h states why: muting the sequence player is a
     * one-way redirection, so cutting a replacement off mid-track would leave
     * silence where the game's own music should be.
     *
     * Installing or removing a pack while the game runs is still a restart:
     * that is the scan, not this switch, and nothing here pretends otherwise.
     */
    mdkr_mod_texture_set_enabled(packs_enabled != 0);
    mdkr_mod_music_set_enabled(packs_enabled);

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
     * Pure is the byte-exact reference and must never redraw a glyph. The
     * preset default already resolves to 0 there, but a preset default is only
     * a default: an ini value, MDKR_HIRES_TEXT, or a CLI pair all outrank it
     * (see test_video_config.c "Env beats the preset"). Byte-exactness cannot
     * rest on precedence, so Pure is denied structurally here. Selecting
     * Restored, Remastered or Custom is how a player opts in.
     */
    g_pcHiresText         = (c->mode != MDKR_VIDEO_MODE_PURE) &&
        (int) c->values[MDKR_VIDEO_HIRES_TEXT].number;

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
    if (!mdkr_video_apply_is_deferred(MDKR_VIDEO_APPLY_PRESENTATION)) {
        mdkr_video_config_push_presentation();
    }
}

/*
 * The pacing keys' half of publish(), split out so exactly one piece of code
 * decides what present_sched is told, whoever is asking.
 *
 * publish() calls this directly while the presentation domain has no
 * registered applier — boot, --video-set, the launcher before Play, every
 * ROM-free test. Once the engine has registered one, publish() stops calling
 * it and the boundary applier does instead, AFTER it has invalidated the
 * replay history. That ordering is the whole point: pushing a new smoothing
 * value into present_sched while a retained walk entry from the other policy is
 * still valid is the stale-segment-table hazard, and it is unreachable when the
 * only caller that can run under a live engine is the one that invalidates
 * first.
 */
void mdkr_video_config_push_presentation(void) {
    const MdkrVideoConfig *c = &s_video;

    if (c->values[MDKR_VIDEO_FRAME_LIMIT].source != MDKR_VIDEO_SOURCE_DEFAULT) {
        mdkr_present_set_frame_limit(c->values[MDKR_VIDEO_FRAME_LIMIT].text);
    }
    if (c->values[MDKR_VIDEO_MOTION_SMOOTHING].source !=
        MDKR_VIDEO_SOURCE_DEFAULT) {
        mdkr_present_set_motion_smoothing(
            c->values[MDKR_VIDEO_MOTION_SMOOTHING].text);
    }
    if (c->values[MDKR_VIDEO_ALLOW_TEARING].source !=
        MDKR_VIDEO_SOURCE_DEFAULT) {
        mdkr_present_set_allow_tearing(
            c->values[MDKR_VIDEO_ALLOW_TEARING].text);
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
    int persist_launcher,
    ConfigIniEntry entries[MDKR_VIDEO_INI_MAX],
    int *out_count) {
    int count = 0;
    int preserve_pure_presentation;

    if (config == NULL || entries == NULL || out_count == NULL) {
        return 0;
    }
    /* Reachable exactly when the caller's config still carries the --pure
     * preset rank, which is what the runtime setter now preserves across a
     * settings transaction. The per-key invocation rule below already refuses
     * every individual Pure value; this states the same invariant once for the
     * whole session, so a future key that no preset pins cannot quietly become
     * the one thing a Pure session writes back. */
    preserve_pure_presentation = mdkr_video_config_readonly_for(config);

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
            if (preserve_pure_presentation &&
                !mdkr_video_key_is_player_comfort((MdkrVideoKey)i)) {
                /* `--pure` is a temporary reference session. Audio, window,
                 * controller, and haptic choices are persistent comfort
                 * exceptions; keep every presentation/gameplay value exactly
                 * as it existed instead of baking Pure into the normal setup. */
                prior = mdkr_video_last_file_entry((MdkrVideoKey)i);
                if (prior == NULL) {
                    continue;
                }
                serialized = prior->value;
            } else if (mdkr_video_source_is_invocation(value->source) &&
                       !(persist_launcher &&
                         value->source == MDKR_VIDEO_SOURCE_LAUNCHER)) {
                /* Never bake an invocation-only override into disk: a preset
                 * flag, a launcher seed this transaction was not asked to
                 * promote, an environment value, or --video-set. Whatever the
                 * file already said about the key is what stays there. */
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

static MdkrVideoWriteResult mdkr_video_write_config_unlocked(
    const MdkrVideoConfig *config, int persist_launcher) {
    ConfigIniEntry entries[MDKR_VIDEO_INI_MAX];
    char text[MDKR_VIDEO_INI_TEXT_MAX];
    char temporary[MDKR_VIDEO_PATH_MAX];
    int count = 0;
    FILE *f;

    if (!mdkr_video_build_persisted_entries(config, persist_launcher,
                                            entries, &count) ||
        !config_ini_serialize(entries, count, text, sizeof(text))) {
        fprintf(stderr, "[video] config is too large to save safely\n");
        return MDKR_VIDEO_WRITE_FAILED;
    }
#ifdef __EMSCRIPTEN__
    if (mkdir("/save", 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "[video] could not create /save: %s\n", strerror(errno));
        return MDKR_VIDEO_WRITE_FAILED;
    }
#endif
    if (!mdkr_video_open_unique_temp(temporary, &f)) {
        return MDKR_VIDEO_WRITE_FAILED;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text) ||
        mdkr_file_sync(f) != 0) {
        fprintf(stderr, "[video] could not write %s: %s\n",
                temporary, strerror(errno));
        fclose(f);
        mdkr_remove_utf8(temporary);
        return MDKR_VIDEO_WRITE_FAILED;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "[video] could not close %s: %s\n",
                temporary, strerror(errno));
        mdkr_remove_utf8(temporary);
        return MDKR_VIDEO_WRITE_FAILED;
    }
    if (mdkr_move_utf8(temporary, s_video_ini_path, 1, 1) != 0) {
        fprintf(stderr, "[video] could not replace %s: %s\n",
                s_video_ini_path, strerror(errno));
        mdkr_remove_utf8(temporary);
        return MDKR_VIDEO_WRITE_FAILED;
    }
    if (mdkr_video_parent_directory_sync(s_video_ini_path) != 0) {
        fprintf(stderr,
                "[video] warning: config replacement is visible, but directory durability was not confirmed: %s\n",
                strerror(errno));
        memcpy(s_file_entries, entries, (size_t) count * sizeof(entries[0]));
        s_file_entry_count = count;
        return MDKR_VIDEO_WRITE_UNCONFIRMED;
    }
#ifdef __EMSCRIPTEN__
    mdkr_video_schedule_persist();
#endif

    memcpy(s_file_entries, entries, (size_t) count * sizeof(entries[0]));
    s_file_entry_count = count;
    return MDKR_VIDEO_WRITE_DURABLE;
}

static MdkrVideoWriteResult mdkr_video_write_config(
    const MdkrVideoConfig *config) {
    MdkrFileLock lock = {(intptr_t)-1};
    ConfigIniEntry fresh_entries[MDKR_VIDEO_INI_MAX];
    MdkrVideoConfig merged;
    int fresh_entry_count = 0;
    MdkrVideoWriteResult written;
    if (config == NULL || !mdkr_video_lock_acquire(&lock)) {
        return MDKR_VIDEO_WRITE_FAILED;
    }
    /* A launcher process can sit at its first-run screen while another
     * instance changes settings. Start from the file committed immediately
     * before this lock, then apply only launcher-owned values; env/CLI values
     * remain invocation-local and never get serialized by this path. */
    if (!mdkr_video_read_config(fresh_entries, &fresh_entry_count)) {
        mdkr_file_lock_release(&lock);
        return MDKR_VIDEO_WRITE_FAILED;
    }
    memcpy(s_file_entries, fresh_entries,
           (size_t)fresh_entry_count * sizeof(s_file_entries[0]));
    s_file_entry_count = fresh_entry_count;
    mdkr_video_config_defaults(&merged);
    mdkr_video_config_resolve(&merged, s_file_entries, s_file_entry_count,
                              mdkr_video_noenv, 0, NULL);
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        if (config->values[key].source == MDKR_VIDEO_SOURCE_LAUNCHER) {
            merged.values[key] = config->values[key];
        }
    }
    if (config->values[MDKR_VIDEO_MODE].source ==
        MDKR_VIDEO_SOURCE_LAUNCHER) {
        merged.mode = config->mode;
    }
    /* This is the one transaction whose whole purpose is to promote launcher
     * choices to disk, so it is the one that passes persist_launcher. */
    written = mdkr_video_write_config_unlocked(&merged, 1);
    mdkr_file_lock_release(&lock);
    return written;
}

static int mdkr_video_mode_locked(void) {
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; i++) {
        if (mdkr_video_key_is_player_comfort((MdkrVideoKey)i)) {
            continue;
        }
        if (s_desired_video.values[i].source > MDKR_VIDEO_SOURCE_RUNTIME) {
            return 1;
        }
    }
    return 0;
}

int mdkr_video_config_runtime_locked(MdkrVideoKey key) {
    if ((int) key < 0 || key >= MDKR_VIDEO_KEY_COUNT) {
        return 1;
    }
    /* Explicit --pure makes the resolved session read-only. Timing-rate keys
     * are orthogonal to the art-direction preset and may therefore retain a
     * preselected non-original value; the UI states that distinction. Player
     * comfort settings remain available. */
    if (mdkr_video_config_is_readonly() &&
        !mdkr_video_key_is_player_comfort(key)) {
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
    ConfigIniEntry fresh_entries[MDKR_VIDEO_INI_MAX];
    MdkrFileLock lock = {(intptr_t)-1};
    MdkrVideoWriteResult write;
    int includes_mode = 0;
    int includes_presentation_setting = 0;
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
        includes_presentation_setting |=
            !mdkr_video_key_is_player_comfort(changes[i].key) &&
            changes[i].key != MDKR_VIDEO_MODE;
        requires_restart |= schema->scope == MDKR_VIDEO_SCOPE_RESTART;
    }

    /* Re-read while holding the cross-process sidecar lock.  The candidate is
     * therefore based on the last committed file, not this process's possibly
     * stale startup snapshot, so independent settings edits merge instead of
     * silently erasing one another. */
    if (!mdkr_video_lock_acquire(&lock)) {
        return MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    }
    if (!mdkr_video_read_config(fresh_entries, &s_file_entry_count)) {
        mdkr_file_lock_release(&lock);
        return MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    }
    memcpy(s_file_entries, fresh_entries,
           (size_t)s_file_entry_count * sizeof(s_file_entries[0]));
    mdkr_video_config_defaults(&candidate);
    mdkr_video_config_resolve(&candidate, s_file_entries, s_file_entry_count,
                              mdkr_video_getenv, 0, NULL);
    /* Runtime rewrites begin from the newest file, but every invocation-owned
     * layer belongs to this launched process and the argc == 0 resolve above
     * cannot reproduce any of it. Carry all four ranks across -- preset flags
     * and launcher seeds as much as environment and CLI -- so an unrelated
     * volume edit cannot silently demote them to whatever the file happens to
     * say. Restoring only ENV/CLI left a --pure session resolving back to its
     * file mode, which unlocked presentation, made the report disagree with the
     * running image, and raised a phantom restart-required. Nothing here
     * reaches disk: mdkr_video_build_persisted_entries() refuses every one of
     * these ranks. */
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        if (mdkr_video_source_is_invocation(
                s_desired_video.values[key].source)) {
            candidate.values[key] = s_desired_video.values[key];
            if (key == MDKR_VIDEO_MODE) candidate.mode = s_desired_video.mode;
        }
    }
    for (int i = 0; i < change_count; i++) {
        if (!mdkr_video_config_set(&candidate, changes[i].key, changes[i].value,
                                   MDKR_VIDEO_SOURCE_RUNTIME)) {
            mdkr_file_lock_release(&lock);
            return MDKR_VIDEO_RUNTIME_INVALID;
        }
    }
    if (!includes_mode && includes_presentation_setting) {
        (void) mdkr_video_config_set(&candidate, MDKR_VIDEO_MODE, "custom",
                                     MDKR_VIDEO_SOURCE_RUNTIME);
    }
    write = mdkr_video_write_config_unlocked(&candidate, 0);
    if (write == MDKR_VIDEO_WRITE_FAILED) {
        mdkr_file_lock_release(&lock);
        return MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    }
    mdkr_file_lock_release(&lock);

    s_desired_video = candidate;
    if (!includes_mode) {
        for (int i = 0; i < change_count; i++) {
            const MdkrVideoSchema *schema = mdkr_video_schema(changes[i].key);
            /*
             * Stage BEFORE the set: mdkr_video_apply_stage reads the outgoing
             * value out of s_video, and the set below is what destroys it.
             *
             * A LEVEL-scoped key is deliberately NOT copied into s_video here.
             * s_video is "what the running engine has", and until the level
             * boundary runs the engine still has the old camera policy --
             * writing it early would make the panel claim the change had landed
             * and would make mdkr_video_config_report() lie. The level applier
             * performs that copy itself, at the moment it becomes true.
             */
            const int staged =
                mdkr_video_apply_stage(changes[i].key, changes[i].value);
            if (schema->scope == MDKR_VIDEO_SCOPE_LIVE ||
                (schema->scope == MDKR_VIDEO_SCOPE_LEVEL && !staged)) {
                (void) mdkr_video_config_set(&s_video, changes[i].key,
                                             changes[i].value,
                                             MDKR_VIDEO_SOURCE_RUNTIME);
            }
        }
        if (includes_presentation_setting) {
            (void) mdkr_video_config_set(&s_video, MDKR_VIDEO_MODE, "custom",
                                         MDKR_VIDEO_SOURCE_RUNTIME);
        }
        mdkr_video_config_publish();
    }
    if (write == MDKR_VIDEO_WRITE_UNCONFIRMED) {
        return MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED;
    }
    return requires_restart ? MDKR_VIDEO_RUNTIME_RESTART
                            : MDKR_VIDEO_RUNTIME_LIVE;
}

MdkrVideoRuntimeResult mdkr_video_config_runtime_set(MdkrVideoKey key,
                                                     const char *value) {
    const MdkrVideoRuntimeChange change = { key, value };
    return mdkr_video_config_runtime_set_many(&change, 1);
}

MdkrVideoRuntimeResult mdkr_video_config_runtime_set_presentation_pace(
    MdkrPresentationPace pace) {
    const char *frame_limit = NULL;
    const char *motion_smoothing = NULL;
    MdkrVideoRuntimeChange changes[2];

    if (!mdkr_video_presentation_pace_values(pace, &frame_limit,
                                             &motion_smoothing)) {
        return MDKR_VIDEO_RUNTIME_INVALID;
    }
    changes[0].key = MDKR_VIDEO_FRAME_LIMIT;
    changes[0].value = frame_limit;
    changes[1].key = MDKR_VIDEO_MOTION_SMOOTHING;
    changes[1].value = motion_smoothing;
    return mdkr_video_config_runtime_set_many(changes, 2);
}

MdkrVideoRuntimeResult mdkr_audio_config_runtime_set_game_levels(
    unsigned music_level, unsigned effects_level) {
    char music[16];
    char effects[16];
    MdkrVideoRuntimeChange changes[2];

    if (music_level > 256u || effects_level > 256u) {
        return MDKR_VIDEO_RUNTIME_INVALID;
    }
    snprintf(music, sizeof(music), "%u", (music_level * 100u + 128u) / 256u);
    snprintf(effects, sizeof(effects), "%u",
             (effects_level * 100u + 128u) / 256u);
    changes[0].key = MDKR_AUDIO_MUSIC_VOLUME;
    changes[0].value = music;
    changes[1].key = MDKR_AUDIO_EFFECTS_VOLUME;
    changes[1].value = effects;
    return mdkr_video_config_runtime_set_many(changes, 2);
}

int mdkr_audio_config_runtime_preview(MdkrVideoKey key, int percent) {
    int master;
    int music;
    int effects;
    if (!s_video_initialized || !mdkr_video_key_is_audio(key) ||
        percent < 0 || percent > 100 ||
        mdkr_video_config_runtime_locked(key)) {
        return 0;
    }
    master = (int)s_video.values[MDKR_AUDIO_MASTER_VOLUME].number;
    music = (int)s_video.values[MDKR_AUDIO_MUSIC_VOLUME].number;
    effects = (int)s_video.values[MDKR_AUDIO_EFFECTS_VOLUME].number;
    if (key == MDKR_AUDIO_MASTER_VOLUME) master = percent;
    if (key == MDKR_AUDIO_MUSIC_VOLUME) music = percent;
    if (key == MDKR_AUDIO_EFFECTS_VOLUME) effects = percent;
    return mdkr_audio_volume_publish(master, music, effects);
}

void mdkr_audio_config_runtime_cancel_preview(void) {
    if (!s_video_initialized) {
        return;
    }
    (void)mdkr_audio_volume_publish(
        (int)s_video.values[MDKR_AUDIO_MASTER_VOLUME].number,
        (int)s_video.values[MDKR_AUDIO_MUSIC_VOLUME].number,
        (int)s_video.values[MDKR_AUDIO_EFFECTS_VOLUME].number);
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
        case MDKR_VIDEO_SOURCE_FILE:
            return mdkr_video_resolve_paths()
                ? s_video_ini_path : "config path unavailable";
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
           mdkr_video_config_is_readonly()
               ? " (presentation read-only)" : "");
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
