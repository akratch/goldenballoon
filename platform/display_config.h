/**
 * display_config.h — host-display policy shared by the game camera and renderer.
 *
 * The original game authors against a 320x240 (4:3) logical framebuffer.  The
 * native port keeps that coordinate system, but maps it into three distinct host
 * regions:
 *
 *   presentation  3D world output (window aspect, or a forced aspect)
 *   safe          undistorted 4:3 HUD/menu content contained in presentation
 *   fullbleed     undistorted 4:3 transition content covering the drawable
 *
 * Keeping the arithmetic in this small, dependency-light module gives camera.c,
 * the F3DDKR interpreter, SDL, native WebGPU and browser WebGPU one source of
 * truth.  The explicit calculation functions are also ROM-free unit-test seams.
 */
#ifndef MDKR64_DISPLAY_CONFIG_H
#define MDKR64_DISPLAY_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_LOGICAL_WIDTH  320.0f
#define MDKR_LOGICAL_HEIGHT 240.0f
#define MDKR_LOGICAL_ASPECT (MDKR_LOGICAL_WIDTH / MDKR_LOGICAL_HEIGHT)
/*
 * The stock CPU object frustum is 2*atan(1.3) = 104.862... degrees wide.
 * Staying just inside it guarantees Hor+ never admits render-only objects into
 * DKR's simulation-coupled object renderer.
 */
#define MDKR_SIMULATION_SAFE_MAX_HFOV 104.0f

typedef struct MdkrDisplayRect {
    float x;
    float y;
    float width;
    float height;
} MdkrDisplayRect;

typedef struct MdkrDisplayLayout {
    MdkrDisplayRect drawable;
    MdkrDisplayRect presentation;
    MdkrDisplayRect safe;
    MdkrDisplayRect fullbleed;
    float presentation_aspect;
    int legacy_stretch;
} MdkrDisplayLayout;

typedef struct MdkrProjection {
    float aspect;
    float vertical_fov;
    float horizontal_fov;
    int horizontal_fov_capped;
} MdkrProjection;

typedef struct MdkrBillboardCorrection {
    float clip_x;
    float clip_y;
} MdkrBillboardCorrection;

/**
 * Pure layout/projection helpers.  These do not read globals or the environment.
 */
MdkrDisplayLayout mdkr_display_calculate_layout(
    unsigned int drawable_width,
    unsigned int drawable_height,
    int widescreen_enabled,
    float forced_aspect);

MdkrProjection mdkr_display_calculate_projection(
    float authored_vertical_fov,
    float logical_viewport_width,
    float logical_viewport_height,
    float presentation_aspect,
    int widescreen_enabled,
    int gameplay_camera,
    float gameplay_vertical_fov,
    float maximum_horizontal_fov);

MdkrBillboardCorrection mdkr_display_calculate_billboard_correction(
    float authored_vertical_fov,
    float effective_vertical_fov,
    float authored_viewport_aspect,
    float effective_viewport_aspect,
    int widescreen_enabled);

void mdkr_display_apply_billboard_correction(
    float matrix[4][4],
    MdkrBillboardCorrection correction);

/**
 * Runtime configuration.  Environment defaults are read once:
 *
 *   MDKR_WIDESCREEN=0|1       1 by default; 0 reproduces legacy stretching
 *   MDKR_ASPECT=auto|4:3|...  presentation aspect, auto by default
 *   MDKR_FOV=<degrees>         60-degree-reference gameplay FOV; authored default
 *   MDKR_MAX_HFOV=<degrees>    ultrawide safety cap; 104 by default
 */
void mdkr_display_config_init(void);
void mdkr_display_report_config(void);
int mdkr_display_set_widescreen(const char *value);
int mdkr_display_set_aspect(const char *value);
int mdkr_display_set_gameplay_fov(const char *value);
int mdkr_display_set_max_horizontal_fov(const char *value);

void mdkr_display_set_dimensions(unsigned int width, unsigned int height);
unsigned int mdkr_display_width(void);
unsigned int mdkr_display_height(void);
int mdkr_display_widescreen_enabled(void);
float mdkr_display_forced_aspect(void);
float mdkr_display_gameplay_fov(void);
float mdkr_display_max_horizontal_fov(void);
MdkrDisplayLayout mdkr_display_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_DISPLAY_CONFIG_H */
