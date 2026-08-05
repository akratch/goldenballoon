#include "gfx_shadow_cascade.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define DKR_CAMERA_NEAR 10.0f
#define DKR_CAMERA_FAR 15000.0f
#define SHADOW_ONE_PLAYER_NEAR_END 2200.0f
#define SHADOW_ONE_PLAYER_FAR_BEGIN 1800.0f
#define SHADOW_ONE_PLAYER_FAR_END 7000.0f
#define SHADOW_SPLIT_VIEW_END 5000.0f
#define SHADOW_DEPTH_MARGIN 100.0f

typedef struct Vec3 {
    float x;
    float y;
    float z;
} Vec3;

static bool finite_float(float value) {
    return value <= FLT_MAX && value >= -FLT_MAX;
}

static float dot3(Vec3 left, Vec3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

static Vec3 cross3(Vec3 left, Vec3 right) {
    return (Vec3) {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

static bool normalize3(Vec3 value, Vec3 *out) {
    float length_squared = dot3(value, value);
    float inverse;

    if (out == NULL || !finite_float(length_squared) ||
        length_squared < 1.0e-12f) {
        return false;
    }
    inverse = 1.0f / sqrtf(length_squared);
    *out = (Vec3) {
        value.x * inverse,
        value.y * inverse,
        value.z * inverse,
    };
    return true;
}

static bool invert4x4(
    const float input[4][4],
    double inverse[4][4]) {
    double work[4][8];

    if (input == NULL || inverse == NULL) {
        return false;
    }
    for (size_t row = 0; row < 4; row++) {
        for (size_t column = 0; column < 4; column++) {
            if (!finite_float(input[row][column])) {
                return false;
            }
            work[row][column] = input[row][column];
            work[row][column + 4] = row == column ? 1.0 : 0.0;
        }
    }
    for (size_t column = 0; column < 4; column++) {
        size_t pivot = column;
        double pivot_size = fabs(work[pivot][column]);
        for (size_t row = column + 1; row < 4; row++) {
            double candidate = fabs(work[row][column]);
            if (candidate > pivot_size) {
                pivot = row;
                pivot_size = candidate;
            }
        }
        if (!(pivot_size > 1.0e-12)) {
            return false;
        }
        if (pivot != column) {
            for (size_t index = 0; index < 8; index++) {
                double temporary = work[column][index];
                work[column][index] = work[pivot][index];
                work[pivot][index] = temporary;
            }
        }
        {
            double scale = work[column][column];
            for (size_t index = 0; index < 8; index++) {
                work[column][index] /= scale;
            }
        }
        for (size_t row = 0; row < 4; row++) {
            double scale;
            if (row == column) {
                continue;
            }
            scale = work[row][column];
            for (size_t index = 0; index < 8; index++) {
                work[row][index] -= scale * work[column][index];
            }
        }
    }
    for (size_t row = 0; row < 4; row++) {
        for (size_t column = 0; column < 4; column++) {
            inverse[row][column] = work[row][column + 4];
        }
    }
    return true;
}

static bool unproject(
    const double inverse[4][4],
    float x,
    float y,
    float z,
    Vec3 *out) {
    double input[4] = {x, y, z, 1.0};
    double result[4] = {0.0, 0.0, 0.0, 0.0};

    if (inverse == NULL || out == NULL) {
        return false;
    }
    for (size_t column = 0; column < 4; column++) {
        for (size_t row = 0; row < 4; row++) {
            result[column] += input[row] * inverse[row][column];
        }
    }
    if (!isfinite(result[3]) || fabs(result[3]) < 1.0e-12) {
        return false;
    }
    out->x = (float)(result[0] / result[3]);
    out->y = (float)(result[1] / result[3]);
    out->z = (float)(result[2] / result[3]);
    return finite_float(out->x) &&
           finite_float(out->y) &&
           finite_float(out->z);
}

static Vec3 lerp3(Vec3 start, Vec3 end, float amount) {
    return (Vec3) {
        start.x + (end.x - start.x) * amount,
        start.y + (end.y - start.y) * amount,
        start.z + (end.z - start.z) * amount,
    };
}

static float distance_fraction(float distance) {
    float value =
        (distance - DKR_CAMERA_NEAR) /
        (DKR_CAMERA_FAR - DKR_CAMERA_NEAR);
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static bool valid_view(const GfxShadowView *view) {
    if (view == NULL || !view->valid || view->triangle_count == 0 ||
        !finite_float(view->viewport[0]) ||
        !finite_float(view->viewport[1]) ||
        !finite_float(view->viewport[2]) ||
        !finite_float(view->viewport[3]) ||
        view->viewport[2] <= 0.0f || view->viewport[3] <= 0.0f) {
        return false;
    }
    for (size_t axis = 0; axis < 3; axis++) {
        if (!finite_float(view->bounds_min[axis]) ||
            !finite_float(view->bounds_max[axis]) ||
            view->bounds_min[axis] > view->bounds_max[axis]) {
            return false;
        }
    }
    return true;
}

GfxShadowBudget gfx_shadow_budget_for_views(size_t view_count) {
    GfxShadowBudget budget;
    memset(&budget, 0, sizeof(budget));

    if (view_count == 0 || view_count > GFX_SHADOW_MAX_VIEWS) {
        return budget;
    }
    if (view_count == 1) {
        budget.resolution = 2048;
        budget.cascades_per_view = 2;
    } else if (view_count == 2) {
        budget.resolution = 1024;
        budget.cascades_per_view = 2;
    } else {
        budget.resolution = 1024;
        budget.cascades_per_view = 1;
    }
    budget.map_count =
        (uint8_t)(view_count * budget.cascades_per_view);
    budget.depth_bytes =
        (size_t)budget.resolution *
        (size_t)budget.resolution *
        sizeof(float) *
        budget.map_count;
    return budget;
}

static bool view_corners(
    const GfxShadowView *view,
    Vec3 near_corners[4],
    Vec3 far_corners[4]) {
    double inverse[4][4];
    size_t index = 0;

    if (!valid_view(view) ||
        !invert4x4(view->view_projection, inverse)) {
        return false;
    }
    for (int y = -1; y <= 1; y += 2) {
        for (int x = -1; x <= 1; x += 2) {
            if (!unproject(
                    inverse, (float)x, (float)y, -1.0f,
                    &near_corners[index]) ||
                !unproject(
                    inverse, (float)x, (float)y, 1.0f,
                    &far_corners[index])) {
                return false;
            }
            index++;
        }
    }
    return true;
}

static void include_light_point(
    Vec3 point,
    Vec3 axis_x,
    Vec3 axis_y,
    Vec3 axis_z,
    float minimum[3],
    float maximum[3]) {
    float values[3] = {
        dot3(point, axis_x),
        dot3(point, axis_y),
        dot3(point, axis_z),
    };
    for (size_t axis = 0; axis < 3; axis++) {
        if (values[axis] < minimum[axis]) {
            minimum[axis] = values[axis];
        }
        if (values[axis] > maximum[axis]) {
            maximum[axis] = values[axis];
        }
    }
}

static bool build_cascade(
    const GfxShadowView *view,
    Vec3 light_axis_z,
    float split_near,
    float split_far,
    uint32_t resolution,
    uint8_t view_index,
    uint8_t cascade_index,
    uint8_t map_index,
    GfxShadowCascade *out) {
    Vec3 near_corners[4];
    Vec3 far_corners[4];
    Vec3 slice[8];
    Vec3 reference_up =
        fabsf(light_axis_z.y) < 0.95f
            ? (Vec3) {0.0f, 1.0f, 0.0f}
            : (Vec3) {1.0f, 0.0f, 0.0f};
    Vec3 axis_x;
    Vec3 axis_y;
    float minimum[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float maximum[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    float near_fraction = distance_fraction(split_near);
    float far_fraction = distance_fraction(split_far);
    float center_x;
    float center_y;
    float radius;
    float texel;
    float z_span;

    if (out == NULL || resolution == 0 ||
        !view_corners(view, near_corners, far_corners) ||
        !normalize3(cross3(reference_up, light_axis_z), &axis_x)) {
        return false;
    }
    axis_y = cross3(light_axis_z, axis_x);
    if (!normalize3(axis_y, &axis_y)) {
        return false;
    }
    for (size_t index = 0; index < 4; index++) {
        slice[index] =
            lerp3(near_corners[index], far_corners[index], near_fraction);
        slice[index + 4] =
            lerp3(near_corners[index], far_corners[index], far_fraction);
    }
    for (size_t index = 0; index < 8; index++) {
        include_light_point(
            slice[index], axis_x, axis_y, light_axis_z,
            minimum, maximum);
    }

    /*
     * Receiver x/y comes from the stable frustum slice. Caster bounds extend
     * depth only, so a distant visible object cannot be clipped along the light
     * ray without broadening both cascades to the entire stage.
     */
    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < 2; y++) {
            for (int z = 0; z < 2; z++) {
                Vec3 corner = {
                    x != 0 ? view->bounds_max[0] : view->bounds_min[0],
                    y != 0 ? view->bounds_max[1] : view->bounds_min[1],
                    z != 0 ? view->bounds_max[2] : view->bounds_min[2],
                };
                float light_z = dot3(corner, light_axis_z);
                if (light_z < minimum[2]) {
                    minimum[2] = light_z;
                }
                if (light_z > maximum[2]) {
                    maximum[2] = light_z;
                }
            }
        }
    }
    /*
     * The map extent must be ROTATION-INVARIANT or the snap below cannot hold
     * the map still: texel size is 2*radius/resolution, so a radius taken from
     * the light-space AABB of the slice changes as the camera turns, moving the
     * quantization grid itself and reintroducing the crawl the snap exists to
     * remove. The slice's bounding SPHERE depends only on the slice's own
     * shape (near/far/fov/aspect), which no camera rotation changes.
     */
    {
        Vec3 slice_center = { 0.0f, 0.0f, 0.0f };
        float radius_squared = 0.0f;
        for (size_t index = 0; index < 8; index++) {
            slice_center.x += slice[index].x;
            slice_center.y += slice[index].y;
            slice_center.z += slice[index].z;
        }
        slice_center.x *= 0.125f;
        slice_center.y *= 0.125f;
        slice_center.z *= 0.125f;
        for (size_t index = 0; index < 8; index++) {
            Vec3 offset = {
                slice[index].x - slice_center.x,
                slice[index].y - slice_center.y,
                slice[index].z - slice_center.z,
            };
            float distance_squared = dot3(offset, offset);
            if (distance_squared > radius_squared) {
                radius_squared = distance_squared;
            }
        }
        center_x = dot3(slice_center, axis_x);
        center_y = dot3(slice_center, axis_y);
        radius = sqrtf(radius_squared);
    }
    if (!finite_float(radius) || radius < 1.0f) {
        return false;
    }
    /* Quantizing the extent keeps float noise in the corner math off the grid. */
    radius = ceilf(radius * 16.0f) / 16.0f;
    texel = (radius * 2.0f) / (float)resolution;
    center_x = floorf(center_x / texel + 0.5f) * texel;
    center_y = floorf(center_y / texel + 0.5f) * texel;
    minimum[0] = center_x - radius;
    maximum[0] = center_x + radius;
    minimum[1] = center_y - radius;
    maximum[1] = center_y + radius;
    minimum[2] -= SHADOW_DEPTH_MARGIN;
    maximum[2] += SHADOW_DEPTH_MARGIN;
    z_span = maximum[2] - minimum[2];
    if (!finite_float(z_span) || z_span < 1.0f) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->view_index = view_index;
    out->cascade_index = cascade_index;
    out->map_index = map_index;
    out->split_near = split_near;
    out->split_far = split_far;
    out->world_units_per_texel = texel;
    memcpy(out->light_bounds_min, minimum, sizeof(minimum));
    memcpy(out->light_bounds_max, maximum, sizeof(maximum));

    out->world_to_clip[0][0] = axis_x.x / radius;
    out->world_to_clip[0][1] = axis_x.y / radius;
    out->world_to_clip[0][2] = axis_x.z / radius;
    out->world_to_clip[0][3] = -center_x / radius;
    out->world_to_clip[1][0] = axis_y.x / radius;
    out->world_to_clip[1][1] = axis_y.y / radius;
    out->world_to_clip[1][2] = axis_y.z / radius;
    out->world_to_clip[1][3] = -center_y / radius;
    /*
     * Depth increases AWAY from the sun, which is the only orientation the rest
     * of the pipeline is built for and the one this row got backwards.
     *
     * `sun_direction_world` (and therefore light_axis_z) points TOWARDS the sun
     * — gfx_level_lighting publishes the same vector the RL-5 receiver dots
     * against its surface normal, where a +Y normal must be lit by a +Y sun.
     * Projecting depth along +light_axis_z therefore made the depth buffer grow
     * towards the light, so with both backends' shadow pass at "keep the
     * smallest depth" (GL_LESS / WGPUCompareFunction_Less, clear 1.0) every
     * cascade stored the surface FURTHEST from the sun in each column — the
     * ground — instead of the one nearest it. The receiver's LESS_EQUAL compare
     * then lit exactly that surface and shadowed everything standing on it.
     *
     * That is not a subtle bias artefact; it is the whole visible behaviour of
     * the feed. Measured on Everfrost Peak, frame 3470, before this fix: the
     * track surface identical with shadows on and off (mean 97.37 either way —
     * nothing ever cast onto the ground, and the actor decal handoff had
     * already removed the blob, so karts floated), while the driver's torso sat
     * at a flat full-umbra 83.57 against 123.04 with shadows off. Raising the
     * comparison bias 25x (MDKR_SHADOW_BIAS=200) moved the torso only to 94.61,
     * which is what ruled acne out: nothing about a bias explains a receiver
     * that is occluded by the ground it is standing on.
     *
     * Flipping the sign also restores the winding both backends' shadow pass
     * already assumes. They cull FRONT faces on purpose (second-depth shadow
     * mapping, the standard acne mitigation); mapping a right-handed light
     * basis onto GL clip space with +z had inverted which faces those were.
     */
    out->world_to_clip[2][0] = -light_axis_z.x * (2.0f / z_span);
    out->world_to_clip[2][1] = -light_axis_z.y * (2.0f / z_span);
    out->world_to_clip[2][2] = -light_axis_z.z * (2.0f / z_span);
    out->world_to_clip[2][3] =
        (maximum[2] + minimum[2]) / z_span;
    out->world_to_clip[3][3] = 1.0f;
    return true;
}

bool gfx_shadow_build_plan(
    const GfxShadowFrame *frame,
    const float sun_direction_world[3],
    GfxShadowPlan *out) {
    Vec3 light_axis_z;
    GfxShadowBudget budget;
    size_t map_index = 0;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (frame == NULL || !frame->valid ||
        frame->view_count == 0 ||
        frame->view_count > GFX_SHADOW_MAX_VIEWS ||
        sun_direction_world == NULL ||
        !normalize3(
            (Vec3) {
                sun_direction_world[0],
                sun_direction_world[1],
                sun_direction_world[2],
            },
            &light_axis_z)) {
        return false;
    }
    budget = gfx_shadow_budget_for_views(frame->view_count);
    if (budget.map_count == 0 ||
        budget.map_count > GFX_SHADOW_MAX_MAPS) {
        return false;
    }
    out->frame_generation = frame->generation;
    out->stage_generation = frame->stage_generation;
    out->budget = budget;
    out->view_count = frame->view_count;
    for (size_t view = 0; view < frame->view_count; view++) {
        for (size_t cascade = 0;
             cascade < budget.cascades_per_view;
             cascade++) {
            float split_near;
            float split_far;
            if (budget.cascades_per_view == 1) {
                split_near = DKR_CAMERA_NEAR;
                split_far = SHADOW_SPLIT_VIEW_END;
            } else if (cascade == 0) {
                split_near = DKR_CAMERA_NEAR;
                split_far = SHADOW_ONE_PLAYER_NEAR_END;
            } else {
                split_near = SHADOW_ONE_PLAYER_FAR_BEGIN;
                split_far = SHADOW_ONE_PLAYER_FAR_END;
            }
            if (!build_cascade(
                    &frame->views[view],
                    light_axis_z,
                    split_near,
                    split_far,
                    budget.resolution,
                    (uint8_t)view,
                    (uint8_t)cascade,
                    (uint8_t)map_index,
                    &out->cascades[map_index])) {
                memset(out, 0, sizeof(*out));
                return false;
            }
            map_index++;
        }
    }
    out->cascade_count = map_index;
    out->valid = map_index == budget.map_count;
    return out->valid;
}
