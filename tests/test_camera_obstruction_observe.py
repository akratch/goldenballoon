#!/usr/bin/env python3
"""ROM-free source guard for camera observe/resolve authority separation."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "game" / "src" / "camera_obstruction_runtime.c"
THREAD = ROOT / "game" / "src" / "thread3_main.c"
RACER = ROOT / "game" / "src" / "racer.c"
TRACKS = ROOT / "game" / "src" / "tracks.c"
CAMERA = ROOT / "game" / "src" / "camera.c"


def require(condition: bool, message: str) -> int:
    if not condition:
        print(f"camera-obstruction-observe: {message}", file=sys.stderr)
        return 1
    return 0


def strip_comments(source: str) -> str:
    """C source with comment text removed.

    A needle that also occurs in prose is not an assertion about code, so every
    order-blind source needle here is matched against the stripped form.
    """

    result: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                continue
            result.append(source[index])
        elif state == "block-comment" and pair == "*/":
            state = "code"
            index += 2
            continue
        elif state == "line-comment" and source[index] == "\n":
            state = "code"
            result.append("\n")
        index += 1
    return "".join(result)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"^[ \t]*(?:static\s+)?[A-Za-z_][A-Za-z0-9_]*\s+\*?\s*"
        rf"{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.M | re.S,
    )
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = source.find("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


def main() -> int:
    runtime = RUNTIME.read_text(encoding="utf-8")
    thread = THREAD.read_text(encoding="utf-8")
    racer = RACER.read_text(encoding="utf-8")
    tracks = TRACKS.read_text(encoding="utf-8")
    camera = CAMERA.read_text(encoding="utf-8")
    failures = 0
    tick = function_body(runtime, "camera_obstruction_tick")

    for needle in (
        "MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT 8",
        "camera_obstruction_snapshot_authored_slots();",
        "cam_latch_effective_projection_for_viewport",
        "mdkr_track_occlusion_sweep",
        "camera_obstruction_track_sweep",
        "MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK",
        "logical_camera_unchanged",
        "MDKR_CAMERA_TRACE",
        "camera_obstruction_runtime_reset",
        "camera_obstruction_intent_capture",
        "record->intent = captured;",
        "record->capture_serial != record->consumed_serial",
        "record->capture_tick = sCameraObstructionRuntime.tick_serial;",
        "camera_obstruction_intent_is_current_tick",
        "record->intent.desired_eye.x == observe->desired_eye.x",
        "camera_obstruction_apply_intent(observe, physical_slot);",
        "intent_missing_or_stale",
        "resolved_cameras",
        "camera_obstruction_presentation_begin",
        "camera_obstruction_camera_for_slot",
        "target_hidden=%d target_embedded=%d depenetrate_only=%d ",
        "safety_only=%d emergency=%d ",
        # MOTION-01. The per-family profile table, its measurement seam, and the
        # motion census that reads the published pose must all stay reachable:
        # a census that silently stops being emitted would read as a clean run.
        "MDKR_CAMERA_OBSTRUCTION_TREATMENT_SAFETY_ONLY",
        "camera_obstruction_shipped_family_treatment",
        "MDKR_CAMERA_PROFILE_FORCE",
        "camera_obstruction_motion_sample",
        "camera_motion summary slot_ticks=%llu ",
        "camera_motion stat name=%s unit=%s samples=%llu ",
        # The retraction release band. Wiring it to 0, or dropping the config
        # field, restores release-on-the-first-clear-tick and the measured
        # pop-and-snap with it; both would leave every other needle here happy.
        ".release_hold_ticks = MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS,",
        "#define MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS 9U",
        "status == MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED;",
        "target_status == MDKR_CAMERA_TARGET_VISIBILITY_INVALID",
        "camera_obstruction_publish_elevated_emergency",
        "MDKR_CAMERA_EXACT_SHADOW",
        "camera_obstruction_build_exact_guard",
        "cam_lens_pose_from_camera_snapshot",
        "camera_obstruction_shadow_exact_corridor",
        "mdkr_camera_obstruction_two_phase_rounded_lens_sweep",
        "mdkr_track_occlusion_rounded_lens_sweep",
        "mdkr_camera_dynamic_occlusion_rounded_lens_sweep",
        "return MDKR_CAMERA_SWEEP_INVALID;",
        "observe->query_source_degraded ||",
        "observe->resolved_valid = FALSE;",
        "!sCameraObstructionRuntime.slots[physical_slot].query_source_degraded",
        "MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK",
        "exact_shadow={enabled=%d invoked=%d sphere_clear=%d exact_clear=%d ",
    ):
        failures += require(needle in runtime, f"missing OBSERVE invariant: {needle}")

    # Shipped policy default. Correction is an opt-in launcher setting
    # (Camera.Obstruction), so an unset or empty MDKR_CAMERA_OBSTRUCTION must
    # resolve to OBSERVE -- the authored camera -- every named arm must keep
    # working, and an unrecognised value must fail safe to OBSERVE as well: a
    # typo may neither silently correct nor silently select the known-unsafe
    # LEGACY arm. Scoped to the function body so a stray mention of an arm name
    # elsewhere in the file cannot satisfy it.
    policy = strip_comments(
        function_body(runtime, "camera_obstruction_runtime_policy"))
    failures += require(
        re.search(
            r"value\s*==\s*NULL[^;{]*\{\s*return\s+MDKR_CAMERA_RUNTIME_OBSERVE\s*;",
            policy,
        ) is not None,
        "an unset MDKR_CAMERA_OBSTRUCTION must resolve to OBSERVE",
    )
    failures += require(
        re.search(
            r'value\s*==\s*NULL[^;{]*strcmp\(value,\s*"observe"\)[^;{]*\{\s*'
            r"return\s+MDKR_CAMERA_RUNTIME_OBSERVE\s*;",
            policy,
        ) is not None,
        "the unset arm must be the observe arm, not a second path to it",
    )
    failures += require(
        re.search(
            r"return\s+MDKR_CAMERA_RUNTIME_OBSERVE\s*;\s*\}\s*\Z", policy
        ) is not None,
        "an unrecognised MDKR_CAMERA_OBSTRUCTION must fail safe to OBSERVE",
    )
    # A dropped request is indistinguishable from an honoured one once both
    # resolve to OBSERVE, so the fallback may not be silent -- and it may not
    # repeat, because the policy resolves per slot per fixed tick.
    failures += require(
        "sCameraObstructionPolicyFallbackReported = TRUE;" in policy and
        "falling back to observe" in runtime,
        "the unrecognised-policy fallback must report itself exactly once",
    )
    for value, arm in (("modern", "MODERN"), ("observe", "OBSERVE"),
                       ("center-ray", "CENTER_RAY"), ("legacy", "LEGACY")):
        failures += require(
            re.search(
                rf'strcmp\(value,\s*"{re.escape(value)}"\)\s*==\s*0[^;{{]*\{{\s*'
                rf"return\s+MDKR_CAMERA_RUNTIME_{arm}\s*;",
                policy,
            ) is not None,
            f"MDKR_CAMERA_OBSTRUCTION={value} must still select {arm}",
        )

    failures += require("MDKR_TRACE(" not in runtime,
                        "OBSERVE tracing must not join the general MDKR_TRACE stream")
    failures += require(not re.search(r"gCameras\s*\[[^]]+\]\s*=", runtime),
                        "OBSERVE gate must not assign a gCameras slot")
    failures += require("set_active_camera(" not in runtime,
                        "OBSERVE gate must not change camera selection")
    for forbidden in ("obj_sort_tick(", "obj_lod_tick(", "obj_visibility_tick(",
                      "audspat_update_all(", "camSetProjMtx("):
        failures += require(forbidden not in runtime,
                            f"OBSERVE gate must not call {forbidden}")

    # Correction is published only through the native presentation sidecar.
    render_scene = function_body(tracks, "render_scene")
    failures += require(
        "camera_obstruction_presentation_begin();" in render_scene and
        "camera_obstruction_presentation_end();" in render_scene and
        render_scene.index("camera_obstruction_presentation_begin();") <
        render_scene.index("camera_obstruction_presentation_end();"),
        "render_scene must bracket resolved-camera presentation scope",
    )
    basis = function_body(camera, "cam_build_view_basis")
    failures += require("camera_obstruction_camera_for_slot" in basis,
                        "render basis must use the scoped sidecar accessor")

    # The scoped accessor stands in for &gCameras[slot] at render call sites
    # that never checked an index, and viewport_reset()'s parked camera 4 plus
    # the cutscene bank's +4 reaches one past the last slot. It must therefore
    # clamp rather than hand those sites a NULL to dereference.
    slot_accessor = function_body(runtime, "camera_obstruction_camera_for_slot")
    failures += require(
        "return NULL" not in strip_comments(slot_accessor),
        "camera_obstruction_camera_for_slot must clamp, never return NULL",
    )
    failures += require(
        "MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT - 1" in slot_accessor,
        "camera_obstruction_camera_for_slot must clamp to the last valid slot",
    )
    failures += require(
        len(re.findall(r"camera_obstruction_camera_for_slot\s*\(",
                       strip_comments(camera))) == 6,
        "the six render camera reads must all route through the scoped accessor",
    )
    # The authored camera record and the matrix beside it are built together in
    # camera.c; the walker only replays that record. Both halves are asserted so
    # neither can silently fall back to the unresolved gCameras entry.
    walk = (ROOT / "platform" / "presentation_snapshot_walk.c").read_text(encoding="utf-8")
    record = function_body(camera, "mdkr_snapshot_authored_camera_record")
    failures += require(
        "camera_obstruction_snapshot_camera_for_slot" in record,
        "interpolation snapshots must capture resolved camera poses",
    )
    failures += require(
        "gCameras[" not in strip_comments(function_body(walk, "capture_cameras")),
        "the snapshot walker must not rebuild a camera pose from gCameras",
    )

    # Physical slot mapping: 1P P2 promotion, cutscene-bank offset, and the
    # 3P T.T. exception all need to stay explicit and bounds checked.
    for needle in (
        "viewport_count == 1 && is_player_two_in_control()",
        "normal_slot + (cutscene_bank ? 4 : 0)",
        "viewport_count, PLAYER_FOUR, PLAYER_FOUR, FALSE, TRUE, runtime_policy",
        "physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT",
        "observe->last_solve_tick == sCameraObstructionRuntime.tick_serial",
    ):
        failures += require(needle in runtime, f"missing slot mapping guard: {needle}")

    # The authority test enforces the same order semantically; retain a cheap
    # local witness that both routes actually invoke the finalizer.
    for function in ("mode_game", "update_menu_scene"):
        body = function_body(thread, function)
        tt = body.find("scene_tt_camera_tick(")
        observe = body.find("camera_obstruction_tick(")
        sort = body.find("obj_sort_tick(")
        failures += require(tt >= 0 and observe >= 0 and sort >= 0 and tt < observe < sort,
                            f"{function} must call OBSERVE after TT and before sort")

    failures += require("camera_obstruction_snapshot_authored_slots();" in tick,
                        "tick must snapshot before selected-slot observation")
    failures += require(
        "cam_latch_effective_projection_for_viewport_context" in runtime and
        "!cutscene_bank, runtime_policy" in runtime,
        "resolver must latch the explicit gameplay/cutscene projection context",
    )
    failures += require(
        "camera_obstruction_projection_matches_render" in camera and
        "camera_obstruction_projection_matches_render" in runtime,
        "render projection must match the fixed-tick validated generation",
    )
    # Freshness is the entire content of the intent record: an intent captured
    # on the previous tick is fresh for this one, and the first tick after a
    # reset has no predecessor. Naming the predicate is not enough -- a stub
    # that always answers "fresh" satisfies every name pin in this file -- so
    # scope the claim to the body and forbid a constant answer.
    freshness = strip_comments(
        function_body(runtime, "camera_obstruction_intent_is_current_tick"))
    failures += require(
        "capture_tick + 1U == tick" in freshness and "tick == 1U" in freshness,
        "intent freshness must compare the capture tick against this tick",
    )
    failures += require(
        re.search(r"return\s+[01]U?\s*;", freshness) is None,
        "intent freshness must not answer with a constant",
    )
    # No ROM route produces target_hidden, so the publication rule itself is the
    # control: outside the no-focus branch the flag is the visibility query's
    # answer, never a constant. tests/test_camera_target_visibility.c carries
    # the matching positive control that an occluded focus reports hidden.
    validate = strip_comments(
        function_body(runtime, "camera_obstruction_validate_resolved"))
    visibility_assignments = [
        " ".join(value.split()) for value in
        re.findall(r"observe->resolved_target_visible\s*=\s*([^;]+);", validate)
    ]
    failures += require(
        len(visibility_assignments) == 2 and
        visibility_assignments.count("TRUE") == 1 and
        any("MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE" in value
            for value in visibility_assignments),
        "resolved target visibility must come from the visibility query, with a "
        f"constant only on the no-focus branch: {visibility_assignments}",
    )
    # Reachability, not presence: both resolver seams still satisfy every name
    # pin here when wrapped in a guard that can never be taken. Assert which
    # body each call lives in, and that the fixed-tick file carries no
    # constant-false guard at all.
    resolve_slot_body = strip_comments(
        function_body(runtime, "camera_obstruction_resolve_slot"))
    observe_slot_body = strip_comments(
        function_body(runtime, "camera_obstruction_observe_slot"))
    failures += require(
        "mdkr_camera_obstruction_resolve(" in resolve_slot_body,
        "the resolver kernel must be invoked from camera_obstruction_resolve_slot",
    )
    failures += require(
        "camera_obstruction_resolve_slot(" in observe_slot_body,
        "each observed slot must invoke the resolver",
    )
    failures += require(
        re.search(r"\b(?:if|while)\s*\(\s*(?:0|0U|FALSE)\s*\)",
                  strip_comments(runtime)) is None,
        "the fixed-tick runtime must not gate work behind a constant-false guard",
    )
    failures += require(
        "cam_restore_latched_effective_projection_for_viewport" in camera and
        "cam_restore_latched_effective_projection_for_viewport" in runtime,
        "projection failure must restore the exact last validated render pair",
    )
    for needle in (
        "observe->selected_previous_tick && !observe->intent.discontinuity",
        "observe->last_validated_projection.display_generation ==",
        "observe->last_validated_viewport_layout == cam_get_viewport_layout()",
        "observe->last_validated_authored_fov == cam_get_fov()",
        "fallback_status == MDKR_CAMERA_SWEEP_CLEAR",
        "observe->last_validated_exact_guard_valid",
        "fallback_lens_query.guard = &observe->last_validated_exact_guard;",
        "observe->last_validated_apply_shake == (uint8_t)(gNoCamShake != 0)",
        "observe->resolver.last_safe_valid = FALSE",
    ):
        failures += require(needle in runtime, f"missing fail-closed fallback guard: {needle}")
    for presentation_read in (
        "camera = camera_obstruction_camera_for_slot(index);",
        "cam_get_active_camera()->trans.rotation.z_rotation",
    ):
        failures += require(
            presentation_read in camera,
            f"presentation camera read must use the scoped sidecar: {presentation_read}",
        )
    failures += require(
        '#ifdef NATIVE_PORT\n#include "camera_obstruction_runtime.h"\n#endif' in thread,
        "matching build must not include the native observer",
    )
    failures += require(
        thread.count("#ifdef NATIVE_PORT\n    /* Camera obstruction finalizer") == 2,
        "both finalizer calls must remain native-only",
    )

    # Final author seams must publish their own focus data. Capture after the
    # final authored write is critical: the sidecar is last-author-wins, but the
    # fixed tick consumes each selected physical slot at most once.
    player = function_body(racer, "update_player_camera")
    final_shake = player.rfind("while (gCameraObject->shakeTimer < 0)")
    player_capture = player.rfind("racer_camera_obstruction_capture_intent(obj, racer);")
    failures += require(final_shake >= 0 and player_capture > final_shake,
                        "player intent must be captured after dialogue/shake")
    for needle in (
        "racer->ox1 * 10.0f",
        "racer->oy1 * 10.0f",
        "racer->oz1 * 10.0f",
        "intent.target.y += 20.0f",
        "MDKR_CAMERA_INTENT_FAMILY_LOOP",
        "MDKR_CAMERA_INTENT_FAMILY_FINISH_CHALLENGE",
    ):
        failures += require(needle in racer, f"missing racer intent focus policy: {needle}")

    tt = function_body(tracks, "ttcam_update")
    tt_final_pose = tt.rfind("gTTCamID = spectateIndex;")
    tt_capture = tt.rfind("camera_obstruction_intent_capture(&intent);")
    failures += require(tt_final_pose >= 0 and tt_capture > tt_final_pose,
                        "T.T. intent must follow its final selected pose")
    failures += require("MDKR_CAMERA_INTENT_FAMILY_TT_SPECTATE" in tt,
                        "T.T. intent family missing")
    failures += require("intent.target.y += 20.0f" in tt,
                        "T.T. target must use the racer chassis focus")

    scripted = function_body(camera, "write_to_object_render_stack")
    final_transform = scripted.rfind("gCameras[stackPos].cameraSegmentID =")
    scripted_capture = scripted.rfind("camera_obstruction_intent_capture(&intent);")
    failures += require(final_transform >= 0 and scripted_capture > final_transform,
                        "scripted intent must follow final transform writes")
    for needle in ("MDKR_CAMERA_INTENT_FAMILY_SCRIPTED_CUTSCENE", "intent.forward_valid = TRUE"):
        failures += require(needle in scripted, f"missing scripted intent policy: {needle}")
    if failures:
        return 1
    print("camera-obstruction-observe: integration invariants passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
