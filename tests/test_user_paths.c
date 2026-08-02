#include "user_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
static int mdkr_test_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, value);
}

static int mdkr_test_unsetenv(const char *name) {
    return _putenv_s(name, "");
}

#define setenv(name, value, overwrite) \
    mdkr_test_setenv((name), (value), (overwrite))
#define unsetenv(name) mdkr_test_unsetenv(name)
#endif

static int s_failures;
static const char *s_mock_pref;
static int s_mock_pref_failure;

/* Link-time SDL preference provider for platform/user_paths.c. */
char *SDL_GetPrefPath(const char *organization, const char *application) {
    char *result;
    size_t length;
    (void)organization;
    (void)application;
    if (s_mock_pref_failure || s_mock_pref == NULL) {
        return NULL;
    }
    length = strlen(s_mock_pref);
    result = (char *)malloc(length + 1u);
    if (result != NULL) {
        memcpy(result, s_mock_pref, length + 1u);
    }
    return result;
}

void SDL_free(void *memory) {
    free(memory);
}

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static int join(char *output, size_t output_size,
                const char *directory, const char *leaf) {
    int written = snprintf(output, output_size, "%s/%s", directory, leaf);
    return written >= 0 && (size_t)written < output_size;
}

static int make_directory(const char *path) {
#ifdef _WIN32
    return mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static int write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    int good;
    if (file == NULL) {
        return 0;
    }
    good = fwrite(bytes, 1u, size, file) == size && fclose(file) == 0;
    return good;
}

static int read_equals(const char *path, const void *bytes, size_t size) {
    unsigned char buffer[256];
    FILE *file;
    size_t got;
    int trailing;
    if (size > sizeof(buffer)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    got = fread(buffer, 1u, sizeof(buffer), file);
    trailing = fgetc(file);
    (void)fclose(file);
    return got == size && trailing == EOF && memcmp(buffer, bytes, size) == 0;
}

static int missing(const char *path) {
    struct stat status;
    return stat(path, &status) != 0 && errno == ENOENT;
}

static int run_pref_failure(void) {
    s_mock_pref_failure = 1;
    expect("packaged preference failure is fatal",
           mdkr_user_paths_init(
               "/tmp/Fixture.app/Contents/MacOS/mdkr64") == -1);
    expect("failed package remains recognized",
           mdkr_user_paths_is_packaged());
    return s_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    char temporary[] = "/tmp/mdkr-user-paths-XXXXXX";
    char original[4096];
    char app[4096];
    char contents[4096];
    char executable_dir[4096];
    char executable[4096];
    char resources[4096];
    char resource_save[4096];
    char cwd[4096];
    char cwd_save[4096];
    char pref[4096];
    char override_dir[4096];
    char override_config[4096];
    char override_save[4096];
    char path[4096];
    char other[4096];
    char interrupted_stage[4096];
    char interrupted_partial[4096];
    static const char controller_bytes[] = "immutable-controller-db\n";
    static const char resource_config[] = "Mode=restored\n";
    static const char cwd_config[] = "Mode=pure\n";
    static const unsigned char resource_eeprom[] = { 1, 2, 3, 4, 5 };
    static const unsigned char cwd_eeprom[] = { 9, 9, 9 };
    static const char autosave[] = "autosave-one";
    static const char pak[] = "controller-pak";
    static const char pak_bad[] = "controller-pak-bad";
    static const char new_config[] = "Mode=custom\n";
    static const unsigned char new_eeprom[] = { 7, 7, 7, 7 };

    if (argc > 1 && strcmp(argv[1], "--pref-fail") == 0) {
        return run_pref_failure();
    }
    expect("captured original cwd", getcwd(original, sizeof(original)) != NULL);
    expect("fixture root created", mkdtemp(temporary) != NULL);
    expect("app path", join(app, sizeof(app), temporary, "Fixture.app"));
    expect("Contents path", join(contents, sizeof(contents), app, "Contents"));
    expect("MacOS path", join(executable_dir, sizeof(executable_dir),
                              contents, "MacOS"));
    expect("executable path", join(executable, sizeof(executable),
                                   executable_dir, "mdkr64"));
    expect("Resources path", join(resources, sizeof(resources),
                                  contents, "Resources"));
    expect("resource save path", join(resource_save, sizeof(resource_save),
                                      resources, "save"));
    expect("cwd path", join(cwd, sizeof(cwd), temporary, "launch-cwd"));
    expect("cwd save path", join(cwd_save, sizeof(cwd_save), cwd, "save"));
    expect("pref path", join(pref, sizeof(pref), temporary, "prefs"));
    expect("override path", join(override_dir, sizeof(override_dir),
                                 temporary, "overrides"));
    expect("mkdir app", make_directory(app));
    expect("mkdir Contents", make_directory(contents));
    expect("mkdir MacOS", make_directory(executable_dir));
    expect("mkdir Resources", make_directory(resources));
    expect("mkdir resource save", make_directory(resource_save));
    expect("mkdir cwd", make_directory(cwd));
    expect("mkdir cwd save", make_directory(cwd_save));
    expect("mkdir pref", make_directory(pref));
    expect("mkdir overrides", make_directory(override_dir));

    expect("resource controller path",
           join(path, sizeof(path), resources, "gamecontrollerdb.txt"));
    expect("wrote immutable controller fixture",
           write_bytes(path, controller_bytes, sizeof(controller_bytes) - 1u));
    expect("resource config path",
           join(path, sizeof(path), resources, "mdkr64.ini"));
    expect("wrote resource config",
           write_bytes(path, resource_config, sizeof(resource_config) - 1u));
    expect("resource EEPROM path",
           join(path, sizeof(path), resource_save, "eeprom.bin"));
    expect("wrote resource EEPROM",
           write_bytes(path, resource_eeprom, sizeof(resource_eeprom)));
    expect("resource autosave path",
           join(path, sizeof(path), resource_save, "eeprom.bin.autosave.1"));
    expect("wrote resource autosave",
           write_bytes(path, autosave, sizeof(autosave) - 1u));
    expect("resource pak path",
           join(path, sizeof(path), resource_save, "controller-pak-1.mdp"));
    expect("wrote resource pak", write_bytes(path, pak, sizeof(pak) - 1u));
    expect("resource bad pak path",
           join(path, sizeof(path), resource_save,
                "controller-pak-1.mdp.bad.1"));
    expect("wrote resource bad pak",
           write_bytes(path, pak_bad, sizeof(pak_bad) - 1u));
    expect("cwd config path", join(path, sizeof(path), cwd, "mdkr64.ini"));
    expect("wrote conflicting cwd config",
           write_bytes(path, cwd_config, sizeof(cwd_config) - 1u));
    expect("cwd EEPROM path", join(path, sizeof(path), cwd_save, "eeprom.bin"));
    expect("wrote conflicting cwd EEPROM",
           write_bytes(path, cwd_eeprom, sizeof(cwd_eeprom)));

    /* Model the signed seal: migration must need read access only. */
    expect("resource controller made immutable",
           join(path, sizeof(path), resources, "gamecontrollerdb.txt") &&
           chmod(path, 0400) == 0);
    expect("resource config made immutable",
           join(path, sizeof(path), resources, "mdkr64.ini") &&
           chmod(path, 0400) == 0);
    expect("resource EEPROM made immutable",
           join(path, sizeof(path), resource_save, "eeprom.bin") &&
           chmod(path, 0400) == 0);
    expect("resource autosave made immutable",
           join(path, sizeof(path), resource_save, "eeprom.bin.autosave.1") &&
           chmod(path, 0400) == 0);
    expect("resource pak made immutable",
           join(path, sizeof(path), resource_save, "controller-pak-1.mdp") &&
           chmod(path, 0400) == 0);
    expect("resource bad pak made immutable",
           join(path, sizeof(path), resource_save,
                "controller-pak-1.mdp.bad.1") && chmod(path, 0400) == 0);
    expect("resource save directory made immutable",
           chmod(resource_save, 0500) == 0);
    expect("Resources directory made immutable", chmod(resources, 0500) == 0);

    expect("override config path",
           join(override_config, sizeof(override_config),
                override_dir, "video.ini"));
    expect("override save path",
           join(override_save, sizeof(override_save), override_dir, "save"));
    expect("set video override",
           setenv("MDKR_VIDEO_CONFIG_PATH", override_config, 1) == 0);
    expect("set save override", setenv("MDKR_SAVE_DIR", override_save, 1) == 0);
    expect("entered launch cwd", chdir(cwd) == 0);
    s_mock_pref = pref;
    expect("packaged paths initialized", mdkr_user_paths_init(executable) == 1);
    expect("package recognized", mdkr_user_paths_is_packaged());
    expect("video override wins",
           mdkr_user_video_config_path(path, sizeof(path)) &&
           strcmp(path, override_config) == 0);
    expect("save override wins",
           mdkr_user_save_directory(path, sizeof(path)) &&
           strcmp(path, override_save) == 0);
    expect("override config not migrated", missing(override_config));
    expect("override save not migrated", missing(override_save));
    expect("default config absent while override active",
           join(path, sizeof(path), pref, "mdkr64.ini") && missing(path));
    expect("default save absent while override active",
           join(path, sizeof(path), pref, "save") && missing(path));

    unsetenv("MDKR_VIDEO_CONFIG_PATH");
    unsetenv("MDKR_SAVE_DIR");
    /* A killed prior copier can leave only a sibling stage. It must never be
     * interpreted as the live save directory or partly installed over it. */
    expect("interrupted stage path",
           snprintf(interrupted_stage, sizeof(interrupted_stage),
                    "%s/save.migrate-%ld.tmp",
                    pref, (long)getpid()) > 0);
    expect("created interrupted stage", make_directory(interrupted_stage));
    expect("interrupted partial path",
           join(interrupted_partial, sizeof(interrupted_partial),
                interrupted_stage, "eeprom.bin"));
    expect("wrote interrupted partial save",
           write_bytes(interrupted_partial, cwd_eeprom, sizeof(cwd_eeprom)));
    expect("interrupted stage fails closed",
           !mdkr_user_paths_prepare_packaged_data());
    expect("partial stage is not authoritative",
           join(other, sizeof(other), pref, "save") && missing(other));
    expect("partial stage retained for explicit cleanup",
           read_equals(interrupted_partial, cwd_eeprom, sizeof(cwd_eeprom)));
    /* The fixture owns this exact staged target, so only the fixture removes
     * it before retrying. Production never deletes an unvalidated stale dir. */
    expect("removed interrupted partial", remove(interrupted_partial) == 0);
    expect("removed interrupted stage", rmdir(interrupted_stage) == 0);
    expect("copy-only migration completed",
           mdkr_user_paths_prepare_packaged_data());
    expect("packaged config resolves below prefs",
           mdkr_user_video_config_path(path, sizeof(path)) &&
           join(other, sizeof(other), pref, "mdkr64.ini") &&
           strcmp(path, other) == 0);
    expect("resource config won migration precedence",
           read_equals(path, resource_config, sizeof(resource_config) - 1u));
    expect("packaged save resolves below prefs",
           mdkr_user_save_directory(path, sizeof(path)) &&
           join(other, sizeof(other), pref, "save") &&
           strcmp(path, other) == 0);
    expect("migrated EEPROM path",
           join(path, sizeof(path), other, "eeprom.bin"));
    expect("resource EEPROM won migration precedence",
           read_equals(path, resource_eeprom, sizeof(resource_eeprom)));
    expect("migrated autosave path",
           join(path, sizeof(path), other, "eeprom.bin.autosave.1"));
    expect("autosave migrated byte-identically",
           read_equals(path, autosave, sizeof(autosave) - 1u));
    expect("migrated pak path",
           join(path, sizeof(path), other, "controller-pak-1.mdp"));
    expect("pak migrated byte-identically",
           read_equals(path, pak, sizeof(pak) - 1u));
    expect("migrated bad pak path",
           join(path, sizeof(path), other, "controller-pak-1.mdp.bad.1"));
    expect("bad pak migrated byte-identically",
           read_equals(path, pak_bad, sizeof(pak_bad) - 1u));

    expect("absolute packaged resource path",
           mdkr_user_resource_path("gamecontrollerdb.txt", path, sizeof(path)) &&
           join(other, sizeof(other), resources, "gamecontrollerdb.txt") &&
           strcmp(path, other) == 0);
    expect("bundle controller fixture stayed byte-identical",
           read_equals(path, controller_bytes, sizeof(controller_bytes) - 1u));
    expect("bundle config stayed byte-identical",
           join(path, sizeof(path), resources, "mdkr64.ini") &&
           read_equals(path, resource_config, sizeof(resource_config) - 1u));
    expect("bundle EEPROM stayed byte-identical",
           join(path, sizeof(path), resource_save, "eeprom.bin") &&
           read_equals(path, resource_eeprom, sizeof(resource_eeprom)));
    expect("bundle gained no config temp",
           join(path, sizeof(path), resources, "mdkr64.ini.tmp") && missing(path));
    expect("bundle gained no save temp",
           join(path, sizeof(path), resource_save, "eeprom.bin.tmp") &&
           missing(path));

    expect("destination config path",
           join(path, sizeof(path), pref, "mdkr64.ini"));
    expect("updated destination config",
           write_bytes(path, new_config, sizeof(new_config) - 1u));
    expect("destination save path",
           join(other, sizeof(other), pref, "save/eeprom.bin"));
    expect("updated destination save",
           write_bytes(other, new_eeprom, sizeof(new_eeprom)));
    expect("migration remains idempotent",
           mdkr_user_paths_prepare_packaged_data());
    expect("existing destination config never replaced",
           read_equals(path, new_config, sizeof(new_config) - 1u));
    expect("existing destination save never replaced",
           read_equals(other, new_eeprom, sizeof(new_eeprom)));

    expect("returned to original cwd", chdir(original) == 0);
    if (s_failures != 0) {
        fprintf(stderr, "%d user-path test(s) failed\n", s_failures);
        return 1;
    }
    puts("packaged user paths: PASS");
    return 0;
}
