#!/usr/bin/env python3
"""End-to-end gate for the native virtual Taj character.

Uses only the supported retail ROM supplied by the caller, always runs muted
and bounded, and starts from a fresh save directory. The scripted route enters
ABRACADABRA through the real keyboard, selects the next virtual roster identity,
starts a race, and restarts it through the pause menu.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_taj_challenges import make_eeprom, replace_bits
from check_taj_character_select import read_ppm
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, seal_slot


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/taj_unlock_select.txt"
SPLIT_SCRIPT = ROOT / "tests/input_scripts/taj_2p_split.txt"
MULTIPLAYER_SCRIPTS = {
    3: ROOT / "tests/input_scripts/taj_3p_split.txt",
    4: ROOT / "tests/input_scripts/taj_4p_split.txt",
}
CANONICAL_TT_SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
FRAMES = 8000
TIME_TRIAL_FRAMES = 12500
CANONICAL_TT_FRAMES = 13000
SPLIT_FRAMES = 3600
VEHICLE_FRAMES = 6200
PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
CARPET_WITNESS_RE = re.compile(
    r"\[TAJVIS\] event=carpet_animation .*"
    r"carpet_animations=(\d+) carpet_frame=(-?\d+) carpet_task=(-?\d+) "
    r"vertex_hash=([0-9a-f]{16}) carpet_scale=([0-9.]+) "
    r"rider_scale=([0-9.]+) carpet_base_scale=([0-9.]+) "
    r"rider_base_scale=([0-9.]+) authored_ratio=([0-9.]+) "
    r"observed_ratio=([0-9.]+) dash_active=(\d+)"
)
DASH_PULSE_RE = re.compile(
    r"\[TAJVIS\] event=dash_pulse .*authored_ratio=([0-9.]+) "
    r"observed_ratio=([0-9.]+) dash_active=1"
)
SUPPORTED_CARPET_RIDER_RATIO = 7.0 / 15.0
# The two ROM-authored header scales the ratio above is made of. Pinning them
# ABSOLUTELY is what makes the ratio assertions mean something: comparing
# carpet_base_scale/rider_base_scale against the authored_ratio printed on the
# same row only ever compared one quantity with itself, so it could not fail
# for any reason except a corrupted printf.
SUPPORTED_CARPET_BASE_SCALE = 0.28
SUPPORTED_RIDER_BASE_SCALE = 0.60
# Every scale on the witness row is printed with "%.6f", so a value
# reconstructed from two printed operands carries up to half an ulp of print
# error from each. Compare reconstructions at the precision the trace actually
# has, not at a tolerance tighter than its own output format.
PRINTED_SCALE_EPSILON = 5e-7
def reconstruction_tolerance(numerator: float, denominator: float) -> float:
    """Worst-case error of numerator/denominator given 6-decimal operands."""
    if denominator <= 0.0:
        return 0.0
    quotient = numerator / denominator
    return (PRINTED_SCALE_EPSILON * (1.0 + abs(quotient)) / denominator) * 2.0


def carpet_witness_failures(output: str) -> list[str]:
    """Validate the exact animated mesh and authored composition scale."""
    rows = [
        (int(animations), int(frame), int(task), int(vertex_hash, 16),
         float(carpet_scale), float(rider_scale), float(carpet_base_scale),
         float(rider_base_scale), float(authored_ratio), float(observed_ratio),
         int(dash_active))
        for animations, frame, task, vertex_hash, carpet_scale, rider_scale,
        carpet_base_scale, rider_base_scale, authored_ratio,
        observed_ratio, dash_active in CARPET_WITNESS_RE.findall(output)
    ]
    failures: list[str] = []
    if len(rows) < 6:
        return [f"only {len(rows)} carpet animation witnesses (need 6)"]
    if any(row[0] < 1 for row in rows):
        failures.append("carpet model did not expose its authored animation")
    if len({row[1] for row in rows}) < 4:
        failures.append("carpet animation clock did not advance")
    if {row[2] for row in rows} != {0, 1}:
        failures.append("carpet animation did not publish both vertex buffers")
    vertex_hashes = {row[3] for row in rows if row[3] != 0}
    if len(vertex_hashes) < 4:
        failures.append("carpet rendered vertex stream remained flat/frozen")
    saw_settled_scale = False
    for row in rows:
        (_, _, _, _, carpet_scale, rider_scale, carpet_base_scale,
         rider_base_scale, authored, observed, dash_active) = row
        if carpet_base_scale <= 0.0 or rider_base_scale <= 0.0:
            failures.append("invalid ROM-authored carpet/rider base scales")
            break
        if not (0.0 < authored < 1.0):
            failures.append(
                f"invalid ROM-authored carpet:rider ratio {authored:.6f}")
            break
        if abs(authored - SUPPORTED_CARPET_RIDER_RATIO) > 0.00001:
            failures.append(
                "supported ROM carpet:rider ratio changed "
                f"({authored:.6f} vs "
                f"{SUPPORTED_CARPET_RIDER_RATIO:.6f})")
            break
        if (abs(carpet_base_scale - SUPPORTED_CARPET_BASE_SCALE) >
                PRINTED_SCALE_EPSILON or
                abs(rider_base_scale - SUPPORTED_RIDER_BASE_SCALE) >
                PRINTED_SCALE_EPSILON):
            failures.append(
                "ROM-authored base scales are not the supported pair "
                f"({carpet_base_scale:.6f}/{rider_base_scale:.6f}, want "
                f"{SUPPORTED_CARPET_BASE_SCALE:.6f}/"
                f"{SUPPORTED_RIDER_BASE_SCALE:.6f})")
            break
        if (abs(carpet_base_scale / rider_base_scale - authored) >
                reconstruction_tolerance(carpet_base_scale,
                                         rider_base_scale)):
            failures.append(
                "authored ratio does not follow from the base scales it was "
                f"derived from ({carpet_base_scale:.6f}/"
                f"{rider_base_scale:.6f} vs {authored:.6f})")
            break
        if dash_active:
            failures.append("regular carpet-animation witness overlapped the dash")
            break
        if abs(authored - observed) > 0.00001:
            failures.append(
                "playable carpet did not preserve the ROM-authored "
                f"scale ratio ({authored:.6f} vs {observed:.6f})")
            break
        else:
            saw_settled_scale = True
        # The live pair is not a fixed constant (the dash pulses the carpet),
        # so pin it against the authored ratio -- an independently sourced
        # value -- and reconcile the row's own operands only at the precision
        # the trace prints them with.
        if rider_scale <= 0.0:
            failures.append(
                f"live rider scale is not positive ({rider_scale:.6f})")
            break
        if abs(carpet_scale / rider_scale - SUPPORTED_CARPET_RIDER_RATIO) > 1e-5:
            failures.append(
                "live carpet/rider scales left the supported ROM ratio "
                f"({carpet_scale:.6f}/{rider_scale:.6f} = "
                f"{carpet_scale / rider_scale:.6f} vs "
                f"{SUPPORTED_CARPET_RIDER_RATIO:.6f})")
            break
        if (abs(carpet_scale / rider_scale - observed) >
                reconstruction_tolerance(carpet_scale, rider_scale)):
            failures.append(
                "observed ratio does not follow from the live scales it was "
                f"derived from ({carpet_scale:.6f}/{rider_scale:.6f} vs "
                f"{observed:.6f})")
            break
    if not failures and not saw_settled_scale:
        failures.append("magic-carpet scale never returned to its authored ratio")
    pulses = [(float(authored), float(observed))
              for authored, observed in DASH_PULSE_RE.findall(output)]
    if not failures and len(pulses) != 1:
        failures.append(f"expected one dash pulse witness, got {len(pulses)}")
    if not failures:
        authored, observed = pulses[0]
        pulse = observed / authored if authored > 0.0 else 0.0
        if not (1.045 <= pulse <= 1.115):
            failures.append(
                "dash carpet pulse left its intended 1.05x–1.11x "
                f"range ({pulse:.6f}x)")
    return failures


def shadow_raster_delta(production: Path, no_shadow: Path) -> tuple[int, int]:
    """Count pixels where the production carpet shadow darkens its control."""
    width, height, pixels = read_ppm(production)
    control_width, control_height, control_pixels = read_ppm(no_shadow)
    if (width, height) != (control_width, control_height):
        raise ValueError("split-screen shadow frames changed dimensions")
    darker = changed = 0
    # P1 owns the upper horizontal viewport. Its third-person carpet/ground
    # footprint remains in this lower-centre subregion throughout the sampled
    # start straight; exclude P2, HUD corners, sky, and unrelated world pixels.
    for y in range(int(height * 0.20), int(height * 0.50)):
        for x in range(int(width * 0.30), int(width * 0.70)):
            offset = (y * width + x) * 3
            rgb = pixels[offset:offset + 3]
            control_rgb = control_pixels[offset:offset + 3]
            differences = tuple(
                int(control_rgb[channel]) - int(rgb[channel])
                for channel in range(3)
            )
            if max(abs(value) for value in differences) > 3:
                changed += 1
            if sum(differences) > 15 and max(differences) > 5:
                darker += 1
    return darker, changed


def make_vehicle_select_eeprom() -> bytes:
    """Return a valid imported save with the track vehicle list unlocked."""
    payload = bytearray(make_eeprom(0x3F, 18))
    slot = bytearray(payload[:40])
    # The packed save stores 34 two-bit course statuses before tajFlags.
    # RACE_CLEARED (2) exposes the retail CAR/HOVER/PLANE selector.
    replace_bits(slot, 16, 68, int("10" * 34, 2))
    seal_slot(slot)
    payload[:40] = slot
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--save-cli",
        help="optional mdkr-save executable (defaults beside the game binary)",
    )
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    save_cli_override = (Path(args.save_cli).resolve()
                         if args.save_cli else None)
    required_paths = [binary, rom, SCRIPT, SPLIT_SCRIPT, CANONICAL_TT_SCRIPT,
                      *MULTIPLAYER_SCRIPTS.values()]
    if save_cli_override is not None:
        required_paths.append(save_cli_override)
    for path in required_paths:
        if not path.exists():
            print(f"check_taj_playable: FAIL -- missing {path}", file=sys.stderr)
            return 1
    if args.frames < FRAMES:
        print(f"check_taj_playable: FAIL -- need at least {FRAMES} frames", file=sys.stderr)
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-taj-playable-") as temporary:
        run_dir = Path(temporary)
        save_dir = run_dir / "save"
        frames_dir = run_dir / "frames"
        frames_dir.mkdir()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_TRACE="1",
            MDKR_TAJ_PHYSICS_TRACE="1",
            MDKR_TAJ_VISUAL_TRACE="1",
            # Deterministic presentation witness: the scripted race is live
            # by this authored tick. This is inert outside test environments.
            MDKR_TAJ_TEST_DASH_AT="5600",
            MDKR_DUMP_FROM="5400",
            MDKR_DUMP_EVERY="100",
            MDKR_SAVE_DIR=str(save_dir),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
        )
        command = [
            str(binary), "--headless-frames", str(args.frames),
            "--input-script", str(SCRIPT), "--dump-frames", str(frames_dir),
            "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            failures.append(f"exit code {process.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
            if marker in output:
                failures.append(f"fatal marker in output: {marker}")

        required = {
            "custom code accepted": "magic_code_submit: accepted=1 id=-2",
            "virtual identity selected": "mod_racer_select: player=0 controller=0 identity=1 donor=9",
            "race loaded": "level_load: levelId=5 numPlayers=0",
            "race rendered on WebGPU": "[mdkr64] renderer backend: webgpu",
            "exact car physics activated": "[TAJPHYS] active racer=0 player=0 vehicle=0",
            "Taj minimap identity": "taj_minimap: identity=taj rgb=255,0,255 player=0",
            "donor draw suppressed": "[TAJVIS] event=donor_suppressed",
            "single grounding decal": "[TAJVIS] event=rider_shadow_suppressed",
            "dash begins": "[TAJPHYS] dash-start racer=0",
            "dash presentation begins": "[TAJVIS] event=dash_fx_start",
            "dash ends": "[TAJPHYS] dash-end racer=0",
            "dash presentation ends": "[TAJVIS] event=dash_fx_end",
            "Taj picker highlight voice": "taj_select_sound: player=0 event=0",
            "Taj picker confirmation voice": "taj_select_sound: player=0 event=2",
        }
        for label, marker in required.items():
            if marker not in output:
                failures.append(f"{label}: missing {marker!r}")

        compose_count = output.count("[TAJVIS] event=compose")
        reset_count = output.count("[TAJVIS] event=reset")
        if compose_count < 2:
            failures.append(f"carpet composed {compose_count} times; expected initial load + restart")
        if reset_count < 1:
            failures.append("carpet lifecycle did not report restart teardown")
        if "carpet_model=203 rider_model=208" not in output:
            failures.append("carpet/rider model probe did not validate the supported ROM assets")
        for failure in carpet_witness_failures(output):
            failures.append(f"carpet presentation: {failure}")

        # Join the composition/model traces to an actual rendered race frame.
        # The centred crop must contain Taj's purple robe and the carpet's red
        # and gold field at meaningful area, not merely IDs from an invisible
        # object graph.
        try:
            width, height, pixels = read_ppm(frames_dir / "frame_5500.ppm")
            x0, x1 = int(width * 0.30), int(width * 0.70)
            y0, y1 = int(height * 0.40), int(height * 0.90)
            purple = red = gold = 0
            for y in range(y0, y1):
                for x in range(x0, x1):
                    offset = (y * width + x) * 3
                    r, g, b = pixels[offset:offset + 3]
                    purple += (b > 90 and r > 35 and b > r * 1.15 and
                               b > g * 1.15)
                    red += (r > 140 and r > g * 1.5 and r > b * 1.4)
                    gold += (r > 150 and g > 100 and b < 80 and
                             r > b * 2 and g > b * 1.5)
            region = (x1 - x0) * (y1 - y0)
            if (purple < region * 0.01 or red < region * 0.025 or
                    gold < region * 0.05):
                failures.append(
                    "rendered race frame did not visibly contain Taj and his "
                    f"carpet: purple={purple}, red={red}, gold={gold}, "
                    f"region={region}")

            # The lower-centre region isolates the carpet from Taj's robe and
            # the HUD. Bounds are deliberately broad across backends: the
            # production, ROM-derived 0.4667 ratio lies inside; the old 1:1
            # donor-transform copy is several times larger and fails closed.
            carpet_red = carpet_gold = 0
            for y in range(int(height * 0.62), int(height * 0.82)):
                for x in range(int(width * 0.35), int(width * 0.65)):
                    offset = (y * width + x) * 3
                    r, g, b = pixels[offset:offset + 3]
                    carpet_red += (
                        r > 140 and r > g * 1.5 and r > b * 1.4)
                    carpet_gold += (
                        r > 150 and g > 100 and b < 80 and
                        r > b * 2 and g > b * 1.5)
            pixel_scale = (width // 320) * (height // 240)
            if not (250 * pixel_scale <= carpet_red <= 900 * pixel_scale):
                failures.append(
                    "rendered carpet width/area is inconsistent with its "
                    f"authored rider ratio ({carpet_red=})")
            if not (150 * pixel_scale <= carpet_gold <= 650 * pixel_scale):
                failures.append(
                    "rendered carpet emblem is missing or oversized "
                    f"({carpet_gold=})")
        except (OSError, ValueError) as error:
            failures.append(f"rendered Taj race evidence failed: {error}")

        # A single instrumented negative-control run recreates both reported
        # defects. Feed it through the same witness validator and require that
        # it is rejected for a frozen mesh and lost authored scale ratio.
        control_dir = run_dir / "flat-oversized-carpet-control"
        control_dir.mkdir()
        control_env = dict(env)
        control_env.update(
            MDKR_DUMP_FROM="999999",
            MDKR_TAJ_VISUAL_FREEZE_CARPET="1",
            MDKR_TAJ_VISUAL_UNSCALED_CARPET="1",
            MDKR_SAVE_DIR=str(control_dir / "save"),
            MDKR_VIDEO_CONFIG_PATH=str(control_dir / "save" / "video.ini"),
        )
        control_process = subprocess.run(
            [str(binary), "--headless-frames", "5450", "--input-script",
             str(SCRIPT), "--rom", str(rom)],
            cwd=control_dir, env=control_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        control_output = control_process.stdout or ""
        control_failures = carpet_witness_failures(control_output)
        if control_process.returncode != 0:
            failures.append("flat/oversized carpet control exited non-zero")
        if not any("flat/frozen" in failure or "clock" in failure
                   for failure in control_failures):
            failures.append(
                "carpet witness oracle accepted the frozen-mesh control")
        if not any("scale ratio" in failure for failure in control_failures):
            failures.append(
                "carpet witness oracle accepted the oversized 1:1 control")

        pace = [tuple(map(int, match.groups())) for match in PACE_RE.finditer(output)]
        if not pace:
            failures.append("no race progress trace")
        elif max(row[2] for row in pace) < 5:
            failures.append(f"race made too little progress (max checkpoint {max(row[2] for row in pace)})")

        state_path = save_dir / "taj_mod_state.ini"
        if not state_path.exists():
            failures.append("global Taj sidecar was not created")
        elif state_path.read_text(encoding="ascii") != (
            "mod_roster_version=3\ntaj_unlocked=1\n"
            "taj_migration_complete=1\nwizpig_unlocked=0\n"
            "wizpig_migration_complete=0\nterry_unlocked=0\n"
            "terry_migration_complete=0"
        ):
            failures.append("global Taj sidecar has unexpected contents")
        eeprom_path = save_dir / "eeprom.bin"
        if not eeprom_path.exists() or eeprom_path.stat().st_size != 512:
            failures.append("retail EEPROM image is missing or changed size")

        # Autopilot owns the live control word, so a separate ordinary-input
        # arm proves Z dispatches Taj's authored magic call rather than Diddy's
        # donor horn. Audio stays muted; the sound-event identity is exact.
        horn_dir = run_dir / "horn"
        horn_dir.mkdir()
        horn_env = dict(env)
        horn_env.pop("MDKR_AUTOPILOT", None)
        horn_process = subprocess.run(
            [str(binary), "--headless-frames", "6300", "--input-script",
             str(SCRIPT), "--rom", str(rom)],
            cwd=horn_dir, env=horn_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        horn_output = horn_process.stdout or ""
        if horn_process.returncode != 0:
            failures.append("Taj horn input arm exited non-zero")
        if "taj_horn: player=0" not in horn_output:
            failures.append("Taj horn input did not dispatch the Taj magic call")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in horn_output:
                failures.append(f"Taj horn arm fatal marker: {marker}")

        # A pre-mod/imported Adventure slot that already completed all three
        # challenges must unlock Taj before the first character-select visit.
        # A migration tombstone prevents Erase All Bonuses from immediately
        # re-importing the same progress on a later boot.
        imported_dir = run_dir / "imported"
        imported_save = imported_dir / "save"
        imported_save.mkdir(parents=True)
        (imported_save / "eeprom.bin").write_bytes(make_eeprom(0x3F, 18))
        imported_env = dict(env, MDKR_SAVE_DIR=str(imported_save),
                            MDKR_VIDEO_CONFIG_PATH=str(imported_save / "video.ini"))
        imported_command = [
            str(binary), "--headless-frames", "1700", "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(imported_command), flush=True)
        imported_process = subprocess.run(
            imported_command, cwd=imported_dir, env=imported_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        imported_output = imported_process.stdout or ""
        imported_state = imported_save / "taj_mod_state.ini"
        if imported_process.returncode != 0:
            failures.append(f"imported-save migration exit code {imported_process.returncode}")
        if (not imported_state.exists() or
                "taj_unlocked=1" not in imported_state.read_text(encoding="ascii")):
            failures.append("completed imported Adventure save did not unlock Taj at boot")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
            if marker in imported_output:
                failures.append(f"imported-save migration fatal marker in output: {marker}")

        # Reboot against the persisted unlock and prove a Taj P1 can race next
        # to an ordinary Diddy P2 even though both safely resolve to donor row 9.
        split_dir = run_dir / "split"
        split_dir.mkdir()
        split_save = split_dir / "save"
        split_frames = split_dir / "frames"
        shutil.copytree(save_dir, split_save)
        split_frames.mkdir()
        split_env = dict(
            env,
            MDKR_SAVE_DIR=str(split_save),
            MDKR_VIDEO_CONFIG_PATH=str(split_save / "video.ini"),
            MDKR_DUMP_FROM="3000",
            MDKR_DUMP_EVERY="50",
        )
        split_command = [
            str(binary), "--headless-frames", str(SPLIT_FRAMES),
            "--input-script", str(SPLIT_SCRIPT), "--dump-frames",
            str(split_frames), "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(split_command), flush=True)
        split_process = subprocess.run(
            split_command, cwd=split_dir, env=split_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        split_output = split_process.stdout or ""
        if split_process.returncode != 0:
            failures.append(f"split-screen exit code {split_process.returncode}")
        split_required = (
            "mod_racer_select: player=0 controller=0 identity=1 donor=9",
            "mod_racer_select: player=1 controller=1 identity=0 donor=9",
            "hud_init: hudPlayers=1 numViewports=2",
        )
        for marker in split_required:
            if marker not in split_output:
                failures.append(f"split-screen missing {marker!r}")
        if split_output.count("[TAJPHYS] active") != 1:
            failures.append("split-screen did not activate physics for Taj alone")
        if split_output.count("[TAJVIS] event=compose") != 1:
            failures.append("split-screen did not compose exactly one Taj carpet")
        if split_output.count("[TAJVIS] event=multiplayer_shadow") != 1:
            failures.append(
                "split-screen did not generate exactly one Taj carpet shadow")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
            if marker in split_output:
                failures.append(f"split-screen fatal marker in output: {marker}")

        # The companion objects deliberately use BHV_NONE, so retail's
        # split-screen shadow filter would otherwise discard both. Recreate
        # that exact pre-fix policy and require a visible grounding-shadow
        # delta across the same deterministic race frames.
        no_shadow_dir = run_dir / "split-no-carpet-shadow-control"
        no_shadow_save = no_shadow_dir / "save"
        no_shadow_frames = no_shadow_dir / "frames"
        no_shadow_dir.mkdir()
        shutil.copytree(save_dir, no_shadow_save)
        no_shadow_frames.mkdir()
        no_shadow_env = dict(
            split_env,
            MDKR_SAVE_DIR=str(no_shadow_save),
            MDKR_VIDEO_CONFIG_PATH=str(no_shadow_save / "video.ini"),
            MDKR_TAJ_VISUAL_DISABLE_MULTIPLAYER_SHADOW="1",
        )
        no_shadow_process = subprocess.run(
            [str(binary), "--headless-frames", str(SPLIT_FRAMES),
             "--input-script", str(SPLIT_SCRIPT), "--dump-frames",
             str(no_shadow_frames), "--rom", str(rom)],
            cwd=no_shadow_dir, env=no_shadow_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        no_shadow_output = no_shadow_process.stdout or ""
        if no_shadow_process.returncode != 0:
            failures.append("split-screen no-shadow control exited non-zero")
        for marker in split_required:
            if marker not in no_shadow_output:
                failures.append(
                    f"split-screen no-shadow control missing {marker!r}")
        if no_shadow_output.count("[TAJPHYS] active") != 1:
            failures.append(
                "split-screen no-shadow control did not activate Taj alone")
        if no_shadow_output.count("[TAJVIS] event=compose") != 1:
            failures.append(
                "split-screen no-shadow control did not compose one Taj carpet")
        if "[TAJVIS] event=multiplayer_shadow" in no_shadow_output:
            failures.append(
                "split-screen no-shadow control still admitted the Taj carpet")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in no_shadow_output:
                failures.append(
                    f"split-screen no-shadow control fatal marker: {marker}")
        production_frames = sorted(split_frames.glob("frame_*.ppm"))
        shadow_deltas: list[tuple[int, int]] = []
        try:
            for production_frame in production_frames:
                control_frame = no_shadow_frames / production_frame.name
                if control_frame.exists():
                    shadow_deltas.append(
                        shadow_raster_delta(production_frame, control_frame))
        except (OSError, ValueError) as error:
            failures.append(
                f"split-screen carpet-shadow evidence failed: {error}")
        if not shadow_deltas:
            failures.append("split-screen carpet-shadow frames were not captured")
        else:
            darker, changed = max(shadow_deltas)
            if darker < 20 or changed < 20:
                failures.append(
                    "split-screen raster did not distinguish the generated "
                    f"carpet shadow from its exact control ({darker=}, "
                    f"{changed=})")

        # P3- and P4-owned Taj selections exercise the remaining live-player
        # bits, split HUD tiers, and companion lifecycle with no virtual ID
        # entering a retail table.
        multiplayer_outputs: list[str] = []
        for players, multiplayer_script in MULTIPLAYER_SCRIPTS.items():
            multiplayer_dir = run_dir / f"{players}p"
            multiplayer_dir.mkdir()
            multiplayer_env = dict(env, MDKR_SAVE_DIR=str(save_dir),
                                   MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"))
            multiplayer_process = subprocess.run(
                [str(binary), "--headless-frames", "3600", "--input-script",
                 str(multiplayer_script), "--rom", str(rom)],
                cwd=multiplayer_dir, env=multiplayer_env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=90, check=False,
            )
            multiplayer_output = multiplayer_process.stdout or ""
            multiplayer_outputs.append(multiplayer_output)
            taj_slot = players - 1
            expected_rows = (
                f"mod_racer_select: player={taj_slot} controller={taj_slot} "
                "identity=1 donor=9",
                f"hud_init: hudPlayers={taj_slot} numViewports={players}",
                f"[TAJPHYS] active racer={taj_slot} player={taj_slot} vehicle=0",
            )
            if multiplayer_process.returncode != 0:
                failures.append(f"{players}P Taj arm exited non-zero")
            for marker in expected_rows:
                if marker not in multiplayer_output:
                    failures.append(f"{players}P Taj arm missing {marker!r}")
            if multiplayer_output.count("[TAJVIS] event=compose") != 1:
                failures.append(
                    f"{players}P did not compose exactly one Taj carpet")
            for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                           "runtime error:"):
                if marker in multiplayer_output:
                    failures.append(
                        f"{players}P Taj arm fatal marker: {marker}")

        # Re-enter through the real unlocked frontend for both remaining player
        # vehicles. The ordinary vehicle sweep is the broad positive control;
        # these arms specifically prove the selected-Taj predicate reaches the
        # hovercraft and plane dispatches while retaining carpet composition.
        vehicle_outputs: list[str] = []
        for vehicle, name in ((1, "hovercraft"), (2, "plane")):
            vehicle_dir = run_dir / name
            vehicle_dir.mkdir()
            vehicle_save_dir = vehicle_dir / "save"
            vehicle_save_dir.mkdir()
            (vehicle_save_dir / "eeprom.bin").write_bytes(
                make_vehicle_select_eeprom()
            )
            vehicle_env = dict(env, MDKR_SAVE_DIR=str(vehicle_save_dir),
                               MDKR_VIDEO_CONFIG_PATH=str(vehicle_save_dir / "video.ini"))
            vehicle_script = vehicle_dir / f"taj_{name}.txt"
            vehicle_lines: list[str] = []
            for line in SCRIPT.read_text(encoding="ascii").splitlines():
                if line == "4560 A 4":
                    # Imported completion data also unlocks Adventure Two,
                    # inserting a row above Tracks on Game Select.
                    vehicle_lines.append("4510 DOWN 4")
                if line == "5030 A 4":
                    # The completed save first presents the Adventure I/II
                    # choice at 4890. Track setup then uses a vertical
                    # CAR/HOVER/PLANE list before confirmation at 5030.
                    if vehicle == 2:
                        vehicle_lines.append("4930 DOWN 4")
                    vehicle_lines.append("4970 DOWN 4")
                vehicle_lines.append(line)
                if line == "5170 A 4":
                    # The extra Adventure I/II prompt shifts the final GO
                    # confirmation one step beyond the base fixture.
                    vehicle_lines.append("5310 A 4")
            vehicle_script.write_text("\n".join(vehicle_lines) + "\n", encoding="ascii")
            vehicle_command = [
                str(binary), "--headless-frames", str(VEHICLE_FRAMES),
                "--input-script", str(vehicle_script), "--rom", str(rom),
            ]
            if args.verbose:
                print("$ " + " ".join(vehicle_command), flush=True)
            vehicle_process = subprocess.run(
                vehicle_command, cwd=vehicle_dir, env=vehicle_env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=90, check=False,
            )
            vehicle_output = vehicle_process.stdout or ""
            vehicle_outputs.append(vehicle_output)
            if args.verbose:
                for output_line in vehicle_output.splitlines():
                    if any(marker in output_line for marker in (
                        "menu_init:", "level_load:", "mod_racer_select:",
                        "trackmenu_input:", "[TAJPHYS]", "[TAJVIS]",
                    )):
                        print(f"[{name}] {output_line}", flush=True)
            if vehicle_process.returncode != 0:
                failures.append(f"{name} exit code {vehicle_process.returncode}")
            if f"[TAJPHYS] active racer=0 player=0 vehicle={vehicle}" not in vehicle_output:
                failures.append(f"selected Taj never entered {name} physics")
            if vehicle_output.count("[TAJVIS] event=compose") != 1:
                failures.append(f"{name} did not compose exactly one Taj carpet")
            for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
                if marker in vehicle_output:
                    failures.append(f"{name} fatal marker in output: {marker}")

        # Run the same earned frontend path as a Time Trial, without the restart
        # tail. The regular Time Trial regression is the positive control that
        # records canonical Diddy runs; this arm must reach the same finish hook
        # and leave both record tables empty.
        tt_dir = run_dir / "time-trial"
        tt_dir.mkdir()
        tt_save_dir = tt_dir / "save"
        canonical_env = dict(
            env,
            MDKR_SAVE_DIR=str(tt_save_dir),
            MDKR_VIDEO_CONFIG_PATH=str(tt_save_dir / "video.ini"),
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
        )
        canonical_command = [
            str(binary), "--headless-frames", str(CANONICAL_TT_FRAMES),
            "--input-script", str(CANONICAL_TT_SCRIPT), "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(canonical_command), flush=True)
        canonical_process = subprocess.run(
            canonical_command, cwd=tt_dir, env=canonical_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        canonical_output = canonical_process.stdout or ""
        if canonical_process.returncode != 0:
            failures.append(f"canonical Time Trial seed exit code {canonical_process.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
            if marker in canonical_output:
                failures.append(f"canonical Time Trial seed fatal marker in output: {marker}")

        save_cli = save_cli_override or binary.parent / "mdkr-save"
        tt_eeprom = tt_save_dir / "eeprom.bin"
        canonical_records: dict | None = None
        canonical_eeprom: bytes | None = None
        if not save_cli.exists() or not tt_eeprom.exists():
            failures.append("canonical Time Trial did not create an inspectable EEPROM")
        else:
            canonical_inspected = subprocess.run(
                [str(save_cli), "inspect", str(tt_eeprom)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            if canonical_inspected.returncode != 0:
                failures.append("mdkr-save could not inspect the canonical Time Trial seed")
            else:
                canonical_records = json.loads(canonical_inspected.stdout)["records"]
                if (canonical_records["fastLaps"]["count"] == 0 or
                        canonical_records["courseTimes"]["count"] == 0):
                    failures.append("canonical Time Trial seed did not establish both record tables")
                canonical_eeprom = tt_eeprom.read_bytes()

        tt_script = tt_dir / "taj_time_trial.txt"
        tt_lines: list[str] = []
        for line in SCRIPT.read_text(encoding="ascii").splitlines():
            fields = line.split()
            if fields and fields[0].isdigit() and int(fields[0]) >= 6000:
                continue
            if line == "5030 A 4":
                tt_lines.append("4970 DOWN 6")
            tt_lines.append(line)
        tt_script.write_text("\n".join(tt_lines) + "\n", encoding="ascii")
        tt_env = dict(env, MDKR_SAVE_DIR=str(tt_save_dir),
                      MDKR_VIDEO_CONFIG_PATH=str(tt_save_dir / "video.ini"))
        tt_command = [
            str(binary), "--headless-frames", str(TIME_TRIAL_FRAMES),
            "--input-script", str(tt_script), "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(tt_command), flush=True)
        tt_process = subprocess.run(
            tt_command, cwd=tt_dir, env=tt_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=90, check=False,
        )
        tt_output = tt_process.stdout or ""
        if tt_process.returncode != 0:
            failures.append(f"Time Trial exit code {tt_process.returncode}")
        if "[TAJPHYS] canonical-records-suppressed" not in tt_output:
            failures.append("Time Trial never reported canonical record suppression")
        if "[TAJVIS] event=owner_ineligible" in tt_output:
            failures.append("Taj carpet identity was lost during the finish handoff")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
            if marker in tt_output:
                failures.append(f"Time Trial fatal marker in output: {marker}")

        if not save_cli.exists() or not tt_eeprom.exists():
            failures.append("cannot inspect the Time Trial EEPROM record boundary")
        else:
            inspected = subprocess.run(
                [str(save_cli), "inspect", str(tt_eeprom)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            if inspected.returncode != 0:
                failures.append("mdkr-save could not inspect the Time Trial EEPROM")
            else:
                records = json.loads(inspected.stdout)["records"]
                if canonical_records is not None and records != canonical_records:
                    failures.append("Taj Time Trial changed an existing canonical record table")
                if (canonical_eeprom is not None and
                        tt_eeprom.read_bytes() != canonical_eeprom):
                    failures.append(
                        "Taj Time Trial changed bytes elsewhere in the canonical EEPROM")

        if args.verbose and failures:
            print((output + split_output + "".join(multiplayer_outputs) +
                   "".join(vehicle_outputs) +
                   canonical_output + tt_output)[-6000:], file=sys.stderr)

    if failures:
        for failure in failures:
            print(f"check_taj_playable: FAIL -- {failure}", file=sys.stderr)
        return 1
    print("check_taj_playable: PASS -- unlock, select, carpet, OP car/hover/plane, restart, 2P/3P/4P identity, persistence, audio identity, and TT quarantine")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
