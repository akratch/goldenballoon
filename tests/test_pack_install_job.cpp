// The launcher's pack-installation job, driven the way a launcher frame drives
// it: the frame's input (Cancel, Quit) happens first, then service() consumes
// a finished copy. A copy that finishes in the same frame as a Cancel or Quit
// must never be published, and nothing it staged may be left behind.
//
// Every scratch path is under the temp directory; the last line is PASS or
// FAIL.
#include "pack_install_job.h"

#include "miniz.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "pack install job:%d: %s\n", __LINE__, #condition); \
        std::puts("FAIL: pack install job"); \
        return 1; \
    } \
} while (0)

namespace {

struct TestRoot {
    fs::path path;
    ~TestRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool writeZip(const fs::path &path, const char *name, const char *body) {
    mz_zip_archive zip{};
    void *bytes = nullptr;
    size_t size = 0;
    bool okay = mz_zip_writer_init_heap(&zip, 0, 4096) &&
        mz_zip_writer_add_mem(&zip, name, body, std::strlen(body), MZ_BEST_SPEED) &&
        mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size);
    mz_zip_writer_end(&zip);
    if (okay) {
        std::ofstream out(path, std::ios::binary);
        out.write(static_cast<const char *>(bytes), static_cast<std::streamsize>(size));
        okay = static_cast<bool>(out);
    }
    mz_free(bytes);
    return okay;
}

bool emptyOrAbsent(const fs::path &directory) {
    std::error_code error;
    return !fs::exists(directory, error) || fs::is_empty(directory, error);
}

bool contains(const std::string &text, const char *part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TestRoot root{fs::temp_directory_path() /
                  ("mdkr-pack-install-job-" + std::to_string(nonce))};
    const fs::path input = root.path / "input";
    REQUIRE(fs::create_directories(input));
    const fs::path pack = input / "Pack.zip";
    REQUIRE(writeZip(pack, "pack.ini", "[pack]\nname=Pack\n"));
    const std::string source = pack.u8string();

    // Cancel clicked in the frame the copy finished: not published, and the
    // staged copy is deleted in the background.
    {
        const auto mods = root.path / "cancel-mods";
        PackInstallJob job;
        job.start(source, mods.u8string());
        REQUIRE(job.busy());
        job.waitForCopy();
        REQUIRE(!fs::exists(mods / "Pack.zip"));
        job.cancel();
        REQUIRE(!job.service(false));
        REQUIRE(!job.busy());
        REQUIRE(contains(job.note(), "cancelled"));
        job.waitForBackground();
        REQUIRE(emptyOrAbsent(mods));
    }
    // Quit in the frame the copy finished: not published.
    {
        const auto mods = root.path / "quit-mods";
        PackInstallJob job;
        job.start(source, mods.u8string());
        job.waitForCopy();
        REQUIRE(!job.service(true));
        REQUIRE(!job.busy());
        job.waitForBackground();
        REQUIRE(emptyOrAbsent(mods));
    }
    // Without Cancel or Quit the finished copy is published by service(), and
    // only once. A second start while busy is ignored.
    {
        const auto mods = root.path / "publish-mods";
        const auto other = input / "Other.zip";
        REQUIRE(writeZip(other, "pack.ini", "[pack]\nname=Other\n"));
        PackInstallJob job;
        job.start(source, mods.u8string());
        job.start(other.u8string(), mods.u8string());
        job.waitForCopy();
        REQUIRE(!fs::exists(mods / "Pack.zip"));
        REQUIRE(job.service(false));
        REQUIRE(fs::file_size(mods / "Pack.zip") == fs::file_size(pack));
        REQUIRE(!fs::exists(mods / "Other.zip"));
        REQUIRE(contains(job.note(), "Pack installed"));
        REQUIRE(!job.busy());
        REQUIRE(!job.service(false));
        REQUIRE(std::distance(fs::directory_iterator(mods), fs::directory_iterator{}) == 1);
    }
    // A copy refused at publication (another pack took the name meanwhile) is
    // not published, the other pack is untouched, and the staged copy goes.
    {
        const auto mods = root.path / "refused-mods";
        PackInstallJob job;
        job.start(source, mods.u8string());
        job.waitForCopy();
        std::ofstream(mods / "Pack.zip", std::ios::binary) << "winner";
        REQUIRE(!job.service(false));
        REQUIRE(contains(job.note(), "meanwhile"));
        job.waitForBackground();
        REQUIRE(fs::file_size(mods / "Pack.zip") == 6);
        REQUIRE(std::distance(fs::directory_iterator(mods), fs::directory_iterator{}) == 1);
    }
    // Leaving the launcher with a finished, unconsumed copy never publishes it
    // and leaves nothing behind -- explicitly and through the destructor.
    {
        const auto mods = root.path / "exit-mods";
        PackInstallJob job;
        job.start(source, mods.u8string());
        job.waitForCopy();
        job.finishForExit();
        REQUIRE(!job.busy());
        REQUIRE(emptyOrAbsent(mods));
        job.finishForExit();
    }
    {
        const auto mods = root.path / "destroyed-mods";
        {
            PackInstallJob job;
            job.start(source, mods.u8string());
            job.waitForCopy();
        }
        REQUIRE(emptyOrAbsent(mods));
    }
    // Staging an earlier crash left behind is removed in the background.
    {
        const auto mods = root.path / "sweep-mods";
        REQUIRE(fs::create_directories(mods / ".install-9-0" / "Pack"));
        std::ofstream(mods / ".install-9-0" / "Pack" / "partial.bin") << "partial";
        PackInstallJob job;
        job.reclaimAbandonedStaging(mods.u8string());
        job.waitForBackground();
        REQUIRE(fs::is_empty(mods));
    }
    std::puts("PASS: pack install job");
    return 0;
}
