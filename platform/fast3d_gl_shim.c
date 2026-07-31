/**
 * fast3d_gl_shim.c — definitions for the handful of GoldenEye-tree symbols the
 * vendored mgb64 OpenGL backend (platform/fast3d/gfx_opengl.c) references.
 *
 * These let gfx_opengl.c compile/link verbatim without dragging in the GE game
 * tree. See platform/fast3d_shim/{vi,othermodemicrocode,front}.h.
 */
#include <stdio.h>
#include "gfx_screen_config.h" /* DESIRED_SCREEN_WIDTH/HEIGHT */
#include "vi.h"

/* Verbose shader/diagnostic flag read by gfx_opengl.c (extern int). Off. */
int g_diag_verbose = 0;

/* N64 VI output resolution. DKR uses the standard 320x240 VI grid. */
int16_t viGetX(void) { return DESIRED_SCREEN_WIDTH; }
int16_t viGetY(void) { return DESIRED_SCREEN_HEIGHT; }

/* GE texture-debug fire-event dump — no-op on the DKR port. */
void texDebugDumpRecentFireEvents(FILE *fp) { (void) fp; }
