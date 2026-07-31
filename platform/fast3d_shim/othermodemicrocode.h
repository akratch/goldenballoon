/**
 * othermodemicrocode.h — shim for the vendored mgb64 gfx_opengl.c backend.
 *
 * The mgb64 OpenGL backend calls texDebugDumpRecentFireEvents() from its GE
 * texture-debug module on a GL upload error. mdkr64 provides a no-op so the
 * backend stays verbatim. The other GE symbols (texSelect, sImageTableEntry,
 * NUM_TEXTURES, ...) are unreferenced by gfx_opengl.c and omitted.
 */
#ifndef GFX_SHIM_OTHERMODEMICROCODE_H
#define GFX_SHIM_OTHERMODEMICROCODE_H

#include <stdio.h>

void texDebugDumpRecentFireEvents(FILE *fp);

#endif
