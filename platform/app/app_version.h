// app_version.h — the single compiled-in product version for the app shell.
//
// The value comes from CMake (-DMDKR_APP_VERSION_STR="${MDKR_VERSION}"), the
// same cache variable that stamps `mdkr64 --version` and the app bundle's
// CFBundleShortVersionString, so the launcher's footer can never disagree with
// the packaged build. A non-CMake build falls back to "0.0.0-dev".
#ifndef MDKR64_APP_VERSION_H
#define MDKR64_APP_VERSION_H

#ifndef MDKR_APP_VERSION_STR
#define MDKR_APP_VERSION_STR "0.0.0-dev"
#endif

// Provenance stamp (tools/release). Empty in a plain dev build.
#ifndef MDKR_APP_BUILD_STAMP
#define MDKR_APP_BUILD_STAMP ""
#endif

inline const char *AppVersion()   { return MDKR_APP_VERSION_STR; }
inline const char *AppBuildStamp() { return MDKR_APP_BUILD_STAMP; }

#endif  // MDKR64_APP_VERSION_H
