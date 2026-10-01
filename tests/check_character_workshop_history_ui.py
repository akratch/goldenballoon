#!/usr/bin/env python3
"""ROM-free rendered routing proof for source-bound Workshop history."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
import character_asset_probe as probe  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_humanoid_glb,
    make_portrait_png,
)
from character_validation_fixture import accepted_character_validation  # noqa: E402

PACKAGE_ID = "org.mdkr.history-proof"


def verify_status_capture_scope() -> None:
    source = (ROOT / "platform" / "app" / "ui_settings.cpp").read_text(
        encoding="utf-8"
    )
    setter = source[source.index("void setStatus("):
                    source.index("void drawWorkshopStatusHistory()")]
    if "g_workshopStatusCaptureDepth == 0u" not in setter:
        raise RuntimeError(
            "Workshop history is not isolated from unrelated settings status"
        )
    for function in (
        "bool Settings_activateCharacterWorkshopPrimaryAction()",
        "bool Settings_drawCharacterWorkshop(",
        "void Settings_serviceCharacterWork()",
        "SettingsCharacterStudioFrame Settings_drawCharacterOffsetStudio(",
    ):
        start = source.index(function)
        if "WorkshopStatusCapture capture" not in source[start:start + 500]:
            raise RuntimeError(
                f"character status source is outside scoped history: {function}"
            )
    generic = source[source.index("void reportResult("):
                     source.index("bool resultSucceeded(")]
    if "WorkshopStatusCapture" in generic:
        raise RuntimeError(
            "generic video status was incorrectly routed into Workshop history"
        )


def verify_workshop_ux_contract() -> None:
    """Pin the Workshop UX surfaces a rendered walk cannot report.

    The routes below speak every focusable control, so the readiness links,
    the vehicle echo and the deletion confirmation are proven by what the
    binary said. Static paragraphs, a keyboard chord and a tab-bar flag are
    not focusable and would survive their own deletion silently.

    Player-facing strings are matched literally -- changing one is the change
    this is here to notice. The two structural patterns allow any whitespace,
    so reformatting the call cannot fail the gate.
    """

    source = (ROOT / "platform" / "app" / "ui_settings.cpp").read_text(
        encoding="utf-8"
    )
    text = re.escape
    for pattern, lost in (
        (text('ImGui::Button("Choose a draft##identity-draft-handoff"'),
         "Identity lost its non-mutating named-draft handoff"),
        (text('ImGui::Button("Back to Identity##draft-handoff-back"'),
         "draft setup lost its safe Identity return"),
        # The compact layout reaches the off-screen tabs through the tab bar's
        # own popup button, and the UX trace reads the same helper.
        (text("ImGuiTabBarFlags_TabListPopupButton"),
         "the compact tab bar dropped its full-list popup button"),
        (r"BeginTabBar\(\s*\"##character-workshop-tabs\",\s*"
         r"characterWorkshopTabBarFlags\(compact\)\)",
         "the tab bar stopped sharing its flags with the UX trace"),
        # A later success must not erase the failure before it, and the depth
        # the trace reports has to be the depth the stack keeps.
        (text('ImGui::SeparatorText("Recent activity")'),
         "the Workshop lost its recent-status stack"),
        (r"g_workshopStatusHistory\.resize\(\s*"
         r"kWorkshopStatusHistoryLimit\s*\)",
         "the status stack stopped honouring the depth it reports"),
        # Undo belongs to the tool on screen, and an active text field keeps
        # its native chord.
        (text("ImGuiInputFlags_RouteFocused"),
         "the history chords stopped yielding to a focused editor"),
        (r"ImGui::Shortcut\(\s*ImGuiMod_Ctrl \| ImGuiKey_Z",
         "the visible tool lost its undo chord"),
        (r"ImGui::Shortcut\(\s*ImGuiMod_Ctrl \| ImGuiMod_Shift \| ImGuiKey_Z",
         "the visible tool lost its redo chord"),
        # One name for the vehicle workspace, in the header line and in the
        # Package tab's echo. The speech walk below reports the friendly name
        # of each control, not the words on it.
        (text('readiness.nextActionLabel + " \u00b7 in " +'),
         "the header next step stopped naming the workspace it opens"),
        (text('ImGui::SmallButton("Change in Offset Studio")'),
         "the Package tab's vehicle echo stopped naming Offset Studio"),
        # Plain-language fit status and the two input-expectation lines.
        (text('"Fit check: Character Select %s \u00b7 %u of %u vehicles '
              'reviewed since your last change"'),
         "the Offset Studio fit status left plain language"),
        (text('"A keyboard is required to enter source paths, names, and '
              'license details. Assigning, enabling, and testing an installed '
              'character is fully navigable with a controller."'),
         "the Workshop stopped saying which parts need a keyboard"),
        (text('"Player N = controller port N for local multiplayer."'),
         "the assignment rows stopped mapping players to controller ports"),
    ):
        if re.search(pattern, source) is None:
            raise RuntimeError(lost)

    handoff = source[source.index("bool returnToCharacterIdentityAfterDraft("):
                     source.index("bool drawCharacterPortraitStudio(")]
    for required in (
        "g_characterDraftIdentityHandoff.matches(entry->id, digest)",
        "draft == nullptr || draft->packageId != entry->id",
        "draft->baseSourceDigest != digest",
        "g_characterIdentityNameFocusRequested = true",
    ):
        if required not in handoff:
            raise RuntimeError("Identity return lost ownership/focus check: " + required)
    route_start = source.index('ImGui::Button("Choose a draft##identity-draft-handoff"')
    route = source[route_start:source.index("ui::SpeakFocusedItem(", route_start)]
    if "characterDigestHex(entry->source_sha256)" not in route:
        raise RuntimeError("Identity draft handoff lost its exact-source intent")
    for forbidden in ("saveCharacterDraft(", "applyCharacterDraftSnapshot(",
                      "buildCharacterDraftSource(", "reviseCharacterIdentity("):
        if forbidden in route:
            raise RuntimeError("Identity navigation acquired mutation authority: " + forbidden)
    lifecycle = source[source.index("bool drawCharacterDraftLifecycle("):
                       source.index("std::string characterRevisionTimestamp(",
                                    source.index("bool drawCharacterDraftLifecycle("))]
    if lifecycle.count("returnToCharacterIdentityAfterDraft(entry)") != 3:
        raise RuntimeError("Identity return must follow save, save-as, and resume success")
    tabs = source[source.index("void drawCharacterWorkshopTabs("):
                  source.index("CharacterWorkshopReadiness characterWorkshopReadiness(")]
    for required in (
        "CharacterWorkshopTab visible = CharacterWorkshopTab::Count",
        "CharacterWorkshop_observeTabSelection(",
        "g_characterWorkshopTab, g_characterWorkshopTabForceSelection, visible",
        "g_characterWorkshopTabForceSelection = selection.awaitingVisibility",
        "persistCharacterWorkshopTab(selection.tab, false)",
    ):
        if required not in tabs:
            raise RuntimeError("Workshop tabs lost deferred-selection policy: " + required)
    if "g_characterWorkshopTabForceSelection = false" in tabs:
        raise RuntimeError("Workshop tab request was cleared before target observation")
    raw = source[source.index("void drawCharacterRawIntakeEditor("):
                 source.index("const char *characterFailureKindLabel(")]
    compact = lambda value: re.sub(r"\s+", "", value)
    admission = re.search(r"const bool ready = (.*?);", raw, re.DOTALL)
    expected_admission = r"""
        intake.inspected && finalTransformAccepted &&
        characterRawPackageIdValid(intake.packageId) &&
        intake.displayName[0] != '\0' && intake.licensePath[0] != '\0' &&
        spdxValid && intake.attribution[0] != '\0' &&
        intake.sourceUrl[0] != '\0' && hasVehicle &&
        intake.targetHeight >= 0.1f && intake.targetHeight <= 10.0f &&
        intake.fallback >= 0 && intake.seat >= 0 && intake.head >= 0
    """
    if admission is None or compact(admission.group(1)) != compact(expected_admission):
        raise RuntimeError("Raw Build prerequisites changed; reconcile the guide and all-input regression")
    for required in (
        "facts.inspected = intake.inspected;",
        "facts.packageIdValid = characterRawPackageIdValid(intake.packageId);",
        r"facts.displayNamed = intake.displayName[0] != '\0';",
        r"facts.licenseSelected = intake.licensePath[0] != '\0';",
        "facts.spdxValid = validSpdx;",
        r"facts.attributionNamed = intake.attribution[0] != '\0';",
        r"facts.sourceUrlNamed = intake.sourceUrl[0] != '\0';",
        "facts.hasVehicle = intake.vehicles[0] || intake.vehicles[1] || intake.vehicles[2];",
        "facts.transformAccepted = acceptedTransform;",
        "facts.heightAllowed = intake.targetHeight >= 0.1f && intake.targetHeight <= 10.0f;",
        "facts.fallbackMapped = intake.fallback >= 0;",
        "facts.seatMapped = intake.seat >= 0;",
        "facts.headMapped = intake.head >= 0;",
        "finalGuide = guideFor(finalTransformAccepted, spdxValid);",
        "if (rawJumpOwner != jumpOwner)",
        "if (ImGui::IsItemVisible()) rawJump = RawStep::Count;",
    ):
        if compact(required) not in compact(raw):
            raise RuntimeError("Raw guide lost prerequisite/navigation parity: " + required)
    navigation = raw[raw.index("const auto openRawStep ="):
                     raw.index("const auto drawRawStepAnchor =")]
    if re.search(r"\b(?:queue\w*|build\w*|save\w*)\s*\(", navigation):
        raise RuntimeError("Raw guide navigation started an authoring operation")
    inspection_action = raw.split("const auto drawRawInspectionAction =", 1)[1].split(
        'drawRawStepAnchor(RawStep::Inspection, "Source inspection");', 1
    )[0]
    for marker in (
        "!g_characterManagerWorker.busy() && !g_characterPortableInstallWorker.busy() && !g_characterPackageInspection.busy()",
        "ImGui::Button(buttonLabel, ui::kBtnFullWidth()) && canInspect",
        "if (!requested) return false;",
        "if (reinspection && !saveCharacterRawIntake())",
        "const std::string modelPath = intake.modelPath;",
        "queueCharacterRawGlbInspection(",
    ):
        if compact(marker) not in compact(inspection_action):
            raise RuntimeError("Raw reinspection lost guarded source-preserving admission: " + marker)
    if not (inspection_action.index("if (!requested)") <
            inspection_action.index("saveCharacterRawIntake()") <
            inspection_action.index("queueCharacterRawGlbInspection(")):
        raise RuntimeError("Raw reinspection must save before the existing reset/queue transaction")
    for label, reinspection, guide_destination in (
        ("Inspect GLB model", "false", "true"),
        ("Reinspect GLB model##raw-source-reinspect", "true", "true"),
        ("Reinspect GLB model##raw-transform-reinspect", "true", "false"),
    ):
        expected = f'if (drawRawInspectionAction("{label}", {reinspection}, {guide_destination})) {{ return; }}'
        if compact(expected) not in compact(raw):
            raise RuntimeError("Inspection request must stop the frame before stale facts/autosave: " + label)
    if re.search(r"\b(?:resetCharacterRawGlbInspection|applyCharacterRawGlbInspection)\s*\(", raw):
        raise RuntimeError("Raw reinspection UI bypassed the existing worker publication transaction")
    transform = raw.split('drawRawStepAnchor(RawStep::Transform, "Scale and facing");', 1)[1].split(
        'static const char *forwards[]', 1
    )[0]
    for marker in (
        "const bool authoredHeightAllowed = std::isfinite(intake.targetHeight) && intake.targetHeight >= 0.1f && intake.targetHeight <= 10.0f;",
        "if (!authoredHeightAllowed)",
        "if (intake.inspected && authoredHeightAllowed && (!intake.inventory.detailedBounds || !transformReview.valid))",
        "Reinspection cannot repair this draft setting",
    ):
        if compact(marker) not in compact(transform):
            raise RuntimeError("Height-entry errors must not prescribe source reinspection: " + marker)
    if transform.index("if (!authoredHeightAllowed)") > transform.index("else if (!transformReview.valid)"):
        raise RuntimeError("Editable height must be diagnosed before blaming source bounds")
    for marker in (
        "openRawField(guide.nextField, guideTransformAvailable)",
        "openRawField(finalGuide.nextField, finalTransformAvailable)",
        "rawFieldFocus = {CharacterWorkshop_resolveRawField(field, transformReviewAvailable), false}",
        "rawFocusLastFrame != rawFocusFrame - 1",
        "ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel",
        "if (cancelled) rawFieldFocus = {}",
        "ImGuiKey_Escape", "ImGuiKey_GamepadFaceRight", "ImGuiKey_GamepadBack",
        "ImGui::IsMouseClicked(ImGuiMouseButton_Left)",
        "if (guideDestination) requestCharacterRawFieldFocus(rawFieldFocus, RawField::Inspection, canInspect)",
        "if (guideDestination) observeCharacterRawFieldFocus(rawFieldFocus, RawField::Inspection)",
    ):
        if compact(marker) not in compact(raw):
            raise RuntimeError("Raw blocker navigation lost field routing/cancellation: " + marker)
    interruption = raw.split("if (rawFieldFocus.target != RawField::Count)", 1)[1].split(
        "rawFocusLastFrame = rawFocusFrame;", 1
    )[0]
    for marker in (
        "!io.InputQueueCharacters.empty()",
        "if (io.KeyCtrl || io.KeySuper) { for (ImGuiKey key : {ImGuiKey_A, ImGuiKey_C, ImGuiKey_V, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z}) { cancelled |= ImGui::IsKeyPressed(key, false); } }",
        "ImGuiKey_KeypadEnter, ImGuiKey_Backspace, ImGuiKey_Delete, ImGuiKey_Insert",
    ):
        if compact(marker) not in compact(interruption):
            raise RuntimeError("Raw focus must yield to character-free manual editing: " + marker)
    if re.search(r"ImGui::(?:Shortcut|SetKeyOwner|SetShortcutRouting)\s*\(", interruption):
        raise RuntimeError("Focus cancellation must observe manual editing without consuming it")
    owner_reset = raw.split("if (rawJumpOwner != jumpOwner)", 1)[1].split("}", 1)[0]
    if "rawFieldFocus = {" not in owner_reset:
        raise RuntimeError("Raw field intent survived an exact source/draft change")
    if "rawFieldFocus = {}" not in navigation:
        raise RuntimeError("Section revisit must cancel pending field navigation")
    for field in ("PackageId", "DisplayName", "License", "Spdx", "Attribution",
                  "SourceUrl", "Vehicles", "Height", "Transform", "Build"):
        for action in ("request", "observe"):
            marker = f"{action}CharacterRawFieldFocus(rawFieldFocus, RawField::{field}"
            if marker not in raw:
                raise RuntimeError("Raw field has no actual-control focus binding: " + marker)
    focus_helpers = source.split("void requestCharacterRawFieldFocus(", 1)[1].split(
        "bool drawCharacterRawChoice(", 1
    )[0]
    for marker in (
        "CharacterWorkshop_requestRawFocus(focus, field, enabled)",
        "ImGui::SetKeyboardFocusHere()", "ImGui::SetNavCursorVisible(true)",
        "ImGuiItemFlags_Disabled", "ImGui::IsItemVisible()",
        "CharacterWorkshop_observeRawFocus(focus, field, visible, ImGui::IsItemFocused() || ImGui::IsItemActive())",
    ):
        if compact(marker) not in compact(focus_helpers):
            raise RuntimeError("Raw focus lost native deferred/disabled acknowledgement: " + marker)
    if re.search(r"\b(?:queue\w*|build\w*|save\w*|ActivateItem\w*)\s*\(", focus_helpers):
        raise RuntimeError("Raw field focus attempted authoring or action activation")
    # The vendored API queues tabbing focus, which activates text entry only.
    # Pin that real implementation distinction before using it on Build/Accept.
    imgui = (ROOT / "lib" / "imgui" / "imgui.cpp").read_text(encoding="utf-8")
    if compact("if ((g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing) && (result->ItemFlags & ImGuiItemFlags_Inputable) == 0) g.NavMoveFlags &= ~ImGuiNavMoveFlags_Activate;") not in compact(imgui):
        raise RuntimeError("Review native focus API: navigation must not activate non-inputable controls")
    verify_workshop_containment_contract(source)


def verify_workshop_containment_contract(source: str) -> None:
    """Keep full authored text separate from IDs and bounded field geometry.

    Rendered qualification must still exercise duplicate names, literal ##,
    long Unicode names/unbroken paths, narrow panels and 200 percent scale.
    """
    row = source.split("bool drawCharacterLibraryRow(", 1)[1].split(
        "const MdkrModernCharacterEntry *drawCharacterLibrary(", 1
    )[0]
    compact = lambda text: re.sub(r"\s+", "", text)
    for marker in (
        "ImGui::PushID(packageId);",
        'ImGui::Selectable("##character-library-row", selected, 0,',
        "label.c_str(), nullptr, false, wrapWidth);",
        "textSize.y + padding.y * 2.0f",
        "ImGui::GetCursorScreenPos();",
        "origin.x + width, origin.y + height",
        "wrapWidth, &clip);",
        "ImGui::IsItemVisible()",
    ):
        if compact(marker) not in compact(row):
            raise RuntimeError("wrapped package row lost " + marker)
    if "ImGui::Text" in row or "ImGui::Dummy" in row:
        raise RuntimeError("row drawing replaced the selectable's focused last item")
    library = source.split(
        "const MdkrModernCharacterEntry *drawCharacterLibrary(", 1
    )[1].split("bool drawCharacterAssignments(", 1)[0]
    if library.count("drawCharacterLibraryRow(") != 2:
        raise RuntimeError("compact and rail libraries must share wrapped package-ID rows")
    if "ImGui::Selectable(" in library:
        raise RuntimeError("library regained name-derived selection IDs")
    for marker in (
        "ImGui::SetNextWindowSizeConstraints(",
        'ImGui::TextWrapped("Selected: %s", workshopEntry->display_name);',
        "entry->narration_name",
    ):
        if marker not in library:
            raise RuntimeError("library lost bounded/full-value presentation: " + marker)
    for field in (
        "character-display-name", "character-short-name",
        "character-narration-name", "character-sort-label",
        "character-portrait-import-path", "character-portrait-path",
        "character-workshop-library", "character-minimap-colour",
    ):
        if f'"##{field}"' not in source:
            raise RuntimeError("stacked field lost its independent input ID: " + field)
        if re.search(r'"[^"\n#]+##' + re.escape(field) + '"', source):
            raise RuntimeError("full-width field regained an overflowing side label: " + field)
    if 'ImGui::TextWrapped("%s", spdxError.c_str());' not in source:
        raise RuntimeError("raw SPDX corrective errors lost wrapping")
    assignments = source.split("bool drawCharacterAssignments(", 1)[1].split(
        "void drawSkippedCharacterInventory(", 1
    )[0]
    for marker in (
        "drawCharacterLibraryRow(entry->id, item, selected == entry->id,",
        "const bool assignable = readiness.readyToPlay;",
        "if (!assignable) ImGui::BeginDisabled();",
        "AppConfig::setAndSave(key, entry->id);",
    ):
        if compact(marker) not in compact(assignments):
            raise RuntimeError("assignment selection lost exact identity/gating: " + marker)
    if "ImGui::Selectable(item.c_str()" in assignments:
        raise RuntimeError("assignment selection regained authored-name IDs")
    motion = source.split("void drawCharacterMotionAuthoringStudio(", 1)[1].split(
        "bool drawCharacterRigStudio(", 1
    )[0]
    for marker in (
        'ImGui::TreeNodeEx("##secondary-chain", ImGuiTreeNodeFlags_DefaultOpen, "%s", title.c_str());',
        "ImGui::PushID(static_cast<int>(node));",
        'const std::string itemLabel = label + "###secondary-root";',
        "ImGui::PushID(static_cast<int>(edit.joints[joint].node));",
        'const std::string itemLabel = label + "###secondary-child";',
        "chain.rootNode = node;",
        "chain.joints[chain.jointCount++] = joint;",
    ):
        if compact(marker) not in compact(motion):
            raise RuntimeError("secondary node selection lost fixed identity: " + marker)
    raw_choices = source.split("bool drawCharacterRawChoice(", 1)[1].split(
        "void drawCharacterRawIntakeEditor(", 1
    )[0]
    for marker in (
        "CharacterWorkshop_filterRawChoices(choices, query, selected)",
        "ImGui::TextUnformatted(label);",
        'const std::string comboLabel = std::string("###") + label;',
        "beginCharacterLiteralCombo(comboLabel.c_str(), preview)",
        "if (appearing || filterChanged)",
        "clipper.Begin(static_cast<int>(filtered.indices.size()));",
        "const int index = filtered.indices[static_cast<size_t>(position)];",
        "clipper.IncludeItemByIndex(filtered.selectedPosition);",
        "if (appearing && selected == index)",
        "ImGui::SetItemDefaultFocus();",
        "ImGui::SetScrollHereY(0.5f);",
        "ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened",
        "searchOwner != sourceOwner || searchWidget != widget",
        "if (ownerChanged && !appearing)",
        "ImGui::CloseCurrentPopup();",
        'ImGui::Button("Clear search", ui::kBtnFullWidth())',
        'ui::TextSubtleWrapped("Selected: %s", choices[static_cast<size_t>(selected)].c_str());',
        'ImGui::TextWrapped("Source name: %s", choices[static_cast<size_t>(detailIndex)].c_str());',
        'ImGui::BeginChild("##raw-choice-detail", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f), ImGuiChildFlags_Borders)',
        'ui::SpeakFocusedItem("Full source name",',
        "ImGui::PushID(index);",
        'drawCharacterLiteralChoice("##raw-choice", choice.c_str(), selected == index)',
        'ui::SpeakFocusedItem(choice.c_str(), selected == index ? "selected" : "available", help);',
    ):
        if compact(marker) not in compact(raw_choices):
            raise RuntimeError("raw choices lost fixed-height literal-label semantics: " + marker)
    if "SetKeyboardFocusHere" in raw_choices:
        raise RuntimeError("mapping search must not steal text-entry focus on popup frames")
    if len(re.findall(r"\bselected\s*=(?!=)", raw_choices)) != 1:
        raise RuntimeError("mapping changes must remain solely explicit row selection")
    raw = source.split("void drawCharacterRawIntakeEditor(", 1)[1].split(
        "const char *characterFailureKindLabel(", 1
    )[0]
    calls = re.findall(r"drawCharacterRawChoice\((.*?)\);", raw, re.DOTALL)
    if len(calls) != 3 or any(
        compact("jumpOwner, rawFieldFocus, RawField::") not in compact(call)
        for call in calls
    ):
        raise RuntimeError("all raw mapping selectors must be bound to the exact draft/source")
    for marker in (
        "requestCharacterRawFieldFocus(fieldFocus, field)",
        "if (!open) observeCharacterRawFieldFocus(fieldFocus, field)",
    ):
        if marker not in raw_choices:
            raise RuntimeError("Raw mapping focus must bind to its combo, not trailing text: " + marker)
    literal_row = source.split("bool drawCharacterLiteralChoice(", 1)[1].split(
        "bool beginCharacterLiteralCombo(", 1
    )[0]
    for marker in (
        "const float height = ImGui::GetTextLineHeight();",
        "ImGui::Selectable(widgetLabel, selected, 0, ImVec2(width, height));",
        "ImGui::IsItemVisible()",
        "ImGui::GetColorU32(ImGuiCol_Text), visibleText, nullptr, 0.0f, &clip);",
    ):
        if compact(marker) not in compact(literal_row):
            raise RuntimeError("literal choices lost native identity/geometry: " + marker)
    if not (literal_row.index("ImGui::PushStyleColor(") <
            literal_row.index("ImGui::Selectable(") <
            literal_row.index("ImGui::PopStyleColor();") <
            literal_row.index("->AddText(")):
        raise RuntimeError("literal choice text opacity was not restored before painting")
    literal_combo = source.split("bool beginCharacterLiteralCombo(", 1)[1].split(
        "void drawCharacterMotionAuthoringStudio(", 1
    )[0]
    for marker in (
        "ImDrawList *draw = ImGui::GetWindowDrawList();",
        "const float width = ImGui::CalcItemWidth();",
        "const float height = ImGui::GetFrameHeight();",
        'ImGui::BeginCombo(label, "");',
        "origin.x + width - height",
        "previewVisible && preview != nullptr",
        "colour, preview, nullptr, 0.0f, &clip);",
    ):
        if compact(marker) not in compact(literal_combo):
            raise RuntimeError("literal combo lost native preview semantics: " + marker)
    if not (literal_combo.index("ImGui::GetWindowDrawList();") <
            literal_combo.index("ImGui::BeginCombo(") <
            literal_combo.index("draw->AddText(")):
        raise RuntimeError("combo preview no longer paints into its captured parent")
    for section in (literal_row, literal_combo):
        if "ImGui::Text" in section or "ImGui::Dummy" in section:
            raise RuntimeError("literal painting replaced the native focused item")
    for marker in (
        "itemLabel.c_str(), label.c_str(), role.joint ==",
        "visible.c_str(), visible.c_str(), draft->id == selectedId",
        "draft.name.c_str(), draft.name.c_str(), false",
        "label.c_str(), visible.c_str(), draft.id == intake.draftId",
        "label.c_str(), visible.c_str(), inventory.selected == static_cast<int>(index)",
    ):
        if compact(marker) not in compact(source):
            raise RuntimeError("an authored selector lost its separate literal text: " + marker)


def install_fixture(root: Path, *, display_name: str = "History Proof",
                    package_id: str = PACKAGE_ID,
                    source_name: str = "source") -> Path:
    source = root / source_name
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "history-proof.mdkrchar"
    model.write_bytes(make_humanoid_glb(with_lod=True))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, package_id, display_name, "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/history-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[100, 180, 240], rig_mode="humanoid-retarget-v1",
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    probe.build_package(
        model, manifest_path, license_path, package, portrait_path=portrait
    )
    with accepted_character_validation(manager):
        manager.install(package, characters)
    return characters


def run_delete_confirmation(binary: Path, root: Path, characters: Path,
                            *, collision: bool) -> None:
    run_root = root / ("delete-collision" if collision else "delete-unique")
    prefs = run_root / "prefs"
    saves = run_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=package\n",
        encoding="utf-8",
    )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "12",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_SMOKE_CHARACTER_REMOVAL_CONFIRMATION":
            "mdkr64-character-removal-confirmation-v1",
    })
    process = subprocess.run(
        [str(binary)], cwd=run_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    marker = (
        f"character-delete-confirmation package={PACKAGE_ID} "
        f"collision={1 if collision else 0} "
        f"confirm={'package-id' if collision else 'display-name'} "
        "hold-ms=1250 external-source=retained"
    )
    if process.returncode != 0 or marker not in process.stdout:
        raise RuntimeError(
            "friendly deletion confirmation did not render its "
            f"{'collision' if collision else 'unique-name'} policy\n" +
            process.stdout[-8000:]
        )


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def run_gamepad_import_focus(binary: Path, root: Path,
                             characters: Path) -> None:
    tab_root = root / "vehicles-gamepad-import-focus"
    prefs = tab_root / "prefs"
    saves = tab_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=vehicles\n"
        "ui_scale=2.0\n",
        encoding="utf-8",
    )
    (tab_root / "video.ini").write_text(
        "[Accessibility]\nSpeech=1\n", encoding="utf-8"
    )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "40",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(tab_root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_SMOKE_A11Y_WALK": "1",
        "MDKR_APP_SMOKE_INPUT": "gamepad",
        "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
        "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS": "1",
        "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS_TOKEN":
            "mdkr64-character-import-focus-v1",
        "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN": "1",
        "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN_TOKEN":
            "mdkr64-character-workshop-return-v1",
        "MDKR_A11Y_TRACE": "1",
    })
    process = subprocess.run(
        [str(binary)], cwd=tab_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    required = (
        "character import-focus shortcut queued input=gamepad",
        "character-import-focus shortcut=1 mutation=0",
        "text=Browse for another character source",
        "character Workshop return shortcut queued input=gamepad",
        "character-workshop-return shortcut=1 mutation=0",
        "character-workshop-return focus=primary",
    )
    if process.returncode != 0 or any(
            marker not in process.stdout for marker in required):
        raise RuntimeError(
            "controller Back/View could not escape the dense editor and "
            "focus safe character import\n" + process.stdout[-8000:]
        )


def run_tab(binary: Path, root: Path, characters: Path, tab: str,
            expected: tuple[str, ...], rom: Path | None) -> None:
    tab_root = root / tab
    prefs = tab_root / "prefs"
    saves = tab_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    accessible = tab in ("overview", "package", "rig-motion", "profile",
                         "vehicles", "performance")
    preferences = (
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        f"character_workshop_last_tab={tab}\n" +
        ("ui_scale=2.0\n" if accessible else "")
    )
    if rom is not None:
        preferences += f"rom_path={rom}\n"
    (prefs / "mdkr64_app.ini").write_text(
        preferences, encoding="utf-8"
    )
    if accessible:
        (tab_root / "video.ini").write_text(
            "[Accessibility]\nSpeech=1\n", encoding="utf-8"
        )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "520" if accessible else "8",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(tab_root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if tab == "overview":
        # One real Workshop operation, so the recent-status stack has
        # something to keep and acknowledge. It reads private recovery
        # metadata only and installs nothing.
        #
        # The check is a real subprocess, timed by the wall clock; the walk's
        # Tab presses and its Ctrl+I/Escape shortcuts are timed by frames. A
        # running character job disables the Workshop's controls and both
        # shortcuts, so which of them the walk met depended on the frame rate:
        # paced at the display's refresh the check ended before the shortcuts;
        # unpaced (seen after the display had been off a long time) it
        # outlasted them. Settle it first, as a player would see the Workshop.
        environment.update({
            "MDKR_APP_SMOKE_CHARACTER_RECOVERY_ACTION": "check",
            "MDKR_APP_SMOKE_CHARACTER_RECOVERY_TOKEN":
                "mdkr64-character-recovery-v1",
            "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS": "1",
            "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS_TOKEN":
                "mdkr64-character-jobs-v1",
        })
    if accessible:
        environment.update({
            "MDKR_APP_SMOKE_A11Y_WALK": "1",
            "MDKR_APP_SMOKE_INPUT": "keyboard",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS": "1",
            "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS_TOKEN":
                "mdkr64-character-import-focus-v1",
            "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN": "1",
            "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN_TOKEN":
                "mdkr64-character-workshop-return-v1",
            "MDKR_A11Y_TRACE": "1",
        })
    process = subprocess.run(
        [str(binary)], cwd=tab_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{tab} history route exited {process.returncode}\n"
            f"{process.stdout[-8000:]}"
        )
    expected_layout = "compact" if accessible else "wide"
    expected_scale = "2.0" if accessible else "1.0"
    expected_popup = "1" if accessible else "0"
    # Only the computed fields and the four un-gated P1/P4/P5 route names
    # remain here. The readiness links, header next step, undo scoping,
    # vehicle echo, deletion policy, controller-port line and keyboard notice
    # are asserted below from what the binary rendered or from the source that
    # draws them -- not from a literal this same print emits.
    ux_marker = (
        "character-workshop-ux "
        f"layout={expected_layout} scale={expected_scale} "
        f"tab-list-popup={expected_popup} status-history=3 "
        "library-next=1 rig-band=1 overlay-guidance=1 "
        "pad-play-setup=complete"
    )
    if ux_marker not in process.stdout:
        raise RuntimeError(
            f"{tab} omitted the complete responsive Workshop UX contract\n"
            + process.stdout[-8000:]
        )
    observed = set(
        re.findall(r"character-history tool=(.+?) source=", process.stdout)
    )
    if observed != set(expected):
        raise RuntimeError(
            f"{tab} rendered history controls for {sorted(observed)}, "
            f"expected {sorted(expected)}\n{process.stdout[-8000:]}"
        )
    if "character-workshop-primary kind=open-readiness-task " not in process.stdout:
        raise RuntimeError(
            f"{tab} bypassed the selected installed character's readiness "
            "task in the persistent primary action\n" + process.stdout[-8000:]
        )
    if (
        "character-source-secondary-picker rendered=1 focusable=1 "
        "mutation=deferred-review" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} omitted the secondary source picker while the "
            "persistent primary action owned installed-character readiness\n" +
            process.stdout[-8000:]
        )
    if accessible and (
        "text=Browse for another character source" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} keyboard/speech traversal could not reach the secondary "
            "source picker\n" + process.stdout[-8000:]
        )
    if accessible and (
        "character-import-focus shortcut=1 mutation=0" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} did not route the import-focus shortcut through the "
            "Workshop\n" + process.stdout[-8000:]
        )
    if accessible and any(
        marker not in process.stdout for marker in (
            "character-workshop-return shortcut=1 mutation=0",
            "character-workshop-return focus=primary",
        )
    ):
        raise RuntimeError(
            f"{tab} could not return from its dense editor to the persistent "
            "launcher action row\n" + process.stdout[-8000:]
        )
    if tab == "profile":
        profile_marker = (
            "character-donor-profile-gallery package=" + PACKAGE_ID
            + " profiles=10 glyph=project-owned-metric-badge "
            + f"copyrighted-art=0 exact={1 if rom else 0} "
            "responsive=1 keyboard=1"
        )
        if profile_marker not in process.stdout:
            raise RuntimeError(
                "vehicle route did not render the project-owned donor "
                "profile library\n" + process.stdout[-8000:]
            )
        for spoken in (
            "text=Krunch gameplay profile",
            "text=Diddy gameplay profile",
        ):
            if spoken not in process.stdout:
                raise RuntimeError(
                    "profile keyboard/speech walk missed " + spoken
                    + "\n" + process.stdout[-8000:]
                )
    if tab == "overview":
        # Readiness leads, and every unfinished area is one press from its
        # own workspace.
        for spoken in (
            "text=Rig and motion, Review. Opens the Workshop workspace that "
            "can complete this readiness area.",
            "text=Vehicle fit, Review. Opens the Workshop workspace that "
            "can complete this readiness area.",
            "text=Review rig and motion, Rig & Motion. Opens the single "
            "highest-priority unfinished Workshop area.",
            "text=Review rig and motion, Rig & Motion. Opens the selected "
            "character's highest-priority next step",
        ):
            if spoken not in process.stdout:
                raise RuntimeError(
                    "Overview lost a readiness jump-link: " + spoken + "\n"
                    + process.stdout[-8000:]
                )
        # The recovery check above reports through setStatus; its result has
        # to survive in the stack with a reachable way to dismiss it. The walk
        # began only once the check had settled.
        if not re.search(r"Character Workshop serviceFrames=\d+ settled=1",
                         process.stdout):
            raise RuntimeError(
                "the overview walk did not wait for the recovery check to "
                "settle\n" + process.stdout[-8000:]
            )
        if ("text=Acknowledge Workshop message, Failed-import recovery is "
                "up to date." not in process.stdout):
            raise RuntimeError(
                "the Workshop status stack kept no acknowledgeable result "
                "after a real operation\n" + process.stdout[-8000:]
            )
    if tab == "package":
        # One name for the vehicle workspace, and the Package tab echoes the
        # choice it does not own.
        echo = (
            "text=Change vehicles to fit and use, Car, Hovercraft, Plane. "
            "Opens Offset Studio, where presentation eligibility and each "
            "enabled vehicle's exact fit are reviewed."
        )
        if echo not in process.stdout:
            raise RuntimeError(
                "Package tab lost its Offset Studio vehicle echo\n"
                + process.stdout[-8000:]
            )
    if tab == "rig-motion":
        marker = (
            "character-animation-intent package=" + PACKAGE_ID +
            " semantics=13 static-detection=1 authored-toggle=1 "
            "disabled-preserved=1 "
            "fallback=reviewed-reference-or-package "
            "inspect-handoff=1 active-revision-guard=1 human-labels=1 "
            "responsive=table-or-cards"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "rig and motion route omitted explicit per-semantic animation "
                "intent\n" + process.stdout[-8000:]
            )
        suggestion_marker = (
            "character-rig-suggestion package=" + PACKAGE_ID +
            " joints=16 roles=16 named=16 hierarchy=0 "
            "common-ancestor-repairs=0 complete=1 structurally-valid=1 "
            "review-required=1"
        )
        if suggestion_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted its complete review-required structural "
                "proposal\n" + process.stdout[-8000:]
            )
        review_marker = (
            "character-rig-review package=" + PACKAGE_ID +
            " anatomy-tasks=5 motion-presets=5 source-bound=1 "
            "reset-on-basis-change=1 exact-test-handoff=1 "
            "context-claim=manual-after-save"
        )
        if review_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted its source-bound anatomy checklist and "
                "exact motion-battery handoff\n" + process.stdout[-8000:]
            )
        motion_marker = (
            "character-motion-authoring constraints=16 secondary-chains=8 "
            "dynamic-joints=64 direct-path=1 presets=4 normalized-axes=1 "
            "source-bound=1 undo=rig-review-reset spoken=1 responsive="
        )
        if motion_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted bounded direct-manipulation limits and "
                "secondary-chain authoring\n" + process.stdout[-8000:]
            )
        if "text=Apply 16-role humanoid proposal" not in process.stdout:
            raise RuntimeError(
                "keyboard/speech traversal could not reach the structural "
                "proposal action\n" + process.stdout[-8000:]
            )
        if "text=Standing silhouette" not in process.stdout:
            raise RuntimeError(
                "keyboard/speech traversal could not reach the motion battery\n"
                + process.stdout[-8000:]
            )
    if tab == "vehicles":
        marker = (
            "character-spatial-controls package=" + PACKAGE_ID +
            " planes=front,side,top placement=ground-or-seat yaw=context "
            "contacts=4 copy=vehicle-only undo=fit-history"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "vehicle route did not render the synchronized placement, "
                "facing, contact, copy, and undo contract\n" +
                process.stdout[-8000:]
            )
        studio_marker = (
            "character-offset-studio package=" + PACKAGE_ID +
            " contexts=select,car,hovercraft,plane "
            "guided-fit=1 "
            "workflow=preview,measure,fine-tune,evidence,review "
            "exact-rom-preview=1 "
            "inline-exact-still=managed-cache,scene-held-midpoint-auto-return "
            "compact-preview=1 "
            "disabled-package-preview=1 "
            "measured-starting-point=vertical,facing,contacts-least-squares "
            "quality-bands=datum,facing,proportions "
            "camera-occlusion=exact-visual-only "
            "reset=package-anchor "
            "review=current-source-and-fit"
        )
        if studio_marker not in process.stdout:
            raise RuntimeError(
                "offset studio omitted its exact-preview and measured-fit "
                "contract\n" + process.stdout[-8000:]
            )
        facing_columns = 1 if accessible else 4
        facing_marker = (
            "character-facing-studio package=" + PACKAGE_ID +
            " candidates=+z,-z,+x,-x equal-thumbnails=1 "
            f"responsive-columns={facing_columns} available=0 "
            "source-fit-context=current "
            "comparison=1p-select.idle@500-bright "
            "selection=reversible-global-yaw capture=model-alpha exact-rom=1"
        )
        if facing_marker not in process.stdout:
            raise RuntimeError(
                "offset studio omitted its equal exact-renderer facing "
                "candidate workflow\n" + process.stdout[-8000:]
            )
        spoken_controls = [
            "text=Prepare +Z view",
            "text=Continue fit: Character select",
            "text=Exact preview camera layout",
        ]
        if rom is not None:
            spoken_controls.append("text=Open exact Character select preview")
            spoken_controls.append("text=Capture exact still")
        for spoken in spoken_controls:
            if spoken not in process.stdout:
                raise RuntimeError(
                    "offset studio keyboard/speech walk missed " + spoken +
                    "\n" + process.stdout[-8000:]
                )
    if tab == "test":
        marker = (
            "character-pose-inspector package=" + PACKAGE_ID +
            " semantics=13 defaultPose=4 defaultPhase=500 "
            "held-presets=0,500,1000 transition=exact-runtime-bidirectional "
            "transitionDwellMs=1000 transitionCapture=disabled "
            "view=0,0 pitchRange=-90:90 top=exact lighting=0 "
            "capture=scene-or-model-alpha-png-create-only "
            "performanceEvidence=session-excluded"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "test route did not render the complete session-only pose "
                f"inspector contract\n{process.stdout[-8000:]}"
            )
    if tab == "performance":
        marker = (
            "character-performance-targets package=" + PACKAGE_ID +
            " targets=quality,balanced,performance,four-player custom=1 "
            "sourceBias=0.0 localBias=0.0 players=4 selectedLod=0 "
            "exactAssembly=1"
        )
        transition = re.search(
            r"lodBands=(\d+) inspectionHeight=12\.0 inspectionLod=0 "
            r"monotonic=([01]) dramatic=([01]) "
            r"importCeiling=unchanged history=performance "
            r"projectedPolicy=1 hysteresis=8% fallback=distance "
            r"blendOrder=posed-centroid-per-view "
            r"transparentSelfSort=visual-review "
            r"transparentSceneQueue=visual-review",
            process.stdout,
        )
        if marker not in process.stdout or transition is None or \
                int(transition.group(1)) < 1:
            raise RuntimeError(
                "performance route did not use the exact runtime-equivalent "
                f"target and assembly policy\n{process.stdout[-8000:]}"
            )
        for spoken in (
            "text=Quality", "text=Balanced", "text=Performance",
            "text=Four-player", "text=Authored LOD preference",
            "text=Projected character height",
            "text=Exact projected-height LOD bands",
        ):
            if spoken not in process.stdout:
                raise RuntimeError(
                    "performance keyboard/speech walk missed " + spoken +
                    "\n" + process.stdout[-8000:]
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve() if args.rom is not None else None
    try:
        verify_status_capture_scope()
        verify_workshop_ux_contract()
        with tempfile.TemporaryDirectory(
                prefix="mdkr-workshop-history-") as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            before = inventory(characters)
            routes = (
                ("overview", ()),
                ("package", ()),
                ("identity", ("Identity",)),
                ("rig-motion", ("Rig & Motion",)),
                ("profile", ("Gameplay",)),
                ("vehicles", ("Offset Studio",)),
                ("performance", ("Performance",)),
                ("test", ("Test",)),
            )
            for tab, tools in routes:
                run_tab(binary, root, characters, tab, tools, rom)
            run_gamepad_import_focus(binary, root, characters)
            run_delete_confirmation(
                binary, root, characters, collision=False
            )
            if inventory(characters) != before:
                raise RuntimeError(
                    "rendering history controls mutated installed character bytes"
                )
            collision_root = root / "collision-fixture"
            collision_root.mkdir()
            collision_characters = install_fixture(collision_root)
            install_fixture(
                collision_root,
                package_id="org.mdkr.history-proof-twin",
                source_name="source-twin",
            )
            collision_before = inventory(collision_characters)
            run_delete_confirmation(
                binary, collision_root, collision_characters,
                collision=True,
            )
            if inventory(collision_characters) != collision_before:
                raise RuntimeError(
                    "rendering collision-aware deletion confirmation mutated "
                    "installed character bytes"
                )
    except (OSError, RuntimeError, subprocess.SubprocessError,
            probe.ProbeError, manager.ManagerError) as error:
        print(f"check_character_workshop_history_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_workshop_history_ui: PASS -- exact-source Identity, "
          "Profile, Rig, Fit, Performance, Test history, project-owned "
          "accessible donor metric badges, reversible animation intent, "
          "spatial fit/contact controls, "
          "keyboard/controller dense-editor escape routes, "
          "wide and 200% compact tab/readiness/status/shortcut policy, "
          "unique-name and collision-safe hold-to-delete confirmation, "
          "accessible performance targets with runtime-equivalent "
          "LOD assembly math, and all semantic pose inspection controls render "
          "without mutating installed bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
