// engine_boot.cpp — synthesize a CLI invocation and hand it to the engine.
//
// The launcher never reimplements engine startup: it builds the exact argv a
// user would have typed and calls mdkr64_headless_main(). Anything the CLI can
// express, the launcher expresses the same way, so there is one boot path to
// reason about and no second one to drift.
#include "engine_entry.h"

#include "video_config.h"   // MdkrVideoMode, mdkr_video_schema

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *modeFlag(int mode) {
    switch (mode) {
        case MDKR_VIDEO_MODE_PURE:       return "--pure";
        case MDKR_VIDEO_MODE_RESTORED:   return "--restored";
        case MDKR_VIDEO_MODE_REMASTERED: return "--remastered";
        default:                         return nullptr;  // Custom / unset
    }
}

}  // namespace

int mdkr64_engine_boot(const MdkrBootConfig *cfg) {
    // Own every string for the lifetime of the call: argv must stay valid for
    // the whole engine run, and the engine keeps pointers into it (romPath is
    // stored, not copied, by main_pc.c).
    std::vector<std::string> owned;
    owned.push_back("mdkr64");

    if (cfg != nullptr) {
        if (cfg->rom_path != nullptr && cfg->rom_path[0] != '\0') {
            owned.push_back("--rom");
            owned.push_back(cfg->rom_path);
        }
        if (const char *mf = modeFlag(cfg->video_mode)) {
            owned.push_back(mf);
        }
        if (cfg->window_width > 0 && cfg->window_height > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%dx%d", cfg->window_width, cfg->window_height);
            owned.push_back("--window-size");
            owned.push_back(buf);
        }
        if (cfg->automation_ticks > 0) {
            owned.push_back("--headless-ticks");
            owned.push_back(std::to_string(cfg->automation_ticks));
        }
        if (cfg->input_script != nullptr && cfg->input_script[0] != '\0') {
            owned.push_back("--input-script");
            owned.push_back(cfg->input_script);
        }
        // Staged RESTART-scope settings ride in as --video-set, which sits at
        // MDKR_VIDEO_SOURCE_CLI — above the ini the panel also wrote them to.
        // Same value from both layers, so precedence is a no-op here; passing
        // them explicitly is what makes the choice apply to THIS boot.
        int n = cfg->override_count;
        if (n > MDKR_BOOT_MAX_OVERRIDES) n = MDKR_BOOT_MAX_OVERRIDES;
        for (int i = 0; i < n; ++i) {
            if (cfg->overrides[i] == nullptr || cfg->overrides[i][0] == '\0') continue;
            owned.push_back("--video-set");
            owned.push_back(cfg->overrides[i]);
        }
    }

    std::vector<char *> argv;
    argv.reserve(owned.size() + 1);
    for (std::string &s : owned) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    // The shell initialized video config before showing Settings. Commit the
    // settings it staged, plus these synthesized CLI overrides, to the active
    // engine config now. main_pc.c's init remains idempotent and publishes this
    // already-resolved state before any presentation subsystem can latch it.
    if (!mdkr_video_config_handoff_to_engine(
            static_cast<int>(owned.size()), argv.data())) {
        std::fprintf(stderr,
                     "[app] video-config handoff was missing or repeated; "
                     "engine boot stopped\n");
        return 2;
    }

    std::fprintf(stderr, "[app] boot:");
    for (size_t i = 1; i < owned.size(); ++i) std::fprintf(stderr, " %s", owned[i].c_str());
    std::fprintf(stderr, "\n");

    return mdkr64_headless_main((int)owned.size(), argv.data());
}
