#include "save_export.h"
#include "save_container.h"
#include "save_tools_core.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
static std::vector<char> read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void write(const std::filesystem::path &p, const void *data, size_t size) {
    std::ofstream f(p, std::ios::binary); f.write(static_cast<const char *>(data), size);
    check(f.good(), "fixture write");
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("mdkr-save-export-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};
    const auto source = root / "eeprom.bin";
    const auto output = root / fs::u8path(u8"progress-保存.mdkr-save");
    std::string error;
    check(!SaveExport::progress(root.u8string(), output.u8string(), "1.8.0", error), "missing save accepted");
    check(!fs::exists(output), "missing save wrote export");
    uint8_t empty[512] = {}, payload[512], next[512];
    check(mdkr_save_edit_slot_state(empty, 1, 1, 0, payload) == MDKR_SAVE_TOOL_OK, "create slot");
    check(mdkr_save_edit_slot_name(payload, 1, "DKR", next) == MDKR_SAVE_TOOL_OK, "name slot");
    check(mdkr_save_edit_balloon(next, 1, 0, 47, payload) == MDKR_SAVE_TOOL_OK, "balloons");
    write(source, payload, sizeof(payload));
    const auto original = read(source); const auto mtime = fs::last_write_time(source);
    check(SaveExport::progress(root.u8string(), output.u8string(), "1.8.0", error), error.c_str());
    const auto exported = read(output); uint8_t decoded[512]; MdkrSaveInputFormat format; MdkrSaveContainerMetadata metadata;
    check(mdkr_save_container_decode(reinterpret_cast<const uint8_t *>(exported.data()), exported.size(), decoded, &format, &metadata) == MDKR_SAVE_CONTAINER_OK, "container decode");
    check(format == MDKR_SAVE_INPUT_CONTAINER && std::memcmp(payload, decoded, 512) == 0, "save bytes changed");
    check(std::strcmp(metadata.app_version, "1.8.0") == 0 && metadata.created_at[0], "metadata absent");
    check(read(source) == original && fs::last_write_time(source) == mtime, "source modified");
    check(!SaveExport::progress(root.u8string(), output.u8string(), "1.8.0", error), "overwrote export");
    check(read(output) == exported, "existing export modified");
    check(!SaveExport::progress(root.u8string(), source.u8string(), "1.8.0", error), "exported over source");
    check(read(source) == original, "save replaced");
    const auto missing = root / "missing" / "new.mdkr-save";
    check(!SaveExport::progress(root.u8string(), missing.u8string(), "1.8.0", error), "missing destination accepted");
    write(source, payload, 511);
    check(!SaveExport::progress(root.u8string(), (root / "short.mdkr-save").u8string(), "1.8.0", error), "short save accepted");
    write(source, payload, 512); std::ofstream(source, std::ios::binary | std::ios::app).put('x');
    check(!SaveExport::progress(root.u8string(), (root / "long.mdkr-save").u8string(), "1.8.0", error), "long save accepted");
    payload[40] ^= 1; write(source, payload, 512);
    check(!SaveExport::progress(root.u8string(), (root / "bad.mdkr-save").u8string(), "1.8.0", error), "corrupt save accepted");
    std::cout << "save export: portable round trip, Unicode, source preservation and refusal controls PASS\n";
}
