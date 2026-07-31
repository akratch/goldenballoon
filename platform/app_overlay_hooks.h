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
void platformOverlayProcessEvent(const void *sdl_event);
int  platformOverlayWantsInput(void);
void platformOverlayRender(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_APP_OVERLAY_HOOKS_H
