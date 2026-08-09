// tool_collision.cpp — see tool_collision.h.
//
// COST. The candidate list is rebuilt by every collision query, many times per
// tick, so this window reads it at draw time and never caches it: what it shows
// is the last query the physics actually ran. The row list is clipped by
// ImGuiListClipper so a 500-entry list costs the rows on screen, not 500 — the
// purity gate compares presented-frame counts, and a window that did 500 rows
// of formatting per frame would be paying for itself out of the frame loop.
#include "tool_collision.h"

#include "app_theme.h"
#include "ui_common.h"

#include "imgui.h"

#include <cstdio>

/*
 * The physics query's own published array, plus the platform-side census that
 * already counts its peak for the [COLL] exit summary
 * (platform/stubs_dkr.c, read by tests/check_collision_headroom.py).
 *
 * Re-declared here rather than reached through game/src/collision.h for the
 * reason tool_diagnostics.cpp states: that header pulls the decomp's N64 SDK
 * headers, whose memmove/memcpy declarations collide with the host CRT. s32 is
 * int32_t and s8 is signed char, per game/include/PR/ultratypes.h.
 *
 * gCollisionCandidates is a TAGGED handle array, not pointers: bit 31 selects
 * the kind (0 == LevelModelSegment anchor, biased by one; 1 == a
 * CollisionFacetPlanes triangle) and the rest is an arena offset. Classifying
 * by sign is exactly what resolve_collisions() and the camera-clip walk do, so
 * this window partitions the list the same way the consumers do — without
 * decoding a pointer it has no business dereferencing.
 */
extern "C" {
extern int         *gCollisionCandidates;
extern signed char *gCollisionSurfaces;
extern int          gNumCollisionCandidates;

int  mdkr_coll_cap(int romCap);
int  mdkr_coll_max_candidates(void);
long mdkr_coll_truncations(void);
}

namespace {

// game/src/collision.h. Named here rather than included for the header reason
// above; the effective cap is asked for at runtime because MDKR_COLLCAP can
// lower it for the boundary tests, and a window that showed 500 while the guard
// was at 64 would be reporting headroom the run does not have.
constexpr int kRomCandidateCap = 500;

// game/include/enums.h SurfaceType. A name is what makes a candidate list
// legible; an unknown value prints its number rather than a wrong name.
const char *surfaceName(int surface) {
    switch (surface) {
        case 0:  return "default";
        case 1:  return "grass";
        case 2:  return "sand";
        case 3:  return "zip pad";
        case 4:  return "stone";
        case 5:  return "egg spawn";
        case 6:  return "egg 01";
        case 7:  return "egg 02";
        case 8:  return "egg 03";
        case 9:  return "egg 04";
        case 10: return "frozen water";
        case 11: return "calm water";
        case 12: return "Taj pad";
        case 13: return "snow";
        case 14: return "wavy water";
        case 15: return "water (unk f)";
        case 16: return "unk 10";
        case 17: return "invisible wall";
        case 18: return "unk 12";
        case 255: return "none";
        default: return nullptr;
    }
}

ImVec4 headroomColour(int used, int cap) {
    if (cap <= 0) return AppTheme::subtle();
    const float fraction = static_cast<float>(used) / static_cast<float>(cap);
    if (fraction >= 1.0f) return AppTheme::bad();
    if (fraction >= 0.8f) return AppTheme::accent();
    return AppTheme::good();
}

}  // namespace

void ToolCollision_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(520.0f * AppTheme::uiScale(),
                                    460.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Collision", open)) {
        ImGui::End();
        return;
    }

    ui::TextSubtleWrapped(
        "The candidate list the physics query built for its last call — the "
        "same entries resolve_collisions answered out of. Nothing here is a "
        "second traversal of the level.");
    ui::Gap(ui::kGapS);

    const int cap = mdkr_coll_cap(kRomCandidateCap);
    const int peak = mdkr_coll_max_candidates();
    const long truncations = mdkr_coll_truncations();
    const int *candidates = gCollisionCandidates;
    const signed char *surfaces = gCollisionSurfaces;
    int count = gNumCollisionCandidates;
    if (candidates == nullptr || count < 0) count = 0;
    if (cap > 0 && count > cap) count = cap;  // fail closed; never read past it

    // --- the number to watch --------------------------------------------------
    {
        char label[96];
        std::snprintf(label, sizeof label, "%d of %d candidates", count, cap);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, headroomColour(count, cap));
        ImGui::ProgressBar(cap > 0 ? static_cast<float>(count) /
                                         static_cast<float>(cap)
                                   : 0.0f,
                           ImVec2(-1.0f, 0.0f), label);
        ImGui::PopStyleColor();
    }
    {
        char line[192];
        std::snprintf(line, sizeof line,
                      "Run peak %d of %d, %ld truncation%s so far.", peak, cap,
                      truncations, truncations == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, headroomColour(peak, cap));
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
    }
    ui::TextSubtleWrapped(
        "Boss levels 41 and 54 peak at 416 of 500, so the shipped margin is "
        "84 entries. At the cap the query stops collecting and the physics "
        "answers out of an incomplete list — which is what a fall-through "
        "looks like from the outside, and why a truncation count above zero "
        "matters more than any single frame here.");
    if (truncations > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextUnformatted("This run has already truncated a candidate list.");
        ImGui::PopStyleColor();
    }

    ui::Gap(ui::kGapS);

    // --- the composition of the list ------------------------------------------
    int segments = 0;
    int facets = 0;
    for (int i = 0; i < count; ++i) {
        // Sign IS the tag; see the extern block's note.
        if (candidates[i] < 0) {
            ++facets;
        } else {
            ++segments;
        }
    }
    {
        char line[160];
        std::snprintf(line, sizeof line,
                      "%d terrain segment%s anchoring %d triangle%s.",
                      segments, segments == 1 ? "" : "s",
                      facets, facets == 1 ? "" : "s");
        ImGui::TextUnformatted(line);
    }

    ui::Gap(ui::kGapXS);
    if (count == 0) {
        ui::TextSubtleWrapped(
            "No candidates. The list is empty before a track loads, and "
            "between queries on a track with nothing near the swept volume.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginChild("##candidates", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (ImGui::BeginTable("##list", 4,
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Surface");
            ImGui::TableSetupColumn("Handle");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(count);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const int handle = candidates[i];
                    const bool facet = handle < 0;
                    const int surface =
                        surfaces != nullptr
                            ? static_cast<int>(
                                  static_cast<unsigned char>(surfaces[i]))
                            : -1;
                    char buffer[64];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    std::snprintf(buffer, sizeof buffer, "%d", i);
                    ImGui::TextUnformatted(buffer);

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(facet ? "triangle" : "segment");

                    ImGui::TableNextColumn();
                    if (!facet) {
                        // The surface byte belongs to a facet entry; a segment
                        // anchor carries none, and printing its neighbour's
                        // would be the window inventing data.
                        ui::TextSubtle("—");
                    } else if (const char *name = surfaceName(surface)) {
                        ImGui::TextUnformatted(name);
                    } else {
                        std::snprintf(buffer, sizeof buffer, "%d", surface);
                        ImGui::TextUnformatted(buffer);
                    }

                    ImGui::TableNextColumn();
                    std::snprintf(buffer, sizeof buffer, "%08x",
                                  static_cast<unsigned>(handle));
                    ImGui::TextUnformatted(buffer);
                }
            }
            ImGui::EndTable();
        }
        ui::TouchScrollCurrentWindow();
    }
    ImGui::EndChild();

    ImGui::End();
}
