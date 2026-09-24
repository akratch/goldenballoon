#include "pack_installer.h"

#include "miniz.h"
#include "mod_source.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <future>
#include <atomic>
#include <utility>
#include <cstdlib>
#include <new>
#include <optional>

// Scoped to this fixture thread so asynchronous copy tests are unaffected.
static thread_local bool rejectCppAllocation;
static thread_local int cppAllocationBudget = -1;
void *operator new(std::size_t size) {
    if (rejectCppAllocation || cppAllocationBudget == 0) throw std::bad_alloc();
    if (cppAllocationBudget > 0) --cppAllocationBudget;
    if (void *memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace fs = std::filesystem;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "pack installer:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct TestRoot {
    fs::path path;
    ~TestRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool writeZip(const fs::path &path, const char *name, const char *body,
              int compression = MZ_BEST_SPEED, const char *extraBody = nullptr,
              const char *extraName = "art.bin") {
    mz_zip_archive zip{};
    const bool okay = mz_zip_writer_init_file(&zip, path.string().c_str(), 0) &&
        mz_zip_writer_add_mem(&zip, name, body, std::strlen(body), compression) &&
        (!extraBody || mz_zip_writer_add_mem(&zip, extraName, extraBody,
                                             std::strlen(extraBody), compression)) &&
        mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return okay;
}

int main(int argc, char **argv) {
    REQUIRE(argc <= 2);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TestRoot root{fs::temp_directory_path() /
                  ("mdkr-pack-installer-" + std::to_string(nonce))};
    const fs::path input = root.path / "input";
    const fs::path mods = root.path / "mods";
    REQUIRE(fs::create_directories(input));
    REQUIRE(writeZip(input / "Textures.ZIP", "pack.ini",
                     "[pack]\nname=Textures\npriority=100\n"));

    {
        auto prepared = PreparedPackInstall::copyZip(input / "Textures.ZIP", mods);
        // A copied ZIP is hidden until validation/publishing finishes.
        REQUIRE(!fs::exists(mods / "Textures.ZIP"));
        const PackInstallResult installed = prepared.finalize();
        REQUIRE(installed.installed);
        REQUIRE(installed.destination == mods / "Textures.ZIP");
        REQUIRE(fs::is_regular_file(installed.destination));
    }
    {
        auto duplicate = PreparedPackInstall::copyZip(input / "Textures.ZIP", mods);
        const PackInstallResult refused = duplicate.finalize();
        REQUIRE(!refused.installed);
        REQUIRE(refused.message.find("already installed") != std::string::npos);
        REQUIRE(fs::is_regular_file(mods / "Textures.ZIP"));
    }
    const fs::path unicodeName = fs::u8path(u8"Forêt.ZIP");
    REQUIRE(writeZip(input / "unicode-source.zip", "pack.ini",
                     "[pack]\nname=Forest\n"));
    fs::rename(input / "unicode-source.zip", input / unicodeName);
    {
        auto prepared = PreparedPackInstall::copyZip(input / unicodeName, mods);
        REQUIRE(prepared.finalize().installed);
        REQUIRE(fs::is_regular_file(mods / unicodeName));
    }
    // A Rice texture ZIP is a supported manifest-less pack, not a malformed
    // ordinary pack. Exercise the same installer path players use for DKR-R.
    REQUIRE(writeZip(input / "rice.zip",
                     "textures/DKR#1234ABCD#0#2_all.png", "synthetic art"));
    {
        auto rice = PreparedPackInstall::copyZip(input / "rice.zip", mods);
        REQUIRE(rice.finalize().installed);
        REQUIRE(fs::is_regular_file(mods / "rice.zip"));
    }

    {
        std::ofstream(input / "broken.zip", std::ios::binary) << "not a ZIP";
        auto malformed = PreparedPackInstall::copyZip(input / "broken.zip", mods);
        REQUIRE(!malformed.finalize().installed);
        REQUIRE(!fs::exists(mods / "broken.zip"));
    }
    REQUIRE(writeZip(input / "no-manifest.zip", "readme.txt", "nothing to load"));
    {
        auto noManifest = PreparedPackInstall::copyZip(input / "no-manifest.zip", mods);
        const PackInstallResult refused = noManifest.finalize();
        REQUIRE(!refused.installed);
        REQUIRE(refused.message.find("pack.ini") != std::string::npos);
        REQUIRE(!fs::exists(mods / "no-manifest.zip"));
    }
    {
        auto wrongType = PreparedPackInstall::copyZip(input / "Textures.txt", mods);
        REQUIRE(!wrongType.finalize().installed);
    }
    {
        // Unsupported Unicode extensions must be refused, not throw during a
        // locale-dependent path conversion. Exercise both public entry points.
        const auto unsupported = input / fs::u8path(u8"Textures.日本語");
        std::ofstream(unsupported, std::ios::binary) << "not a pack";
        const auto refusedMods = root.path / "unsupported-extension-mods";
        auto zip = PreparedPackInstall::copyZip(unsupported, refusedMods);
        REQUIRE(!zip.finalize().installed);
        auto pack = PreparedPackInstall::copyPack(unsupported, refusedMods);
        REQUIRE(!pack.finalize().installed);
        REQUIRE(!fs::exists(refusedMods));
    }
    REQUIRE(writeZip(input / "race.zip", "pack.ini", "[pack]\nname=Race\n"));
    {
        const auto asyncMods = root.path / "async-ready-cancel";
        std::atomic<bool> cancelled{false};
        {
            auto result = std::async(std::launch::async, [&] {
                return PreparedPackInstall::copyZip(input / "race.zip", asyncMods,
                    [&] { return cancelled.load(); });
            });
            // Deterministic ready-but-unconsumed state, no timing assumptions.
            result.wait();
            REQUIRE(!fs::exists(asyncMods / "race.zip"));
            cancelled.store(true);
            auto prepared = result.get();
            const auto refused = prepared.finalize([&] { return cancelled.load(); });
            REQUIRE(!refused.installed);
            REQUIRE(refused.message.find("cancelled") != std::string::npos);
        }
        REQUIRE(fs::is_empty(asyncMods));
    }
    {
        const auto discardedMods = root.path / "async-discarded-result";
        {
            auto result = std::async(std::launch::async, [&] {
                return PreparedPackInstall::copyZip(input / "race.zip", discardedMods);
            });
            result.wait();
            REQUIRE(!fs::exists(discardedMods / "race.zip"));
            // Destroy the sole future without get(): its prepared result must
            // release staging rather than install or leak the completed copy.
        }
        REQUIRE(fs::is_empty(discardedMods));
    }
    {
        const char manifest[] = "[pack]\nname=Budget\n";
        const std::string payload(10000, 'x');
        REQUIRE(writeZip(input / "expanded.zip", "pack.ini", manifest,
                         MZ_BEST_SPEED, payload.c_str()));
        const auto expanded = std::strlen(manifest) + payload.size();
        REQUIRE(fs::file_size(input / "expanded.zip") < expanded);
        REQUIRE(fs::file_size(input / "expanded.zip") < 256 * 1024);
        // This one-chunk ZIP has four copy-phase cancellation checks: intake,
        // first chunk, EOF, and completed copy. Checks five/six are its two
        // metadata members. Exercise cancellation and exception unwinding at
        // each member, not only before reader allocation.
        for (unsigned stopAt : {5u, 6u}) {
            for (bool throws : {false, true}) {
                const auto cancelMods = root.path / ("metadata-cancel-" +
                    std::to_string(stopAt) + (throws ? "-throw" : "-return"));
                unsigned checks = 0;
                bool caught = false;
                try {
                    auto prepared = PreparedPackInstall::copyZip(input / "expanded.zip",
                        cancelMods, [&] {
                            if (++checks != stopAt) return false;
                            if (throws) throw std::runtime_error("metadata cancel fixture");
                            return true;
                        });
                    const auto result = prepared.finalize();
                    REQUIRE(!result.installed);
                    REQUIRE(result.message.find("cancelled") != std::string::npos);
                } catch (const std::runtime_error &error) {
                    REQUIRE(std::string(error.what()) == "metadata cancel fixture");
                    caught = true;
                }
                REQUIRE(caught == throws);
                REQUIRE(checks == stopAt);
                REQUIRE(fs::is_empty(cancelMods));
            }
        }
        for (unsigned test = 0; test < 4; ++test) {
            const auto budgetMods = root.path / ("expanded-budget-" + std::to_string(test));
            {
                PackInstallLimits limits;
                limits.maxEntries = test == 2 ? 1 : 2;
                limits.maxExpandedBytes = expanded - (test == 1 ? 1 : 0);
                if (test == 3) limits.maxZipMetadataBytes = 0;
                auto prepared = PreparedPackInstall::copyZip(input / "expanded.zip",
                                                             budgetMods, {}, limits);
                const auto result = prepared.finalize();
                REQUIRE(result.installed == (test == 0));
                if (test != 0) REQUIRE(result.message.find(test == 1
                    ? "expanded-byte budget" : test == 2 ? "entry budget"
                    : "metadata budget") != std::string::npos);
            }
            if (test != 0) REQUIRE(fs::is_empty(budgetMods));
        }
        // Fail after reader-state allocation, when miniz requests the central
        // directory. No large payload or memory-heavy fixture is needed.
        const std::string longName(20000, 'a');
        REQUIRE(writeZip(input / "metadata.zip", "pack.ini", manifest,
                         MZ_BEST_SPEED, "x", longName.c_str()));
        const auto budgetMods = root.path / "metadata-budget";
        {
            PackInstallLimits limits;
            limits.maxZipMetadataBytes = 4096;
            auto prepared = PreparedPackInstall::copyZip(input / "metadata.zip",
                                                         budgetMods, {}, limits);
            const auto result = prepared.finalize();
            REQUIRE(!result.installed);
            REQUIRE(result.message.find("metadata budget") != std::string::npos);
        }
        REQUIRE(fs::is_empty(budgetMods));
    }
    {
        const auto size = fs::file_size(input / "race.zip");
        for (const bool exact : {false, true}) {
            const auto budgetMods = root.path / (exact ? "zip-exact" : "zip-over-budget");
            {
                auto prepared = PreparedPackInstall::copyZip(input / "race.zip", budgetMods,
                    {}, PackInstallLimits{size - (exact ? 0 : 1), 1});
                const auto result = prepared.finalize();
                REQUIRE(result.installed == exact);
                if (!exact) REQUIRE(result.message.find("byte budget") != std::string::npos);
            }
            REQUIRE(fs::exists(budgetMods / "race.zip") == exact);
            if (!exact) REQUIRE(fs::is_empty(budgetMods));
        }
        const auto budgetMods = root.path / "zip-zero-entries";
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", budgetMods,
                {}, PackInstallLimits{size, 0});
            const auto result = prepared.finalize();
            REQUIRE(!result.installed);
            REQUIRE(result.message.find("entry budget") != std::string::npos);
        }
        REQUIRE(fs::is_empty(budgetMods));
    }
    {
        auto prepared = PreparedPackInstall::copyZip(input / "race.zip", mods);
        // Another installer wins after staging. Publication must not replace
        // it, even though the initial existence check found a free name.
        std::ofstream(mods / "race.zip", std::ios::binary) << "winner";
        REQUIRE(!prepared.finalize().installed);
        std::ifstream winner(mods / "race.zip", std::ios::binary);
        std::string contents;
        winner >> contents;
        REQUIRE(contents == "winner");
    }
    REQUIRE(fs::remove(mods / "race.zip"));
    for (unsigned cancelAt : {1u, 2u}) {
        const auto cancelMods = root.path / ("late-cancel-" + std::to_string(cancelAt));
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", cancelMods);
            unsigned checks = 0;
            const auto refused = prepared.finalize([&] { return ++checks == cancelAt; });
            REQUIRE(!refused.installed);
            REQUIRE(refused.destination.empty());
            REQUIRE(refused.message.find("cancelled") != std::string::npos);
            REQUIRE(checks == cancelAt);
            REQUIRE(!fs::exists(cancelMods / "race.zip"));
        }
        // The cancelled owner's destructor must also reclaim hidden staging,
        // not just leave the destination absent.
        REQUIRE(fs::is_empty(cancelMods));
    }
    for (bool assign : {false, true}) {
        const auto lowMemoryMods = root.path / (assign ? "oom-assign" : "oom-destruct");
        std::optional<PreparedPackInstall> prepared;
        prepared.emplace(PreparedPackInstall::copyZip(input / "race.zip", lowMemoryMods));
        bool armed = false;
        rejectCppAllocation = true;
        try { ::operator delete(::operator new(1)); }
        catch (const std::bad_alloc &) { armed = true; }
        if (assign) *prepared = PreparedPackInstall{};
        else prepared.reset();
        rejectCppAllocation = false;
        REQUIRE(armed);
        REQUIRE(fs::is_empty(lowMemoryMods));
    }
    {
        bool completed = false;
        for (int budget = 0; budget < 128 && !completed; ++budget) {
            const auto faultMods = root.path / ("publish-oom-" + std::to_string(budget));
            {
                auto prepared = PreparedPackInstall::copyZip(input / "race.zip", faultMods);
                bool threw = false;
                bool installed = false;
                cppAllocationBudget = budget;
                try { installed = prepared.finalize().installed; }
                catch (const std::bad_alloc &) { threw = true; }
                cppAllocationBudget = -1;
                // Every failure must precede publication. Once published, the
                // operation must return success, not throw during reporting.
                REQUIRE(fs::exists(faultMods / "race.zip") == installed);
                REQUIRE(threw || installed);
                completed = installed;
            }
            if (!completed) REQUIRE(fs::is_empty(faultMods));
        }
        REQUIRE(completed);
    }
    for (unsigned throwAt : {1u, 2u}) {
        const auto exceptionMods = root.path / ("finalize-throw-" + std::to_string(throwAt));
        unsigned checks = 0;
        bool caught = false;
        try {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", exceptionMods);
            prepared.finalize([&] {
                if (++checks == throwAt) throw std::runtime_error("finalize fixture");
                return false;
            });
        } catch (const std::runtime_error &error) {
            REQUIRE(std::string(error.what()) == "finalize fixture");
            caught = true;
        }
        REQUIRE(caught);
        REQUIRE(checks == throwAt);
        // Unwinding must close pinned validator/payload handles before stage
        // cleanup, including the last callback immediately before publication.
        REQUIRE(fs::is_empty(exceptionMods));
    }
    {
        const auto committedMods = root.path / "committed";
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", committedMods);
            unsigned checks = 0;
            const auto installed = prepared.finalize([&] { ++checks; return false; });
            REQUIRE(installed.installed);
            REQUIRE(checks == 2);
            // A late cancellation/repeated finalize must not roll back an
            // already committed pack, including after owner destruction.
            REQUIRE(!prepared.finalize([] { return true; }).installed);
            REQUIRE(fs::file_size(installed.destination) == fs::file_size(input / "race.zip"));
        }
        REQUIRE(fs::is_regular_file(committedMods / "race.zip"));
        REQUIRE(std::distance(fs::directory_iterator(committedMods),
                              fs::directory_iterator{}) == 1);
    }
    {
        auto abandoned = PreparedPackInstall::copyZip(input / "race.zip", mods);
        auto owner = std::move(abandoned);
        REQUIRE(!abandoned.finalize().installed);
        // Leaving without finalize simulates cancellation or launcher exit.
    }
    REQUIRE(!fs::exists(mods / "race.zip"));
#ifdef _WIN32
    // Windows keeps a non-delete-shared staging handle for the transaction.
    // Moving it away must fail without preventing normal publication.
    for (unsigned swapAt : {1u, 2u}) {
        const auto raceMods = root.path / ("locked-stage-" + std::to_string(swapAt));
        const auto held = root.path / ("locked-stage-held-" + std::to_string(swapAt));
        auto prepared = PreparedPackInstall::copyZip(input / "race.zip", raceMods);
        const auto stage = fs::directory_iterator(raceMods)->path();
        unsigned checks = 0;
        std::error_code moveError;
        auto installed = prepared.finalize([&] {
            if (++checks == swapAt) fs::rename(stage, held, moveError);
            return false;
        });
        REQUIRE(checks == 2);
        REQUIRE(moveError);
        REQUIRE(installed.installed);
        REQUIRE(!fs::exists(held));
        REQUIRE(!fs::exists(stage));
        REQUIRE(fs::file_size(installed.destination) == fs::file_size(input / "race.zip"));
    }
#endif
#ifndef _WIN32
    {
        const auto raceMods = root.path / "payload-replacement";
        const auto held = root.path / "validated-payload.zip";
        fs::path stage;
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", raceMods);
            stage = fs::directory_iterator(raceMods)->path();
            unsigned checks = 0;
            const auto result = prepared.finalize([&] {
                if (++checks == 2) {
                    fs::rename(stage / "race.zip", held);
                    std::ofstream(stage / "race.zip") << "unvalidated replacement";
                }
                return false;
            });
            REQUIRE(!result.installed);
            REQUIRE(!fs::exists(raceMods / "race.zip"));
        }
        REQUIRE(fs::file_size(stage / "race.zip") == 23);
        REQUIRE(fs::is_regular_file(held));
    }
    {
        const auto raceMods = root.path / "copy-directory-race";
        const auto held = root.path / "copy-directory-held";
        fs::path replacement;
        bool swapped = false;
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", raceMods, [&] {
                if (!swapped && fs::exists(raceMods)) {
                    auto entry = fs::directory_iterator(raceMods);
                    if (entry != fs::directory_iterator{}) {
                        replacement = entry->path();
                        fs::rename(replacement, held);
                        fs::create_directory(replacement);
                        std::ofstream(replacement / "race.zip") << "preserve replacement";
                        swapped = true;
                    }
                }
                return false;
            });
            REQUIRE(swapped);
            REQUIRE(!prepared.finalize().installed);
            REQUIRE(fs::file_size(held / "race.zip") == fs::file_size(input / "race.zip"));
        }
        REQUIRE(fs::file_size(replacement / "race.zip") == 20);
        REQUIRE(fs::is_regular_file(held / "race.zip"));
        REQUIRE(!fs::exists(raceMods / "race.zip"));
    }
    // A real replacement directory is not caught by symlink_status. The
    // installer owns the original directory, not whichever entry later takes
    // its name. All paths here are private fixture data.
    for (unsigned swapAt : {1u, 2u}) {
        const auto raceMods = root.path / ("publish-directory-" + std::to_string(swapAt));
        const auto held = root.path / ("directory-held-" + std::to_string(swapAt));
        const auto bait = root.path / ("directory-bait-" + std::to_string(swapAt));
        REQUIRE(fs::create_directory(bait));
        REQUIRE(writeZip(bait / "race.zip", "pack.ini", "[pack]\nname=Substituted\n"));
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", raceMods);
            const auto stage = fs::directory_iterator(raceMods)->path();
            unsigned checks = 0;
            const auto result = prepared.finalize([&] {
                if (++checks == swapAt) {
                    fs::rename(stage, held);
                    fs::rename(bait, stage);
                }
                return false;
            });
            REQUIRE(!result.installed);
            REQUIRE(!fs::exists(raceMods / "race.zip"));
            REQUIRE(fs::is_regular_file(stage / "race.zip"));
        }
        REQUIRE(fs::is_regular_file(held / "race.zip"));
    }
    // A replaced staging-root link must not publish someone else's payload.
    // Exercise replacement before validation and at the last callback boundary.
    for (unsigned swapAt : {1u, 2u}) {
        const auto raceMods = root.path / ("publish-link-" + std::to_string(swapAt));
        const auto bait = root.path / ("publish-bait-" + std::to_string(swapAt));
        const auto held = root.path / ("publish-held-" + std::to_string(swapAt));
        REQUIRE(fs::create_directory(bait));
        REQUIRE(fs::copy_file(input / "race.zip", bait / "race.zip"));
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", raceMods);
            const auto stage = fs::directory_iterator(raceMods)->path();
            unsigned checks = 0;
            const auto result = prepared.finalize([&] {
                if (++checks == swapAt) {
                    fs::rename(stage, held);
                    fs::create_directory_symlink(bait, stage);
                }
                return false;
            });
            REQUIRE(!result.installed);
            REQUIRE(!fs::exists(raceMods / "race.zip"));
            REQUIRE(fs::is_regular_file(bait / "race.zip"));
        }
        // Identity loss must preserve the replacement entry, even a link.
        REQUIRE(fs::is_symlink(fs::directory_iterator(raceMods)->path()));
        REQUIRE(fs::is_regular_file(held / "race.zip"));
    }
    // Cleanup must not traverse a replacement link at the staging root.
    // Both the original stage and the outside bait belong to this fixture.
    {
        const auto cleanupMods = root.path / "cleanup-mods";
        const auto cleanupBait = root.path / "cleanup-bait";
        const auto heldStage = root.path / "held-stage";
        REQUIRE(fs::create_directory(cleanupBait));
        std::ofstream(cleanupBait / "race.zip") << "keep outside bytes";
        {
            auto prepared = PreparedPackInstall::copyZip(input / "race.zip", cleanupMods);
            auto entry = fs::directory_iterator(cleanupMods);
            REQUIRE(entry != fs::directory_iterator{});
            const auto stage = entry->path();
            REQUIRE(stage.filename().string().find(".install-") == 0);
            fs::rename(stage, heldStage);
            fs::create_directory_symlink(cleanupBait, stage);
        }
        REQUIRE(fs::is_regular_file(cleanupBait / "race.zip"));
        REQUIRE(fs::file_size(cleanupBait / "race.zip") == 18);
        REQUIRE(fs::is_symlink(fs::directory_iterator(cleanupMods)->path()));
        REQUIRE(fs::is_regular_file(heldStage / "race.zip"));
    }
    // A dangling destination symlink is an occupied name, not permission to
    // replace it or to follow it into another location.
    fs::create_symlink(root.path / "absent", mods / "race.zip");
    {
        auto prepared = PreparedPackInstall::copyZip(input / "race.zip", mods);
        REQUIRE(!prepared.finalize().installed);
        REQUIRE(fs::is_symlink(mods / "race.zip"));
        REQUIRE(!fs::exists(root.path / "absent"));
    }
    REQUIRE(fs::remove(mods / "race.zip"));
#endif
    {
        auto cancelled = PreparedPackInstall::copyZip(input / "race.zip",
            root.path / "never-created", [] { return true; });
        REQUIRE(!cancelled.finalize().installed);
        REQUIRE(!fs::exists(root.path / "never-created"));
    }
    const std::string largeBody(600000, 'x');
    REQUIRE(writeZip(input / "cancel.zip", "readme.txt", largeBody.c_str(),
                     MZ_NO_COMPRESSION));
    {
        unsigned checks = 0;
        auto cancelled = PreparedPackInstall::copyZip(input / "cancel.zip", mods,
            [&] { return ++checks == 3; });
        REQUIRE(checks == 3); // cancellation after the first copied chunk
        const auto result = cancelled.finalize();
        REQUIRE(!result.installed);
        REQUIRE(result.message.find("cancelled") != std::string::npos);
        REQUIRE(!fs::exists(mods / "cancel.zip"));
    }
    const fs::path folder = input / "FolderPack";
    const fs::path folderMods = root.path / "folder-mods";
    REQUIRE(fs::create_directories(folder / "nested"));
    std::ofstream(folder / "pack.ini") << "[pack]\nname=Folder\n";
    std::ofstream(folder / "nested" / "art.txt") << "original bytes";
    {
        const auto bytes = fs::file_size(folder / "pack.ini") +
                           fs::file_size(folder / "nested" / "art.txt");
        // Exactly three descendants: a directory and two files. Exercise each
        // aggregate boundary independently; neither file exceeds the byte cap.
        for (unsigned test = 0; test < 3; ++test) {
            const auto budgetMods = root.path / ("folder-budget-" + std::to_string(test));
            {
                const PackInstallLimits limits{bytes - (test == 1 ? 1 : 0),
                                               test == 2 ? 2u : 3u};
                auto prepared = PreparedPackInstall::copyPack(folder, budgetMods, {}, limits);
                const auto result = prepared.finalize();
                REQUIRE(result.installed == (test == 0));
                if (test != 0) REQUIRE(result.message.find(
                    test == 1 ? "byte budget" : "entry budget") != std::string::npos);
            }
            if (test != 0) REQUIRE(fs::is_empty(budgetMods));
        }
    }
    {
        auto prepared = PreparedPackInstall::copyPack(folder, folderMods);
        REQUIRE(!fs::exists(folderMods / "FolderPack"));
        REQUIRE(prepared.finalize().installed);
        REQUIRE(fs::file_size(folderMods / "FolderPack" / "nested" / "art.txt") == 14);
        auto duplicate = PreparedPackInstall::copyPack(folder, folderMods);
        REQUIRE(!duplicate.finalize().installed);
    }
    {
        const auto raceMods = root.path / "folder-race";
        auto prepared = PreparedPackInstall::copyPack(folder, raceMods);
        REQUIRE(fs::create_directory(raceMods / "FolderPack"));
        REQUIRE(!prepared.finalize().installed);
        REQUIRE(fs::is_empty(raceMods / "FolderPack"));
    }
    {
        auto self = PreparedPackInstall::copyPack(folder, folder / "mods");
        REQUIRE(!self.finalize().installed);
        REQUIRE(!fs::exists(folder / "mods"));
    }
    {
        unsigned checks = 0;
        auto cancelled = PreparedPackInstall::copyPack(folder, root.path / "folder-cancel",
            [&] { return ++checks >= 3; });
        REQUIRE(!cancelled.finalize().installed);
        REQUIRE(!fs::exists(root.path / "folder-cancel" / "FolderPack"));
    }
    REQUIRE(fs::is_empty(root.path / "folder-cancel"));
    {
        const auto oversized = input / "OversizedPack";
        REQUIRE(fs::create_directory(oversized));
        std::ofstream(oversized / "pack.ini") << "[pack]\nname=Oversized\n";
        std::ofstream(oversized / "large.bin", std::ios::binary).close();
        fs::resize_file(oversized / "large.bin", static_cast<uintmax_t>(MDKR_MOD_SOURCE_ENTRY_MAX) + 1);
        auto prepared = PreparedPackInstall::copyPack(oversized, root.path / "oversized-mods");
        const auto result = prepared.finalize();
        REQUIRE(!result.installed);
        REQUIRE(result.message.find("entry size") != std::string::npos);
        REQUIRE(!fs::exists(root.path / "oversized-mods" / "OversizedPack"));
    }
    REQUIRE(fs::is_empty(root.path / "oversized-mods"));
#ifndef _WIN32
    // Swap the selected root after its iterator is opened. Every bait file is
    // fixture-owned; a pathname-only copy would follow the replacement link.
    {
        const auto changing = input / "ChangingPack";
        const auto retained = input / "RetainedPack";
        const auto bait = input / "OutsideBait";
        const auto changingMods = root.path / "changing-mods";
        REQUIRE(fs::create_directory(changing));
        REQUIRE(fs::create_directory(bait));
        std::ofstream(changing / "pack.ini") << "[pack]\nname=Original\n";
        std::ofstream(bait / "pack.ini") << "[pack]\nname=OUTSIDE\n";
        unsigned checks = 0;
        auto prepared = PreparedPackInstall::copyPack(changing, changingMods, [&] {
            if (++checks == 2) {
                fs::rename(changing, retained);
                fs::create_directory_symlink(bait, changing);
            }
            return false;
        });
        REQUIRE(checks >= 2);
        const auto installed = prepared.finalize();
        REQUIRE(installed.installed);
        if (installed.installed) {
            std::ifstream file(installed.destination / "pack.ini");
            const std::string body((std::istreambuf_iterator<char>(file)), {});
            REQUIRE(body.find("Original") != std::string::npos);
            REQUIRE(body.find("OUTSIDE") == std::string::npos);
        }
        REQUIRE(fs::remove(changing));
    }
    fs::create_symlink(input / "Textures.ZIP", folder / "linked.zip");
    {
        auto linked = PreparedPackInstall::copyPack(folder, root.path / "folder-linked");
        REQUIRE(!linked.finalize().installed);
        REQUIRE(!fs::exists(root.path / "folder-linked" / "FolderPack"));
    }
    REQUIRE(fs::is_empty(root.path / "folder-linked"));
    REQUIRE(fs::remove(folder / "linked.zip"));
#endif
    int visible = 0;
    for (const auto &entry : fs::directory_iterator(mods)) {
        REQUIRE(entry.path().filename() == "Textures.ZIP" ||
                entry.path().filename() == unicodeName ||
                entry.path().filename() == "rice.zip");
        ++visible;
    }
    REQUIRE(visible == 3);
    if (argc == 2) {
        // Optional maintainer-owned fixture: copy/validate in disposable
        // storage, never install into the user's live mods directory.
        const fs::path source = fs::u8path(argv[1]);
        auto prepared = PreparedPackInstall::copyZip(source, root.path / "real-mods");
        const PackInstallResult installed = prepared.finalize();
        if (!installed.installed) {
            std::fprintf(stderr, "fixture refused: %s\n", installed.message.c_str());
        }
        REQUIRE(installed.installed);
        REQUIRE(fs::file_size(source) == fs::file_size(installed.destination));
        REQUIRE(!prepared.finalize().installed);
    }
    std::puts("pack installer tests passed");
    return 0;
}
