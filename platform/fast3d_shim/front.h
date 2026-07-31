/**
 * front.h — shim for the vendored mgb64 gfx_opengl.c backend.
 *
 * The mgb64 OpenGL backend was authored inside the GoldenEye tree and includes
 * "front.h" (its main-menu header). gfx_opengl.c references NONE of front.h's
 * symbols in mdkr64, so this is an empty stand-in that satisfies the include.
 * (See platform/fast3d/PROVENANCE.md.)
 */
#ifndef GFX_SHIM_FRONT_H
#define GFX_SHIM_FRONT_H
#endif
