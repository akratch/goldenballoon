// tool_objects.cpp — see tool_objects.h.
//
// COST. Rows are clipped, so a 512-slot pool costs the rows on screen. The
// purity gate compares presented-frame counts between a tools-off race and a
// tool-open one, and formatting 512 rows per frame would be a window paying for
// itself out of the frame loop even though it changed no state.
#include "tool_objects.h"

#include "app_theme.h"
#include "ui_common.h"

#include "sim_hash_view.h"  // the hash's own walk, published read-only

#include "imgui.h"

#include <cstdio>

void ToolObjects_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(620.0f * AppTheme::uiScale(),
                                    460.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Objects", open)) {
        ImGui::End();
        return;
    }

    const int slots = mdkr_sim_object_count();
    const int authoritative = mdkr_sim_object_authoritative_count();

    ui::TextSubtleWrapped(
        "Every entry of the object list the authoritative state hash walks, in "
        "its order. This window and the [SIMHASH] stream read the same array "
        "through the same filter, so they cannot disagree about what is live.");
    ui::Gap(ui::kGapS);

    {
        char line[192];
        std::snprintf(line, sizeof line,
                      "%d slots walked; %d counted as the authoritative "
                      "population (the objs= field of a [SIMHASH] row).",
                      slots, authoritative);
        ImGui::TextUnformatted(line);
    }

    static bool liveOnly = true;
    (void)ImGui::Checkbox("Hide empty slots", &liveOnly);
    ImGui::SameLine();
    ui::TextSubtle(
        "An empty slot is still hashed — its index and a presence byte.");

    ui::Gap(ui::kGapXS);

    if (slots <= 0) {
        ui::TextSubtleWrapped(
            "The object pool has not been allocated yet. It is created during "
            "early boot, before any level loads.");
        ImGui::End();
        return;
    }

    // Build the visible index set once per frame. Cheap (an int per slot) and it
    // is what lets the clipper work with the filter on: a clipper cannot skip
    // rows it has not been told the count of.
    static ImVector<int> visible;
    visible.clear();
    visible.reserve(slots);
    for (int i = 0; i < slots; ++i) {
        MdkrSimObjectView view;
        if (!mdkr_sim_object_view(i, &view)) continue;
        if (liveOnly && view.live == 0u) continue;
        visible.push_back(i);
    }

    if (ImGui::BeginChild("##objects", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (ImGui::BeginTable("##list", 6,
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Behaviour");
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableSetupColumn("Particle");
            ImGui::TableSetupColumn("Hashed");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(visible.Size);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                     ++row) {
                    MdkrSimObjectView view;
                    if (!mdkr_sim_object_view(visible[row], &view)) continue;
                    char buffer[96];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    std::snprintf(buffer, sizeof buffer, "%d", view.index);
                    ImGui::TextUnformatted(buffer);

                    ImGui::TableNextColumn();
                    if (view.live == 0u) {
                        ui::TextSubtle("empty");
                    } else if (view.behaviour_id < 0) {
                        // A particle's behaviorId is a different union member;
                        // the hash's particle arm skips it and so does this.
                        ui::TextSubtle("—");
                    } else {
                        std::snprintf(buffer, sizeof buffer, "%d",
                                      view.behaviour_id);
                        ImGui::TextUnformatted(buffer);
                    }

                    ImGui::TableNextColumn();
                    if (view.live == 0u) {
                        ui::TextSubtle("—");
                    } else {
                        std::snprintf(buffer, sizeof buffer,
                                      "%.1f, %.1f, %.1f",
                                      static_cast<double>(view.position[0]),
                                      static_cast<double>(view.position[1]),
                                      static_cast<double>(view.position[2]));
                        ImGui::TextUnformatted(buffer);
                    }

                    ImGui::TableNextColumn();
                    if (view.live == 0u) {
                        ui::TextSubtle("—");
                    } else {
                        std::snprintf(buffer, sizeof buffer, "%04x",
                                      static_cast<unsigned>(view.flags) &
                                          0xFFFFu);
                        ImGui::TextUnformatted(buffer);
                    }

                    ImGui::TableNextColumn();
                    if (view.live == 0u) {
                        ui::TextSubtle("—");
                    } else if (view.is_particle != 0u) {
                        std::snprintf(buffer, sizeof buffer,
                                      "particle (%d emitter%s)",
                                      view.active_emitters,
                                      view.active_emitters == 1 ? "" : "s");
                        ImGui::TextUnformatted(buffer);
                    } else if (view.active_emitters > 0 ||
                               view.emitters_on != 0u) {
                        std::snprintf(buffer, sizeof buffer,
                                      "emits %d, %s", view.active_emitters,
                                      view.emitters_on ? "enabled" : "disabled");
                        ImGui::TextUnformatted(buffer);
                    } else {
                        ui::TextSubtle("none");
                    }

                    ImGui::TableNextColumn();
                    if (view.live == 0u) {
                        ui::TextSubtle("presence only");
                    } else if (view.hashed != 0u) {
                        ImGui::TextUnformatted("yes");
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                        ImGui::TextUnformatted("companion");
                        ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::EndTable();
        }
        ui::TouchScrollCurrentWindow();
    }
    ImGui::EndChild();

    ImGui::End();
}
