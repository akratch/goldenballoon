#!/usr/bin/env python3
"""Structural/lifecycle guardrails for CAM-05 ObjectModel visual caches.

ObjectModel is ROM-normalized and pointer-token based, so a standalone fixture
cannot construct a faithful loaded model safely. This source-level gate protects
the integration properties that must hold before ROM routes exercise the cache.
"""

from pathlib import Path
import sys


def require(source: str, needle: str) -> int:
    if needle not in source:
        print(f"missing object-cache invariant: {needle}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    cache = (root / "game/src/camera_object_occlusion.c").read_text(encoding="utf-8")
    model = (root / "game/src/object_models.c").read_text(encoding="utf-8")
    failures = 0

    for needle in (
        "#ifdef NATIVE_PORT",
        "typedef struct MdkrCameraObjectOcclusionCache",
        "typedef struct MdkrCameraObjectOcclusionChunk",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES",
        "MDKR_CAMERA_OBJECT_OCCLUSION_MAX_STATIONARY_TESTS",
        "mdkr_camera_rounded_lens_sweep_object_local_profiled_limited",
        "mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled",
        "mdkr_camera_object_occlusion_sweep_model_profiled",
        "mdkr_camera_object_occlusion_validate_node",
        "mdkr_camera_object_occlusion_cache_layout_valid",
        "mdkr_camera_object_occlusion_node_integrity",
        "mdkr_camera_object_occlusion_chunk_integrity",
        "node->left == node->right",
        "node->leaf != 0 && node->leaf != 1",
        "sCameraObjectOcclusionNextGeneration",
        "sCameraObjectOcclusionNextStableId",
        "stable_id_base + source_ordinal - 1U",
        "mdkr_camera_object_occlusion_batch_nonblocking",
        "RENDER_HIDDEN | RENDER_WATER | RENDER_DECAL | RENDER_SEMI_TRANSPARENT",
        "RENDER_NO_COLLISION",
        "RENDER_CUTOUT | RENDER_VTX_ALPHA",
        "mdkr_camera_object_occlusion_triangle_degenerate",
        "reject the face",
        "mdkr_camera_sweep_object_local",
        "ignored_object_generation",
        "mdkr_camera_object_occlusion_model_pre_free",
    ):
        failures += require(cache, needle)

    if "gCollisionObject" in cache or "collision_objectmodel" in cache:
        print("object visual cache must not use gameplay interaction collision", file=sys.stderr)
        failures += 1

    loaded = model.index("mdkr_camera_object_occlusion_model_loaded(objMdl);")
    finalized = model.rindex("model_init_normals(objMdl)", 0, loaded)
    if loaded <= finalized:
        print("cache must build after object model finalization", file=sys.stderr)
        failures += 1

    pre_free = model.index("mdkr_camera_object_occlusion_model_pre_free(mdl);")
    heap_free = model.index("mempool_free(mdl);", pre_free)
    if pre_free >= heap_free:
        print("cache must be removed before ObjectModel heap free", file=sys.stderr)
        failures += 1

    query_start = cache.index("MdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model(")
    query_end = cache.index("\nvoid mdkr_camera_object_occlusion_get_telemetry", query_start)
    query = cache[query_start:query_end]
    if "malloc" in query or "calloc" in query or "free(" in query:
        print("object cache query must not allocate", file=sys.stderr)
        failures += 1

    exact_start = cache.index(
        "MdkrCameraSweepStatus mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(")
    exact_end = cache.index(
        "\nMdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model(", exact_start)
    exact_query = cache[exact_start:exact_end]
    if any(token in exact_query for token in ("malloc", "calloc", "realloc", "free(")):
        print("exact object cache query must not allocate", file=sys.stderr)
        failures += 1

    if failures:
        return 1
    print("camera-object-occlusion-cache: integration invariants passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
