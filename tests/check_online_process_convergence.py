#!/usr/bin/env python3
"""Prove four isolated launcher/engine processes converge on one authority stream.

This is the first real-process online-layout gate.  Every endpoint enters via
``SessionRuntime`` and a launcher-owned launch envelope; the engine sees
the resulting copy-owned roster only for its bounded lifetime.  The four
endpoints deliberately disagree about local seats and rendered views while
sharing one byte-identical canonical manifest.

The gate pins the process boundary, manifest identity, roster lifetime, live
launcher transport, endpoint-local physical listener policy, mapped camera
output/lens compatibility, drawable coverage, and authority invariance.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_4p_split.txt"
TICKS = 3600
CAPTURE_FRAME = 3400
# The rollback provider is installed process-wide but begins authored input at
# the race level, whose local rollback tick one is process authority tick 2421.
# Start impairment 29 race ticks later: visibly in gameplay, at global tick 2450.
PROFILE_PROVIDER_START_TICK = 30
PROFILE_AUTHORITY_START_TICK = 2450
# Rollback tick 240 is process authority tick 2660. The fixture authors an A
# edge on every canonical slot there, giving takeover runs a non-vacuous proof
# that the exact-cutoff input is replaced by confirmed neutral history.
TAKEOVER_INPUT_WITNESS_TICK = 240
TAKEOVER_AUTHORITY_WITNESS_TICK = 2660
RECOVERY_PROFILES = frozenset(("two-second-outage", "adversarial"))
ROSTER_RE = re.compile(
    r"^\[NET-ROSTER\] epoch=(\d+) manifest=([0-9a-f]{16}) players=(\d+) "
    r"local-mask=(0x[0-9a-f]{2}) viewport-mask=(0x[0-9a-f]{2}) "
    r"local-map=([0-3](?:,[0-3])*)? viewport-map=([0-3](?:,[0-3])*)?$",
    re.MULTILINE,
)
VIEW_RE = re.compile(
    r"^\[NET-VIEW\] output=(\d+) canonical=(\d+) layout=(\d+)$",
    re.MULTILINE,
)
LENS_RE = re.compile(
    r"^\[NET-LENS\] output=(\d+) canonical=(\d+) layout=(\d+) "
    r"aspect=([0-9.]+) vfov=([0-9.]+) compatible=1$",
    re.MULTILINE,
)
HUD_RE = re.compile(
    r"^\[NET-HUD\] canonical-layout=(\d+) output-layout=(\d+) "
    r"minimap=(-?\d+),(-?\d+)$",
    re.MULTILINE,
)
HUD_REFLOW_RE = re.compile(
    r"^\[NET-HUD-REFLOW\] output=(\d+) canonical=(\d+) layout=(\d+) "
    r"moved=([01]) scaled=([01]) shadow=1$",
    re.MULTILINE,
)
AUDIO_RE = re.compile(
    r"^\[NET-AUDIO\] listeners=(\d+) canonical=(\d+) map=([0-3](?:,[0-3])*)? "
    r"mode=(silent|spatial|shared-center) authority=canonical$",
    re.MULTILINE,
)
PROFILE_RE = re.compile(
    r"^\[NET-PROFILE\] name=([a-z-]+) sent=(\d+) dropped=(\d+) "
    r"duplicate=(\d+) reordered=(\d+) corrupted=(\d+) outage=(\d+) "
    r"throttled=(\d+) overflow=(\d+) decoded=(\d+) rejected=(\d+) "
    r"offers=(\d+) skipped=(\d+) long=(\d+) sleep=(\d+)$",
    re.MULTILINE,
)
TRANSPORT_RE = re.compile(
    r"^\[NET-TRANSPORT\] epoch=(\d+) accepted=(\d+) corrected=(\d+) "
    r"duplicate=(\d+) invalid=(\d+) stale=(\d+) unauthorized=(\d+) "
    r"conflict=(\d+) outWindow=(\d+) drained=(\d+) drainRejected=(\d+) "
    r"recovery=(\d+) takeoverStarted=(\d+) takeoverIgnored=(\d+)$",
    re.MULTILINE,
)
ROLLBACK_STATS_RE = re.compile(
    r"^\[ROLLBACK\] lab stats: ticks=(\d+) captures=(\d+) restores=(\d+) "
    r"capture_avg_ns=(\d+) capture_p50_ns=(\d+) capture_p95_ns=(\d+) "
    r"capture_p99_ns=(\d+) capture_max_ns=(\d+) restore_avg_ns=(\d+) "
    r"restore_p50_ns=(\d+) restore_p95_ns=(\d+) restore_p99_ns=(\d+) "
    r"restore_max_ns=(\d+) timing_overflow=(\d+)/(\d+) "
    r"over_8333333ns=(\d+)/(\d+) over_16666667ns=(\d+)/(\d+)$",
    re.MULTILINE,
)
RESIM_STATS_RE = re.compile(
    r"^\[ROLLBACK\] resimulation stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$", re.MULTILINE,
)
FRAME_STATS_RE = re.compile(
    r"^\[ROLLBACK\] authored-frame stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$", re.MULTILINE,
)
RECOVERY_RE = re.compile(
    r"^\[NET-RECOVERY\] reason=(input-gap|late-input) slot=([0-3]) "
    r"first=(\d+) observed=(\d+) action=return-to-launcher$",
    re.MULTILINE,
)
SESSION_RECOVERY_RE = re.compile(
    r"^\[SESSION-RECOVERY\] scene=7 engine=5 error=6 epoch=(\d+)$",
    re.MULTILINE,
)
ENDPOINTS = (
    ("slot0", "0x1", "0x1", "0", "0", (("0", "0", "0"),)),
    ("slot1", "0x2", "0x2", "1", "1", (("0", "1", "0"),)),
    ("two-local", "0x5", "0x5", "0,2", "0,2",
     (("0", "0", "1"), ("1", "2", "1"))),
    ("no-render", "0x8", "0x0", "3", "", ()),
)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online process convergence: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def stream(output: str, prefix: str) -> tuple[str, ...]:
    return tuple(line for line in output.splitlines() if line.startswith(prefix))


def input_row_at(rows: tuple[str, ...], tick: int) -> str | None:
    prefix = f"[INPUTHASH] v=1 tick={tick} "
    return next((row for row in rows if row.startswith(prefix)), None)


def input_slot_sample(row: str, slot: int) -> tuple[int, int, int, int, int, int]:
    match = re.search(
        rf"(?:^| )p{slot + 1}=(\d),([0-9a-fA-F]{{4}}),"
        rf"([0-9a-fA-F]{{4}}),([0-9a-fA-F]{{4}}),(-?\d+),(-?\d+)",
        row,
    )
    if match is None:
        raise RuntimeError(f"malformed P{slot + 1} INPUTHASH sample: {row}")
    return (
        int(match[1]), int(match[2], 16), int(match[3], 16),
        int(match[4], 16), int(match[5]), int(match[6]),
    )


def canonical_event_stream(output: str) -> tuple[tuple[int, ...], ...]:
    """Event fields that are match authority, excluding endpoint feedback.

    Sound, music and rumble are intentionally local feedback: a verifier with
    no views/devices must not manufacture them just to match a rendering peer.
    The cumulative event hash therefore cannot be compared across endpoint
    presentation layouts. Match transitions and world/result events can.
    """
    keys = (
        "tick", "transition", "save", "spawn", "despawn", "level",
        "checkpoint", "result", "context",
    )
    rows: list[tuple[int, ...]] = []
    for line in stream(output, "[EVENTHASH]"):
        fields: dict[str, int] = {}
        for token in line.split():
            key, separator, value = token.partition("=")
            if separator and key in keys:
                fields[key] = int(value)
        if tuple(fields) != keys:
            raise RuntimeError(f"malformed EVENTHASH authority row: {line}")
        rows.append(tuple(fields[key] for key in keys))
    return tuple(rows)


def run_endpoint(binary: Path, rom: Path, root: Path, name: str,
                 local_mask: str, viewport_mask: str,
                 timeout: int, verbose: bool,
                 profile: str | None = None,
                 ticks: int = TICKS,
                 takeover_slot: int | None = None,
                 takeover_tick: int = TAKEOVER_INPUT_WITNESS_TICK,
                 launch_v3: bool = False,
                 ) -> tuple[str, bytes | None]:
    run_dir = root / name
    save_dir = run_dir / "saves"
    prefs_dir = run_dir / "preferences"
    save_dir.mkdir(parents=True)
    prefs_dir.mkdir()
    frame_dir = run_dir / "frames"
    frame_dir.mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
        MDKR_APP_AUTOPLAY_TICKS=str(ticks),
        MDKR_APP_PREFS_DIR=str(prefs_dir),
        MDKR_APP_TEST_ONLINE_LOCAL_MASK=local_mask,
        # Test-only perfect carrier: every scripted canonical port is delivered
        # through launcher authentication/transport before the engine drains it.
        MDKR_APP_TEST_ONLINE_LOOPBACK_INPUTS="1",
        MDKR_APP_TEST_ONLINE_VIEWPORT_MASK=viewport_mask,
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_EVENT_HASH="1",
        MDKR_DUMP_EVERY="100000",
        MDKR_INPUT_HASH="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR_PRESENT_RATE="original",
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH="3",
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    if profile is None:
        environment["MDKR_APP_AUTOPLAY_DUMP_FRAMES"] = str(frame_dir)
        environment["MDKR_DUMP_FROM"] = str(CAPTURE_FRAME)
    if launch_v3:
        environment["MDKR_APP_TEST_ONLINE_LAUNCH_V3"] = "1"
    if profile is not None:
        environment["MDKR_APP_TEST_NET_PROFILE"] = profile
        # Exercise disruption after all four endpoints are in the actual race,
        # rather than spending the named profile on deterministic boot menus.
        environment["MDKR_APP_TEST_NET_PROFILE_START_TICK"] = str(
            PROFILE_PROVIDER_START_TICK
        )
    if takeover_slot is not None:
        environment["MDKR_APP_TEST_AI_TAKEOVER_SLOT"] = str(takeover_slot)
        environment["MDKR_APP_TEST_AI_TAKEOVER_TICK"] = str(takeover_tick)
    if name == "slot1" and profile is None:
        # Canonical slot 0 is remote here. Withhold its A edge for four ticks,
        # then require the production launcher transport and real-game snapshot
        # runtime to correct the predicted timeline.
        environment["MDKR_APP_TEST_ONLINE_DELAY_INPUT"] = "1"
    if verbose:
        print(
            f"$ ({name}) local={local_mask} viewport={viewport_mask} {binary}",
            flush=True,
        )
    process = subprocess.run(
        [str(binary)], cwd=run_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{name}: exit {process.returncode}\n{output[-8000:]}")
    forbidden = (
        "[FATAL]", "[CRASH]", "AddressSanitizer",
        "overflows=1", "forbidden_io=1", "simulation witness mismatch",
    )
    for marker in forbidden:
        if marker in output:
            raise RuntimeError(f"{name}: observed {marker!r}\n{output[-8000:]}")
    frame_path = frame_dir / f"frame_{CAPTURE_FRAME:04d}.ppm"
    return output, frame_path.read_bytes() if frame_path.is_file() else None


def ppm_pixels(frame: bytes) -> tuple[int, int, bytes]:
    parts = frame.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        raise RuntimeError("malformed mapped-view PPM capture")
    width_text, separator, height_text = parts[1].partition(b" ")
    if not separator:
        raise RuntimeError("malformed mapped-view PPM dimensions")
    width = int(width_text)
    height = int(height_text)
    pixels = parts[3]
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        raise RuntimeError("mapped-view PPM payload length mismatch")
    return width, height, pixels


def region_digest(pixels: bytes, width: int, y0: int, y1: int) -> str:
    row_bytes = width * 3
    return hashlib.sha256(pixels[y0 * row_bytes:y1 * row_bytes]).hexdigest()


def dark_fraction(pixels: bytes, width: int, y0: int, y1: int) -> float:
    row_bytes = width * 3
    region = pixels[y0 * row_bytes:y1 * row_bytes]
    count = len(region) // 3
    if count == 0:
        return 0.0
    dark = sum(
        1 for offset in range(0, len(region), 3)
        if region[offset] < 12 and region[offset + 1] < 12 and
        region[offset + 2] < 12
    )
    return dark / count


def dominant_colour_fraction(pixels: bytes, width: int, height: int,
                             x0: int, y0: int, x1: int, y1: int) -> float:
    """Cheaply detect a clear-colour gutter without classifying game art.

    A stale quarter-screen viewport leaves the right and bottom quarters as one
    byte-identical clear colour.  Sampling every fourth pixel keeps this gate
    fast at Retina drawable sizes while remaining overwhelmingly non-vacuous.
    """
    counts: dict[bytes, int] = {}
    samples = 0
    x0 = max(0, min(width, x0))
    x1 = max(x0, min(width, x1))
    y0 = max(0, min(height, y0))
    y1 = max(y0, min(height, y1))
    for y in range(y0, y1, 4):
        row = y * width * 3
        for x in range(x0, x1, 4):
            colour = pixels[row + x * 3:row + x * 3 + 3]
            counts[colour] = counts.get(colour, 0) + 1
            samples += 1
    return max(counts.values(), default=0) / samples if samples else 1.0


def reject_remote_view(binary: Path, rom: Path, root: Path,
                       timeout: int) -> None:
    run_dir = root / "invalid-remote-view"
    run_dir.mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS="1",
        MDKR_APP_TEST_ONLINE_LOCAL_MASK="0x1",
        MDKR_APP_TEST_ONLINE_VIEWPORT_MASK="0x2",
        MDKR_AUDIO="0",
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR64_HIDDEN="1",
    )
    process = subprocess.run(
        [str(binary)], cwd=run_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 2 or output.count(
            "[session-test] invalid online mask contract") != 1:
        raise RuntimeError(
            "invalid remote viewport did not fail closed before engine boot\n"
            + output[-8000:]
        )
    if "[app] boot:" in output or "[NET-ROSTER]" in output:
        raise RuntimeError("invalid remote viewport reached engine admission")


def reject_manifest_race_mismatch(binary: Path, rom: Path, root: Path,
                                  timeout: int) -> None:
    run_dir = root / "invalid-manifest-track"
    run_dir.mkdir()
    (run_dir / "preferences").mkdir()
    (run_dir / "saves").mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
        MDKR_APP_AUTOPLAY_TICKS="2800",
        MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
        MDKR_APP_TEST_ONLINE_LOCAL_MASK="0x1",
        MDKR_APP_TEST_ONLINE_LOOPBACK_INPUTS="1",
        MDKR_APP_TEST_ONLINE_MANIFEST_TRACK="6",
        MDKR_APP_TEST_ONLINE_VIEWPORT_MASK="0x1",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_PRESENT_RATE="original",
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(run_dir / "saves"),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    process = subprocess.run(
        [str(binary)], cwd=run_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    required = (
        "[ROLLBACK] online race admission rejected epoch=1 "
        "manifestTrack=6 loadedTrack=5 raceType=0 manifestHz=30 "
        "authoredHz=30 rules=1",
        "[ROLLBACK] engine startup rejected before authored tick one",
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
    )
    if process.returncode == 0 or any(output.count(marker) != 1
                                      for marker in required):
        raise RuntimeError(
            "manifest/loaded-race mismatch did not fail closed and unwind\n"
            + output[-12000:]
        )
    if any(marker in output for marker in
           ("[FATAL]", "[CRASH]", "AddressSanitizer")):
        raise RuntimeError("manifest mismatch used a crash path\n" + output[-12000:])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument(
        "--profile",
        choices=("lan", "regional-good", "regional-variable", "poor",
                 "two-second-outage", "adversarial"),
        help="route loopback carrier bytes through one named packet/clock profile",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--launch-v3", action="store_true",
        help="exercise the production descriptor/direct-load envelope",
    )
    parser.add_argument("--ai-takeover-slot", type=int, choices=range(4))
    parser.add_argument(
        "--ai-takeover-tick", type=int,
        default=TAKEOVER_INPUT_WITNESS_TICK,
    )
    args = parser.parse_args()
    if args.ai_takeover_tick <= 1:
        parser.error("--ai-takeover-tick must be greater than one")

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    outputs: dict[str, str] = {}
    captures: dict[str, bytes | None] = {}
    # The scripted route enters gameplay around tick 2,400. Keep enough race
    # time for delayed packets to reconcile, without paying for finish pixels
    # already owned by the unprofiled 3,600-tick topology gate.
    run_ticks = TICKS if args.profile is None else 2800
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr64-online-process-") as temp:
            temp_root = Path(temp)
            for name, local_mask, viewport_mask, _, _, _ in ENDPOINTS:
                outputs[name], captures[name] = run_endpoint(
                    binary, rom, temp_root, name, local_mask, viewport_mask,
                    args.timeout, args.verbose, args.profile, run_ticks,
                    args.ai_takeover_slot, args.ai_takeover_tick,
                    args.launch_v3,
                )
            reject_remote_view(binary, rom, temp_root, args.timeout)
            if (args.profile is None and args.ai_takeover_slot is None and
                    not args.launch_v3):
                reject_manifest_race_mismatch(
                    binary, rom, temp_root, args.timeout,
                )
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        return fail(str(error))

    # Pixel-level non-vacuity: the two one-seat endpoints must actually show
    # different canonical cameras full-screen. The couch endpoint must contain
    # two live, different halves separated by the production 2P divider.
    width = height = 0
    divider_dark = one_seat_gutter = 0.0
    try:
        if args.profile is not None:
            pass
        else:
            for name in ("slot0", "slot1", "two-local"):
                if captures[name] is None:
                    raise RuntimeError(
                        f"{name}: no mapped-view frame {CAPTURE_FRAME} was captured"
                    )
            if captures["slot0"] == captures["slot1"]:
                raise RuntimeError("slot0 and slot1 mapped cameras produced one image")
            one_seat_gutter = 0.0
            for name in ("slot0", "slot1"):
                one_width, one_height, one_pixels = ppm_pixels(
                    captures[name] or b""
                )
                gutter = max(
                    dominant_colour_fraction(
                        one_pixels, one_width, one_height,
                        3 * one_width // 4, 0, one_width, one_height,
                    ),
                    dominant_colour_fraction(
                        one_pixels, one_width, one_height,
                        0, 3 * one_height // 4, one_width, one_height,
                    ),
                )
                one_seat_gutter = max(one_seat_gutter, gutter)
                if gutter >= 0.90:
                    raise RuntimeError(
                        f"{name}: mapped world left a uniform drawable gutter "
                        f"(dominant={gutter:.3f})"
                    )
            width, height, couch_pixels = ppm_pixels(captures["two-local"] or b"")
            top = region_digest(couch_pixels, width, 0, height // 2 - 2)
            bottom = region_digest(couch_pixels, width, height // 2 + 2, height)
            if top == bottom:
                raise RuntimeError("two-local top and bottom views were identical")
            divider_dark = dark_fraction(
                couch_pixels, width, max(0, height // 2 - 2),
                min(height, height // 2 + 2),
            )
            if divider_dark < 0.80:
                raise RuntimeError(
                    f"two-local divider was not visible (dark={divider_dark:.3f})"
                )
    except (ValueError, RuntimeError) as error:
        return fail(str(error))

    manifest_digests: set[str] = set()
    authority: dict[str, tuple[str, ...]] = {}
    inputs: dict[str, tuple[str, ...]] = {}
    events: dict[str, tuple[str, ...]] = {}
    canonical_events: dict[str, tuple[tuple[int, ...], ...]] = {}
    profile_stats: list[tuple[int, ...]] = []
    recovery_expected = args.profile in RECOVERY_PROFILES
    for (name, local_mask, viewport_mask, local_map, viewport_map,
         expected_views) in ENDPOINTS:
        output = outputs[name]
        if args.launch_v3:
            if (output.count("[NET-LAUNCH] epoch=1 ") != 1 or
                    output.count("[NET-SELECTIONS] epoch=1 ") != 1):
                return fail(
                    f"{name}: V3 descriptor/selection witness missing", output,
                )
        matches = ROSTER_RE.findall(output)
        if len(matches) != 1:
            return fail(f"{name}: expected one roster witness, got {matches!r}", output)
        epoch, digest, players, seen_local, seen_view, seen_lmap, seen_vmap = matches[0]
        if (epoch, players, seen_local, seen_view, seen_lmap or "",
                seen_vmap or "") != (
                    "1", "4", f"0x{int(local_mask, 0):02x}",
                    f"0x{int(viewport_mask, 0):02x}", local_map, viewport_map):
            return fail(f"{name}: wrong roster witness {matches[0]!r}", output)
        manifest_digests.add(digest)
        authority[name] = stream(output, "[SIMHASH]")
        inputs[name] = stream(output, "[INPUTHASH]")
        events[name] = stream(output, "[EVENTHASH]")
        canonical_events[name] = canonical_event_stream(output)
        if recovery_expected:
            if not (PROFILE_AUTHORITY_START_TICK < len(authority[name]) < run_ticks):
                return fail(
                    f"{name}: recovery stopped at implausible tick "
                    f"{len(authority[name])}", output,
                )
        elif len(authority[name]) != run_ticks:
            return fail(
                f"{name}: expected {run_ticks} authority rows, got {len(authority[name])}",
                output,
            )
        if not inputs[name] or not canonical_events[name]:
            return fail(f"{name}: input/event witness was vacuous", output)
        if output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") != 1:
            return fail(f"{name}: engine lifetime did not tear down cleanly", output)
        rollback_stats = [
            tuple(map(int, row)) for row in ROLLBACK_STATS_RE.findall(output)
        ]
        if len(rollback_stats) != 1:
            return fail(
                f"{name}: expected one rollback budget witness, "
                f"got {rollback_stats!r}", output,
            )
        perf = rollback_stats[0]
        authored, captures, restores = perf[0:3]
        capture_avg, capture_p50, capture_p95, capture_p99, capture_max = perf[3:8]
        restore_avg, restore_p50, restore_p95, restore_p99, restore_max = perf[8:13]
        capture_overflow, restore_overflow = perf[13:15]
        over_p99_capture, over_p99_restore = perf[15:17]
        over_tail_capture, over_tail_restore = perf[17:19]
        if authored == 0 or captures <= authored or capture_avg == 0 or \
                not (capture_p50 <= capture_p95 <= capture_p99) or \
                capture_max == 0 or any((
                    capture_overflow, restore_overflow,
                    over_p99_capture, over_p99_restore,
                    over_tail_capture, over_tail_restore)):
            return fail(f"{name}: rollback capture budget failed {perf!r}", output)
        if restores == 0:
            if any((restore_avg, restore_p50, restore_p95,
                    restore_p99, restore_max)):
                return fail(
                    f"{name}: empty restore histogram was nonzero {perf!r}",
                    output,
                )
        elif restore_avg == 0 or not (
                restore_p50 <= restore_p95 <= restore_p99) or restore_max == 0:
            return fail(f"{name}: rollback restore budget failed {perf!r}", output)
        resimulation_stats = [
            tuple(map(int, row)) for row in RESIM_STATS_RE.findall(output)
        ]
        if len(resimulation_stats) != 1:
            return fail(
                f"{name}: expected one resimulation budget witness, "
                f"got {resimulation_stats!r}", output,
            )
        resim = resimulation_stats[0]
        samples, average, p50, p95, p99, maximum = resim[:6]
        if any(resim[6:]) or (samples == 0 and any(resim[1:])) or (
                samples != 0 and (
                    average == 0 or not (p50 <= p95 <= p99) or maximum == 0)):
            return fail(f"{name}: resimulation budget failed {resim!r}", output)
        if name == "slot1" and args.profile is None and samples == 0:
            return fail(
                f"{name}: delayed correction did not time resimulation", output,
            )
        frame_stats = [
            tuple(map(int, row)) for row in FRAME_STATS_RE.findall(output)
        ]
        if len(frame_stats) != 1:
            return fail(
                f"{name}: expected one authored-frame budget witness, "
                f"got {frame_stats!r}", output,
            )
        frame = frame_stats[0]
        frame_samples, frame_average, frame_p50, frame_p95, frame_p99, \
            frame_maximum = frame[:6]
        if frame_samples == 0 or frame_samples > authored or \
                frame_average == 0 or not (
                    frame_p50 <= frame_p95 <= frame_p99) or \
                frame_p99 > 8_333_333 or frame_maximum == 0 or \
                frame[6] != 0 or frame[8] != 0:
            return fail(f"{name}: authored-frame budget failed {frame!r}", output)
        transports = [
            tuple(map(int, row)) for row in TRANSPORT_RE.findall(output)
        ]
        if len(transports) != 1:
            return fail(
                f"{name}: expected one transport retirement witness, "
                f"got {transports!r}", output,
            )
        transport = transports[0]
        epoch = transport[0]
        accepted, _corrected, _duplicate = transport[1:4]
        invalid, stale, unauthorized, conflict, out_window = transport[4:9]
        drained, drain_rejected, transport_recovery = transport[9:12]
        takeover_started, takeover_ignored = transport[12:14]
        takeover_remote = args.ai_takeover_slot is not None and (
            int(local_mask, 0) & (1 << args.ai_takeover_slot)) == 0
        if epoch != 1 or accepted == 0 or drained == 0 or any((
                invalid, stale, unauthorized, conflict, out_window,
                drain_rejected)) or transport_recovery != int(recovery_expected) \
                or takeover_started != int(args.ai_takeover_slot is not None) \
                or ((not takeover_remote) !=
                    (takeover_ignored == 0)):
            return fail(
                f"{name}: transport safety/isolation witness was {transport!r}",
                output,
            )
        takeover_witness = (
            f"[NET-TAKEOVER] epoch=1 slot={args.ai_takeover_slot} "
            f"tick={args.ai_takeover_tick} policy=ai-no-handback"
        )
        if output.count(takeover_witness) != int(
                args.ai_takeover_slot is not None):
            return fail(f"{name}: wrong AI takeover witness", output)
        if args.ai_takeover_slot is not None and args.ai_takeover_tick == (
                TAKEOVER_INPUT_WITNESS_TICK):
            fixture_witness = (
                f"{TAKEOVER_AUTHORITY_WITNESS_TICK} A 4 "
                f"P{args.ai_takeover_slot + 1}"
            )
            if fixture_witness not in SCRIPT.read_text(encoding="utf-8").splitlines():
                return fail(
                    f"takeover fixture lost authored edge {fixture_witness!r}"
                )
            input_row = input_row_at(
                inputs[name], TAKEOVER_AUTHORITY_WITNESS_TICK,
            )
            if input_row is None or input_slot_sample(
                    input_row, args.ai_takeover_slot) != (1, 0, 0, 0, 0, 0):
                return fail(
                    f"{name}: exact-tick takeover did not replace authored "
                    f"P{args.ai_takeover_slot + 1} edge with present-neutral "
                    f"history at authority tick {TAKEOVER_AUTHORITY_WITNESS_TICK}",
                    output,
                )
        views = VIEW_RE.findall(output)
        if tuple(views) != expected_views:
            return fail(
                f"{name}: output/canonical view witnesses were {views!r}, "
                f"expected {expected_views!r}", output,
            )
        no_render_marker = "[NET-VIEW] endpoint=no-render hidden-world-passes=0"
        expected_no_render = 1 if name == "no-render" else 0
        if output.count(no_render_marker) != expected_no_render:
            return fail(
                f"{name}: expected no-render witness count {expected_no_render}",
                output,
            )
        lenses = LENS_RE.findall(output)
        expected_lenses = tuple(
            (view[0], view[1], view[2]) for view in expected_views
        )
        if tuple(lens[:3] for lens in lenses) != expected_lenses:
            return fail(
                f"{name}: output/canonical lens witnesses were {lenses!r}, "
                f"expected mappings {expected_lenses!r}", output,
            )
        if any(float(lens[3]) <= 0.0 or float(lens[4]) <= 0.0
               for lens in lenses):
            return fail(f"{name}: invalid mapped lens witness", output)
        hud = HUD_RE.findall(output)
        if expected_views:
            expected_hud_layout = expected_views[0][2]
            if len(hud) != 1 or hud[0][0:2] != ("3", expected_hud_layout):
                return fail(
                    f"{name}: endpoint minimap layout witness was {hud!r}, "
                    f"expected canonical 3/output {expected_hud_layout}",
                    output,
                )
        elif hud:
            return fail(f"{name}: no-render endpoint drew a minimap", output)
        hud_reflow = HUD_REFLOW_RE.findall(output)
        expected_reflow = tuple(
            (view[0], view[1], view[2]) for view in expected_views
        )
        if tuple(row[:3] for row in hud_reflow) != expected_reflow or any(
                row[3] == "0" and row[4] == "0" for row in hud_reflow):
            return fail(
                f"{name}: HUD reflow witnesses were {hud_reflow!r}, "
                f"expected moved/scaled shadows {expected_reflow!r}", output,
            )
        audio = AUDIO_RE.findall(output)
        expected_audio = (
            str(len(expected_views)), "4", viewport_map,
            "silent" if not expected_views else
            "spatial" if len(expected_views) == 1 else "shared-center",
        )
        if len(audio) != 1 or (
                audio[0][0], audio[0][1], audio[0][2] or "", audio[0][3]
        ) != expected_audio:
            return fail(
                f"{name}: endpoint listener witness was {audio!r}, "
                f"expected {expected_audio!r}", output,
            )
        if args.profile is not None:
            profiles = PROFILE_RE.findall(output)
            if len(profiles) != 1 or profiles[0][0] != args.profile:
                return fail(
                    f"{name}: missing/wrong named profile witness {profiles!r}",
                    output,
                )
            stats = tuple(int(value) for value in profiles[0][1:])
            profile_stats.append(stats)
            if stats[0] == 0 or stats[8] == 0 or stats[7] != 0:
                return fail(
                    f"{name}: vacuous/overflowing profile stats {profiles[0]!r}",
                    output,
                )
            recovery = RECOVERY_RE.findall(output)
            session_recovery = SESSION_RECOVERY_RE.findall(output)
            if recovery_expected:
                if len(recovery) != 1 or session_recovery != ["1"]:
                    return fail(
                        f"{name}: recovery handoff was not exact/launcher-owned "
                        f"network={recovery!r} session={session_recovery!r}",
                        output,
                    )
                _, _, first, observed = recovery[0]
                if int(first) >= int(observed) or (
                        int(observed) - int(first) < 31):
                    return fail(
                        f"{name}: recovery was not bounded by retained history",
                        output,
                    )
            elif recovery or session_recovery:
                return fail(
                    f"{name}: convergent profile entered recovery", output,
                )
    if len(manifest_digests) != 1:
        return fail(f"canonical manifests diverged: {manifest_digests!r}")

    if args.profile is not None:
        aggregate = tuple(map(sum, zip(*profile_stats, strict=True)))
        required = {
            "lan": (0, 8),
            "regional-good": (0, 2, 3, 8),
            "regional-variable": (0, 1, 2, 3, 8, 11, 12),
            "poor": (0, 1, 2, 3, 8, 11, 12),
            "two-second-outage": (0, 1, 5, 8, 13),
            "adversarial": (0, 1, 2, 3, 4, 8, 9, 11, 12, 13),
        }[args.profile]
        missing = tuple(index for index in required if aggregate[index] == 0)
        if missing:
            return fail(
                f"profile {args.profile} did not exercise counters {missing}: "
                f"aggregate={aggregate}"
            )

    baseline = ENDPOINTS[0][0]
    for name, *_ in ENDPOINTS[1:]:
        for label, rows in (("authority", authority), ("input", inputs),
                            ("canonical-event", canonical_events)):
            if recovery_expected:
                # All endpoints are byte-exact until the same post-race-start
                # disruption. Their recovery ticks may differ with endpoint
                # clocks, so divergent predicted suffixes are intentionally
                # not presented as convergence.
                common = PROFILE_AUTHORITY_START_TICK - 1
                comparison_matches = rows[name][:common] == rows[baseline][:common]
            elif args.profile is not None:
                if label == "authority":
                    comparison_matches = rows[name][-1:] == rows[baseline][-1:]
                elif label == "input":
                    comparison_matches = (
                        rows[name][-1].partition(" p1=")[2] ==
                        rows[baseline][-1].partition(" p1=")[2]
                    )
                else:
                    comparison_matches = rows[name][-32:] == rows[baseline][-32:]
            else:
                comparison_matches = rows[name] == rows[baseline]
            if name == "slot1" and args.profile is None:
                withheld = "[NET-INPUT-TEST] withheld slot=0 tick=131 deliver=135"
                delivered = "[NET-INPUT-TEST] delivered slot=0 tick=131 at=135"
                reconciled = (
                    "[ROLLBACK] online correction reconciled "
                    "ticks=131..135 depth=5"
                )
                if any(outputs[name].count(marker) != 1 for marker in
                       (withheld, delivered, reconciled)):
                    return fail(
                        f"{name}: delayed-input correction witness missing",
                        outputs[name],
                    )
                start = outputs[name][:outputs[name].index(withheld)].count(
                    "[SIMHASH]"
                )
                resume = outputs[name][:outputs[name].index(reconciled)].count(
                    "[SIMHASH]"
                )
                comparison_matches = (
                    rows[name][:start] == rows[baseline][:start] and
                    rows[name][resume:] == rows[baseline][resume:]
                )
            if not comparison_matches:
                limit = min(len(rows[name]), len(rows[baseline]))
                first = next((index for index in range(limit)
                              if rows[name][index] != rows[baseline][index]),
                             limit)
                context = ""
                if first < limit:
                    context = (
                        f"\n  baseline: {rows[baseline][first]}"
                        f"\n  endpoint: {rows[name][first]}"
                    )
                return fail(
                    f"{name}: {label} stream diverged at row {first}{context}"
                )
    # This is a meaningful no-render endpoint, not a label over four hidden
    # views: its endpoint-local feedback stream must differ once finish HUD
    # audio would be authored, while the canonical event projection above stays
    # identical.
    if args.profile is None and events["no-render"] == events[baseline]:
        return fail("no-render endpoint did not suppress any local feedback")

    if recovery_expected:
        takeover_text = (
            f" aiTakeover=slot{args.ai_takeover_slot}@{args.ai_takeover_tick}"
            if args.ai_takeover_slot is not None else ""
        )
        print(
            "PASS online process recovery: endpoints=4 "
            f"profile={args.profile} profileStart={PROFILE_AUTHORITY_START_TICK} "
            f"preFaultExactTicks={PROFILE_AUTHORITY_START_TICK - 1} "
            f"manifest={next(iter(manifest_digests))} "
            "bounded=31ticks engineUnwind=clean launcherScene=recovery "
            "error=connection-lost invalidRemoteView=rejected" + takeover_text
        )
    elif args.profile is not None:
        takeover_text = (
            f" aiTakeover=slot{args.ai_takeover_slot}@{args.ai_takeover_tick}"
            if args.ai_takeover_slot is not None else ""
        )
        print(
            "PASS online process convergence: endpoints=4 "
            f"ticks={run_ticks} manifest={next(iter(manifest_digests))} "
            f"profile={args.profile} profileStart={PROFILE_AUTHORITY_START_TICK} "
            f"inputRows={len(inputs[baseline])} "
            f"canonicalEventRows={len(canonical_events[baseline])} "
            "carrier=three-frame-bundle correction=named-profile "
            "endpointPresentation=witnessed invalidRemoteView=rejected" +
            takeover_text
        )
    else:
        takeover_text = (
            f" aiTakeover=slot{args.ai_takeover_slot}@{args.ai_takeover_tick}"
            if args.ai_takeover_slot is not None else ""
        )
        print(
            "PASS online process convergence: endpoints=4 "
            f"ticks={run_ticks} manifest={next(iter(manifest_digests))} "
            f"inputRows={len(inputs[baseline])} "
            f"canonicalEventRows={len(canonical_events[baseline])} "
            f"mappedPixels={width}x{height} dividerDark={divider_dark:.3f} "
            f"drawableGutter={one_seat_gutter:.3f} "
            "mappedLenses=compatible endpointMinimap=local endpointHud=reflowed "
            "endpointAudio=local-listeners noRenderWorldPasses=0 "
            "localFeedbackSuppressed=1 invalidRemoteView=rejected "
            "manifestRaceMismatch=rejected "
            "correction=5ticks" + takeover_text
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
