#!/usr/bin/env python3
"""Prove every public WebGPU fault point is wired and explicitly classified.

This is deliberately ROM/GPU-free. Runtime checks prove the reachable routes;
this gate prevents the registry from growing names that no constructor/status
site consumes, and prevents inherited compatibility code from being mistaken
for shipped DKR coverage.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "platform" / "fast3d" / "gfx_webgpu_fault.h"
BACKEND = ROOT / "platform" / "fast3d" / "gfx_webgpu.c"
NATIVE_RUNTIME = ROOT / "tests" / "check_webgpu_recovery.py"
BROWSER_RUNTIME = ROOT / "tests" / "check_browser_runtime.py"

REGISTRY_RE = re.compile(
    r'X\(\s*([A-Z0-9_]+)\s*,\s*"([^"]+)"\s*\)'
)


@dataclass(frozen=True)
class Classification:
    route: str
    dialect: str
    policy: str


MINIMAP = {
    "minimap.module",
    "minimap.bind-group-layout",
    "minimap.uniform",
    "minimap.bind-group",
    "minimap.pipeline-layout",
    "minimap.pipeline",
    "minimap.pass",
}
MODERN = {
    "modern.module",
    "modern.bind-group-layout",
    "modern.pipeline-layout",
    "modern.pipeline",
    "modern.vertex-buffer",
    "modern.index-buffer",
    "modern.texture",
    "modern.view",
    "modern.uniform",
    "modern.sampler",
    "modern.bind-group",
}
DORMANT_MIDFRAME_READBACK = {
    "readback.partial-finish",
    "readback.partial-encoder",
    "readback.partial-pass",
    "readback.resolve-encoder",
    "readback.resolve-finish",
}
DORMANT_RDP_DIAGNOSTIC = {
    # These shader option bits belong to the inherited GE007 diagnostic
    # frontend. dkr_setup_draw_state() never emits either bit, so no DKR
    # material can enter the snapshot/viewport-uniform route.
    "memory-blend.texture",
    "memory-blend.view",
    "memory-blend.sampler",
    "memory-blend.pass",
    "draw.diagnostic-uniform",
}
DORMANT = (
    MINIMAP
    | MODERN
    | DORMANT_MIDFRAME_READBACK
    | DORMANT_RDP_DIAGNOSTIC
    | {"shader.pipeline-prewarm"}
)
BROWSER_ONLY = {"shader.pipeline-async", "overlay.view", "overlay.pass"}
NATIVE_ONLY = {"capture.surface-buffer"}
LOCAL_DIAGNOSTIC = {
    "capture.frame-buffer",
    "readback.buffer",
    "readback.encoder",
    "readback.finish",
    "readback.map",
}
DIAGNOSTIC_MATERIAL = {
    "draw.diagnostic-uniform",
}
LOCAL_DEGRADE = {
    "texture.mip-texture",
    "texture.mip-view",
    "surface.direct-view",
    "overlay.view",
    "overlay.pass",
}
BOUNDED_RETRY = {
    "surface.suboptimal",
    "surface.timeout",
    "surface.outdated",
    "surface.lost",
}
FEATURE_DEGRADE = {"capabilities.depth-clip-absent"}
LIMIT_RETRY = {"bringup.device-limits"}
STARTUP_FALLBACK = {
    "bringup.instance",
    "bringup.surface",
    "bringup.adapter",
    "bringup.device-defaults",
    "bringup.queue",
    "capabilities.format",
    "capabilities.alpha",
    "capabilities.present",
}


def classify(name: str) -> Classification:
    if name in DORMANT:
        if name in MINIMAP:
            route = "dormant-mgb64-minimap"
        elif name in MODERN:
            route = "dormant-mgb64-modern-mesh"
        elif name in DORMANT_MIDFRAME_READBACK:
            route = "dormant-ge007-midframe-readback"
        elif name in DORMANT_RDP_DIAGNOSTIC:
            route = "dormant-ge007-rdp-diagnostic"
        else:
            route = "dormant-mgb64-prewarm"
        return Classification(route, "none", "unsupported-not-executed")

    if name in NATIVE_ONLY:
        return Classification(
            "native-surface-capture", "native", "local-diagnostic-failure"
        )

    if name in BROWSER_ONLY:
        policy = "local-degrade" if name in LOCAL_DEGRADE else "browser-fatal-panel"
        return Classification("browser-conditional", "browser", policy)

    if name in LOCAL_DIAGNOSTIC:
        return Classification(
            "diagnostic-capture-readback", "native+browser", "local-diagnostic-failure"
        )
    if name in DIAGNOSTIC_MATERIAL:
        return Classification(
            "shipped-rdp-diagnostic-material",
            "native+browser",
            "fatal-at-boundary",
        )

    if name in LOCAL_DEGRADE:
        return Classification("shipped-conditional", "native+browser", "local-degrade")
    if name in BOUNDED_RETRY:
        return Classification(
            "shipped-surface", "native+browser", "bounded-reconfigure-then-recover"
        )
    if name in FEATURE_DEGRADE:
        return Classification(
            "shipped-bringup", "native+browser", "featureless-far-clip"
        )
    if name in LIMIT_RETRY:
        return Classification(
            "shipped-bringup", "native+browser", "retry-default-limits"
        )
    if name in STARTUP_FALLBACK:
        return Classification(
            "shipped-bringup", "native+browser", "native-gl-or-browser-panel"
        )
    if name.startswith(("resolve.", "post.")):
        return Classification(
            "shipped-video-mode", "native+browser", "fatal-at-boundary"
        )
    if name == "overlay.output-pass":
        return Classification(
            "shipped-output-resolution-ui",
            "native+browser",
            "fatal-at-boundary",
        )
    if name.startswith("memory-blend."):
        return Classification(
            "shipped-rdp-memory-blend", "native+browser", "fatal-at-boundary"
        )
    if name.startswith(
        (
            "bringup.",
            "capabilities.",
            "surface.",
            "device.",
            "queue.",
            "frame.",
            "target.",
            "shader.",
            "texture.",
            "draw.",
        )
    ):
        return Classification(
            "shipped-core-renderer", "native+browser", "fatal-at-boundary"
        )
    raise AssertionError(f"unclassified WebGPU fault point: {name}")


def wired_symbols(source: str) -> set[str]:
    direct = re.findall(
        r"gfx_webgpu_fault_(?:hit|selected)\s*\(\s*"
        r"GFX_WEBGPU_FAULT_([A-Z0-9_]+)\s*\)",
        source,
    )
    constructors = re.findall(
        r"WGPU_FAULT_CREATE\s*\(\s*([A-Z0-9_]+)\s*,",
        source,
    )
    return set(direct) | set(constructors)


def named_runtime_points(source: str, public_names: set[str]) -> set[str]:
    """Return public fault names named by an executable runtime gate.

    The runtime scripts use literal public names both for direct dictionary
    injections and loop-driven cases. Intersecting every dotted literal with
    the registry handles both shapes and deliberately ignores diagnostic text
    that is not itself a public point.
    """

    literals = set(
        re.findall(r'"([a-z][a-z0-9-]*\.[a-z0-9.-]+)(?:@all)?"', source)
    )
    return literals & public_names


def main() -> int:
    registry = REGISTRY_RE.findall(HEADER.read_text(encoding="utf-8"))
    if not registry:
        raise AssertionError("fault registry is empty or unreadable")
    symbols = [symbol for symbol, _ in registry]
    names = [name for _, name in registry]
    if len(symbols) != len(set(symbols)):
        raise AssertionError("duplicate symbolic fault point")
    if len(names) != len(set(names)):
        raise AssertionError("duplicate public fault name")

    wired = wired_symbols(BACKEND.read_text(encoding="utf-8"))
    missing_wiring = sorted(set(symbols) - wired)
    stale_wiring = sorted(wired - set(symbols))
    if missing_wiring or stale_wiring:
        raise AssertionError(
            f"fault wiring drift: missing={missing_wiring}, stale={stale_wiring}"
        )

    classifications = {name: classify(name) for name in names}
    dormant = sorted(name for name, item in classifications.items() if item.dialect == "none")
    browser = sorted(
        name for name, item in classifications.items() if item.dialect == "browser"
    )
    native = sorted(
        name for name, item in classifications.items() if item.dialect == "native"
    )
    active = sorted(
        name
        for name, item in classifications.items()
        if item.dialect == "native+browser"
    )
    public_names = set(names)
    native_runtime = named_runtime_points(
        NATIVE_RUNTIME.read_text(encoding="utf-8"), public_names
    )
    browser_runtime = named_runtime_points(
        BROWSER_RUNTIME.read_text(encoding="utf-8"), public_names
    )
    native_required = {
        name
        for name, item in classifications.items()
        if item.dialect in {"native", "native+browser"}
    }
    browser_required = {
        name
        for name, item in classifications.items()
        if item.dialect == "browser"
    }
    dormant_set = set(dormant)
    missing_native = sorted(native_required - native_runtime)
    missing_browser = sorted(browser_required - browser_runtime)
    false_dormant_credit = sorted(
        dormant_set & (native_runtime | browser_runtime)
    )
    if missing_native or missing_browser or false_dormant_credit:
        raise AssertionError(
            "runtime fault coverage drift: "
            f"missingNative={missing_native}, "
            f"missingBrowser={missing_browser}, "
            f"dormantClaimed={false_dormant_credit}"
        )

    expected_dormant = sorted(DORMANT)
    if dormant != expected_dormant:
        raise AssertionError(
            f"dormant-route classification drift: {dormant} != {expected_dormant}"
        )
    if len(active) + len(native) + len(browser) + len(dormant) != len(names):
        raise AssertionError("fault classifications do not partition the registry")

    policies: dict[str, int] = {}
    for item in classifications.values():
        policies[item.policy] = policies.get(item.policy, 0) + 1
    policy_summary = ", ".join(
        f"{name}={count}" for name, count in sorted(policies.items())
    )
    print(
        "check_webgpu_fault_matrix: PASS — "
        f"{len(names)}/{len(names)} wired and classified; "
        f"shipped shared={len(active)}, native-only={len(native)}, "
        f"browser-only={len(browser)}, "
        f"explicitly dormant={len(dormant)}; "
        f"runtime native={len(native_required)}/{len(native_required)}, "
        f"browser={len(browser_required)}/{len(browser_required)}; "
        f"{policy_summary}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
