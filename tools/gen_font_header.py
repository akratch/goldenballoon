#!/usr/bin/env python3
"""Generate the embedded outline-face header from subsetted TTFs.

The repo tracks no binary blobs, so the shipped build input is the generated C
header, not the .ttf. This script is the documented, reproducible path from an
upstream release to that header.

Full regeneration (needs `pip install fonttools` in a throwaway venv):

  # 1. Roboto SemiBold -- instanced from the upstream variable font. Roboto
  #    carries no OFL Reserved Font Name, so instancing is permitted.
  curl -LO https://raw.githubusercontent.com/google/fonts/main/ofl/roboto/'Roboto[wdth,wght].ttf'
  python -c "from fontTools import ttLib; from fontTools.varLib import instancer; \
      f=ttLib.TTFont('Roboto[wdth,wght].ttf'); \
      instancer.instantiateVariableFont(f,{'wght':600,'wdth':100},updateFontNames=True) \
          .save('Roboto-SemiBold.ttf')"

  # 2. Concert One -- already a static face upstream.
  curl -LO https://raw.githubusercontent.com/google/fonts/main/ofl/concertone/ConcertOne-Regular.ttf

  # 3. Subset both to printable ASCII (the range ASSET_FONTS covers) and strip
  #    hinting and layout tables the renderer never consults.
  for f in Roboto-SemiBold ConcertOne-Regular; do
    pyftsubset $f.ttf --unicodes=U+0020-007E --output-file=$f.sub.ttf \
      --no-hinting --desubroutinize --drop-tables+=GSUB,GPOS,GDEF,DSIG
  done

  # 4. Emit the header.
  tools/gen_font_header.py platform/fast3d/gfx_font_faces.h \
      MDKR_FONT_FACE_SMALL:Roboto-SemiBold.sub.ttf \
      MDKR_FONT_FACE_SUBTITLE:ConcertOne-Regular.sub.ttf
"""
import os
import sys

BANNER = """\
/*
 * gfx_font_faces.h -- embedded outline faces. GENERATED FILE, DO NOT EDIT.
 *
 * Regenerate with tools/gen_font_header.py; see that script for the exact
 * upstream URLs, the variable-font instancing step, and the subsetting flags.
 *
 * Both faces are SIL Open Font License 1.1 and are redistributable in binary
 * form as part of this program. See lib/fonts/LICENSE.txt for the full licence
 * text and THIRD_PARTY.md for attribution. These bytes are our own shipped
 * asset -- nothing here is derived from any game ROM.
 *
 *   MDKR_FONT_FACE_SMALL    Roboto SemiBold  (instanced wght=600, wdth=100)
 *   MDKR_FONT_FACE_SUBTITLE Concert One Regular
 *
 * Both are subsetted to printable ASCII (U+0020..U+007E), which is exactly the
 * range ASSET_FONTS addresses.
 */
#ifndef MDKR_GFX_FONT_FACES_H
#define MDKR_GFX_FONT_FACES_H

#include <stddef.h>
#include <stdint.h>
"""


def emit(symbol, path, out):
    data = open(path, "rb").read()
    out.append("\n/* %s: %d bytes */" % (os.path.basename(path), len(data)))
    out.append("static const uint8_t %s_ttf[%d] = {" % (symbol, len(data)))
    for i in range(0, len(data), 16):
        out.append("    " + " ".join("0x%02x," % b for b in data[i:i + 16]))
    out.append("};")
    out.append("static const size_t %s_ttf_size = sizeof(%s_ttf);"
               % (symbol, symbol))


def main():
    dest = sys.argv[1]
    out = [BANNER]
    for spec in sys.argv[2:]:
        symbol, path = spec.split(":", 1)
        emit(symbol, path, out)
    out.append("\n#endif")
    with open(dest, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d bytes)" % (dest, os.path.getsize(dest)))


main()
