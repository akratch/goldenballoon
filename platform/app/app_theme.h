// app_theme.h — mdkr64 app visual identity: palette, metrics, embedded fonts.
//
// One place defines the look so every panel reads as one system. Colors come
// from macos/BRANDING.md (Steel Blue primary, Amber Gold accent, charcoal
// surfaces). Fonts are the embedded Roboto Medium (app_font.h), rasterized at
// the physical pixel size for Retina crispness.
#ifndef MDKR_APP_THEME_H
#define MDKR_APP_THEME_H

#include "imgui.h"

struct AppFonts {
    ImFont *body  = nullptr;  // default UI text
    ImFont *title = nullptr;  // section headers / brand
    ImFont *small = nullptr;  // captions, secondary
};

namespace AppTheme {

// Apply the palette + metrics and build the font atlas. Call once after the
// ImGui context exists and before the first frame. fbScale is the framebuffer/
// logical ratio (2.0 on Retina) so glyphs are rasterized crisp.
void setup(float fbScale);

// RX.2: apply the player's UI.Scale (0.75-2.0). Rescales the style metrics
// (padding, spacing, min sizes) from the captured base and sets FontGlobalScale
// so text grows too. Cheap + idempotent (early-outs when unchanged), so the
// app can call it every frame from the live config value. Call after setup().
void setUiScale(float uiScale);

// Current UI.Scale multiplier (1.0 until setUiScale is called). Panels use it to
// scale their explicit button/control sizes in step with the style metrics.
float uiScale();

const AppFonts &fonts();

// Brand colors for direct use in panels (ImGui-normalized RGBA).
ImVec4 primary();     // steel blue — active/interactive
ImVec4 accent();      // amber gold — highlights, valid states
ImVec4 surface();     // graphite card
ImVec4 subtle();      // secondary text
ImVec4 good();        // success/valid
ImVec4 bad();         // error/invalid

// Convert a 0xRRGGBB literal (+ alpha) to an ImVec4.
ImVec4 hex(unsigned rgb, float a = 1.0f);

}  // namespace AppTheme

#endif  // MDKR_APP_THEME_H
