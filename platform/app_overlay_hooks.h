// app_overlay_hooks.h — the ONLY overlay symbols the C engine references.
//
// The app shell registers function pointers here; the engine's event pump and
// frame-end call them. This keeps the engine free of any ImGui/C++ dependency:
// the overlay is drawn by the app, invoked by the engine at the right moments.
#ifndef MDKR64_APP_OVERLAY_HOOKS_H
#define MDKR64_APP_OVERLAY_HOOKS_H

// AppOverlayHooks + platformSetOverlayHooks are defined ONCE in the app-facing
// seam header (platform/app/), so the app and the engine share a single definition
// rather than two hand-synced typedefs. (The app can only reach platform/app; the
// engine can reach both, so the canonical copy lives in platform/app/.)
#include "app/engine_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called by the engine each frame. No-ops when no hooks are registered
// (the automation path never registers them, so it stays byte-identical).
int  platformOverlayProcessEvent(const void *sdl_event);
void platformOverlayService(void);
int  platformOverlayWantsInput(void);
int  platformOverlayWantsRender(void);
int  platformOverlayRender(void);

// --- Presentation-depth substitution hook -----------------------------------
//
// WHY THIS IS NOT ONE OF THE OVERLAY HOOKS ABOVE. Those run at the event-pump
// boundary and just before the swap -- both OUTSIDE the drawn frame. A tool
// that wants to change what the frame shows has to act in the one window
// between the fixed-tick camera finalizer latching this tick's projection
// record and render reading it, and neither overlay hook is in that window:
// service() runs before the tick that re-latches, render() runs after the
// display list has already been walked.
//
// WHERE IT IS CALLED FROM. platform/enh_draw_distance.c's
// mdkr_enh_draw_distance_begin_frame(), which game/src/tracks.c calls at the
// top of render_scene() -- after camera_obstruction_presentation_begin() and
// before any viewport_main() authors a lens. That is presentation depth in the
// literal sense the camera-obstruction work gave the phrase: the same scope in
// which the obstruction resolver is allowed to substitute a corrected camera,
// and for the same reason -- the authoritative tick is already over, so nothing
// substituted here can be read back by simulation.
//
// A hook that is not registered costs one null compare per drawn frame, which
// is what keeps every CLI invocation byte-identical to a build without it.
typedef void (*AppPresentationHookFn)(void);
void platformSetPresentationHook(AppPresentationHookFn hook);
void platformPresentationHook(void);

// The closing half of the same scope, called from platform/sim_hash.c's
// mdkr_render_census_post() -- the statement immediately after render_scene()
// returns, at both of thread3_main.c's call sites.
//
// WHY A SUBSTITUTION HAS TO BE CLOSED AND NOT JUST MADE. The projection the
// renderer consumes is not fully presentation-scoped in this port:
// cam_rebuild_native_projection() writes gPerspectiveMatrixF and
// gEffectiveCamHFOV, and the NEXT fixed tick's obj_visibility_tick rebuilds its
// cull planes from exactly those globals (tracks.c's
// scene_visibility_prepare_viewport -> func_8002A31C) without refreshing them
// from the authored record first. So a lens left substituted at the end of a
// drawn frame reaches the visibility test that gates AI RNG -- measured, not
// theorised: it diverges the v3 [SIMHASH] stream. Closing the scope here is
// what keeps the substitution inside the frame it was made for.
void platformSetPresentationEndHook(AppPresentationHookFn hook);
void platformPresentationEndHook(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_APP_OVERLAY_HOOKS_H
