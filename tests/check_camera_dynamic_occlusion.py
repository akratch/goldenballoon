#!/usr/bin/env python3
"""ROM-free structural/lifecycle contract for dynamic camera occluders."""

from pathlib import Path
import re
import sys


def code_only(source: str) -> str:
    """C source with comment and string-literal text blanked to spaces.

    Every needle in this file is an assertion about what the code does. A raw
    substring search also matches prose, so a call site could be deleted and
    still "pass" on the comment that describes it. Offsets are preserved so the
    ordering checks below stay meaningful.
    """

    result: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                result.append("  ")
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                result.append("  ")
                continue
            if char in ('"', "'"):
                state = "string" if char == '"' else "character"
            result.append(char)
        elif state == "block-comment":
            if pair == "*/":
                state = "code"
                index += 2
                result.append("  ")
                continue
            result.append("\n" if char == "\n" else " ")
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            result.append("\n" if char == "\n" else " ")
        else:
            if char == "\\":
                result.append("  ")
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
                result.append(char)
            else:
                result.append(" ")
        index += 1
    return "".join(result)


def calls_named(source: str, name: str) -> list[int]:
    """Offsets of real call expressions to `name` in comment-stripped code."""

    pattern = re.compile(rf"\b{re.escape(name)}\s*\(")
    return [match.start() for match in pattern.finditer(code_only(source))]


def require_call(source: str, name: str, expected: int) -> int:
    found = len(calls_named(source, name))
    if found != expected:
        print(
            f"dynamic-occlusion call-site census: {name} appears {found} time(s), "
            f"expected {expected}",
            file=sys.stderr,
        )
        return 1
    return 0


def require(source: str, needle: str) -> int:
    if needle not in source:
        print(f"missing dynamic-occlusion invariant: {needle}", file=sys.stderr)
        return 1
    return 0


def body(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    dynamic = code_only(
        (root / "game/src/camera_dynamic_occlusion.c").read_text(encoding="utf-8"))
    objects = code_only(
        (root / "game/src/objects.c").read_text(encoding="utf-8"))
    snapshots = code_only(
        (root / "platform/presentation_snapshot_walk.c").read_text(encoding="utf-8"))
    main_pc = code_only((root / "platform/main_pc.c").read_text(encoding="utf-8"))
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
    # Census the lifecycle seam as calls, not text: one prepare, two spawn
    # notifications, one retirement.
    failures += require_call(objects, "mdkr_camera_dynamic_occlusion_prepare", 1)
    failures += require_call(objects, "mdkr_camera_dynamic_occlusion_note_spawn", 2)
    failures += require_call(objects, "mdkr_camera_dynamic_occlusion_note_free", 1)

    # A side-table allocation failure disables dynamic occluders; it does not
    # end the process. Every consumer of an unprepared census fails closed, so
    # aborting here would turn a recoverable condition into a crash on the
    # default path.
    prepare_failure = body(
        objects,
        "if (!mdkr_camera_dynamic_occlusion_prepare(OBJECT_SLOT_COUNT))",
        "\n    }\n")
    if calls_named(prepare_failure, "abort"):
        print("dynamic-occlusion preparation failure must degrade, not abort",
              file=sys.stderr)
        failures += 1
    if not calls_named(prepare_failure,
                       "mdkr_camera_dynamic_occlusion_shutdown"):
        print("a failed preparation must leave the subsystem explicitly disabled",
              file=sys.stderr)
        failures += 1

    # The census owns host allocations for the life of the process; teardown
    # must release them.
    failures += require_call(
        main_pc, "mdkr_camera_dynamic_occlusion_shutdown", 1)

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
    # Winner selection is the whole point of the precedence contract, so both
    # sweeps are censused as call expressions rather than as text. The sphere
    # sweep selects at three seams (temporal proxy, indexed narrow phase, and
    # the per-instance winner); the rounded-lens sweep selects at two.
    failures += require_call(sweep, "mdkr_camera_dynamic_hit_precedes", 3)
    failures += require_call(rounded_sweep, "mdkr_camera_dynamic_hit_precedes", 2)
    for needle in (
        "MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES - nodes_visited",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS - chunks_retained",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES - chunk_triangles",
        "mdkr_camera_object_occlusion_sweep_model_profiled(",
        "instance->temporal_proxy &&",
        "MDKR_CAMERA_DYNAMIC_OCCLUSION_QUERY_CURRENT_POSE",
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
    # The ordering itself is pinned against the production comparator by the
    # camera_dynamic_precedence CTest target. Asserting it here against a Python
    # copy of the same rule would only prove the two copies agree, so this gate
    # keeps the structural half and requires that target to exist.
    precedence_test = root / "tests" / "test_camera_dynamic_precedence.c"
    if not precedence_test.is_file():
        print("dynamic precedence ordering needs its production-driven C test",
              file=sys.stderr)
        failures += 1
    # The registration lives in cmake/tests.cmake (included from CMakeLists.txt)
    # alongside the rest of the ROM-free unit tests, not in CMakeLists.txt itself.
    cmake = ((root / "CMakeLists.txt").read_text(encoding="utf-8") +
              (root / "cmake" / "tests.cmake").read_text(encoding="utf-8"))
    if "add_test(NAME camera_dynamic_precedence" not in cmake:
        print("the dynamic precedence C test must be registered with ctest",
              file=sys.stderr)
        failures += 1
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
