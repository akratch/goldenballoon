#!/usr/bin/env python3
"""Keep the authoritative water clock safe on the mixed scene-object list."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TRACKS = ROOT / "game/src/tracks.c"


def authoritative_water_loop(source: str) -> str:
    start = source.index("void scene_authoritative_render_tick(s32 updateRate)")
    end = source.index("    savedCamera = get_current_viewport();", start)
    return source[start:end]


def validate(loop: str) -> None:
    particle_gate = loop.index("waterObject->trans.flags & OBJ_FLAGS_PARTICLE")
    group_gate = loop.index(
        "waterObject->header->waterEffectGroup != SHADOW_ACTORS"
    )
    pointer_load = loop.index("waterEffect = waterObject->waterEffect;")
    pointer_read = loop.index("waterEffect->scale")

    assert particle_gate < group_gate < pointer_load < pointer_read, (
        "the mixed object/particle list must be gated in renderer order before "
        "the optional WaterEffect allocation is dereferenced"
    )


def main() -> None:
    loop = authoritative_water_loop(TRACKS.read_text(encoding="utf-8"))
    validate(loop)

    # Positive controls prove that this contract fails for both regressions
    # which caused the four-player race-start crash.
    without_particle_gate = loop.replace(
        "waterObject->trans.flags & OBJ_FLAGS_PARTICLE", "particle_gate_removed", 1
    )
    try:
        validate(without_particle_gate)
    except ValueError:
        pass
    else:
        raise AssertionError("particle-gate positive control did not fail")

    unsafe_order = loop.replace(
        "waterEffect = waterObject->waterEffect;",
        "waterEffect->scale;\n            waterEffect = waterObject->waterEffect;",
        1,
    )
    try:
        validate(unsafe_order)
    except AssertionError:
        pass
    else:
        raise AssertionError("dereference-order positive control did not fail")

    print("test_water_effect_tick_contract: PASS")


if __name__ == "__main__":
    main()
