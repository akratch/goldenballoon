#!/usr/bin/env python3
"""ROM-free structural/lifecycle contract for dynamic camera occluders."""

from pathlib import Path
import re
import sys


def require(source: str, needle: str) -> int:
    if needle not in source:
        print(f"missing dynamic-occlusion invariant: {needle}", file=sys.stderr)
        return 1
    return 0


def body(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def dynamic_precedes_contract(candidate, best, epsilon):
    """Dynamic winner ordering: time ties resolve through immutable identity."""
    if candidate[0] < best[0] - epsilon:
        return True
    if abs(candidate[0] - best[0]) > epsilon:
        return False
    return candidate[1:] < best[1:]


def check_dynamic_time_tie_boundaries(epsilon):
    failures = 0

    # A slightly later hit is a time tie, so dynamic provenance wins.
    if not dynamic_precedes_contract((0.5 * epsilon, 1, 1, 1, 1, 1),
                                    (0.0, 2, 1, 1, 1, 1), epsilon):
        print("dynamic within-epsilon hit must tie-break by spawn generation", file=sys.stderr)
        failures += 1
    # A material time difference wins over lower identity fields.
    if dynamic_precedes_contract((2.0 * epsilon, 1, 1, 1, 1, 1),
                                 (0.0, 2, 1, 1, 1, 1), epsilon):
        print("dynamic beyond-epsilon later hit must not win by identity", file=sys.stderr)
        failures += 1
    if not dynamic_precedes_contract((0.0, 9, 9, 9, 9, 9),
                                     (2.0 * epsilon, 1, 1, 1, 1, 1), epsilon):
        print("dynamic beyond-epsilon earlier hit must win over identity", file=sys.stderr)
        failures += 1
    # Equal-time ordering remains spawn, model, source triangle, global ID,
    # then authoritative list index.
    if not dynamic_precedes_contract((0.0, 3, 5, 7, 11, 13),
                                     (0.0, 3, 5, 7, 12, 1), epsilon):
        print("dynamic equal-time provenance ordering regressed", file=sys.stderr)
        failures += 1
    return failures


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    dynamic = (root / "game/src/camera_dynamic_occlusion.c").read_text(encoding="utf-8")
    objects = (root / "game/src/objects.c").read_text(encoding="utf-8")
    snapshots = (root / "platform/presentation_snapshot_walk.c").read_text(
        encoding="utf-8"
    )
    query_header = (root / "platform/camera_obstruction_query.h").read_text(encoding="utf-8")
    failures = 0

    for needle in (
        "#ifdef NATIVE_PORT",
        "objGetObjList(&first, &count)",
        "object->behaviorId == BHV_RACER",
        "OBJ_FLAGS_PARTICLE",
        "INTERACT_FLAGS_SOLID",
        "BHV_DOOR || object->behaviorId == BHV_TT_DOOR",
        "object_spawn_generation",
        "model_generation",
        "mdkr_camera_dynamic_world_aabb",
        "mdkr_camera_dynamic_temporal_bounds",
        "mdkr_camera_dynamic_sphere_aabb_sweep",
        "sPreviousInstances",
        "sPublicationState",
        "recovery_discontinuity",
        "temporal_moved_instance_count",
        "temporal_proxy_hit_count",
        "mdkr_camera_dynamic_swept_aabb_intersects",
        "mdkr_camera_dynamic_rounded_lens_input_valid",
        "mdkr_camera_dynamic_occlusion_rounded_lens_sweep_detailed",
        "mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled",
        "mdkr_camera_object_occlusion_sweep_model_profiled",
        "mdkr_camera_rounded_lens_guard_conservative_radius",
        "exact_max_instances_per_sweep",
        "exact_max_model_triangles_per_sweep",
        "exact_max_single_model_triangles",
        "exact_max_narrowed_triangles_per_sweep",
        "exact_max_stationary_tests_per_sweep",
        "sphere_max_nodes_visited_per_sweep",
        "sphere_max_chunks_retained_per_sweep",
        "sphere_max_chunk_triangles_per_sweep",
        "exact_max_nodes_visited_per_sweep",
        "exact_max_chunk_triangles_per_sweep",
        "local_input.ignored_object_generation = 0U",
        "mdkr_camera_dynamic_hit_precedes",
        "sNextStableInstanceId = UINT32_C(0x80000000)",
        "candidate.hit.stable_id = instance->stable_instance_id",
        "mtxf_from_transform(&visual_transform, &object->trans)",
        "sNextStableInstanceId",
        "source_triangle_stable_id",
        "publication_degraded = 1",
        "mdkr_camera_dynamic_publication_finish(",
        "object_capacity > SIZE_MAX / bytes_per_slot",
        "sTelemetry.allocation_bytes = object_capacity * bytes_per_slot",
    ):
        failures += require(dynamic, needle)

    for needle in (
        "mdkr_camera_dynamic_occlusion_prepare(OBJECT_SLOT_COUNT)",
        "mdkr_camera_dynamic_occlusion_note_spawn(obj);",
        "mdkr_camera_dynamic_occlusion_note_spawn(curObj);",
        "mdkr_camera_dynamic_occlusion_note_free(obj);",
    ):
        failures += require(objects, needle)

    tick = body(dynamic, "void mdkr_camera_dynamic_occlusion_tick(void)", "static int mdkr_camera_dynamic_hit_precedes")
    sweep = body(dynamic, "MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep_detailed(", "MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep(\n")
    rounded_sweep = body(
        dynamic,
        "MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep_detailed(",
        "MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep(\n")
    precedes = body(dynamic, "static int mdkr_camera_dynamic_hit_precedes(",
                     "MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep_detailed(")
    if any(token in tick for token in ("malloc(", "calloc(", "realloc(", "free(")):
        print("dynamic tick must not allocate or free", file=sys.stderr)
        failures += 1
    if any(token in sweep for token in ("malloc(", "calloc(", "realloc(", "free(", "collision_objectmodel", "gCollisionObjects")):
        print("dynamic sweep must be allocation-free and independent of gameplay collision", file=sys.stderr)
        failures += 1
    if "memset(out_hit, 0, sizeof(*out_hit));" not in sweep:
        print("dynamic sweep must canonicalize invalid and clear output", file=sys.stderr)
        failures += 1
    if "object->" in sweep:
        print("published sweep must not read or mutate live Object state", file=sys.stderr)
        failures += 1
    if "mdkr_camera_sweep_object_local(" in sweep:
        print("dynamic sphere sweep must use the bounded immutable model index", file=sys.stderr)
        failures += 1
    for needle in (
        "MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES - nodes_visited",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS - chunks_retained",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES - chunk_triangles",
        "mdkr_camera_object_occlusion_sweep_model_profiled(",
        "instance->temporal_proxy",
        "&instance->temporal_bounds",
    ):
        failures += require(sweep, needle)
    if any(token in rounded_sweep for token in ("malloc(", "calloc(", "realloc(", "free(", "collision_objectmodel", "gCollisionObjects")):
        print("rounded dynamic sweep must be allocation-free and independent of gameplay collision", file=sys.stderr)
        failures += 1
    if "memset(out_hit, 0, sizeof(*out_hit));" not in rounded_sweep:
        print("rounded dynamic sweep must canonicalize invalid and clear output", file=sys.stderr)
        failures += 1
    if "object->" in rounded_sweep:
        print("rounded published sweep must not read or mutate live Object state", file=sys.stderr)
        failures += 1
    for needle in (
        "mdkr_camera_dynamic_rounded_lens_input_valid(input, &outward_radius)",
        "input->ignored_object_generation != 0U",
        "local_input.ignored_object_generation = 0U",
        "outward_radius",
        "mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(",
        "mdkr_camera_dynamic_hit_precedes(&candidate, &best)",
        "instance->temporal_proxy",
        "&instance->temporal_bounds",
    ):
        failures += require(rounded_sweep, needle)
    if "mdkr_camera_sweep_object_local(" in rounded_sweep:
        print("rounded dynamic sweep must not select a sphere narrow-phase winner", file=sys.stderr)
        failures += 1
    if "input->guard.broadphase_radius" in rounded_sweep:
        print("rounded dynamic sweep must use recomputed outward radius for AABB retention", file=sys.stderr)
        failures += 1
    for needle in (
        "MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON",
        "(double)candidate->hit.fraction < (double)best->hit.fraction -",
        "fabs((double)candidate->hit.fraction - (double)best->hit.fraction) >",
        "candidate->object_spawn_generation",
        "candidate->model_generation",
        "candidate->source_triangle_stable_id",
        "candidate->hit.stable_id",
        "candidate->authoritative_list_index",
    ):
        failures += require(precedes, needle)
    epsilon_match = re.search(
        r"^#define MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON ([0-9.eE+-]+)$",
        query_header, re.MULTILINE)
    if epsilon_match is None:
        print("dynamic precedence requires the public query time-tie epsilon", file=sys.stderr)
        failures += 1
    else:
        failures += check_dynamic_time_tie_boundaries(float(epsilon_match.group(1)))
    if "sTelemetry.uncategorized_model_count++" in tick:
        print("explicit non-solid exclusions must not be mislabeled unclassified", file=sys.stderr)
        failures += 1
    failures += require(
        snapshots,
        "mdkr_camera_dynamic_occlusion_object_discontinuous(object)",
    )
    for needle in (
        "mdkr_camera_dynamic_publication_begin(&sPublicationState)",
        "mdkr_camera_dynamic_publication_requires_global_cut(",
        "mdkr_camera_dynamic_publication_current_valid(&sPublicationState)",
        "mdkr_camera_dynamic_publication_previous_valid(&sPublicationState)",
        "return mdkr_camera_dynamic_candidate_kind(object, &is_door)",
        "published_instance->temporal_proxy = FALSE",
    ):
        failures += require(dynamic, needle)
    if objects.index("mdkr_camera_dynamic_occlusion_note_free(obj);") < objects.index("if (obj->trans.flags & OBJ_FLAGS_PARTICLE)", objects.index("void obj_destroy(Object *obj, s32 arg1)")):
        pass
    else:
        print("object retirement must precede particle early return", file=sys.stderr)
        failures += 1
    if failures:
        return 1
    print("camera-dynamic-occlusion: integration invariants passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
