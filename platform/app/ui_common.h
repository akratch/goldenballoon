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
// A full-width action. Four panels spelled this ImVec2(-1, kBtnSecondary().y)
// inline; naming it is what stops the fifth from picking a different height.
ImVec2 kBtnFullWidth();
// Navigation selection geometry. The selected destination is a rounded pill
// with a gold leading rule (see drawRailPanelItem), so both numbers belong to
// the design system rather than to the one function that draws them.
float  kNavPillRounding();  // 8
float  kNavRuleWidth();     // 3
// The ROM drop target's height, and the floor below which a side-by-side button
// pair stacks instead of shrinking.
float  kDropZoneHeight();   // 86
float  kPairMinWidth();     // 140
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
// There is no separate restart/live badge helper. "Takes effect at the next
// launch" has ONE spelling in this product -- the gold "Next launch" chip that
// RowStyle::restart draws -- and the two helpers that used to offer a second
// and third spelling ("* restart required", "* <live note>") were carried for
// several releases without a single call site. One fact, one chip.

// Filled actions. BrandPrimaryButton is the launcher’s persistent gold CTA;
// PrimaryButton remains cobalt for in-game overlay actions.
// Size defaults to the scaled primary button when passed a zero vector.
bool PrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0));
bool BrandPrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0));

// Bordered content card. CardBegin returns true when open (call CardEnd then).
bool CardBegin(const char *id, const ImVec4 &borderColor, float height);
void CardEnd();

// --- Settings composition ---------------------------------------------------
//
// A settings page has three levels of heading — page, group, setting — and a
// per-setting explanation that has to be readable without being a wall. These
// four primitives are that shape, so every group is built the same way instead
// of each one inventing its own spacing.

// Filled pill. `color` supplies both the text and, at low alpha, the fill.
// Non-interactive; it is a label, not a control.
void Chip(const char *text, const ImVec4 &color);
// The persistent marker for a setting that changes how the game plays. One
// spelling, one colour, everywhere — a player learns it once.
void GameplayChip();
// A small "?" that reveals a longer explanation on hover or focus. Use it only
// where a sentence genuinely cannot carry the answer.
void HelpMarker(const char *text);

// Group heading: section-font title, optional one-line subtitle, hairline.
void GroupHeader(const char *title, const char *subtitle);

// One setting's label row: name in body, optional chips, then a one-line
// description in the small font. `gameplay` draws the persistent marker and a
// warm left rule down the row so the marker survives scrolling past the chip.
struct RowStyle {
    bool gameplay = false;   // changes how the game plays
    bool restart = false;    // takes effect at the next launch
    bool experimental = false;
    const char *badge = nullptr;   // extra free-form chip, e.g. "next race"
    // Longer explanation, revealed from a marker on the LABEL line. It belongs
    // in the row style rather than after the call so it lands beside the name
    // instead of after a wrapped description, where nobody looks for it.
    const char *tooltip = nullptr;
};
void SettingLabel(const char *label, const char *description,
                  const RowStyle &style = RowStyle{});

// Warm-toned callout for a live gameplay caveat. Wider than a tooltip because
// the player needs to see it without hunting for it.
void CautionBox(const char *title, const char *body);

// Call immediately before BeginPopupModal for a confirmation dialog, and do
// NOT pass ImGuiWindowFlags_AlwaysAutoResize with it. Auto-resize sizes the
// window to its widest unwrapped item while TextWrapped wraps to the window,
// so a modal whose widest item is one button converges into a tall, one-word-
// per-line column (worst on small high-scale displays such as gaming
// handhelds). This pins a readable width — clamped to the work area — and
// leaves the height auto-fitted.
void ConfirmModalSize();

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
