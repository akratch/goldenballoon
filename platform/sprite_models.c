#include "sprite_models.h"
#include "mod_texture_store.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct ModelAtlas {
    int pack_index, width, height;
    unsigned refs;
    size_t bytes;
    char path[256];
    uint32_t id;
    unsigned char *rgba;
} ModelAtlas;

typedef struct ModelSlot {
    unsigned sprite;
    const char *family, *stem, *texture;
    struct GfxModernMesh meshes[2];
    ModelAtlas *atlas;
    int ready;
    uint64_t draws[2];
} ModelSlot;

static ModelSlot slots[] = {
    {.sprite=107, .family="beach-tree", .stem="", .texture="4245414348330001"},
    {.sprite=108, .family="blueberry", .stem="", .texture="424c554542330001"},
    {.sprite=113, .family="rubber-tree", .stem="", .texture="5255424252330001"},
    {.sprite=114, .family="palm", .stem="canopy-", .texture="50414c4d33440001"},
    {.sprite=115, .family="palm", .stem="", .texture="50414c4d33440001"},
    {.sprite=116, .family="palm", .stem="canopy-", .texture="50414c4d33440001"},
    {.sprite=147, .family="balloons", .stem="", .texture="42414c4c33440093"},
    {.sprite=148, .family="balloons", .stem="", .texture="42414c4c33440094"},
    {.sprite=149, .family="balloons", .stem="", .texture="42414c4c33440095"},
    {.sprite=150, .family="balloons", .stem="", .texture="42414c4c33440096"},
    {.sprite=151, .family="balloons", .stem="", .texture="42414c4c33440097"},
    {.sprite=154, .family="balloons", .stem="collectible-", .texture="42414c4c3344009a"},
    {.sprite=155, .family="balloons", .stem="collectible-", .texture="42414c4c3344009b"},
    {.sprite=156, .family="banana", .stem="", .texture="42414e414e413301"},
};
static uint32_t next_mesh_id, next_texture_id;
static int enabled = 1, count;
static ModelAtlas atlases[sizeof(slots)/sizeof(slots[0])];
static unsigned atlas_count;
static size_t atlas_bytes;

#define MDKR_MODEL_ATLAS_FILE_MAX  (16u * 1024u * 1024u)
#define MDKR_MODEL_ATLAS_BYTES_MAX (64u * 1024u * 1024u)
#define MDKR_MODEL_ATLAS_TOTAL_MAX (256u * 1024u * 1024u)

static void *read_file(MdkrModSource *source, const char *path, size_t limit, size_t *size) {
    void *bytes;
    *size = 0;
    if (mdkr_mod_source_read(source, path, NULL, 0, size) != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL ||
        !*size || *size > limit) return NULL;
    bytes = malloc(*size);
    if (bytes && mdkr_mod_source_read(source, path, bytes, *size, size) == MDKR_MOD_SOURCE_OK) return bytes;
    free(bytes); return NULL;
}

static void clear_slot(ModelSlot *slot) {
    mdkr_sprite_model_mesh_free(&slot->meshes[0]);
    mdkr_sprite_model_mesh_free(&slot->meshes[1]);
    if (slot->atlas != NULL && slot->atlas->refs > 0) {
        ModelAtlas *atlas = slot->atlas;
        atlas->refs--;
        if (atlas->refs == 0) {
            stbi_image_free(atlas->rgba);
            atlas_bytes -= atlas->bytes;
            memset(atlas, 0, sizeof(*atlas));
        }
    }
    slot->atlas = NULL; slot->ready = 0;
    memset(slot->draws, 0, sizeof(slot->draws));
}

void mdkr_sprite_models_shutdown(void) {
    unsigned i;
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i) {
        ModelSlot *s = &slots[i];
        if (s->draws[0] || s->draws[1])
            fprintf(stderr, "[MODELS] sprite=%u near=%llu far=%llu\n", s->sprite,
                    (unsigned long long)s->draws[0], (unsigned long long)s->draws[1]);
        clear_slot(s);
    }
    count = 0;
    for (i = 0; i < atlas_count; ++i) {
        if (atlases[i].rgba != NULL) stbi_image_free(atlases[i].rgba);
    }
    memset(atlases, 0, sizeof(atlases));
    atlas_count = 0;
    atlas_bytes = 0;
}

/* The pack index is part of the identity: identical paths in two packs may
 * hold different artwork. Atlas IDs keep increasing after CPU storage is freed,
 * so a renderer retaining old commands never mistakes a recycled pointer for
 * the new image. Near/far meshes and both canopy sizes share one decoded atlas. */
static ModelAtlas *load_atlas(MdkrModFile *file, const char *path) {
    unsigned i;
    size_t size;
    void *bytes;
    int width, height, channels, dw, dh;
    uint64_t decoded_bytes;
    unsigned char *rgba;
    ModelAtlas *atlas;
    if (file == NULL || file->source == NULL || path == NULL ||
        strlen(path) >= sizeof(atlases[0].path)) return NULL;
    for (i = 0; i < atlas_count; ++i)
        if (atlases[i].rgba != NULL && atlases[i].pack_index == file->pack_index &&
            !strcmp(atlases[i].path, path)) {
            atlases[i].refs++;
            return &atlases[i];
        }
    if (next_texture_id == UINT32_MAX) return NULL;
    bytes = read_file(file->source, path, MDKR_MODEL_ATLAS_FILE_MAX, &size);
    if (!bytes) return NULL;
    if (size < 8 || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) ||
        !stbi_info_from_memory(bytes, (int)size, &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 4096 || height > 4096) { free(bytes); return NULL; }
    decoded_bytes = (uint64_t)(unsigned)width * (uint64_t)(unsigned)height * 4u;
    if (decoded_bytes > MDKR_MODEL_ATLAS_BYTES_MAX ||
        decoded_bytes > MDKR_MODEL_ATLAS_TOTAL_MAX - atlas_bytes) {
        free(bytes);
        return NULL;
    }
    rgba = stbi_load_from_memory(bytes, (int)size, &dw, &dh, &channels, 4);
    free(bytes);
    if (!rgba || dw != width || dh != height) { stbi_image_free(rgba); return NULL; }
    for (i = 0; i < atlas_count; ++i) {
        if (atlases[i].rgba == NULL) break;
    }
    if (i == atlas_count) {
        if (atlas_count == sizeof(atlases)/sizeof(atlases[0])) {
            stbi_image_free(rgba);
            return NULL;
        }
        atlas_count++;
    }
    atlas = &atlases[i];
    atlas->pack_index = file->pack_index;
    snprintf(atlas->path, sizeof(atlas->path), "%s", path);
    atlas->width = width; atlas->height = height;
    atlas->rgba = rgba; atlas->bytes = (size_t)decoded_bytes;
    atlas->refs = 1; atlas->id = ++next_texture_id;
    atlas_bytes += atlas->bytes;
    return atlas;
}

void mdkr_sprite_models_init(const MdkrModRegistry *registry) {
    unsigned i;
    const char *opt = getenv("MDKR_SPRITE_MODELS");
    mdkr_sprite_models_shutdown();
    enabled = !opt || strcmp(opt, "0");
    for (i = 0; registry && i < sizeof(slots)/sizeof(slots[0]); ++i) {
        ModelSlot *s = &slots[i];
        MdkrModFile file = {0};
        char path[256];
        size_t size;
        void *bytes;
        int lod;
        snprintf(path, sizeof path, "models/%s/%snear.dkrmesh", s->family, s->stem);
        if (!mdkr_mod_registry_open_file(registry, path, &file)) continue;
        /* Geometry and atlas come from ONE selected pack. Mixing families
         * across priorities can silently apply the wrong UV layout. */
        for (lod = 0; lod < 2; ++lod) {
            snprintf(path, sizeof path, "models/%s/%s%s.dkrmesh", s->family, s->stem, lod ? "far" : "near");
            bytes = read_file(file.source, path, 4u*1024u*1024u, &size);
            if (!bytes) goto reject;
            if (!mdkr_sprite_model_parse(bytes, size, &s->meshes[lod])) { free(bytes); goto reject; }
            free(bytes);
        }
        snprintf(path, sizeof path, "models/%s/%s.png", s->family, s->texture);
        s->atlas = load_atlas(&file, path);
        if (!s->atlas) goto reject;
        for (lod = 0; lod < 2; ++lod) {
            if (next_mesh_id == UINT32_MAX) goto reject;
            s->meshes[lod].mesh_id = ++next_mesh_id;
            s->meshes[lod].texture_id = s->atlas->id;
            s->meshes[lod].tex_rgba = s->atlas->rgba;
            s->meshes[lod].tex_w = s->atlas->width; s->meshes[lod].tex_h = s->atlas->height;
        }
        s->ready = 1; ++count;
        fprintf(stderr, "[MODELS] ready sprite=%u %s triangles=%u/%u\n", s->sprite, s->family,
                s->meshes[0].idx_count/3, s->meshes[1].idx_count/3);
        mdkr_mod_registry_close_file(&file);
        continue;
reject:
        fprintf(stderr, "[MODELS] sprite=%u invalid/incomplete family in %s; keeping original sprite\n",
                s->sprite, registry->entries[file.pack_index].manifest.name);
        clear_slot(s); mdkr_mod_registry_close_file(&file);
    }
    if (count) {
        size_t bytes = 0;
        for (i = 0; i < atlas_count; ++i) bytes += (size_t)atlases[i].width * atlases[i].height * 4;
        fprintf(stderr, "[MODELS] %d sprite replacements loaded; F8 toggles 3D models\n", count);
        fprintf(stderr, "[MODELS] atlases=%u decodedBytes=%zu\n", atlas_count, bytes);
    }
}

void mdkr_sprite_models_toggle(void) {
    if (!count) return;
    enabled = !enabled;
    fprintf(stderr, "[MODELS] 3D replacements %s\n", enabled ? "on" : "off");
}
int mdkr_sprite_models_enabled(void) { return enabled && mdkr_mod_texture_enabled(); }
int mdkr_sprite_models_count(void) { return count; }
int mdkr_sprite_models_ready(unsigned sprite) {
    unsigned i;
    if (!mdkr_sprite_models_enabled()) return 0;
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i)
        if (slots[i].sprite == sprite) return slots[i].ready;
    return 0;
}
struct GfxModernMesh *mdkr_sprite_models_mesh(unsigned sprite, int far) {
    unsigned i;
    /* A retained command keeps the authored decision even if F8 was pressed
     * after it was built. The next authored list makes the toggle visible. */
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i)
        if (slots[i].sprite == sprite && slots[i].ready) return &slots[i].meshes[far != 0];
    return NULL;
}
void mdkr_sprite_models_note_draw(unsigned sprite, int far) {
    unsigned i;
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i)
        if (slots[i].sprite == sprite) { ++slots[i].draws[far != 0]; return; }
}
