// ui_settings.h — settings panel generated from the video/gameplay schema.
//
// DKR ADAPTATION. mgb64's panel enumerates its own engine config registry
// (mgb_config_*) and opens an explicit staging session in the overlay, because
// that registry writes straight into live globals and needs Apply/Cancel to be
// safe. mdkr64 already has that transaction built into the config layer:
// mdkr_video_config_runtime_set() validates, persists, and then publishes only
// the LIVE-scope half, leaving RESTART-scope values staged in `desired` for the
// next launch. So this panel drives that API directly instead of layering a
// second staging model on top of it — every edit is already all-or-nothing.
#ifndef MDKR64_UI_SETTINGS_H
#define MDKR64_UI_SETTINGS_H

// Draw the settings sections (one per MdkrVideoCategory) inside the current
// content region. Shared verbatim by the launcher and the in-game F1 overlay;
// the only difference is `compact`, which drops the per-key help text so the
// overlay panel stays readable over a running race.
//
// Returns true when any setting was changed this frame (the caller may want to
// re-read live state).
bool Settings_draw(bool compact = false);

// Collect the settings the player has staged but that the running/next engine
// has not picked up yet, as "Key=Value" strings, so the launcher can pass them
// straight into this boot via --video-set instead of making the player relaunch
// twice. Writes at most `cap` entries; returns how many were written.
int Settings_collectStagedOverrides(const char **out, int cap);

// True when at least one RESTART-scope setting differs from what is live.
bool Settings_restartPending();

#endif  // MDKR64_UI_SETTINGS_H
