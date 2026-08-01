#include "gfx_presentation_packet.h"

#include <stdlib.h>
#include <string.h>

#define PACKET_INITIAL_BINDINGS 128u
#define PACKET_MAX_BINDINGS (16u * 1024u)
#define DEFORMATION_MAX_BINDINGS (4u * 1024u)

typedef struct PacketEntry {
    const void *key;
    GfxPresentationPacketBinding binding;
} PacketEntry;

typedef struct PacketList {
    PacketEntry *entries;
    size_t count;
    size_t capacity;
} PacketList;

typedef struct DeformationEntry {
    const void *owner_address;
    uint64_t owner_generation;
    const void *secondary_address;
    uint64_t secondary_generation;
    int viewport;
    uint32_t ordinal;
    uint32_t count;
    uint32_t stride;
    size_t byte_size;
    bool ambiguous;
    uint8_t bytes[GFX_PRESENTATION_DEFORM_MAX_BYTES];
} DeformationEntry;

typedef struct DeformationList {
    DeformationEntry *entries;
    size_t count;
    size_t capacity;
} DeformationList;

static PacketList s_live_matrices;
static PacketList s_live_vertices;
static PacketList s_frozen_matrices;
static PacketList s_frozen_vertices;
static DeformationList s_deform_live;
static DeformationList s_deform_current;
static DeformationList s_deform_previous;
static uint64_t s_deform_live_tick;
static uint64_t s_deform_current_tick;
static uint64_t s_deform_previous_tick;
static bool s_deform_capture_active;
static bool s_deform_capture_failed;
static bool s_frozen_valid;
static GfxPresentationPacketStats s_stats;

static bool list_grow(PacketList *list, size_t need) {
    size_t next;
    PacketEntry *grown;

    if (list == NULL || need > PACKET_MAX_BINDINGS) {
        return false;
    }
    if (list->capacity >= need) {
        return true;
    }
    next = list->capacity != 0u ? list->capacity : PACKET_INITIAL_BINDINGS;
    while (next < need) {
        if (next > PACKET_MAX_BINDINGS / 2u) {
            next = PACKET_MAX_BINDINGS;
        } else {
            next *= 2u;
        }
        if (next < need && next == PACKET_MAX_BINDINGS) {
            return false;
        }
    }
    grown = (PacketEntry *)realloc(list->entries, next * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    list->entries = grown;
    list->capacity = next;
    return true;
}

static bool deformation_grow(DeformationList *list, size_t need) {
    size_t next;
    DeformationEntry *grown;

    if (list == NULL || need > DEFORMATION_MAX_BINDINGS) {
        return false;
    }
    if (list->capacity >= need) {
        return true;
    }
    next = list->capacity != 0u ? list->capacity : PACKET_INITIAL_BINDINGS;
    while (next < need) {
        if (next > DEFORMATION_MAX_BINDINGS / 2u) {
            next = DEFORMATION_MAX_BINDINGS;
        } else {
            next *= 2u;
        }
        if (next < need && next == DEFORMATION_MAX_BINDINGS) {
            return false;
        }
    }
    grown = (DeformationEntry *)realloc(
        list->entries, next * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    list->entries = grown;
    list->capacity = next;
    return true;
}

static bool list_register(PacketList *list, const void *key, size_t key_size,
                          bool identity_only, int viewport,
                          const GfxPresentationMatrixOwner *owner) {
    PacketEntry *entry = NULL;

    if (list == NULL || key == NULL || (!identity_only && key_size == 0u) ||
        key_size > GFX_PRESENTATION_PACKET_MAX_KEY_BYTES || owner == NULL ||
        !owner->valid || owner->address == NULL || owner->generation == 0u) {
        return false;
    }
    for (size_t index = list->count; index > 0u; index--) {
        if (list->entries[index - 1u].key == key) {
            entry = &list->entries[index - 1u];
            break;
        }
    }
    if (entry == NULL) {
        if (!list_grow(list, list->count + 1u)) {
            return false;
        }
        entry = &list->entries[list->count++];
    }
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->binding.owner = *owner;
    entry->binding.viewport = viewport;
    entry->binding.key_size = key_size;
    if (key_size != 0u) {
        memcpy(entry->binding.key_bytes, key, key_size);
    }
    return true;
}

bool gfx_presentation_packet_register_matrix(
    const void *key, size_t key_size, int viewport,
    const GfxPresentationMatrixOwner *owner) {
    if (!list_register(&s_live_matrices, key, key_size, false, viewport,
                       owner)) {
        return false;
    }
    s_stats.matrix_registrations++;
    if (s_live_matrices.count > s_stats.matrix_peak) {
        s_stats.matrix_peak = s_live_matrices.count;
    }
    return true;
}

bool gfx_presentation_packet_register_vertex(
    const void *key, size_t key_size, int viewport,
    const GfxPresentationMatrixOwner *owner) {
    if (!list_register(&s_live_vertices, key, key_size, false, viewport,
                       owner)) {
        return false;
    }
    s_stats.vertex_registrations++;
    if (s_live_vertices.count > s_stats.vertex_peak) {
        s_stats.vertex_peak = s_live_vertices.count;
    }
    return true;
}

bool gfx_presentation_packet_register_vertex_identity(
    const void *key, int viewport,
    const GfxPresentationMatrixOwner *owner) {
    if (owner == NULL ||
        owner->matrix_class != GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES ||
        !list_register(&s_live_vertices, key, 0u, true, viewport, owner)) {
        return false;
    }
    s_stats.vertex_registrations++;
    s_stats.particle_vertex_registrations++;
    if (s_live_vertices.count > s_stats.vertex_peak) {
        s_stats.vertex_peak = s_live_vertices.count;
    }
    return true;
}

void gfx_presentation_packet_capture_begin(uint64_t tick) {
    s_deform_live.count = 0u;
    s_deform_live_tick = tick;
    s_deform_capture_active = true;
    s_deform_capture_failed = false;
}

bool gfx_presentation_packet_capture_deformation(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    const void *bytes, size_t byte_size, uint32_t count, uint32_t stride) {
    DeformationEntry *entry = NULL;

    if (!s_deform_capture_active || s_deform_capture_failed || owner == NULL ||
        !owner->valid || owner->address == NULL || owner->generation == 0u ||
        bytes == NULL || byte_size == 0u ||
        byte_size > GFX_PRESENTATION_DEFORM_MAX_BYTES || count == 0u ||
        stride == 0u || (size_t)count * (size_t)stride != byte_size ||
        (owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT &&
         (owner->secondary_address == NULL ||
          owner->secondary_generation == 0u))) {
        return false;
    }
    for (size_t index = s_deform_live.count; index > 0u; index--) {
        DeformationEntry *candidate = &s_deform_live.entries[index - 1u];
        if (candidate->owner_address == owner->address &&
            candidate->owner_generation == owner->generation &&
            candidate->secondary_address == owner->secondary_address &&
            candidate->secondary_generation == owner->secondary_generation &&
            candidate->viewport == viewport &&
            candidate->ordinal == ordinal) {
            /* Direct world-space particle meshes are submitted unchanged to
             * every viewport. Collapse that repeated observation into the one
             * shared retained stream. A byte or shape disagreement is still a
             * collision and is poisoned below. */
            if (owner->matrix_class ==
                    GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES &&
                !candidate->ambiguous &&
                candidate->count == count && candidate->stride == stride &&
                candidate->byte_size == byte_size &&
                memcmp(candidate->bytes, bytes, byte_size) == 0) {
                return true;
            }
            /* Two batches with the same stable recipe cannot be told apart
             * on replay. Poison this key instead of letting the later batch
             * overwrite the earlier one and silently interpolate both from
             * whichever happened to win. */
            candidate->ambiguous = true;
            if (owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
                s_stats.effect_collisions++;
            } else {
                s_stats.deformation_collisions++;
            }
            return false;
        }
    }
    if (entry == NULL) {
        if (!deformation_grow(&s_deform_live, s_deform_live.count + 1u)) {
            s_deform_capture_failed = true;
            return false;
        }
        entry = &s_deform_live.entries[s_deform_live.count++];
    }
    memset(entry, 0, sizeof(*entry));
    entry->owner_address = owner->address;
    entry->owner_generation = owner->generation;
    entry->secondary_address = owner->secondary_address;
    entry->secondary_generation = owner->secondary_generation;
    entry->viewport = viewport;
    entry->ordinal = ordinal;
    entry->count = count;
    entry->stride = stride;
    entry->byte_size = byte_size;
    memcpy(entry->bytes, bytes, byte_size);
    if (owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
        s_stats.effect_registrations++;
    } else {
        s_stats.deformation_registrations++;
    }
    if (s_deform_live.count > s_stats.deformation_peak) {
        s_stats.deformation_peak = s_deform_live.count;
    }
    return true;
}

static bool list_freeze(PacketList *frozen, const PacketList *live) {
    if (live->count != 0u) {
        if (!list_grow(frozen, live->count)) {
            frozen->count = 0u;
            return false;
        }
        memcpy(frozen->entries, live->entries,
               live->count * sizeof(*frozen->entries));
    }
    frozen->count = live->count;
    return true;
}

void gfx_presentation_packet_freeze(void) {
    bool ok = list_freeze(&s_frozen_matrices, &s_live_matrices) &&
              list_freeze(&s_frozen_vertices, &s_live_vertices);

    if (s_deform_capture_active && s_deform_capture_failed) {
        ok = false;
    }

    s_live_matrices.count = 0u;
    s_live_vertices.count = 0u;
    if (!ok) {
        s_frozen_matrices.count = 0u;
        s_frozen_vertices.count = 0u;
        s_frozen_valid = false;
        s_stats.freeze_failures++;
        s_stats.frozen_matrices = 0u;
        s_stats.frozen_vertices = 0u;
        s_deform_live.count = 0u;
        s_deform_capture_active = false;
        return;
    }
    if (s_deform_capture_active) {
        DeformationList recycled = s_deform_previous;
        s_deform_previous = s_deform_current;
        s_deform_previous_tick = s_deform_current_tick;
        s_deform_current = s_deform_live;
        s_deform_current_tick = s_deform_live_tick;
        s_deform_live = recycled;
        s_deform_live.count = 0u;
        s_deform_capture_active = false;
    }
    s_frozen_valid = true;
    s_stats.freezes++;
    s_stats.frozen_matrices = s_frozen_matrices.count;
    s_stats.frozen_vertices = s_frozen_vertices.count;
}

bool gfx_presentation_packet_frozen(void) {
    return s_frozen_valid;
}

static bool list_lookup(const PacketList *list, const void *key,
                        GfxPresentationPacketBinding *out,
                        uint64_t *hits, uint64_t *misses) {
    if (!s_frozen_valid || list == NULL || key == NULL || out == NULL) {
        (*misses)++;
        return false;
    }
    for (size_t index = list->count; index > 0u; index--) {
        const PacketEntry *entry = &list->entries[index - 1u];
        if (entry->key != key) {
            continue;
        }
        if (entry->binding.key_size != 0u &&
            memcmp(entry->binding.key_bytes, key,
                   entry->binding.key_size) != 0) {
            s_stats.stale_keys++;
            (*misses)++;
            return false;
        }
        *out = entry->binding;
        (*hits)++;
        return true;
    }
    (*misses)++;
    return false;
}

bool gfx_presentation_packet_lookup_matrix(
    const void *key, GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_matrices, key, out, &s_stats.matrix_hits,
                       &s_stats.matrix_misses);
}

bool gfx_presentation_packet_lookup_vertex(
    const void *key, GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_vertices, key, out, &s_stats.vertex_hits,
                       &s_stats.vertex_misses);
}

bool gfx_presentation_packet_lookup_live_vertex(
    const void *key, GfxPresentationPacketBinding *out) {
    if (key == NULL || out == NULL) {
        return false;
    }
    for (size_t index = s_live_vertices.count; index > 0u; index--) {
        const PacketEntry *entry = &s_live_vertices.entries[index - 1u];
        if (entry->key != key) {
            continue;
        }
        if (entry->binding.key_size != 0u &&
            memcmp(entry->binding.key_bytes, key,
                   entry->binding.key_size) != 0) {
            return false;
        }
        *out = entry->binding;
        return true;
    }
    return false;
}

static bool list_contains_key(const PacketList *list, const void *key) {
    if (list == NULL || key == NULL) {
        return false;
    }
    for (size_t index = list->count; index > 0u; index--) {
        if (list->entries[index - 1u].key == key) {
            return true;
        }
    }
    return false;
}

bool gfx_presentation_packet_has_live_vertex(const void *key) {
    return list_contains_key(&s_live_vertices, key);
}

bool gfx_presentation_packet_has_frozen_vertex(const void *key) {
    return s_frozen_valid && list_contains_key(&s_frozen_vertices, key);
}

static const DeformationEntry *deformation_find(
    const DeformationList *list, const GfxPresentationMatrixOwner *owner,
    int viewport, uint32_t ordinal) {
    if (list == NULL || owner == NULL) {
        return NULL;
    }
    for (size_t index = list->count; index > 0u; index--) {
        const DeformationEntry *entry = &list->entries[index - 1u];
        if (entry->owner_address == owner->address &&
            entry->owner_generation == owner->generation &&
            entry->secondary_address == owner->secondary_address &&
            entry->secondary_generation == owner->secondary_generation &&
            entry->viewport == viewport && entry->ordinal == ordinal) {
            return entry;
        }
    }
    return NULL;
}

bool gfx_presentation_packet_lookup_deformation(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    uint64_t current_tick, uint32_t count, uint32_t stride,
    GfxPresentationDeformationBinding *out) {
    const DeformationEntry *current;
    const DeformationEntry *previous;

    if (!s_frozen_valid || owner == NULL || out == NULL || current_tick == 0u ||
        s_deform_current_tick != current_tick ||
        s_deform_previous_tick + 1u != s_deform_current_tick) {
        if (owner != NULL &&
            owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
            s_stats.effect_misses++;
        } else {
            s_stats.deformation_misses++;
        }
        return false;
    }
    current = deformation_find(
        &s_deform_current, owner, viewport, ordinal);
    previous = deformation_find(
        &s_deform_previous, owner, viewport, ordinal);
    if (current == NULL || previous == NULL || current->count != count ||
        current->ambiguous || previous->ambiguous ||
        previous->count != count || current->stride != stride ||
        previous->stride != stride ||
        current->byte_size != previous->byte_size) {
        if (owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
            s_stats.effect_misses++;
        } else {
            s_stats.deformation_misses++;
        }
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->owner = *owner;
    out->viewport = viewport;
    out->ordinal = ordinal;
    out->count = count;
    out->stride = stride;
    out->byte_size = current->byte_size;
    out->previous_bytes = previous->bytes;
    out->current_bytes = current->bytes;
    if (owner->matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
        s_stats.effect_hits++;
    } else {
        s_stats.deformation_hits++;
    }
    return true;
}

void gfx_presentation_packet_note_deformation_incompatible(void) {
    s_stats.deformation_incompatible++;
}

void gfx_presentation_packet_note_deformation_override(void) {
    s_stats.deformation_overrides++;
}

void gfx_presentation_packet_note_particle_deformation(bool overridden) {
    s_stats.particle_deformation_hits++;
    if (overridden) {
        s_stats.particle_deformation_overrides++;
    }
}

void gfx_presentation_packet_note_deformation_color(bool particle,
                                                    bool overridden) {
    s_stats.deformation_color_hits++;
    if (overridden) {
        s_stats.deformation_color_overrides++;
    }
    if (particle) {
        s_stats.particle_color_hits++;
        if (overridden) {
            s_stats.particle_color_overrides++;
        }
    }
}

void gfx_presentation_packet_note_primitive_alpha(bool particle,
                                                  bool overridden) {
    s_stats.primitive_alpha_hits++;
    if (overridden) {
        s_stats.primitive_alpha_overrides++;
    }
    if (particle) {
        s_stats.particle_primitive_alpha_hits++;
        if (overridden) {
            s_stats.particle_primitive_alpha_overrides++;
        }
    }
}

void gfx_presentation_packet_note_effect_override(void) {
    s_stats.effect_overrides++;
}

void gfx_presentation_packet_invalidate(void) {
    s_live_matrices.count = 0u;
    s_live_vertices.count = 0u;
    s_frozen_matrices.count = 0u;
    s_frozen_vertices.count = 0u;
    s_frozen_valid = false;
    s_deform_live.count = 0u;
    s_deform_current.count = 0u;
    s_deform_previous.count = 0u;
    s_deform_live_tick = 0u;
    s_deform_current_tick = 0u;
    s_deform_previous_tick = 0u;
    s_deform_capture_active = false;
    s_deform_capture_failed = false;
    s_stats.frozen_matrices = 0u;
    s_stats.frozen_vertices = 0u;
}

void gfx_presentation_packet_get_stats(GfxPresentationPacketStats *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}

void gfx_presentation_packet_shutdown(void) {
    free(s_live_matrices.entries);
    free(s_live_vertices.entries);
    free(s_frozen_matrices.entries);
    free(s_frozen_vertices.entries);
    free(s_deform_live.entries);
    free(s_deform_current.entries);
    free(s_deform_previous.entries);
    memset(&s_live_matrices, 0, sizeof(s_live_matrices));
    memset(&s_live_vertices, 0, sizeof(s_live_vertices));
    memset(&s_frozen_matrices, 0, sizeof(s_frozen_matrices));
    memset(&s_frozen_vertices, 0, sizeof(s_frozen_vertices));
    memset(&s_deform_live, 0, sizeof(s_deform_live));
    memset(&s_deform_current, 0, sizeof(s_deform_current));
    memset(&s_deform_previous, 0, sizeof(s_deform_previous));
    memset(&s_stats, 0, sizeof(s_stats));
    s_deform_live_tick = 0u;
    s_deform_current_tick = 0u;
    s_deform_previous_tick = 0u;
    s_deform_capture_active = false;
    s_deform_capture_failed = false;
    s_frozen_valid = false;
}
