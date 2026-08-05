#!/usr/bin/env python3
"""Cheap structural guardrails for the ROM-backed CAM-03 cache.

The level-model structs require a loaded, user-supplied ROM, so the exhaustive
geometry tests remain in the ROM-free camera_obstruction kernel suite.  This
test protects the integration invariants that a unit fixture cannot represent:
the cache is native-only, is built after finalization, owns no gameplay
collision scratch, and is freed before the level-model heap disappears.
"""

from pathlib import Path
import sys


def require(source: str, needle: str) -> int:
    if needle not in source:
        print(f"missing cache invariant: {needle}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    source = (root / "game/src/tracks.c").read_text(encoding="utf-8")
    header = (root / "game/src/tracks.h").read_text(encoding="utf-8")
    failures = 0
    cache_start = source.index("CAM-03 static visual-occlusion cache")
    cache_end = source.index("\n#endif\n\n/**\n * Sets the number of expected viewports", cache_start)
    cache_region = source[cache_start:cache_end]
    for needle in (
        "typedef struct MdkrTrackOcclusionCache",
        "static void mdkr_track_occlusion_cache_build(void)",
        "static void mdkr_track_occlusion_cache_free(void)",
        "MdkrCameraSweepStatus mdkr_track_occlusion_sweep(",
        "MdkrCameraSweepStatus mdkr_track_occlusion_rounded_lens_sweep(",
        "MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK",
        "RENDER_HIDDEN | RENDER_WATER | RENDER_DECAL | RENDER_SEMI_TRANSPARENT",
        "RENDER_NO_COLLISION",
        "RENDER_CUTOUT | RENDER_VTX_ALPHA",
        "mdkr_track_occlusion_triangle_degenerate",
        "MdkrTrackOcclusionChunk",
        "mdkr_track_occlusion_bounds_overlap_sweep",
        "mdkr_camera_rounded_lens_guard_conservative_radius",
        "MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON",
        "exact_max_triangle_candidates_per_sweep",
        "exact_max_stationary_tests_per_sweep",
        "vertex_count > UINT32_MAX",
    ):
        failures += require(cache_region, needle)

    if "MdkrCameraSweepStatus mdkr_track_occlusion_rounded_lens_sweep(" not in header:
        print("missing rounded-lens track source declaration", file=sys.stderr)
        failures += 1

    exact_start = cache_region.index("MdkrCameraSweepStatus mdkr_track_occlusion_rounded_lens_sweep(")
    exact_end = cache_region.index("\nvoid mdkr_track_occlusion_get_telemetry", exact_start)
    exact_source = cache_region[exact_start:exact_end]
    for needle in (
        "mdkr_camera_rounded_lens_guard_conservative_radius(",
        "mdkr_track_occlusion_bounds_overlap_sweep(",
        "local_world.indices += chunk->triangle_offset * 3U;",
        "local_world.triangles += chunk->triangle_offset;",
        "local_world.triangle_count = chunk->triangle_count;",
        "mdkr_camera_rounded_lens_sweep_profiled(",
        "mdkr_track_occlusion_record_exact_work",
        "mdkr_track_occlusion_rounded_candidate_better",
    ):
        failures += require(exact_source, needle)
    if "mdkr_camera_sweep(" in exact_source:
        print("rounded track source must retain exact candidates, not sphere-narrow them", file=sys.stderr)
        failures += 1
    if "break;" in exact_source or "return MDKR_CAMERA_SWEEP_CLEAR;" in exact_source:
        print("rounded track source must let later retained chunks defeat an earlier clear", file=sys.stderr)
        failures += 1
    if "input->guard.broadphase_radius" in exact_source:
        print("rounded track source must recompute, not trust, its stored broadphase radius", file=sys.stderr)
        failures += 1
    if "return status;" not in exact_source or "status == MDKR_CAMERA_SWEEP_INVALID" not in exact_source:
        print("rounded track source must fail closed on an invalid retained chunk", file=sys.stderr)
        failures += 1

    # Both offsets of an ordering claim are anchored independently inside the
    # same function region; bounding one search by the other offset can only
    # reproduce the ordering it is supposed to prove.
    generate = source.find("void generate_track(s32 modelId)")
    generate_end = source.find("\nvoid free_track(void)", generate + 1)
    build = source.find("mdkr_track_occlusion_cache_build();", generate, generate_end)
    finalized_vertices = source.rfind(".flags |= 0x08000000;", generate, generate_end)
    if min(generate, generate_end, build, finalized_vertices) < 0:
        print("track generation ordering anchors are missing", file=sys.stderr)
        failures += 1
    elif build <= finalized_vertices:
        print("cache build must follow vertex/batch finalization", file=sys.stderr)
        failures += 1

    free_track = source.index("void free_track(void)")
    cache_free = source.index("mdkr_track_occlusion_cache_free();", free_track)
    heap_free = source.index("mempool_free(gTrackModelHeap);", free_track)
    if cache_free >= heap_free:
        print("cache must free before track heap teardown", file=sys.stderr)
        failures += 1

    if "gCollisionCandidates" in cache_region or "gNumCollisionCandidates" in cache_region:
        print("cache must not use gameplay collision globals", file=sys.stderr)
        failures += 1

    if failures:
        return 1
    print("camera-track-occlusion-cache: integration invariants passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
