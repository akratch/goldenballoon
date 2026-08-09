// ui_common.h — shared UI primitives + sizing constants for every panel.
//
// One place owns button sizes, spacing rhythm, and the repeated ImGui idioms
// (primary button, subtle caption, section header, card). Panels compose these
// so the app reads as one system and new panels (Part 2: bindings, diagnostics,
// modes) stay consistent by construction.
#ifndef MDKR64_UI_COMMON_H
#define MDKR64_UI_COMMON_H

#include "imgui.h"

namespace ui {

// --- Sizing (logical px) ---
// RX.2: the explicit button/control sizes multiply by the player's UI.Scale
// (AppTheme::uiScale) so they grow in step with the scaled fonts + style metrics
// on a handheld. They are functions (not constants) so the current scale is read
// at the call site each frame. Base sizes stay the desktop 1.0 values.
ImVec2 kBtnPrimary();    // 190 x 48
ImVec2 kBtnSecondary();  // 190 x 44
ImVec2 kBtnWide();       // 210 x 44
float  kTouchRowHeight(); // 44 px baseline for rows, tabs, and menu choices
float  kControlWidth(float multiplier = 1.0f);  // clamped to available width
float  kNavWidth();      // nav rail width (244)
// Layout height BrandRule() consumes. Published by its owner so a header that
// must reserve room for the rule cannot drift from what the rule draws.
float  kBrandRuleHeight();
// Vertical rhythm scale (fixed logical px; ItemSpacing already scales via style).
constexpr float kGapXS = 4.0f;
constexpr float kGapS  = 8.0f;
constexpr float kGapM  = 14.0f;
constexpr float kGapL  = 22.0f;

// --- Primitives ---
void Gap(float y);                                   // vertical spacer
void TextSubtle(const char *fmt, ...);               // secondary/muted text
void TextSubtleWrapped(const char *fmt, ...);        // muted text, wrapping at edge
// Path- and log-safe variant: no fixed formatting buffer, so long UTF-8 text
// is neither truncated nor split in the middle of a code point.
void TextSubtleUnformattedWrapped(const char *text);
void BrandWordmark();
void BrandRule();
// Direct 1:1 scrolling for a touch drag that begins on non-interactive content.
// Call inside the scroll-owning child before EndChild().
void TouchScrollCurrentWindow();
void SectionHeader(const char *title, const char *subtitle);
void RestartBadge();                                 // non-interactive muted chip
// The "this one changes under the running game" chip. Subtle, not accent: it
// informs where RestartBadge warns.
void LiveBadge(const char *text);

// Filled actions. BrandPrimaryButton is the launcher’s persistent gold CTA;
// PrimaryButton remains cobalt for in-game overlay actions.
// Size defaults to the scaled primary button when passed a zero vector.
bool PrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0));
bool BrandPrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0));

// Bordered content card. CardBegin returns true when open (call CardEnd then).
bool CardBegin(const char *id, const ImVec4 &borderColor, float height);
void CardEnd();

// --- Self-voicing ----------------------------------------------------------
// ImGui exposes no accessibility tree, so the shell says out loud what it has
// put the keyboard on. THE TWO Speak FUNCTIONS BELOW ARE THE ONLY PLACE IN THE
// APP THAT HAPPENS.
//
// Every settings row goes through drawKey(); every panel and collapsible goes
// through a section header. Announcing inside those shared helpers means a row
// added later is voiced the day it is added, and a row can only fall silent by
// being hand-rolled outside them -- which is exactly what
// tests/check_a11y_shell.py enumerates the schema to catch. Per-call-site
// announcements are how one control ends up mute and nobody notices.
//
// Both are a no-op unless Accessibility.Speech is on, so a player who does not
// use speech pays a pointer dereference per row and nothing else.

// Announces the item submitted immediately before this call, but only while
// that item holds keyboard/gamepad focus, and only when what it would say has
// changed. Call it directly after the widget: any text drawn in between
// becomes "the last item" and the focus test then reads the wrong thing.
void SpeakFocusedItem(const char *name, const char *value, const char *help);
// "You are now in <name>." Repeats are dropped, so calling it every frame from
// a header that draws every frame is correct and silent.
void SpeakSection(const char *name);
// Whether Accessibility.Speech is on. Exposed for callers that would otherwise
// build the value string for nothing.
bool SpeechEnabled();

}  // namespace ui

#endif  // MDKR64_UI_COMMON_H
