/**
 * display_config.c — aspect/FOV policy for the native and WebGPU ports.
 */
#include "display_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDKR_DEG_TO_RAD (0.01745329251994329577f)
#define MDKR_RAD_TO_DEG (57.295779513082320876f)

typedef struct MdkrDisplayRuntimeConfig {
    int initialized;
    int widescreen_enabled;
    float forced_aspect;
    float gameplay_vertical_fov;
    float maximum_horizontal_fov;
    unsigned int width;
    unsigned int height;
    uint64_t generation;
} MdkrDisplayRuntimeConfig;

static MdkrDisplayRuntimeConfig s_display = {
    0,
    1,
    0.0f,
    0.0f,
    MDKR_SIMULATION_SAFE_MAX_HFOV,
    320,
    240,
    1,
};

/* FNV-1a gives the record an architecture-independent identity without making
 * pointer addresses or padding part of the projection handshake. */
#define MDKR_FNV64_OFFSET 14695981039346656037ULL
#define MDKR_FNV64_PRIME  1099511628211ULL

static uint64_t mdkr_projection_hash_u32(uint64_t hash, uint32_t value) {
    int byte;

    for (byte = 0; byte < 4; byte++) {
        hash ^= (uint8_t) (value & 0xffU);
        hash *= MDKR_FNV64_PRIME;
        value >>= 8;
    }
    return hash;
}

static uint64_t mdkr_projection_hash_u64(uint64_t hash, uint64_t value) {
    int byte;

    for (byte = 0; byte < 8; byte++) {
        hash ^= (uint8_t) (value & 0xffU);
        hash *= MDKR_FNV64_PRIME;
        value >>= 8;
    }
    return hash;
}

static uint64_t mdkr_projection_hash_float(uint64_t hash, float value) {
    uint32_t bits;

    /* The public calculation normalizes invalid inputs before this helper is
     * reached, so copying the representation is a stable way to distinguish
     * projection-relevant values without aliasing violations. */
    memcpy(&bits, &value, sizeof(bits));
    return mdkr_projection_hash_u32(hash, bits);
}

static void mdkr_display_bump_generation(void) {
    /* Zero denotes an uninitialized/invalid projection record. */
    s_display.generation++;
    if (s_display.generation == 0) {
        s_display.generation = 1;
    }
}

static float mdkr_clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* C/POSIX-neutral ASCII comparison: unlike strcasecmp(), this is available on
 * MSVC and needs no locale-dependent behavior for our command-line tokens. */
static int mdkr_ascii_equal_ci(const char *left, const char *right) {
    unsigned char a;
    unsigned char b;

    if (left == NULL || right == NULL) {
        return left == right;
    }
    do {
        a = (unsigned char) *left++;
        b = (unsigned char) *right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char) (a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char) (b + ('a' - 'A'));
        if (a != b) {
            return 0;
        }
    } while (a != '\0');
    return 1;
}

static MdkrDisplayRect mdkr_centered_fit(float outer_width, float outer_height, float aspect) {
    MdkrDisplayRect rect = { 0.0f, 0.0f, outer_width, outer_height };

    if (!(aspect > 0.0f) || !isfinite(aspect) || outer_width <= 0.0f || outer_height <= 0.0f) {
        return rect;
    }

    if ((outer_width / outer_height) > aspect) {
        rect.width = outer_height * aspect;
        rect.x = (outer_width - rect.width) * 0.5f;
    } else {
        rect.height = outer_width / aspect;
        rect.y = (outer_height - rect.height) * 0.5f;
    }
    return rect;
}

static MdkrDisplayRect mdkr_centered_cover(float outer_width, float outer_height, float aspect) {
    MdkrDisplayRect rect = { 0.0f, 0.0f, outer_width, outer_height };

    if (!(aspect > 0.0f) || !isfinite(aspect) || outer_width <= 0.0f || outer_height <= 0.0f) {
        return rect;
    }

    if ((outer_width / outer_height) > aspect) {
        rect.width = outer_width;
        rect.height = outer_width / aspect;
        rect.y = (outer_height - rect.height) * 0.5f;
    } else {
        rect.height = outer_height;
        rect.width = outer_height * aspect;
        rect.x = (outer_width - rect.width) * 0.5f;
    }
    return rect;
}

MdkrDisplayLayout mdkr_display_calculate_layout(
    unsigned int drawable_width,
    unsigned int drawable_height,
    int widescreen_enabled,
    float forced_aspect) {
    MdkrDisplayLayout layout;
    float width = drawable_width > 0 ? (float) drawable_width : MDKR_LOGICAL_WIDTH;
    float height = drawable_height > 0 ? (float) drawable_height : MDKR_LOGICAL_HEIGHT;
    float drawable_aspect = width / height;

    memset(&layout, 0, sizeof(layout));
    layout.drawable = (MdkrDisplayRect) { 0.0f, 0.0f, width, height };
    layout.fullbleed = mdkr_centered_cover(width, height, MDKR_LOGICAL_ASPECT);

    if (!widescreen_enabled) {
        /*
         * Exact compatibility mode: both axes cover the drawable independently,
         * reproducing the pre-widescreen renderer (including its stretch).
         */
        layout.presentation = layout.drawable;
        layout.safe = layout.drawable;
        layout.fullbleed = layout.drawable;
        layout.presentation_aspect = MDKR_LOGICAL_ASPECT;
        layout.legacy_stretch = 1;
        return layout;
    }

    if (!(forced_aspect > 0.0f) || !isfinite(forced_aspect)) {
        forced_aspect = drawable_aspect;
    }
    forced_aspect = mdkr_clampf(forced_aspect, 0.5f, 4.0f);

    layout.presentation = mdkr_centered_fit(width, height, forced_aspect);
    layout.safe = mdkr_centered_fit(layout.presentation.width, layout.presentation.height,
                                    MDKR_LOGICAL_ASPECT);
    layout.safe.x += layout.presentation.x;
    layout.safe.y += layout.presentation.y;
    layout.presentation_aspect = layout.presentation.width / layout.presentation.height;
    layout.legacy_stretch = 0;
    return layout;
}

MdkrProjection mdkr_display_calculate_projection(
    float authored_vertical_fov,
    float logical_viewport_width,
    float logical_viewport_height,
    float presentation_aspect,
    int widescreen_enabled,
    int gameplay_camera,
    float gameplay_vertical_fov,
    float maximum_horizontal_fov) {
    MdkrProjection projection;
    float vertical_fov = authored_vertical_fov;
    float logical_aspect;
    float half_vertical_tangent;

    memset(&projection, 0, sizeof(projection));

    if (!(vertical_fov > 1.0f) || !isfinite(vertical_fov)) {
        vertical_fov = 60.0f;
    }
    vertical_fov = mdkr_clampf(vertical_fov, 1.0f, 170.0f);

    if (gameplay_camera && gameplay_vertical_fov > 1.0f && isfinite(gameplay_vertical_fov)) {
        float authored_half_tangent =
            tanf(vertical_fov * 0.5f * MDKR_DEG_TO_RAD);
        float requested_half_tangent =
            tanf(mdkr_clampf(gameplay_vertical_fov, 20.0f, 140.0f) *
                 0.5f * MDKR_DEG_TO_RAD);
        float reference_half_tangent = tanf(30.0f * MDKR_DEG_TO_RAD);

        /*
         * The setting is a 60-degree-reference lens, not an absolute override.
         * Scaling in tangent space keeps DKR's authored per-level zooms and
         * gameplay camera effects proportional: authored 60 maps exactly to the
         * requested value, while an authored 40-degree shot remains tighter.
         */
        vertical_fov =
            2.0f * atanf(authored_half_tangent *
                         (requested_half_tangent / reference_half_tangent)) *
            MDKR_RAD_TO_DEG;
        vertical_fov = mdkr_clampf(vertical_fov, 1.0f, 170.0f);
    }

    if (!widescreen_enabled) {
        projection.aspect = MDKR_LOGICAL_ASPECT;
    } else {
        if (!(logical_viewport_width > 0.0f) || !(logical_viewport_height > 0.0f) ||
            !isfinite(logical_viewport_width) || !isfinite(logical_viewport_height)) {
            logical_viewport_width = MDKR_LOGICAL_WIDTH;
            logical_viewport_height = MDKR_LOGICAL_HEIGHT;
        }
        if (!(presentation_aspect > 0.0f) || !isfinite(presentation_aspect)) {
            presentation_aspect = MDKR_LOGICAL_ASPECT;
        }

        /*
         * Convert the RSP viewport's 320x240 logical fraction into its physical
         * aspect inside the host presentation rectangle. DKR's stock two-player
         * mode deliberately retains a full-height RSP viewport and clips it with
         * a half-height scissor, so it passes 320x240 here; true custom half-
         * height viewports still calculate at twice the host aspect.
         */
        logical_aspect = logical_viewport_width / logical_viewport_height;
        projection.aspect = logical_aspect * (presentation_aspect / MDKR_LOGICAL_ASPECT);
        projection.aspect = mdkr_clampf(projection.aspect, 0.25f, 8.0f);
    }

    half_vertical_tangent = tanf(vertical_fov * 0.5f * MDKR_DEG_TO_RAD);
    projection.horizontal_fov =
        2.0f * atanf(half_vertical_tangent * projection.aspect) * MDKR_RAD_TO_DEG;

    /*
     * A correct top/bottom split on an ultrawide display can otherwise approach
     * a 150-degree horizontal lens.  Preserve shape and the requested horizontal
     * cap by reducing vertical FOV; never fake it by stretching the projection.
     */
    if (widescreen_enabled && maximum_horizontal_fov > 0.0f &&
        isfinite(maximum_horizontal_fov)) {
        maximum_horizontal_fov = mdkr_clampf(maximum_horizontal_fov, 60.0f, 175.0f);
        if (projection.horizontal_fov > maximum_horizontal_fov) {
            float half_horizontal_tangent =
                tanf(maximum_horizontal_fov * 0.5f * MDKR_DEG_TO_RAD);
            vertical_fov =
                2.0f * atanf(half_horizontal_tangent / projection.aspect) * MDKR_RAD_TO_DEG;
            projection.horizontal_fov = maximum_horizontal_fov;
            projection.horizontal_fov_capped = 1;
        }
    }

    projection.vertical_fov = vertical_fov;
    return projection;
}

static int mdkr_display_logical_viewport_dimensions(
    int viewport_layout,
    int viewport,
    float *width,
    float *height) {
    if (width == NULL || height == NULL || viewport < 0 || viewport >= 4) {
        return 0;
    }

    switch (viewport_layout) {
        case MDKR_DISPLAY_VIEWPORT_1_PLAYER:
            if (viewport != 0) {
                return 0;
            }
            *width = MDKR_LOGICAL_WIDTH;
            *height = MDKR_LOGICAL_HEIGHT;
            return 1;
        case MDKR_DISPLAY_VIEWPORT_2_PLAYERS:
            if (viewport >= 2) {
                return 0;
            }
            /* DKR clips this full-height RSP viewport with a scissor. */
            *width = MDKR_LOGICAL_WIDTH;
            *height = MDKR_LOGICAL_HEIGHT;
            return 1;
        case MDKR_DISPLAY_VIEWPORT_3_PLAYERS:
            /* viewport_main intentionally renders 3P through the 4P branch.
             * The fourth quadrant is normally the track map, but native
             * spectator/T.T. presentation can select camera slot 3 there. */
            *width = MDKR_LOGICAL_WIDTH * 0.5f;
            *height = MDKR_LOGICAL_HEIGHT * 0.5f;
            return 1;
        case MDKR_DISPLAY_VIEWPORT_4_PLAYERS:
            *width = MDKR_LOGICAL_WIDTH * 0.5f;
            *height = MDKR_LOGICAL_HEIGHT * 0.5f;
            return 1;
        default:
            return 0;
    }
}

bool mdkr_display_calculate_camera_projection(
    const MdkrCameraProjectionRequest *request,
    MdkrCameraProjection *out) {
    MdkrProjection projection;
    float logical_width;
    float logical_height;
    uint64_t hash;

    if (request == NULL || out == NULL || request->display_generation == 0 ||
        request->camera_id < 0 || request->camera_id >= 8 ||
        !(request->near_plane > 0.0f) || !isfinite(request->near_plane) ||
        !(request->far_plane > request->near_plane) || !isfinite(request->far_plane) ||
        !mdkr_display_logical_viewport_dimensions(
            request->viewport_layout, request->viewport, &logical_width, &logical_height)) {
        return false;
    }

    projection = mdkr_display_calculate_projection(
        request->authored_vertical_fov,
        logical_width,
        logical_height,
        request->presentation_aspect,
        request->widescreen_enabled,
        request->gameplay_camera,
        request->gameplay_vertical_fov,
        request->maximum_horizontal_fov);

    memset(out, 0, sizeof(*out));
    out->logical_viewport_width = logical_width;
    out->logical_viewport_height = logical_height;
    out->aspect = projection.aspect;
    out->vertical_fov = projection.vertical_fov;
    out->horizontal_fov = projection.horizontal_fov;
    out->near_plane = request->near_plane;
    out->far_plane = request->far_plane;
    out->display_generation = request->display_generation;
    out->viewport = request->viewport;
    out->camera_id = request->camera_id;
    out->camera_bank = request->camera_id / 4;
    out->horizontal_fov_capped = projection.horizontal_fov_capped;

    /* Include every value that can change the lens, its viewport meaning, or
     * the selected camera bank.  This is deliberately a value key rather than
     * an address/timestamp so it is useful to snapshots and diagnostics too. */
    hash = MDKR_FNV64_OFFSET;
    hash = mdkr_projection_hash_u64(hash, out->display_generation);
    hash = mdkr_projection_hash_float(hash, out->logical_viewport_width);
    hash = mdkr_projection_hash_float(hash, out->logical_viewport_height);
    hash = mdkr_projection_hash_float(hash, out->aspect);
    hash = mdkr_projection_hash_float(hash, out->vertical_fov);
    hash = mdkr_projection_hash_float(hash, out->horizontal_fov);
    hash = mdkr_projection_hash_float(hash, out->near_plane);
    hash = mdkr_projection_hash_float(hash, out->far_plane);
    hash = mdkr_projection_hash_u32(hash, (uint32_t) out->viewport);
    hash = mdkr_projection_hash_u32(hash, (uint32_t) out->camera_id);
    hash = mdkr_projection_hash_u32(hash, (uint32_t) out->camera_bank);
    hash = mdkr_projection_hash_u32(hash, (uint32_t) out->horizontal_fov_capped);
    out->generation = hash != 0 ? hash : 1;
    return true;
}

MdkrBillboardCorrection mdkr_display_calculate_billboard_correction(
    float authored_vertical_fov,
    float effective_vertical_fov,
    float authored_viewport_aspect,
    float effective_viewport_aspect,
    int widescreen_enabled) {
    MdkrBillboardCorrection correction = { 1.0f, 1.0f };
    float authored_half_tangent;
    float effective_half_tangent;

    if (!(authored_vertical_fov > 1.0f) || !isfinite(authored_vertical_fov) ||
        !(effective_vertical_fov > 1.0f) || !isfinite(effective_vertical_fov)) {
        return correction;
    }

    authored_vertical_fov = mdkr_clampf(authored_vertical_fov, 1.0f, 170.0f);
    effective_vertical_fov = mdkr_clampf(effective_vertical_fov, 1.0f, 170.0f);
    authored_half_tangent =
        tanf(authored_vertical_fov * 0.5f * MDKR_DEG_TO_RAD);
    effective_half_tangent =
        tanf(effective_vertical_fov * 0.5f * MDKR_DEG_TO_RAD);
    if (!(authored_half_tangent > 0.0f) || !isfinite(authored_half_tangent) ||
        !(effective_half_tangent > 0.0f) || !isfinite(effective_half_tangent)) {
        return correction;
    }

    /*
     * F3DDKR adds billboard-local coordinates to the projected anchor in clip
     * space. They therefore bypass both the perspective focal scale and the
     * viewport's pixel aspect. Scale the X/Y clip columns independently:
     *
     *   - clip_y tracks a user/cap-induced lens change relative to the authored
     *     lens, while remaining 1 for ordinary Hor+;
     *   - clip_x additionally cancels the host viewport aspect relative to the
     *     original TV viewport.
     *
     * Applying the correction to output columns, rather than changing the
     * billboard builder's non-uniform Y parameter, preserves every rotated
     * sprite's original pixel-space transform. Compatibility mode deliberately
     * omits the aspect term so it retains the historical stretch.
     */
    correction.clip_y = authored_half_tangent / effective_half_tangent;
    correction.clip_x = correction.clip_y;
    if (widescreen_enabled) {
        if (!(authored_viewport_aspect > 0.0f) || !isfinite(authored_viewport_aspect)) {
            authored_viewport_aspect = MDKR_LOGICAL_ASPECT;
        }
        if (!(effective_viewport_aspect > 0.0f) || !isfinite(effective_viewport_aspect)) {
            effective_viewport_aspect = authored_viewport_aspect;
        }
        correction.clip_x *= authored_viewport_aspect / effective_viewport_aspect;
    }
    return correction;
}

void mdkr_display_apply_billboard_correction(
    float matrix[4][4],
    MdkrBillboardCorrection correction) {
    int row;

    if (matrix == NULL) {
        return;
    }
    if (!isfinite(correction.clip_x)) {
        correction.clip_x = 1.0f;
    }
    if (!isfinite(correction.clip_y)) {
        correction.clip_y = 1.0f;
    }
    for (row = 0; row < 4; row++) {
        matrix[row][0] *= correction.clip_x;
        matrix[row][1] *= correction.clip_y;
    }
}

static int mdkr_parse_bool(const char *value, int *out) {
    if (value == NULL || value[0] == '\0' || out == NULL) {
        return 0;
    }
    if (mdkr_ascii_equal_ci(value, "1") || mdkr_ascii_equal_ci(value, "on") ||
        mdkr_ascii_equal_ci(value, "true") || mdkr_ascii_equal_ci(value, "yes")) {
        *out = 1;
        return 1;
    }
    if (mdkr_ascii_equal_ci(value, "0") || mdkr_ascii_equal_ci(value, "off") ||
        mdkr_ascii_equal_ci(value, "false") || mdkr_ascii_equal_ci(value, "no")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int mdkr_parse_float(const char *value, float *out) {
    char *end = NULL;
    float parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return 0;
    }
    parsed = strtof(value, &end);
    if (end == value) {
        return 0;
    }
    while (*end != '\0' && isspace((unsigned char) *end)) {
        end++;
    }
    if (*end != '\0' || !isfinite(parsed)) {
        return 0;
    }
    *out = parsed;
    return 1;
}

static int mdkr_parse_aspect(const char *value, float *out) {
    const char *separator;
    float numerator;
    float denominator;
    char left[32];
    size_t left_length;

    if (value == NULL || out == NULL) {
        return 0;
    }
    if (mdkr_ascii_equal_ci(value, "auto") ||
        mdkr_ascii_equal_ci(value, "window") ||
        mdkr_ascii_equal_ci(value, "native")) {
        *out = 0.0f;
        return 1;
    }

    separator = strchr(value, ':');
    if (separator == NULL) {
        separator = strchr(value, '/');
    }
    if (separator != NULL) {
        left_length = (size_t) (separator - value);
        if (left_length == 0 || left_length >= sizeof(left)) {
            return 0;
        }
        memcpy(left, value, left_length);
        left[left_length] = '\0';
        if (!mdkr_parse_float(left, &numerator) ||
            !mdkr_parse_float(separator + 1, &denominator) ||
            denominator == 0.0f) {
            return 0;
        }
        *out = numerator / denominator;
    } else if (!mdkr_parse_float(value, out)) {
        return 0;
    }

    return *out >= 0.5f && *out <= 4.0f && isfinite(*out);
}

void mdkr_display_config_init(void) {
    const char *value;
    int parsed_bool;
    float parsed_float;

    if (s_display.initialized) {
        return;
    }
    s_display.initialized = 1;

    value = getenv("MDKR_WIDESCREEN");
    if (value != NULL && mdkr_parse_bool(value, &parsed_bool)) {
        s_display.widescreen_enabled = parsed_bool;
    }

    value = getenv("MDKR_ASPECT");
    if (value != NULL && mdkr_parse_aspect(value, &parsed_float)) {
        s_display.forced_aspect = parsed_float;
    }

    value = getenv("MDKR_FOV");
    if (value != NULL && mdkr_parse_float(value, &parsed_float) &&
        parsed_float >= 20.0f && parsed_float <= 140.0f) {
        s_display.gameplay_vertical_fov = parsed_float;
    }

    value = getenv("MDKR_MAX_HFOV");
    if (value != NULL) {
        if (mdkr_ascii_equal_ci(value, "off") || !strcmp(value, "0")) {
            s_display.maximum_horizontal_fov = 0.0f;
        } else if (mdkr_parse_float(value, &parsed_float) &&
                   parsed_float >= 60.0f && parsed_float <= 175.0f) {
            s_display.maximum_horizontal_fov = parsed_float;
        }
    }

}

void mdkr_display_report_config(void) {
    mdkr_display_config_init();
    printf("[DISPLAY] widescreen=%s aspect=%s",
           s_display.widescreen_enabled ? "on" : "legacy-stretch",
           s_display.forced_aspect > 0.0f ? "forced" : "auto");
    if (s_display.forced_aspect > 0.0f) {
        printf("(%.5f)", s_display.forced_aspect);
    }
    printf(" gameplay-vfov=%s",
           s_display.gameplay_vertical_fov > 0.0f ? "scaled" : "authored");
    if (s_display.gameplay_vertical_fov > 0.0f) {
        printf("(%.1f@authored60)", s_display.gameplay_vertical_fov);
    }
    printf(" max-hfov=%s", s_display.maximum_horizontal_fov > 0.0f ? "" : "off");
    if (s_display.maximum_horizontal_fov > 0.0f) {
        printf("%.1f", s_display.maximum_horizontal_fov);
    }
    putchar('\n');
    if (s_display.widescreen_enabled &&
        (s_display.maximum_horizontal_fov == 0.0f ||
         s_display.maximum_horizontal_fov > MDKR_SIMULATION_SAFE_MAX_HFOV)) {
        fprintf(stderr,
                "[DISPLAY] warning: max-hfov above %.1f can reveal an "
                "object-free edge band; level geometry remains wide and "
                "gameplay visibility stays faithful\n",
                MDKR_SIMULATION_SAFE_MAX_HFOV);
    }
}

int mdkr_display_set_widescreen(const char *value) {
    int enabled;
    mdkr_display_config_init();
    if (!mdkr_parse_bool(value, &enabled)) {
        return 0;
    }
    if (s_display.widescreen_enabled != enabled) {
        s_display.widescreen_enabled = enabled;
        mdkr_display_bump_generation();
    }
    return 1;
}

int mdkr_display_set_aspect(const char *value) {
    float aspect;
    mdkr_display_config_init();
    if (!mdkr_parse_aspect(value, &aspect)) {
        return 0;
    }
    if (s_display.forced_aspect != aspect) {
        s_display.forced_aspect = aspect;
        mdkr_display_bump_generation();
    }
    return 1;
}

int mdkr_display_set_gameplay_fov(const char *value) {
    float fov;
    mdkr_display_config_init();
    if (value != NULL &&
        (mdkr_ascii_equal_ci(value, "authored") ||
         mdkr_ascii_equal_ci(value, "default") ||
         mdkr_ascii_equal_ci(value, "off"))) {
        if (s_display.gameplay_vertical_fov != 0.0f) {
            s_display.gameplay_vertical_fov = 0.0f;
            mdkr_display_bump_generation();
        }
        return 1;
    }
    if (!mdkr_parse_float(value, &fov) || fov < 20.0f || fov > 140.0f) {
        return 0;
    }
    if (s_display.gameplay_vertical_fov != fov) {
        s_display.gameplay_vertical_fov = fov;
        mdkr_display_bump_generation();
    }
    return 1;
}

int mdkr_display_set_max_horizontal_fov(const char *value) {
    float fov;
    mdkr_display_config_init();
    if (value != NULL &&
        (mdkr_ascii_equal_ci(value, "off") || !strcmp(value, "0"))) {
        if (s_display.maximum_horizontal_fov != 0.0f) {
            s_display.maximum_horizontal_fov = 0.0f;
            mdkr_display_bump_generation();
        }
        return 1;
    }
    if (!mdkr_parse_float(value, &fov) || fov < 60.0f || fov > 175.0f) {
        return 0;
    }
    if (s_display.maximum_horizontal_fov != fov) {
        s_display.maximum_horizontal_fov = fov;
        mdkr_display_bump_generation();
    }
    return 1;
}

void mdkr_display_set_dimensions(unsigned int width, unsigned int height) {
    MdkrDisplayLayout layout;

    mdkr_display_config_init();
    if (width == 0) {
        width = 320;
    }
    if (height == 0) {
        height = 240;
    }
    if (width == s_display.width && height == s_display.height) {
        return;
    }

    s_display.width = width;
    s_display.height = height;
    mdkr_display_bump_generation();
    layout = mdkr_display_layout();
    printf("[DISPLAY] drawable=%ux%u presentation=%.0fx%.0f%+.0f%+.0f "
           "safe=%.0fx%.0f%+.0f%+.0f aspect=%.5f\n",
           width, height,
           layout.presentation.width, layout.presentation.height,
           layout.presentation.x, layout.presentation.y,
           layout.safe.width, layout.safe.height,
           layout.safe.x, layout.safe.y,
           layout.presentation_aspect);
}

uint64_t mdkr_display_config_generation(void) {
    mdkr_display_config_init();
    return s_display.generation;
}

unsigned int mdkr_display_width(void) {
    mdkr_display_config_init();
    return s_display.width;
}

unsigned int mdkr_display_height(void) {
    mdkr_display_config_init();
    return s_display.height;
}

int mdkr_display_widescreen_enabled(void) {
    mdkr_display_config_init();
    return s_display.widescreen_enabled;
}

float mdkr_display_forced_aspect(void) {
    mdkr_display_config_init();
    return s_display.forced_aspect;
}

float mdkr_display_gameplay_fov(void) {
    mdkr_display_config_init();
    return s_display.gameplay_vertical_fov;
}

float mdkr_display_max_horizontal_fov(void) {
    mdkr_display_config_init();
    return s_display.maximum_horizontal_fov;
}

MdkrDisplayLayout mdkr_display_layout(void) {
    mdkr_display_config_init();
    return mdkr_display_calculate_layout(
        s_display.width,
        s_display.height,
        s_display.widescreen_enabled,
        s_display.forced_aspect);
}
