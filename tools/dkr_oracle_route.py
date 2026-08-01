#!/usr/bin/env python3
"""Compile mdkr64 visual-oracle route specs for the native port and ares.

A single route JSON (schema "mdkr64.oracle.route.v1") describes a scripted input
sequence plus the frames to capture. This tool emits BOTH runner artifacts:

  * native  --input-script  ("frame TOKEN[+TOKEN] [hold]" lines, edge-detected)
  * ares    injection route ("start len buttonsHex stickX stickY" lines)

The two emulators reach a given menu at DIFFERENT frame numbers (ares boots the
full ROM incl. IPL/logos in real time; the native port has its own present
clock). Each route carries a per-route "sync" point -- the frame at which a
shared visual event (e.g. the title screen) appears in each runner -- and the
ares frames are derived from the authoritative native frames by that offset:

    delta    = sync.ares_frame - sync.native_frame
    ares_abs = native_abs + delta

Event frames are given as absolute NATIVE present-frames (so a route reproduces
the proven tests/input_scripts fixtures verbatim). "marks" are capture points
given as logical offsets from the sync event, resolved per-runner.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

SCHEMA = "mdkr64.oracle.route.v1"
ROUTE_DIR = Path(__file__).resolve().parent / "oracle_routes"

# Standard N64 controller button bits (high 16 bits of the SI state word).
# Identical to the native port's masks (platform/platform_sdl_min.c) and to a
# stock N64 controller, so the value the ares hook returns lands verbatim.
BUTTON_MASKS = {
    "a": 0x8000, "b": 0x4000, "z": 0x2000, "start": 0x1000,
    "up": 0x0800, "down": 0x0400, "left": 0x0200, "right": 0x0100,
    "dup": 0x0800, "ddown": 0x0400, "dleft": 0x0200, "dright": 0x0100,
    "l": 0x0020, "r": 0x0010,
    "cup": 0x0008, "cdown": 0x0004, "cleft": 0x0002, "cright": 0x0001,
}

# Native --input-script token names.
NATIVE_TOKEN = {
    "a": "A", "b": "B", "z": "Z", "start": "START", "l": "L", "r": "R",
    "cup": "CUP", "cdown": "CDOWN", "cleft": "CLEFT", "cright": "CRIGHT",
    "up": "UP", "down": "DOWN", "left": "LEFT", "right": "RIGHT",
    "dup": "UP", "ddown": "DOWN", "dleft": "LEFT", "dright": "RIGHT",
}

# Directional buttons also drive the analog stick (DKR menus read the stick).
# Matches the native token->stick behavior in platform_sdl_min.c.
STICK_DELTA = {
    "up": (0, 80), "down": (0, -80), "left": (-80, 0), "right": (80, 0),
    "dup": (0, 80), "ddown": (0, -80), "dleft": (-80, 0), "dright": (80, 0),
}

DEFAULT_HOLD = 4
ARM_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def is_integer(value: Any) -> bool:
    """JSON integer, excluding bool (which subclasses int in Python)."""
    return isinstance(value, int) and not isinstance(value, bool)


def route_path(name_or_path: str) -> Path:
    candidate = Path(name_or_path)
    if candidate.exists():
        return candidate.resolve()
    if candidate.suffix != ".json":
        candidate = ROUTE_DIR / f"{name_or_path}.json"
    else:
        candidate = ROUTE_DIR / candidate.name
    if candidate.exists():
        return candidate.resolve()
    raise SystemExit(f"FAIL: route not found: {name_or_path}")


def load_route(name_or_path: str) -> dict[str, Any]:
    path = route_path(name_or_path)
    with path.open("r", encoding="utf-8") as handle:
        route = json.load(handle)
    if not isinstance(route, dict):
        raise SystemExit(f"FAIL: route root must be an object: {path}")
    if route.get("schema") != SCHEMA:
        raise SystemExit(f"FAIL: unsupported schema in {path}: {route.get('schema')!r}")
    route["_path"] = str(path)
    return route


def norm_button(button: str) -> str:
    key = str(button).strip().lower().replace("-", "_").replace("_", "")
    # normalize c_up -> cup, dpad_up -> dup etc.
    key = key.replace("dpad", "d").replace("cbtn", "c")
    if key in ("cu",): key = "cup"
    if key not in BUTTON_MASKS:
        raise SystemExit(f"FAIL: unknown route button {button!r}")
    return key


def event_buttons(event: dict[str, Any]) -> list[str]:
    buttons = event.get("buttons", [])
    if isinstance(buttons, str):
        buttons = [buttons]
    if not isinstance(buttons, list):
        raise SystemExit(f"FAIL: event buttons must be a string or list: {event!r}")
    return [norm_button(b) for b in buttons]


def sync_delta(route: dict[str, Any]) -> int:
    sync = route.get("sync", {})
    if not isinstance(sync, dict):
        raise SystemExit("FAIL: route sync must be an object")
    native = sync.get("native_frame")
    ares = sync.get("ares_frame")
    if not isinstance(native, int) or not isinstance(ares, int):
        raise SystemExit("FAIL: route sync must set integer native_frame and ares_frame")
    return ares - native


def runner_block(route: dict[str, Any], runner: str) -> dict[str, Any]:
    block = route.get(runner, {})
    if not isinstance(block, dict):
        raise SystemExit(f"FAIL: route {runner} block must be an object")
    return block


def clamp_axis(v: int) -> int:
    return max(-80, min(80, v))


def event_ares_extra(route: dict[str, Any], event: dict[str, Any]) -> int:
    """Return explicit event offset plus any measured route-phase offset."""
    native_frame = int(event["frame"])
    extra = int(event.get("ares_extra", 0))
    for phase in route.get("ares_phase_offsets", []):
        if native_frame >= int(phase["from_native_frame"]):
            extra += int(phase["extra"])
    return extra


def native_arms(route: dict[str, Any]) -> list[dict[str, Any]]:
    arms = route.get("native_arms", [])
    return arms if isinstance(arms, list) else []


def native_arm(route: dict[str, Any], name: str) -> dict[str, Any]:
    for arm in native_arms(route):
        if isinstance(arm, dict) and arm.get("name") == name:
            return arm
    raise SystemExit(f"FAIL: route has no native arm {name!r}")


def emit_native_script(route: dict[str, Any], arm_name: str | None = None) -> None:
    print(f"# mdkr64 oracle native input-script for route: {route.get('name', '?')}")
    print("# frame TOKEN[+TOKEN...] hold")
    arm = native_arm(route, arm_name) if arm_name is not None else {}
    divisor = int(arm.get("event_divisor", route.get("native_event_divisor", 1)))
    for event in route.get("events", []):
        frame = (int(event["frame"]) + divisor // 2) // divisor
        hold = max(1, (int(event.get("hold", DEFAULT_HOLD)) + divisor - 1) // divisor)
        tokens = [NATIVE_TOKEN[b] for b in event_buttons(event)]
        if not tokens:
            continue
        print(f"{frame} {'+'.join(tokens)} {hold}")


def emit_ares_script(route: dict[str, Any]) -> None:
    """
    ares event frames are native + sync delta + an optional per-event `ares_extra`.

    Why `ares_extra` is needed: a single global delta CANNOT keep the two runners
    on the same screen for a multi-tap route. Each menu transition takes longer on
    real hardware than in this port -- a level load there is a genuine multi-frame
    stall (DMA + decompression) that ares presents frames through, while this port
    loads from host memory almost instantly (measured on title_to_options: our
    black transition ~20 frames, the real ROM's ~40). The error ACCUMULATES per
    tap, so by the third or fourth press the runners are on different screens
    entirely and the comparison scores a render gap that does not exist -- e.g.
    our port on CAUTION while the real ROM was still on PLAYER SELECT.

    `ares_extra` is cumulative in effect (each event carries the total extra delay
    owed by all preceding transitions), and is calibrated per route by observing
    which screen ares actually reaches. Keep it in the route so the calibration is
    reviewable data, not a hidden constant.
    """
    delta = sync_delta(route)
    print("# mdkr64 oracle ares input-script (start len buttonsHex stickX stickY)")
    for event in route.get("events", []):
        native_frame = int(event["frame"])
        ares_frame = native_frame + delta + event_ares_extra(route, event)
        if ares_frame < 1:
            continue
        hold = int(event.get("hold", DEFAULT_HOLD))
        mask = 0
        sx = sy = 0
        for b in event_buttons(event):
            mask |= BUTTON_MASKS[b]
            dx, dy = STICK_DELTA.get(b, (0, 0))
            sx += dx
            sy += dy
        # explicit stick overrides
        if "stick_x" in event:
            sx = int(event["stick_x"])
        if "stick_y" in event:
            sy = int(event["stick_y"])
        sx, sy = clamp_axis(sx), clamp_axis(sy)
        print(f"{ares_frame} {hold} 0x{mask:04x} {sx} {sy}")


def resolved_marks(route: dict[str, Any]) -> list[dict[str, int]]:
    """Return marks as {name, native_frame, ares_frame}."""
    sync = route.get("sync", {})
    native_sync = int(sync.get("native_frame", 0))
    ares_sync = int(sync.get("ares_frame", 0))
    out = []
    for mark in route.get("marks", []):
        if not isinstance(mark, dict):
            raise SystemExit(f"FAIL: mark must be an object: {mark!r}")
        logical = int(mark.get("logical", 0))
        # `ares_extra` shifts only the ares side of this mark, for the same
        # transition-length reason as events (see emit_ares_script).
        out.append({
            "name": str(mark.get("name", f"m{logical}")),
            "native_frame": native_sync + logical,
            "ares_frame": ares_sync + logical + int(mark.get("ares_extra", 0)),
        })
    return out


def emit_mark_pairs(route: dict[str, Any]) -> None:
    for mark in resolved_marks(route):
        print(f"{mark['name']} {mark['native_frame']} {mark['ares_frame']}")


def emit_marks(route: dict[str, Any], runner: str) -> None:
    key = f"{runner}_frame"
    frames = sorted({m[key] for m in resolved_marks(route) if m[key] >= 0})
    print(",".join(str(f) for f in frames))


def route_field(route: dict[str, Any], field: str) -> Any:
    native = runner_block(route, "native")
    ares = runner_block(route, "ares")
    sync = route.get("sync", {})
    table = {
        "name": route.get("name", ""),
        "description": route.get("description", ""),
        "compare_frames": int(bool(route.get("compare_frames", True))),
        "state_trace": int(bool(route.get("state_trace", False))),
        "state_require_finish": int(bool(route.get("state_require_finish", False))),
        "forced_track": int(route.get("forced_track", -1)),
        "forced_track_source": int(route.get("forced_track_source", 5)),
        "state_racer_index": int(route.get("state_racer_index", 0)),
        "state_min_common_clocks": int(route.get("state_min_common_clocks", 1000)),
        "state_min_lap": int(route.get("state_min_lap", 1)),
        "state_min_checkpoint": int(route.get("state_min_checkpoint", 20)),
        "state_max_position_p95": float(route.get("state_max_position_p95", 5.0)),
        "state_min_progress_agreement": float(route.get("state_min_progress_agreement", 0.98)),
        "native_cadence": route.get("native_cadence", ""),
        "native_synth_fields": int(route.get("native_synth_fields", 1)),
        "native_event_divisor": int(route.get("native_event_divisor", 1)),
        "native_frames": native.get("frames", 0),
        "native_dump_every": native.get("dump_every", 0),
        "native_dump_start": native.get("dump_start", 0),
        "ares_frames": ares.get("frames", 0),
        "ares_dump_every": ares.get("dump_every", 0),
        "ares_dump_start": ares.get("dump_start", 0),
        "sync_native_frame": sync.get("native_frame", 0),
        "sync_ares_frame": sync.get("ares_frame", 0),
        "sync_event": sync.get("event", ""),
    }
    if field not in table:
        raise SystemExit(f"FAIL: unknown field {field}")
    return table[field]


def arm_field(route: dict[str, Any], name: str, field: str) -> Any:
    arm = native_arm(route, name)
    replay = arm.get("reference_replay", {})
    table = {
        "frames": arm.get("frames"),
        "cadence": arm.get("cadence"),
        "synth_fields": arm.get("synth_fields"),
        "event_divisor": arm.get("event_divisor"),
        "reference_replay": int(isinstance(replay, dict) and bool(replay)),
        "replay_ares_frame_start": (
            replay.get("ares_frame_start") if isinstance(replay, dict) else None
        ),
        "replay_field_start": (
            replay.get("native_field_start") if isinstance(replay, dict) else None
        ),
        "replay_input_start": (
            replay.get("native_input_start") if isinstance(replay, dict) else None
        ),
    }
    if field not in table:
        raise SystemExit(f"FAIL: unknown native-arm field {field}")
    return table[field]


def validate_route(route: dict[str, Any]) -> None:
    errors: list[str] = []
    if not route.get("name"):
        errors.append("route must set name")
    for runner in ("native", "ares"):
        block = runner_block(route, runner)
        frames = block.get("frames")
        if not is_integer(frames) or frames < 1:
            errors.append(f"{runner}.frames must be a positive integer")
    for field in ("compare_frames", "state_trace", "state_require_finish"):
        if field in route and not isinstance(route[field], bool):
            errors.append(f"{field} must be a boolean")
    if (
        "native_synth_fields" not in route
        or not is_integer(route["native_synth_fields"])
        or route["native_synth_fields"] < 1
    ):
        errors.append("native_synth_fields must be a positive integer")
    if "native_event_divisor" in route and (
        not is_integer(route["native_event_divisor"])
        or route["native_event_divisor"] < 1
    ):
        errors.append("native_event_divisor must be a positive integer")
    if route.get("native_cadence") not in ("original", "enhanced"):
        errors.append("native_cadence must be explicit: original or enhanced")
    for field in (
        "forced_track", "forced_track_source", "state_racer_index",
        "state_min_common_clocks", "state_min_lap", "state_min_checkpoint",
    ):
        if field in route and not is_integer(route[field]):
            errors.append(f"{field} must be an integer")
    forced_track = route.get("forced_track", -1)
    forced_track_source = route.get("forced_track_source", 5)
    racer_index = route.get("state_racer_index", 0)
    common_clocks = route.get("state_min_common_clocks", 1000)
    min_lap = route.get("state_min_lap", 1)
    min_checkpoint = route.get("state_min_checkpoint", 20)
    if is_integer(forced_track) and forced_track < -1:
        errors.append("forced_track must be -1 or a non-negative level id")
    if is_integer(forced_track_source) and forced_track_source < 0:
        errors.append("forced_track_source must be a non-negative level id")
    if is_integer(racer_index) and racer_index < 0:
        errors.append("state_racer_index must be non-negative")
    if is_integer(common_clocks) and common_clocks < 1:
        errors.append("state_min_common_clocks must be positive")
    if (
        (is_integer(min_lap) and min_lap < 0)
        or (is_integer(min_checkpoint) and min_checkpoint < 0)
    ):
        errors.append("state_min_lap and state_min_checkpoint must be non-negative")
    for field in ("state_max_position_p95", "state_min_progress_agreement"):
        if field in route and (
            isinstance(route[field], bool)
            or not isinstance(route[field], (int, float))
            or not math.isfinite(float(route[field]))
        ):
            errors.append(f"{field} must be numeric")
    max_position = route.get("state_max_position_p95", 5.0)
    min_agreement = route.get("state_min_progress_agreement", 0.98)
    if (
        isinstance(max_position, (int, float))
        and not isinstance(max_position, bool)
        and math.isfinite(float(max_position))
        and max_position < 0
    ):
        errors.append("state_max_position_p95 must be non-negative")
    if (
        isinstance(min_agreement, (int, float))
        and not isinstance(min_agreement, bool)
        and math.isfinite(float(min_agreement))
        and not 0.0 <= min_agreement <= 1.0
    ):
        errors.append("state_min_progress_agreement must be in 0..1")
    if is_integer(route.get("native_synth_fields")) and not (
        1 <= route["native_synth_fields"] <= 6
    ):
        errors.append("native_synth_fields must be in the original 1..6 range")
    if (
        route.get("native_cadence") == "original"
        and is_integer(route.get("native_synth_fields"))
        and route["native_synth_fields"] < 2
    ):
        errors.append("original cadence cannot request fewer than two fields")
    raw_arms = route.get("native_arms", [])
    if not isinstance(raw_arms, list):
        errors.append("native_arms must be a list")
        raw_arms = []
    seen_arm_names: set[str] = set()
    for index, arm in enumerate(raw_arms):
        if not isinstance(arm, dict):
            errors.append(f"native_arms[{index}] must be an object")
            continue
        name = arm.get("name")
        if not isinstance(name, str) or ARM_NAME_RE.fullmatch(name) is None:
            errors.append(
                f"native_arms[{index}].name must match {ARM_NAME_RE.pattern}"
            )
        elif name in seen_arm_names:
            errors.append(f"duplicate native arm {name!r}")
        else:
            seen_arm_names.add(name)
        if not is_integer(arm.get("frames")) or arm["frames"] < 1:
            errors.append(f"native_arms[{index}].frames must be positive")
        if arm.get("cadence") not in ("original", "enhanced"):
            errors.append(
                f"native_arms[{index}].cadence must be original or enhanced"
            )
        if not is_integer(arm.get("synth_fields")) or not (
            1 <= arm["synth_fields"] <= 6
        ):
            errors.append(f"native_arms[{index}].synth_fields must be in 1..6")
        if not is_integer(arm.get("event_divisor")) or arm["event_divisor"] < 1:
            errors.append(f"native_arms[{index}].event_divisor must be positive")
        if (
            arm.get("cadence") == "original"
            and is_integer(arm.get("synth_fields"))
            and arm["synth_fields"] < 2
        ):
            errors.append(
                f"native_arms[{index}] original cadence requires at least two fields"
            )
        replay = arm.get("reference_replay")
        if replay is not None:
            if not isinstance(replay, dict):
                errors.append(
                    f"native_arms[{index}].reference_replay must be an object"
                )
            else:
                for key in (
                    "ares_frame_start", "native_field_start", "native_input_start"
                ):
                    if not is_integer(replay.get(key)) or replay[key] < 0:
                        errors.append(
                            f"native_arms[{index}].reference_replay.{key} "
                            "must be a non-negative integer"
                        )
                if (
                    is_integer(replay.get("native_field_start"))
                    and is_integer(replay.get("native_input_start"))
                    and replay["native_input_start"] < replay["native_field_start"]
                ):
                    errors.append(
                        f"native_arms[{index}].reference_replay native_input_start "
                        "must not precede native_field_start"
                    )
                if not isinstance(replay.get("basis"), str) or not replay["basis"].strip():
                    errors.append(
                        f"native_arms[{index}].reference_replay.basis must explain "
                        "the measured handoff"
                    )
    if raw_arms and not route.get("state_trace", False):
        errors.append("native_arms are only supported by state-trace routes")
    if route.get("state_require_finish", False) and not route.get(
        "state_trace", False
    ):
        errors.append("state_require_finish requires state_trace")
    if not route.get("compare_frames", True) and not route.get("state_trace", False):
        errors.append("route must enable compare_frames, state_trace, or both")
    sync = route.get("sync", {})
    if not isinstance(sync, dict) or not is_integer(sync.get("native_frame")) \
            or not is_integer(sync.get("ares_frame")):
        errors.append("sync must set integer native_frame and ares_frame")
    for i, event in enumerate(route.get("events", [])):
        if not isinstance(event, dict):
            errors.append(f"events[{i}] must be an object")
            continue
        if not is_integer(event.get("frame")) or event["frame"] < 0:
            errors.append(f"events[{i}].frame must be a non-negative integer")
        try:
            event_buttons(event)
        except SystemExit as exc:
            errors.append(str(exc))
    phases = route.get("ares_phase_offsets", [])
    if not isinstance(phases, list):
        errors.append("ares_phase_offsets must be a list")
        phases = []
    previous_phase = -1
    for i, phase in enumerate(phases):
        if not isinstance(phase, dict):
            errors.append(f"ares_phase_offsets[{i}] must be an object")
            continue
        if not is_integer(phase.get("from_native_frame")):
            errors.append(
                f"ares_phase_offsets[{i}].from_native_frame must be an integer"
            )
        elif phase["from_native_frame"] <= previous_phase:
            errors.append("ares_phase_offsets must have increasing start frames")
        else:
            previous_phase = phase["from_native_frame"]
        if not is_integer(phase.get("extra")):
            errors.append(f"ares_phase_offsets[{i}].extra must be an integer")
        if not isinstance(phase.get("basis"), str) or not phase["basis"].strip():
            errors.append(f"ares_phase_offsets[{i}].basis must explain the measurement")
    for i, mark in enumerate(route.get("marks", [])):
        if not isinstance(mark, dict) or not is_integer(mark.get("logical")):
            errors.append(f"marks[{i}] must be an object with integer logical")
    if errors:
        print(f"FAIL: invalid route {route.get('name', route.get('_path'))}")
        for e in errors:
            print(f"  {e}")
        raise SystemExit(2)


def list_routes() -> None:
    for path in sorted(ROUTE_DIR.glob("*.json")):
        try:
            route = load_route(str(path))
        except SystemExit:
            continue
        sync = route.get("sync", {})
        print(f"{path.stem}\tnative_frames={route.get('native', {}).get('frames')}"
              f"\tares_frames={route.get('ares', {}).get('frames')}"
              f"\tsync=native:{sync.get('native_frame')}/ares:{sync.get('ares_frame')}"
              f"\tevents={len(route.get('events', []))}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for cmd in ("resolve", "ares-script", "mark-pairs",
                "summary", "validate"):
        p = sub.add_parser(cmd)
        p.add_argument("route")
    p = sub.add_parser("native-script")
    p.add_argument("route")
    p.add_argument("--arm")
    p = sub.add_parser("field"); p.add_argument("route"); p.add_argument("field")
    p = sub.add_parser("arm-field")
    p.add_argument("route")
    p.add_argument("arm")
    p.add_argument("field")
    p = sub.add_parser("native-arms")
    p.add_argument("route")
    p = sub.add_parser("native-marks"); p.add_argument("route")
    p = sub.add_parser("ares-marks"); p.add_argument("route")
    sub.add_parser("list")

    args = parser.parse_args()
    if args.command == "list":
        list_routes()
        return 0

    route = load_route(args.route)
    if args.command == "resolve":
        print(route["_path"])
    elif args.command == "native-script":
        emit_native_script(route, args.arm)
    elif args.command == "ares-script":
        emit_ares_script(route)
    elif args.command == "mark-pairs":
        emit_mark_pairs(route)
    elif args.command == "native-marks":
        emit_marks(route, "native")
    elif args.command == "ares-marks":
        emit_marks(route, "ares")
    elif args.command == "field":
        print(route_field(route, args.field))
    elif args.command == "arm-field":
        print(arm_field(route, args.arm, args.field))
    elif args.command == "native-arms":
        for arm in native_arms(route):
            print(arm["name"])
    elif args.command == "summary":
        print(json.dumps({k: v for k, v in route.items() if not k.startswith("_")}, indent=2))
    elif args.command == "validate":
        validate_route(route)
        print(f"PASS: route validated: {route.get('name', route['_path'])}")
    else:
        raise AssertionError(args.command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
