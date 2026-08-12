#ifndef MDKR64_UI_ONLINE_ROOM_H
#define MDKR64_UI_ONLINE_ROOM_H

struct LauncherAction;
struct LauncherState;

void OnlineRoomPanel_draw(LauncherState &state, LauncherAction &action);

// Machine-readable, windowless scenario inventory for render-test discovery.
// Returns zero after writing one versioned row per deterministic gallery case.
int OnlineRoom_dumpGalleryContract();

// Exact-action witness for token-gated production-input smoke scripts.
bool OnlineRoom_smokeActionResult(unsigned action, bool *accepted);

#endif
