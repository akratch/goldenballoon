#!/usr/bin/env python3
"""The native app shell safely hands capped/uncapped rendering to the engine.

This enters through ``MDKR_APP_AUTOPLAY`` rather than the headless CLI.  The
launcher creates the production graphics context/device, the engine adopts it,
and a finite tick budget makes that handoff deterministic.  It protects both
the qualified WebGPU default and the explicit GL diagnostic path; the latter
previously crashed on macOS when a numeric limit or Uncapped selected its
interval-zero completion-fence path before GLAD had been initialized.
"""

from __future__ import annotations

import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ABORT_MARKERS, fatal_re, GPU_MARKERS,
                           present_mode_rows, resolve_binary, row_fields,
                           text_rows)


TICKS = 12
LAUNCHSERVICES_PROBE = (
    Path(__file__).resolve().parent.parent
    / "macos" / "Scripts" / "run_launchservices_probe.py"
)
PRESSURE_RE = {
    "gl": re.compile(r"\[GL-BACKPRESSURE\] (.*)"),
    "webgpu": re.compile(r"\[WGPU-BACKPRESSURE\] (.*)"),
}
REPLAY_RE = re.compile(r"\[REPLAY-SUMMARY\] (.*)")
RETAINED_RE = re.compile(r"\[RETAINED-TASK\] (.*)")
HOST_PRESENT_RE = re.compile(
    r"^\[APP-WGPU-PRESENT\] attempts=(\d+) presented=(\d+) "
    r"unavailable=(\d+) lastStatus=(-?\d+) encodeFailures=(\d+) "
    r"captureRequests=(\d+) captureFailures=(\d+)$",
    re.MULTILINE,
)
WGPU_OCCLUDED_STATUS = 0x00030001
WGPU_ERROR_STATUS = 6
FATAL_RE = fatal_re(*GPU_MARKERS)
CLEAN_FAILURE_FATAL_RE = fatal_re(*ABORT_MARKERS)


@dataclass(frozen=True)
class Backend:
    selector: str | None
    resolved: str
    host_marker: str
    adopted_marker: str

    @property
    def label(self) -> str:
        return self.selector or "default-webgpu"


BACKENDS = (
    Backend(
        None, "webgpu", "[app] host: WebGPU",
        "[SDL] adopted the app shell's WebGPU window",
    ),
    Backend(
        "gl", "gl", "[app] host: OpenGL",
        "[SDL] adopted the app shell's GL window",
    ),
)


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def validate_pressure(backend: Backend, policy: str,
                      row: str, host_presented: bool,
                      minimum_submissions: int = TICKS) -> None:
    fields = row_fields(row)
    minimum_admitted = minimum_submissions
    if backend.resolved == "webgpu":
        endpoint_skips = fields.get("endpointSkips", 0)
        if endpoint_skips < 0 or endpoint_skips > minimum_submissions:
            raise RuntimeError(
                f"{backend.label}/{policy}: impossible endpoint shedding: {row}")
        minimum_admitted -= endpoint_skips
    if fields.get("submitted", 0) < minimum_admitted:
        raise RuntimeError(f"{backend.label}/{policy}: too few submissions: {row}")
    if fields.get("completed") != fields.get("submitted"):
        raise RuntimeError(
            f"{backend.label}/{policy}: GPU work did not drain: {row}")
    if fields.get("failures") != 0 or fields.get("inflight") != 0:
        raise RuntimeError(
            f"{backend.label}/{policy}: backpressure failure: {row}")
    if backend.resolved == "gl" and fields.get("effectiveSwap") != 0:
        raise RuntimeError(
            f"{backend.label}/{policy}: interval-zero GL was not active: {row}")
    if backend.resolved == "webgpu":
        if (fields.get("abandoned") != 0 or
                fields.get("runtimewaits", 0) != 0 or
                fields.get("runtimewaitns", 0) != 0):
            raise RuntimeError(
                f"{backend.label}/{policy}: WebGPU blocked or abandoned "
                f"runtime work: {row}")
        if fields.get("skips", -1) != (
                fields.get("endpointSkips", -1) +
                fields.get("replaySkips", -1)):
            raise RuntimeError(
                f"{backend.label}/{policy}: WebGPU admission skips do not "
                f"reconcile by image class: {row}")
        if (fields.get("presented", 0) + fields.get("unavailable", 0) !=
                fields.get("submitted", 0)):
            raise RuntimeError(
                f"{backend.label}/{policy}: WebGPU surface outcomes do not "
                f"account for every submission: {row}")
        if host_presented:
            if fields.get("presented", 0) < minimum_admitted:
                raise RuntimeError(
                    f"{backend.label}/{policy}: too few surface presents: {row}")
        elif (fields.get("presented") != 0 or
              fields.get("unavailable", 0) == 0):
            raise RuntimeError(
                f"{backend.label}/{policy}: occluded run did not stay on the "
                f"bounded unavailable-surface path: {row}")


def validate_host_presentation(backend: Backend, policy: str,
                               output: str) -> bool:
    if backend.resolved != "webgpu":
        if "[app] autoplay: host surface ready presents=" not in output:
            raise RuntimeError(
                f"{backend.label}/{policy}: GL warm-up did not swap")
        return True

    rows = HOST_PRESENT_RE.findall(output)
    if len(rows) != 1:
        raise RuntimeError(
            f"{backend.label}/{policy}: expected one AppHost presentation "
            f"summary, got {len(rows)}")
    (attempts, presented, unavailable, last_status, encode_failures,
     capture_requests, capture_failures) = map(int, rows[0])
    if attempts < 1 or attempts != presented + unavailable or \
            encode_failures != 0 or capture_requests != 0 or \
            capture_failures != 0:
        raise RuntimeError(
            f"{backend.label}/{policy}: unhealthy AppHost summary: "
            f"{HOST_PRESENT_RE.search(output).group(0)}")
    if presented > 0:
        if "[app] autoplay: host surface ready presents=" not in output:
            raise RuntimeError(
                f"{backend.label}/{policy}: presented without ready marker")
        return True
    if (unavailable != attempts or last_status != WGPU_OCCLUDED_STATUS or
            "[app] autoplay: host surface occluded attempts=" not in output):
        raise RuntimeError(
            f"{backend.label}/{policy}: zero-present host was not explicitly "
            f"and exclusively occluded")
    return False


def explain_headless_window_server(output: str) -> str | None:
    """Name the likely cause when the AppHost surface never had a drawable.

    A locked screen or a headless/CI session with no active window server
    hands back zero drawables for every present attempt: attempts=N
    presented=0 unavailable=N, with N == attempts, in the run's one canonical
    telemetry row. validate_host_presentation already treats that shape as a
    legitimate (non-crashing) "no host-side present" outcome rather than
    raising, but a caller that then reports it with a generic missing-marker
    message sends debugging down the wrong path -- this codebase burned a full
    debugging session on exactly that before the distinction was added here.
    Detect the signature and say so plainly instead.
    """
    match = HOST_PRESENT_RE.search(output)
    if not match:
        return None
    attempts, presented, unavailable = (
        int(match.group(1)), int(match.group(2)), int(match.group(3)))
    if attempts > 0 and presented == 0 and unavailable == attempts:
        return (
            f"the window server provided no drawables (attempts={attempts} "
            f"presented=0 unavailable={attempts}) — active display session "
            "required for this arm; rerun when a session is available")
    return None


def host_telemetry(output: str, label: str) -> tuple[int, ...]:
    """Parse telemetry only; each outcome owns its accounting policy.

    An acquired surface whose view/encoder creation fails is intentionally
    neither presented nor unavailable.  Applying the ordinary balance rule
    here made that real failure shape impossible to test.
    """
    rows = HOST_PRESENT_RE.findall(output)
    if len(rows) != 1:
        raise RuntimeError(
            f"{label}: expected one canonical AppHost telemetry row, "
            f"got {len(rows)}")
    return tuple(map(int, rows[0]))


def require_clean_failure(label: str, returncode: int, output: str) -> None:
    """Accept only the engine's deliberate EXIT_FAILURE path, never a crash."""
    if returncode != 1:
        raise RuntimeError(
            f"{label}: expected clean EXIT_FAILURE (1), got {returncode}\n"
            f"{output[-5000:]}")
    fatal = CLEAN_FAILURE_FATAL_RE.search(output)
    if fatal:
        raise RuntimeError(f"{label}: emitted fatal marker {fatal.group(0)!r}")


def require_overlay_renderer_stop(label: str, output: str) -> None:
    """Require the deliberate renderer stop at either legal engine boundary.

    An overlay failure can be observed immediately after the graphics task or
    at the following cooperative retrace, depending on whether a game display
    list was submitted in that boot phase. Both paths stop before another
    simulation ticket; neither is a crash or a silently dropped overlay.
    """
    markers = (
        "renderer entered an unrecoverable state; stopping before game state advances",
        "renderer entered an unrecoverable state; stopping at the frame boundary",
        "renderer entered an unrecoverable state during resource preparation; "
        "stopping at the retrace boundary",
    )
    if not any(marker in output for marker in markers):
        raise RuntimeError(
            f"{label}: missing deliberate renderer-stop boundary\n"
            f"{output[-5000:]}")


def stage_macos_sdl2(bundle: Path, executable: Path) -> None:
    """Make the focused LaunchServices fixture self-contained and sealed.

    Release builds link the pinned standalone SDL2 through ``@rpath``. A bare
    copy of that executable can make dyld probe the developer build directory;
    LaunchServices may block that Desktop-folder access before ``main``. The
    production bundle rewrites the dependency into ``Contents/Frameworks``.
    Mirror that exact boundary here so this is a packaged-app test, not an
    accidental test of the build tree's permissions.

    Developer machines may resolve ``sdl2`` to Homebrew's ``sdl2-compat``
    shim. Unlike the release dependency (which packaging deliberately requires
    to be standalone), that shim loads SDL3 at runtime rather than recording it
    as a Mach-O dependency. Stage SDL3 beside the shim for this local fixture;
    otherwise sdl2-compat presents an AppKit error dialog from its initializer
    before ``main`` and a noninteractive qualification run merely times out.
    """
    linked = subprocess.run(
        ["otool", "-L", str(executable)], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout
    load_paths = sorted({
        line.strip().split(" (", 1)[0]
        for line in linked.splitlines()[1:]
        if "libSDL2" in line and ".dylib" in line
    })
    if len(load_paths) != 1:
        raise RuntimeError(
            f"FPS overlay fixture expected one SDL2 dependency, got {load_paths}")

    commands = subprocess.run(
        ["otool", "-l", str(executable)], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.splitlines()
    rpaths: list[Path] = []
    in_rpath = False
    for line in commands:
        fields = line.strip().split()
        if fields[:2] == ["cmd", "LC_RPATH"]:
            in_rpath = True
        elif in_rpath and fields[:1] == ["path"]:
            rpaths.append(Path(fields[1]))
            in_rpath = False

    load_path = load_paths[0]
    if load_path.startswith("@rpath/"):
        relative = load_path.removeprefix("@rpath/")
        candidates = [path / relative for path in rpaths if path.is_absolute()]
    elif Path(load_path).is_absolute():
        candidates = [Path(load_path)]
    else:
        candidates = []
    sources = [path for path in candidates if path.is_file()]
    if len(sources) != 1:
        raise RuntimeError(
            "FPS overlay fixture could not resolve its SDL2 dependency: "
            f"load={load_path!r} candidates={candidates}")

    frameworks = bundle / "Contents" / "Frameworks"
    frameworks.mkdir()
    bundled_sdl = frameworks / sources[0].name
    shutil.copy2(sources[0], bundled_sdl)
    nested_code = [bundled_sdl]
    if b"sdl2-compat:" in bundled_sdl.read_bytes():
        sdl3_libdir = subprocess.run(
            ["pkg-config", "--variable=libdir", "sdl3"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout.strip()
        sdl3_source = Path(sdl3_libdir) / "libSDL3.dylib"
        if not sdl3_source.is_file():
            raise RuntimeError(
                "FPS overlay fixture resolved sdl2-compat but could not "
                f"locate its SDL3 runtime at {sdl3_source}")
        bundled_sdl3 = frameworks / "libSDL3.dylib"
        shutil.copy2(sdl3_source, bundled_sdl3)
        nested_code.append(bundled_sdl3)
    bundled_load_path = f"@executable_path/../Frameworks/{bundled_sdl.name}"
    subprocess.run(
        ["install_name_tool", "-change", load_path, bundled_load_path,
         str(executable)],
        check=True,
    )
    for rpath in rpaths:
        if rpath.is_absolute():
            subprocess.run(
                ["install_name_tool", "-delete_rpath", str(rpath),
                 str(executable)],
                check=True,
            )

    # install_name_tool invalidates the linker's signature. Remove copied
    # metadata, sign nested code first, then seal the finished outer bundle.
    subprocess.run(["xattr", "-cr", str(bundle)], check=True)
    for nested_path in nested_code:
        subprocess.run(
            ["codesign", "--force", "--sign", "-", str(nested_path)],
            check=True,
        )
    subprocess.run(
        ["codesign", "--force", "--sign", "-", str(bundle)], check=True)
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(bundle)],
        check=True,
    )


def validate_present_mode(backend: Backend, requested: str,
                          output: str) -> None:
    rows = present_mode_rows(output)
    if len(rows) != 1:
        raise RuntimeError(
            f"{backend.label}/{requested}: expected one native presentation "
            f"mode row, got {len(rows)}")
    row = rows[0]
    details = " ".join(f"{key}={value}" for key, value in row.items())

    expected = (backend.resolved, "capped", "240")
    if requested == "uncapped":
        expected = (backend.resolved, "uncapped", "0")
    actual = (row.get("backend"), row.get("policy"), row.get("rate"))
    if actual != expected:
        raise RuntimeError(
            f"{backend.label}/{requested}: presentation policy resolved as "
            f"{actual}, expected {expected}")
    if row.get("tearing") != "0":
        raise RuntimeError(
            f"{backend.label}/{requested}: presentation reported a tearing "
            f"opt-in nothing here asked for: {details}")
    if backend.resolved == "webgpu":
        # A rate the display cannot show asks for the queue that replaces an
        # undisplayed image; anything it can show is paced exactly by the
        # deadline grid on the blocking queue. Mailbox is not advertised
        # everywhere, so FIFO is the permitted fallback — and immediate stays
        # unreachable without the opt-in ruled out above.
        display_hz = row_fields(details).get("displayHz", 0)
        above_display = (requested == "uncapped" or
                         (display_hz and int(requested) > display_hz))
        want = "mailbox" if above_display else "fifo"
        if row.get("requested") != want:
            raise RuntimeError(
                f"{backend.label}/{requested}: policy did not request the "
                f"{want} queue at displayHz={display_hz}: {details}")
        if row.get("effective") not in ("mailbox", "fifo"):
            raise RuntimeError(
                f"{backend.label}/{requested}: WebGPU resolved a present mode "
                f"that is neither mailbox nor FIFO: {details}")
        # Swap-chain depth is part of the same configuration and is the half of
        # it a player feels rather than sees: every extra queued image is a
        # refresh of lag between the stick and the kart. It must be the
        # backend's minimum on every configure, because a re-configure that
        # drops the extras chain silently restores the two-deep default.
        #
        # TWO SEPARATE WITNESSES, DELIBERATELY. This [PRESENT-MODE] row is the
        # ENGINE's configure and nothing else — the row is emitted by the
        # present-mode ranking, which only the engine performs. The launcher's
        # adopted-window configure hardcodes FIFO, ranks nothing, and so emits
        # no such row; asserting only here would check the engine twice while
        # reading as if it covered both. The adopted window reports itself on
        # [SURFACE-CONFIG] and is asserted below.
        if row.get("frameLatency") != "1":
            raise RuntimeError(
                f"{backend.label}/{requested}: the engine configured the "
                f"surface with frameLatency={row.get('frameLatency')}, "
                f"expected the pinned minimum 1: {details}")
        shell_rows = [shell for shell in text_rows(output, "SURFACE-CONFIG")
                      if shell.get("owner") == "app-shell"]
        if not shell_rows:
            raise RuntimeError(
                f"{backend.label}/{requested}: the adopted WebGPU window "
                "configured no surface — expected at least one "
                "[SURFACE-CONFIG] owner=app-shell row")
        for shell in shell_rows:
            if shell.get("frameLatency") != "1":
                shell_details = " ".join(
                    f"{key}={value}" for key, value in shell.items())
                raise RuntimeError(
                    f"{backend.label}/{requested}: the app shell configured "
                    f"the adopted surface with "
                    f"frameLatency={shell.get('frameLatency')}, expected the "
                    f"pinned minimum 1: {shell_details}")


def run_policy(binary: Path, rom: Path, backend: Backend, policy: str,
               timeout: int, verbose: bool,
               stage_via_launcher: bool = False) -> None:
    source = "launcher-stage" if stage_via_launcher else "environment"
    with tempfile.TemporaryDirectory(
            prefix=f"mdkr64_app_{backend.label}_{policy}_{source}_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
        )
        if stage_via_launcher:
            env["MDKR_APP_AUTOPLAY_VIDEO_SET"] = f"Video.FrameLimit={policy}"
            env["MDKR_APP_PANEL"] = "Settings"
            env["MDKR_APP_UI_TRACE"] = "1"
        else:
            env["MDKR_PRESENT_RATE"] = policy
            env["MDKR_PRESENT_SMOOTHING"] = "interpolate"
            env["MDKR_PRESENT_SCHED_TRACE"] = "1"
        if backend.selector is not None:
            env["MDKR_RENDERER"] = backend.selector
        command = [str(binary)]
        if verbose:
            print(f"$ ({backend.label}/{policy}) {' '.join(command)}", flush=True)
        process = subprocess.run(
            command, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            raise RuntimeError(
                f"{backend.label}/{policy}: exit {process.returncode}\n"
                f"{output[-5000:]}")
        fatal = FATAL_RE.search(output)
        if fatal:
            raise RuntimeError(
                f"{backend.label}/{policy}: emitted {fatal.group(0)!r}")
        required = (
            backend.host_marker,
            backend.adopted_marker,
            f"[app] boot: --rom {rom} --headless-ticks {TICKS}",
            f"[mdkr64] renderer backend: {backend.resolved}",
        )
        for marker in required:
            if marker not in output:
                raise RuntimeError(
                    f"{backend.label}/{policy}: missing {marker!r}")
        if stage_via_launcher:
            # APPLIED, not "staged", and restartPending=0. Video.FrameLimit
            # became SCOPE_LIVE: the launcher no longer holds the value back
            # for the next launch, and the panel no longer has a restart to
            # announce. The claim this arm makes is unchanged and is checked
            # by the presentation rows below -- a value chosen in the launcher
            # and never placed in the environment reaches the engine's own
            # pacing -- but the words the panel uses to say so are the LIVE
            # ones now, and asserting the RESTART ones would be asserting that
            # a fixed restart requirement came back.
            for marker in (
                "[app] autoplay video setting applied: "
                f"Video.FrameLimit={policy}",
                "[app-ui] settings action=play restartPending=0",
                f"[app-ui] frame-limit value={policy} label={policy} Hz "
                "restartPending=0",
            ):
                if marker not in output:
                    raise RuntimeError(
                        f"{backend.label}/{policy}: staged Settings UI missing "
                        f"{marker!r}")

        host_presented = validate_host_presentation(backend, policy, output)
        validate_present_mode(backend, policy, output)
        rows = PRESSURE_RE[backend.resolved].findall(output)
        if len(rows) != 1:
            raise RuntimeError(
                f"{backend.label}/{policy}: expected one backpressure row, "
                f"got {len(rows)}")
        validate_pressure(backend, policy, rows[0], host_presented)
        if not stage_via_launcher:
            replay_rows = REPLAY_RE.findall(output)
            retained_rows = RETAINED_RE.findall(output)
            if len(replay_rows) != 1 or len(retained_rows) != 1:
                raise RuntimeError(
                    f"{backend.label}/{policy}: missing public interpolation "
                    "telemetry")
            replay = row_fields(replay_rows[0])
            retained = row_fields(retained_rows[0])
            pressure = row_fields(rows[0])
            completed_or_shed = replay.get("walks", 0) > 0
            if backend.resolved == "webgpu":
                completed_or_shed = completed_or_shed or (
                    pressure.get("replaySkips", 0) > 0 and
                    retained.get("acquires", 0) >=
                    pressure.get("replaySkips", 0) and
                    replay.get("restores", 0) >=
                    pressure.get("replaySkips", 0))
            if (not completed_or_shed or
                    replay.get("freezefail", -1) != 0 or
                    replay.get("restorefail", -1) != 0 or
                    retained.get("captures", 0) <= 0 or
                    retained.get("failures", -1) != 0 or
                    retained.get("rejects", -1) != 0):
                raise RuntimeError(
                    f"{backend.label}/{policy}: public immutable replay was "
                    f"not healthy: replay={replay} retained={retained}")
        if verbose:
            print(f"  PASS {rows[0]}")


def run_borrowed_configure_failure(binary: Path, rom: Path, timeout: int,
                                   verbose: bool) -> None:
    """A borrowed surface failure must drain without replacing host roots."""
    with tempfile.TemporaryDirectory(prefix="mdkr64_app_borrowed_fault_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_WEBGPU_FAULT="surface.configure",
        )
        if verbose:
            print(f"$ (borrowed-surface/configure-fault) {binary}", flush=True)
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(
            "borrowed-surface/configure-fault", process.returncode, output)
        for marker in (
            "[SDL] adopted the app shell's WebGPU window",
            "[webgpu-fault] injected surface.configure@1",
            "renderer entered an unrecoverable state; stopping before game state advances",
            "[WGPU-SHUTDOWN] roots=borrowed",
            "liveChildren=0 cpuArrays=0",
            "[SDL-SHUTDOWN] window=host glContext=host controllers=0 sdl=host",
            "[APP-WGPU-PRESENT]",
        ):
            if marker not in output:
                raise RuntimeError(
                    f"borrowed-surface/configure-fault: missing {marker!r}\n"
                    f"{output[-5000:]}")
        for forbidden in (
            "attempting one native device reinitialization",
            "native device recovery succeeded",
            "switched to OpenGL",
            "[SDL] GL ready:",
            "AddressSanitizer",
            "UndefinedBehaviorSanitizer",
            "runtime error:",
        ):
            if forbidden in output:
                raise RuntimeError(
                    f"borrowed-surface/configure-fault: emitted {forbidden!r}")
        validate_host_presentation(BACKENDS[0], "configure-fault", output)
        if verbose:
            print("  PASS borrowed roots drained without fallback or replacement")


def run_borrowed_device_loss(binary: Path, rom: Path, timeout: int,
                             verbose: bool) -> None:
    """AppHost replaces failed borrowed roots and gameplay resumes on WebGPU."""
    with tempfile.TemporaryDirectory(prefix="mdkr64_app_borrowed_loss_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_WEBGPU_FAULT="device.lost",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            raise RuntimeError(
                "borrowed-device/loss: recovery did not complete cleanly "
                f"(exit {process.returncode})\n{output[-5000:]}")
        fatal = CLEAN_FAILURE_FATAL_RE.search(output)
        if fatal:
            raise RuntimeError(
                "borrowed-device/loss: emitted fatal marker "
                f"{fatal.group(0)!r}")
        for marker in (
            "[SDL] adopted the app shell's WebGPU window",
            "[webgpu-fault] injected device.lost@1",
            "[webgpu] device lost",
            "attempting one native device reinitialization (AppHost-owned roots)",
            "[app] replacement WebGPU roots prepared",
            "[app] replacement WebGPU roots committed transactionally",
            "[webgpu] native device recovery succeeded; gameplay continues",
            f"[SDL] headless: reached {TICKS} simulation ticks, exiting cleanly.",
            "[WGPU-SHUTDOWN] roots=borrowed",
            "liveChildren=0 cpuArrays=0",
            "[APP-WGPU-PRESENT]",
        ):
            if marker not in output:
                raise RuntimeError(
                    f"borrowed-device/loss: missing {marker!r}\n{output[-5000:]}")
        for forbidden in (
            "renderer entered an unrecoverable state",
            "recovery failed; terminating cleanly",
            "durable WebGPU recovery error",
            "switched to OpenGL",
            "[SDL] GL ready:",
        ):
            if forbidden in output:
                raise RuntimeError(
                    f"borrowed-device/loss: emitted {forbidden!r}")
        host_presented = validate_host_presentation(
            BACKENDS[0], "borrowed-device-loss", output)
        rows = PRESSURE_RE["webgpu"].findall(output)
        if len(rows) != 1:
            raise RuntimeError(
                "borrowed-device/loss: expected one post-recovery WebGPU "
                f"pressure row, got {len(rows)}")
        validate_pressure(
            BACKENDS[0], "borrowed-device-loss", rows[0], host_presented,
            minimum_submissions=TICKS - 1)
        if verbose:
            print("  PASS borrowed device loss recreated AppHost roots and "
                  "resumed WebGPU presentation")


def run_borrowed_second_device_loss(binary: Path, rom: Path, timeout: int,
                                    verbose: bool) -> None:
    """A second loss after recovery stops with a durable launcher error."""
    with tempfile.TemporaryDirectory(
            prefix="mdkr64_app_borrowed_second_loss_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_WEBGPU_FAULT="device.lost@all",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(
            "borrowed-device/second-loss", process.returncode, output)
        for marker in (
            "[webgpu-fault] injected device.lost@1",
            "[webgpu-fault] injected device.lost@2",
            "[app] replacement WebGPU roots committed transactionally",
            "[webgpu] native device recovery succeeded; gameplay continues",
            "[webgpu] native recovery already attempted; stopping after the "
            "repeated device failure",
            "recovery failed; terminating cleanly without an automatic "
            "OpenGL fallback",
            "[app] durable WebGPU recovery error:",
            "[WGPU-SHUTDOWN] roots=borrowed",
            "liveChildren=0 cpuArrays=0",
        ):
            if marker not in output:
                raise RuntimeError(
                    "borrowed-device/second-loss: missing "
                    f"{marker!r}\n{output[-7000:]}")
        if output.count(
                "[app] replacement WebGPU roots committed transactionally") != 1:
            raise RuntimeError(
                "borrowed-device/second-loss: replacement roots were not "
                "committed exactly once")
        for forbidden in (
            "switched to OpenGL",
            "[SDL] GL ready:",
            "AddressSanitizer",
            "UndefinedBehaviorSanitizer",
            "runtime error:",
        ):
            if forbidden in output:
                raise RuntimeError(
                    "borrowed-device/second-loss: emitted "
                    f"{forbidden!r}")
        if verbose:
            print("  PASS repeated borrowed loss stopped with a durable "
                  "launcher error")


def run_borrowed_candidate_callback_failure(
        binary: Path, rom: Path, timeout: int, verbose: bool) -> None:
    """A replacement lost before root commit is rejected transactionally."""
    with tempfile.TemporaryDirectory(
            prefix="mdkr64_app_borrowed_candidate_loss_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_WEBGPU_FAULT="device.lost",
            MDKR_TEST_WEBGPU_RECOVERY_FAIL="candidate-callback",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(
            "borrowed-device/candidate-callback", process.returncode, output)
        for marker in (
            "[webgpu-fault] injected device.lost@1",
            "[webgpu] candidate device failed before root transaction commit "
            "(event=2)",
            "[app] replacement WebGPU root preparation failed",
            "recovery failed; terminating cleanly without an automatic "
            "OpenGL fallback",
            "[app] durable WebGPU recovery error:",
            "[WGPU-SHUTDOWN] roots=borrowed",
            "liveChildren=0 cpuArrays=0",
        ):
            if marker not in output:
                raise RuntimeError(
                    "borrowed-device/candidate-callback: missing "
                    f"{marker!r}\n{output[-7000:]}")
        for forbidden in (
            "replacement WebGPU roots committed transactionally",
            "native device recovery succeeded; gameplay continues",
            "switched to OpenGL",
            "[SDL] GL ready:",
        ):
            if forbidden in output:
                raise RuntimeError(
                    "borrowed-device/candidate-callback: emitted "
                    f"{forbidden!r}")
        if verbose:
            print("  PASS early replacement callback rejected before root "
                  "publication")


def run_borrowed_device_recovery_failure(
        binary: Path, rom: Path, timeout: int, verbose: bool) -> None:
    """A failed AppHost recovery exits visibly without GL or ownership damage."""
    with tempfile.TemporaryDirectory(
            prefix="mdkr64_app_borrowed_loss_fail_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_AUDIO="0",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_WEBGPU_FAULT="device.lost",
            MDKR_TEST_WEBGPU_RECOVERY_FAIL="1",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(
            "borrowed-device/recovery-failure", process.returncode, output)
        for marker in (
            "[webgpu] device lost",
            "[webgpu] injected native recovery failure",
            "recovery failed; terminating cleanly without an automatic OpenGL fallback",
            "[app] durable WebGPU recovery error:",
            "[WGPU-SHUTDOWN] roots=borrowed",
            "liveChildren=0 cpuArrays=0",
            "[APP-WGPU-PRESENT]",
        ):
            if marker not in output:
                raise RuntimeError(
                    "borrowed-device/recovery-failure: missing "
                    f"{marker!r}\n{output[-5000:]}")
        for forbidden in (
            "replacement WebGPU roots committed transactionally",
            "native device recovery succeeded",
            "switched to OpenGL",
            "[SDL] GL ready:",
        ):
            if forbidden in output:
                raise RuntimeError(
                    "borrowed-device/recovery-failure: emitted "
                    f"{forbidden!r}")
        validate_host_presentation(
            BACKENDS[0], "borrowed-recovery-failure", output)
        if verbose:
            print("  PASS borrowed recovery failure stayed WebGPU and surfaced "
                  "a durable AppHost error")


def run_host_fault(binary: Path, point: str, required: tuple[str, ...],
                   timeout: int, verbose: bool) -> None:
    """AppHost surface failures stop visibly without changing renderer."""
    with tempfile.TemporaryDirectory(prefix="mdkr64_app_host_fault_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_SMOKE_FRAMES="2",
            MDKR_AUDIO="0",
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_WEBGPU_FAULT=point,
        )
        if point == "host.surface-view":
            env["MDKR_TEST_OCCLUDED_SURFACE_FAULTS"] = "1"
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(point, process.returncode, output)
        for marker in (f"[webgpu-fault] injected {point}@1", *required):
            if marker not in output:
                raise RuntimeError(f"{point}: missing {marker!r}\n{output[-5000:]}")
        for forbidden in ("[app] host: OpenGL", "[SDL] GL ready:"):
            if forbidden in output:
                raise RuntimeError(f"{point}: silently selected GL")
        (attempts, presented, unavailable, last_status, encode_failures,
         capture_requests, capture_failures) = host_telemetry(output, point)
        if point == "host.surface-configure":
            healthy_accounting = (
                attempts, presented, unavailable, last_status,
                encode_failures) == (0, 0, 0, -1, 0)
        elif point == "host.surface-acquire":
            healthy_accounting = (
                attempts, presented, unavailable, last_status,
                encode_failures) == (1, 0, 1, WGPU_ERROR_STATUS, 0)
        else:
            actual_surface_failure = (
                attempts == 1 and presented == 0 and unavailable == 0 and
                last_status in (1, 2) and encode_failures == 1)
            occluded_ci_failure = (
                attempts == 1 and presented == 0 and unavailable == 1 and
                last_status == WGPU_OCCLUDED_STATUS and encode_failures == 1 and
                "[app-test] no surface drawable; injecting configured "
                "host surface-view failure" in output)
            healthy_accounting = actual_surface_failure or occluded_ci_failure
        if not healthy_accounting:
            raise RuntimeError(
                f"{point}: unexpected presentation accounting "
                f"{(attempts, presented, unavailable, last_status, encode_failures)}")
        if capture_requests != 0 or capture_failures != 0:
            raise RuntimeError(
                f"{point}: fault shutdown reported an unrelated capture failure")
        if verbose:
            print(f"  PASS {point} failed visibly without fallback")


def run_host_imgui_init_fault(binary: Path, timeout: int, verbose: bool) -> None:
    """A partial ImGui renderer init drains every already-created child/root."""
    point = "host.imgui-init-resource"
    with tempfile.TemporaryDirectory(prefix="mdkr64_app_imgui_init_fault_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_SMOKE_FRAMES="1",
            MDKR_AUDIO="0",
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_WEBGPU_FAULT=point,
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(point, process.returncode, output)
        for marker in (
            f"[webgpu-fault] injected {point}@1",
            "[app] ImGui WebGPU uniform-buffer creation failed",
            "[app] gfx_webgpu_imgui_init failed",
        ):
            if marker not in output:
                raise RuntimeError(f"{point}: missing {marker!r}\n{output[-5000:]}")
        if "[app] host: OpenGL" in output or "[SDL] GL ready:" in output:
            raise RuntimeError(f"{point}: silently selected GL")
        if host_telemetry(output, point) != (0, 0, 0, -1, 0, 0, 0):
            raise RuntimeError(f"{point}: startup failure reported frame activity")
        if verbose:
            print(f"  PASS {point} drained partial initialization")


def run_host_capture_fault(binary: Path, point: str, timeout: int,
                           verbose: bool) -> None:
    """A capture/render fault fails the shell instead of accepting a blank BMP."""
    with tempfile.TemporaryDirectory(prefix="mdkr64_app_capture_fault_") as temp:
        root = Path(temp)
        shot = root / "fault.bmp"
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_SMOKE_FRAMES="1",
            MDKR_APP_SMOKE_SHOT=str(shot),
            MDKR_AUDIO="0",
            MDKR_APP_PREFS_DIR=str(root / "prefs"),
            MDKR_WEBGPU_FAULT=f"{point}@all",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        require_clean_failure(point, process.returncode, output)
        for marker in (
            f"[webgpu-fault] injected {point}@1",
            "[app] smoke: requested capture",
        ):
            if marker not in output:
                raise RuntimeError(f"{point}: missing {marker!r}\n{output[-5000:]}")
        if shot.exists():
            raise RuntimeError(f"{point}: accepted a capture after renderer failure")
        if "[app] host: OpenGL" in output or "[SDL] GL ready:" in output:
            raise RuntimeError(f"{point}: silently selected GL")

        (attempts, presented, unavailable, _last_status, encode_failures,
         capture_requests, capture_failures) = host_telemetry(output, point)
        if attempts != 1 or capture_requests != 1 or capture_failures != 1:
            raise RuntimeError(
                f"{point}: unexpected capture accounting "
                f"{(attempts, presented, unavailable, encode_failures, capture_requests, capture_failures)}")
        if point == "host.capture-map-timeout":
            timeout_markers = (
                "[app] capture map timed out; callback remains safely owned",
                "[app] capture map drain: cancelling pending request",
                "[app] capture map drain: callback resolved status=",
                "retained buffer released",
            )
            if encode_failures != 0 or any(
                    marker not in output for marker in timeout_markers):
                raise RuntimeError(f"{point}: did not isolate the async map timeout")
        elif encode_failures < 1:
            raise RuntimeError(f"{point}: blank ImGui frame did not fail closed")
        if verbose:
            print(f"  PASS {point} rejected the capture and drained cleanly")


def launch_macos_app(binary: Path, rom: Path, root: Path, timeout: int,
                     fault: str | None = None) -> str:
    """Launch a minimal real app bundle so CAMetalLayer gets a drawable.

    Direct command-line launches are legitimately reported as Occluded by
    macOS.  This focused gate uses LaunchServices, just like a player opening
    the release bundle, so it can prove that an FPS-only overlay reaches an
    actual WebGPU render pass rather than merely advancing UI state.
    """
    bundle = root / "mdkr64-fps-probe.app"
    contents = bundle / "Contents"
    executable_dir = contents / "MacOS"
    resources = contents / "Resources"
    executable_dir.mkdir(parents=True)
    resources.mkdir()
    bundled_binary = executable_dir / "mdkr64"
    bundled_rom = resources / "validation.z64"
    shutil.copy2(binary, bundled_binary)
    # A newly registered test bundle must not trigger macOS Desktop-folder
    # privacy consent. Keeping the test ROM inside its own bundle also mirrors
    # the release app's unrestricted access to its packaged Resources tree.
    shutil.copyfile(rom, bundled_rom)
    with (contents / "Info.plist").open("wb") as stream:
        plistlib.dump(
            {
                "CFBundleDevelopmentRegion": "en",
                "CFBundleExecutable": "mdkr64",
                "CFBundleIdentifier": "com.mdkr64.fps-overlay-probe",
                "CFBundleInfoDictionaryVersion": "6.0",
                "CFBundleName": "mdkr64 FPS overlay probe",
                "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": "1.0.1",
                "CFBundleVersion": "1.0.1",
                "LSMinimumSystemVersion": "13.0",
                "NSHighResolutionCapable": True,
            },
            stream,
            sort_keys=True,
        )
    stage_macos_sdl2(bundle, bundled_binary)

    stdout_path = root / "launch.stdout"
    stderr_path = root / "launch.stderr"
    prefs = root / "prefs"
    prefs.mkdir()
    launch_env = {
        "LC_ALL": "C",
        "MDKR_APP_AUTOPLAY": "1",
        "MDKR_APP_AUTOPLAY_TICKS": str(TICKS),
        "MDKR_APP_FPS_TEST": "1",
        "MDKR_AUDIO": "0",
        "MDKR_PRESENT_RATE": "240",
        # Never consult the developer/player's real launcher preferences. A
        # saved ROM path can point into a macOS privacy-protected directory and
        # block this noninteractive LaunchServices process on a consent sheet.
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_ROM": str(bundled_rom),
        "MDKR_SAVE_DIR": str(root / "save"),
        "MDKR_VIDEO_CONFIG_PATH": str(root / "mdkr64.ini"),
    }
    if fault is not None:
        launch_env["MDKR_WEBGPU_FAULT"] = fault
    command = [
        str(LAUNCHSERVICES_PROBE),
        "--timeout", str(min(timeout, 600)),
        "--executable", str(bundled_binary),
        "--stdout", str(stdout_path),
        "--stderr", str(stderr_path),
        "--work-dir", str(root),
    ]
    for key, value in launch_env.items():
        command.extend(("--env", f"{key}={value}"))
    command.append(str(bundle))
    process = subprocess.run(
        command,
        cwd=root,
        env=clean_environment(LC_ALL="C"),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    launcher_output = process.stdout or ""
    output = launcher_output
    for path in (stdout_path, stderr_path):
        if path.is_file():
            output += path.read_text(encoding="utf-8", errors="replace")
    if process.returncode != 0:
        raise RuntimeError(
            f"LaunchServices failed for FPS overlay probe: "
            f"exit {process.returncode}\n{output[-5000:]}")
    return output


def run_fps_only_overlay(binary: Path, rom: Path, timeout: int,
                         verbose: bool) -> None:
    """F10/FPS-only UI must render on WebGPU without capturing game input."""
    if sys.platform != "darwin":
        if verbose:
            print("  SKIP FPS-only drawable proof (macOS release gate)")
        return

    with tempfile.TemporaryDirectory(prefix="mdkr64_app_fps_overlay_") as temp:
        output = launch_macos_app(binary, rom, Path(temp), timeout)
        required = (
            "[app] host: WebGPU",
            "[SDL] adopted the app shell's WebGPU window",
            "[overlay-test] FPS-only state wantsRender=1 wantsInput=0",
            "[overlay-test] FPS-only WebGPU pass rendered",
            "[WGPU-SHUTDOWN] roots=borrowed",
        )
        # Ask the drawable question FIRST. "[overlay-test] FPS-only WebGPU
        # pass rendered" is emitted from inside the render pass, so a session
        # with no window server loses that marker as a CONSEQUENCE of having no
        # drawable. Reporting the missing marker sends debugging into the
        # overlay code, which is the wrong place and is the exact mistake
        # explain_headless_window_server() was written to prevent -- it was
        # simply consulted too late to cover this arm.
        starved = explain_headless_window_server(output)
        if starved:
            raise RuntimeError(f"FPS-only WebGPU overlay: {starved}")
        for marker in required:
            if marker not in output:
                raise RuntimeError(
                    f"FPS-only WebGPU overlay: missing {marker!r}\n"
                    f"{output[-5000:]}")
        for forbidden in (
            "[app] host: OpenGL", "[SDL] GL ready:",
            "[webgpu] ImGui overlay rendering failed",
        ):
            if forbidden in output:
                raise RuntimeError(
                    f"FPS-only WebGPU overlay: emitted {forbidden!r}")
        fatal = FATAL_RE.search(output)
        if fatal:
            raise RuntimeError(
                f"FPS-only WebGPU overlay: emitted {fatal.group(0)!r}")
        host_presented = validate_host_presentation(
            BACKENDS[0], "fps-only", output)
        if not host_presented:
            cause = explain_headless_window_server(output)
            if cause:
                raise RuntimeError(f"FPS-only WebGPU overlay: {cause}")
            raise RuntimeError(
                "FPS-only WebGPU overlay did not obtain a macOS drawable")
        rows = PRESSURE_RE["webgpu"].findall(output)
        if len(rows) != 1:
            raise RuntimeError(
                "FPS-only WebGPU overlay: expected one backpressure row, "
                f"got {len(rows)}")
        validate_pressure(BACKENDS[0], "fps-only", rows[0], True)
        if verbose:
            print("  PASS FPS-only UI rendered through a real WebGPU pass "
                  "without input capture or GL fallback")


def run_native_overlay_fault(binary: Path, rom: Path, point: str,
                             timeout: int, verbose: bool) -> None:
    """A requested native overlay may not disappear behind a successful frame."""
    if sys.platform != "darwin":
        return
    with tempfile.TemporaryDirectory(
            prefix=f"mdkr64_app_{point.replace('.', '_')}_") as temp:
        output = launch_macos_app(binary, rom, Path(temp), timeout, point)
        required = (
            f"[webgpu-fault] injected {point}@1",
            "[overlay-test] FPS-only state wantsRender=1 wantsInput=0",
            "[webgpu] ImGui overlay rendering failed",
            "[WGPU-SHUTDOWN] roots=borrowed",
        )
        # Same ordering as the FPS-only arm: this arm's markers also come from
        # inside a render pass, so name a drawable-starved session before
        # blaming the fault-injection path.
        starved = explain_headless_window_server(output)
        if starved:
            raise RuntimeError(f"{point}: {starved}")
        for marker in required:
            if marker not in output:
                raise RuntimeError(
                    f"{point}: missing {marker!r}\n{output[-5000:]}")
        require_overlay_renderer_stop(point, output)
        if CLEAN_FAILURE_FATAL_RE.search(output):
            raise RuntimeError(f"{point}: renderer failure became a process crash")
        for forbidden in (
            "[overlay-test] FPS-only WebGPU pass rendered",
            "[app] host: OpenGL",
            "[SDL] GL ready:",
        ):
            if forbidden in output:
                raise RuntimeError(f"{point}: emitted {forbidden!r}")
        if not validate_host_presentation(BACKENDS[0], point, output):
            raise RuntimeError(f"{point}: fault did not run against a real drawable")
        if verbose:
            print(f"  PASS {point} failed closed on the native app boundary")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")
    if not rom.is_file():
        parser.error(f"ROM not found: {rom}")

    try:
        for backend in BACKENDS:
            for policy in ("240", "uncapped"):
                run_policy(
                    binary, rom, backend, policy, args.timeout, args.verbose)
        # Regression for launcher -> engine config activation: unlike the
        # matrix above, this arm has no MDKR_PRESENT_RATE environment override.
        # It stages 240 after launcher initialization, then requires the real
        # engine presentation row to resolve that setting in the same process.
        run_policy(binary, rom, BACKENDS[0], "240", args.timeout,
                   args.verbose, stage_via_launcher=True)
        run_borrowed_configure_failure(
            binary, rom, args.timeout, args.verbose)
        run_borrowed_device_loss(binary, rom, args.timeout, args.verbose)
        run_borrowed_second_device_loss(
            binary, rom, args.timeout, args.verbose)
        run_borrowed_candidate_callback_failure(
            binary, rom, args.timeout, args.verbose)
        run_borrowed_device_recovery_failure(
            binary, rom, args.timeout, args.verbose)
        run_host_fault(
            binary,
            "host.surface-configure",
            (
                "[app] injected initial WebGPU surface configuration failure",
                "[app] initial WebGPU surface configuration failed",
            ),
            args.timeout,
            args.verbose,
        )
        run_host_fault(
            binary,
            "host.surface-acquire",
            (
                "[app] injected WebGPU host surface acquisition failure",
                "[app] smoke: host renderer entered an unrecoverable state",
            ),
            args.timeout,
            args.verbose,
        )
        run_host_fault(
            binary,
            "host.surface-view",
            (
                "[app] smoke: host renderer entered an unrecoverable state",
            ),
            args.timeout,
            args.verbose,
        )
        run_host_imgui_init_fault(binary, args.timeout, args.verbose)
        for point in (
            "host.imgui-texture-view",
            "host.imgui-draw-buffer",
            "host.imgui-image-bind-group",
            "host.capture-map-timeout",
        ):
            run_host_capture_fault(binary, point, args.timeout, args.verbose)
        run_fps_only_overlay(binary, rom, args.timeout, args.verbose)
        for point in ("overlay.view", "overlay.pass"):
            run_native_overlay_fault(
                binary, rom, point, args.timeout, args.verbose)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"app adopted pacing: FAIL — {error}")
        return 1

    print("app adopted pacing: PASS — the WebGPU-default and explicit-GL "
          "launcher handoffs completed at numeric and uncapped rates with "
          "fully drained GPU work and no completion failures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
