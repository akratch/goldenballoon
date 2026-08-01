#!/usr/bin/env python3
"""Compile a real-ROM state trace into a local native oracle replay.

The instrumented ares trace observes the exact input and ``updateRate`` used by
each completed rendered game update. This tool collapses repeated VI samples by
framebuffer serial, then emits two ROM-free diagnostic artifacts:

* a strict ``tick fields`` schedule for ``MDKR_ORACLE_UPDATE_FIELDS``;
* a native ``--input-script`` with the observed controller states.

The output remains local and git-ignored. It is evidence about timestep
partitioning, not a shipping gameplay mode.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn


REQUIRED_COLUMNS = {
    "frame",
    "valid",
    "racer_index",
    "logic_update_rate",
    "framebuffer_serial",
    "input_buttons",
    "stick_x",
    "stick_y",
}
BUTTONS = (
    (0x8000, "A"),
    (0x4000, "B"),
    (0x2000, "Z"),
    (0x1000, "START"),
    (0x0800, "UP"),
    (0x0400, "DOWN"),
    (0x0200, "LEFT"),
    (0x0100, "RIGHT"),
    (0x0020, "L"),
    (0x0010, "R"),
    (0x0008, "CUP"),
    (0x0004, "CDOWN"),
    (0x0002, "CLEFT"),
    (0x0001, "CRIGHT"),
)
SUPPORTED_MASK = sum(bit for bit, _ in BUTTONS)
SCRIPT_RE = re.compile(
    r"^(?P<frame>\d+)\s+(?P<tokens>\S+)"
    r"(?:\s+(?P<hold>\d+))?(?:\s+(?P<port>P[1-4]))?\s*$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class ReferenceUpdate:
    frame: int
    serial: int
    fields: int
    buttons: int
    stick_x: int
    stick_y: int


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FAIL: {message}")


def parse_int(value: str, label: str) -> int:
    try:
        return int(value, 0)
    except (TypeError, ValueError):
        fail(f"invalid {label}: {value!r}")


def load_updates(path: Path, *, racer_index: int,
                 ares_frame_start: int) -> list[ReferenceUpdate]:
    collapsed: dict[int, ReferenceUpdate] = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames or [])
        if missing:
            fail(f"{path} lacks required column(s): {', '.join(sorted(missing))}")
        for line_number, raw in enumerate(reader, 2):
            if raw["valid"] != "1":
                continue
            if parse_int(raw["racer_index"], f"racer_index at line {line_number}") \
                    != racer_index:
                continue
            frame = parse_int(raw["frame"], f"frame at line {line_number}")
            if frame < ares_frame_start:
                continue
            serial = parse_int(
                raw["framebuffer_serial"],
                f"framebuffer_serial at line {line_number}",
            )
            fields = parse_int(
                raw["logic_update_rate"],
                f"logic_update_rate at line {line_number}",
            )
            if serial <= 0:
                fail(f"non-positive framebuffer serial at line {line_number}")
            if not 1 <= fields <= 6:
                fail(f"logic_update_rate outside 1..6 at line {line_number}")
            collapsed[serial] = ReferenceUpdate(
                frame=frame,
                serial=serial,
                fields=fields,
                buttons=parse_int(
                    raw["input_buttons"], f"input_buttons at line {line_number}"
                ),
                stick_x=parse_int(raw["stick_x"], f"stick_x at line {line_number}"),
                stick_y=parse_int(raw["stick_y"], f"stick_y at line {line_number}"),
            )

    updates = [collapsed[serial] for serial in sorted(collapsed)]
    if not updates:
        fail(f"{path} has no usable racer {racer_index} rows at frame {ares_frame_start}+")
    for previous, current in zip(updates, updates[1:]):
        if current.serial != previous.serial + 1:
            fail(
                "framebuffer serial gap: "
                f"{previous.serial} at frame {previous.frame} -> "
                f"{current.serial} at frame {current.frame}"
            )
    return updates


def native_tokens(update: ReferenceUpdate) -> tuple[str, ...]:
    if update.buttons < 0 or update.buttons > 0xFFFF:
        fail(f"input mask outside 16 bits at ares frame {update.frame}")
    unknown = update.buttons & ~SUPPORTED_MASK
    if unknown:
        fail(f"unsupported input bits 0x{unknown:04x} at ares frame {update.frame}")
    tokens = tuple(name for bit, name in BUTTONS if update.buttons & bit)

    expected_x = 0
    expected_y = 0
    if update.buttons & 0x0200:
        expected_x = -80
    if update.buttons & 0x0100:
        if expected_x:
            fail(f"opposed horizontal directions at ares frame {update.frame}")
        expected_x = 80
    if update.buttons & 0x0800:
        expected_y = 80
    if update.buttons & 0x0400:
        if expected_y:
            fail(f"opposed vertical directions at ares frame {update.frame}")
        expected_y = -80
    if (update.stick_x, update.stick_y) != (expected_x, expected_y):
        fail(
            "native script cannot represent analog state at ares frame "
            f"{update.frame}: buttons imply ({expected_x},{expected_y}), "
            f"trace contains ({update.stick_x},{update.stick_y})"
        )
    return tokens


def clipped_base_script(path: Path, boundary: int) -> list[str]:
    output = ["# Native route retained before the real-ROM replay boundary."]
    for line_number, source in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = source.split("#", 1)[0].strip()
        if not line:
            continue
        match = SCRIPT_RE.fullmatch(line)
        if match is None:
            fail(f"malformed native input at {path}:{line_number}: {source!r}")
        frame = int(match["frame"])
        hold = int(match["hold"] or "3")
        if hold < 1:
            fail(f"non-positive native hold at {path}:{line_number}")
        if frame >= boundary:
            continue
        hold = min(hold, boundary - frame)
        suffix = f" {match['port'].upper()}" if match["port"] else ""
        output.append(f"{frame} {match['tokens'].upper()} {hold}{suffix}")
    return output


def emit_inputs(updates: list[ReferenceUpdate], *, input_start: int,
                base_script: Path) -> str:
    lines = clipped_base_script(base_script, input_start)
    lines.append("# Reference-observed state: one native tick per rendered ROM update.")
    states = [native_tokens(update) for update in updates]
    run_start = 0
    for index in range(1, len(states) + 1):
        if index < len(states) and states[index] == states[run_start]:
            continue
        tokens = states[run_start]
        if tokens:
            lines.append(
                f"{input_start + run_start} {'+'.join(tokens)} {index - run_start}"
            )
        run_start = index
    return "\n".join(lines) + "\n"


def emit_fields(updates: list[ReferenceUpdate], *, field_start: int,
                ares_frame_start: int, input_start: int) -> str:
    lines = [
        "# Real-ROM updateRate replay: nativeTick fields.",
        f"# Native field boundary: {field_start}",
        f"# Native input boundary: {input_start}",
        f"# Ares observation boundary: {ares_frame_start}",
    ]
    lines.extend(
        f"{field_start + index} {update.fields}"
        for index, update in enumerate(updates)
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ares-trace", required=True, type=Path)
    parser.add_argument("--base-native-input", required=True, type=Path)
    parser.add_argument("--fields-out", required=True, type=Path)
    parser.add_argument("--input-out", required=True, type=Path)
    parser.add_argument("--ares-frame-start", required=True, type=int)
    parser.add_argument("--field-start", required=True, type=int)
    parser.add_argument("--input-start", required=True, type=int)
    parser.add_argument("--racer-index", default=0, type=int)
    args = parser.parse_args()
    for label in ("ares_frame_start", "field_start", "input_start"):
        if getattr(args, label) < 0:
            fail(f"--{label.replace('_', '-')} must be non-negative")
    if args.input_start < args.field_start:
        fail("--input-start must not precede --field-start")
    for label in ("ares_trace", "base_native_input"):
        path = getattr(args, label)
        if not path.is_file():
            fail(f"--{label.replace('_', '-')} is not a file: {path}")

    updates = load_updates(
        args.ares_trace,
        racer_index=args.racer_index,
        ares_frame_start=args.ares_frame_start,
    )
    fields = emit_fields(
        updates,
        field_start=args.field_start,
        ares_frame_start=args.ares_frame_start,
        input_start=args.input_start,
    )
    inputs = emit_inputs(
        updates,
        input_start=args.input_start,
        base_script=args.base_native_input,
    )
    source_sha256 = hashlib.sha256(args.ares_trace.read_bytes()).hexdigest()
    fields = f"# Source ares trace SHA-256: {source_sha256}\n" + fields
    inputs = f"# Source ares trace SHA-256: {source_sha256}\n" + inputs
    args.fields_out.parent.mkdir(parents=True, exist_ok=True)
    args.input_out.parent.mkdir(parents=True, exist_ok=True)
    args.fields_out.write_text(fields, encoding="utf-8")
    args.input_out.write_text(inputs, encoding="utf-8")
    print(
        "PASS: reference replay "
        f"updates={len(updates)} serials={updates[0].serial}..{updates[-1].serial} "
        f"fields={args.field_start}..{args.field_start + len(updates) - 1} "
        f"inputs={args.input_start}..{args.input_start + len(updates) - 1}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
