// app_theme.h — Golden Balloon native identity: palette, metrics, embedded fonts.
//
// One place defines the look so every panel reads as one system. Brand cobalt,
// Amber Gold, website sky blue, and charcoal surfaces carry the public brand
// without importing web-only artwork. Fonts are the embedded Roboto Medium
// (app_font.h), rasterized at the physical pixel size for Retina crispness.
#ifndef MDKR_APP_THEME_H
#define MDKR_APP_THEME_H

#include "imgui.h"

struct AppFonts {
    ImFont *body    = nullptr;  // default UI text
    ImFont *title   = nullptr;  // page title / brand
    ImFont *section = nullptr;  // group heading, between title and body
    ImFont *small   = nullptr;  // captions, secondary, chips
};

namespace AppTheme {

// Apply the palette + metrics and build the font atlas. Call once after the
// ImGui context exists and before the first frame. fbScale is the framebuffer/
// logical ratio (2.0 on Retina) so glyphs are rasterized crisp.
void setup(float fbScale);

// Rebuild the font atlas when a window moves between monitors with different
// framebuffer scales. Call before the renderer backend's NewFrame hook. The
// ImGui 1.92 texture lifecycle retires the old atlas safely on the next render.
void refreshFramebufferScale(float fbScale);

// RX.2: apply the player's UI.Scale (0.75-2.0). Rescales the style metrics
// (padding, spacing, min sizes) from the captured base and sets FontGlobalScale
// so text grows too. This changes the geometry of the entire interface; call it
// only at a stable frame boundary, never while its slider is being dragged.
void setUiScale(float uiScale);

// Queue a committed UI-scale change while widgets are being drawn, then apply
// it at the next pre-NewFrame safe point so one frame never mixes metrics.
void requestUiScale(float uiScale);
void applyPendingUiScale();

// Current UI.Scale multiplier (1.0 until setUiScale is called). Panels use it to
// scale their explicit button/control sizes in step with the style metrics.
float uiScale();
unsigned atlasGeneration();
// Monotonic count of effective UI-scale changes. The launcher drag smoke uses
// this to prove a held pointer cannot repeatedly rebuild the whole layout.
unsigned uiScaleApplicationCount();

const AppFonts &fonts();

// Brand colors for direct use in panels (ImGui-normalized RGBA).
ImVec4 primary();     // brand cobalt — active location/selection
ImVec4 accent();      // amber gold — highlights, valid states
ImVec4 brandSky();    // bright website blue — wordmark only
ImVec4 surface();     // graphite card
ImVec4 subtle();      // secondary text
ImVec4 good();        // success/valid
ImVec4 bad();         // error/invalid

// Semantic additions for the settings redesign. `warn` is deliberately NOT the
// brand gold: gold already means "staged, needs a restart", and a player has to
// be able to tell "this will look different next launch" from "this changes how
// the game plays". `line` is the hairline used for rules and control borders;
// `raised` is the elevated surface a settings group sits on.
ImVec4 warn();        // orange — changes gameplay
ImVec4 line();        // hairline border
ImVec4 raised();      // elevated group surface
ImVec4 field();       // control background

// --- Navigation ------------------------------------------------------------
// The rail and the compact top-tab strip are the same idea in two geometries,
// so they read their states from one place. They used to hand-roll
// hex(0x3A3A3D) / hex(0x284B7C) / hex(0x424247) at four separate call sites,
// which is how two surfaces that must agree start disagreeing.
//
// navSelected() stays SOLID brand cobalt rather than a low-alpha tint, and that
// is load-bearing beyond taste: tests/check_launcher_tabs.py locates the
// selected destination by finding exactly one connected #315C98 component of a
// minimum area. A tinted selection would look calmer and leave that assertion
// unable to fail. The modernisation is in the geometry -- inset, rounded, with
// a gold leading rule -- which ui_launcher.cpp draws.
ImVec4 navSelected();       // active destination fill
ImVec4 navSelectedActive(); // active destination, pressed
ImVec4 navHover();          // neutral lift under the pointer; never a 2nd selection
ImVec4 navPressed();        // neutral, pressed

// --- Settings group header --------------------------------------------------
// Deliberately NOT the navigation colours. A settings group is an independent
// expandable region, not a mutually exclusive destination, and painting an open
// group cobalt made every open group look like another selected panel.
ImVec4 groupHeader();
ImVec4 groupHeaderHover();
ImVec4 groupHeaderActive();

// Keyboard/gamepad focus. One colour for focus everywhere, so "where am I" is
// never answered by two different visual languages.
ImVec4 focusRing();

// A destructive action's filled button. Returned as a triple so the one
// derivation lives here instead of being re-multiplied at each call site.
struct ButtonColors {
    ImVec4 normal, hovered, active, text;
};
ButtonColors destructiveButton();

// The QR code's own two values. A quiet zone must be light and the modules dark
// for a phone camera to read it, whatever the surrounding theme does, so these
// are fixed -- but named, because an unexplained IM_COL32_WHITE in a panel
// looks like an oversight rather than a decision.
ImVec4 qrLight();
ImVec4 qrDark();

// Convert a 0xRRGGBB literal (+ alpha) to an ImVec4.
ImVec4 hex(unsigned rgb, float a = 1.0f);

}  // namespace AppTheme

#endif  // MDKR_APP_THEME_H
