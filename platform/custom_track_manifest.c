#include "custom_track_manifest.h"
#include "bounded_json.h"
#include "mod_source.h"
#include <stdlib.h>
#ifdef MDKR_TRACK_PACKAGE_TEST_ALLOCATOR
void *mdkr_track_test_calloc(size_t count, size_t size);
void mdkr_track_test_free(void *pointer);
#define calloc mdkr_track_test_calloc
#define free mdkr_track_test_free
#endif

static int take(JsonParser *p, unsigned char c) {
    json_skip_space(p);
    if (p->cursor == p->end || *p->cursor != c) return 0;
    ++p->cursor;
    return 1;
}

/* Object delimiter state rejects trailing commas and missing separators. */
static int member(JsonParser *p, int *first, char key[64]) {
    json_skip_space(p);
    if (p->cursor == p->end) return -1;
    if (*p->cursor == '}') { ++p->cursor; return 0; }
    if (!*first && !take(p, ',')) return -1;
    *first = 0;
    json_skip_space(p);
    if (!json_parse_string(p, key, 64, NULL) || !take(p, ':')) return -1;
    json_skip_space(p);
    return 1;
}

static int unique(unsigned *seen, unsigned bit) {
    if (*seen & bit) return 0;
    *seen |= bit;
    return 1;
}

static int identifier(const char *id) {
    size_t n = strlen(id);
    if (!n) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (i && (c == '-' || c == '_')))) return 0;
    }
    /* Device names remain reserved on Windows even with a file extension. */
    char upper[96];
    for (size_t i = 0; i <= n; ++i)
        upper[i] = id[i] >= 'a' && id[i] <= 'z' ? id[i] - 'a' + 'A' : id[i];
    if (!strcmp(upper, "CON") || !strcmp(upper, "PRN") ||
        !strcmp(upper, "AUX") || !strcmp(upper, "NUL")) return 0;
    return !(n == 4 && (!memcmp(upper, "COM", 3) || !memcmp(upper, "LPT", 3)) &&
             upper[3] >= '1' && upper[3] <= '9');
}

static int addition(JsonParser *p, MdkrTrackAddition *out) {
    static const char *const sections[] = {
        "LEVEL_HEADERS", "LEVEL_OBJECT_MAPS", "LEVEL_NAMES", "LEVEL_MODELS", "TEXTURES_3D"
    };
    char key[64], section[64] = {0}, slot[32] = {0};
    int first = 1, state;
    unsigned seen = 0;
    if (!take(p, '{')) return 0;
    while ((state = member(p, &first, key)) > 0) {
        if (!strcmp(key, "section")) {
            if (!unique(&seen, 1) || !json_parse_string(p, section, sizeof section, NULL)) return 0;
        } else if (!strcmp(key, "file")) {
            if (!unique(&seen, 2) || !json_parse_string(p, out->file, sizeof out->file, NULL)) return 0;
        } else if (!strcmp(key, "slot")) {
            if (!unique(&seen, 4) || !json_parse_string(p, slot, sizeof slot, NULL)) return 0;
        } else if (!json_skip_value(p)) return 0;
    }
    if (state < 0 || (seen & 3) != 3 || !mdkr_mod_source_path_is_safe(out->file)) return 0;
    unsigned i;
    for (i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i)
        if (!strcmp(section, sections[i])) break;
    if (i == MDKR_CUSTOM_TRACK_SECTION_COUNT) return 0;
    out->section = (MdkrCustomTrackSectionKind)i;
    if (i == MDKR_CUSTOM_TRACK_OBJECT_MAPS) {
        if (!strcmp(slot, "structure")) out->slot = MDKR_TRACK_MAP_STRUCTURE;
        else if (!strcmp(slot, "collectables")) out->slot = MDKR_TRACK_MAP_COLLECTABLES;
        else return 0;
    } else if (seen & 4) return 0;
    return 1;
}

static int additions(JsonParser *p, MdkrTrackManifest *m) {
    unsigned roles = 0;
    if (!take(p, '[')) return 0;
    for (;;) {
        if (m->count == MDKR_TRACK_ADDS_MAX || !addition(p, &m->adds[m->count])) return 0;
        const MdkrTrackAddition *a = &m->adds[m->count++];
        /* Texture repetition is expected; other roles identify one course. */
        if (a->section != MDKR_CUSTOM_TRACK_TEXTURES) {
            static const unsigned section_roles[] = {1u, 0u, 4u, 8u, 0u};
            unsigned role = a->section == MDKR_CUSTOM_TRACK_OBJECT_MAPS ?
                (a->slot == MDKR_TRACK_MAP_STRUCTURE ? 32u : 64u) : section_roles[a->section];
            if (!unique(&roles, role)) return 0;
        }
        json_skip_space(p);
        if (p->cursor != p->end && *p->cursor == ']') { ++p->cursor; return 1; }
        if (!take(p, ',')) return 0;
    }
}

static int companion(JsonParser *p, MdkrTrackManifest *m) {
    char key[64];
    unsigned seen = 0;
    int first = 1, state;
    if (!take(p, '{')) return 0;
    while ((state = member(p, &first, key)) > 0) {
        if (!strcmp(key, "file")) {
            if (!unique(&seen, 1) || !json_parse_string(p, m->hd_file, sizeof m->hd_file, NULL)) return 0;
        } else if (!strcmp(key, "textureDigest")) {
            if (!unique(&seen, 2) || !json_parse_string(p, m->hd_digest, sizeof m->hd_digest, NULL)) return 0;
        } else if (!json_skip_value(p)) return 0;
    }
    return state == 0 && seen == 3 && m->hd_digest[0] &&
        mdkr_mod_source_path_is_safe(m->hd_file) && !strchr(m->hd_file, '/');
}

MdkrTrackManifest *mdkr_track_manifest_parse(const void *bytes, size_t size) {
    if (!bytes || !size || size > MDKR_TRACK_MANIFEST_BYTES_MAX) return NULL;
    JsonParser p = {bytes, (const uint8_t *)bytes + size, 0};
    MdkrTrackManifest *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    char key[64];
    unsigned seen = 0;
    int first = 1, state;
    if (!take(&p, '{')) goto fail;
    while ((state = member(&p, &first, key)) > 0) {
        if (!strcmp(key, "schemaVersion")) {
            const uint8_t *begin = p.cursor;
            if (!unique(&seen, 1) || !json_skip_number(&p) || p.cursor - begin != 1 || *begin != '1') goto fail;
        } else if (!strcmp(key, "id")) {
            if (!unique(&seen, 2) || !json_parse_string(&p, m->id, sizeof m->id, NULL)) goto fail;
        } else if (!strcmp(key, "name")) {
            if (!unique(&seen, 4) || !json_parse_string(&p, m->name, sizeof m->name, NULL)) goto fail;
        } else if (!strcmp(key, "author")) {
            if (!unique(&seen, 8) || !json_parse_string(&p, m->author, sizeof m->author, NULL)) goto fail;
        } else if (!strcmp(key, "adds")) {
            if (!unique(&seen, 16) || !additions(&p, m)) goto fail;
        } else if (!strcmp(key, "hdTexturePack")) {
            if (!unique(&seen, 32) || !companion(&p, m)) goto fail;
        } else if (!json_skip_value(&p)) goto fail;
    }
    json_skip_space(&p);
    if (state < 0 || p.cursor != p.end || (seen & 19) != 19 || !identifier(m->id)) goto fail;
    if (!(seen & 4)) memcpy(m->name, m->id, strlen(m->id) + 1);
    return m;
fail:
    free(m);
    return NULL;
}

void mdkr_track_manifest_destroy(MdkrTrackManifest *manifest) { free(manifest); }
