// tool_freecam.h — the free camera (F5).
//
// WHAT IT DETACHES, AND WHY THAT AND NOTHING ELSE. The camera-obstruction work
// established that rendering does not compute its own lens: it consumes the
// MdkrCameraProjection record the fixed-tick finalizer latched
// (game/src/camera.c's cam_rebuild_native_projection reads it back through
// cam_get_latched_effective_projection_for_viewport). This tool substitutes
// THAT record, inside the presentation scope, which is precisely the seam the
// obstruction resolver is allowed to substitute a corrected camera in — and for
// the same reason. By the time the scope is open the authoritative tick is
// over; nothing downstream of it is read back by simulation, so a substitution
// made here cannot become moved authoritative state.
//
// It deliberately does NOT touch the authored camera slots. gCameras is
// authoritative — platform/sim_hash.c mixes gCameras[PLAYER_FOUR] into the v3
// stream directly — so writing a pose there would move the hash and the purity
// gate would (correctly) reject the tool.
//
// The consequence, stated plainly: what detaches is the LENS, not the eye. The
// eye lives in the authored slots and in the obstruction runtime's resolved
// sidecar, and neither is substitutable from outside game/. So the free camera
// flies the projection — zoom, aspect and the near plane — rather than the
// viewpoint. A window that offered a viewpoint it could not actually move, or
// moved it by writing the authored slot, would be the worse of the two.
//
// RESTORING IS CEASING TO SUBSTITUTE. There is no saved pose and no re-apply.
// The finalizer relatches the authored record from authored inputs on every
// fixed tick, so the frame after re-attaching is built from bytes this tool
// never touched — byte-identical, not approximately restored.
// tests/check_tool_freecam.py is the assertion.
#ifndef MDKR64_TOOL_FREECAM_H
#define MDKR64_TOOL_FREECAM_H

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_FREECAM.
void ToolFreecam_draw(bool *open);

// The presentation-depth substitution, registered with
// platformSetPresentationHook(). C linkage because the engine holds it as a C
// function pointer. Inert unless the tool is open AND detached; on every other
// frame it is one compare.
extern "C" void ToolFreecam_presentationHook(void);

// The closing half, registered with platformSetPresentationEndHook(). It runs
// immediately after render_scene() returns and puts back the lens globals the
// drawn frame left behind — by re-deriving them through camera.c's authored
// rebuild, not by restoring a copy. See the long note at its definition: the
// projection RECORD is presentation-scoped, but the globals derived from it are
// read by the next fixed tick's visibility frusta, so a substitution has to be
// closed inside the frame it was made for or it moves authoritative state.
extern "C" void ToolFreecam_presentationEndHook(void);

#endif  // MDKR64_TOOL_FREECAM_H
