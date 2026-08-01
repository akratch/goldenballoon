// app_brand.h — the product name players see. One definition, one place.
//
// "mdkr64" is the INTERNAL name: the repository, the CMake target, technical
// filenames, the env-var prefix, and the log file. Branded GUI surfaces use the
// product name below; technical documentation may still name an internal file.
//
// "Golden Balloon" is the public brand. Per the project's published governance
// (tools/web/demo-repo/DISCLAIMER.md) it is deliberately an unofficial project
// codename and NOT the title of the original game, so no player-facing surface
// should present the original title as this product's name either. Where the
// UI has to talk about what the player must supply, it says "the original game"
// — matching the language the public disclaimer already uses.
#ifndef MDKR64_APP_BRAND_H
#define MDKR64_APP_BRAND_H

// The product name, for titles, headings and labels.
#define MDKR_BRAND_NAME "Golden Balloon"

#ifdef __cplusplus

#include "app_version.h"

#include <cstdio>

// "Golden Balloon v<release>" — the canonical version line, plus the provenance
// commit when the build carries one (tools/release/stamp_provenance.sh binds
// releases to a source commit; a dev build simply has no stamp to show).
inline const char *AppBrandVersionLine() {
    static char s_line[160];
    if (s_line[0] == '\0') {
        if (AppBuildStamp()[0] != '\0') {
            std::snprintf(s_line, sizeof(s_line), "%s v%s (%s)",
                          MDKR_BRAND_NAME, AppVersion(), AppBuildStamp());
        } else {
            std::snprintf(s_line, sizeof(s_line), "%s v%s",
                          MDKR_BRAND_NAME, AppVersion());
        }
    }
    return s_line;
}

#endif  // __cplusplus

#endif  // MDKR64_APP_BRAND_H
