# Provenance — `platform/fast3d/`

The Fast3D display-list interpreter and its GPU backends in this directory are a
**derivative of third-party code**, not original MGB64 work. This file records
that origin and reproduces the applicable license notice, as required by the
upstream license (see the source-redistribution condition below).

> **Vendored file.** This document came with the backend from the author's
> GoldenEye port **mgb64**, and is kept close to its mgb64 form deliberately so the
> two projects can converge on common code. Where it says "MGB64" the same statements
> apply to this port; where it names mgb64 paths, the Golden Balloon equivalents are:
>
> | mgb64 | Golden Balloon |
> | --- | --- |
> | `src/platform/fast3d/` | `platform/fast3d/` |
> | `THIRD_PARTY.md` | [`NOTICE.md`](../../NOTICE.md) |
> | `gfx_pc.c` (F3DEX interpreter) | `gfx_pc_dkr.c` (F3DDKR — rewritten, not ported) |
> | `gfx_room_normals.c` | *(not present — GoldenEye-specific)* |
> | `macos/Scripts/verify_asset_free.sh`, `scripts/ci/check_no_rom_data.sh` | [`tools/check_no_rom.sh`](../../tools/check_no_rom.sh), [`tools/check_clean_room.sh`](../../tools/check_clean_room.sh) |

## Origin

| File(s) | Derived from | License |
| --- | --- | --- |
| `gfx_opengl.c`, `gfx_rendering_api.h`, `gfx_cc.c`, `gfx_cc.h` | Emill/n64-fast3d-engine — `https://github.com/Emill/n64-fast3d-engine` (attributed in each file header) | n64-fast3d-engine license — modified BSD-2-Clause (see below) |
| `gfx_cc.c`, `gfx_cc.h` | Emill/n64-fast3d-engine, **rewritten for 2-cycle support** based on the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port | n64-fast3d-engine license — modified BSD-2-Clause (see below) |
| `gfx_metal.mm`, `gfx_uniforms.h`, `gfx_screen_config.h` | MGB64-authored code written against the upstream `GfxRenderingAPI` seam: the native Metal backend and shared declarations. `gfx_metal.mm` used the libultraship Metal backend (Kenix3, MIT) as a structural reference only; it is not compiled into any build target. | MIT (first-party, per repo [LICENSE](../../LICENSE)) |
| `gfx_msaa_util.c`, `gfx_msaa_util.h`, `gfx_palette.h`, `gfx_texgen.h`, `screenshot_series.c`, `screenshot_series.h` | MGB64-authored support code vendored alongside the backends (MSAA sample selection, palette and texgen helpers, the screenshot series harness). `screenshot_series.c` is excluded from this project's build. | MIT (first-party, per repo [LICENSE](../../LICENSE)) |
| `gfx_pc_dkr.c`, `gfx_pc_dkr.h` | **Golden Balloon-authored** F3DDKR display-list interpreter: written for this project because DKR uses its own microcode (F3DDKR) rather than F3DEX, so upstream's `gfx_pc.c` could not be reused. It targets the same upstream `GfxRenderingAPI` seam and reuses upstream's combiner (`gfx_cc.c`) and VBO attribute layout, so it is a derivative of that interface. | MIT (first-party, per repo [LICENSE](../../LICENSE)) |
| `gfx_webgpu.c`, `gfx_webgpu.h`, `gfx_webgpu_shader.c`, `gfx_webgpu_shader.h`, `gfx_webgpu_compat.h` | **MGB64-authored origin**, ported from mgb64 `src/platform/fast3d/` at M4.5 and subsequently adapted for this project's DKR lifecycle, app handoff, presentation, recovery, and validation needs. Implements the same upstream `GfxRenderingAPI` seam. Runtime WGSL shader generator (`gfx_webgpu_shader.c`) consumes the shared `gfx_cc.c` combiner output. | MIT (first-party, per repo [LICENSE](../../LICENSE)) |
| `smaa_area_tex.h`, `smaa_search_tex.h` | Generated LUTs — see [`../../NOTICE.md`](../../NOTICE.md) (SMAA) | MIT |

The WebGPU backend links **wgpu-native** (gfx-rs, the C `webgpu.h` implementation)
as a **pinned prebuilt** — see `cmake/webgpu.cmake` (version `v29.0.1.1`, SHA-256
verified). wgpu-native is dual MIT/Apache-2.0; only its prebuilt static lib is
consumed (no source vendored). Under Emscripten (M8) WebGPU comes from the
browser via the `emdawnwebgpu` port instead, so no wgpu-native there.

The MGB64-authored files above build on the upstream's `GfxRenderingAPI` seam and
data structures; they are covered by MGB64's own MIT license but are noted here so
the boundary with the upstream code is explicit.

## Upstream license (n64-fast3d-engine)

The n64-fast3d-engine upstream is **not** MIT-licensed (an earlier revision of
this file incorrectly reproduced an MIT notice). It is distributed under a
custom, BSD-2-Clause–style license: source redistribution is permitted on
BSD-style terms, but **binary redistribution is restricted** to binaries that
contain no assets the distributor lacks the right to distribute. The notice
below was reconciled verbatim against the authoritative upstream `LICENSE.txt`
(`https://github.com/Emill/n64-fast3d-engine/blob/master/LICENSE.txt`):

```
Copyright (c) 2020 Emill, MaikelChan

Redistribution and use in source forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form are not allowed except in cases where the binary contains no assets you do not have the right to distribute.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

**Binary-redistribution note (condition 2).** MGB64's prebuilt binaries contain
no ROM or ROM-derived assets: they load a user-supplied ROM at runtime and are
verified asset-free by `macos/Scripts/verify_asset_free.sh` (built binary/app)
and the source-tree guard `scripts/ci/check_no_rom_data.sh`. They therefore
satisfy condition 2, which is what permits distributing them at all.

The 2-cycle combiner work in `gfx_cc.c` additionally derives from the Perfect Dark
decompilation/port (MIT); see the `platform/mixer.c` and `gfx_cc.c` rows in
[`THIRD_PARTY.md`](../../THIRD_PARTY.md) for that project's licensing.
