#include "gfx_presentation_packet.h"
#include "gfx_deformation_shape.h"

#include <stdint.h>
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
    GfxPresentationMatrixClass matrix_class;
    uint64_t geometry_signature;
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
static int s_force_dependency_rewrite = -1;
static bool s_forced_matrix_rewrite_used;
static bool s_forced_vertex_rewrite_used;

static uint64_t endpoint_hash_bytes(uint64_t hash, const void *data,
                                    size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool force_dependency_rewrite(bool matrix) {
    bool *used = matrix ? &s_forced_matrix_rewrite_used
                        : &s_forced_vertex_rewrite_used;
    if (s_force_dependency_rewrite < 0) {
        const char *value = getenv("MDKR_TEST_RETAINED_DEPENDENCY_REWRITE");
        s_force_dependency_rewrite =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    if (s_force_dependency_rewrite == 0 || *used) {
        return false;
    }
    *used = true;
    if (matrix) {
        s_stats.forced_matrix_rewrites++;
    } else {
        s_stats.forced_vertex_rewrites++;
    }
    return true;
}

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

bool gfx_presentation_packet_register_projected_shadow_vertex(
    const void *key, int viewport, uint32_t ordinal,
    const GfxPresentationMatrixOwner *owner) {
    if (owner == NULL ||
        owner->matrix_class !=
            GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES ||
        !list_register(&s_live_vertices, key, 0u, true, viewport, owner)) {
        return false;
    }
    for (size_t index = s_live_vertices.count; index > 0u; index--) {
        PacketEntry *entry = &s_live_vertices.entries[index - 1u];
        if (entry->key == key) {
            entry->binding.ordinal = ordinal;
            break;
        }
    }
    s_stats.vertex_registrations++;
    s_stats.projected_shadow_vertex_registrations++;
    if (s_live_vertices.count > s_stats.vertex_peak) {
        s_stats.vertex_peak = s_live_vertices.count;
    }
    return true;
}

static bool list_note_walked(
    PacketList *list, const void *key, const void *bytes, size_t size) {
    if (list == NULL || key == NULL || bytes == NULL || size == 0u ||
        size > GFX_PRESENTATION_PACKET_MAX_KEY_BYTES) {
        return false;
    }
    for (size_t index = list->count; index > 0u; index--) {
        PacketEntry *entry = &list->entries[index - 1u];
        if (entry->key != key) {
            continue;
        }
        if (entry->binding.key_size == 0u ||
            entry->binding.key_size != size) {
            return false;
        }
        memcpy(entry->binding.key_bytes, bytes, size);
        return true;
    }
    return false;
}

bool gfx_presentation_packet_note_walked_matrix(
    const void *key, const void *bytes, size_t size) {
    return list_note_walked(&s_live_matrices, key, bytes, size);
}

bool gfx_presentation_packet_note_walked_vertex(
    const void *key, const void *bytes, size_t size) {
    return list_note_walked(&s_live_vertices, key, bytes, size);
}

void gfx_presentation_packet_note_stale_hold(bool matrix, bool safe) {
    if (!safe) {
        s_stats.unsafe_stale_fallbacks++;
    } else if (matrix) {
        s_stats.stale_matrix_holds++;
    } else {
        s_stats.stale_vertex_holds++;
    }
}

void gfx_presentation_packet_note_dependency_endpoint(
    const void *expected, const void *actual, size_t size) {
    if (expected == NULL || actual == NULL || size == 0u) {
        return;
    }
    if (s_stats.dependency_endpoint_checks == 0u) {
        s_stats.dependency_expected_hash = UINT64_C(1469598103934665603);
        s_stats.dependency_actual_hash = UINT64_C(1469598103934665603);
    }
    s_stats.dependency_endpoint_checks++;
    s_stats.dependency_expected_hash = endpoint_hash_bytes(
        s_stats.dependency_expected_hash, expected, size);
    s_stats.dependency_actual_hash = endpoint_hash_bytes(
        s_stats.dependency_actual_hash, actual, size);
    if (memcmp(expected, actual, size) != 0) {
        s_stats.dependency_endpoint_mismatches++;
    }
}

void gfx_presentation_packet_capture_begin(uint64_t tick) {
    s_deform_live.count = 0u;
    s_deform_live_tick = tick;
    s_deform_capture_active = true;
    s_deform_capture_failed = false;
}

void gfx_presentation_packet_capture_abort(void) {
    s_deform_live.count = 0u;
    s_deform_live_tick = 0u;
    s_deform_capture_active = false;
    s_deform_capture_failed = false;
}

bool gfx_presentation_packet_capture_deformation(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    const void *bytes, size_t byte_size, uint32_t count, uint32_t stride) {
    DeformationEntry *entry = NULL;

    if (!s_deform_capture_active || s_deform_capture_failed || owner == NULL ||
        !owner->valid || owner->address == NULL || owner->generation == 0u ||
        bytes == NULL || byte_size == 0u ||
        byte_size > GFX_PRESENTATION_DEFORM_MAX_BYTES ||
        !gfx_deformation_shape_matches(count, stride, byte_size, SIZE_MAX) ||
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
            candidate->matrix_class == owner->matrix_class &&
            candidate->geometry_signature == owner->geometry_signature &&
            candidate->viewport == viewport &&
            candidate->ordinal == ordinal) {
            /* Direct world-space particle meshes are submitted unchanged to
             * every viewport. Collapse that repeated observation into the one
             * shared retained stream. A byte or shape disagreement is still a
             * collision and is poisoned below. */
            if ((owner->matrix_class ==
                     GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES ||
                 owner->matrix_class ==
                     GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES) &&
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
    entry->matrix_class = owner->matrix_class;
    entry->geometry_signature = owner->geometry_signature;
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

bool gfx_presentation_packet_publish_deformation(void) {
    DeformationList recycled;

    if (!s_deform_capture_active) {
        return false;
    }
    if (s_deform_capture_failed) {
        s_deform_live.count = 0u;
        s_deform_capture_active = false;
        return false;
    }
    recycled = s_deform_previous;
    s_deform_previous = s_deform_current;
    s_deform_previous_tick = s_deform_current_tick;
    s_deform_current = s_deform_live;
    s_deform_current_tick = s_deform_live_tick;
    s_deform_live = recycled;
    s_deform_live.count = 0u;
    s_deform_capture_active = false;
    return true;
}

void gfx_presentation_packet_note_future_capture(bool success) {
    if (success) {
        s_stats.future_captures++;
    } else {
        s_stats.future_failures++;
    }
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
    if (s_deform_capture_active &&
        !gfx_presentation_packet_publish_deformation()) {
        ok = false;
    }
    if (!ok) {
        s_frozen_matrices.count = 0u;
        s_frozen_vertices.count = 0u;
        s_frozen_valid = false;
        s_stats.freeze_failures++;
        s_stats.frozen_matrices = 0u;
        s_stats.frozen_vertices = 0u;
        return;
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
                        const void *observed,
                        GfxPresentationPacketBinding *out,
                        uint64_t *hits, uint64_t *misses, bool matrix) {
    if (!s_frozen_valid || list == NULL || key == NULL || observed == NULL ||
        out == NULL) {
        (*misses)++;
        return false;
    }
    for (size_t index = list->count; index > 0u; index--) {
        const PacketEntry *entry = &list->entries[index - 1u];
        if (entry->key != key) {
            continue;
        }
        if (entry->binding.key_size != 0u &&
            (force_dependency_rewrite(matrix) ||
             memcmp(entry->binding.key_bytes, observed,
                    entry->binding.key_size) != 0)) {
            s_stats.stale_keys++;
            *out = entry->binding;
            out->stale = true;
            (*hits)++;
            return true;
        }
        *out = entry->binding;
        out->stale = false;
        (*hits)++;
        return true;
    }
    (*misses)++;
    return false;
}

bool gfx_presentation_packet_lookup_matrix(
    const void *key, GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_matrices, key, key, out, &s_stats.matrix_hits,
                       &s_stats.matrix_misses, true);
}

bool gfx_presentation_packet_lookup_vertex(
    const void *key, GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_vertices, key, key, out, &s_stats.vertex_hits,
                       &s_stats.vertex_misses, false);
}

bool gfx_presentation_packet_lookup_matrix_observed(
    const void *key, const void *observed,
    GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_matrices, key, observed, out,
                       &s_stats.matrix_hits, &s_stats.matrix_misses, true);
}

bool gfx_presentation_packet_lookup_vertex_observed(
    const void *key, const void *observed,
    GfxPresentationPacketBinding *out) {
    return list_lookup(&s_frozen_vertices, key, observed, out,
                       &s_stats.vertex_hits, &s_stats.vertex_misses, false);
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
            entry->matrix_class == owner->matrix_class &&
            entry->geometry_signature == owner->geometry_signature &&
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

bool gfx_presentation_packet_lookup_deformation_hold(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    uint64_t authored_tick, uint32_t count, uint32_t stride,
    GfxPresentationDeformationBinding *out) {
    const DeformationEntry *current;

    if (!s_frozen_valid || owner == NULL || out == NULL ||
        s_deform_current_tick != authored_tick) {
        return false;
    }
    current = deformation_find(
        &s_deform_current, owner, viewport, ordinal);
    if (current == NULL || current->ambiguous || current->count != count ||
        current->stride != stride ||
        !gfx_deformation_shape_matches(
            count, stride, current->byte_size, SIZE_MAX)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->owner = *owner;
    out->viewport = viewport;
    out->ordinal = ordinal;
    out->count = count;
    out->stride = stride;
    out->byte_size = current->byte_size;
    out->previous_bytes = current->bytes;
    out->current_bytes = current->bytes;
    s_stats.deformation_holds++;
    return true;
}

void gfx_presentation_packet_note_deformation_incompatible(void) {
    s_stats.deformation_incompatible++;
}

void gfx_presentation_packet_note_phase_hold(bool effect) {
    if (effect) {
        s_stats.effect_phase_holds++;
    } else {
        s_stats.deformation_phase_holds++;
    }
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

void gfx_presentation_packet_note_projected_shadow_deformation(
    bool overridden) {
    s_stats.projected_shadow_deformation_hits++;
    if (overridden) {
        s_stats.projected_shadow_deformation_overrides++;
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

void gfx_presentation_packet_note_projected_shadow_primitive_alpha(
    bool overridden) {
    s_stats.projected_shadow_primitive_alpha_hits++;
    if (overridden) {
        s_stats.projected_shadow_primitive_alpha_overrides++;
    }
}

void gfx_presentation_packet_note_effect_override(void) {
    s_stats.effect_overrides++;
}

void gfx_presentation_packet_note_endpoint_semantic(
    const void *expected, const void *actual, size_t size) {
    if (expected == NULL || actual == NULL || size == 0u) {
        return;
    }
    if (s_stats.endpoint_vertex_checks == 0u) {
        s_stats.endpoint_expected_hash = UINT64_C(1469598103934665603);
        s_stats.endpoint_actual_hash = UINT64_C(1469598103934665603);
    }
    s_stats.endpoint_vertex_checks++;
    s_stats.endpoint_expected_hash = endpoint_hash_bytes(
        s_stats.endpoint_expected_hash, expected, size);
    s_stats.endpoint_actual_hash = endpoint_hash_bytes(
        s_stats.endpoint_actual_hash, actual, size);
    if (memcmp(expected, actual, size) != 0) {
        s_stats.endpoint_vertex_mismatches++;
    }
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
    s_force_dependency_rewrite = -1;
    s_forced_matrix_rewrite_used = false;
    s_forced_vertex_rewrite_used = false;
}
