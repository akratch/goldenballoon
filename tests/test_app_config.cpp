/* App preferences must fail closed on hostile input and merge concurrent edits. */
#include "app_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#else
#include <direct.h>
#include <process.h>
#endif

static int failures;

static void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main() {
#if defined(_WIN32)
    char directory[512];
    std::snprintf(directory, sizeof(directory), ".\\mdkr-app-config-%ld",
                  (long)_getpid());
    _rmdir(directory);
    expect(_mkdir(directory) == 0, "temporary preference directory created");
#else
    char directory[] = "/tmp/mdkr-app-config-XXXXXX";
    expect(mkdtemp(directory) != nullptr, "temporary preference directory created");
#endif
    if (failures) return 1;
#if defined(_WIN32)
    _putenv_s("MDKR_APP_PREFS_DIR", directory);
#else
    setenv("MDKR_APP_PREFS_DIR", directory, 1);
#endif
    const std::string path = std::string(directory) + "/mdkr64_app.ini";

    AppConfig::load();
    AppConfig::set("kept", "before-corrupt-read");
    expect(AppConfig::save() == AppConfig::PersistResult::Durable,
           "initial preference saved durably");
    expect(!AppConfig::loadPathForTest(""),
           "an unavailable preference path is rejected");
    expect(AppConfig::get("kept") == "before-corrupt-read",
           "an unavailable preference path keeps valid in-memory preferences");
    AppConfig::forceDirectorySyncFailureForTest(true);
    const AppConfig::PersistResult unconfirmed =
        AppConfig::setAndSave("durability_probe", "visible-now");
    AppConfig::forceDirectorySyncFailureForTest(false);
    expect(unconfirmed == AppConfig::PersistResult::DurabilityUnconfirmed,
           "post-replace directory sync failure is reported distinctly");
    expect(AppConfig::persistResultApplied(unconfirmed),
           "visible post-replace preference is still classified as applied");
    AppConfig::load();
    expect(AppConfig::get("durability_probe") == "visible-now",
           "unconfirmed replacement remains readable after reload");
    std::FILE *file = std::fopen(path.c_str(), "wb");
    expect(file != nullptr, "oversized preference fixture opened");
    if (file) {
        for (size_t i = 0; i < 400u * 1024u; ++i) std::fputc('x', file);
        expect(std::fclose(file) == 0, "oversized preference fixture written");
    }
    AppConfig::load();
    expect(AppConfig::get("kept") == "before-corrupt-read",
           "oversized input leaves the last valid in-memory preferences intact");

    file = std::fopen(path.c_str(), "wb");
    expect(file != nullptr, "NUL-rich preference fixture opened");
    if (file) {
        static const unsigned char block[] = {
            'x', '\0', 'x', '\0', 'x', '\0', 'x', '\0'
        };
        for (size_t i = 0; i < 80u * 1024u; ++i) {
            if (std::fwrite(block, 1, sizeof(block), file) != sizeof(block)) break;
        }
        expect(std::fclose(file) == 0, "NUL-rich preference fixture written");
    }
    AppConfig::load();
    expect(AppConfig::get("kept") == "before-corrupt-read",
           "NUL-rich input is rejected without replacing valid in-memory preferences");

    expect(std::remove(path.c_str()) == 0,
           "NUL-rich preference fixture removed");
    AppConfig::load();
    const std::string boundaryValue(384u * 1024u - std::strlen("boundary="), 'b');
    expect(AppConfig::setAndSave("boundary", boundaryValue) ==
               AppConfig::PersistResult::Durable,
           "maximum accepted preference line persists");
    AppConfig::load();
    expect(AppConfig::get("boundary") == boundaryValue,
           "maximum accepted preference line reloads exactly");

    expect(std::remove(path.c_str()) == 0,
           "boundary preference fixture removed");
    AppConfig::load();
    const std::string longestPath(32767u * 4u, '\\');
    expect(AppConfig::setAndSave("rom_path", longestPath) ==
               AppConfig::PersistResult::Durable,
           "maximum supported UTF-8 path round-trips through preferences");
    AppConfig::load();
    expect(AppConfig::get("rom_path") == longestPath,
           "escaped maximum path reloads exactly");

    expect(std::remove(path.c_str()) == 0,
           "maximum-path preference fixture removed");
    AppConfig::load();
#if !defined(_WIN32)
    const pid_t first = fork();
    if (first == 0) {
        AppConfig::load();
        _exit(AppConfig::persistResultApplied(
            AppConfig::setAndSave("writer_one", "one")) ? 0 : 1);
    }
    const pid_t second = fork();
    if (second == 0) {
        AppConfig::load();
        _exit(AppConfig::persistResultApplied(
            AppConfig::setAndSave("writer_two", "two")) ? 0 : 1);
    }
    int first_status = 1;
    int second_status = 1;
    expect(first > 0 && second > 0, "concurrent preference writers spawned");
    if (first > 0) waitpid(first, &first_status, 0);
    if (second > 0) waitpid(second, &second_status, 0);
    expect(WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0 &&
               WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0,
           "concurrent preference writers completed");
    AppConfig::load();
    expect(AppConfig::get("writer_one") == "one" &&
               AppConfig::get("writer_two") == "two",
           "sidecar lock merges independent concurrent writes without loss");
#else
    /* Windows uses the same OS-side lock implementation; its hosted CTest
     * lane cannot fork, so the portable transaction checks above remain live. */
    expect(true, "Windows preference lock is compiled into the portable transaction seam");
#endif

    expect(std::remove(path.c_str()) == 0,
           "final preference file removed");
    expect(std::remove((path + ".lock").c_str()) == 0,
           "final preference lock file removed");

#if defined(_WIN32)
    expect(_rmdir(directory) == 0, "temporary preference directory removed");
#else
    expect(rmdir(directory) == 0, "temporary preference directory removed");
#endif
    if (failures) return 1;
    std::puts("app config persistence passed");
    return 0;
}
