#include "camera_object_occlusion.h"

#ifdef NATIVE_PORT

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textures_sprites.h"
#include "camera_obstruction_query.h"

typedef struct MdkrCameraObjectOcclusionChunk {
    MdkrCameraVec3 minimum;
    MdkrCameraVec3 maximum;
    size_t triangle_offset;
    size_t triangle_count;
    uint64_t integrity;
    int valid;
} MdkrCameraObjectOcclusionChunk;

typedef struct MdkrCameraObjectOcclusionNode {
    MdkrCameraVec3 minimum;
    MdkrCameraVec3 maximum;
    size_t left;
    size_t right;
    size_t chunk_index;
    uint64_t integrity;
    int valid;
    int leaf;
} MdkrCameraObjectOcclusionNode;

/* CAM-05 object-model visual-occlusion cache. */
typedef struct MdkrCameraObjectOcclusionCache {
    const ObjectModel *model;
    uint32_t generation;
    MdkrCameraOcclusionWorld world;
    MdkrCameraVec3 *vertices;
    uint32_t *indices;
    MdkrCameraOcclusionTriangle *triangles;
    size_t vertex_capacity;
    size_t triangle_capacity;
    MdkrCameraObjectOcclusionChunk *chunks;
    size_t chunk_capacity;
    size_t chunk_count;
    MdkrCameraObjectOcclusionNode *nodes;
    size_t node_capacity;
    size_t node_count;
    size_t bytes;
    struct MdkrCameraObjectOcclusionCache *next;
} MdkrCameraObjectOcclusionCache;

static MdkrCameraObjectOcclusionCache *sCameraObjectOcclusionCaches;
static MdkrCameraObjectOcclusionTelemetry sCameraObjectOcclusionTelemetry;
static uint32_t sCameraObjectOcclusionNextGeneration = 1U;
/* Hit records expose one stable blocker ID but not model generation. Reserve a
 * never-reused source-ID range for each published model so deterministic ties
 * and temporal blocker identity remain valid across model pointer reuse. */
static uint32_t sCameraObjectOcclusionNextStableId = 1U;

static int mdkr_camera_object_occlusion_trace_enabled(void) {
    const char *value = getenv("MDKR_CAMERA_TRACE");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static int mdkr_camera_object_occlusion_size_mul(
    size_t count,
    size_t item_size,
    size_t *out_size) {
    if (out_size == NULL || (count != 0U && item_size > SIZE_MAX / count)) {
        return 0;
    }
    *out_size = count * item_size;
    return 1;
}

static int mdkr_camera_object_occlusion_bounds_valid(
    MdkrCameraVec3 minimum,
    MdkrCameraVec3 maximum) {
    return isfinite(minimum.x) && isfinite(minimum.y) && isfinite(minimum.z) &&
           isfinite(maximum.x) && isfinite(maximum.y) && isfinite(maximum.z) &&
           minimum.x <= maximum.x && minimum.y <= maximum.y &&
           minimum.z <= maximum.z;
}

static uint64_t mdkr_camera_object_occlusion_hash_bytes(
    uint64_t hash,
    const void *bytes,
    size_t count) {
    const unsigned char *cursor = bytes;
    size_t index;
    for (index = 0U; index < count; index++) {
        hash ^= cursor[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mdkr_camera_object_occlusion_chunk_integrity(
    const MdkrCameraObjectOcclusionChunk *chunk) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &chunk->minimum, sizeof(chunk->minimum));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &chunk->maximum, sizeof(chunk->maximum));
    hash = mdkr_camera_object_occlusion_hash_bytes(
        hash, &chunk->triangle_offset, sizeof(chunk->triangle_offset));
    hash = mdkr_camera_object_occlusion_hash_bytes(
        hash, &chunk->triangle_count, sizeof(chunk->triangle_count));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &chunk->valid, sizeof(chunk->valid));
    return hash;
}

static uint64_t mdkr_camera_object_occlusion_node_integrity(
    const MdkrCameraObjectOcclusionNode *node) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->minimum, sizeof(node->minimum));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->maximum, sizeof(node->maximum));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->left, sizeof(node->left));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->right, sizeof(node->right));
    hash = mdkr_camera_object_occlusion_hash_bytes(
        hash, &node->chunk_index, sizeof(node->chunk_index));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->valid, sizeof(node->valid));
    hash = mdkr_camera_object_occlusion_hash_bytes(hash, &node->leaf, sizeof(node->leaf));
    return hash;
}

static int mdkr_camera_object_occlusion_triangle_degenerate(
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c) {
    const double abx = (double)b.x - a.x;
    const double aby = (double)b.y - a.y;
    const double abz = (double)b.z - a.z;
    const double acx = (double)c.x - a.x;
    const double acy = (double)c.y - a.y;
    const double acz = (double)c.z - a.z;
    const double bcx = (double)c.x - b.x;
    const double bcy = (double)c.y - b.y;
    const double bcz = (double)c.z - b.z;
    const double cross_x = aby * acz - abz * acy;
    const double cross_y = abz * acx - abx * acz;
    const double cross_z = abx * acy - aby * acx;
    const double ab2 = abx * abx + aby * aby + abz * abz;
    const double ac2 = acx * acx + acy * acy + acz * acz;
    const double bc2 = bcx * bcx + bcy * bcy + bcz * bcz;
    const double max_edge2 = fmax(ab2, fmax(ac2, bc2));
    const double cross2 = cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;

    return !isfinite(max_edge2) || !isfinite(cross2) || max_edge2 <= 0.0 ||
           cross2 <= 1.0e-24 * max_edge2 * max_edge2;
}

static void mdkr_camera_object_occlusion_expand_chunk(
    MdkrCameraObjectOcclusionChunk *chunk,
    MdkrCameraVec3 point) {
    if (!chunk->valid) {
        chunk->minimum = point;
        chunk->maximum = point;
        chunk->valid = 1;
        return;
    }
    if (point.x < chunk->minimum.x) chunk->minimum.x = point.x;
    if (point.y < chunk->minimum.y) chunk->minimum.y = point.y;
    if (point.z < chunk->minimum.z) chunk->minimum.z = point.z;
    if (point.x > chunk->maximum.x) chunk->maximum.x = point.x;
    if (point.y > chunk->maximum.y) chunk->maximum.y = point.y;
    if (point.z > chunk->maximum.z) chunk->maximum.z = point.z;
}

static double mdkr_camera_object_occlusion_chunk_centroid_axis(
    const MdkrCameraObjectOcclusionChunk *chunk,
    size_t axis) {
    if (axis == 0U) return 0.5 * ((double)chunk->minimum.x + chunk->maximum.x);
    if (axis == 1U) return 0.5 * ((double)chunk->minimum.y + chunk->maximum.y);
    return 0.5 * ((double)chunk->minimum.z + chunk->maximum.z);
}

static int mdkr_camera_object_occlusion_chunk_order_less(
    const MdkrCameraObjectOcclusionCache *cache,
    size_t first,
    size_t second,
    size_t primary_axis) {
    size_t offset;
    for (offset = 0U; offset < 3U; offset++) {
        const size_t axis = (primary_axis + offset) % 3U;
        const double a = mdkr_camera_object_occlusion_chunk_centroid_axis(
            &cache->chunks[first], axis);
        const double b = mdkr_camera_object_occlusion_chunk_centroid_axis(
            &cache->chunks[second], axis);
        if (a < b) return 1;
        if (a > b) return 0;
    }
    return first < second;
}

static void mdkr_camera_object_occlusion_sort_chunk_range(
    const MdkrCameraObjectOcclusionCache *cache,
    size_t *order,
    size_t begin,
    size_t end,
    size_t axis) {
    size_t index;
    for (index = begin + 1U; index < end; index++) {
        const size_t value = order[index];
        size_t position = index;
        while (position > begin && mdkr_camera_object_occlusion_chunk_order_less(
                cache, value, order[position - 1U], axis)) {
            order[position] = order[position - 1U];
            position--;
        }
        order[position] = value;
    }
}

static int mdkr_camera_object_occlusion_build_node(
    MdkrCameraObjectOcclusionCache *cache,
    size_t *order,
    size_t begin,
    size_t end,
    size_t node_capacity,
    size_t *out_node_index) {
    MdkrCameraObjectOcclusionNode *node;
    size_t node_index;
    size_t order_index;
    double centroid_minimum[3];
    double centroid_maximum[3];
    size_t axis = 0U;

    if (begin >= end || cache->node_count >= node_capacity || out_node_index == NULL) {
        return 0;
    }
    node_index = cache->node_count++;
    node = &cache->nodes[node_index];
    node->minimum = cache->chunks[order[begin]].minimum;
    node->maximum = cache->chunks[order[begin]].maximum;
    for (axis = 0U; axis < 3U; axis++) {
        centroid_minimum[axis] = centroid_maximum[axis] =
            mdkr_camera_object_occlusion_chunk_centroid_axis(
                &cache->chunks[order[begin]], axis);
    }
    for (order_index = begin; order_index < end; order_index++) {
        const MdkrCameraObjectOcclusionChunk *chunk = &cache->chunks[order[order_index]];
        size_t component;
        if (!chunk->valid) return 0;
        if (chunk->minimum.x < node->minimum.x) node->minimum.x = chunk->minimum.x;
        if (chunk->minimum.y < node->minimum.y) node->minimum.y = chunk->minimum.y;
        if (chunk->minimum.z < node->minimum.z) node->minimum.z = chunk->minimum.z;
        if (chunk->maximum.x > node->maximum.x) node->maximum.x = chunk->maximum.x;
        if (chunk->maximum.y > node->maximum.y) node->maximum.y = chunk->maximum.y;
        if (chunk->maximum.z > node->maximum.z) node->maximum.z = chunk->maximum.z;
        for (component = 0U; component < 3U; component++) {
            const double centroid = mdkr_camera_object_occlusion_chunk_centroid_axis(
                chunk, component);
            if (centroid < centroid_minimum[component]) centroid_minimum[component] = centroid;
            if (centroid > centroid_maximum[component]) centroid_maximum[component] = centroid;
        }
    }
    node->valid = 1;
    if (end - begin == 1U) {
        node->leaf = 1;
        node->chunk_index = order[begin];
        node->integrity = mdkr_camera_object_occlusion_node_integrity(node);
        *out_node_index = node_index;
        return 1;
    }
    axis = 0U;
    if (centroid_maximum[1] - centroid_minimum[1] >
        centroid_maximum[axis] - centroid_minimum[axis]) axis = 1U;
    if (centroid_maximum[2] - centroid_minimum[2] >
        centroid_maximum[axis] - centroid_minimum[axis]) axis = 2U;
    mdkr_camera_object_occlusion_sort_chunk_range(cache, order, begin, end, axis);
    {
        const size_t middle = begin + (end - begin) / 2U;
        if (!mdkr_camera_object_occlusion_build_node(
                cache, order, begin, middle, node_capacity, &node->left) ||
            !mdkr_camera_object_occlusion_build_node(
                cache, order, middle, end, node_capacity, &node->right)) {
            return 0;
        }
    }
    node->integrity = mdkr_camera_object_occlusion_node_integrity(node);
    *out_node_index = node_index;
    return 1;
}

static int mdkr_camera_object_occlusion_bounds_contain(
    MdkrCameraVec3 outer_minimum,
    MdkrCameraVec3 outer_maximum,
    MdkrCameraVec3 inner_minimum,
    MdkrCameraVec3 inner_maximum) {
    return outer_minimum.x <= inner_minimum.x && outer_minimum.y <= inner_minimum.y &&
           outer_minimum.z <= inner_minimum.z && outer_maximum.x >= inner_maximum.x &&
           outer_maximum.y >= inner_maximum.y && outer_maximum.z >= inner_maximum.z;
}

static int mdkr_camera_object_occlusion_validate_node(
    const MdkrCameraObjectOcclusionCache *cache,
    size_t node_index,
    const MdkrCameraObjectOcclusionNode *parent,
    size_t *chunk_seen,
    size_t *visited_nodes,
    size_t *visited_leaves) {
    const MdkrCameraObjectOcclusionNode *node;
    const MdkrCameraObjectOcclusionChunk *chunk;

    if (node_index >= cache->node_count || *visited_nodes >= cache->node_count) return 0;
    node = &cache->nodes[node_index];
    if (node->valid != 1 || (node->leaf != 0 && node->leaf != 1) ||
        !mdkr_camera_object_occlusion_bounds_valid(node->minimum, node->maximum) ||
        node->integrity != mdkr_camera_object_occlusion_node_integrity(node) ||
        (parent != NULL && !mdkr_camera_object_occlusion_bounds_contain(
            parent->minimum, parent->maximum, node->minimum, node->maximum))) {
        return 0;
    }
    (*visited_nodes)++;
    if (node->leaf == 1) {
        if (node->left != 0U || node->right != 0U ||
            node->chunk_index >= cache->chunk_count ||
            chunk_seen[node->chunk_index] != 0U) {
            return 0;
        }
        chunk = &cache->chunks[node->chunk_index];
        if (chunk->valid != 1 ||
            !mdkr_camera_object_occlusion_bounds_valid(chunk->minimum, chunk->maximum) ||
            chunk->integrity != mdkr_camera_object_occlusion_chunk_integrity(chunk) ||
            !mdkr_camera_object_occlusion_bounds_contain(
                node->minimum, node->maximum, chunk->minimum, chunk->maximum) ||
            !mdkr_camera_object_occlusion_bounds_contain(
                chunk->minimum, chunk->maximum, node->minimum, node->maximum)) {
            return 0;
        }
        chunk_seen[node->chunk_index] = 1U;
        (*visited_leaves)++;
        return 1;
    }
    if (node->left >= cache->node_count || node->right >= cache->node_count ||
        node->left == node->right || node->left == node_index ||
        node->right == node_index) {
        return 0;
    }
    return mdkr_camera_object_occlusion_validate_node(
               cache, node->left, node, chunk_seen, visited_nodes, visited_leaves) &&
           mdkr_camera_object_occlusion_validate_node(
               cache, node->right, node, chunk_seen, visited_nodes, visited_leaves);
}

static MdkrCameraObjectOcclusionCache *mdkr_camera_object_occlusion_find(
    const ObjectModel *model) {
    MdkrCameraObjectOcclusionCache *cache;

    for (cache = sCameraObjectOcclusionCaches; cache != NULL; cache = cache->next) {
        if (cache->model == model) {
            return cache;
        }
    }
    return NULL;
}

static void mdkr_camera_object_occlusion_destroy(
    MdkrCameraObjectOcclusionCache *cache) {
    if (cache == NULL) {
        return;
    }
    free(cache->vertices);
    free(cache->indices);
    free(cache->triangles);
    free(cache->chunks);
    free(cache->nodes);
    free(cache);
}

static void mdkr_camera_object_occlusion_remove(const ObjectModel *model) {
    MdkrCameraObjectOcclusionCache **link = &sCameraObjectOcclusionCaches;

    while (*link != NULL) {
        MdkrCameraObjectOcclusionCache *cache = *link;
        if (cache->model == model) {
            *link = cache->next;
            sCameraObjectOcclusionTelemetry.model_cache_count--;
            sCameraObjectOcclusionTelemetry.vertex_count -= cache->world.vertex_count;
            sCameraObjectOcclusionTelemetry.hard_triangle_count -= cache->world.triangle_count;
            sCameraObjectOcclusionTelemetry.chunk_count -= cache->chunk_count;
            sCameraObjectOcclusionTelemetry.node_count -= cache->node_count;
            sCameraObjectOcclusionTelemetry.bytes -= cache->bytes;
            mdkr_camera_object_occlusion_destroy(cache);
            return;
        }
        link = &cache->next;
    }
}

/* Return nonzero only for direct material evidence that the camera can ignore.
 * Cutout and vertex-alpha remain hard/unknown until CAM-08 can classify assets. */
static int mdkr_camera_object_occlusion_batch_nonblocking(
    const TriangleBatchInfo *batch,
    const TextureInfo *textures,
    s32 texture_count,
    MdkrCameraObjectOcclusionTelemetry *telemetry) {
    const u32 flags = batch->flags;

    if (flags & (RENDER_HIDDEN | RENDER_WATER | RENDER_DECAL | RENDER_SEMI_TRANSPARENT)) {
        return TRUE;
    }
    if (batch->textureIndex != 0xFF) {
        const TextureHeader *texture;
        if (textures == NULL || batch->textureIndex >= texture_count) {
            telemetry->unknown_policy_triangle_count++;
            return FALSE;
        }
        texture = DKR_PTR(TextureHeader, textures[batch->textureIndex].texture);
        if (texture == NULL) {
            telemetry->unknown_policy_triangle_count++;
            return FALSE;
        }
        if (texture->flags & RENDER_SEMI_TRANSPARENT) {
            return TRUE;
        }
    }
    if (flags & (RENDER_CUTOUT | RENDER_VTX_ALPHA)) {
        telemetry->unknown_policy_triangle_count++;
    }
    return FALSE;
}

static int mdkr_camera_object_occlusion_batch_bounds_valid(
    const ObjectModel *model,
    const TriangleBatchInfo *batch,
    const TriangleBatchInfo *next_batch) {
    return batch->facesOffset >= 0 && next_batch->facesOffset >= batch->facesOffset &&
           next_batch->facesOffset <= model->numberOfTriangles &&
           batch->verticesOffset >= 0 && next_batch->verticesOffset >= batch->verticesOffset &&
           next_batch->verticesOffset <= model->numberOfVertices;
}

static void mdkr_camera_object_occlusion_reject(const char *reason) {
    sCameraObjectOcclusionTelemetry.rejected_model_count++;
    fprintf(stderr, "[CAM-OBJECT-OCCLUSION] rejected model cache: %s\n", reason);
}

void mdkr_camera_object_occlusion_model_loaded(ObjectModel *model) {
    const Vertex *source_vertices;
    const Triangle *source_triangles;
    const TriangleBatchInfo *batches;
    const TextureInfo *textures;
    MdkrCameraObjectOcclusionCache *cache;
    size_t vertex_bytes;
    size_t index_bytes;
    size_t triangle_bytes;
    size_t chunk_bytes;
    size_t node_bytes;
    size_t order_bytes;
    size_t triangle_capacity;
    size_t chunk_capacity;
    size_t node_capacity;
    size_t *chunk_order = NULL;
    size_t triangle_count = 0U;
    uint32_t stable_id_base;
    uint32_t source_ordinal = 0U;
    uint32_t model_generation;
    s32 batch_index;
    s32 vertex_index;

    if (model == NULL) {
        mdkr_camera_object_occlusion_reject("null model");
        return;
    }
    mdkr_camera_object_occlusion_remove(model);
    if (model->numberOfVertices < 0 || model->numberOfTriangles < 0 ||
        model->numberOfBatches < 0 || model->numberOfTextures < 0 ||
        model->vertices == 0 || model->triangles == 0 ||
        model->batches == 0 || (model->numberOfTextures > 0 && model->textures == 0) ||
        (size_t)model->numberOfVertices > UINT32_MAX ||
        (size_t)model->numberOfTriangles > SIZE_MAX / 3U) {
        sCameraObjectOcclusionTelemetry.malformed_input_count++;
        mdkr_camera_object_occlusion_reject("invalid model header");
        return;
    }
    if (sCameraObjectOcclusionNextGeneration == 0U ||
        sCameraObjectOcclusionNextGeneration == UINT32_MAX) {
        mdkr_camera_object_occlusion_reject("object generation space exhausted");
        return;
    }
    source_vertices = DKR_PTR(Vertex, model->vertices);
    source_triangles = DKR_PTR(Triangle, model->triangles);
    batches = DKR_PTR(TriangleBatchInfo, model->batches);
    textures = model->textures != 0 ? DKR_PTR(TextureInfo, model->textures) : NULL;
    triangle_capacity = (size_t)model->numberOfTriangles;
    chunk_capacity = triangle_capacity / MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES +
        (triangle_capacity % MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES != 0U);
    if (chunk_capacity > SIZE_MAX / 2U + 1U) {
        mdkr_camera_object_occlusion_reject("BVH node capacity overflow");
        return;
    }
    node_capacity = chunk_capacity == 0U ? 0U : chunk_capacity * 2U - 1U;
    if (triangle_capacity > (size_t)(UINT32_MAX - sCameraObjectOcclusionNextStableId)) {
        mdkr_camera_object_occlusion_reject("stable triangle-ID space exhausted");
        return;
    }
    stable_id_base = sCameraObjectOcclusionNextStableId;
    model_generation = sCameraObjectOcclusionNextGeneration;
    if (!mdkr_camera_object_occlusion_size_mul(
            (size_t)model->numberOfVertices, sizeof(MdkrCameraVec3), &vertex_bytes) ||
        !mdkr_camera_object_occlusion_size_mul(triangle_capacity, 3U * sizeof(uint32_t), &index_bytes) ||
        !mdkr_camera_object_occlusion_size_mul(
            triangle_capacity, sizeof(MdkrCameraOcclusionTriangle), &triangle_bytes) ||
        !mdkr_camera_object_occlusion_size_mul(
            chunk_capacity, sizeof(MdkrCameraObjectOcclusionChunk), &chunk_bytes) ||
        !mdkr_camera_object_occlusion_size_mul(
            node_capacity, sizeof(MdkrCameraObjectOcclusionNode), &node_bytes) ||
        !mdkr_camera_object_occlusion_size_mul(
            chunk_capacity, sizeof(size_t), &order_bytes) ||
        vertex_bytes > SIZE_MAX - index_bytes ||
        vertex_bytes + index_bytes > SIZE_MAX - triangle_bytes ||
        vertex_bytes + index_bytes + triangle_bytes > SIZE_MAX - chunk_bytes ||
        vertex_bytes + index_bytes + triangle_bytes + chunk_bytes > SIZE_MAX - node_bytes) {
        sCameraObjectOcclusionTelemetry.malformed_input_count++;
        mdkr_camera_object_occlusion_reject("allocation size overflow");
        return;
    }

    /* Validate the complete visual-batch layout before allocating or publishing.
     * Nonblocking batches may be excluded from the cache, but malformed offsets
     * are still an asset contract failure rather than an excuse to skip bytes. */
    for (batch_index = 0; batch_index < model->numberOfBatches; batch_index++) {
        const TriangleBatchInfo *batch = &batches[batch_index];
        const TriangleBatchInfo *next_batch = &batches[batch_index + 1];
        if (!mdkr_camera_object_occlusion_batch_bounds_valid(model, batch, next_batch)) {
            sCameraObjectOcclusionTelemetry.malformed_input_count++;
            mdkr_camera_object_occlusion_reject("malformed batch offsets");
            return;
        }
    }

    cache = calloc(1U, sizeof(*cache));
    if (cache == NULL) {
        mdkr_camera_object_occlusion_reject("cache allocation failed");
        return;
    }
    cache->vertices = vertex_bytes != 0U ? malloc(vertex_bytes) : NULL;
    cache->indices = index_bytes != 0U ? malloc(index_bytes) : NULL;
    cache->triangles = triangle_bytes != 0U ? malloc(triangle_bytes) : NULL;
    cache->chunks = chunk_bytes != 0U ? calloc(1U, chunk_bytes) : NULL;
    cache->nodes = node_bytes != 0U ? calloc(1U, node_bytes) : NULL;
    chunk_order = order_bytes != 0U ? malloc(order_bytes) : NULL;
    cache->vertex_capacity = (size_t)model->numberOfVertices;
    cache->triangle_capacity = triangle_capacity;
    cache->chunk_capacity = chunk_capacity;
    cache->node_capacity = node_capacity;
    if ((vertex_bytes != 0U && cache->vertices == NULL) ||
        (index_bytes != 0U && cache->indices == NULL) ||
        (triangle_bytes != 0U && cache->triangles == NULL) ||
        (chunk_bytes != 0U && cache->chunks == NULL) ||
        (node_bytes != 0U && cache->nodes == NULL) ||
        (order_bytes != 0U && chunk_order == NULL)) {
        free(chunk_order);
        mdkr_camera_object_occlusion_destroy(cache);
        mdkr_camera_object_occlusion_reject("geometry allocation failed");
        return;
    }

    for (vertex_index = 0; vertex_index < model->numberOfVertices; vertex_index++) {
        const Vertex *source = &source_vertices[vertex_index];
        cache->vertices[vertex_index] =
            (MdkrCameraVec3) { (float)source->x, (float)source->y, (float)source->z };
    }

    for (batch_index = 0; batch_index < model->numberOfBatches; batch_index++) {
        const TriangleBatchInfo *batch = &batches[batch_index];
        const TriangleBatchInfo *next_batch = &batches[batch_index + 1];
        const s32 vertex_start = batch->verticesOffset;
        const s32 vertex_end = next_batch->verticesOffset;
        s32 face_index;

        for (face_index = batch->facesOffset; face_index < next_batch->facesOffset; face_index++) {
            const Triangle *source = &source_triangles[face_index];
            const uint32_t index0 = (uint32_t)vertex_start + source->vi0;
            const uint32_t index1 = (uint32_t)vertex_start + source->vi1;
            const uint32_t index2 = (uint32_t)vertex_start + source->vi2;
            MdkrCameraVec3 a;
            MdkrCameraVec3 b;
            MdkrCameraVec3 c;

            if (source_ordinal >= triangle_capacity || index0 >= (uint32_t)vertex_end ||
                index1 >= (uint32_t)vertex_end || index2 >= (uint32_t)vertex_end) {
                sCameraObjectOcclusionTelemetry.malformed_input_count++;
                mdkr_camera_object_occlusion_destroy(cache);
                mdkr_camera_object_occlusion_reject("malformed triangle indices");
                return;
            }
            source_ordinal++;
            a = cache->vertices[index0];
            b = cache->vertices[index1];
            c = cache->vertices[index2];
            if (mdkr_camera_object_occlusion_triangle_degenerate(a, b, c)) {
                sCameraObjectOcclusionTelemetry.degenerate_triangle_count++;
                /* Real supported assets contain isolated zero-area visual
                 * faces. They have no occluding area, so reject the face—not
                 * the otherwise valid door/prop shell—and keep it visible in
                 * the census. The published kernel world remains globally
                 * finite and nondegenerate. */
                continue;
            }
            if (mdkr_camera_object_occlusion_batch_nonblocking(
                    batch, textures, model->numberOfTextures,
                    &sCameraObjectOcclusionTelemetry)) {
                sCameraObjectOcclusionTelemetry.nonblocking_triangle_count++;
                continue;
            }
            if (batch->flags & RENDER_NO_COLLISION) {
                sCameraObjectOcclusionTelemetry.visual_no_collision_hard_triangle_count++;
            }
            cache->indices[triangle_count * 3U] = index0;
            cache->indices[triangle_count * 3U + 1U] = index1;
            cache->indices[triangle_count * 3U + 2U] = index2;
            cache->triangles[triangle_count] = (MdkrCameraOcclusionTriangle) {
                stable_id_base + source_ordinal - 1U,
                MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
                MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
                model_generation,
            };
            triangle_count++;
        }
    }

    for (cache->chunk_count = 0U; cache->chunk_count *
             MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES < triangle_count;
         cache->chunk_count++) {
        MdkrCameraObjectOcclusionChunk *chunk = &cache->chunks[cache->chunk_count];
        size_t triangle_index;
        const size_t offset = cache->chunk_count *
            MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES;
        size_t end = offset + MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES;
        if (end > triangle_count) end = triangle_count;
        chunk->triangle_offset = offset;
        chunk->triangle_count = end - offset;
        for (triangle_index = offset; triangle_index < end; triangle_index++) {
            const size_t index_offset = triangle_index * 3U;
            mdkr_camera_object_occlusion_expand_chunk(
                chunk, cache->vertices[cache->indices[index_offset]]);
            mdkr_camera_object_occlusion_expand_chunk(
                chunk, cache->vertices[cache->indices[index_offset + 1U]]);
            mdkr_camera_object_occlusion_expand_chunk(
                chunk, cache->vertices[cache->indices[index_offset + 2U]]);
        }
        if (chunk->valid != 1 || chunk->triangle_offset != offset ||
            chunk->triangle_count == 0U ||
            chunk->triangle_count > MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES ||
            !mdkr_camera_object_occlusion_bounds_valid(chunk->minimum, chunk->maximum)) {
            free(chunk_order);
            mdkr_camera_object_occlusion_destroy(cache);
            mdkr_camera_object_occlusion_reject("invalid chunk coverage");
            return;
        }
        chunk->integrity = mdkr_camera_object_occlusion_chunk_integrity(chunk);
    }
    if (cache->chunk_count != 0U) {
        size_t root_index;
        size_t chunk_index;
        size_t visited_nodes = 0U;
        size_t visited_leaves = 0U;
        for (chunk_index = 0U; chunk_index < cache->chunk_count; chunk_index++) {
            chunk_order[chunk_index] = chunk_index;
        }
        if (!mdkr_camera_object_occlusion_build_node(
                cache, chunk_order, 0U, cache->chunk_count, node_capacity,
                &root_index) || root_index != 0U ||
            cache->node_count != cache->chunk_count * 2U - 1U) {
            free(chunk_order);
            mdkr_camera_object_occlusion_destroy(cache);
            mdkr_camera_object_occlusion_reject("BVH build failed");
            return;
        }
        memset(chunk_order, 0, cache->chunk_count * sizeof(*chunk_order));
        if (!mdkr_camera_object_occlusion_validate_node(
                cache, root_index, NULL, chunk_order, &visited_nodes,
                &visited_leaves) ||
            visited_nodes != cache->node_count ||
            visited_leaves != cache->chunk_count) {
            free(chunk_order);
            mdkr_camera_object_occlusion_destroy(cache);
            mdkr_camera_object_occlusion_reject("BVH validation failed");
            return;
        }
    }
    free(chunk_order);
    chunk_order = NULL;

    cache->model = model;
    cache->generation = model_generation;
    cache->world.vertices = cache->vertices;
    cache->world.vertex_count = (size_t)model->numberOfVertices;
    cache->world.indices = cache->indices;
    cache->world.triangles = cache->triangles;
    cache->world.triangle_count = triangle_count;
    cache->bytes = sizeof(*cache) + vertex_bytes + index_bytes + triangle_bytes +
        chunk_bytes + node_bytes;
    cache->next = sCameraObjectOcclusionCaches;
    sCameraObjectOcclusionCaches = cache;
    sCameraObjectOcclusionTelemetry.model_cache_count++;
    sCameraObjectOcclusionTelemetry.built_model_count++;
    sCameraObjectOcclusionTelemetry.vertex_count += cache->world.vertex_count;
    sCameraObjectOcclusionTelemetry.hard_triangle_count += triangle_count;
    sCameraObjectOcclusionTelemetry.chunk_count += cache->chunk_count;
    sCameraObjectOcclusionTelemetry.node_count += cache->node_count;
    sCameraObjectOcclusionTelemetry.bytes += cache->bytes;
    sCameraObjectOcclusionNextGeneration++;
    sCameraObjectOcclusionNextStableId += (uint32_t)triangle_capacity;
    if (mdkr_camera_object_occlusion_trace_enabled()) {
        fprintf(stderr,
                "[CAM-OBJECT-OCCLUSION] model=%p generation=%u vertices=%zu hard=%zu bytes=%zu\n",
                (void *)model, cache->generation, cache->world.vertex_count,
                cache->world.triangle_count, cache->bytes);
    }
}

void mdkr_camera_object_occlusion_model_pre_free(ObjectModel *model) {
    mdkr_camera_object_occlusion_remove(model);
}

const MdkrCameraOcclusionWorld *mdkr_camera_object_occlusion_world_for_model(
    const ObjectModel *model,
    uint32_t *out_generation) {
    MdkrCameraObjectOcclusionCache *cache = mdkr_camera_object_occlusion_find(model);

    if (out_generation != NULL) {
        *out_generation = cache != NULL ? cache->generation : 0U;
    }
    return cache != NULL ? &cache->world : NULL;
}

static int mdkr_camera_object_occlusion_chunk_intersects(
    const MdkrCameraObjectOcclusionChunk *chunk,
    MdkrCameraVec3 start,
    MdkrCameraVec3 desired,
    double radius) {
    const double start_axis[3] = { start.x, start.y, start.z };
    const double desired_axis[3] = { desired.x, desired.y, desired.z };
    const double minimum[3] = {
        (double)chunk->minimum.x - radius,
        (double)chunk->minimum.y - radius,
        (double)chunk->minimum.z - radius,
    };
    const double maximum[3] = {
        (double)chunk->maximum.x + radius,
        (double)chunk->maximum.y + radius,
        (double)chunk->maximum.z + radius,
    };
    double enter = 0.0;
    double exit = 1.0;
    size_t axis;

    if (chunk == NULL || chunk->valid != 1 ||
        !mdkr_camera_object_occlusion_bounds_valid(chunk->minimum, chunk->maximum) ||
        !isfinite(radius) || radius < 0.0 ||
        !isfinite(start.x) || !isfinite(start.y) || !isfinite(start.z) ||
        !isfinite(desired.x) || !isfinite(desired.y) || !isfinite(desired.z)) {
        return -1;
    }
    for (axis = 0U; axis < 3U; axis++) {
        const double delta = desired_axis[axis] - start_axis[axis];
        if (!isfinite(minimum[axis]) || !isfinite(maximum[axis]) ||
            !isfinite(delta)) return -1;
        if (fabs(delta) <= DBL_MIN) {
            if (start_axis[axis] < minimum[axis] || start_axis[axis] > maximum[axis]) {
                return 0;
            }
        } else {
            double first = (minimum[axis] - start_axis[axis]) / delta;
            double last = (maximum[axis] - start_axis[axis]) / delta;
            if (first > last) {
                const double temporary = first;
                first = last;
                last = temporary;
            }
            if (first > enter) enter = first;
            if (last < exit) exit = last;
            if (enter > exit) return 0;
        }
    }
    return exit >= 0.0 && enter <= 1.0;
}

static int mdkr_camera_object_occlusion_accumulate_kernel_work(
    MdkrCameraRoundedLensSweepTelemetry *total,
    const MdkrCameraRoundedLensSweepTelemetry *part) {
#define MDKR_CAMERA_OBJECT_ADD_WORK(field) \
    do { \
        if (UINT64_MAX - total->field < part->field) return 0; \
        total->field += part->field; \
    } while (0)
    MDKR_CAMERA_OBJECT_ADD_WORK(triangles_seen);
    MDKR_CAMERA_OBJECT_ADD_WORK(triangles_filtered);
    MDKR_CAMERA_OBJECT_ADD_WORK(triangles_aabb_rejected);
    MDKR_CAMERA_OBJECT_ADD_WORK(triangles_narrowed);
    MDKR_CAMERA_OBJECT_ADD_WORK(analytic_swept_sat_tests);
    MDKR_CAMERA_OBJECT_ADD_WORK(analytic_revalidation_misses);
    MDKR_CAMERA_OBJECT_ADD_WORK(bounded_interval_tests);
    MDKR_CAMERA_OBJECT_ADD_WORK(bounded_interval_exhaustions);
    MDKR_CAMERA_OBJECT_ADD_WORK(stationary_tests);
    MDKR_CAMERA_OBJECT_ADD_WORK(conservative_advance_iterations);
    MDKR_CAMERA_OBJECT_ADD_WORK(contact_refinement_tests);
    MDKR_CAMERA_OBJECT_ADD_WORK(interval_fallbacks);
    MDKR_CAMERA_OBJECT_ADD_WORK(interval_samples);
    MDKR_CAMERA_OBJECT_ADD_WORK(ambiguous_intervals);
    MDKR_CAMERA_OBJECT_ADD_WORK(publication_revalidations);
#undef MDKR_CAMERA_OBJECT_ADD_WORK
    return 1;
}

static int mdkr_camera_object_occlusion_hit_precedes(
    const MdkrCameraSweepHit *candidate,
    const MdkrCameraSweepHit *best) {
    if ((double)candidate->fraction < (double)best->fraction -
            MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) return 1;
    if (fabs((double)candidate->fraction - (double)best->fraction) >
            MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) return 0;
    if (candidate->stable_id != best->stable_id) {
        return candidate->stable_id < best->stable_id;
    }
    return candidate->feature < best->feature;
}

static void mdkr_camera_object_occlusion_set_clear_hit(
    MdkrCameraSweepHit *out_hit) {
    memset(out_hit, 0, sizeof(*out_hit));
    out_hit->fraction = 1.0f;
    out_hit->clearance = INFINITY;
}

static int mdkr_camera_object_occlusion_cache_layout_valid(
    const MdkrCameraObjectOcclusionCache *cache) {
    size_t expected_chunk_count;
    size_t expected_node_count;

    if (cache == NULL || cache->world.vertices != cache->vertices ||
        cache->world.indices != cache->indices ||
        cache->world.triangles != cache->triangles ||
        cache->world.vertex_count != cache->vertex_capacity ||
        cache->world.triangle_count > cache->triangle_capacity ||
        cache->chunk_count > cache->chunk_capacity ||
        cache->node_count > cache->node_capacity) {
        return 0;
    }
    expected_chunk_count = cache->world.triangle_count /
        MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES +
        (cache->world.triangle_count % MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES != 0U);
    if (expected_chunk_count > SIZE_MAX / 2U + 1U) return 0;
    expected_node_count = expected_chunk_count == 0U ? 0U :
        expected_chunk_count * 2U - 1U;
    return cache->chunk_count == expected_chunk_count &&
           cache->node_count == expected_node_count;
}

MdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model_profiled(
    const ObjectModel *model,
    uint32_t expected_model_generation,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    const MdkrCameraObjectOcclusionExactLimits *limits,
    MdkrCameraObjectOcclusionExactWork *out_work) {
    MdkrCameraObjectOcclusionCache *cache = mdkr_camera_object_occlusion_find(model);
    MdkrCameraVec3 local_start;
    MdkrCameraVec3 local_desired;
    MdkrCameraSweepHit best;
    size_t node_stack[MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES];
    float scale;
    double local_radius;
    size_t stack_count = 0U;
    int have_hit = 0;

    if (out_work != NULL) memset(out_work, 0, sizeof(*out_work));
    if (out_world_hit != NULL) memset(out_world_hit, 0, sizeof(*out_world_hit));
    if (cache == NULL || cache->generation != expected_model_generation ||
        expected_model_generation == 0U || out_world_hit == NULL || out_work == NULL ||
        limits == NULL || limits->nodes == 0U || limits->chunks == 0U ||
        limits->triangles == 0U ||
        limits->nodes > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES ||
        limits->chunks > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS ||
        limits->triangles > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES ||
        world_input == NULL || object_transform == NULL ||
        world_input->guard.kind != MDKR_CAMERA_LENS_GUARD_SPHERE ||
        !isfinite(world_input->guard.radius) || world_input->guard.radius < 0.0f) {
        if (cache == NULL || cache->generation != expected_model_generation) {
            sCameraObjectOcclusionTelemetry.missing_cache_query_count++;
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (!mdkr_camera_object_occlusion_cache_layout_valid(cache) ||
        !mdkr_camera_object_transform_validate(object_transform, &scale) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->start_eye, &local_start) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->desired_eye, &local_desired)) {
        sCameraObjectOcclusionTelemetry.invalid_query_count++;
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_radius = (double)world_input->guard.radius / scale;
    if (!isfinite(local_radius) || local_radius < 0.0) {
        sCameraObjectOcclusionTelemetry.invalid_query_count++;
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(&best, 0, sizeof(best));
    if (cache->node_count != 0U) node_stack[stack_count++] = 0U;
    while (stack_count != 0U) {
        const size_t node_index = node_stack[--stack_count];
        const MdkrCameraObjectOcclusionNode *node;
        MdkrCameraObjectOcclusionChunk node_bounds;
        const MdkrCameraObjectOcclusionChunk *chunk;
        MdkrCameraOcclusionWorld chunk_world;
        MdkrCameraSweepHit candidate;
        MdkrCameraSweepStatus status;
        int overlap;

        if (node_index >= cache->node_count) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (out_work->nodes_visited >= limits->nodes) {
            out_work->exhausted = 1U;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        node = &cache->nodes[node_index];
        out_work->nodes_visited++;
        if (node->valid != 1 || (node->leaf != 0 && node->leaf != 1) ||
            !mdkr_camera_object_occlusion_bounds_valid(node->minimum, node->maximum) ||
            node->integrity != mdkr_camera_object_occlusion_node_integrity(node)) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        memset(&node_bounds, 0, sizeof(node_bounds));
        node_bounds.minimum = node->minimum;
        node_bounds.maximum = node->maximum;
        node_bounds.valid = 1;
        overlap = mdkr_camera_object_occlusion_chunk_intersects(
            &node_bounds, local_start, local_desired, local_radius);
        if (overlap < 0) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (overlap == 0) {
            out_work->nodes_rejected++;
            continue;
        }
        if (node->leaf == 0) {
            if (node->left >= cache->node_count || node->right >= cache->node_count ||
                node->left == node->right || node->left == node_index ||
                node->right == node_index) {
                sCameraObjectOcclusionTelemetry.invalid_query_count++;
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (stack_count + 2U > limits->nodes) {
                out_work->exhausted = 1U;
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            node_stack[stack_count++] = node->right;
            node_stack[stack_count++] = node->left;
            continue;
        }
        if (node->chunk_index >= cache->chunk_count) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (out_work->chunks_retained >= limits->chunks) {
            out_work->exhausted = 1U;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        chunk = &cache->chunks[node->chunk_index];
        if (chunk->valid != 1 ||
            !mdkr_camera_object_occlusion_bounds_valid(chunk->minimum, chunk->maximum) ||
            chunk->integrity != mdkr_camera_object_occlusion_chunk_integrity(chunk) ||
            chunk->triangle_count == 0U ||
            chunk->triangle_count > MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES ||
            chunk->triangle_offset > cache->world.triangle_count ||
            chunk->triangle_count > cache->world.triangle_count - chunk->triangle_offset ||
            chunk->triangle_offset != node->chunk_index *
                MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES ||
            !mdkr_camera_object_occlusion_bounds_contain(
                node->minimum, node->maximum, chunk->minimum, chunk->maximum) ||
            !mdkr_camera_object_occlusion_bounds_contain(
                chunk->minimum, chunk->maximum, node->minimum, node->maximum)) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (chunk->triangle_count >
            limits->triangles - out_work->triangles_retained) {
            out_work->exhausted = 1U;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        out_work->chunks_retained++;
        out_work->triangles_retained += chunk->triangle_count;
        chunk_world = cache->world;
        chunk_world.indices += chunk->triangle_offset * 3U;
        chunk_world.triangles += chunk->triangle_offset;
        chunk_world.triangle_count = chunk->triangle_count;
        status = mdkr_camera_sweep_object_local(
            &chunk_world, object_transform, world_input, &candidate);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            memset(out_world_hit, 0, sizeof(*out_world_hit));
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_HIT &&
            (!have_hit || mdkr_camera_object_occlusion_hit_precedes(&candidate, &best))) {
            best = candidate;
            have_hit = 1;
        }
    }
    sCameraObjectOcclusionTelemetry.query_count++;
    if (have_hit) {
        *out_world_hit = best;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    mdkr_camera_object_occlusion_set_clear_hit(out_world_hit);
    return MDKR_CAMERA_SWEEP_CLEAR;
}

MdkrCameraSweepStatus mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(
    const ObjectModel *model,
    uint32_t expected_model_generation,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    const MdkrCameraObjectOcclusionExactLimits *limits,
    MdkrCameraObjectOcclusionExactWork *out_work) {
    MdkrCameraObjectOcclusionCache *cache = mdkr_camera_object_occlusion_find(model);
    MdkrCameraRoundedLensSweepInput local_input;
    MdkrCameraSweepHit best;
    size_t node_stack[MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES];
    double local_radius;
    size_t stack_count = 0U;
    int have_hit = 0;

    if (out_work != NULL) memset(out_work, 0, sizeof(*out_work));
    if (out_world_hit != NULL) memset(out_world_hit, 0, sizeof(*out_world_hit));
    if (cache == NULL || cache->generation != expected_model_generation ||
        expected_model_generation == 0U || out_world_hit == NULL || out_work == NULL ||
        limits == NULL || limits->nodes == 0U || limits->chunks == 0U ||
        limits->triangles == 0U || limits->stationary_tests == 0U ||
        limits->nodes > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES ||
        limits->chunks > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS ||
        limits->triangles > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES ||
        limits->stationary_tests > MDKR_CAMERA_OBJECT_OCCLUSION_MAX_STATIONARY_TESTS ||
        world_input == NULL || object_transform == NULL) {
        if (cache == NULL || cache->generation != expected_model_generation) {
            sCameraObjectOcclusionTelemetry.missing_cache_query_count++;
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (!mdkr_camera_object_occlusion_cache_layout_valid(cache) ||
        !mdkr_camera_rounded_lens_guard_to_object_local(
            object_transform, &world_input->guard, world_input->start_eye,
            &local_input.start_eye, &local_input.guard) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->desired_eye, &local_input.desired_eye) ||
        !mdkr_camera_rounded_lens_guard_conservative_radius(
            &local_input.guard, &local_radius)) {
        sCameraObjectOcclusionTelemetry.invalid_query_count++;
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_input.mask = world_input->mask;
    local_input.ignored_object_generation = world_input->ignored_object_generation;
    memset(&best, 0, sizeof(best));
    if (cache->node_count != 0U) node_stack[stack_count++] = 0U;
    while (stack_count != 0U) {
        const size_t node_index = node_stack[--stack_count];
        const MdkrCameraObjectOcclusionNode *node;
        MdkrCameraObjectOcclusionChunk node_bounds;
        const MdkrCameraObjectOcclusionChunk *chunk;
        MdkrCameraOcclusionWorld chunk_world;
        MdkrCameraRoundedLensSweepTelemetry part;
        MdkrCameraSweepHit candidate;
        MdkrCameraSweepStatus status;
        int overlap;

        /* A corrupt child index and a reached work fence both fail closed, but
         * they are not the same event: only the fence is healthy bounded
         * operation, so only the fence sets the flag the consumer classifies
         * on. Kept split for that reason, as in the sphere sibling above. */
        if (node_index >= cache->node_count) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (out_work->nodes_visited >= limits->nodes) {
            out_work->exhausted = 1U;
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        node = &cache->nodes[node_index];
        out_work->nodes_visited++;
        if (node->valid != 1 || (node->leaf != 0 && node->leaf != 1) ||
            !mdkr_camera_object_occlusion_bounds_valid(node->minimum, node->maximum) ||
            node->integrity != mdkr_camera_object_occlusion_node_integrity(node)) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        memset(&node_bounds, 0, sizeof(node_bounds));
        node_bounds.minimum = node->minimum;
        node_bounds.maximum = node->maximum;
        node_bounds.valid = 1;
        overlap = mdkr_camera_object_occlusion_chunk_intersects(
            &node_bounds, local_input.start_eye, local_input.desired_eye,
            local_radius);
        if (overlap < 0) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (overlap == 0) {
            out_work->nodes_rejected++;
            continue;
        }
        if (node->leaf == 0) {
            if (node->left >= cache->node_count || node->right >= cache->node_count ||
                node->left == node->right || node->left == node_index ||
                node->right == node_index) {
                sCameraObjectOcclusionTelemetry.invalid_query_count++;
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            /* The node stack is the one fence the consumer cannot recover from
             * the reported work counters: stack depth is local to this frame
             * and nodes_visited can sit arbitrarily far below its own fence
             * when the stack fills. Without this flag a healthy bounded
             * traversal is indistinguishable from index corruption. */
            if (stack_count + 2U > limits->nodes) {
                out_work->exhausted = 1U;
                sCameraObjectOcclusionTelemetry.invalid_query_count++;
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            /* LIFO right then left preserves deterministic left-first traversal. */
            node_stack[stack_count++] = node->right;
            node_stack[stack_count++] = node->left;
            continue;
        }
        if (node->chunk_index >= cache->chunk_count) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (out_work->chunks_retained >= limits->chunks) {
            out_work->exhausted = 1U;
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        chunk = &cache->chunks[node->chunk_index];
        if (chunk->valid != 1 ||
            !mdkr_camera_object_occlusion_bounds_valid(chunk->minimum, chunk->maximum) ||
            chunk->integrity != mdkr_camera_object_occlusion_chunk_integrity(chunk) ||
            chunk->triangle_count == 0U ||
            chunk->triangle_count > MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES ||
            chunk->triangle_offset > cache->world.triangle_count ||
            chunk->triangle_count > cache->world.triangle_count - chunk->triangle_offset ||
            chunk->triangle_offset != node->chunk_index *
                MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES ||
            !mdkr_camera_object_occlusion_bounds_contain(
                node->minimum, node->maximum, chunk->minimum, chunk->maximum) ||
            !mdkr_camera_object_occlusion_bounds_contain(
                chunk->minimum, chunk->maximum, node->minimum, node->maximum)) {
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (chunk->triangle_count > limits->triangles -
                out_work->triangles_retained) {
            out_work->exhausted = 1U;
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            memset(out_world_hit, 0, sizeof(*out_world_hit));
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        out_work->chunks_retained++;
        out_work->triangles_retained += chunk->triangle_count;
        chunk_world = cache->world;
        chunk_world.indices += chunk->triangle_offset * 3U;
        chunk_world.triangles += chunk->triangle_offset;
        chunk_world.triangle_count = chunk->triangle_count;
        if (out_work->kernel.stationary_tests >= limits->stationary_tests) {
            out_work->exhausted = 1U;
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        status = mdkr_camera_rounded_lens_sweep_object_local_profiled_limited(
            &chunk_world, object_transform, world_input, &candidate, &part,
            limits->stationary_tests - out_work->kernel.stationary_tests);
        if (!mdkr_camera_object_occlusion_accumulate_kernel_work(
                &out_work->kernel, &part) || status == MDKR_CAMERA_SWEEP_INVALID) {
            /* The kernel spends the remaining stationary-test budget handed to
             * it above and returns INVALID the moment it needs one more, so
             * this exit carries the same fence as the pre-call check, reached
             * one triangle later instead of one chunk earlier. The accumulated
             * count separates that from a genuinely invalid chunk, and it is
             * exactly the condition the consumer already derives, so flagging
             * it here only moves the answer to its authoritative source. */
            if (out_work->kernel.stationary_tests >= limits->stationary_tests) {
                out_work->exhausted = 1U;
            }
            sCameraObjectOcclusionTelemetry.invalid_query_count++;
            memset(out_world_hit, 0, sizeof(*out_world_hit));
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_HIT &&
            (!have_hit || mdkr_camera_object_occlusion_hit_precedes(&candidate, &best))) {
            best = candidate;
            have_hit = 1;
        }
    }
    sCameraObjectOcclusionTelemetry.query_count++;
    if (have_hit) {
        *out_world_hit = best;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    mdkr_camera_object_occlusion_set_clear_hit(out_world_hit);
    return MDKR_CAMERA_SWEEP_CLEAR;
}

MdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model(
    const ObjectModel *model,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit) {
    MdkrCameraObjectOcclusionCache *cache = mdkr_camera_object_occlusion_find(model);
    MdkrCameraSweepStatus status;

    if (cache == NULL) {
        sCameraObjectOcclusionTelemetry.missing_cache_query_count++;
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    /* The transform wrapper copies world_input verbatim before changing only
     * local eye coordinates/radius, so mask and ignored_object_generation keep
     * their kernel meaning for this cache's generation-tagged triangles. */
    status = mdkr_camera_sweep_object_local(
        &cache->world, object_transform, world_input, out_world_hit);
    sCameraObjectOcclusionTelemetry.query_count++;
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        sCameraObjectOcclusionTelemetry.invalid_query_count++;
    }
    return status;
}

void mdkr_camera_object_occlusion_get_telemetry(
    MdkrCameraObjectOcclusionTelemetry *out_telemetry) {
    if (out_telemetry != NULL) {
        *out_telemetry = sCameraObjectOcclusionTelemetry;
    }
}

#endif /* NATIVE_PORT */
