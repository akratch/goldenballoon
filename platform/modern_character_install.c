#include "modern_character_install.h"

#include "fs_utf8.h"
#include "modern_character_asset.h"
#include "modern_character_registry.h"
#include "modern_character_text.h"
#include "sha256.h"
#include "miniz.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <process.h>
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <signal.h>
#include <unistd.h>
#endif

#define SOURCE_PACKAGE_MAX (512u * 1024u * 1024u)
#define MANIFEST_MAX (1024u * 1024u)
#define LICENSE_MAX (1024u * 1024u)
#define PORTRAIT_MAX (8u * 1024u * 1024u)
#define COMPILER_ID "mdkr-character-compiler/9"
#define LEGACY_COMPILER_ID_V7 "mdkr-character-compiler/7"
#define LEGACY_COMPILER_ID_V6 "mdkr-character-compiler/6"
#define LEGACY_COMPILER_ID_V5 "mdkr-character-compiler/5"
#define LEGACY_COMPILER_ID_V4 "mdkr-character-compiler/4"
#define LEGACY_COMPILER_ID_V3 "mdkr-character-compiler/3"
#define LEGACY_COMPILER_ID_V2 "mdkr-character-compiler/2"
#define LEGACY_COMPILER_ID_V1 "mdkr-character-compiler/1"

static _Atomic unsigned s_stage_serial;

static long native_process_id(void) {
#if defined(__EMSCRIPTEN__)
    return 1;
#elif defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int native_process_alive(long process_id) {
    if (process_id <= 0) return 1;
#if defined(__EMSCRIPTEN__)
    return process_id == 1;
#elif defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process_id);
    DWORD wait_result;
    if (process == NULL) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    wait_result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait_result == WAIT_TIMEOUT;
#else
    if (kill((pid_t)process_id, 0) == 0) return 1;
    return errno != ESRCH;
#endif
}

static int retire_stale_native_lock(const char *path) {
    FILE *input = mdkr_fopen_utf8(path, "rb");
    char magic[32] = {0};
    long owner = 0;
    int parsed;
    if (input == NULL) return 0;
    parsed = fscanf(input, "%31s %ld", magic, &owner);
    (void)fclose(input);
    if (parsed != 2 || strcmp(magic, "mdkr-native-lock-v1") != 0 ||
        native_process_alive(owner)) return 0;
    if (mdkr_remove_utf8(path) != 0) return 0;
    (void)mdkr_parent_directory_sync_utf8(path);
    return 1;
}

static FILE *open_native_lock(const char *path, const char *operation) {
    FILE *lock;
    int attempt;
    for (attempt = 0; attempt < 2; ++attempt) {
        lock = mdkr_fopen_utf8(path, "wbx");
        if (lock != NULL) {
            if (fprintf(lock, "mdkr-native-lock-v1 %ld\n%s\n",
                        native_process_id(), operation) >= 0 &&
                mdkr_file_sync(lock) == 0) {
                return lock;
            }
            (void)fclose(lock);
            (void)mdkr_remove_utf8(path);
            return NULL;
        }
        if (errno != EEXIST || !retire_stale_native_lock(path)) break;
    }
    return NULL;
}

static void result_reset(MdkrModernCharacterInstallResult *result) {
    if (result != NULL) memset(result, 0, sizeof(*result));
}

static void result_message(MdkrModernCharacterInstallResult *result,
                           const char *message) {
    if (result != NULL) {
        (void)snprintf(result->message, sizeof(result->message), "%s",
                       message != NULL ? message : "unknown character import error");
    }
}

static int copy_string_exact(char *output, size_t capacity,
                             const char *input) {
    size_t length;
    if (output == NULL || capacity == 0u || input == NULL) return 0;
    length = strlen(input);
    if (length >= capacity) return 0;
    memcpy(output, input, length + 1u);
    return 1;
}

static int id_valid(const char *id) {
    size_t index;
    size_t length;
    if (id == NULL) return 0;
    length = strlen(id);
    if (length < 2u || length > 64u ||
        !((id[0] >= 'a' && id[0] <= 'z') ||
          (id[0] >= '0' && id[0] <= '9'))) return 0;
    for (index = 1u; index < length; index++) {
        const unsigned char byte = (unsigned char)id[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return 0;
    }
    return 1;
}

static int digest_text_valid(const char *digest, int allow_empty) {
    size_t index;
    if (digest == NULL) return 0;
    if (digest[0] == '\0') return allow_empty;
    if (strlen(digest) != 64u) return 0;
    for (index = 0u; index < 64u; index++) {
        const char byte = digest[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

static int json_escape(char *output, size_t capacity, const char *input) {
    static const char hex[] = "0123456789abcdef";
    size_t write = 0u;
    const unsigned char *read = (const unsigned char *)input;
    if (output == NULL || capacity == 0u || input == NULL) return 0;
    while (*read != 0u) {
        unsigned char byte = *read++;
        if (byte == '"' || byte == '\\') {
            if (write + 2u >= capacity) return 0;
            output[write++] = '\\';
            output[write++] = (char)byte;
        } else if (byte < 0x20u) {
            if (write + 6u >= capacity) return 0;
            output[write++] = '\\';
            output[write++] = 'u';
            output[write++] = '0';
            output[write++] = '0';
            output[write++] = hex[byte >> 4u];
            output[write++] = hex[byte & 15u];
        } else {
            if (write + 1u >= capacity) return 0;
            output[write++] = (char)byte;
        }
    }
    output[write] = '\0';
    return 1;
}

static int path_join(char *output, size_t capacity,
                     const char *directory, const char *leaf) {
    const size_t length = directory != NULL ? strlen(directory) : 0u;
    const char *separator = length != 0u &&
        (directory[length - 1u] == '/' || directory[length - 1u] == '\\')
        ? "" : "/";
    const int written = snprintf(output, capacity, "%s%s%s",
                                 directory != NULL ? directory : "",
                                 separator, leaf != NULL ? leaf : "");
    return written >= 0 && (size_t)written < capacity;
}

/* A lifecycle state check: 1 with *exists answered (a missing path answers 0
 * with exists cleared on every platform, fs_utf8.h), 0 on a real error. */
static int query_path_state(const char *path, int *exists, int *regular) {
    return mdkr_path_query_utf8(path, exists, regular, NULL) == 0;
}

/* The character folder, created when missing. A failed query is a real
 * error, never a folder still to be created; whatever ends up at the path
 * must be an ordinary folder, not a link. */
static int ensure_directory(const char *directory) {
    int exists = 0;
    int is_directory = 0;
    if (mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) != 0) {
        return 0;
    }
    if (!exists) {
        if (mdkr_mkdir_utf8(directory) != 0 && errno != EEXIST) return 0;
        if (mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) != 0) {
            return 0;
        }
    }
    return exists && is_directory &&
           mdkr_path_is_link_or_reparse_utf8(directory) == 0;
}

static int open_stage_mode(const char *destination, char *stage,
                           size_t stage_size, const char *mode,
                           FILE **output) {
    unsigned attempt;
    if (stage == NULL || stage_size == 0u || mode == NULL || output == NULL) {
        return 0;
    }
    stage[0] = '\0';
    *output = NULL;
    for (attempt = 0u; attempt < 64u; attempt++) {
        const unsigned serial = atomic_fetch_add_explicit(
            &s_stage_serial, 1u, memory_order_relaxed) + 1u;
        const int written = snprintf(stage, stage_size, "%s.tmp.%ld.%u",
                                     destination, native_process_id(),
                                     serial);
        if (written < 0 || (size_t)written >= stage_size) {
            stage[0] = '\0';
            return 0;
        }
        *output = mdkr_fopen_utf8(stage, mode);
        if (*output != NULL) return 1;
        if (errno != EEXIST) {
            stage[0] = '\0';
            return 0;
        }
    }
    stage[0] = '\0';
    return 0;
}

static int open_stage(const char *destination, char *stage, size_t stage_size,
                      FILE **output) {
    return open_stage_mode(destination, stage, stage_size, "wbx", output);
}

static int write_atomic(const char *destination, const void *bytes, size_t size) {
    char stage[4096] = {0};
    FILE *output = NULL;
    int okay = open_stage(destination, stage, sizeof(stage), &output);
    if (okay && size != 0u && fwrite(bytes, 1u, size, output) != size) okay = 0;
    if (okay && mdkr_file_sync(output) != 0) okay = 0;
    if (output != NULL && fclose(output) != 0) okay = 0;
    if (okay && mdkr_move_utf8(stage, destination, 1, 1) != 0) okay = 0;
    if (!okay && stage[0] != '\0') (void)mdkr_remove_utf8(stage);
    if (okay && mdkr_parent_directory_sync_utf8(destination) != 0) {
        /* The committed file is visible and content-synced. Do not lie that it
         * vanished merely because this filesystem cannot sync a directory. */
    }
    return okay;
}

static int hash_file(FILE *file, char output[MDKR_SHA256_HEX_SIZE]);

static int stored_file_matches(const char *path, const char *expected_hash) {
    int regular = 0;
    char actual[MDKR_SHA256_HEX_SIZE];
    FILE *file;
    int matches;
    if (mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 || !regular ||
        mdkr_path_is_link_or_reparse_utf8(path) != 0) return 0;
    file = mdkr_fopen_utf8(path, "rb");
    if (file == NULL) return 0;
    matches = hash_file(file, actual) && strcmp(actual, expected_hash) == 0;
    (void)fclose(file);
    return matches;
}

static int copy_source_if_absent(FILE *input, const char *destination,
                                 const char *expected_hash) {
    int exists = 0;
    FILE *output = NULL;
    char stage[4096];
    unsigned char buffer[64u * 1024u];
    size_t count;
    int okay = 1;
    if (mdkr_path_query_utf8(destination, &exists, NULL, NULL) != 0) return 0;
    if (exists) return stored_file_matches(destination, expected_hash);
    if (input == NULL) return 0;
    clearerr(input);
    if (fseek(input, 0, SEEK_SET) != 0 ||
        !open_stage(destination, stage, sizeof(stage), &output)) {
        return 0;
    }
    while ((count = fread(buffer, 1u, sizeof(buffer), input)) != 0u) {
        if (fwrite(buffer, 1u, count, output) != count) {
            okay = 0;
            break;
        }
    }
    if (ferror(input)) okay = 0;
    if (okay && mdkr_file_sync(output) != 0) okay = 0;
    if (fclose(output) != 0) okay = 0;
    if (okay && mdkr_move_utf8(stage, destination, 0, 1) != 0) {
        /* A concurrent identical import may have won the content-addressed
         * source name. Treat an extant regular destination as success. */
        okay = stored_file_matches(destination, expected_hash);
    }
    (void)mdkr_remove_utf8(stage);
    return okay && stored_file_matches(destination, expected_hash);
}

static int hash_file(FILE *file, char output[MDKR_SHA256_HEX_SIZE]) {
    MdkrSha256 digest;
    uint8_t bytes[MDKR_SHA256_DIGEST_SIZE];
    unsigned char buffer[64u * 1024u];
    static const char hex[] = "0123456789abcdef";
    size_t count;
    unsigned index;
    if (fseek(file, 0, SEEK_SET) != 0) return 0;
    mdkr_sha256_init(&digest);
    while ((count = fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
        mdkr_sha256_update(&digest, buffer, count);
    }
    if (ferror(file)) return 0;
    mdkr_sha256_final(&digest, bytes);
    for (index = 0u; index < sizeof(bytes); index++) {
        output[index * 2u] = hex[bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[bytes[index] & 15u];
    }
    output[64] = '\0';
    return fseek(file, 0, SEEK_SET) == 0;
}

static void hash_bytes(const void *data, size_t size,
                       char output[MDKR_SHA256_HEX_SIZE]) {
    MdkrSha256 digest;
    uint8_t bytes[MDKR_SHA256_DIGEST_SIZE];
    static const char hex[] = "0123456789abcdef";
    unsigned index;
    mdkr_sha256_init(&digest);
    mdkr_sha256_update(&digest, data, size);
    mdkr_sha256_final(&digest, bytes);
    for (index = 0u; index < sizeof(bytes); index++) {
        output[index * 2u] = hex[bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[bytes[index] & 15u];
    }
    output[64] = '\0';
}

static int regular_files_equal(const char *left, const char *right) {
    FILE *left_file = NULL;
    FILE *right_file = NULL;
    char left_hash[MDKR_SHA256_HEX_SIZE];
    char right_hash[MDKR_SHA256_HEX_SIZE];
    int left_regular = 0;
    int right_regular = 0;
    int equal = 0;
    if (mdkr_path_query_utf8(left, NULL, &left_regular, NULL) != 0 ||
        mdkr_path_query_utf8(right, NULL, &right_regular, NULL) != 0 ||
        !left_regular || !right_regular ||
        mdkr_path_is_link_or_reparse_utf8(left) != 0 ||
        mdkr_path_is_link_or_reparse_utf8(right) != 0) return 0;
    left_file = mdkr_fopen_utf8(left, "rb");
    right_file = mdkr_fopen_utf8(right, "rb");
    if (left_file != NULL && right_file != NULL &&
        hash_file(left_file, left_hash) &&
        hash_file(right_file, right_hash) &&
        strcmp(left_hash, right_hash) == 0) {
        equal = 1;
    }
    if (left_file != NULL) (void)fclose(left_file);
    if (right_file != NULL) (void)fclose(right_file);
    return equal;
}

static void digest_hex(const uint8_t bytes[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned index;
    for (index = 0u; index < 32u; index++) {
        output[index * 2u] = hex[bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[bytes[index] & 15u];
    }
    output[64] = '\0';
}

static int digest_matches_hex(const uint8_t bytes[32], const char *hex) {
    static const char digits[] = "0123456789abcdef";
    unsigned index;
    if (hex == NULL || strlen(hex) != 64u) return 0;
    for (index = 0u; index < 32u; index++) {
        if (hex[index * 2u] != digits[bytes[index] >> 4u] ||
            hex[index * 2u + 1u] != digits[bytes[index] & 15u]) return 0;
    }
    return 1;
}

static int archive_entry(mz_zip_archive *archive, mz_uint index,
                         const char *expected, mz_uint64 maximum,
                         mz_zip_archive_file_stat *stat) {
    if (!mz_zip_reader_file_stat(archive, index, stat) ||
        strcmp(stat->m_filename, expected) != 0 || stat->m_is_directory ||
        stat->m_is_encrypted || stat->m_method != 0u ||
        stat->m_uncomp_size > maximum || stat->m_comp_size != stat->m_uncomp_size) {
        return 0;
    }
    return 1;
}

static int package_snapshot_close(mz_zip_archive *archive, FILE **package,
                                  char *snapshot_path) {
    int okay = 1;
    if (archive != NULL && archive->m_zip_mode != MZ_ZIP_MODE_INVALID &&
        !mz_zip_reader_end(archive)) {
        okay = 0;
    }
    if (package != NULL && *package != NULL) {
        if (fclose(*package) != 0) okay = 0;
        *package = NULL;
    }
    if (snapshot_path != NULL && snapshot_path[0] != '\0') {
        if (mdkr_remove_utf8(snapshot_path) == 0 || errno == ENOENT) {
            snapshot_path[0] = '\0';
        } else {
            okay = 0;
        }
    }
    return okay;
}

typedef struct DigestExtractState {
    MdkrSha256 *digest;
    mz_uint64 expected_offset;
} DigestExtractState;

static size_t digest_extract(void *opaque, mz_uint64 offset,
                             const void *bytes, size_t size) {
    DigestExtractState *state = (DigestExtractState *)opaque;
    if (state == NULL || state->digest == NULL ||
        offset != state->expected_offset) return 0u;
    mdkr_sha256_update(state->digest, bytes, size);
    state->expected_offset += (mz_uint64)size;
    return size;
}

static int archive_source_digest(mz_zip_archive *archive,
                                 const char *const *names,
                                 const mz_uint64 *sizes,
                                 unsigned count,
                                 const char *compiler_id,
                                 uint8_t output[32]) {
    MdkrSha256 digest;
    unsigned index;
    mdkr_sha256_init(&digest);
    mdkr_sha256_update(&digest, compiler_id, strlen(compiler_id) + 1u);
    for (index = 0u; index < count; index++) {
        uint8_t length[8];
        unsigned byte;
        DigestExtractState state;
        mdkr_sha256_update(&digest, names[index], strlen(names[index]) + 1u);
        for (byte = 0u; byte < 8u; byte++) {
            length[byte] = (uint8_t)(sizes[index] >> (byte * 8u));
        }
        mdkr_sha256_update(&digest, length, sizeof(length));
        state.digest = &digest;
        state.expected_offset = 0u;
        if (!mz_zip_reader_extract_to_callback(
                archive, index, digest_extract, &state, 0u) ||
            state.expected_offset != sizes[index]) return 0;
    }
    mdkr_sha256_final(&digest, output);
    return 1;
}

static int portable_package_operation(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result, int inspect_only,
    const char *expected_package_sha256,
    const char *expected_installed_source_digest) {
    static const char *const names_v2[] = {
        "manifest.json", "model.glb", "LICENSE.txt", "compiled.mdkc"
    };
    static const mz_uint64 caps_v2[] = {
        MANIFEST_MAX, SOURCE_PACKAGE_MAX, LICENSE_MAX, MDKR_MDKC_FILE_MAX
    };
    static const char *const names_v3[] = {
        "manifest.json", "model.glb", "portrait.png", "LICENSE.txt",
        "compiled.mdkc"
    };
    static const mz_uint64 caps_v3[] = {
        MANIFEST_MAX, SOURCE_PACKAGE_MAX, PORTRAIT_MAX, LICENSE_MAX,
        MDKR_MDKC_FILE_MAX
    };
    FILE *package = NULL;
    FILE *source_input = NULL;
    void *package_memory = NULL;
    FILE *lock = NULL;
    mz_zip_archive archive;
    mz_zip_archive_file_stat stat;
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterAsset installed_asset;
    MdkrModernCharacterDefinition definition;
    void *compiled = NULL;
    size_t compiled_size = 0u;
    char error[256];
    char hash[MDKR_SHA256_HEX_SIZE];
    char leaf[256] = {0};
    char source_leaf[256] = {0};
    char report_leaf[256] = {0};
    char source_path[4096] = {0};
    char report_path[4096] = {0};
    char cache_path[4096] = {0};
    char disabled_cache_path[4096] = {0};
    char lock_path[4096] = {0};
    char package_snapshot_base[4096] = {0};
    char package_snapshot_path[4096] = {0};
    mz_uint64 package_size = 0u;
    mz_uint64 member_sizes[5] = {0u, 0u, 0u, 0u, 0u};
    uint8_t source_digest[32];
    uint8_t legacy_source_digest_v7[32];
    uint8_t legacy_source_digest_v6[32];
    uint8_t legacy_source_digest_v5[32];
    uint8_t legacy_source_digest_v3[32];
    uint8_t legacy_source_digest_v4[32];
    uint8_t legacy_source_digest_v2[32];
    uint8_t legacy_source_digest_v1[32];
    char cache_source_digest[65] = {0};
    char installed_id[65] = {0};
    char installed_display_name[97] = {0};
    char escaped_display_name[577] = {0};
    char report_text[2048];
    int regular = 0;
    int okay = 0;
    int lock_owned = 0;
    mz_uint file_count;
    mz_uint index;
    const char *const *names = NULL;
    const mz_uint64 *caps = NULL;
    unsigned source_count = 0u;
    unsigned compiled_index = 0u;
    int source_only = 0;
    int legacy_digest_allowed = 0;
    int active_exists = 0;
    int disabled_exists = 0;
    int active_regular = 0;
    int disabled_regular = 0;
    const char *publish_cache_path = NULL;
    result_reset(result);
    error[0] = '\0';
    memset(&archive, 0, sizeof(archive));
    memset(&asset, 0, sizeof(asset));
    memset(&installed_asset, 0, sizeof(installed_asset));
    if (package_path == NULL ||
        mdkr_path_query_utf8(package_path, NULL, &regular, NULL) != 0 ||
        !regular) {
        result_message(result, "character package path is unavailable");
        goto done;
    }
    if (!inspect_only &&
        (directory == NULL || directory[0] == '\0' ||
         !ensure_directory(directory))) {
        result_message(result, "character directory is unavailable");
        goto done;
    }
    if (!inspect_only) {
        if (!path_join(package_snapshot_base,
                       sizeof(package_snapshot_base), directory,
                       ".character-package-snapshot")) {
            result_message(result,
                           "character package snapshot path is too long");
            goto done;
        }
    }
    source_input = mdkr_fopen_utf8(package_path, "rb");
    if (source_input == NULL) {
        result_message(result, "character package could not be opened");
        goto done;
    }
    if (inspect_only) {
        long measured_size;
        if (fseek(source_input, 0, SEEK_END) != 0 ||
            (measured_size = ftell(source_input)) <= 0 ||
            (unsigned long)measured_size > SOURCE_PACKAGE_MAX ||
            fseek(source_input, 0, SEEK_SET) != 0) {
            result_message(result,
                           "character package exceeds its bounded size or could not be read");
            goto done;
        }
        package_size = (mz_uint64)(unsigned long)measured_size;
        package_memory = malloc((size_t)package_size);
        if (package_memory == NULL ||
            fread(package_memory, 1u, (size_t)package_size, source_input) !=
                (size_t)package_size ||
            fgetc(source_input) != EOF || ferror(source_input)) {
            result_message(result,
                           "character package exceeds its bounded size or could not be snapshotted");
            goto done;
        }
        hash_bytes(package_memory, (size_t)package_size, hash);
    } else {
        unsigned char buffer[64u * 1024u];
        size_t count;
        if (!open_stage_mode(package_snapshot_base, package_snapshot_path,
                             sizeof(package_snapshot_path), "w+bx", &package)) {
            result_message(result, "character package could not be opened");
            goto done;
        }
        while ((count = fread(buffer, 1u, sizeof(buffer), source_input)) != 0u) {
            if (package_size > SOURCE_PACKAGE_MAX - count ||
                fwrite(buffer, 1u, count, package) != count) {
                result_message(result,
                    "character package exceeds its bounded size or could not be snapshotted");
                goto done;
            }
            package_size += count;
        }
        if (ferror(source_input) || package_size == 0u ||
            fflush(package) != 0 || !hash_file(package, hash)) {
            result_message(result, "character package exceeds its bounded size or could not be read");
            goto done;
        }
    }
    if (fclose(source_input) != 0) {
        source_input = NULL;
        result_message(result,
                       "character package snapshot could not be finalized");
        goto done;
    }
    source_input = NULL;
    if (expected_package_sha256 != NULL &&
        strcmp(hash, expected_package_sha256) != 0) {
        result_message(result,
            "the package file changed after review; validate the new bytes before installing");
        goto done;
    }
    if (!(inspect_only
              ? mz_zip_reader_init_mem(
                    &archive, package_memory, (size_t)package_size, 0u)
              : mz_zip_reader_init_cfile(
                    &archive, package, package_size, 0u))) {
        result_message(result, "character package is not a readable deterministic ZIP");
        goto done;
    }
    file_count = mz_zip_reader_get_num_files(&archive);
    if (file_count == 3u) {
        names = names_v2;
        caps = caps_v2;
        source_count = 3u;
        source_only = 1;
        legacy_digest_allowed = 1;
    } else if (file_count == 4u) {
        if (!mz_zip_reader_file_stat(&archive, 2u, &stat)) {
            result_message(result, "character package inventory could not be read");
            goto done;
        }
        if (strcmp(stat.m_filename, "portrait.png") == 0) {
            names = names_v3;
            caps = caps_v3;
            source_count = 4u;
            source_only = 1;
        } else {
            names = names_v2;
            caps = caps_v2;
            source_count = 3u;
            compiled_index = 3u;
            legacy_digest_allowed = 1;
        }
    } else if (file_count == 5u) {
        names = names_v3;
        caps = caps_v3;
        source_count = 4u;
        compiled_index = 4u;
    } else {
        result_message(result,
                       "package does not match a canonical source or portable layout");
        goto done;
    }
    for (index = 0u; index < file_count; index++) {
        if (!archive_entry(&archive, index, names[index], caps[index], &stat)) {
            result_message(result,
                           "package members are unsafe, reordered, compressed, or oversized");
            goto done;
        }
        member_sizes[index] = stat.m_uncomp_size;
        if (!source_only && index == compiled_index) {
            compiled_size = (size_t)stat.m_uncomp_size;
        }
    }
    if (source_only) {
        if (result != NULL) result->needs_compiler = 1;
        result_message(result,
            "This source-only package needs the author compiler; ask its author for a portable package or import from a developer checkout.");
        goto done;
    }
    if (!archive_source_digest(&archive, names, member_sizes, source_count,
                               COMPILER_ID, source_digest) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V7,
                               legacy_source_digest_v7) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V6,
                               legacy_source_digest_v6) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V5,
                               legacy_source_digest_v5) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V4,
                               legacy_source_digest_v4) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V3,
                               legacy_source_digest_v3) ||
        (legacy_digest_allowed &&
         (!archive_source_digest(&archive, names, member_sizes, source_count,
                                 LEGACY_COMPILER_ID_V2,
                                 legacy_source_digest_v2) ||
          !archive_source_digest(&archive, names, member_sizes, source_count,
                                 LEGACY_COMPILER_ID_V1,
                                 legacy_source_digest_v1)))) {
        result_message(result, "portable package source members could not be hashed");
        goto done;
    }
    compiled = malloc(compiled_size != 0u ? compiled_size : 1u);
    if (compiled == NULL ||
        !mz_zip_reader_extract_to_mem(&archive, compiled_index, compiled,
                                      compiled_size, 0u) ||
        !mdkr_modern_character_asset_load_memory(compiled, compiled_size,
                                                  &asset, error, sizeof(error)) ||
        !mdkr_modern_character_asset_definition(&asset, &definition)) {
        result_message(result, error[0] != '\0' ? error :
                       "embedded compiled character cache is invalid");
        goto done;
    }
    if (memcmp(asset.source_sha256, source_digest, sizeof(source_digest)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v7,
               sizeof(legacy_source_digest_v7)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v6,
               sizeof(legacy_source_digest_v6)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v5,
               sizeof(legacy_source_digest_v5)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v4,
               sizeof(legacy_source_digest_v4)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v3,
               sizeof(legacy_source_digest_v3)) != 0 &&
        (!legacy_digest_allowed ||
         (memcmp(asset.source_sha256, legacy_source_digest_v2,
                 sizeof(legacy_source_digest_v2)) != 0 &&
          memcmp(asset.source_sha256, legacy_source_digest_v1,
                 sizeof(legacy_source_digest_v1)) != 0))) {
        result_message(result,
            "embedded cache does not match this package's canonical source members");
        goto done;
    }
    {
        const char *id = mdkr_modern_character_asset_string(&asset, definition.id);
        const char *display = mdkr_modern_character_asset_string(
            &asset, definition.display_name);
        if (!id_valid(id) || display == NULL || display[0] == '\0' ||
            strlen(display) >= 97u) {
            result_message(result, "embedded character identity is unsafe");
            goto done;
        }
        if (result != NULL) {
            MdkrModernCharacterStats stats;
            (void)snprintf(result->id, sizeof(result->id), "%s", id);
            (void)mdkr_modern_character_copy_bounded_name(
                result->display_name, sizeof(result->display_name), display);
            if (!copy_string_exact(result->short_name,
                                   sizeof(result->short_name), display)) {
                result_message(result,
                               "embedded character short name exceeds its bound");
                goto done;
            }
            (void)mdkr_modern_character_copy_bounded_name(
                result->narration_name, sizeof(result->narration_name),
                display);
            (void)mdkr_modern_character_copy_bounded_name(
                result->sort_label, sizeof(result->sort_label), display);
            (void)snprintf(result->package_sha256,
                           sizeof(result->package_sha256), "%s", hash);
            digest_hex(asset.source_sha256, result->source_digest);
            result->donor = definition.donor;
            result->vehicle_mask = definition.vehicle_mask;
            mdkr_modern_character_asset_stats(&asset, &stats);
            result->vertices = stats.vertices;
            result->triangles = stats.triangles;
            result->primitives = stats.primitives;
            result->lod_levels = stats.lod_levels;
            result->materials = stats.materials;
            result->textures = stats.textures;
            result->nodes = stats.nodes;
            result->skins = stats.skins;
            result->joints = stats.joints;
            result->animations = stats.animations;
            result->animation_channels = stats.animation_channels;
            result->animation_keys = stats.animation_keys;
            {
                uint32_t semantic_index;
                for (semantic_index = 0u;
                     semantic_index < stats.semantics; ++semantic_index) {
                    MdkrModernSemantic semantic;
                    const char *semantic_name;
                    uint32_t bit;
                    if (!mdkr_modern_character_asset_semantic(
                            &asset, semantic_index, &semantic)) continue;
                    semantic_name = mdkr_modern_character_asset_string(
                        &asset, semantic.semantic);
                    bit = mdkr_modern_character_semantic_bit(semantic_name);
                    if ((semantic.flags &
                         MDKR_MODERN_SEMANTIC_DISABLED) != 0u) {
                        result->disabled_semantic_mask |= bit;
                    } else {
                        result->semantic_mask |= bit;
                    }
                }
            }
            result->rig_roles = stats.rig_roles;
            result->joint_constraints = stats.joint_constraints;
            result->secondary_chains = stats.secondary_chains;
            result->secondary_joints = stats.secondary_joints;
            result->encoded_texture_bytes = stats.encoded_texture_bytes;
            result->decoded_texture_bytes = stats.decoded_texture_bytes;
            result->ktx2_textures = stats.ktx2_textures;
            result->ktx2_source_bytes = stats.ktx2_source_bytes;
            {
                uint32_t primitive_index;
                for (primitive_index = 0u;
                     primitive_index < stats.primitives; ++primitive_index) {
                    MdkrModernPrimitive primitive;
                    if (mdkr_modern_character_asset_primitive(
                            &asset, primitive_index, &primitive) &&
                        primitive.lod < 4u) {
                        result->lod_vertices[primitive.lod] +=
                            primitive.vertex_count;
                        result->lod_triangles[primitive.lod] +=
                            primitive.index_count / 3u;
                        result->lod_primitives[primitive.lod]++;
                    }
                }
            }
            {
                MdkrModernIdentity identity;
                MdkrModernIdentityNames identity_names;
                MdkrModernRig rig;
                MdkrModernProvenance provenance;
                result->identity_present =
                    mdkr_modern_character_asset_identity(
                        &asset, &identity, NULL) && identity.portrait_size != 0u;
                if (result->identity_present) {
                    const char *short_name = identity.short_name != 0u
                        ? mdkr_modern_character_asset_string(
                              &asset, identity.short_name)
                        : display;
                    if (short_name == NULL) {
                        result_message(result,
                            "embedded character short name is unavailable");
                        goto done;
                    }
                    if (!copy_string_exact(result->short_name,
                                           sizeof(result->short_name),
                                           short_name)) {
                        result_message(result,
                            "embedded character short name exceeds its bound");
                        goto done;
                    }
                    if (mdkr_modern_character_asset_identity_names(
                            &asset, &identity_names)) {
                        const char *narration_name =
                            identity_names.narration_name != 0u
                                ? mdkr_modern_character_asset_string(
                                      &asset,
                                      identity_names.narration_name)
                                : display;
                        const char *sort_label =
                            identity_names.sort_label != 0u
                                ? mdkr_modern_character_asset_string(
                                      &asset, identity_names.sort_label)
                                : display;
                        if (narration_name == NULL || sort_label == NULL) {
                            result_message(result,
                                "embedded character identity names are unavailable");
                            goto done;
                        }
                        (void)mdkr_modern_character_copy_bounded_name(
                            result->narration_name,
                            sizeof(result->narration_name), narration_name);
                        (void)mdkr_modern_character_copy_bounded_name(
                            result->sort_label, sizeof(result->sort_label),
                            sort_label);
                    }
                }
                if (mdkr_modern_character_asset_rig(&asset, &rig)) {
                    /* Candidate protocol: 0 absent, 1 authored, 2 humanoid. */
                    result->rig_mode = rig.mode + 1u;
                    result->rig_reviewed =
                        (rig.flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
                }
                if (mdkr_modern_character_asset_provenance(
                        &asset, &provenance)) {
                    const char *spdx = mdkr_modern_character_asset_string(
                        &asset, provenance.spdx);
                    const char *attribution =
                        mdkr_modern_character_asset_string(
                            &asset, provenance.attribution);
                    const char *source_url =
                        mdkr_modern_character_asset_string(
                            &asset, provenance.source_url);
                    if (spdx == NULL || attribution == NULL ||
                        source_url == NULL) {
                        result_message(result,
                            "embedded character provenance is unavailable");
                        goto done;
                    }
                    result->provenance_present = 1u;
                    (void)snprintf(result->license_spdx,
                                   sizeof(result->license_spdx), "%s", spdx);
                    (void)snprintf(result->attribution,
                                   sizeof(result->attribution), "%s",
                                   attribution);
                    (void)snprintf(result->source_url,
                                   sizeof(result->source_url), "%s",
                                   source_url);
                }
            }
        }
        (void)snprintf(installed_id, sizeof(installed_id), "%s", id);
        (void)snprintf(installed_display_name,
                       sizeof(installed_display_name), "%s", display);
        if (inspect_only) {
            result_message(result,
                "Portable character validated without changing installed files.");
            okay = 1;
            goto done;
        }
        if (!json_escape(escaped_display_name, sizeof(escaped_display_name),
                         installed_display_name)) {
            result_message(result, "embedded character display name cannot be recorded safely");
            goto done;
        }
        (void)snprintf(source_leaf, sizeof(source_leaf), "%s.%s.mdkrchar", id, hash);
        if (!path_join(source_path, sizeof(source_path), directory, source_leaf)) {
            result_message(result, "content-addressed source path is too long");
            goto done;
        }
        (void)snprintf(report_leaf, sizeof(report_leaf), "%s.%s.json", id, hash);
        if (!path_join(report_path, sizeof(report_path), directory, report_leaf)) {
            result_message(result, "character provenance path is too long");
            goto done;
        }
        (void)snprintf(leaf, sizeof(leaf), "%s.mdkc", id);
        if (!path_join(cache_path, sizeof(cache_path), directory, leaf) ||
            snprintf(leaf, sizeof(leaf), "%s.mdkc.disabled", id) < 0 ||
            !path_join(disabled_cache_path, sizeof(disabled_cache_path),
                       directory, leaf) ||
            !path_join(lock_path, sizeof(lock_path), directory,
                       ".character-import.lock")) {
            result_message(result, "character install path is too long");
            goto done;
        }
    }
    digest_hex(asset.source_sha256, cache_source_digest);
    {
        const int report_length = snprintf(
            report_text, sizeof(report_text),
            "{\n"
            "  \"schema\": \"mdkr-character-install-v1\",\n"
            "  \"id\": \"%s\",\n"
            "  \"display_name\": \"%s\",\n"
            "  \"source_sha256\": \"%s\",\n"
            "  \"cache_source_digest\": \"%s\",\n"
            "  \"compiler\": \"portable-embedded-mdkc/1\",\n"
            "  \"source_file\": \"%s\"\n"
            "}\n",
            installed_id, escaped_display_name,
            hash, cache_source_digest, source_leaf);
        if (report_length < 0 || (size_t)report_length >= sizeof(report_text)) {
            result_message(result, "character provenance report exceeds its bound");
            goto done;
        }
    }
    lock = open_native_lock(lock_path, "install");
    if (lock == NULL) {
        result_message(result, "another character import is active (or left a stale import lock)");
        goto done;
    }
    lock_owned = 1;
    if (fclose(lock) != 0) {
        lock = NULL;
        result_message(result, "character import lock could not be committed");
        goto done;
    }
    lock = NULL;
    if (!query_path_state(cache_path, &active_exists, &active_regular) ||
        !query_path_state(disabled_cache_path, &disabled_exists,
                          &disabled_regular) ||
        (active_exists && (!active_regular ||
         mdkr_path_is_link_or_reparse_utf8(cache_path) != 0)) ||
        (disabled_exists && (!disabled_regular ||
         mdkr_path_is_link_or_reparse_utf8(disabled_cache_path) != 0)) ||
        (active_exists && disabled_exists)) {
        result_message(result,
            "installed character cache state is ambiguous or unsafe");
        goto done;
    }
    publish_cache_path = disabled_exists ? disabled_cache_path : cache_path;
    if (expected_installed_source_digest != NULL) {
        if (expected_installed_source_digest[0] == '\0') {
            if (active_exists || disabled_exists) {
                result_message(result,
                    "a character with this id was installed after review; review the update before installing");
                goto done;
            }
        } else if ((!active_exists && !disabled_exists) ||
                   !mdkr_modern_character_asset_load_file(
                       publish_cache_path, &installed_asset,
                       error, sizeof(error)) ||
                   !digest_matches_hex(installed_asset.source_sha256,
                                       expected_installed_source_digest)) {
            result_message(result,
                "the installed character changed after review; review the latest revision and try again");
            goto done;
        }
    }
    if (!copy_source_if_absent(package, source_path, hash)) {
        result_message(result,
                       "character source could not be installed transactionally");
        goto done;
    }
    /* Retire the private snapshot before publishing the mutable report/cache.
     * A cleanup failure may leave a harmless content-addressed source orphan,
     * but it can never report failure after making the character playable. */
    if (!package_snapshot_close(&archive, &package,
                                package_snapshot_path)) {
        result_message(result,
                       "character package snapshot could not be removed");
        goto done;
    }
    if (!write_atomic(report_path, report_text, strlen(report_text)) ||
        !write_atomic(publish_cache_path, compiled, compiled_size)) {
        result_message(result,
                       "character report or cache could not be installed transactionally");
        goto done;
    }
    result_message(result, disabled_exists
        ? "Portable character validated and updated while disabled."
        : "Portable character validated and installed.");
    if (result != NULL) result->enabled = disabled_exists ? 0 : 1;
    okay = 1;

done:
    if (lock != NULL) fclose(lock);
    if (lock_owned && lock_path[0] != '\0') (void)mdkr_remove_utf8(lock_path);
    mdkr_modern_character_asset_unload(&asset);
    mdkr_modern_character_asset_unload(&installed_asset);
    free(compiled);
    free(package_memory);
    if (source_input != NULL && fclose(source_input) != 0 && okay) {
        result_message(result, "character package source could not be closed");
        okay = 0;
    }
    if (!package_snapshot_close(&archive, &package,
                                package_snapshot_path)) {
        if (okay) {
            result_message(result,
                           "character package snapshot could not be removed");
        }
        okay = 0;
    }
    return okay;
}

int mdkr_modern_character_inspect_portable(
    const char *package_path, MdkrModernCharacterInstallResult *result) {
    return portable_package_operation(package_path, NULL, result, 1,
                                      NULL, NULL);
}

int mdkr_modern_character_install_portable(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result) {
    return portable_package_operation(package_path, directory, result, 0,
                                      NULL, NULL);
}

int mdkr_modern_character_install_portable_reviewed(
    const char *package_path, const char *directory,
    const char *expected_package_sha256,
    const char *expected_installed_source_digest,
    MdkrModernCharacterInstallResult *result) {
    if (!digest_text_valid(expected_package_sha256, 0) ||
        !digest_text_valid(expected_installed_source_digest, 1)) {
        result_reset(result);
        result_message(result, "reviewed import digests are invalid");
        return 0;
    }
    return portable_package_operation(
        package_path, directory, result, 0, expected_package_sha256,
        expected_installed_source_digest);
}

int mdkr_modern_character_set_enabled(
    const char *package_id, const char *directory, int enabled,
    MdkrModernCharacterInstallResult *result) {
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterDefinition definition;
    char active_leaf[256];
    char disabled_leaf[256];
    char active_path[4096];
    char disabled_path[4096];
    char lock_path[4096];
    char error[256];
    const char *source;
    const char *destination;
    const char *asset_id;
    FILE *lock = NULL;
    int regular = 0;
    int destination_exists = 0;
    int okay = 0;
    result_reset(result);
    memset(&asset, 0, sizeof(asset));
    error[0] = '\0';
    if (!id_valid(package_id) || directory == NULL ||
        snprintf(active_leaf, sizeof(active_leaf), "%s.mdkc", package_id) < 0 ||
        snprintf(disabled_leaf, sizeof(disabled_leaf), "%s.mdkc.disabled",
                 package_id) < 0 ||
        !path_join(active_path, sizeof(active_path), directory, active_leaf) ||
        !path_join(disabled_path, sizeof(disabled_path), directory,
                   disabled_leaf) ||
        !path_join(lock_path, sizeof(lock_path), directory,
                   ".character-import.lock")) {
        result_message(result, "installed character directory or id is invalid");
        return 0;
    }
    source = enabled ? disabled_path : active_path;
    destination = enabled ? active_path : disabled_path;
    lock = open_native_lock(
        lock_path, enabled ? "enable" : "disable");
    if (lock == NULL) {
        result_message(result, "another character import or lifecycle change is active");
        return 0;
    }
    if (fclose(lock) != 0) {
        lock = NULL;
        (void)mdkr_remove_utf8(lock_path);
        result_message(result, "character lifecycle lock could not be committed");
        return 0;
    }
    lock = NULL;
    if (!query_path_state(source, NULL, &regular) || !regular ||
        mdkr_path_is_link_or_reparse_utf8(source) != 0 ||
        !query_path_state(destination, &destination_exists, NULL) ||
        destination_exists) {
        result_message(result, enabled
            ? "disabled character cache is unavailable or an active cache already exists"
            : "active character cache is unavailable or a disabled cache already exists");
        goto done;
    }
    if (!mdkr_modern_character_asset_load_file(
            source, &asset, error, sizeof(error)) ||
        !mdkr_modern_character_asset_definition(&asset, &definition) ||
        (asset_id = mdkr_modern_character_asset_string(
             &asset, definition.id)) == NULL ||
        strcmp(asset_id, package_id) != 0) {
        result_message(result, error[0] != '\0' ? error :
                       "character cache identity does not match its filename");
        goto done;
    }
    if (mdkr_move_utf8(source, destination, 0, 1) == 0) {
        if (result != NULL) {
            result->enabled = enabled ? 1 : 0;
            (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
        }
        result_message(result, enabled
            ? "Custom character enabled; retained source history was unchanged."
            : "Custom character disabled; retained source history was unchanged.");
        okay = 1;
    } else {
        result_message(result, "character cache state could not be changed atomically");
    }
done:
    mdkr_modern_character_asset_unload(&asset);
    (void)mdkr_remove_utf8(lock_path);
    return okay;
}

static int content_addressed_leaf(const char *name, const char *package_id,
                                  const char *suffix) {
    const size_t id_length = strlen(package_id);
    const size_t suffix_length = strlen(suffix);
    size_t index;
    if (strlen(name) != id_length + 1u + 64u + suffix_length ||
        strncmp(name, package_id, id_length) != 0 ||
        name[id_length] != '.' ||
        strcmp(name + id_length + 1u + 64u, suffix) != 0) return 0;
    for (index = id_length + 1u; index < id_length + 1u + 64u; index++) {
        const char byte = name[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

typedef struct RemovalCandidate {
    char source[4096];
    char quarantine[4096];
    int source_revision;
    int provenance_report;
} RemovalCandidate;

static int removal_owned_leaf(const char *name, const char *package_id,
                              int *source_revision,
                              int *provenance_report) {
    const size_t id_length = strlen(package_id);
    int source = 0;
    int report = 0;
    int cache = strlen(name) == id_length + 5u &&
                strncmp(name, package_id, id_length) == 0 &&
                strcmp(name + id_length, ".mdkc") == 0;
    if (!cache) {
        cache = strlen(name) == id_length + 14u &&
                strncmp(name, package_id, id_length) == 0 &&
                strcmp(name + id_length, ".mdkc.disabled") == 0;
    }
    if (!cache) {
        source = content_addressed_leaf(name, package_id, ".mdkrchar");
        report = content_addressed_leaf(name, package_id, ".json");
    }
    if (source_revision != NULL) *source_revision = source;
    if (provenance_report != NULL) *provenance_report = report;
    return cache || source || report;
}

static int removal_candidate_compare(const void *left, const void *right) {
    const RemovalCandidate *a = (const RemovalCandidate *)left;
    const RemovalCandidate *b = (const RemovalCandidate *)right;
    const int a_cache = !a->source_revision && !a->provenance_report;
    const int b_cache = !b->source_revision && !b->provenance_report;
    if (a_cache != b_cache) return a_cache - b_cache;
    return strcmp(a->source, b->source);
}

static int rollback_removal_candidates(RemovalCandidate *candidates,
                                       size_t moved,
                                       const char *quarantine_path) {
    int failed = 0;
    while (moved > 0u) {
        --moved;
        if (mdkr_move_utf8(candidates[moved].quarantine,
                           candidates[moved].source, 0, 1) != 0) {
            failed++;
        }
    }
    if (failed == 0 && mdkr_rmdir_utf8(quarantine_path) != 0) failed++;
    (void)mdkr_parent_directory_sync_utf8(quarantine_path);
    return failed == 0;
}

int mdkr_modern_character_remove_installed_coordinated(
    const char *package_id, const char *directory,
    MdkrModernCharacterLifecycleCommit commit, void *context,
    MdkrModernCharacterInstallResult *result) {
    DIR *handle;
    struct dirent *item;
    char lock_path[4096];
    char quarantine_path[4096];
    FILE *lock = NULL;
    RemovalCandidate *candidates = NULL;
    size_t candidate_count = 0u;
    size_t candidate_capacity = 0u;
    size_t moved = 0u;
    unsigned nonce;
    int removed_sources = 0;
    int removed_reports = 0;
    int failed = 0;
    int quarantine_created = 0;
    int lifecycle_pending = 0;
    int okay = 0;
    result_reset(result);
    if (!id_valid(package_id) || directory == NULL ||
        !path_join(lock_path, sizeof(lock_path), directory,
                   ".character-import.lock")) {
        result_message(result, "installed character directory or id is invalid");
        return 0;
    }
    lock = open_native_lock(lock_path, "remove");
    if (lock == NULL) {
        result_message(result, "another character import or removal is active");
        return 0;
    }
    if (fclose(lock) != 0 || (handle = opendir(directory)) == NULL) {
        (void)mdkr_remove_utf8(lock_path);
        result_message(result, "installed character directory could not be opened");
        return 0;
    }
    while ((item = readdir(handle)) != NULL) {
        int regular = 0;
        const char *name = item->d_name;
        int source_revision = 0;
        int provenance_report = 0;
        const int candidate = removal_owned_leaf(
            name, package_id, &source_revision, &provenance_report);
        {
            if (!candidate) continue;
            if (candidate_count == candidate_capacity) {
                const size_t next_capacity = candidate_capacity == 0u
                    ? 8u : candidate_capacity * 2u;
                RemovalCandidate *grown;
                if (next_capacity < candidate_capacity ||
                    next_capacity > SIZE_MAX / sizeof(*candidates)) {
                    failed = 1;
                    break;
                }
                grown = (RemovalCandidate *)realloc(
                    candidates, next_capacity * sizeof(*candidates));
                if (grown == NULL) {
                    failed = 1;
                    break;
                }
                candidates = grown;
                candidate_capacity = next_capacity;
            }
            memset(&candidates[candidate_count], 0,
                   sizeof(candidates[candidate_count]));
            if (!path_join(candidates[candidate_count].source,
                           sizeof(candidates[candidate_count].source),
                           directory, name) ||
                mdkr_path_query_utf8(
                    candidates[candidate_count].source, NULL, &regular,
                    NULL) != 0 || !regular ||
                mdkr_path_is_link_or_reparse_utf8(
                    candidates[candidate_count].source) != 0) {
                failed = 1;
                break;
            }
            candidates[candidate_count].source_revision = source_revision;
            candidates[candidate_count].provenance_report = provenance_report;
            candidate_count++;
        }
    }
    (void)closedir(handle);
    handle = NULL;
    if (failed != 0) {
        result_message(result,
            "Character deletion was refused before changing any file because the complete owned set could not be validated.");
        goto done;
    }
    if (candidate_count == 0u) {
        result_message(result, "no regular installed files matched that character id");
        goto done;
    }
    /* The cache is the durable commit witness used by restart recovery. Move
     * every retained source/report first and the discoverable cache last. */
    qsort(candidates, candidate_count, sizeof(*candidates),
          removal_candidate_compare);
    for (nonce = 0u; nonce < 1024u; ++nonce) {
        char leaf[65u + 48u];
        int exists = 0;
        if (snprintf(leaf, sizeof(leaf), ".character-trash.%s.%u",
                     package_id, nonce) < 0 ||
            !path_join(quarantine_path, sizeof(quarantine_path),
                       directory, leaf) ||
            !query_path_state(quarantine_path, &exists, NULL)) {
            continue;
        }
        if (!exists && mdkr_mkdir_utf8(quarantine_path) == 0) {
            quarantine_created = 1;
            break;
        }
    }
    if (!quarantine_created) {
        result_message(result,
            "Character deletion could not create a private recovery quarantine; no file changed.");
        goto done;
    }
    for (moved = 0u; moved < candidate_count; ++moved) {
        const char *leaf = strrchr(candidates[moved].source, '/');
#if defined(_WIN32)
        const char *backslash = strrchr(candidates[moved].source, '\\');
        if (backslash != NULL && (leaf == NULL || backslash > leaf)) {
            leaf = backslash;
        }
#endif
        leaf = leaf != NULL ? leaf + 1 : candidates[moved].source;
        if (moved + 1u == candidate_count && candidate_count > 1u) {
            /* The last element is the cache. The pre-commit moves must reach
             * both directory journals before that final rename can retire
             * runtime discovery. */
            if (mdkr_parent_directory_sync_utf8(
                    candidates[moved - 1u].quarantine) != 0 ||
                mdkr_parent_directory_sync_utf8(quarantine_path) != 0) {
                const int rolled_back = rollback_removal_candidates(
                    candidates, moved, quarantine_path);
                result_message(result,
                    rolled_back
                        ? "Character deletion could not confirm its pre-commit directory durability and was rolled back; no file changed."
                        : "Character deletion durability failed and rollback needs recovery from the private character trash directory.");
                goto done;
            }
        }
        if (!path_join(candidates[moved].quarantine,
                       sizeof(candidates[moved].quarantine),
                       quarantine_path, leaf) ||
            mdkr_move_utf8(candidates[moved].source,
                           candidates[moved].quarantine, 0, 1) != 0) {
            const int rolled_back = rollback_removal_candidates(
                candidates, moved, quarantine_path);
            result_message(result,
                rolled_back
                    ? "Character deletion could not publish its complete quarantine and was rolled back; no file changed."
                    : "Character deletion rollback needs recovery from the private character trash directory.");
            goto done;
        }
    }
    if (candidate_count != 0u &&
        (mdkr_parent_directory_sync_utf8(
             candidates[candidate_count - 1u].quarantine) != 0 ||
         mdkr_parent_directory_sync_utf8(quarantine_path) != 0)) {
        const int rolled_back = rollback_removal_candidates(
            candidates, candidate_count, quarantine_path);
        result_message(result,
            rolled_back
                ? "Character deletion could not confirm its cache retirement and was rolled back; no file changed."
                : "Character deletion cache retirement is ambiguous and needs recovery from its durable launcher marker.");
        goto done;
    }
    for (moved = 0u; moved < candidate_count; ++moved) {
        if (candidates[moved].source_revision) removed_sources++;
        if (candidates[moved].provenance_report) removed_reports++;
        if (mdkr_remove_utf8(candidates[moved].quarantine) != 0) failed++;
    }
    if (failed == 0 && mdkr_rmdir_utf8(quarantine_path) != 0) failed++;
    if (mdkr_parent_directory_sync_utf8(quarantine_path) != 0) failed++;
    okay = 1;
    if (commit != NULL && !commit(context, failed != 0 ? 1u : 0u)) {
        lifecycle_pending = 1;
        failed++;
    }
done:
    free(candidates);
    if (mdkr_remove_utf8(lock_path) != 0 ||
        mdkr_parent_directory_sync_utf8(lock_path) != 0) {
        if (okay) failed++;
    }
    if (okay) {
        if (result != NULL) {
            (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
            result->removed_files = (unsigned)candidate_count;
            result->removed_source_revisions = (unsigned)removed_sources;
            result->removed_provenance_reports = (unsigned)removed_reports;
            result->failed_files = 0u;
            result->cleanup_pending_files = (unsigned)failed;
        }
        if (failed != 0) {
            result_message(result, lifecycle_pending
                ? "Installed character retired transactionally; launcher metadata or private trash cleanup remains pending."
                : "Installed character retired transactionally; private trash cleanup remains pending.");
        } else {
            result_message(result, "Installed character files removed transactionally.");
        }
    }
    return okay;
}

int mdkr_modern_character_remove_installed(
    const char *package_id, const char *directory,
    MdkrModernCharacterInstallResult *result) {
    return mdkr_modern_character_remove_installed_coordinated(
        package_id, directory, NULL, NULL, result);
}

static int reconcile_removal_quarantine(
    const char *package_id, const char *directory,
    const char *quarantine_path, int retire, unsigned *changed) {
    DIR *handle;
    struct dirent *item;
    int valid = 1;

    /* Validate the complete quarantine before changing any entry. A private
     * directory containing anything not owned by this exact package is never
     * treated as launcher trash. */
    handle = opendir(quarantine_path);
    if (handle == NULL) return 0;
    while ((item = readdir(handle)) != NULL) {
        char source[4096];
        char destination[4096];
        int exists = 0;
        int regular = 0;
        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0) continue;
        if (!removal_owned_leaf(item->d_name, package_id, NULL, NULL) ||
            !path_join(source, sizeof(source), quarantine_path,
                       item->d_name) ||
            mdkr_path_query_utf8(source, NULL, &regular, NULL) != 0 ||
            !regular || mdkr_path_is_link_or_reparse_utf8(source) != 0) {
            valid = 0;
            break;
        }
        if (!retire) {
            if (!path_join(destination, sizeof(destination), directory,
                           item->d_name) ||
                !query_path_state(destination, &exists, NULL) ||
                (exists && !regular_files_equal(source, destination))) {
                valid = 0;
                break;
            }
        }
    }
    (void)closedir(handle);
    if (!valid) return 0;

    handle = opendir(quarantine_path);
    if (handle == NULL) return 0;
    while ((item = readdir(handle)) != NULL) {
        char source[4096];
        char destination[4096];
        int operation;
        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0) continue;
        if (!path_join(source, sizeof(source), quarantine_path,
                       item->d_name)) {
            valid = 0;
            break;
        }
        if (retire) {
            operation = mdkr_remove_utf8(source);
        } else if (path_join(destination, sizeof(destination), directory,
                             item->d_name)) {
            int exists = 0;
            if (!query_path_state(destination, &exists, NULL)) {
                operation = -1;
            } else if (exists) {
                operation = regular_files_equal(source, destination)
                    ? mdkr_remove_utf8(source) : -1;
            } else {
                operation = mdkr_move_utf8(source, destination, 0, 1);
            }
        } else {
            operation = -1;
        }
        if (operation != 0) {
            valid = 0;
            break;
        }
        if (changed != NULL) ++*changed;
    }
    (void)closedir(handle);
    if (valid && mdkr_rmdir_utf8(quarantine_path) != 0) valid = 0;
    if (mdkr_parent_directory_sync_utf8(quarantine_path) != 0) valid = 0;
    return valid;
}

int mdkr_modern_character_reconcile_removal_coordinated(
    const char *package_id, const char *directory, int retire,
    MdkrModernCharacterLifecycleCommit commit, void *context,
    MdkrModernCharacterInstallResult *result) {
    char lock_path[4096];
    FILE *lock = NULL;
    unsigned nonce;
    unsigned changed = 0u;
    int lifecycle_pending = 0;
    int okay = 1;

    result_reset(result);
    if (!id_valid(package_id) || directory == NULL ||
        !path_join(lock_path, sizeof(lock_path), directory,
                   ".character-import.lock")) {
        result_message(result, "character removal recovery path or id is invalid");
        return 0;
    }
    lock = open_native_lock(
        lock_path, retire ? "remove-finish" : "remove-rollback");
    if (lock == NULL) {
        result_message(result,
            "another character import or removal recovery is active");
        return 0;
    }
    if (fclose(lock) != 0) {
        (void)mdkr_remove_utf8(lock_path);
        result_message(result, "character removal recovery lock could not be committed");
        return 0;
    }
    lock = NULL;

    for (nonce = 0u; nonce < 1024u; ++nonce) {
        char leaf[65u + 48u];
        char quarantine_path[4096];
        int exists = 0;
        int is_directory = 0;
        if (snprintf(leaf, sizeof(leaf), ".character-trash.%s.%u",
                     package_id, nonce) < 0 ||
            !path_join(quarantine_path, sizeof(quarantine_path),
                       directory, leaf) ||
            !query_path_state(quarantine_path, &exists, NULL)) {
            okay = 0;
            break;
        }
        if (!exists) continue;
        if (mdkr_path_query_utf8(quarantine_path, NULL, NULL,
                                 &is_directory) != 0 || !is_directory ||
            mdkr_path_is_link_or_reparse_utf8(quarantine_path) != 0 ||
            !reconcile_removal_quarantine(
                package_id, directory, quarantine_path, retire, &changed)) {
            okay = 0;
            break;
        }
    }
    if (okay && commit != NULL && !commit(context, 0u)) {
        lifecycle_pending = 1;
        okay = 0;
    }
    if (mdkr_remove_utf8(lock_path) != 0 ||
        mdkr_parent_directory_sync_utf8(lock_path) != 0) okay = 0;
    if (result != NULL) {
        (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
        result->removed_files = retire ? changed : 0u;
        result->cleanup_pending_files = okay ? 0u : 1u;
    }
    result_message(
        result,
        okay ? retire
                   ? "Interrupted character deletion cleanup completed."
                   : "Interrupted character deletion rolled back safely."
             : lifecycle_pending
                   ? "Character deletion file recovery completed, but launcher metadata cleanup remains pending."
             : retire
                   ? "Character deletion recovery still has private trash pending."
                   : "Character deletion rollback could not restore every quarantined file without overwrite.");
    return okay;
}

int mdkr_modern_character_reconcile_removal(
    const char *package_id, const char *directory, int retire,
    MdkrModernCharacterInstallResult *result) {
    return mdkr_modern_character_reconcile_removal_coordinated(
        package_id, directory, retire, NULL, NULL, result);
}
