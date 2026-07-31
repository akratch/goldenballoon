# Colour and directional-lighting contract

This document is the CO-1 end-to-end colour-space policy and the RL-2/RL-5
implementation contract. It describes the production GL, WebGPU, browser and
explicit Metal paths as of Wave 2.

## The boundary

DKR's texture texels, vertex colours, primitive/environment colours, fog colour,
framebuffer snapshots, and UI assets are authored eight-bit code values. The
legacy N64 combiner operates on those code values. Pure and Restored therefore
keep that arithmetic byte-for-byte; treating every texture as hardware-sRGB
would change the combiner and is not a correctness fix.

Remastered crosses one explicit boundary only on RL-5-tagged racer and character
fragments:

1. Run the complete N64 combiner and clamp in authored sRGB code space.
2. Decode the resulting RGB with the IEC sRGB transfer function.
3. Add the restrained level-derived diffuse term in linear RGB.
4. When fog is active, decode its authored RGB and blend it in linear RGB.
5. Encode once to sRGB code values for the ordinary UNORM scene target.

Alpha stays linear coverage throughout. The scene target remains UNORM because
the rest of the renderer still intentionally models an encoded N64 combiner.
Consequently, hardware automatic sRGB decode/encode must not be enabled for
these textures or targets without first moving the complete combiner across the
same explicit boundary.

Post-processing outside RL-5 still consumes the encoded scene image. CO-2 must
introduce an equally explicit scene-wide decode before a filmic tonemap and one
encode at output; it must not stack an undocumented gamma approximation on the
current target. Pure and Restored remain outside that future path.

## Runtime-derived light

`gfx_level_lighting` receives a pointer-free normalized view of the loaded
`LevelHeader` and its seven optional `LevelHeader_70` colour-cycle records. It
derives:

- chromaticity from background, upper/lower sky, fog, and colour-cycle values;
- horizontal direction from stable weather velocity and sky scrolling, with a
  deterministic header-identity hash only when neither supplies a vector;
- elevation from the authored upper/lower sky luminance relationship; and
- a 10–16% additive strength from authored source luminance and weather
  intensity.

There is no per-level lookup table and no frame-time animation. The rig is
published after the level header and colour-cycle pointers have been resolved,
byte-swapped, and initialized. Empty/cutscene headers receive a deterministic
neutral fallback. Invalid input resets to a valid upward direction with zero
strength, and the shader option cannot activate.

This use of level data is mood extraction, not re-authoring. DKR's baked vertex
colour remains the low-frequency ambient and exposure base, as required by the
RL-1 A/B decision. The added sun is exposure-normalized and deliberately quiet,
so Fire Mountain stays dark and warm rather than being pushed toward a global
daylight grade.

## Smooth model normals

RL-1 showed that a geometric face normal exposes the karts' coarse triangle
faceting. RL-5 therefore never uses `dFdx`/`dFdy` or a per-face cross product.

The object renderer classifies only racers and character/vehicle-animation
behaviours. It walks each `ObjectModel`'s compact normal stream with the same
`BATCH_VTX_COL`/`RENDER_ENVMAP` rules used by the game's shading code. A
native-only F3DDKR MoveWord carries the normal subspan for each vertex command
and a signed, normalized object-local sun direction for the queued object.
Near-plane vertices interpolate the same smooth normals.

The shader interpolates and normalizes those model normals per fragment. It
multiplies the preserved baked result by:

`1 + sun_colour_linear * sun_strength * max(dot(normal, sun_direction), 0)`

Terrain, UI, particles, billboards, pickups, texture rectangles, and untagged
objects never select this shader variant. Pure and Restored cannot select it
even if the diagnostic `MDKR_RL5_LIGHT` seam is enabled.

## Backend and failure contract

- OpenGL carries two extra `vec3` attributes and two uniforms.
- WebGPU carries the same attributes plus a 16-byte colour/strength UBO at
  binding 8. If that required buffer cannot be created, the affected draw is
  dropped rather than binding incompatible state.
- The browser uses the same WebGPU shader generator and binding contract.
- Explicit Metal generates the same MSL and preserves its existing 160-byte
  aligned uniform layout. This port does not currently build or run that
  backend, so it is source-maintained but not claimed as runtime-validated.

The `[LIGHT]` terminal row records the selected A/B arm, rig identity,
direction, linear colour, strength, source mask, target triangle counts,
missing-normal batches, and the literal
`srgb-authored/linear-light/srgb-output` boundary. Missing/invalid normal
pointers disable the option safely and increment telemetry.

## Regression evidence

`level_lighting` is ROM-free and checks transfer-function anchors, deterministic
derivation, authored direction/colour positive controls, empty-header fallback,
packed display-list direction round trips, publication, and reset behavior.

`check_remaster_lighting.py` drives Ancient Lake and Fire Mountain on GL and
WebGPU. It requires distinct header-derived rigs, nonzero racer and character
coverage, zero missing-normal batches, restrained visual deltas, backend parity,
and exact `[PACE]`. Its reverse arms require Pure and Restored captures to stay
byte-identical when RL-5 is toggled. `check_browser_runtime.py` requires the same
production arm, colour boundary, target coverage, and zero missing normals in a
real Chromium/WebGPU run.

Current native measurements at frame 3475 are intentionally subtle:

| Backend | Ancient Lake baked→sun MAD | Fire Mountain baked→sun MAD |
|---|---:|---:|
| OpenGL | 0.148 | 0.060 |
| WebGPU | 0.150 | 0.061 |

Those are positive-control measurements, not tolerance targets or an invitation
to amplify the effect. The upper gate of 2.0 prevents an accidentally global or
destructive relight.
