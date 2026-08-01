#!/usr/bin/env bash
# Single source of truth for the standalone SDL2 used by macOS releases.
# Consumers must continue to verify both the source archive and installed
# license bytes; these constants identify the authenticated inputs.

# Values are intentionally consumed only by scripts that source this file.
# shellcheck disable=SC2034

readonly MDKR_RELEASE_SDL2_VERSION="2.32.10"
readonly MDKR_RELEASE_SDL2_SOURCE_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
readonly MDKR_RELEASE_SDL2_LICENSE_SHA256="97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127"
readonly MDKR_RELEASE_SDL2_URL="https://www.libsdl.org/release/SDL2-${MDKR_RELEASE_SDL2_VERSION}.tar.gz"
