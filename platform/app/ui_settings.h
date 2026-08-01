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

// Apply the persisted app-shell scale after AppConfig::load() and after ImGui
// setup. Invalid/out-of-range preference text safely resolves to 1.0.
void Settings_loadUiScalePreference();

// Center of the rendered Frame limit combo in logical window coordinates.
// Returns false until the widget has been drawn. Smoke-only observation; it
// never mutates UI state or bypasses ImGui input handling.
bool Settings_smokeFrameLimitCenter(int *x, int *y);

// Center of the save-failure Retry control for Frame limit. This is exposed
// only so the shell smoke can route a real SDL click through ImGui after write
// access is restored in the same process.
bool Settings_smokeFrameLimitRetryCenter(int *x, int *y);

// Collect the settings the player has staged but that the running/next engine
// has not picked up yet, as "Key=Value" strings, so the launcher can pass them
// straight into this boot via --video-set instead of making the player relaunch
// twice. Writes at most `cap` entries; returns how many were written.
int Settings_collectStagedOverrides(const char **out, int cap);

// True when at least one RESTART-scope setting differs from what is live.
bool Settings_restartPending();

// Print the stable player-facing frame-limit labels used by the schema and
// rendered-settings smoke tests. This keeps the conservative release wording
// an executable contract rather than documentation that can silently drift.
void Settings_dumpSchemaContract();

#endif  // MDKR64_UI_SETTINGS_H
