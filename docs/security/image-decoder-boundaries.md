# Image decoder boundaries and qualification

Source inventory: 2026-09-06, `int-1.7.0` at `c657fa62` plus local changes.
This is a defensive integration record, not a security certification. Exact
artifact qualification and the dated full advisory inventory remain required
by [the release checklist](../RELEASE_CHECKLIST.md).

## PNG implementation and entry points

`lib/stb/stb_image.h` 2.30 and `stb_image_write.h` 1.16 use the base
`nothings/stb` commit `f58f558c120e9b32c217290b80bad1a0729fbb2c`.
The decoder has the local conversion-failure amendment documented below; the
writer remains unmodified. Their original/amended SHA-256 values and licenses
are recorded in [THIRD_PARTY.md](../../THIRD_PARTY.md).
One first-party implementation file instantiates both headers. PNG-only,
memory-input decoding and callback-output writing remain configured; no
additional format, path API or decoder pin is enabled by this maintenance.

| First-party entry point | Pre-decode admission |
|---|---|
| `platform/mod_texture_store.c` | Owned bounded file bytes; mandatory header inspection and decoded-size budget; decoded/header dimensions must agree |
| `platform/modern_character_render.c` | Compiled texture range and PNG header; at most 4096 pixels per axis |
| `platform/modern_character_identity.c` | Compiled portrait range and PNG header; at most 1024 pixels per axis |
| `platform/app/character_portrait_import.cpp` | Bounded owned source bytes and structural PNG validation; 16–4096 px, non-interlaced 8-bit RGB/RGBA |

All four callers request RGBA and have compile-time conversion-size guards.
Every one also sets a per-thread decode bound from the header it admitted (see
below) around its single decode call.
These are integration bounds, not a claim that every decoder operation is
safe. The later mandatory texture-header admission tests remain unexecuted on
the latest candidate. Header inspection and decoding must consume the same
owned bytes; post-decode checks cannot replace pre-decode admission.

## Allocation admission candidate

The cited upstream integration fix for the older general-loader advisory adds
an empty-allocation check. The candidate applies that defensive policy through
first-party `stb_image_alloc.h`, without editing vendored headers. A refused
resize preserves the original allocation for ordinary failure cleanup,
including when the requested size is zero.
[Upstream fix](https://github.com/saitoha/libsixel/commit/d6e34fc),
[upstream security release](https://github.com/saitoha/libsixel/releases/tag/v1.8.7).

Positive requests use the system allocator unless the calling thread set a
decode bound (`mdkr_stbi_set_allocation_limit`, stored by the translation
unit that instantiates stb_image). Header admission alone bounds what a
decode returns, not what it allocates on the way: the zlib stage grows its
buffer by doubling until the IDAT stream ends, so a crafted stream of a few
megabytes could otherwise demand gigabytes for an admitted 64x64 picture.
`mdkr_stbi_png_allocation_bound` derives the largest single allocation a header
can legitimately need (twice the inflated scanlines, for the doubling
estimate interlaced images exceed; the converted output; twice the file for
the concatenated IDAT bytes; plus 64 KiB), and every pack/character caller
sets it around its decode. The `mod_texture_store` CTest decodes a fixture
that inflates far past its header and requires the refusal to stay within
that bound.

This addresses the documented allocation condition at our boundary. It does
not prove the full applicability or closure of
[CVE-2019-19777](https://nvd.nist.gov/vuln/detail/CVE-2019-19777), whose general
description is broader than the referenced integration fix. Keep its final
disposition and current-candidate behavioral qualification open. No reproducer,
malformed-input payload, or exploit analysis is part of this change.

The texture-store allocation probe delegates to the production helpers after
its counters/refusal seam. `png_write_layout` adds small direct allocation,
refused-resize ownership, positive grow/shrink and ordinary RGB/RGBA roundtrip
coverage. Strict C syntax, native optimized/ASan+UBSan compilation, and Windows
cross-compilation of the game and affected PNG/store/portrait targets pass;
preprocessing confirms the production allocator macros and excluded format
decoders. Execution and sanitizer-runtime evidence are pending. Compiler
success is not a test pass. The Windows developer profile is not the final
release artifact and the BasisU rebuild still emits separately reviewed GCC
diagnostics; no warning suppression or decoder-pin change was added here.

## Conversion-failure ownership amendment

Later source review found an ordinary allocation-failure cleanup gap in the
pinned bit-depth conversion helpers. These consuming helpers freed their input
on success but not when destination allocation failed. Their callers replaced
the input pointer with the result, so the input allocation was no longer owned
for cleanup. The local amendment frees the consumed buffer on either outcome
and returns immediately from the two postprocessing paths if conversion fails.
It changes no successful sample conversion, image limit, source admission or
format configuration. Product callers request 8-bit RGBA and do not enable
vertical flipping; the symmetric 16-bit API/postprocessing correction does not
enable those features in the product.

This is an explicitly marked local change to `stb_image.h`, not an upstream
release or a disposition of CVE-2019-19777. The base and amended hashes are
different and both are recorded in the notices. The writer, allocator policy
and KTX2 amendment are unchanged by this correction.

The registered `stb_conversion_ownership` fixture uses small ordinary pixel
buffers for both conversion directions and all 1–4 component counts, checking
success values, ownership transfer and deterministic destination-allocation
refusal. It also encodes an ordinary 8-bit 2x2 RGBA PNG and checks both the
product's public 8-bit RGBA loader and the optional 16-bit output API: successful
samples and each allocation-failure cleanup, with and without the optional
flip. The narrowing helper's ordinary 16-bit input buffers are tested directly;
this fixture does not claim full encoded 16-bit PNG/caller-admission coverage.
No malformed input or external reproducer is involved.
Assertions are active in optimized builds. Strict C11 syntax passes; runtime
assertions, old-code control and final sanitizer/fuzz qualification remain
required. Native optimized and ASan/UBSan compilation/link pass for the game,
new ownership fixture, PNG layout, texture store and portrait studio. CTest
registration was inspected without executing it. A source-confirmed cleanup
correction or instrumented build is not a sanitizer-runtime pass.
The Windows game and those four test targets also cross-compile. This is a
developer build, not final Windows package/runtime evidence; existing BasisU
GCC warnings remain recorded separately and do not become cleared by this pass.
The later web engine also compiles with the amendment. Generated JavaScript
passes `node --check`, and `WebAssembly.validate` accepts the module without
instantiation or execution. Same-link JS/wasm/symbol-map hashes are retained in
private evidence. This incremental build is not final staged-payload provenance,
real browser behavior, decoder-runtime validation or release clearance.

Read-only upstream issue metadata was checked again on 2026-09-06:
[conversion allocation report](https://github.com/nothings/stb/issues/1936)
is open, and [conversion null-handling report](https://github.com/nothings/stb/issues/1921)
is closed. Only metadata was consulted, not payloads or reproducers. Those
states do not establish applicability, a fix in this pin, or closure of this
project's broader advisory review.

## Separate boundaries that remain in force

- PNG writing uses `png_write_layout.h` for packed RGB/RGBA arithmetic before
  texture export or native capture. Its allocator is unchanged here.
- Custom-character KTX2/BasisU has a separate hash-verified alignment amendment
  and prior sanitizer/fuzz evidence. This PNG change neither disables KTX2 nor
  clears its final-artifact qualification.
- Re-run structural, texture-store, portrait, capture and sanitizer/fuzz gates
  on the final source and exact shipping artifacts. Retain ordinary valid-input
  compatibility and all failure/ownership checks. Do not relabel historical
  artifacts as qualifying a later decoder boundary.
