#include <math.h>
#include <stdio.h>
#include <string.h>

/* White-box by design: this ROM-free target exercises the exact immutable cache
 * representation and fault paths, not a second test-only BVH implementation. */
#include "../game/src/camera_object_occlusion.c"

/* Model loading is not part of this fixture, but the included production
 * translation unit retains its native pointer resolver reference. */
void *dkr_lo32_to_ptr(uint32_t lo32) {
    (void)lo32;
    return NULL;
}

static int failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void build_fixture(
    MdkrCameraObjectOcclusionCache *cache,
    ObjectModel *model,
    MdkrCameraVec3 *vertices,
    uint32_t *indices,
    MdkrCameraOcclusionTriangle *triangles,
    MdkrCameraObjectOcclusionChunk *chunks,
    MdkrCameraObjectOcclusionNode *nodes) {
    size_t triangle_index;
    size_t chunk_index;
    size_t order[2] = { 0U, 1U };
    size_t root_index = SIZE_MAX;
    size_t seen[2] = { 0U, 0U };
    size_t visited_nodes = 0U;
    size_t visited_leaves = 0U;

    memset(cache, 0, sizeof(*cache));
    memset(model, 0, sizeof(*model));
    memset(chunks, 0, 2U * sizeof(*chunks));
    memset(nodes, 0, 3U * sizeof(*nodes));
    vertices[0] = (MdkrCameraVec3) { 0.0f, -5.0f, -5.0f };
    vertices[1] = (MdkrCameraVec3) { 0.0f, 5.0f, -5.0f };
    vertices[2] = (MdkrCameraVec3) { 0.0f, 0.0f, 5.0f };
    vertices[3] = (MdkrCameraVec3) { 10.0f, -5.0f, -5.0f };
    vertices[4] = (MdkrCameraVec3) { 10.0f, 5.0f, -5.0f };
    vertices[5] = (MdkrCameraVec3) { 10.0f, 0.0f, 5.0f };
    for (triangle_index = 0U; triangle_index < 9U; triangle_index++) {
        const uint32_t vertex_base = triangle_index < 8U ? 0U : 3U;
        indices[triangle_index * 3U] = vertex_base;
        indices[triangle_index * 3U + 1U] = vertex_base + 1U;
        indices[triangle_index * 3U + 2U] = vertex_base + 2U;
        triangles[triangle_index] = (MdkrCameraOcclusionTriangle) {
            (uint32_t)(100U + triangle_index),
            MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
            MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
            7U,
        };
    }
    cache->model = model;
    cache->generation = 7U;
    cache->vertices = vertices;
    cache->indices = indices;
    cache->triangles = triangles;
    cache->vertex_capacity = 6U;
    cache->triangle_capacity = 9U;
    cache->chunks = chunks;
    cache->chunk_capacity = 2U;
    cache->chunk_count = 2U;
    cache->nodes = nodes;
    cache->node_capacity = 3U;
    cache->world = (MdkrCameraOcclusionWorld) {
        vertices, 6U, indices, triangles, 9U,
    };
    for (chunk_index = 0U; chunk_index < 2U; chunk_index++) {
        MdkrCameraObjectOcclusionChunk *chunk = &chunks[chunk_index];
        const size_t begin = chunk_index * 8U;
        const size_t end = chunk_index == 0U ? 8U : 9U;
        chunk->triangle_offset = begin;
        chunk->triangle_count = end - begin;
        for (triangle_index = begin; triangle_index < end; triangle_index++) {
            const size_t offset = triangle_index * 3U;
            mdkr_camera_object_occlusion_expand_chunk(chunk, vertices[indices[offset]]);
            mdkr_camera_object_occlusion_expand_chunk(chunk, vertices[indices[offset + 1U]]);
            mdkr_camera_object_occlusion_expand_chunk(chunk, vertices[indices[offset + 2U]]);
        }
        chunk->integrity = mdkr_camera_object_occlusion_chunk_integrity(chunk);
    }
    expect_true("fixture BVH builds",
                mdkr_camera_object_occlusion_build_node(
                    cache, order, 0U, 2U, 3U, &root_index));
    expect_true("fixture BVH shape", root_index == 0U && cache->node_count == 3U);
    expect_true("fixture BVH validates",
                mdkr_camera_object_occlusion_validate_node(
                    cache, root_index, NULL, seen, &visited_nodes,
                    &visited_leaves) &&
                    visited_nodes == 3U && visited_leaves == 2U);
    sCameraObjectOcclusionCaches = cache;
}

static MdkrCameraSweepStatus indexed_sphere(
    ObjectModel *model,
    uint32_t generation,
    const MdkrCameraObjectTransform *transform,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *hit,
    MdkrCameraObjectOcclusionExactLimits limits,
    MdkrCameraObjectOcclusionExactWork *work) {
    return mdkr_camera_object_occlusion_sweep_model_profiled(
        model, generation, transform, input, hit, &limits, work);
}

static MdkrCameraSweepStatus indexed_exact(
    ObjectModel *model,
    uint32_t generation,
    const MdkrCameraObjectTransform *transform,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *hit,
    MdkrCameraObjectOcclusionExactLimits limits,
    MdkrCameraObjectOcclusionExactWork *work) {
    return mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(
        model, generation, transform, input, hit, &limits, work);
}

int main(void) {
    ObjectModel model;
    MdkrCameraObjectOcclusionCache cache;
    MdkrCameraVec3 vertices[6];
    uint32_t indices[27];
    MdkrCameraOcclusionTriangle triangles[9];
    MdkrCameraObjectOcclusionChunk chunks[2];
    MdkrCameraObjectOcclusionNode nodes[3];
    MdkrCameraObjectOcclusionNode saved_root;
    MdkrCameraObjectOcclusionChunk saved_chunk;
    MdkrCameraObjectTransform transform;
    MdkrCameraObjectTransform pinned_transform;
    MdkrCameraSweepInput pinned_input;
    MdkrCameraSweepInput sphere_input;
    MdkrCameraRoundedLensSweepInput exact_input;
    MdkrCameraSweepHit linear_hit;
    MdkrCameraSweepHit indexed_hit;
    MdkrCameraObjectOcclusionExactWork work;
    MdkrCameraObjectOcclusionExactLimits limits = { 64U, 16U, 128U, 128U };
    /* Fence-class arms below shrink these locally; the struct is caller-owned. */
    MdkrCameraObjectOcclusionExactLimits stationary_limits;
    uint64_t first_chunk_stationary_tests = 0U;
    MdkrCameraSweepStatus linear_status;
    MdkrCameraSweepStatus indexed_status;
    /* The sphere arm's own result. Reusing one variable for both arms silently
     * compared later sphere queries against the exact sweep's status. */
    MdkrCameraSweepStatus sphere_linear_status;
    MdkrCameraObjectOcclusionExactWork cull_work;
    MdkrCameraSweepInput cull_input;
    size_t culled_iterations = 0U;
    size_t seen[2] = { 0U, 0U };
    size_t visited_nodes = 0U;
    size_t visited_leaves = 0U;
    size_t case_index;

    build_fixture(&cache, &model, vertices, indices, triangles, chunks, nodes);
    expect_true("identity transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3) { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, 1.0f,
        &transform));
    sphere_input = (MdkrCameraSweepInput) {
        { MDKR_CAMERA_LENS_GUARD_SPHERE, 0.25f },
        { -5.0f, 0.0f, 0.0f },
        { 15.0f, 0.0f, 0.0f },
        MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
        0U,
    };
    sphere_linear_status = mdkr_camera_sweep_object_local(
        &cache.world, &transform, &sphere_input, &linear_hit);
    linear_status = sphere_linear_status;
    indexed_status = indexed_sphere(
        &model, 7U, &transform, &sphere_input, &indexed_hit, limits, &work);
    expect_true("sphere indexed/full status", indexed_status == sphere_linear_status);
    expect_true("sphere indexed/full hit", memcmp(&indexed_hit, &linear_hit, sizeof(linear_hit)) == 0);
    expect_true("sphere work census",
                work.nodes_visited == 3U && work.chunks_retained == 2U &&
                work.triangles_retained == 9U);

    /*
     * The census above spans the whole model, so on its own it cannot tell a
     * working index from one that never culls. These two queries are chosen
     * geometrically: the fixture's chunks sit on the x = 0 and x = 10 planes,
     * so a sweep confined to one side must retain exactly one chunk and must
     * visit fewer nodes and triangles than the model holds. An index that
     * returns every chunk still answers correctly and is still wrong.
     */
    cull_input = sphere_input;
    cull_input.start_eye = (MdkrCameraVec3) { -5.0f, 0.0f, 0.0f };
    cull_input.desired_eye = (MdkrCameraVec3) { 5.0f, 0.0f, 0.0f };
    indexed_status = indexed_sphere(
        &model, 7U, &transform, &cull_input, &indexed_hit, limits, &cull_work);
    expect_true("near-chunk query culls the far chunk",
                indexed_status != MDKR_CAMERA_SWEEP_INVALID &&
                cull_work.chunks_retained < 2U &&
                cull_work.triangles_retained < 9U);
    expect_true("near-chunk query keeps its own chunk",
                cull_work.nodes_visited <= 3U &&
                cull_work.chunks_retained == 1U &&
                cull_work.triangles_retained == 8U);

    cull_input.start_eye = (MdkrCameraVec3) { 7.0f, 0.0f, 0.0f };
    cull_input.desired_eye = (MdkrCameraVec3) { 15.0f, 0.0f, 0.0f };
    indexed_status = indexed_sphere(
        &model, 7U, &transform, &cull_input, &indexed_hit, limits, &cull_work);
    expect_true("far-chunk query culls the near chunk",
                indexed_status != MDKR_CAMERA_SWEEP_INVALID &&
                cull_work.chunks_retained < 2U &&
                cull_work.triangles_retained < 9U);
    expect_true("far-chunk query keeps its own chunk",
                cull_work.nodes_visited <= 3U &&
                cull_work.chunks_retained == 1U &&
                cull_work.triangles_retained == 1U);

    /*
     * A query wholly outside the model must be rejected at the root, so this is
     * the arm that makes nodes_visited discriminating: this fixture is two
     * leaves under one root, and a traversal that descends at all reaches every
     * node in it. Both children are examined whenever the root overlaps, so
     * node-level strictness below the root needs a deeper tree than a
     * three-node fixture can express.
     */
    cull_input.start_eye = (MdkrCameraVec3) { 50.0f, 0.0f, 0.0f };
    cull_input.desired_eye = (MdkrCameraVec3) { 60.0f, 0.0f, 0.0f };
    indexed_status = indexed_sphere(
        &model, 7U, &transform, &cull_input, &indexed_hit, limits, &cull_work);
    expect_true("disjoint query is rejected at the root",
                indexed_status == MDKR_CAMERA_SWEEP_CLEAR &&
                cull_work.nodes_visited < 3U &&
                cull_work.chunks_retained < 2U &&
                cull_work.triangles_retained < 9U);
    expect_true("disjoint query does no leaf work",
                cull_work.nodes_visited == 1U &&
                cull_work.chunks_retained == 0U &&
                cull_work.triangles_retained == 0U);

    exact_input.start_eye = sphere_input.start_eye;
    exact_input.desired_eye = sphere_input.desired_eye;
    exact_input.mask = sphere_input.mask;
    exact_input.ignored_object_generation = 0U;
    expect_true("exact guard", mdkr_camera_rounded_lens_guard_from_projection(
        1.0f, 1.0471975512f, 1.0f, 0.1f,
        (MdkrCameraVec3) { 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3) { 0.0f, 1.0f, 0.0f }, &exact_input.guard));
    linear_status = mdkr_camera_rounded_lens_sweep_object_local(
        &cache.world, &transform, &exact_input, &linear_hit);
    indexed_status = indexed_exact(
        &model, 7U, &transform, &exact_input, &indexed_hit, limits, &work);
    expect_true("exact indexed/full status", indexed_status == linear_status);
    expect_true("exact indexed/full hit", memcmp(&indexed_hit, &linear_hit, sizeof(linear_hit)) == 0);

    expect_true("sphere node cap fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 1U, 16U, 128U, 1U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID && work.exhausted == 1U);
    expect_true("sphere chunk cap fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 1U, 128U, 1U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID && work.exhausted == 1U);
    expect_true("sphere triangle cap fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 16U, 8U, 1U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID && work.exhausted == 1U);
    expect_true("exact stationary cap fails closed",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 16U, 128U, 1U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.kernel.stationary_tests == 1U);
    expect_true("exact node cap fails closed",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 1U, 16U, 128U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("exact chunk cap fails closed",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 1U, 128U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("exact triangle cap fails closed",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 16U, 8U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID);

    /*
     * Failing closed is only half the contract. The consumer classifies a
     * fenced query apart from a corrupt one on
     * MdkrCameraObjectOcclusionExactWork::exhausted, honouring the flag before
     * any counter-derived guess, so every fence in the exact lens sweep has to
     * raise it. The limits struct is caller-supplied, so each class below is
     * reached by shrinking one fence to the value that trips it, with the work
     * census pinning which fence was hit -- otherwise a single early exit
     * could satisfy every arm.
     *
     * The node stack is the reason these are asserted individually: its fence
     * leaves no trace in the reported counters (stack depth is frame-local and
     * nodes_visited stops well short of its own fence), so a consumer that
     * derives the answer instead of reading the flag reads healthy bounded
     * work as index corruption.
     */
    expect_true("exact node-visit fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 2U, 16U, 128U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.nodes_visited == 2U);
    expect_true("exact node-stack fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 1U, 16U, 128U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.nodes_visited == 1U &&
                    work.chunks_retained == 0U);
    expect_true("exact chunk fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 1U, 128U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.chunks_retained == 1U);
    first_chunk_stationary_tests = work.kernel.stationary_tests;
    expect_true("exact triangle fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 16U, 8U, 128U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.chunks_retained == 1U &&
                    work.triangles_retained <= 8U);
    /* Two distinct stationary-test exits share one fence. The kernel returns
     * INVALID mid-chunk when the budget it was handed runs out; the sweep's own
     * pre-call check fires on the next leaf when the budget is already spent.
     * Handing the query exactly the first chunk's measured demand reaches the
     * second, so both are covered. */
    expect_true("first chunk spends stationary tests",
                first_chunk_stationary_tests > 0U);
    expect_true("exact in-kernel stationary fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    (MdkrCameraObjectOcclusionExactLimits) { 64U, 16U, 128U, 1U },
                    &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.kernel.stationary_tests == 1U &&
                    work.chunks_retained == 1U);
    stationary_limits = (MdkrCameraObjectOcclusionExactLimits) {
        64U, 16U, 128U, first_chunk_stationary_tests,
    };
    expect_true("exact pre-call stationary fence flags exhausted",
                indexed_exact(&model, 7U, &transform, &exact_input, &indexed_hit,
                    stationary_limits, &work) == MDKR_CAMERA_SWEEP_INVALID &&
                    work.exhausted == 1U && work.chunks_retained == 2U &&
                    work.kernel.stationary_tests == first_chunk_stationary_tests);
    expect_true("generation mismatch fails closed",
                indexed_sphere(&model, 6U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);

    saved_root = nodes[0];
    nodes[0].minimum.x = NAN;
    expect_true("NaN node fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    nodes[0] = saved_root;
    nodes[0].leaf = 2;
    expect_true("non-boolean leaf fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    nodes[0] = saved_root;
    nodes[0].right = nodes[0].left;
    nodes[0].integrity = mdkr_camera_object_occlusion_node_integrity(&nodes[0]);
    expect_true("duplicate child fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    nodes[0] = saved_root;

    saved_chunk = chunks[1];
    chunks[1].triangle_offset = 7U;
    chunks[1].integrity = mdkr_camera_object_occlusion_chunk_integrity(&chunks[1]);
    expect_true("corrupt chunk range fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    chunks[1] = saved_chunk;

    cache.world.triangle_count = 8U;
    expect_true("incomplete chunk coverage fails closed",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    cache.world.triangle_count = 9U;

    nodes[0].maximum.x = 1.0f;
    nodes[0].integrity = mdkr_camera_object_occlusion_node_integrity(&nodes[0]);
    expect_true("build validator rejects shrunken parent",
                !mdkr_camera_object_occlusion_validate_node(
                    &cache, 0U, NULL, seen, &visited_nodes, &visited_leaves));
    nodes[0] = saved_root;

    cache.generation = 8U;
    expect_true("reused model pointer rejects stale generation",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("reused model pointer accepts new generation",
                indexed_sphere(&model, 8U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == sphere_linear_status);
    cache.generation = 7U;

    expect_true("rotated scaled transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3) { 30.0f, -7.0f, 12.0f }, 0.7f, -0.2f, 0.3f, 2.0f,
        &transform));
    expect_true("rotated exact guard", mdkr_camera_rounded_lens_guard_from_projection(
        2.0f, 1.0471975512f, 1.0f, 0.2f,
        (MdkrCameraVec3) {
            transform.local_x_axis.x * 0.5f,
            transform.local_x_axis.y * 0.5f,
            transform.local_x_axis.z * 0.5f,
        },
        (MdkrCameraVec3) {
            transform.local_y_axis.x * 0.5f,
            transform.local_y_axis.y * 0.5f,
            transform.local_y_axis.z * 0.5f,
        }, &exact_input.guard));
    exact_input.mask = MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK;
    exact_input.ignored_object_generation = 0U;

    /* Every fault arm above ran under the identity transform, where the
     * object-local and world frames coincide. Re-run one under the rotated,
     * scaled transform: a validator that only fails closed in object space is
     * not the guarantee the query needs. */
    /* Span the whole model in object space so both chunks are retained and the
     * corrupted one is actually reached. */
    expect_true("rotated fault start",
                mdkr_camera_object_transform_point_to_world(
                    &transform, (MdkrCameraVec3) { -8.0f, 0.0f, 0.0f },
                    &sphere_input.start_eye));
    expect_true("rotated fault desired",
                mdkr_camera_object_transform_point_to_world(
                    &transform, (MdkrCameraVec3) { 18.0f, 0.0f, 0.0f },
                    &sphere_input.desired_eye));
    sphere_input.guard.radius = 0.5f;
    saved_root = nodes[0];
    nodes[0].minimum.x = NAN;
    expect_true("NaN node fails closed under a rotated transform",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    nodes[0] = saved_root;
    saved_chunk = chunks[1];
    chunks[1].triangle_offset = 7U;
    chunks[1].integrity = mdkr_camera_object_occlusion_chunk_integrity(&chunks[1]);
    expect_true("corrupt chunk range fails closed under a rotated transform",
                indexed_sphere(&model, 7U, &transform, &sphere_input, &indexed_hit,
                    limits, &work) == MDKR_CAMERA_SWEEP_INVALID);
    chunks[1] = saved_chunk;

    for (case_index = 0U; case_index < 64U; case_index++) {
        MdkrCameraVec3 local_start = {
            -8.0f + 0.03125f * (float)case_index,
            -7.0f + 0.25f * (float)(case_index % 57U),
            -4.0f + 0.125f * (float)(case_index % 49U),
        };
        MdkrCameraVec3 local_desired = {
            18.0f - 0.0625f * (float)case_index,
            local_start.y + 0.5f,
            local_start.z - 0.25f,
        };
        expect_true("transform differential start",
                    mdkr_camera_object_transform_point_to_world(
                        &transform, local_start, &sphere_input.start_eye));
        expect_true("transform differential desired",
                    mdkr_camera_object_transform_point_to_world(
                        &transform, local_desired, &sphere_input.desired_eye));
        sphere_input.guard.radius = 0.5f;
        linear_status = mdkr_camera_sweep_object_local(
            &cache.world, &transform, &sphere_input, &linear_hit);
        indexed_status = indexed_sphere(
            &model, 7U, &transform, &sphere_input, &indexed_hit, limits, &work);
        expect_true("rotated sphere differential status", indexed_status == linear_status);
        expect_true("rotated sphere differential hit",
                    memcmp(&indexed_hit, &linear_hit, sizeof(linear_hit)) == 0);
        /* Matching answers are not enough: the index must also be doing less
         * work than the full-world sweep it agrees with. */
        expect_true("rotated sphere differential work is bounded",
                    work.nodes_visited <= 3U && work.chunks_retained <= 2U &&
                    work.triangles_retained <= 9U && work.exhausted == 0U);
        if (work.chunks_retained < 2U || work.triangles_retained < 9U) {
            culled_iterations++;
        }
        exact_input.start_eye = sphere_input.start_eye;
        exact_input.desired_eye = sphere_input.desired_eye;
        linear_status = mdkr_camera_rounded_lens_sweep_object_local(
            &cache.world, &transform, &exact_input, &linear_hit);
        indexed_status = indexed_exact(
            &model, 7U, &transform, &exact_input, &indexed_hit, limits, &work);
        expect_true("rotated exact differential status", indexed_status == linear_status);
        expect_true("rotated exact differential hit",
                    memcmp(&indexed_hit, &linear_hit, sizeof(linear_hit)) == 0);
        expect_true("rotated exact differential work is bounded",
                    work.nodes_visited <= 3U && work.chunks_retained <= 2U &&
                    work.triangles_retained <= 9U && work.exhausted == 0U);
        if (work.chunks_retained < 2U || work.triangles_retained < 9U) {
            culled_iterations++;
        }
    }
    expect_true("rotated differential sweep exercises real culling",
                culled_iterations > 0U);

    /*
     * Every differential arm above compares the index against
     * mdkr_camera_sweep_object_local, so a fault in that shared oracle agrees
     * with itself and passes. This arm is pinned to values computed by hand
     * from the fixture instead: a quarter turn about Y written out literally,
     * and a world sweep that must reach the far chunk's single triangle first.
     *
     * Object +X maps to world -Z, so the model's x = 10 chunk lies on the world
     * z = -10 plane and its x = 0 chunk on z = 0. A 0.25 sphere travelling +Z
     * from z = -20 contacts the far triangle after 9.75 of the 25-unit path.
     */
    pinned_transform = (MdkrCameraObjectTransform) {
        { 4.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
    };
    pinned_input = (MdkrCameraSweepInput) {
        { MDKR_CAMERA_LENS_GUARD_SPHERE, 0.25f },
        { 4.0f, 0.0f, -20.0f },
        { 4.0f, 0.0f, 5.0f },
        MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK,
        0U,
    };
    linear_status = mdkr_camera_sweep_object_local(
        &cache.world, &pinned_transform, &pinned_input, &linear_hit);
    indexed_status = indexed_sphere(
        &model, 7U, &pinned_transform, &pinned_input, &indexed_hit, limits, &work);
    expect_true("pinned rotated oracle hits the far triangle",
                linear_status == MDKR_CAMERA_SWEEP_HIT &&
                linear_hit.stable_id == 108U &&
                linear_hit.started_overlapping == 0U &&
                fabsf(linear_hit.fraction - 0.39f) < 1.0e-3f &&
                fabsf(linear_hit.point.x - 4.0f) < 1.0e-3f &&
                fabsf(linear_hit.point.y) < 1.0e-3f &&
                fabsf(linear_hit.point.z + 10.0f) < 1.0e-3f);
    expect_true("pinned rotated index hits the far triangle",
                indexed_status == MDKR_CAMERA_SWEEP_HIT &&
                indexed_hit.stable_id == 108U &&
                indexed_hit.started_overlapping == 0U &&
                fabsf(indexed_hit.fraction - 0.39f) < 1.0e-3f &&
                fabsf(indexed_hit.point.x - 4.0f) < 1.0e-3f &&
                fabsf(indexed_hit.point.y) < 1.0e-3f &&
                fabsf(indexed_hit.point.z + 10.0f) < 1.0e-3f);

    sCameraObjectOcclusionCaches = NULL;
    if (failures != 0) {
        fprintf(stderr, "%d camera-object-bvh assertion(s) failed\n", failures);
        return 1;
    }
    printf("camera-object-bvh: all tests passed\n");
    return 0;
}
