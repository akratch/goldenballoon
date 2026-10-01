#include "sprite_models.h"
#include "mod_texture_store.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "stb_image_alloc.h"
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

/* One parsed .dkrmesh, shared by every slot that names the same file in the
 * same pack (the five balloon colours, both canopy sizes). It owns the vertex
 * and index arrays; slot meshes borrow them. */
typedef struct ModelGeometry {
    int pack_index;
    unsigned refs;
    uint32_t id;
    char path[256];
    struct GfxModernMesh mesh;
} ModelGeometry;

typedef struct ModelSlot {
    unsigned sprite;
    const char *family, *stem, *texture;
    struct GfxModernMesh meshes[2];
    ModelGeometry *geometry[2];
    ModelAtlas *atlas;
    int ready;
    uint64_t draws[2];
    /* Pose census: authored and replayed (interpolated) model poses, poses
     * attached to a trunk, and which of the 256 phases were seen. */
    uint64_t authored, replayed, attached;
    uint32_t phases[8];
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
static uint32_t next_mesh_id, next_texture_id, next_geometry_id;
static int enabled = 1, count;
static ModelAtlas atlases[sizeof(slots)/sizeof(slots[0])];
static unsigned atlas_count;
static size_t atlas_bytes;
static ModelGeometry geometries[2 * sizeof(slots)/sizeof(slots[0])];
static unsigned geometry_count, geometry_parses;
/* Material classes of model draws since the last init: fading (prim alpha
 * below one), fading draws that wrote depth, and draws tinted toward the
 * level light colour. */
static uint64_t material_draws[3];

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

static void release_geometry(ModelGeometry *geometry) {
    if (geometry == NULL || geometry->refs == 0) return;
    if (--geometry->refs == 0) {
        mdkr_sprite_model_mesh_free(&geometry->mesh);
        memset(geometry, 0, sizeof(*geometry));
    }
}

/* Parse each (pack, path) once. IDs keep increasing across reloads, so a
 * backend that still holds old buffers never mistakes them for new ones. */
static ModelGeometry *load_geometry(MdkrModFile *file, const char *path) {
    unsigned i;
    size_t size;
    void *bytes;
    ModelGeometry *geometry = NULL;
    if (file == NULL || file->source == NULL || path == NULL ||
        strlen(path) >= sizeof(geometries[0].path)) return NULL;
    for (i = 0; i < geometry_count; ++i)
        if (geometries[i].refs != 0 && geometries[i].pack_index == file->pack_index &&
            !strcmp(geometries[i].path, path)) {
            geometries[i].refs++;
            return &geometries[i];
        }
    if (next_geometry_id == UINT32_MAX) return NULL;
    for (i = 0; i < geometry_count; ++i)
        if (geometries[i].refs == 0) { geometry = &geometries[i]; break; }
    if (geometry == NULL) {
        if (geometry_count == sizeof(geometries)/sizeof(geometries[0])) return NULL;
        geometry = &geometries[geometry_count++];
    }
    bytes = read_file(file->source, path, 4u*1024u*1024u, &size);
    if (!bytes) return NULL;
    memset(geometry, 0, sizeof(*geometry));
    if (!mdkr_sprite_model_parse(bytes, size, &geometry->mesh)) {
        free(bytes);
        return NULL;
    }
    free(bytes);
    ++geometry_parses;
    geometry->pack_index = file->pack_index;
    snprintf(geometry->path, sizeof(geometry->path), "%s", path);
    geometry->refs = 1;
    geometry->id = ++next_geometry_id;
    return geometry;
}

static void clear_slot(ModelSlot *slot) {
    /* Slot meshes borrow their geometry's arrays. */
    memset(slot->meshes, 0, sizeof(slot->meshes));
    release_geometry(slot->geometry[0]);
    release_geometry(slot->geometry[1]);
    slot->geometry[0] = slot->geometry[1] = NULL;
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
    slot->authored = slot->replayed = slot->attached = 0;
    memset(slot->phases, 0, sizeof(slot->phases));
}

void mdkr_sprite_models_shutdown(void) {
    unsigned i;
    int drew = 0;
    if (count != 0 || geometry_count != 0 || atlas_count != 0) {
        /* Renderer references first, CPU storage after: retained lists name
         * models by sprite ID, and the GPU cache holds their buffers. */
        gfx_dkr_replay_invalidate();
        gfx_clear_modern_mesh_cache();
    }
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i) {
        ModelSlot *s = &slots[i];
        if (s->draws[0] || s->draws[1]) {
            unsigned phases = 0, word;
            drew = 1;
            for (word = 0; word < 8; ++word) {
                uint32_t bits = s->phases[word];
                while (bits) { phases += bits & 1u; bits >>= 1; }
            }
            fprintf(stderr, "[MODELS] sprite=%u near=%llu far=%llu\n", s->sprite,
                    (unsigned long long)s->draws[0], (unsigned long long)s->draws[1]);
            fprintf(stderr, "[MODELS] poses sprite=%u authored=%llu replayed=%llu attached=%llu "
                    "phases=%u\n", s->sprite, (unsigned long long)s->authored,
                    (unsigned long long)s->replayed, (unsigned long long)s->attached, phases);
        }
        clear_slot(s);
    }
    if (drew)
        fprintf(stderr, "[MODELS] material fading=%llu fadingDepthWrites=%llu envTinted=%llu\n",
                (unsigned long long)material_draws[0], (unsigned long long)material_draws[1],
                (unsigned long long)material_draws[2]);
    memset(material_draws, 0, sizeof(material_draws));
    count = 0;
    for (i = 0; i < atlas_count; ++i) {
        if (atlases[i].rgba != NULL) stbi_image_free(atlases[i].rgba);
    }
    memset(atlases, 0, sizeof(atlases));
    atlas_count = 0;
    atlas_bytes = 0;
    for (i = 0; i < geometry_count; ++i) {
        if (geometries[i].refs != 0) mdkr_sprite_model_mesh_free(&geometries[i].mesh);
    }
    memset(geometries, 0, sizeof(geometries));
    geometry_count = 0;
    geometry_parses = 0;
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
    size_t decode_bound, previous_bound;
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
    /* The header admitted the decoded size; this bounds every allocation the
     * decoder makes on the way there, so an IDAT stream that inflates past
     * what the header can need is refused instead of growing to gigabytes. */
    decode_bound = mdkr_stbi_png_allocation_bound(width, height, channels,
        stbi_is_16_bit_from_memory(bytes, (int)size), 4, size);
    if (decode_bound == 0) { free(bytes); return NULL; }
    previous_bound = mdkr_stbi_set_allocation_limit(decode_bound);
    rgba = stbi_load_from_memory(bytes, (int)size, &dw, &dh, &channels, 4);
    (void)mdkr_stbi_set_allocation_limit(previous_bound);
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

unsigned mdkr_sprite_models_present(const MdkrModRegistry *registry) {
    unsigned i, present = 0;
    for (i = 0; registry && i < sizeof(slots)/sizeof(slots[0]); ++i) {
        MdkrModFile file = {0};
        char path[256];
        snprintf(path, sizeof path, "models/%s/%snear.dkrmesh", slots[i].family, slots[i].stem);
        if (mdkr_mod_registry_open_file(registry, path, &file)) {
            ++present;
            mdkr_mod_registry_close_file(&file);
        }
    }
    return present;
}

void mdkr_sprite_models_init(const MdkrModRegistry *registry) {
    unsigned i;
    const char *opt = getenv("MDKR_SPRITE_MODELS");
    mdkr_sprite_models_shutdown();
    enabled = !opt || strcmp(opt, "0");
    /* Nothing decodes where nothing can draw: the launcher has no game
     * renderer yet, and the OpenGL backend has no model path. */
    if (!gfx_sprite_models_supported()) return;
    for (i = 0; registry && i < sizeof(slots)/sizeof(slots[0]); ++i) {
        ModelSlot *s = &slots[i];
        MdkrModFile file = {0};
        char path[256];
        int lod;
        snprintf(path, sizeof path, "models/%s/%snear.dkrmesh", s->family, s->stem);
        if (!mdkr_mod_registry_open_file(registry, path, &file)) continue;
        /* Geometry and atlas come from ONE selected pack. Mixing families
         * across priorities can silently apply the wrong UV layout. */
        for (lod = 0; lod < 2; ++lod) {
            snprintf(path, sizeof path, "models/%s/%s%s.dkrmesh", s->family, s->stem, lod ? "far" : "near");
            s->geometry[lod] = load_geometry(&file, path);
            if (!s->geometry[lod]) goto reject;
        }
        snprintf(path, sizeof path, "models/%s/%s.png", s->family, s->texture);
        s->atlas = load_atlas(&file, path);
        if (!s->atlas) goto reject;
        for (lod = 0; lod < 2; ++lod) {
            if (next_mesh_id == UINT32_MAX) goto reject;
            s->meshes[lod] = s->geometry[lod]->mesh;
            s->meshes[lod].geometry_id = s->geometry[lod]->id;
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
        unsigned geometries_live = 0;
        for (i = 0; i < atlas_count; ++i) bytes += (size_t)atlases[i].width * atlases[i].height * 4;
        for (i = 0; i < geometry_count; ++i) geometries_live += geometries[i].refs != 0;
        fprintf(stderr, "[MODELS] %d sprite replacements loaded; F2 toggles 3D models\n", count);
        fprintf(stderr, "[MODELS] atlases=%u decodedBytes=%zu meshes=%u\n", atlas_count, bytes,
                geometries_live);
    }
}

void mdkr_sprite_models_toggle(void) {
    /* count is zero wherever models cannot draw, so a backend without a
     * model path says nothing about a toggle that would change nothing. */
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
void mdkr_sprite_models_note_draw(unsigned sprite, int far, unsigned flags) {
    unsigned i;
    if (flags & MDKR_SPRITE_MODEL_DRAW_FADING) {
        ++material_draws[0];
        if (flags & MDKR_SPRITE_MODEL_DRAW_DEPTH_WRITE) ++material_draws[1];
    }
    if (flags & MDKR_SPRITE_MODEL_DRAW_ENV_TINT) ++material_draws[2];
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i)
        if (slots[i].sprite == sprite) { ++slots[i].draws[far != 0]; return; }
}
unsigned mdkr_sprite_models_geometry_parses(void) { return geometry_parses; }
void mdkr_sprite_models_note_pose(unsigned sprite, float phase, int attached, int replayed) {
    unsigned i;
    for (i = 0; i < sizeof(slots)/sizeof(slots[0]); ++i)
        if (slots[i].sprite == sprite) {
            const unsigned bucket = (unsigned)(int)phase & 255u;
            if (replayed) ++slots[i].replayed;
            else ++slots[i].authored;
            if (attached) ++slots[i].attached;
            slots[i].phases[bucket >> 5] |= 1u << (bucket & 31u);
            return;
        }
}
