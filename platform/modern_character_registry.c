#include "modern_character_registry.h"

#include "modern_character_text.h"

#include "fs_utf8.h"
#include "modern_character_identity.h"

#include <stdio.h>
#include <string.h>

static int ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int ascii_casecmp(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        int a = ascii_lower((unsigned char)*left++);
        int b = ascii_lower((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
    }
    if (*left == *right) return 0;
    return *left == '\0' ? -1 : 1;
}

static int copy_string(char *output, size_t output_size, const char *value) {
    size_t length;
    if (output == NULL || output_size == 0u || value == NULL) return 0;
    length = strlen(value);
    if (length >= output_size) return 0;
    memcpy(output, value, length + 1u);
    return 1;
}

static int path_join(char *output, size_t output_size,
                     const char *directory, const char *name) {
    size_t length;
    const char *separator;
    int written;
    if (directory == NULL || directory[0] == '\0' || name == NULL || name[0] == '\0') return 0;
    length = strlen(directory);
    separator = directory[length - 1u] == '/' || directory[length - 1u] == '\\' ? "" : "/";
    written = snprintf(output, output_size, "%s%s%s", directory, separator, name);
    return written >= 0 && (size_t)written < output_size;
}

static int has_cache_suffix(const char *name) {
    size_t length = strlen(name);
    return length > 5u && name[length - 5u] == '.' &&
           ascii_lower((unsigned char)name[length - 4u]) == 'm' &&
           ascii_lower((unsigned char)name[length - 3u]) == 'd' &&
           ascii_lower((unsigned char)name[length - 2u]) == 'k' &&
           ascii_lower((unsigned char)name[length - 1u]) == 'c';
}

static int has_disabled_cache_suffix(const char *name) {
    static const char suffix[] = ".mdkc.disabled";
    size_t length = strlen(name);
    size_t suffix_length = sizeof(suffix) - 1u;
    size_t index;
    if (length <= suffix_length) return 0;
    for (index = 0u; index < suffix_length; index++) {
        if (ascii_lower((unsigned char)name[length - suffix_length + index]) !=
            ascii_lower((unsigned char)suffix[index])) return 0;
    }
    return 1;
}

static int content_addressed_leaf(const char *name, const char *package_id,
                                  const char *suffix) {
    const size_t id_length = strlen(package_id);
    const size_t suffix_length = strlen(suffix);
    size_t index;
    if (strlen(name) != id_length + 1u + 64u + suffix_length ||
        memcmp(name, package_id, id_length) != 0 || name[id_length] != '.' ||
        strcmp(name + id_length + 1u + 64u, suffix) != 0) return 0;
    for (index = id_length + 1u; index < id_length + 1u + 64u; index++) {
        const char byte = name[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

uint32_t mdkr_modern_character_semantic_bit(const char *name) {
    static const struct { const char *name; uint32_t bit; } table[] = {
        {"fallback", MDKR_CHARACTER_SEMANTIC_FALLBACK},
        {"race.steer", MDKR_CHARACTER_SEMANTIC_RACE_STEER},
        {"race.reverse", MDKR_CHARACTER_SEMANTIC_RACE_REVERSE},
        {"race.boost", MDKR_CHARACTER_SEMANTIC_RACE_BOOST},
        {"race.damage", MDKR_CHARACTER_SEMANTIC_RACE_DAMAGE},
        {"race.item", MDKR_CHARACTER_SEMANTIC_RACE_ITEM},
        {"race.spin", MDKR_CHARACTER_SEMANTIC_RACE_SPIN},
        {"race.airborne", MDKR_CHARACTER_SEMANTIC_RACE_AIRBORNE},
        {"race.land", MDKR_CHARACTER_SEMANTIC_RACE_LAND},
        {"race.finish_win", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_WIN},
        {"race.finish_lose", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_LOSE},
        {"select.idle", MDKR_CHARACTER_SEMANTIC_SELECT_IDLE},
        {"select.hover", MDKR_CHARACTER_SEMANTIC_SELECT_HOVER},
        {"select.confirm", MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM},
    };
    size_t index;
    if (name == NULL) return 0u;
    for (index = 0u; index < sizeof(table) / sizeof(table[0]); index++) {
        if (strcmp(name, table[index].name) == 0) return table[index].bit;
    }
    return 0u;
}

static uint32_t socket_bit(const char *name) {
    if (name == NULL) return 0u;
    if (strcmp(name, "seat") == 0) return MDKR_CHARACTER_SOCKET_SEAT;
    if (strcmp(name, "head") == 0) return MDKR_CHARACTER_SOCKET_HEAD;
    if (strcmp(name, "hand") == 0) return MDKR_CHARACTER_SOCKET_HAND;
    if (strcmp(name, "hand.left") == 0) return MDKR_CHARACTER_SOCKET_HAND_LEFT;
    if (strcmp(name, "hand.right") == 0) return MDKR_CHARACTER_SOCKET_HAND_RIGHT;
    if (strcmp(name, "foot.left") == 0) return MDKR_CHARACTER_SOCKET_FOOT_LEFT;
    if (strcmp(name, "foot.right") == 0) return MDKR_CHARACTER_SOCKET_FOOT_RIGHT;
    return 0u;
}

static uint32_t rig_role_bit(const char *name) {
    static const char *roles[] = {
        "hips", "spine", "chest", "head",
        "upper_arm.left", "lower_arm.left", "hand.left",
        "upper_arm.right", "lower_arm.right", "hand.right",
        "upper_leg.left", "lower_leg.left", "foot.left",
        "upper_leg.right", "lower_leg.right", "foot.right"
    };
    uint32_t index;
    if (name == NULL) return 0u;
    for (index = 0u; index < MDKR_MODERN_HUMANOID_ROLE_COUNT; index++) {
        if (strcmp(name, roles[index]) == 0) return 1u << index;
    }
    return 0u;
}

static int rig_role_slot(const char *name) {
    uint32_t bit = rig_role_bit(name);
    int slot = 0;
    if (bit == 0u) return -1;
    while ((bit >> (unsigned)slot) != 1u) slot++;
    return slot;
}

/* glTF node names are not usefully bounded. Registry rows are: retain a
 * UTF-8-safe diagnostic prefix plus an explicit ellipsis and always publish
 * the unambiguous compiled node index alongside it. */
static void copy_node_name(char output[MDKR_MODERN_CHARACTER_NODE_NAME_MAX],
                           const char *value) {
    (void)mdkr_modern_character_copy_bounded_name(
        output, MDKR_MODERN_CHARACTER_NODE_NAME_MAX, value);
}

static void add_skip(MdkrModernCharacterRegistry *registry,
                     const char *name, const char *reason) {
    int slot;
    if (registry->skipped >= MDKR_MODERN_CHARACTER_MAX) {
        slot = MDKR_MODERN_CHARACTER_MAX - 1;
        (void)copy_string(registry->skip_name[slot], sizeof(registry->skip_name[slot]),
                          "More characters");
        (void)copy_string(registry->skip_reason[slot], sizeof(registry->skip_reason[slot]),
                          "further character failures were omitted");
        return;
    }
    slot = registry->skipped++;
    (void)copy_string(registry->skip_name[slot], sizeof(registry->skip_name[slot]), name);
    (void)copy_string(registry->skip_reason[slot], sizeof(registry->skip_reason[slot]), reason);
}

static int entry_compare(const MdkrModernCharacterEntry *left,
                         const MdkrModernCharacterEntry *right) {
    int identity = strcmp(left->id, right->id);
    if (identity != 0) return identity;
    if (left->enabled != right->enabled) return left->enabled ? -1 : 1;
    return ascii_casecmp(left->path, right->path);
}

static void sort_entries(MdkrModernCharacterRegistry *registry) {
    int index;
    for (index = 1; index < registry->count; index++) {
        MdkrModernCharacterEntry held = registry->entries[index];
        int scan = index - 1;
        while (scan >= 0 && entry_compare(&registry->entries[scan], &held) > 0) {
            registry->entries[scan + 1] = registry->entries[scan];
            scan--;
        }
        registry->entries[scan + 1] = held;
    }
}

static int registry_init(MdkrModernCharacterRegistry *registry,
                         const char *directory, int include_disabled) {
    MdkrDirectoryNames names;
    size_t name_index;
    if (registry == NULL) return -1;
    memset(registry, 0, sizeof(*registry));
    if (directory == NULL || directory[0] == '\0') return 0;
    /* Listed through the UTF-8 boundary, so a characters folder (or a cache
     * name) outside the Windows ANSI code page is listed exactly. The
     * inventory must be complete, so a listing that stops part-way makes it
     * unavailable rather than partial. */
    if (mdkr_directory_names_utf8(directory, &names) != 0) {
        int exists = 0;
        mdkr_directory_names_free(&names);
        /* A missing character directory is the documented empty state (the
         * query answers 0 with exists cleared, fs_utf8.h). An existing path
         * that cannot be enumerated, or a query that fails, is different:
         * callers must not mistake an unavailable inventory for an empty
         * one. */
        if (mdkr_path_query_utf8(directory, &exists, NULL, NULL) == 0 &&
            !exists) {
            return 0;
        }
        return -1;
    }
    /* A cache whose file name has no UTF-8 spelling is never loaded
     * (fs_utf8.h: every path in the game is UTF-8), but it is never dropped
     * without a word either: the Workshop lists it with what to do. */
    for (name_index = 0u; name_index < names.unnamed_count; ++name_index) {
        const char *stand_in = names.unnamed[name_index];
        if (stand_in[0] != '.' &&
            (has_cache_suffix(stand_in) ||
             (include_disabled && has_disabled_cache_suffix(stand_in)))) {
            add_skip(registry, stand_in,
                     "the file name is not valid Unicode; rename the file to "
                     "load this character");
        }
    }
    for (name_index = 0u; name_index < names.count; ++name_index) {
        const char *leaf = names.names[name_index];
        MdkrModernCharacterAsset asset;
        MdkrModernCharacterDefinition definition;
        MdkrModernCharacterEntry entry;
        MdkrModernDecodedIdentity decoded_identity;
        const char *id;
        const char *display_name;
        const char *short_name;
        const char *narration_name;
        const char *sort_label;
        char path[MDKR_MODERN_CHARACTER_PATH_MAX];
        char error[MDKR_MODERN_CHARACTER_SKIP_REASON_MAX];
        int regular = 0;
        int enabled;
        if (leaf[0] == '.') continue;
        enabled = has_cache_suffix(leaf);
        if (!enabled &&
            !(include_disabled && has_disabled_cache_suffix(leaf))) {
            continue;
        }
        if (!path_join(path, sizeof(path), directory, leaf) ||
            mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 || !regular) {
            add_skip(registry, leaf, "cache path is not a regular readable file");
            continue;
        }
        if (!mdkr_modern_character_asset_load_file(path, &asset, error, sizeof(error))) {
            add_skip(registry, leaf, error);
            continue;
        }
        if (!mdkr_modern_identity_init(&asset, &decoded_identity,
                                       error, sizeof(error))) {
            mdkr_modern_character_asset_unload(&asset);
            add_skip(registry, leaf, error);
            continue;
        }
        memset(&entry, 0, sizeof(entry));
        (void)mdkr_modern_character_asset_definition(&asset, &definition);
        id = mdkr_modern_character_asset_string(&asset, definition.id);
        display_name = mdkr_modern_character_asset_string(&asset, definition.display_name);
        short_name = display_name;
        narration_name = display_name;
        sort_label = display_name;
        {
            MdkrModernIdentity identity;
            MdkrModernIdentityNames identity_names;
            const char *candidate;
            if (mdkr_modern_character_asset_identity(
                    &asset, &identity, NULL) && identity.short_name != 0u &&
                (candidate = mdkr_modern_character_asset_string(
                    &asset, identity.short_name)) != NULL &&
                candidate[0] != '\0') {
                short_name = candidate;
            }
            if (mdkr_modern_character_asset_identity_names(
                    &asset, &identity_names)) {
                if (identity_names.narration_name != 0u &&
                    (candidate = mdkr_modern_character_asset_string(
                        &asset, identity_names.narration_name)) != NULL &&
                    candidate[0] != '\0') {
                    narration_name = candidate;
                }
                if (identity_names.sort_label != 0u &&
                    (candidate = mdkr_modern_character_asset_string(
                        &asset, identity_names.sort_label)) != NULL &&
                    candidate[0] != '\0') {
                    sort_label = candidate;
                }
            }
        }
        if (!copy_string(entry.id, sizeof(entry.id), id) ||
            !copy_string(entry.display_name, sizeof(entry.display_name), display_name) ||
            !copy_string(entry.short_name, sizeof(entry.short_name), short_name) ||
            !copy_string(entry.narration_name, sizeof(entry.narration_name),
                         narration_name) ||
            !copy_string(entry.sort_label, sizeof(entry.sort_label), sort_label) ||
            !copy_string(entry.path, sizeof(entry.path), path)) {
            mdkr_modern_character_asset_unload(&asset);
            add_skip(registry, leaf, "compiled identity or path exceeds the registry bound");
            continue;
        }
        memcpy(entry.source_sha256, asset.source_sha256, sizeof(entry.source_sha256));
        entry.enabled = enabled ? 1u : 0u;
        entry.donor = definition.donor;
        entry.vehicle_mask = definition.vehicle_mask;
        entry.source_lod_bias = definition.lod_bias;
        if (decoded_identity.has_portrait) {
            memcpy(entry.portrait_rgba, decoded_identity.portrait_rgba,
                   sizeof(entry.portrait_rgba));
        }
        {
            MdkrModernIdentity identity;
            if (mdkr_modern_character_asset_identity(&asset, &identity, NULL)) {
                entry.identity_flags = identity.flags;
                entry.portrait_bytes = identity.portrait_size;
                entry.minimap_rgba = identity.minimap_rgba;
            }
        }
        {
            MdkrModernProvenance provenance;
            if (mdkr_modern_character_asset_provenance(
                    &asset, &provenance)) {
                const char *spdx = mdkr_modern_character_asset_string(
                    &asset, provenance.spdx);
                const char *attribution = mdkr_modern_character_asset_string(
                    &asset, provenance.attribution);
                const char *source_url = mdkr_modern_character_asset_string(
                    &asset, provenance.source_url);
                if (!copy_string(entry.license_spdx,
                                 sizeof(entry.license_spdx), spdx) ||
                    !copy_string(entry.attribution,
                                 sizeof(entry.attribution), attribution) ||
                    !copy_string(entry.source_url,
                                 sizeof(entry.source_url), source_url)) {
                    mdkr_modern_character_asset_unload(&asset);
                    add_skip(registry, leaf,
                             "compiled provenance exceeds the registry bound");
                    continue;
                }
                entry.provenance_present = 1u;
            }
        }
        {
            MdkrModernCalibration calibration;
            const MdkrModernSectionView *attachments =
                mdkr_modern_character_asset_section(
                    &asset, MDKR_MDKC_ATTACHMENTS);
            uint32_t attachment_index;
            if (mdkr_modern_character_asset_calibration(&asset, &calibration)) {
                memcpy(entry.bounds_min, calibration.bounds_min,
                       sizeof(entry.bounds_min));
                memcpy(entry.bounds_max, calibration.bounds_max,
                       sizeof(entry.bounds_max));
                memcpy(entry.ground, calibration.ground,
                       sizeof(entry.ground));
                entry.source_height = calibration.source_height;
                entry.normalized_height = calibration.normalized_height;
                entry.target_height = calibration.target_height;
                entry.source_forward = calibration.source_forward;
                entry.calibration_flags = calibration.flags;
            }
            if (attachments != NULL) {
                for (attachment_index = 0u;
                     attachment_index < attachments->count;
                     attachment_index++) {
                    MdkrModernAttachment attachment;
                    (void)mdkr_modern_character_asset_attachment(
                        &asset, attachment_index, &attachment);
                    entry.attachment_context_mask |=
                        1u << attachment.context;
                }
            }
        }
        mdkr_modern_character_asset_stats(&asset, &entry.stats);
        {
            MdkrModernRig rig;
            if (mdkr_modern_character_asset_rig(&asset, &rig)) {
                uint32_t role_index;
                entry.rig_present = 1u;
                entry.rig_mode = rig.mode;
                entry.rig_flags = rig.flags;
                entry.rig_role_mask = rig.role_mask;
                entry.rig_min_confidence_milli = 1000u;
                for (role_index = 0u; role_index < rig.role_count;
                     role_index++) {
                    MdkrModernRigRole role;
                    MdkrModernNode node;
                    const char *semantic;
                    int slot;
                    (void)mdkr_modern_character_asset_rig_role(
                        &asset, role_index, &role);
                    semantic = mdkr_modern_character_asset_string(
                        &asset, role.semantic);
                    slot = rig_role_slot(semantic);
                    if (slot >= 0 && mdkr_modern_character_asset_node(
                            &asset, role.node, &node)) {
                        entry.rig_role_node[slot] = role.node;
                        entry.rig_role_flags[slot] = role.flags;
                        entry.rig_role_confidence_milli[slot] =
                            role.confidence_milli;
                        copy_node_name(
                            entry.rig_role_node_name[slot],
                            mdkr_modern_character_asset_string(
                                &asset, node.name));
                        memcpy(entry.rig_role_rest_rotation[slot],
                               role.rest_rotation,
                               sizeof(role.rest_rotation));
                        memcpy(entry.rig_role_bend_axis[slot],
                               role.bend_axis, sizeof(role.bend_axis));
                    }
                    if ((role.flags & 1u) != 0u) {
                        entry.inferred_rig_role_mask |= rig_role_bit(semantic);
                    }
                    if (role.confidence_milli <
                        entry.rig_min_confidence_milli) {
                        entry.rig_min_confidence_milli =
                            role.confidence_milli;
                    }
                }
            }
        }
        {
            const MdkrModernSectionView *primitives =
                mdkr_modern_character_asset_section(
                    &asset, MDKR_MDKC_PRIMITIVES);
            uint32_t primitive_index;
            if (primitives != NULL) {
                for (primitive_index = 0u;
                     primitive_index < primitives->count; primitive_index++) {
                    MdkrModernPrimitive primitive;
                    if (mdkr_modern_character_asset_primitive(
                            &asset, primitive_index, &primitive) &&
                        primitive.lod < MDKR_MODERN_CHARACTER_LOD_LEVELS) {
                        entry.lod_vertices[primitive.lod] +=
                            primitive.vertex_count;
                        entry.lod_triangles[primitive.lod] +=
                            primitive.index_count / 3u;
                        entry.lod_primitives[primitive.lod]++;
                        if (primitive.skin >= 0) {
                            MdkrModernSkin skin;
                            if (mdkr_modern_character_asset_skin(
                                    &asset, (uint32_t)primitive.skin, &skin)) {
                                entry.lod_palette_matrices[primitive.lod] +=
                                    skin.joint_count;
                            }
                        }
                    }
                }
            }
        }
        {
            uint32_t index;
            /* Sized by the validator's own ceiling, not by a number that
             * happened to be large enough when this was written. At 64 an asset
             * with 65+ animations wrote 1 byte per extra clip into the stack
             * frame beside `entry`, `path[4096]` and an asset whose owned_bytes
             * is freed below -- reached at every boot and every roster refresh,
             * without the character ever being selected. */
            uint8_t animation_moves[MDKR_MODERN_ANIMATIONS_MAX] = {0};
            for (index = 0u; index < entry.stats.sockets; index++) {
                MdkrModernSocket socket;
                if (mdkr_modern_character_asset_socket(&asset, index, &socket)) {
                    entry.socket_mask |= socket_bit(
                        mdkr_modern_character_asset_string(&asset,
                                                           socket.semantic));
                }
            }
            /* Defence in depth: the validator already refuses a count above
             * the array bound, so this cannot clamp a valid asset. It is here so
             * that a future ceiling change cannot silently reintroduce the
             * overflow before anyone notices the two constants disagree. */
            for (index = 0u;
                 index < entry.stats.animations &&
                 index < MDKR_MODERN_ANIMATIONS_MAX;
                 index++) {
                MdkrModernAnimation animation;
                uint32_t channel_offset;
                (void)mdkr_modern_character_asset_animation(
                    &asset, index, &animation);
                for (channel_offset = 0u;
                     channel_offset < animation.channel_count;
                     channel_offset++) {
                    MdkrModernChannel channel;
                    MdkrModernKey first;
                    uint32_t key_index;
                    int moves = 0;
                    if (!mdkr_modern_character_asset_channel(
                            &asset, animation.first_channel + channel_offset,
                            &channel) ||
                        !mdkr_modern_character_asset_key(
                            &asset, channel.first_key, &first)) continue;
                    for (key_index = 1u;
                         key_index < channel.key_count && !moves;
                         key_index++) {
                        MdkrModernKey key;
                        uint32_t component;
                        (void)mdkr_modern_character_asset_key(
                            &asset, channel.first_key + key_index, &key);
                        for (component = 0u;
                             component < channel.components; component++) {
                            if (key.value[component] != first.value[component] ||
                                key.incoming[component] != 0.0f ||
                                key.outgoing[component] != 0.0f ||
                                first.incoming[component] != 0.0f ||
                                first.outgoing[component] != 0.0f) {
                                moves = 1;
                                break;
                            }
                        }
                    }
                    if (moves) {
                        animation_moves[index] = 1u;
                        entry.motion_channels++;
                    }
                }
            }
            for (index = 0u; index < entry.stats.semantics; index++) {
                MdkrModernSemantic semantic;
                const char *name;
                const char *clip;
                uint32_t bit;
                uint32_t animation_index;
                if (!mdkr_modern_character_asset_semantic(
                        &asset, index, &semantic)) continue;
                name = mdkr_modern_character_asset_string(
                    &asset, semantic.semantic);
                clip = mdkr_modern_character_asset_string(
                    &asset, semantic.clip);
                bit = mdkr_modern_character_semantic_bit(name);
                if ((semantic.flags & MDKR_MODERN_SEMANTIC_DISABLED) != 0u) {
                    entry.disabled_semantic_mask |= bit;
                } else {
                    entry.semantic_mask |= bit;
                }
                /* Same ceiling as the write loop above: this READS
                 * animation_moves[animation_index], so bounding only the write
                 * would have left an out-of-bounds read behind. */
                for (animation_index = 0u;
                     animation_index < entry.stats.animations &&
                     animation_index < MDKR_MODERN_ANIMATIONS_MAX;
                     animation_index++) {
                    MdkrModernAnimation animation;
                    const char *animation_name;
                    (void)mdkr_modern_character_asset_animation(
                        &asset, animation_index, &animation);
                    animation_name = mdkr_modern_character_asset_string(
                        &asset, animation.name);
                    if (clip != NULL && animation_name != NULL &&
                        strcmp(clip, animation_name) == 0) {
                        if (animation_moves[animation_index]) {
                            entry.authored_moving_semantic_mask |= bit;
                            if ((semantic.flags &
                                 MDKR_MODERN_SEMANTIC_DISABLED) == 0u) {
                                entry.moving_semantic_mask |= bit;
                            }
                        }
                        break;
                    }
                }
            }
        }
        mdkr_modern_character_asset_unload(&asset);
        if (registry->count >= MDKR_MODERN_CHARACTER_MAX) {
            add_skip(registry, leaf, "the 64-character local registry is full");
            continue;
        }
        registry->entries[registry->count++] = entry;
    }
    sort_entries(registry);
    {
        int read_index;
        int write_index = 0;
        for (read_index = 0; read_index < registry->count; read_index++) {
            if (write_index > 0 && strcmp(registry->entries[write_index - 1].id,
                                          registry->entries[read_index].id) == 0) {
                const char *leaf = strrchr(registry->entries[read_index].path, '/');
                add_skip(registry, leaf != NULL ? leaf + 1 : registry->entries[read_index].path,
                         "another compiled character uses the same package id");
                continue;
            }
            if (write_index != read_index) registry->entries[write_index] = registry->entries[read_index];
            write_index++;
        }
        registry->count = write_index;
    }
    if (include_disabled && registry->count > 0) {
        /* Retained sources and provenance reports, counted from the same
         * listing the caches came from. */
        for (name_index = 0u; name_index < names.count; ++name_index) {
            const char *leaf = names.names[name_index];
            int regular = 0;
            char path[MDKR_MODERN_CHARACTER_PATH_MAX];
            int index;
            if (leaf[0] == '.' ||
                !path_join(path, sizeof(path), directory, leaf) ||
                mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 ||
                !regular || mdkr_path_is_link_or_reparse_utf8(path) != 0) {
                continue;
            }
            for (index = 0; index < registry->count; index++) {
                MdkrModernCharacterEntry *entry = &registry->entries[index];
                if (content_addressed_leaf(leaf, entry->id, ".mdkrchar")) {
                    if (entry->source_revisions != UINT32_MAX) {
                        entry->source_revisions++;
                    }
                    break;
                }
                if (content_addressed_leaf(leaf, entry->id, ".json")) {
                    if (entry->provenance_reports != UINT32_MAX) {
                        entry->provenance_reports++;
                    }
                    break;
                }
            }
        }
    }
    mdkr_directory_names_free(&names);
    return 0;
}

int mdkr_modern_character_registry_init(MdkrModernCharacterRegistry *registry,
                                        const char *directory) {
    return registry_init(registry, directory, 0);
}

int mdkr_modern_character_registry_init_inventory(
    MdkrModernCharacterRegistry *registry, const char *directory) {
    return registry_init(registry, directory, 1);
}

void mdkr_modern_character_registry_shutdown(MdkrModernCharacterRegistry *registry) {
    if (registry != NULL) memset(registry, 0, sizeof(*registry));
}

int mdkr_modern_character_registry_count(const MdkrModernCharacterRegistry *registry) {
    return registry != NULL ? registry->count : 0;
}

const MdkrModernCharacterEntry *mdkr_modern_character_registry_entry(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->count) return NULL;
    return &registry->entries[index];
}

int mdkr_modern_character_registry_find(const MdkrModernCharacterRegistry *registry,
                                        const char *id) {
    int low = 0;
    int high;
    if (registry == NULL || id == NULL) return -1;
    high = registry->count - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        int comparison = strcmp(registry->entries[middle].id, id);
        if (comparison == 0) return middle;
        if (comparison < 0) low = middle + 1;
        else high = middle - 1;
    }
    return -1;
}

int mdkr_modern_character_registry_skipped(const MdkrModernCharacterRegistry *registry) {
    return registry != NULL ? registry->skipped : 0;
}

const char *mdkr_modern_character_registry_skip_name(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->skipped) return NULL;
    return registry->skip_name[index];
}

const char *mdkr_modern_character_registry_skip_reason(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->skipped) return NULL;
    return registry->skip_reason[index];
}

int mdkr_modern_character_registry_load(const MdkrModernCharacterRegistry *registry,
                                        int index, MdkrModernCharacterAsset *out,
                                        char *error, size_t error_size) {
    const MdkrModernCharacterEntry *entry = mdkr_modern_character_registry_entry(registry, index);
    if (entry == NULL) {
        if (out != NULL) memset(out, 0, sizeof(*out));
        if (error != NULL && error_size != 0u) (void)snprintf(error, error_size, "character registry index is invalid");
        return 0;
    }
    return mdkr_modern_character_asset_load_file(entry->path, out, error, error_size);
}
