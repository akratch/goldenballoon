# High-resolution text

Restored and Remastered redraw DKR's two plain lettering faces from outline
fonts shipped with the port, instead of magnifying their 11- and 14-pixel source
images. Layout is untouched: the same words break in the same places at the same
pixel positions. Only the letter shapes gain resolution.

Pure is byte-exact and never redraws a glyph.

Player switch: **Video.HighResolutionText** (`MDKR_HIRES_TEXT`), restart scope.
Preset default is off in Pure, on in Restored and Remastered.

Pure's byte-exactness does not rest on that default. A preset default is
outranked by an ini value, the environment variable, or a CLI pair, so the
publish step denies Pure structurally: `g_pcHiresText` is `(mode != Pure) &&
value`. Selecting Restored, Remastered or Custom is how a player opts in.

## Which fonts, and why only those

`ASSET_FONTS` holds four authored faces. Measured against `baserom.us.v80.z64`:

| # | Name | Format | Atlases | Cell height | Glyphs | Treatment |
|---|------|--------|---------|-------------|--------|-----------|
| 0 | `FunFont` | RGBA32 | 6 | 12 px | 71 | **Untouched.** Multicolour gradient caps; lowercase aliases to uppercase. DKR's own lettering. |
| 1 | `SmallFont` | IA8 | 3 | 11 px | 94 of 96 | **Replaced.** A plain grotesque with full ASCII and real lowercase. Menus, HUD, times. |
| 2 | `BigFont` | RGBA32 | 28 | 28 px | 54 | **Untouched.** Blue/orange gradient caps, no digits. |
| 3 | `SubtitleFont` | IA8 | 3 | 14 px | 71 | **Replaced.** A chunky monochrome display face. |

The two replaced faces carry no house style — they are generic lettering that
happens to be low resolution. `FunFont` and `BigFont` are the game's identity and
keep their ROM pixels; in Remastered they continue down the SDF contour path.

Classification happens at the font loader (`font_face_class()` in
`game/src/font.c`), never by texture dimensions. The asset id decides and the
authored face name must agree, so a revision that renumbers `ASSET_FONTS`
degrades to "leave it alone" rather than redrawing the wrong face.

## Why layout cannot move

`render_text_string()` takes every layout quantity from `FontData` and none from
the texture:

* advance is `letter[c].ulx`, or `fontData->x` when non-zero, minus one under
  compact kerning;
* the rect origin is `letter[c].width` / `.height` (the per-glyph offsets);
* the rect size and source UV are `.lrx`/`.lry` and `.s`/`.t`.

So `get_text_width()`, centring, dialogue-box wrapping and scissoring are
functions of the ROM metrics alone. Substituting the pixels inside a glyph cell
cannot move any text, and this module writes nowhere else.

## The fit

Each glyph is fitted to its own ROM ink box, per glyph:

* **Vertical fit is exact.** This derives baseline, x-height, cap height and
  descender placement from the ROM data itself, with no global metrics to get
  wrong and nothing to re-derive per region or per revision.
* **Horizontal fit follows the ROM ink width**, so the line keeps the ROM's
  rhythm — but the stretch is bounded to ±15%. At a 7-pixel cap height the ROM
  cannot draw a stem thinner than about 2 pixels, so `l`, `i`, `I` and `1` have
  ink boxes more than twice as wide as any outline face's natural stem.
  Matching those exactly would produce grotesquely fat verticals; measured
  ratios cluster at 1.04–1.14 with those four as far outliers.
* **The result is clamped to the cell**, so a glyph is never clipped.
* **Colour is the region's alpha-weighted mean**, so any tint the ROM glyph
  carried is preserved and the combiner behaves exactly as before.
* Any glyph that cannot be rendered falls back to a nearest-neighbour blit of
  the ROM cell, so a missing outline degrades to today's output, never a hole.
* One authored cell can serve two characters (FunFont draws `a` from the `A`
  cell). Neither replaced face aliases a cell today, but the renderer settles it
  as first-character-wins rather than resting on that measurement, because two
  glyphs in one rectangle would superimpose rather than replace.

The scale is solved against stbtt's box **at the final scale**, not extrapolated
from a reference measurement: the integer box does not scale linearly, and
trusting the extrapolation clipped a row from descenders and from the right stem
of `W`. Measured on the real atlases, that was 32 texels per run before the fix,
and `clippedTexels` in the `[FONT]` trace now asserts it stays at zero.

## Face selection

Candidates were scored against the ROM glyphs on shape (IoU after normalising
both to a common box), proportion (ink width over cap height), fit (worst
per-glyph compression needed to stay inside the authored cell) and stroke weight
(ink density at matched cap height), then judged on a simulation of the real
pipeline.

* **`SmallFont` → Roboto SemiBold.** The ROM face measures at SemiBold weight;
  Regular candidates were 22–29% too light. Roboto at wght=600 lands within 2%
  and has the tightest width distribution of the field. It is also the family
  already embedded in the launcher shell, so the launcher and the game agree.
* **`SubtitleFont` → Concert One**, shipped as "MDKR Subtitle" (see Licensing).
  The ROM face is a heavy, condensed, rounded display face with a single-storey
  `a`, which ruled out Anton and the other double-storey candidates. Concert One
  matches its colour and proportions and is fully correctable within the ±15%
  band.

Both are subsetted to printable ASCII, which is exactly the range `ASSET_FONTS`
addresses: 9.8 KB and 16.7 KB.

## Licensing

Both faces are SIL Open Font License 1.1. Full text in `lib/fonts/LICENSE.txt`;
attribution in `THIRD_PARTY.md`.

The replacement fonts are **our** assets, not game assets, so this stays inside
the decomp-native-port policy. No ROM glyph bitmaps are shipped and none are
written to disk: `tools/check_no_rom.sh` proves both derivation modules expose no
file-output API and that their only production consumer hands the buffer
straight to the GPU uploader.

The fit is computed at runtime rather than baked at build time on purpose. It
derives from ROM cell geometry, so freezing it into a shipped artifact would
embed ROM-derived layout data. Runtime fitting keeps the shipped bytes purely
our own.

Concert One reserves the font names "Concert" and "Concert One". Subsetting
produces a Modified Version under the OFL, so our derivative is renamed to
"MDKR Subtitle" and uses neither reserved name, while retaining the upstream
copyright notice in its own name table. Roboto reserves no name, so the
instanced and subsetted derivative keeps the Roboto name.

`tools/gen_font_header.py` documents the exact upstream URLs, the variable-font
instancing step and the subsetting flags. No `.ttf` is tracked; the generated
header is the build input.

## Scope

* **Non-JP only.** `REGION == REGION_JP` bypasses `load_font` entirely for a
  16-bit charmap path, which is untouched.
* **Text baked into menu and screen textures is not affected.** Those are not
  `ASSET_FONTS` glyphs and stay as they are.

## Verification

| Claim | Evidence |
|-------|----------|
| Pure is unchanged | Captured frame is **byte-identical** to a build of `origin/main` |
| Stylized faces are unchanged | Remastered character-select (12 SDF atlases, 2 outline) is **byte-identical** to `origin/main` |
| The redraw happens and is confined | `check_font_outline.py`: Restored and Remastered upload, Pure does not, every changed pixel inside the text rectangle, on GL and WebGPU |
| The SDF path still works | `check_font_sdf.py`, with `MDKR_HIRES_TEXT=0` pinning this path off so it measures the contour pass alone |
| No write escapes a glyph cell | `font_outline` CTest scans the whole atlas for ink outside registered regions |
| No glyph is clipped | `clippedTexels` in the `[FONT]` trace, asserted zero by `check_font_outline` and `check_font_sdf` on real ROM glyphs, and by the CTest. Negative control: reverting the fit bound reports 32 clipped texels |
| The rasteriser is actually running | The CTest requires antialiased coverage and output that differs from a nearest-neighbour copy — both of which the ROM fallback fails, so a dead rasteriser cannot pass |
| Pure resists a forced override | `check_font_outline`'s Pure arms run with `MDKR_HIRES_TEXT=1`. Negative control: removing the mode guard fails them |
| Output is stable | `font_outline` CTest renders twice and compares |
| Registration is sound | `font_registry` CTest covers character-keyed regions and face-conflict degradation |
| Costs no timing | 1200 normalized `[PACE]` rows identical across all six arms, both backends |

`MDKR_FONT_OUTLINE=0` is a test control, not a player setting; the player switch
is `Video.HighResolutionText`.
