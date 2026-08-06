#!/usr/bin/env python3
"""Census the production F3DDKR/WebGPU material corpus and its safety margins.

This is deliberately a content gate, not another screenshot oracle. It runs all
nine authored menu routes, every main race, every boss race, all four challenge
courses, the Adventure intro/hub, and the distinct 3P-minimap/4P layouts through
the real WebGPU backend with MDKR_DL_STRICT=1. Each process must report:

* zero display-list faults and a valid command/depth census;
* every material identity and dynamic pipeline key it created;
* no shader guard, pipeline failure, or portable attribute/varying breach;
* at least 4x shader-index and 2x per-material pipeline/bind-group headroom.

The per-process reports are unioned so the final line documents the measured
corpus rather than presenting one representative race as all-content evidence.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "tests" / "input_scripts"
RACE_SCRIPT = SCRIPTS / "race_full_3lap_tt.txt"

TEXCACHE_RE = re.compile(r"\[TEXCACHE\] staleHits=(\d+)")

MENU_ROUTES = (
    ("nav_to_options", 1700, "menuId=12"),
    ("nav_to_audio_options", 1900, "menuId=13"),
    ("nav_to_save_options", 1950, "menuId=14"),
    ("nav_to_magic_codes", 2050, "menuId=10"),
    ("nav_to_character_select", 1600, "menuId=3"),
    ("nav_to_game_select", 2000, "menuId=19"),
    ("nav_to_file_select_adventure", 2100, "menuId=6"),
    ("nav_to_track_select", 2300, "menuId=15"),
    ("nav_to_time_trial_race", 2900, "levelId=5"),
)
MAIN_TRACKS = (5, 3, 29, 7, 8, 4, 10, 30, 13, 6, 9, 28, 19, 18, 20, 31, 17, 32, 33, 15)
BOSS_TRACKS = (38, 46, 40, 53, 1, 52, 41, 54, 37, 55)
CHALLENGE_TRACKS = (11, 25, 26, 27)
LAYOUT_ROUTES = (
    ("race_3p_split", 3500, "level_load: levelId=5 numPlayers=2"),
    ("race_4p_split", 8400, "level_load: levelId=5 numPlayers=3"),
)

BAD_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
    "Assertion failed",
)
DL_RE = re.compile(
    r"^\[DL-CENSUS\] commands=(\d+) lists=(\d+) maxDepth=(\d+) "
    r"faults=(\d+) opcodes=(.*)$",
    re.MULTILINE,
)
LIMIT_RE = re.compile(
    r"^\[WGPU-LIMITS\] shaders=(\d+)/(\d+) pipelines=(\d+) "
    r"attrs=(\d+)/(\d+) varyings=(\d+)/(\d+) tableOverflow=(\d+) "
    r"pipelineFailures=(\d+) vertexBytes=(\d+) vertexSegments=(\d+) "
    r"vertexSegmentCap=(\d+) textures=(\d+) samplers=(\d+) "
    r"bindGroups=(\d+)/(\d+)$",
    re.MULTILINE,
)
MATERIAL_RE = re.compile(
    r"^\[WGPU-MATERIAL\] id=([0-9a-f]{16})/([0-9a-f]{8}) "
    r"attrs=(\d+) varyings=(\d+) pipelines=(\d+)/(\d+) keys=(.*)$",
    re.MULTILINE,
)
SUMMARY_RE = re.compile(
    r"^\[WGPU-CENSUS\] materials=(\d+) maxPipelines=(\d+)/(\d+) "
    r"maxPipelineShader=([0-9a-f]{16})/([0-9a-f]{8})$",
    re.MULTILINE,
)
KEY_RE = re.compile(r"^([0-9a-f]{3}):([0-3])$")

# Structural coverage anchors. Strict mode proves that every other opcode in
# the corpus is supported too; these prevent an accidentally shallow route from
# passing merely because it emitted a well-formed census.
REQUIRED_OPCODES = {
    0x01,  # G_MTX
    0x03,  # G_MOVEMEM
    0x04,  # G_VTX
    0x05,  # G_TRIN
    0x06,  # G_DL
    0x07,  # G_DMADL
    0xB8,  # G_ENDDL
    0xBC,  # G_MOVEWORD
    0xE4,  # G_TEXRECT
    0xED,  # G_SETSCISSOR
    0xEF,  # G_RDPSETOTHERMODE
    0xF3,  # G_LOADBLOCK
    0xF5,  # G_SETTILE
    0xFC,  # G_SETCOMBINE
    0xFD,  # G_SETTIMG
}


@dataclass(frozen=True)
class Parsed:
    commands: int
    lists: int
    depth: int
    opcodes: dict[int, int]
    materials: set[tuple[int, int]]
    pipelines: set[tuple[int, int, int]]
    shader_count: int
    shader_cap: int
    pipeline_count: int
    max_material_pipelines: int


def one_match(pattern: re.Pattern[str], output: str, label: str) -> re.Match[str]:
    matches = list(pattern.finditer(output))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {label} report, found {len(matches)}")
    return matches[0]


def parse_report(output: str) -> Parsed:
    dl = one_match(DL_RE, output, "display-list census")
    limits = one_match(LIMIT_RE, output, "WebGPU limits")
    summary = one_match(SUMMARY_RE, output, "WebGPU census summary")

    commands, lists, depth, faults = map(int, dl.groups()[:4])
    if commands <= 0 or lists <= 0:
        raise ValueError("display-list census is empty")
    if faults != 0:
        raise ValueError(f"display-list census recorded {faults} fault(s)")
    opcodes: dict[int, int] = {}
    for item in dl.group(5).split(","):
        fields = item.split(":")
        if len(fields) != 2:
            raise ValueError(f"malformed opcode census item {item!r}")
        opcode, count = int(fields[0], 16), int(fields[1])
        if opcode in opcodes or not 0 <= opcode <= 0xFF or count <= 0:
            raise ValueError(f"invalid opcode census item {item!r}")
        opcodes[opcode] = count
    if sum(opcodes.values()) != commands:
        raise ValueError(
            f"opcode histogram sums to {sum(opcodes.values())}, commands={commands}"
        )

    (
        shader_count,
        shader_cap,
        pipeline_count,
        attrs,
        attr_cap,
        varyings,
        varying_cap,
        guard_hits,
        pipeline_failures,
        vertex_bytes,
        vertex_segments,
        vertex_segment_cap,
        _textures,
        _samplers,
        bind_groups,
        bind_group_cap,
    ) = map(int, limits.groups())
    if guard_hits or pipeline_failures:
        raise ValueError(
            f"WebGPU safety event: guard={guard_hits}, pipelineFailures={pipeline_failures}"
        )
    if attrs > attr_cap or varyings > varying_cap:
        raise ValueError(
            f"portable shader limit exceeded: attrs={attrs}/{attr_cap}, "
            f"varyings={varyings}/{varying_cap}"
        )
    if shader_count * 4 > shader_cap:
        raise ValueError(f"shader index has less than 4x headroom: {shader_count}/{shader_cap}")
    if bind_groups * 2 > bind_group_cap:
        raise ValueError(
            f"bind-group cache has less than 2x headroom: {bind_groups}/{bind_group_cap}"
        )
    if vertex_bytes <= 0 or vertex_segments <= 0 or vertex_segment_cap <= 0:
        raise ValueError(
            "vertex streaming census is empty or invalid: "
            f"bytes={vertex_bytes}, segments={vertex_segments}, cap={vertex_segment_cap}"
        )

    materials: set[tuple[int, int]] = set()
    pipelines: set[tuple[int, int, int]] = set()
    measured_max = 0
    material_rows = list(MATERIAL_RE.finditer(output))
    for row in material_rows:
        id0, id1 = int(row.group(1), 16), int(row.group(2), 16)
        material = (id0, id1)
        if material in materials:
            raise ValueError(f"duplicate material report {id0:016x}/{id1:08x}")
        materials.add(material)
        row_attrs, row_varyings = int(row.group(3)), int(row.group(4))
        count, cap = int(row.group(5)), int(row.group(6))
        if row_attrs > attr_cap or row_varyings > varying_cap:
            raise ValueError(f"material {id0:016x}/{id1:08x} exceeds portable limits")
        if count * 2 > cap:
            raise ValueError(
                f"material {id0:016x}/{id1:08x} has less than 2x pipeline "
                f"headroom: {count}/{cap}"
            )
        keys = [] if not row.group(7) else row.group(7).split(",")
        if len(keys) != count:
            raise ValueError(
                f"material {id0:016x}/{id1:08x} reports {count} pipelines "
                f"but {len(keys)} keys"
            )
        for item in keys:
            key_match = KEY_RE.fullmatch(item)
            if key_match is None:
                raise ValueError(f"malformed pipeline key {item!r}")
            key, state = int(key_match.group(1), 16), int(key_match.group(2))
            if state != 2:
                raise ValueError(
                    f"pipeline {id0:016x}/{id1:08x}/{key:03x} is not READY "
                    f"(state={state})"
                )
            triple = (id0, id1, key)
            if triple in pipelines:
                raise ValueError(f"duplicate pipeline key {triple!r}")
            pipelines.add(triple)
        measured_max = max(measured_max, count)

    summary_materials, summary_max, summary_cap = map(
        int, summary.groups()[:3]
    )
    if summary_materials != len(materials) or shader_count != len(materials):
        raise ValueError(
            "material totals disagree: "
            f"limits={shader_count}, rows={len(materials)}, summary={summary_materials}"
        )
    if pipeline_count != len(pipelines):
        raise ValueError(
            f"pipeline totals disagree: limits={pipeline_count}, rows={len(pipelines)}"
        )
    if summary_max != measured_max or summary_cap != 32:
        raise ValueError(
            f"pipeline high-water summary disagrees: {summary_max}/{summary_cap}, "
            f"measured={measured_max}"
        )

    return Parsed(
        commands=commands,
        lists=lists,
        depth=depth,
        opcodes=opcodes,
        materials=materials,
        pipelines=pipelines,
        shader_count=shader_count,
        shader_cap=shader_cap,
        pipeline_count=pipeline_count,
        max_material_pipelines=measured_max,
    )


def parser_positive_controls() -> None:
    valid = """\
[DL-CENSUS] commands=2 lists=1 maxDepth=0 faults=0 opcodes=01:1,b8:1
[WGPU-LIMITS] shaders=1/4096 pipelines=1 attrs=2/16 varyings=1/16 tableOverflow=0 pipelineFailures=0 vertexBytes=36 vertexSegments=1 vertexSegmentCap=4096 textures=1 samplers=1 bindGroups=1/2048
[WGPU-MATERIAL] id=0000000000000001/00000002 attrs=2 varyings=1 pipelines=1/32 keys=000:2
[WGPU-CENSUS] materials=1 maxPipelines=1/32 maxPipelineShader=0000000000000001/00000002
"""
    parse_report(valid)
    broken = (
        valid.replace("faults=0", "faults=1", 1),
        valid.replace("tableOverflow=0", "tableOverflow=1", 1),
        valid.replace("pipelines=1/32", "pipelines=17/32", 1),
        valid.replace("keys=000:2", "keys=000:1", 1),
        valid.replace("commands=2", "commands=3", 1),
    )
    for index, text in enumerate(broken):
        try:
            parse_report(text)
        except ValueError:
            continue
        raise AssertionError(f"parser positive control {index} was accepted")


def run_case(
    binary: Path,
    rom: Path,
    name: str,
    frames: int,
    script: Path,
    expected: str,
    extra_env: dict[str, str] | None,
    timeout: int,
) -> tuple[Parsed | None, str | None]:
    with tempfile.TemporaryDirectory(prefix=f"mdkr-wgpu-census-{name}-") as temp:
        run_root = Path(temp)
        save_dir = run_root / "save"
        save_dir.mkdir()
        env = dict(os.environ)
        env.update(
            {
                "MDKR_AUDIO": "0",
                "MDKR_RENDERER": "webgpu",
                "MDKR_RENDER_SCALE": "1",
                "MDKR_TRACE": "1",
                "MDKR_DL_STRICT": "1",
                "MDKR_DL_CENSUS": "1",
                "MDKR_WGPU_CENSUS": "1",
                # The texture-cache content audit (gfx_pc_dkr.c) has to run on
                # the routes that reach the menus, which is where the historical
                # key-aliasing bug rendered a stale texture. nav_to_track_select
                # is one of them.
                "MDKR_TEXCACHE_VERIFY": "1",
                "MDKR_SAVE_DIR": str(save_dir),
            }
        )
        if extra_env:
            env.update(extra_env)
        command = [
            str(binary),
            "--headless-frames",
            str(frames),
            "--input-script",
            str(script),
            "--rom",
            str(rom),
        ]
        try:
            proc = subprocess.run(
                command,
                cwd=run_root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return None, f"timed out after {timeout}s"
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        return None, f"exit code {proc.returncode}\n" + "\n".join(output.splitlines()[-30:])
    marker = next((item for item in BAD_MARKERS if item in output), None)
    if marker:
        return None, f"bad log marker {marker!r}"
    if "[mdkr64] renderer backend: webgpu" not in output:
        return None, "requested WebGPU backend was not active"
    if expected not in output:
        return None, f"route did not emit expected marker {expected!r}"
    # An absent report is a failure, not a skip: it means the audit did not run
    # and the assertion below would otherwise cover nothing.
    stale = TEXCACHE_RE.search(output)
    if stale is None:
        return None, "MDKR_TEXCACHE_VERIFY=1 produced no [TEXCACHE] report"
    if int(stale.group(1)) != 0:
        return None, (f"{stale.group(1)} texture-cache hit(s) served content that "
                      "had changed since upload")
    try:
        return parse_report(output), None
    except ValueError as exc:
        return None, str(exc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--quick",
        action="store_true",
        help="run one menu, one main race, one boss, and one challenge",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.exists() or not rom.exists():
        print(f"FAIL: missing {binary} or {rom}", file=sys.stderr)
        return 1

    parser_positive_controls()
    cases: list[tuple[str, int, Path, str, dict[str, str] | None]] = []
    menus = MENU_ROUTES[:1] if args.quick else MENU_ROUTES
    mains = MAIN_TRACKS[:1] if args.quick else MAIN_TRACKS
    bosses = BOSS_TRACKS[:1] if args.quick else BOSS_TRACKS
    challenges = CHALLENGE_TRACKS[:1] if args.quick else CHALLENGE_TRACKS
    for name, frames, expected in menus:
        cases.append((name, frames, SCRIPTS / f"{name}.txt", expected, None))
    for track in mains:
        cases.append(
            (
                f"main_{track}",
                3200,
                RACE_SCRIPT,
                f"level_load: levelId={track} numPlayers=0",
                {"MDKR_AUTOPILOT": "1", "MDKR_TRACE": "1", "MDKR_LOAD_TRACK": str(track)},
            )
        )
    for track in bosses:
        cases.append(
            (
                f"boss_{track}",
                3200,
                RACE_SCRIPT,
                f"level_load: levelId={track} numPlayers=0",
                {
                    "MDKR_AUTOPILOT": "1",
                    "MDKR_TRACE": "1",
                    "MDKR_LOAD_TRACK": str(track),
                    "MDKR_BOSS_PRECLEARED": str(track),
                },
            )
        )
    for track in challenges:
        cases.append(
            (
                f"challenge_{track}",
                3200,
                RACE_SCRIPT,
                f"level_load: levelId={track} numPlayers=0",
                {"MDKR_AUTOPILOT": "1", "MDKR_TRACE": "1", "MDKR_LOAD_TRACK": str(track)},
            )
        )
    if not args.quick:
        for name, frames, expected in LAYOUT_ROUTES:
            cases.append(
                (
                    name,
                    frames,
                    SCRIPTS / f"{name}.txt",
                    expected,
                    {"MDKR_AUTOPILOT": "1", "MDKR_UI_OVERLAY_TRACE": "1"},
                )
            )
        cases.append(
            (
                "adventure_intro_hub",
                6200,
                SCRIPTS / "adventure_hub_drive.txt",
                "level_load: levelId=0 numPlayers=0",
                {"MDKR_AUTOPILOT": "1"},
            )
        )

    union_opcodes: dict[int, int] = {}
    union_materials: set[tuple[int, int]] = set()
    union_pipelines: set[tuple[int, int, int]] = set()
    total_commands = 0
    max_depth = 0
    max_material_pipelines = 0
    failures: list[str] = []
    for index, (name, frames, script, expected, env) in enumerate(cases, 1):
        if args.verbose:
            print(f"  [{index:02d}/{len(cases)}] {name}")
        result, error = run_case(
            binary, rom, name, frames, script, expected, env, args.timeout
        )
        if error:
            failures.append(f"{name}: {error}")
            continue
        assert result is not None
        total_commands += result.commands
        max_depth = max(max_depth, result.depth)
        max_material_pipelines = max(
            max_material_pipelines, result.max_material_pipelines
        )
        union_materials.update(result.materials)
        union_pipelines.update(result.pipelines)
        for opcode, count in result.opcodes.items():
            union_opcodes[opcode] = union_opcodes.get(opcode, 0) + count

    missing_opcodes = sorted(REQUIRED_OPCODES - set(union_opcodes))
    if missing_opcodes:
        failures.append(
            "corpus missed structural opcode anchors: "
            + ", ".join(f"0x{opcode:02x}" for opcode in missing_opcodes)
        )
    if len(union_opcodes) < 30:
        failures.append(
            f"corpus exercised only {len(union_opcodes)} distinct opcodes; need at least 30"
        )
    if max_depth < 2:
        failures.append(f"corpus display-list recursion depth only reached {max_depth}")
    if not union_materials or not union_pipelines:
        failures.append("corpus produced no WebGPU material/pipeline identities")

    if failures:
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("check_webgpu_content_census: FAIL")
        return 1
    print(
        "check_webgpu_content_census: PASS — "
        f"{len(cases)} routes, {total_commands:,} DL commands, "
        f"{len(union_opcodes)} opcodes, depth {max_depth}, "
        f"{len(union_materials)} material ids, {len(union_pipelines)} "
        f"material/pipeline keys, max {max_material_pipelines}/32 pipelines "
        "per material; zero strict faults/guard hits/pipeline failures"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
