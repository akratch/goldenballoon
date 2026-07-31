// test_app_shell.cpp — ROM-free, window-free unit tests for the app shell's
// pure logic: the launcher/engine argv triage and the ROM picker's validator.
//
// The triage tests are the load-bearing ones. The shell owns main(), so a bug
// there does not merely misroute a flag — it opens a window inside a headless
// gate and hangs it. These assert the deny-by-default rule directly, including
// against a census of the flags the real harness actually passes.
#include "arg_triage.h"
#include "rom_validate.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static int g_failures = 0;

static void expect(bool cond, const char *what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

// Build an argv from a vector of strings and run the triage over it.
static int triage(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    std::vector<std::string> owned = args;
    owned.insert(owned.begin(), "mdkr64");
    for (std::string &s : owned) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    return mdkr_is_automation_invocation((int)owned.size(), argv.data());
}

static void test_triage() {
    // The ONLY interactive shapes.
    expect(triage({}) == 0, "bare invocation opens the launcher");
    expect(triage({"--ui"}) == 0, "--ui opens the launcher");
    expect(triage({"--ui", "--rom", "x.z64"}) == 0, "--ui wins over other flags");

    // Every flag the harness actually passes must bypass the shell. This list is
    // the census taken from tests/, tools/ and .github/ — if someone adds a flag
    // here they are documenting a real call site, and deny-by-default means the
    // rule still holds for flags nobody thought to list.
    const char *harness[] = {
        "--rom", "--headless-frames", "--dump-frames", "--input-script",
        "--window-size", "--video-list", "--video-set", "--pure", "--restored",
        "--remastered", "--renderer", "--version", "--help", "-h",
        "--video-launch-mode", "--video-launch-set", "--video-launch-persist",
        "--aspect", "--fov", "--max-hfov", "--widescreen", "--legacy-stretch",
    };
    for (const char *f : harness) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s bypasses the launcher", f);
        expect(triage({f}) == 1, msg);
    }

    // A bare positional ROM is still an engine invocation, not a launcher seed.
    expect(triage({"baserom.us.v80.z64"}) == 1, "positional ROM bypasses the launcher");
    // And an unknown/future flag defaults to the SAFE side.
    expect(triage({"--some-flag-invented-tomorrow"}) == 1,
           "unknown flag bypasses the launcher (deny by default)");

    expect(mdkr_argv_requests_ui(0, nullptr) == 0, "null argv is not a --ui request");
}

static void test_rom_validate() {
    // A path that does not exist must be reported, not crash.
    RomInfo missing = mdkr_validate_rom("/nonexistent/definitely-not-here.z64");
    expect(missing.valid == 0, "missing file is not valid");
    expect(missing.message[0] != '\0', "missing file has a message");

    RomInfo none = mdkr_validate_rom(nullptr);
    expect(none.valid == 0, "null path is not valid");
    expect(none.message[0] != '\0', "null path has a message");

    // A file that is too short to hold a header.
    const char *shortPath = "test_app_shell_short.bin";
    if (std::FILE *f = std::fopen(shortPath, "wb")) {
        std::fputs("nope", f);
        std::fclose(f);
        RomInfo tiny = mdkr_validate_rom(shortPath);
        expect(tiny.valid == 0, "truncated file is not valid");
        expect(std::strstr(tiny.message, "too small") != nullptr,
               "truncated file says it is too small");
        std::remove(shortPath);
    }

    // 64 bytes of non-N64 data: right size, wrong magic.
    const char *junkPath = "test_app_shell_junk.bin";
    if (std::FILE *f = std::fopen(junkPath, "wb")) {
        unsigned char junk[0x40];
        std::memset(junk, 0xAB, sizeof(junk));
        std::fwrite(junk, 1, sizeof(junk), f);
        std::fclose(f);
        RomInfo j = mdkr_validate_rom(junkPath);
        expect(j.valid == 0, "non-N64 header is not valid");
        expect(std::strstr(j.message, "not an N64 ROM") != nullptr,
               "non-N64 header says so");
        std::remove(junkPath);
    }

    // The supported list must be non-empty and name both accepted revisions —
    // this is the string the picker shows when a player's ROM is refused, so an
    // empty one would leave them with no idea what to get.
    const char *list = mdkr_supported_rom_list();
    expect(list != nullptr && list[0] != '\0', "supported list is non-empty");
    expect(std::strstr(list, "us.v80") != nullptr, "supported list names us.v80");
    expect(std::strstr(list, "pal.v80") != nullptr, "supported list names pal.v80");
}

int main(void) {
    test_triage();
    test_rom_validate();
    if (g_failures) {
        std::printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nall app-shell tests passed\n");
    return 0;
}
