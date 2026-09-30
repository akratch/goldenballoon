#include "custom_track_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "manifest line %d\n", __LINE__); return 1; } } while (0)
static MdkrTrackManifest *parse(const char *s) { return mdkr_track_manifest_parse(s, strlen(s)); }
int main(void) {
    const char *valid = "{\"schemaVersion\":1,\"id\":\"lake-2\",\"author\":\"A\\u00e9\\ud83c\\udfce\","
        "\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"},"
        "{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"maps/a.bin\",\"slot\":\"structure\"},"
        "{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"maps/b.bin\",\"slot\":\"collectables\"},"
        "{\"section\":\"TEXTURES_3D\",\"file\":\"t1.bin\"},"
        "{\"section\":\"TEXTURES_3D\",\"file\":\"t2.bin\"}],"
        "\"hdTexturePack\":{\"file\":\"lake-hd.zip\",\"textureDigest\":\"abc\"},"
        "\"extra\":{\"future\":[true,false,null,1.5e-2,\"\\u0000\"]}}";
    MdkrTrackManifest *m = parse(valid);
    CHECK(m && m->count == 5 && !strcmp(m->name, "lake-2"));
    CHECK(!strcmp(m->author, "A\xc3\xa9\xf0\x9f\x8f\x8e"));
    CHECK(m->adds[1].slot == MDKR_TRACK_MAP_STRUCTURE && m->adds[2].slot == MDKR_TRACK_MAP_COLLECTABLES);
    CHECK(!strcmp(m->adds[4].file, "t2.bin") && !strcmp(m->hd_file, "lake-hd.zip"));
    mdkr_track_manifest_destroy(m);
    for (size_t i = 0; i < strlen(valid); ++i) CHECK(mdkr_track_manifest_parse(valid, i) == NULL);
    const char *prefix = "{\"schemaVersion\":1,\"id\":\"x\",\"adds\":[{\"section\":\"TEXTURES_3D\",\"file\":\"a.bin\"}]";
    char text[4096];
    const char *bad_fields[] = {
        ",\"schemaVersion\":1}", ",\"id\":\"y\"}", ",\"name\":null}", ",\"author\":3}",
        ",\"name\":\"x\\u0000y\"}", ",\"name\":\"\\ud800\"}", ",\"name\":\"\xc0\xaf\"}",
        ",\"hdTexturePack\":false}", ",\"hdTexturePack\":{\"file\":\"../x\",\"textureDigest\":\"a\"}}",
        ",\"hdTexturePack\":{\"file\":\"x/y\",\"textureDigest\":\"a\"}}", ",}", "}garbage"
    };
    for (size_t i = 0; i < sizeof bad_fields / sizeof *bad_fields; ++i) {
        snprintf(text, sizeof text, "%s%s", prefix, bad_fields[i]);
        CHECK(parse(text) == NULL);
    }
    const char *bad_ids[] = {"", "../x", "/x", "a.b", "CON", "com1", "LPT9", "a:b", "-x"};
    for (size_t i = 0; i < sizeof bad_ids / sizeof *bad_ids; ++i) {
        snprintf(text, sizeof text, "{\"schemaVersion\":1,\"id\":\"%s\",\"adds\":[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\"}]}", bad_ids[i]);
        CHECK(parse(text) == NULL);
    }
    const char *bad_adds[] = {
        "[]", "null", "[false]", "[{}]",
        "[{\"section\":\"BOGUS\",\"file\":\"a\"}]",
        "[{\"section\":\"LEVEL_NAMES\",\"file\":\"../a\"}]",
        "[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\",\"file\":\"b\"}]",
        "[{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"a\"}]",
        "[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\",\"slot\":\"structure\"}]",
        "[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\"},{\"section\":\"LEVEL_NAMES\",\"file\":\"b\"}]",
        "[{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"a\",\"slot\":\"structure\"},{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"b\",\"slot\":\"structure\"}]",
        "[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\"},]"
    };
    for (size_t i = 0; i < sizeof bad_adds / sizeof *bad_adds; ++i) {
        snprintf(text, sizeof text, "{\"schemaVersion\":1,\"id\":\"x\",\"adds\":%s}", bad_adds[i]);
        CHECK(parse(text) == NULL);
    }
    const char *bad_versions[] = {"0", "2", "1.0", "1e0", "true", "\"1\"", "null", "[]"};
    for (size_t i = 0; i < sizeof bad_versions / sizeof *bad_versions; ++i) {
        snprintf(text, sizeof text, "{\"schemaVersion\":%s,\"id\":\"x\",\"adds\":[{\"section\":\"LEVEL_NAMES\",\"file\":\"a\"}]}", bad_versions[i]);
        CHECK(parse(text) == NULL);
    }
    CHECK(mdkr_track_manifest_parse(NULL, 0) == NULL);
    CHECK(mdkr_track_manifest_parse(valid, MDKR_TRACK_MANIFEST_BYTES_MAX + 1) == NULL);
    /* Unknown metadata cannot bypass nesting/work limits. */
    snprintf(text, sizeof text, "%s,\"extra\":", prefix);
    for (int i = 0; i < 17; ++i) strcat(text, "[");
    strcat(text, "0");
    for (int i = 0; i < 17; ++i) strcat(text, "]");
    strcat(text, "}");
    CHECK(parse(text) == NULL);
    char *large = calloc(1, MDKR_TRACK_MANIFEST_BYTES_MAX);
    CHECK(large != NULL);
    strcpy(large, "{\"schemaVersion\":1,\"id\":\"x\",\"adds\":[");
    for (unsigned i = 0; i < MDKR_TRACK_ADDS_MAX; ++i) {
        if (i) strcat(large, ",");
        strcat(large, "{\"section\":\"TEXTURES_3D\",\"file\":\"a\"}");
    }
    size_t end = strlen(large);
    strcat(large, "]}");
    m = parse(large);
    CHECK(m && m->count == MDKR_TRACK_ADDS_MAX);
    mdkr_track_manifest_destroy(m);
    strcpy(large + end, ",{\"section\":\"TEXTURES_3D\",\"file\":\"a\"}]}");
    CHECK(parse(large) == NULL);
    free(large);
    mdkr_track_manifest_destroy(NULL);
    puts("custom track manifest: PASS");
    return 0;
}
