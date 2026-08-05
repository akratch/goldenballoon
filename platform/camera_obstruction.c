#include "camera_obstruction.h"

#include <float.h>
#include <math.h>
#include <string.h>

typedef struct MdkrCameraDVec3 {
    double x;
    double y;
    double z;
} MdkrCameraDVec3;

typedef struct MdkrCameraCandidate {
    double fraction;
    MdkrCameraDVec3 point;
    MdkrCameraDVec3 normal;
    uint32_t stable_id;
    uint32_t kind;
    size_t triangle_index;
    MdkrCameraSweepFeature feature;
    int valid;
} MdkrCameraCandidate;

#define MDKR_CAMERA_PI 3.14159265358979323846264338327950288
#define MDKR_CAMERA_TIME_TIE_EPSILON 1.0e-9
#define MDKR_CAMERA_DEGENERATE_RATIO 1.0e-24

static MdkrCameraDVec3 mdkr_camera_dvec3(MdkrCameraVec3 value) {
    MdkrCameraDVec3 result = { value.x, value.y, value.z };
    return result;
}

static MdkrCameraVec3 mdkr_camera_vec3(MdkrCameraDVec3 value) {
    MdkrCameraVec3 result = { (float)value.x, (float)value.y, (float)value.z };
    return result;
}

static MdkrCameraDVec3 mdkr_camera_add(MdkrCameraDVec3 a, MdkrCameraDVec3 b) {
    MdkrCameraDVec3 result = { a.x + b.x, a.y + b.y, a.z + b.z };
    return result;
}

static MdkrCameraDVec3 mdkr_camera_sub(MdkrCameraDVec3 a, MdkrCameraDVec3 b) {
    MdkrCameraDVec3 result = { a.x - b.x, a.y - b.y, a.z - b.z };
    return result;
}

static MdkrCameraDVec3 mdkr_camera_scale(MdkrCameraDVec3 value, double scale) {
    MdkrCameraDVec3 result = { value.x * scale, value.y * scale, value.z * scale };
    return result;
}

static double mdkr_camera_dot(MdkrCameraDVec3 a, MdkrCameraDVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static MdkrCameraDVec3 mdkr_camera_cross(MdkrCameraDVec3 a, MdkrCameraDVec3 b) {
    MdkrCameraDVec3 result = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
    return result;
}

static int mdkr_camera_dvec3_finite(MdkrCameraDVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static int mdkr_camera_vec3_finite(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static double mdkr_camera_length_squared(MdkrCameraDVec3 value) {
    return mdkr_camera_dot(value, value);
}

static int mdkr_camera_normalize(MdkrCameraDVec3 value, MdkrCameraDVec3 *out_normal) {
    const double length_squared = mdkr_camera_length_squared(value);
    double inverse_length;

    if (!isfinite(length_squared) || length_squared <= DBL_MIN) {
        return 0;
    }
    inverse_length = 1.0 / sqrt(length_squared);
    *out_normal = mdkr_camera_scale(value, inverse_length);
    return mdkr_camera_dvec3_finite(*out_normal);
}

static MdkrCameraDVec3 mdkr_camera_position_at(
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    double fraction) {
    return mdkr_camera_add(start, mdkr_camera_scale(delta, fraction));
}

static int mdkr_camera_time_in_range(double value) {
    return isfinite(value) && value >= -MDKR_CAMERA_TIME_TIE_EPSILON &&
           value <= 1.0 + MDKR_CAMERA_TIME_TIE_EPSILON;
}

static double mdkr_camera_clamp_unit_time(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static int mdkr_camera_triangle_is_degenerate(
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 *out_unit_normal) {
    const MdkrCameraDVec3 ab = mdkr_camera_sub(b, a);
    const MdkrCameraDVec3 ac = mdkr_camera_sub(c, a);
    const MdkrCameraDVec3 bc = mdkr_camera_sub(c, b);
    const MdkrCameraDVec3 cross = mdkr_camera_cross(ab, ac);
    const double ab_squared = mdkr_camera_length_squared(ab);
    const double ac_squared = mdkr_camera_length_squared(ac);
    const double bc_squared = mdkr_camera_length_squared(bc);
    const double max_edge_squared = fmax(ab_squared, fmax(ac_squared, bc_squared));
    const double cross_squared = mdkr_camera_length_squared(cross);

    if (!isfinite(max_edge_squared) || !isfinite(cross_squared) ||
        max_edge_squared <= DBL_MIN ||
        cross_squared <= MDKR_CAMERA_DEGENERATE_RATIO * max_edge_squared * max_edge_squared) {
        return 1;
    }
    return !mdkr_camera_normalize(cross, out_unit_normal);
}

/* Ericson's closest-point regions, with a deterministic feature classification. */
static MdkrCameraDVec3 mdkr_camera_closest_point_triangle(
    MdkrCameraDVec3 point,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraSweepFeature *out_feature) {
    const MdkrCameraDVec3 ab = mdkr_camera_sub(b, a);
    const MdkrCameraDVec3 ac = mdkr_camera_sub(c, a);
    const MdkrCameraDVec3 ap = mdkr_camera_sub(point, a);
    const double d1 = mdkr_camera_dot(ab, ap);
    const double d2 = mdkr_camera_dot(ac, ap);
    MdkrCameraDVec3 bp;
    MdkrCameraDVec3 cp;
    double d3;
    double d4;
    double d5;
    double d6;
    double vc;
    double vb;
    double va;

    if (d1 <= 0.0 && d2 <= 0.0) {
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_VERTEX;
        return a;
    }

    bp = mdkr_camera_sub(point, b);
    d3 = mdkr_camera_dot(ab, bp);
    d4 = mdkr_camera_dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_VERTEX;
        return b;
    }

    vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_EDGE;
        return mdkr_camera_add(a, mdkr_camera_scale(ab, d1 / (d1 - d3)));
    }

    cp = mdkr_camera_sub(point, c);
    d5 = mdkr_camera_dot(ab, cp);
    d6 = mdkr_camera_dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_VERTEX;
        return c;
    }

    vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_EDGE;
        return mdkr_camera_add(a, mdkr_camera_scale(ac, d2 / (d2 - d6)));
    }

    va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const MdkrCameraDVec3 bc = mdkr_camera_sub(c, b);
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_EDGE;
        return mdkr_camera_add(b, mdkr_camera_scale(bc, (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }

    {
        const double denominator = 1.0 / (va + vb + vc);
        *out_feature = MDKR_CAMERA_SWEEP_FEATURE_FACE;
        return mdkr_camera_add(a, mdkr_camera_add(
            mdkr_camera_scale(ab, vb * denominator),
            mdkr_camera_scale(ac, vc * denominator)));
    }
}

static MdkrCameraDVec3 mdkr_camera_fallback_normal(
    MdkrCameraDVec3 triangle_normal,
    MdkrCameraDVec3 movement) {
    const double motion_dot = mdkr_camera_dot(triangle_normal, movement);

    if (motion_dot > 0.0) {
        return mdkr_camera_scale(triangle_normal, -1.0);
    }
    if (motion_dot < 0.0) {
        return triangle_normal;
    }
    /* A stable orientation for a stationary centre lying exactly on a sheet. */
    if (triangle_normal.x < 0.0 ||
        (triangle_normal.x == 0.0 && triangle_normal.y < 0.0) ||
        (triangle_normal.x == 0.0 && triangle_normal.y == 0.0 && triangle_normal.z < 0.0)) {
        return mdkr_camera_scale(triangle_normal, -1.0);
    }
    return triangle_normal;
}

static int mdkr_camera_candidate_better(
    const MdkrCameraCandidate *candidate,
    const MdkrCameraCandidate *best) {
    if (!best->valid) {
        return 1;
    }
    if (candidate->fraction < best->fraction - MDKR_CAMERA_TIME_TIE_EPSILON) {
        return 1;
    }
    if (fabs(candidate->fraction - best->fraction) <= MDKR_CAMERA_TIME_TIE_EPSILON) {
        if (candidate->stable_id != best->stable_id) {
            return candidate->stable_id < best->stable_id;
        }
        if (candidate->feature != best->feature) {
            return candidate->feature < best->feature;
        }
        return candidate->triangle_index < best->triangle_index;
    }
    return 0;
}

static void mdkr_camera_consider_candidate(
    MdkrCameraCandidate *best,
    double fraction,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 triangle_normal,
    double radius,
    uint32_t stable_id,
    uint32_t kind,
    size_t triangle_index,
    MdkrCameraSweepFeature feature) {
    MdkrCameraCandidate candidate;
    MdkrCameraDVec3 center;
    MdkrCameraDVec3 point;
    MdkrCameraDVec3 offset;
    double distance_squared;
    double tolerance;

    if (!mdkr_camera_time_in_range(fraction)) {
        return;
    }
    fraction = mdkr_camera_clamp_unit_time(fraction);
    center = mdkr_camera_position_at(start, delta, fraction);
    point = mdkr_camera_closest_point_triangle(center, a, b, c, &feature);
    offset = mdkr_camera_sub(center, point);
    distance_squared = mdkr_camera_length_squared(offset);
    tolerance = 1.0e-7 * fmax(1.0, radius * radius);
    if (!isfinite(distance_squared) || distance_squared > radius * radius + tolerance) {
        return;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.fraction = fraction;
    candidate.point = point;
    if (!mdkr_camera_normalize(offset, &candidate.normal)) {
        candidate.normal = mdkr_camera_fallback_normal(triangle_normal, delta);
    }
    candidate.stable_id = stable_id;
    candidate.kind = kind;
    candidate.triangle_index = triangle_index;
    candidate.feature = feature;
    candidate.valid = 1;
    if (mdkr_camera_candidate_better(&candidate, best)) {
        *best = candidate;
    }
}

static void mdkr_camera_consider_point_sweep(
    MdkrCameraCandidate *best,
    MdkrCameraDVec3 point,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 triangle_normal,
    double radius,
    uint32_t stable_id,
    uint32_t kind,
    size_t triangle_index) {
    const MdkrCameraDVec3 relative = mdkr_camera_sub(start, point);
    const double qa = mdkr_camera_length_squared(delta);
    const double qb = 2.0 * mdkr_camera_dot(relative, delta);
    const double qc = mdkr_camera_length_squared(relative) - radius * radius;
    double discriminant;
    double root;

    if (qa <= DBL_MIN) {
        return;
    }
    discriminant = qb * qb - 4.0 * qa * qc;
    if (discriminant < 0.0) {
        return;
    }
    root = sqrt(fmax(0.0, discriminant));
    mdkr_camera_consider_candidate(best, (-qb - root) / (2.0 * qa), start, delta,
                                   a, b, c, triangle_normal, radius, stable_id, kind,
                                   triangle_index, MDKR_CAMERA_SWEEP_FEATURE_VERTEX);
    if (root > 0.0) {
        mdkr_camera_consider_candidate(best, (-qb + root) / (2.0 * qa), start, delta,
                                       a, b, c, triangle_normal, radius, stable_id, kind,
                                       triangle_index, MDKR_CAMERA_SWEEP_FEATURE_VERTEX);
    }
}

static void mdkr_camera_consider_edge_sweep(
    MdkrCameraCandidate *best,
    MdkrCameraDVec3 edge_start,
    MdkrCameraDVec3 edge_end,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 triangle_normal,
    double radius,
    uint32_t stable_id,
    uint32_t kind,
    size_t triangle_index) {
    const MdkrCameraDVec3 edge = mdkr_camera_sub(edge_end, edge_start);
    const double edge_squared = mdkr_camera_length_squared(edge);
    const MdkrCameraDVec3 relative = mdkr_camera_sub(start, edge_start);
    double edge_parameter_start;
    double edge_parameter_delta;
    MdkrCameraDVec3 perpendicular_relative;
    MdkrCameraDVec3 perpendicular_delta;
    double qa;
    double qb;
    double qc;
    double discriminant;
    double root;
    double roots[2];
    size_t root_count;
    size_t root_index;

    mdkr_camera_consider_point_sweep(best, edge_start, start, delta, a, b, c,
                                     triangle_normal, radius, stable_id, kind, triangle_index);
    mdkr_camera_consider_point_sweep(best, edge_end, start, delta, a, b, c,
                                     triangle_normal, radius, stable_id, kind, triangle_index);
    if (edge_squared <= DBL_MIN) {
        return;
    }

    edge_parameter_start = mdkr_camera_dot(relative, edge) / edge_squared;
    edge_parameter_delta = mdkr_camera_dot(delta, edge) / edge_squared;
    perpendicular_relative = mdkr_camera_sub(
        relative, mdkr_camera_scale(edge, edge_parameter_start));
    perpendicular_delta = mdkr_camera_sub(delta, mdkr_camera_scale(edge, edge_parameter_delta));
    qa = mdkr_camera_length_squared(perpendicular_delta);
    qb = 2.0 * mdkr_camera_dot(perpendicular_relative, perpendicular_delta);
    qc = mdkr_camera_length_squared(perpendicular_relative) - radius * radius;
    if (qa <= DBL_MIN) {
        return;
    }
    discriminant = qb * qb - 4.0 * qa * qc;
    if (discriminant < 0.0) {
        return;
    }
    root = sqrt(fmax(0.0, discriminant));
    roots[0] = (-qb - root) / (2.0 * qa);
    roots[1] = (-qb + root) / (2.0 * qa);
    root_count = root > 0.0 ? 2U : 1U;
    for (root_index = 0; root_index < root_count; root_index++) {
        const double fraction = roots[root_index];
        const double edge_parameter = edge_parameter_start + edge_parameter_delta * fraction;
        if (edge_parameter > 0.0 && edge_parameter < 1.0) {
            mdkr_camera_consider_candidate(best, fraction, start, delta, a, b, c,
                                           triangle_normal, radius, stable_id, kind,
                                           triangle_index, MDKR_CAMERA_SWEEP_FEATURE_EDGE);
        }
    }
}

static int mdkr_camera_point_in_triangle(
    MdkrCameraDVec3 point,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c) {
    const MdkrCameraDVec3 ab = mdkr_camera_sub(b, a);
    const MdkrCameraDVec3 ac = mdkr_camera_sub(c, a);
    const MdkrCameraDVec3 ap = mdkr_camera_sub(point, a);
    const double dot_ab_ab = mdkr_camera_dot(ab, ab);
    const double dot_ab_ac = mdkr_camera_dot(ab, ac);
    const double dot_ac_ac = mdkr_camera_dot(ac, ac);
    const double dot_ap_ab = mdkr_camera_dot(ap, ab);
    const double dot_ap_ac = mdkr_camera_dot(ap, ac);
    const double denominator = dot_ab_ab * dot_ac_ac - dot_ab_ac * dot_ab_ac;
    double u;
    double v;

    if (!isfinite(denominator) || fabs(denominator) <= DBL_MIN) {
        return 0;
    }
    u = (dot_ac_ac * dot_ap_ab - dot_ab_ac * dot_ap_ac) / denominator;
    v = (dot_ab_ab * dot_ap_ac - dot_ab_ac * dot_ap_ab) / denominator;
    return u >= -1.0e-10 && v >= -1.0e-10 && u + v <= 1.0 + 1.0e-10;
}

static void mdkr_camera_consider_face_sweep(
    MdkrCameraCandidate *best,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 triangle_normal,
    double radius,
    uint32_t stable_id,
    uint32_t kind,
    size_t triangle_index) {
    const double start_distance = mdkr_camera_dot(mdkr_camera_sub(start, a), triangle_normal);
    const double distance_delta = mdkr_camera_dot(delta, triangle_normal);
    double roots[2];
    size_t root_count = 0;
    size_t root_index;

    if (fabs(distance_delta) <= DBL_MIN) {
        return;
    }
    roots[root_count++] = (radius - start_distance) / distance_delta;
    roots[root_count++] = (-radius - start_distance) / distance_delta;
    for (root_index = 0; root_index < root_count; root_index++) {
        const double fraction = roots[root_index];
        MdkrCameraDVec3 center;
        double signed_distance;
        MdkrCameraDVec3 plane_point;
        if (!mdkr_camera_time_in_range(fraction)) {
            continue;
        }
        center = mdkr_camera_position_at(start, delta, mdkr_camera_clamp_unit_time(fraction));
        signed_distance = mdkr_camera_dot(mdkr_camera_sub(center, a), triangle_normal);
        plane_point = mdkr_camera_sub(center, mdkr_camera_scale(triangle_normal, signed_distance));
        if (mdkr_camera_point_in_triangle(plane_point, a, b, c)) {
            mdkr_camera_consider_candidate(best, fraction, start, delta, a, b, c,
                                           triangle_normal, radius, stable_id, kind,
                                           triangle_index, MDKR_CAMERA_SWEEP_FEATURE_FACE);
        }
    }
}

static void mdkr_camera_set_invalid_hit(MdkrCameraSweepHit *out_hit) {
    memset(out_hit, 0, sizeof(*out_hit));
    out_hit->fraction = 0.0f;
}

static void mdkr_camera_set_clear_hit(MdkrCameraSweepHit *out_hit) {
    memset(out_hit, 0, sizeof(*out_hit));
    out_hit->fraction = 1.0f;
    out_hit->clearance = INFINITY;
}

typedef struct MdkrCameraLensClosest {
    double distance_squared;
    MdkrCameraDVec3 lens_point;
    MdkrCameraDVec3 triangle_point;
    MdkrCameraSweepFeature triangle_feature;
    unsigned int face_index;
    unsigned int candidate_index;
    int valid;
} MdkrCameraLensClosest;

#define MDKR_CAMERA_LENS_AXIS_TOLERANCE 1.0e-4
#define MDKR_CAMERA_LENS_DISTANCE_TIE_EPSILON 1.0e-18
#define MDKR_CAMERA_LENS_ADVANCE_ITERATIONS 128U
#define MDKR_CAMERA_LENS_BISECTION_ITERATIONS 16U
#define MDKR_CAMERA_LENS_INTERVAL_SUBDIVISIONS 1024U
#define MDKR_CAMERA_LENS_BOUNDED_INTERVAL_TESTS 96U
#define MDKR_CAMERA_LENS_CLEARANCE_UNCERTAINTY_RATIO 1.0e-9

static int mdkr_camera_dvec3_to_vec3(MdkrCameraDVec3 value, MdkrCameraVec3 *out_value) {
    if (out_value == NULL || !mdkr_camera_dvec3_finite(value) ||
        fabs(value.x) > FLT_MAX || fabs(value.y) > FLT_MAX || fabs(value.z) > FLT_MAX) {
        return 0;
    }
    *out_value = mdkr_camera_vec3(value);
    return 1;
}

static MdkrCameraDVec3 mdkr_camera_closest_point_segment(
    MdkrCameraDVec3 point,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 end,
    double *out_parameter) {
    const MdkrCameraDVec3 edge = mdkr_camera_sub(end, start);
    const double edge_squared = mdkr_camera_length_squared(edge);
    double parameter = 0.0;

    if (edge_squared > DBL_MIN && isfinite(edge_squared)) {
        parameter = mdkr_camera_dot(mdkr_camera_sub(point, start), edge) / edge_squared;
        parameter = fmax(0.0, fmin(1.0, parameter));
    }
    if (out_parameter != NULL) {
        *out_parameter = parameter;
    }
    return mdkr_camera_add(start, mdkr_camera_scale(edge, parameter));
}

/* Closest pair for two non-degenerate (or safely point-like) segments. */
static void mdkr_camera_closest_segment_segment(
    MdkrCameraDVec3 a0,
    MdkrCameraDVec3 a1,
    MdkrCameraDVec3 b0,
    MdkrCameraDVec3 b1,
    MdkrCameraDVec3 *out_a,
    MdkrCameraDVec3 *out_b,
    double *out_b_parameter) {
    const MdkrCameraDVec3 u = mdkr_camera_sub(a1, a0);
    const MdkrCameraDVec3 v = mdkr_camera_sub(b1, b0);
    const MdkrCameraDVec3 w = mdkr_camera_sub(a0, b0);
    const double uu = mdkr_camera_dot(u, u);
    const double uv = mdkr_camera_dot(u, v);
    const double vv = mdkr_camera_dot(v, v);
    const double uw = mdkr_camera_dot(u, w);
    const double vw = mdkr_camera_dot(v, w);
    const double denominator = uu * vv - uv * uv;
    double numerator_a;
    double denominator_a;
    double numerator_b;
    double denominator_b;
    double parameter_a;
    double parameter_b;

    if (uu <= DBL_MIN) {
        *out_a = a0;
        *out_b = mdkr_camera_closest_point_segment(a0, b0, b1, &parameter_b);
        if (out_b_parameter != NULL) {
            *out_b_parameter = parameter_b;
        }
        return;
    }
    if (vv <= DBL_MIN) {
        *out_b = b0;
        *out_a = mdkr_camera_closest_point_segment(b0, a0, a1, NULL);
        if (out_b_parameter != NULL) {
            *out_b_parameter = 0.0;
        }
        return;
    }

    if (fabs(denominator) <= DBL_MIN) {
        numerator_a = 0.0;
        denominator_a = 1.0;
        numerator_b = vw;
        denominator_b = vv;
    } else {
        numerator_a = uv * vw - vv * uw;
        denominator_a = denominator;
        numerator_b = uu * vw - uv * uw;
        denominator_b = denominator;
        if (numerator_a < 0.0) {
            numerator_a = 0.0;
            numerator_b = vw;
            denominator_b = vv;
        } else if (numerator_a > denominator_a) {
            numerator_a = denominator_a;
            numerator_b = vw + uv;
            denominator_b = vv;
        }
    }
    if (numerator_b < 0.0) {
        numerator_b = 0.0;
        if (-uw < 0.0) {
            numerator_a = 0.0;
            denominator_a = 1.0;
        } else if (-uw > uu) {
            numerator_a = denominator_a;
        } else {
            numerator_a = -uw;
            denominator_a = uu;
        }
    } else if (numerator_b > denominator_b) {
        numerator_b = denominator_b;
        if (-uw + uv < 0.0) {
            numerator_a = 0.0;
            denominator_a = 1.0;
        } else if (-uw + uv > uu) {
            numerator_a = denominator_a;
        } else {
            numerator_a = -uw + uv;
            denominator_a = uu;
        }
    }
    parameter_a = fabs(numerator_a) <= DBL_MIN ? 0.0 : numerator_a / denominator_a;
    parameter_b = fabs(numerator_b) <= DBL_MIN ? 0.0 : numerator_b / denominator_b;
    *out_a = mdkr_camera_add(a0, mdkr_camera_scale(u, parameter_a));
    *out_b = mdkr_camera_add(b0, mdkr_camera_scale(v, parameter_b));
    if (out_b_parameter != NULL) {
        *out_b_parameter = parameter_b;
    }
}

static void mdkr_camera_lens_consider_closest(
    MdkrCameraLensClosest *best,
    MdkrCameraDVec3 lens_point,
    MdkrCameraDVec3 triangle_point,
    MdkrCameraSweepFeature triangle_feature,
    unsigned int face_index,
    unsigned int candidate_index) {
    const double distance_squared = mdkr_camera_length_squared(
        mdkr_camera_sub(lens_point, triangle_point));
    MdkrCameraLensClosest candidate;

    if (!isfinite(distance_squared)) {
        return;
    }
    candidate.distance_squared = fmax(0.0, distance_squared);
    candidate.lens_point = lens_point;
    candidate.triangle_point = triangle_point;
    candidate.triangle_feature = triangle_feature;
    candidate.face_index = face_index;
    candidate.candidate_index = candidate_index;
    candidate.valid = 1;
    if (!best->valid ||
        candidate.distance_squared < best->distance_squared - MDKR_CAMERA_LENS_DISTANCE_TIE_EPSILON ||
        (fabs(candidate.distance_squared - best->distance_squared) <=
             MDKR_CAMERA_LENS_DISTANCE_TIE_EPSILON &&
         (candidate.triangle_feature < best->triangle_feature ||
          (candidate.triangle_feature == best->triangle_feature &&
           (candidate.face_index < best->face_index ||
            (candidate.face_index == best->face_index &&
             candidate.candidate_index < best->candidate_index)))))) {
        *best = candidate;
    }
}

static int mdkr_camera_rounded_lens_guard_valid(const MdkrCameraRoundedLensGuard *guard) {
    MdkrCameraDVec3 forward;
    MdkrCameraDVec3 right;
    MdkrCameraDVec3 up;
    MdkrCameraDVec3 handed;
    double expected_radius;

    if (guard == NULL || !mdkr_camera_vec3_finite(guard->forward) ||
        !mdkr_camera_vec3_finite(guard->right) || !mdkr_camera_vec3_finite(guard->up) ||
        !isfinite(guard->near_distance) || !isfinite(guard->half_width) ||
        !isfinite(guard->half_height) || !isfinite(guard->skin) ||
        !isfinite(guard->broadphase_radius) || guard->near_distance <= 0.0f ||
        guard->half_width <= 0.0f || guard->half_height <= 0.0f || guard->skin < 0.0f ||
        guard->broadphase_radius < 0.0f) {
        return 0;
    }
    forward = mdkr_camera_dvec3(guard->forward);
    right = mdkr_camera_dvec3(guard->right);
    up = mdkr_camera_dvec3(guard->up);
    handed = mdkr_camera_cross(right, up);
    if (fabs(mdkr_camera_length_squared(forward) - 1.0) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_length_squared(right) - 1.0) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_length_squared(up) - 1.0) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_dot(forward, right)) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_dot(forward, up)) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_dot(right, up)) > MDKR_CAMERA_LENS_AXIS_TOLERANCE ||
        mdkr_camera_dot(handed, forward) > -1.0 + MDKR_CAMERA_LENS_AXIS_TOLERANCE) {
        return 0;
    }
    expected_radius = sqrt((double)guard->near_distance * guard->near_distance +
                           (double)guard->half_width * guard->half_width +
                           (double)guard->half_height * guard->half_height) + guard->skin;
    return isfinite(expected_radius) && expected_radius <= FLT_MAX &&
           (double)guard->broadphase_radius >=
               (double)nextafterf((float)expected_radius, -INFINITY) &&
           (double)guard->broadphase_radius <=
               MDKR_CAMERA_LENS_AXIS_TOLERANCE * fmax(1.0, expected_radius) + expected_radius;
}

int mdkr_camera_rounded_lens_guard_conservative_radius(
    const MdkrCameraRoundedLensGuard *guard,
    double *out_radius) {
    double radius;

    if (out_radius == NULL || !mdkr_camera_rounded_lens_guard_valid(guard)) {
        return 0;
    }
    radius = sqrt((double)guard->near_distance * guard->near_distance +
                  (double)guard->half_width * guard->half_width +
                  (double)guard->half_height * guard->half_height) + guard->skin;
    if (!isfinite(radius) || radius < 0.0) {
        return 0;
    }
    *out_radius = nextafter(radius, INFINITY);
    return isfinite(*out_radius);
}

static int mdkr_camera_rounded_lens_corners(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 eye,
    MdkrCameraDVec3 out_corners[4]) {
    const MdkrCameraDVec3 forward = mdkr_camera_dvec3(guard->forward);
    const MdkrCameraDVec3 right = mdkr_camera_dvec3(guard->right);
    const MdkrCameraDVec3 up = mdkr_camera_dvec3(guard->up);
    const MdkrCameraDVec3 center = mdkr_camera_add(
        eye, mdkr_camera_scale(forward, guard->near_distance));
    const MdkrCameraDVec3 horizontal = mdkr_camera_scale(right, guard->half_width);
    const MdkrCameraDVec3 vertical = mdkr_camera_scale(up, guard->half_height);

    out_corners[0] = mdkr_camera_sub(mdkr_camera_sub(center, horizontal), vertical);
    out_corners[1] = mdkr_camera_add(mdkr_camera_sub(center, vertical), horizontal);
    out_corners[2] = mdkr_camera_add(mdkr_camera_add(center, horizontal), vertical);
    out_corners[3] = mdkr_camera_add(mdkr_camera_sub(center, horizontal), vertical);
    return mdkr_camera_dvec3_finite(center) && mdkr_camera_dvec3_finite(horizontal) &&
           mdkr_camera_dvec3_finite(vertical) && mdkr_camera_dvec3_finite(out_corners[0]) &&
           mdkr_camera_dvec3_finite(out_corners[1]) && mdkr_camera_dvec3_finite(out_corners[2]) &&
           mdkr_camera_dvec3_finite(out_corners[3]);
}

static int mdkr_camera_point_in_rounded_lens_pyramid(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 eye,
    MdkrCameraDVec3 point) {
    const MdkrCameraDVec3 relative = mdkr_camera_sub(point, eye);
    const double forward = mdkr_camera_dot(relative, mdkr_camera_dvec3(guard->forward));
    const double horizontal = fabs(mdkr_camera_dot(relative, mdkr_camera_dvec3(guard->right)));
    const double vertical = fabs(mdkr_camera_dot(relative, mdkr_camera_dvec3(guard->up)));
    const double tolerance = 1.0e-10 * fmax(1.0, guard->near_distance);
    const double scale = forward / guard->near_distance;

    return forward >= -tolerance && forward <= guard->near_distance + tolerance &&
           horizontal <= scale * guard->half_width + tolerance &&
           vertical <= scale * guard->half_height + tolerance;
}

static MdkrCameraSweepFeature mdkr_camera_segment_target_feature(double parameter) {
    return parameter <= 1.0e-10 || parameter >= 1.0 - 1.0e-10 ?
        MDKR_CAMERA_SWEEP_FEATURE_VERTEX : MDKR_CAMERA_SWEEP_FEATURE_EDGE;
}

static int mdkr_camera_lens_triangle_axes(
    const MdkrCameraDVec3 lens[5],
    const MdkrCameraDVec3 triangle[3],
    MdkrCameraDVec3 out_axes[31],
    size_t *out_axis_count) {
    static const unsigned int faces[6][3] = {
        { 4U, 0U, 1U }, { 4U, 1U, 2U }, { 4U, 2U, 3U },
        { 4U, 3U, 0U }, { 0U, 1U, 2U }, { 0U, 2U, 3U },
    };
    static const unsigned int lens_edges[8][2] = {
        { 4U, 0U }, { 4U, 1U }, { 4U, 2U }, { 4U, 3U },
        { 0U, 1U }, { 1U, 2U }, { 2U, 3U }, { 3U, 0U },
    };
    size_t axis_count = 0U;
    size_t face_index;
    size_t lens_edge;

    if (out_axes == NULL || out_axis_count == NULL) {
        return 0;
    }
    for (face_index = 0U; face_index < 6U; face_index++) {
        const MdkrCameraDVec3 ab = mdkr_camera_sub(lens[faces[face_index][1]],
                                                    lens[faces[face_index][0]]);
        const MdkrCameraDVec3 ac = mdkr_camera_sub(lens[faces[face_index][2]],
                                                    lens[faces[face_index][0]]);
        out_axes[axis_count++] = mdkr_camera_cross(ab, ac);
    }
    out_axes[axis_count++] = mdkr_camera_cross(
        mdkr_camera_sub(triangle[1], triangle[0]),
        mdkr_camera_sub(triangle[2], triangle[0]));
    for (lens_edge = 0U; lens_edge < 8U; lens_edge++) {
        const MdkrCameraDVec3 edge = mdkr_camera_sub(lens[lens_edges[lens_edge][1]],
                                                      lens[lens_edges[lens_edge][0]]);
        size_t triangle_edge;
        for (triangle_edge = 0U; triangle_edge < 3U; triangle_edge++) {
            out_axes[axis_count++] = mdkr_camera_cross(
                edge, mdkr_camera_sub(triangle[(triangle_edge + 1U) % 3U], triangle[triangle_edge]));
        }
    }
    *out_axis_count = axis_count;
    return 1;
}

/* Separating-axis test for a triangle against the unrounded convex lens hull. */
static int mdkr_camera_rounded_lens_pyramid_intersects_triangle(
    const MdkrCameraDVec3 lens[5],
    const MdkrCameraDVec3 triangle[3],
    int *out_intersects) {
    MdkrCameraDVec3 axes[31];
    size_t axis_count;
    size_t axis_index;

    if (out_intersects == NULL ||
        !mdkr_camera_lens_triangle_axes(lens, triangle, axes, &axis_count)) {
        return 0;
    }
    for (axis_index = 0U; axis_index < axis_count; axis_index++) {
        const MdkrCameraDVec3 axis = axes[axis_index];
        const double axis_squared = mdkr_camera_length_squared(axis);
        double lens_min;
        double lens_max;
        double triangle_min;
        double triangle_max;
        double tolerance;
        size_t vertex_index;

        if (!isfinite(axis_squared)) {
            return 0;
        }
        if (axis_squared <= DBL_MIN) {
            continue;
        }
        lens_min = lens_max = mdkr_camera_dot(lens[0], axis);
        triangle_min = triangle_max = mdkr_camera_dot(triangle[0], axis);
        if (!isfinite(lens_min) || !isfinite(triangle_min)) {
            return 0;
        }
        for (vertex_index = 1U; vertex_index < 5U; vertex_index++) {
            const double projection = mdkr_camera_dot(lens[vertex_index], axis);
            if (!isfinite(projection)) {
                return 0;
            }
            lens_min = fmin(lens_min, projection);
            lens_max = fmax(lens_max, projection);
        }
        for (vertex_index = 1U; vertex_index < 3U; vertex_index++) {
            const double projection = mdkr_camera_dot(triangle[vertex_index], axis);
            if (!isfinite(projection)) {
                return 0;
            }
            triangle_min = fmin(triangle_min, projection);
            triangle_max = fmax(triangle_max, projection);
        }
        tolerance = 1.0e-12 * fmax(1.0, fmax(fabs(lens_min), fmax(fabs(lens_max),
                                                              fmax(fabs(triangle_min), fabs(triangle_max)))));
        if (lens_max < triangle_min - tolerance || triangle_max < lens_min - tolerance) {
            *out_intersects = 0;
            return 1;
        }
    }
    *out_intersects = 1;
    return 1;
}

/*
 * With a fixed lens basis, every lens edge and face normal is fixed during
 * translation. Project the start hull, expand its interval by skin * |axis|,
 * and intersect the per-axis time intervals over [0,1]. A separating interval
 * is a proof of CLEAR; an overlapping interval supplies a conservative entry
 * candidate which the closest-feature rounded predicate must revalidate.
 */
static int mdkr_camera_rounded_lens_swept_sat_interval(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    MdkrCameraVec3 a_value,
    MdkrCameraVec3 b_value,
    MdkrCameraVec3 c_value,
    int *out_intersects,
    double *out_enter_fraction,
    double *out_exit_fraction) {
    MdkrCameraDVec3 lens[5];
    MdkrCameraDVec3 triangle[3];
    MdkrCameraDVec3 axes[31];
    size_t axis_count;
    size_t axis_index;
    double enter = 0.0;
    double exit = 1.0;

    if (guard == NULL || out_intersects == NULL || out_enter_fraction == NULL ||
        out_exit_fraction == NULL ||
        !mdkr_camera_rounded_lens_corners(guard, start, lens)) {
        return 0;
    }
    lens[4] = start;
    triangle[0] = mdkr_camera_dvec3(a_value);
    triangle[1] = mdkr_camera_dvec3(b_value);
    triangle[2] = mdkr_camera_dvec3(c_value);
    if (!mdkr_camera_dvec3_finite(delta) ||
        !mdkr_camera_lens_triangle_axes(lens, triangle, axes, &axis_count)) {
        return 0;
    }
    for (axis_index = 0U; axis_index < axis_count; axis_index++) {
        const MdkrCameraDVec3 axis = axes[axis_index];
        const double axis_squared = mdkr_camera_length_squared(axis);
        double lens_min;
        double lens_max;
        double triangle_min;
        double triangle_max;
        double expansion;
        double velocity;
        size_t vertex_index;

        if (!isfinite(axis_squared)) {
            return 0;
        }
        if (axis_squared <= DBL_MIN) {
            continue;
        }
        lens_min = lens_max = mdkr_camera_dot(lens[0], axis);
        triangle_min = triangle_max = mdkr_camera_dot(triangle[0], axis);
        for (vertex_index = 1U; vertex_index < 5U; vertex_index++) {
            const double projection = mdkr_camera_dot(lens[vertex_index], axis);
            if (!isfinite(projection)) {
                return 0;
            }
            lens_min = fmin(lens_min, projection);
            lens_max = fmax(lens_max, projection);
        }
        for (vertex_index = 1U; vertex_index < 3U; vertex_index++) {
            const double projection = mdkr_camera_dot(triangle[vertex_index], axis);
            if (!isfinite(projection)) {
                return 0;
            }
            triangle_min = fmin(triangle_min, projection);
            triangle_max = fmax(triangle_max, projection);
        }
        expansion = (double)guard->skin * sqrt(axis_squared);
        velocity = mdkr_camera_dot(delta, axis);
        if (!isfinite(lens_min) || !isfinite(lens_max) ||
            !isfinite(triangle_min) || !isfinite(triangle_max) ||
            !isfinite(expansion) || !isfinite(velocity)) {
            return 0;
        }
        lens_min -= expansion;
        lens_max += expansion;
        if (fabs(velocity) <= DBL_MIN) {
            const double tolerance = 1.0e-12 * fmax(
                1.0, fmax(fabs(lens_min), fmax(fabs(lens_max),
                    fmax(fabs(triangle_min), fabs(triangle_max)))));
            if (lens_max < triangle_min - tolerance ||
                triangle_max < lens_min - tolerance) {
                *out_intersects = 0;
                *out_enter_fraction = 1.0;
                *out_exit_fraction = 0.0;
                return 1;
            }
        } else {
            double axis_enter = (triangle_min - lens_max) / velocity;
            double axis_exit = (triangle_max - lens_min) / velocity;
            if (!isfinite(axis_enter) || !isfinite(axis_exit)) {
                return 0;
            }
            if (axis_enter > axis_exit) {
                const double temporary = axis_enter;
                axis_enter = axis_exit;
                axis_exit = temporary;
            }
            enter = fmax(enter, axis_enter);
            exit = fmin(exit, axis_exit);
            if (enter > exit + 1.0e-12 || exit < 0.0 || enter > 1.0) {
                *out_intersects = 0;
                *out_enter_fraction = 1.0;
                *out_exit_fraction = 0.0;
                return 1;
            }
        }
    }
    *out_intersects = enter <= exit + 1.0e-12 && exit >= 0.0 && enter <= 1.0;
    *out_enter_fraction = fmin(1.0, fmax(0.0, enter));
    *out_exit_fraction = fmin(1.0, fmax(0.0, exit));
    return 1;
}

static int mdkr_camera_segment_intersects_triangle(
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 end,
    MdkrCameraDVec3 a,
    MdkrCameraDVec3 b,
    MdkrCameraDVec3 c,
    MdkrCameraDVec3 triangle_normal,
    MdkrCameraDVec3 *out_point) {
    const double start_distance = mdkr_camera_dot(mdkr_camera_sub(start, a), triangle_normal);
    const double end_distance = mdkr_camera_dot(mdkr_camera_sub(end, a), triangle_normal);
    const double denominator = start_distance - end_distance;
    double fraction;
    MdkrCameraDVec3 point;

    if (!isfinite(start_distance) || !isfinite(end_distance) || fabs(denominator) <= DBL_MIN) {
        return 0;
    }
    fraction = start_distance / denominator;
    if (!mdkr_camera_time_in_range(fraction)) {
        return 0;
    }
    point = mdkr_camera_add(start, mdkr_camera_scale(mdkr_camera_sub(end, start), fraction));
    if (!mdkr_camera_point_in_triangle(point, a, b, c)) {
        return 0;
    }
    *out_point = point;
    return 1;
}

int mdkr_camera_lens_guard_from_projection(
    float near_distance,
    float vertical_fov_radians,
    float aspect,
    float skin,
    MdkrCameraLensGuard *out_guard) {
    double half_y;
    double half_x;
    double radius;

    if (out_guard == NULL || !isfinite(near_distance) || !isfinite(vertical_fov_radians) ||
        !isfinite(aspect) || !isfinite(skin) || near_distance <= 0.0f || aspect <= 0.0f ||
        skin < 0.0f || vertical_fov_radians <= 0.0f ||
        vertical_fov_radians >= (float)MDKR_CAMERA_PI) {
        return 0;
    }
    half_y = (double)near_distance * tan((double)vertical_fov_radians * 0.5);
    half_x = half_y * (double)aspect;
    radius = sqrt((double)near_distance * near_distance + half_x * half_x + half_y * half_y) + skin;
    if (!isfinite(radius) || radius > FLT_MAX) {
        return 0;
    }
    out_guard->kind = MDKR_CAMERA_LENS_GUARD_SPHERE;
    out_guard->radius = (float)radius;
    return 1;
}

int mdkr_camera_rounded_lens_guard_from_projection(
    float near_distance,
    float vertical_fov_radians,
    float aspect,
    float skin,
    MdkrCameraVec3 forward,
    MdkrCameraVec3 up_hint,
    MdkrCameraRoundedLensGuard *out_guard) {
    MdkrCameraDVec3 basis_forward;
    MdkrCameraDVec3 basis_right;
    MdkrCameraDVec3 basis_up;
    double half_height;
    double half_width;
    double broadphase_radius;
    MdkrCameraRoundedLensGuard guard;

    if (out_guard == NULL || !isfinite(near_distance) || !isfinite(vertical_fov_radians) ||
        !isfinite(aspect) || !isfinite(skin) || !mdkr_camera_vec3_finite(forward) ||
        !mdkr_camera_vec3_finite(up_hint) || near_distance <= 0.0f || aspect <= 0.0f ||
        skin < 0.0f || vertical_fov_radians <= 0.0f ||
        vertical_fov_radians >= (float)MDKR_CAMERA_PI) {
        return 0;
    }
    if (!mdkr_camera_normalize(mdkr_camera_dvec3(forward), &basis_forward) ||
        !mdkr_camera_normalize(mdkr_camera_cross(basis_forward, mdkr_camera_dvec3(up_hint)),
                               &basis_right) ||
        !mdkr_camera_normalize(mdkr_camera_cross(basis_right, basis_forward), &basis_up)) {
        return 0;
    }
    half_height = (double)near_distance * tan((double)vertical_fov_radians * 0.5);
    half_width = half_height * (double)aspect;
    broadphase_radius = sqrt((double)near_distance * near_distance + half_width * half_width +
                             half_height * half_height) + skin;
    if (!isfinite(half_height) || !isfinite(half_width) || !isfinite(broadphase_radius) ||
        half_height > FLT_MAX || half_width > FLT_MAX || broadphase_radius > FLT_MAX ||
        !mdkr_camera_dvec3_to_vec3(basis_forward, &guard.forward) ||
        !mdkr_camera_dvec3_to_vec3(basis_right, &guard.right) ||
        !mdkr_camera_dvec3_to_vec3(basis_up, &guard.up)) {
        return 0;
    }
    guard.near_distance = near_distance;
    guard.half_width = (float)half_width;
    guard.half_height = (float)half_height;
    guard.skin = skin;
    guard.broadphase_radius = (float)broadphase_radius;
    if (!mdkr_camera_rounded_lens_guard_valid(&guard)) {
        return 0;
    }
    *out_guard = guard;
    return 1;
}

static MdkrCameraSweepStatus mdkr_camera_rounded_lens_guard_triangle_test_d(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 eye_value,
    MdkrCameraVec3 a_value,
    MdkrCameraVec3 b_value,
    MdkrCameraVec3 c_value,
    MdkrCameraLensNarrowHit *out_hit,
    double *out_exact_clearance) {
    static const unsigned int face_corners[6][3] = {
        { 4U, 0U, 1U }, { 4U, 1U, 2U }, { 4U, 2U, 3U },
        { 4U, 3U, 0U }, { 0U, 1U, 2U }, { 0U, 2U, 3U },
    };
    MdkrCameraDVec3 corners[4];
    MdkrCameraDVec3 vertices[5];
    MdkrCameraDVec3 triangle[3];
    MdkrCameraDVec3 triangle_normal;
    MdkrCameraLensClosest best;
    int pyramid_intersects;
    size_t triangle_vertex;
    unsigned int face_index;
    double distance;
    double contact_tolerance;
    MdkrCameraDVec3 normal;

    if (out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_hit, 0, sizeof(*out_hit));
    if (!mdkr_camera_rounded_lens_guard_valid(guard) || !mdkr_camera_dvec3_finite(eye_value) ||
        !mdkr_camera_vec3_finite(a_value) || !mdkr_camera_vec3_finite(b_value) ||
        !mdkr_camera_vec3_finite(c_value)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    vertices[4] = eye_value;
    triangle[0] = mdkr_camera_dvec3(a_value);
    triangle[1] = mdkr_camera_dvec3(b_value);
    triangle[2] = mdkr_camera_dvec3(c_value);
    if (!mdkr_camera_rounded_lens_corners(guard, vertices[4], corners) ||
        mdkr_camera_triangle_is_degenerate(triangle[0], triangle[1], triangle[2], &triangle_normal)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    vertices[0] = corners[0];
    vertices[1] = corners[1];
    vertices[2] = corners[2];
    vertices[3] = corners[3];
    memset(&best, 0, sizeof(best));

    if (!mdkr_camera_rounded_lens_pyramid_intersects_triangle(vertices, triangle,
                                                               &pyramid_intersects)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (pyramid_intersects) {
        MdkrCameraSweepFeature triangle_feature;
        const MdkrCameraDVec3 triangle_point = mdkr_camera_closest_point_triangle(
            vertices[4], triangle[0], triangle[1], triangle[2], &triangle_feature);
        mdkr_camera_lens_consider_closest(&best, triangle_point, triangle_point,
                                          triangle_feature, 0U, 0U);
    }
    {
        static const unsigned int lens_edges[8][2] = {
            { 4U, 0U }, { 4U, 1U }, { 4U, 2U }, { 4U, 3U },
            { 0U, 1U }, { 1U, 2U }, { 2U, 3U }, { 3U, 0U },
        };
        size_t edge_index;
        for (edge_index = 0U; edge_index < 8U; edge_index++) {
            MdkrCameraDVec3 point;
            if (mdkr_camera_segment_intersects_triangle(
                    vertices[lens_edges[edge_index][0]], vertices[lens_edges[edge_index][1]],
                    triangle[0], triangle[1], triangle[2], triangle_normal, &point)) {
                mdkr_camera_lens_consider_closest(&best, point, point,
                                                  MDKR_CAMERA_SWEEP_FEATURE_FACE, 0U,
                                                  (unsigned int)edge_index);
            }
        }
    }

    /* A triangle vertex inside the unrounded solid is an exact zero-distance hit. */
    for (triangle_vertex = 0U; triangle_vertex < 3U; triangle_vertex++) {
        if (mdkr_camera_point_in_rounded_lens_pyramid(guard, vertices[4], triangle[triangle_vertex])) {
            mdkr_camera_lens_consider_closest(&best, triangle[triangle_vertex], triangle[triangle_vertex],
                                              MDKR_CAMERA_SWEEP_FEATURE_VERTEX, 0U,
                                              (unsigned int)triangle_vertex);
        }
    }

    for (face_index = 0U; face_index < 6U; face_index++) {
        const MdkrCameraDVec3 fa = vertices[face_corners[face_index][0]];
        const MdkrCameraDVec3 fb = vertices[face_corners[face_index][1]];
        const MdkrCameraDVec3 fc = vertices[face_corners[face_index][2]];
        unsigned int candidate_index = 0U;
        size_t index;

        for (index = 0U; index < 3U; index++) {
            MdkrCameraSweepFeature unused_feature;
            const MdkrCameraDVec3 lens_point = mdkr_camera_closest_point_triangle(
                triangle[index], fa, fb, fc, &unused_feature);
            mdkr_camera_lens_consider_closest(&best, lens_point, triangle[index],
                                              MDKR_CAMERA_SWEEP_FEATURE_VERTEX, face_index,
                                              candidate_index++);
        }
        for (index = 0U; index < 3U; index++) {
            MdkrCameraSweepFeature triangle_feature;
            const MdkrCameraDVec3 triangle_point = mdkr_camera_closest_point_triangle(
                vertices[face_corners[face_index][index]], triangle[0], triangle[1], triangle[2],
                &triangle_feature);
            mdkr_camera_lens_consider_closest(
                &best, vertices[face_corners[face_index][index]], triangle_point,
                triangle_feature, face_index, candidate_index++);
        }
        for (index = 0U; index < 3U; index++) {
            const MdkrCameraDVec3 face_edge_start = vertices[face_corners[face_index][index]];
            const MdkrCameraDVec3 face_edge_end =
                vertices[face_corners[face_index][(index + 1U) % 3U]];
            size_t target_edge;
            for (target_edge = 0U; target_edge < 3U; target_edge++) {
                MdkrCameraDVec3 lens_point;
                MdkrCameraDVec3 triangle_point;
                double triangle_parameter;
                mdkr_camera_closest_segment_segment(
                    face_edge_start, face_edge_end, triangle[target_edge],
                    triangle[(target_edge + 1U) % 3U], &lens_point, &triangle_point,
                    &triangle_parameter);
                mdkr_camera_lens_consider_closest(
                    &best, lens_point, triangle_point,
                    mdkr_camera_segment_target_feature(triangle_parameter), face_index,
                    candidate_index++);
            }
        }
    }
    if (!best.valid || !isfinite(best.distance_squared)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    distance = sqrt(best.distance_squared);
    contact_tolerance = 1.0e-9 * fmax(1.0, (double)guard->near_distance +
                                             guard->half_width + guard->half_height + guard->skin);
    if (!isfinite(distance) || distance > FLT_MAX ||
        !mdkr_camera_dvec3_to_vec3(best.triangle_point, &out_hit->point)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    normal = mdkr_camera_sub(best.lens_point, best.triangle_point);
    if (mdkr_camera_length_squared(normal) <= contact_tolerance * contact_tolerance ||
        !mdkr_camera_normalize(normal, &normal)) {
        const MdkrCameraDVec3 toward_eye = mdkr_camera_sub(vertices[4], best.triangle_point);
        if (mdkr_camera_dot(triangle_normal, toward_eye) < 0.0) {
            triangle_normal = mdkr_camera_scale(triangle_normal, -1.0);
        }
        normal = triangle_normal;
    }
    if (!mdkr_camera_dvec3_to_vec3(normal, &out_hit->normal)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    out_hit->clearance = distance <= guard->skin + contact_tolerance ?
        (float)fmin(0.0, distance - guard->skin) : (float)(distance - guard->skin);
    out_hit->penetration_depth = distance < guard->skin ? (float)(guard->skin - distance) : 0.0f;
    out_hit->feature = best.triangle_feature;
    out_hit->overlapping = distance <= guard->skin + contact_tolerance ? 1U : 0U;
    if (out_exact_clearance != NULL) {
        *out_exact_clearance = distance - guard->skin;
    }
    return out_hit->overlapping ? MDKR_CAMERA_SWEEP_HIT : MDKR_CAMERA_SWEEP_CLEAR;
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_guard_triangle_test(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraVec3 eye,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    MdkrCameraLensNarrowHit *out_hit) {
    return mdkr_camera_rounded_lens_guard_triangle_test_d(
        guard, mdkr_camera_dvec3(eye), a, b, c, out_hit, NULL);
}

typedef struct MdkrCameraRoundedLensSweepCandidate {
    double fraction;
    MdkrCameraLensNarrowHit narrow_hit;
    uint32_t stable_id;
    uint32_t kind;
    size_t triangle_index;
    uint8_t started_overlapping;
    int valid;
} MdkrCameraRoundedLensSweepCandidate;

static void mdkr_camera_rounded_lens_telemetry_increment(uint64_t *counter) {
    if (counter != NULL && *counter != UINT64_MAX) {
        (*counter)++;
    }
}

static int mdkr_camera_rounded_lens_candidate_better(
    const MdkrCameraRoundedLensSweepCandidate *candidate,
    const MdkrCameraRoundedLensSweepCandidate *best) {
    if (!best->valid) {
        return 1;
    }
    if (candidate->fraction < best->fraction - MDKR_CAMERA_TIME_TIE_EPSILON) {
        return 1;
    }
    if (fabs(candidate->fraction - best->fraction) <= MDKR_CAMERA_TIME_TIE_EPSILON) {
        if (candidate->stable_id != best->stable_id) {
            return candidate->stable_id < best->stable_id;
        }
        if (candidate->narrow_hit.feature != best->narrow_hit.feature) {
            return candidate->narrow_hit.feature < best->narrow_hit.feature;
        }
        return candidate->triangle_index < best->triangle_index;
    }
    return 0;
}

/* The enclosing sphere bounds every fixed-basis rounded-lens pose. */
static int mdkr_camera_rounded_lens_swept_aabb_may_overlap(
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 desired,
    double radius,
    MdkrCameraVec3 a_value,
    MdkrCameraVec3 b_value,
    MdkrCameraVec3 c_value,
    int *out_may_overlap) {
    const MdkrCameraDVec3 a = mdkr_camera_dvec3(a_value);
    const MdkrCameraDVec3 b = mdkr_camera_dvec3(b_value);
    const MdkrCameraDVec3 c = mdkr_camera_dvec3(c_value);
    const double path_minimum[3] = {
        fmin(start.x, desired.x) - radius,
        fmin(start.y, desired.y) - radius,
        fmin(start.z, desired.z) - radius,
    };
    const double path_maximum[3] = {
        fmax(start.x, desired.x) + radius,
        fmax(start.y, desired.y) + radius,
        fmax(start.z, desired.z) + radius,
    };
    const double triangle_minimum[3] = {
        fmin(a.x, fmin(b.x, c.x)), fmin(a.y, fmin(b.y, c.y)), fmin(a.z, fmin(b.z, c.z)),
    };
    const double triangle_maximum[3] = {
        fmax(a.x, fmax(b.x, c.x)), fmax(a.y, fmax(b.y, c.y)), fmax(a.z, fmax(b.z, c.z)),
    };
    size_t axis;

    if (out_may_overlap == NULL || !isfinite(radius) || radius < 0.0 ||
        !mdkr_camera_dvec3_finite(start) || !mdkr_camera_dvec3_finite(desired) ||
        !mdkr_camera_dvec3_finite(a) || !mdkr_camera_dvec3_finite(b) ||
        !mdkr_camera_dvec3_finite(c)) {
        return 0;
    }
    for (axis = 0U; axis < 3U; axis++) {
        if (!isfinite(path_minimum[axis]) || !isfinite(path_maximum[axis]) ||
            !isfinite(triangle_minimum[axis]) || !isfinite(triangle_maximum[axis])) {
            return 0;
        }
        if (path_maximum[axis] < triangle_minimum[axis] ||
            triangle_maximum[axis] < path_minimum[axis]) {
            *out_may_overlap = 0;
            return 1;
        }
    }
    *out_may_overlap = 1;
    return 1;
}

static MdkrCameraSweepStatus mdkr_camera_rounded_lens_profiled_triangle_test(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 eye,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    MdkrCameraLensNarrowHit *out_hit,
    double *out_exact_clearance,
    MdkrCameraRoundedLensSweepTelemetry *telemetry,
    uint64_t stationary_test_limit) {
    if (telemetry != NULL) {
        if (telemetry->stationary_tests >= stationary_test_limit) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        mdkr_camera_rounded_lens_telemetry_increment(&telemetry->stationary_tests);
    }
    return mdkr_camera_rounded_lens_guard_triangle_test_d(
        guard, eye, a, b, c, out_hit, out_exact_clearance);
}

typedef struct MdkrCameraLensClearInterval {
    double low_fraction;
    double high_fraction;
    double low_clearance;
    double high_clearance;
} MdkrCameraLensClearInterval;

/*
 * Prove the SAT candidate interval clear, or find its first contact, with a
 * fixed work budget. Triangle-to-convex-lens clearance is 1-Lipschitz under
 * translation, so the endpoint lower bound below is conservative. Intervals
 * are visited left-first; a sampled HIT can therefore be refined without
 * skipping an earlier unresolved contact. Exhaustion is INVALID, never CLEAR.
 */
static MdkrCameraSweepStatus mdkr_camera_rounded_lens_bounded_interval_sweep(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    double travel_distance,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    double enter_fraction,
    double exit_fraction,
    double enter_clearance,
    double clearance_uncertainty,
    double *out_fraction,
    MdkrCameraLensNarrowHit *out_narrow_hit,
    uint8_t *out_started_overlapping,
    MdkrCameraRoundedLensSweepTelemetry *telemetry,
    uint64_t stationary_test_limit) {
    MdkrCameraLensClearInterval intervals[MDKR_CAMERA_LENS_BOUNDED_INTERVAL_TESTS];
    MdkrCameraLensNarrowHit exit_hit;
    MdkrCameraSweepStatus status;
    double exit_clearance;
    size_t interval_count = 0U;
    unsigned int test_count = 0U;

    if (!isfinite(enter_fraction) || !isfinite(exit_fraction) ||
        !isfinite(enter_clearance) || enter_clearance <= 0.0 ||
        enter_fraction < 0.0 || exit_fraction > 1.0 ||
        enter_fraction > exit_fraction || !isfinite(clearance_uncertainty) ||
        clearance_uncertainty <= 0.0) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (exit_fraction <= enter_fraction) {
        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &telemetry->bounded_interval_exhaustions);
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (telemetry != NULL) {
        mdkr_camera_rounded_lens_telemetry_increment(&telemetry->bounded_interval_tests);
    }
    test_count++;
    status = mdkr_camera_rounded_lens_profiled_triangle_test(
        guard, mdkr_camera_position_at(start, delta, exit_fraction), a, b, c,
        &exit_hit, &exit_clearance, telemetry, stationary_test_limit);
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }
    if (status == MDKR_CAMERA_SWEEP_HIT) {
        double low = enter_fraction;
        double high = exit_fraction;
        MdkrCameraLensNarrowHit high_hit = exit_hit;
        unsigned int bisection;

        for (bisection = 0U; bisection < MDKR_CAMERA_LENS_BISECTION_ITERATIONS;
             bisection++) {
            const double middle = (low + high) * 0.5;
            MdkrCameraLensNarrowHit middle_hit;
            if (!isfinite(middle) || middle <= low || middle >= high) {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(
                    &telemetry->contact_refinement_tests);
            }
            status = mdkr_camera_rounded_lens_profiled_triangle_test(
                guard, mdkr_camera_position_at(start, delta, middle), a, b, c,
                &middle_hit, NULL, telemetry, stationary_test_limit);
            if (status == MDKR_CAMERA_SWEEP_INVALID) {
                return status;
            }
            if (status == MDKR_CAMERA_SWEEP_HIT) {
                high = middle;
                high_hit = middle_hit;
            } else if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                low = middle;
            } else {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
        }
        *out_fraction = high;
        *out_narrow_hit = high_hit;
        *out_started_overlapping = 0U;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    if (status != MDKR_CAMERA_SWEEP_CLEAR || !isfinite(exit_clearance) ||
        exit_clearance <= 0.0) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    intervals[interval_count++] = (MdkrCameraLensClearInterval){
        enter_fraction, exit_fraction, enter_clearance, exit_clearance,
    };
    while (interval_count > 0U) {
        const MdkrCameraLensClearInterval interval = intervals[--interval_count];
        const double width = interval.high_fraction - interval.low_fraction;
        const double lower_bound = 0.5 * (interval.low_clearance +
            interval.high_clearance - travel_distance * width);
        double middle;
        MdkrCameraLensNarrowHit middle_hit;
        double middle_clearance;

        if (!isfinite(width) || width <= 0.0 || !isfinite(lower_bound)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (lower_bound > clearance_uncertainty) {
            continue;
        }
        if (test_count >= MDKR_CAMERA_LENS_BOUNDED_INTERVAL_TESTS ||
            interval_count + 2U > MDKR_CAMERA_LENS_BOUNDED_INTERVAL_TESTS) {
            if (telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(
                    &telemetry->bounded_interval_exhaustions);
            }
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        middle = (interval.low_fraction + interval.high_fraction) * 0.5;
        if (!isfinite(middle) || middle <= interval.low_fraction ||
            middle >= interval.high_fraction) {
            if (telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(
                    &telemetry->bounded_interval_exhaustions);
            }
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &telemetry->bounded_interval_tests);
        }
        test_count++;
        status = mdkr_camera_rounded_lens_profiled_triangle_test(
            guard, mdkr_camera_position_at(start, delta, middle), a, b, c,
            &middle_hit, &middle_clearance, telemetry, stationary_test_limit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return status;
        }
        if (status == MDKR_CAMERA_SWEEP_HIT) {
            double low = interval.low_fraction;
            double high = middle;
            MdkrCameraLensNarrowHit high_hit = middle_hit;
            unsigned int bisection;

            for (bisection = 0U; bisection < MDKR_CAMERA_LENS_BISECTION_ITERATIONS;
                 bisection++) {
                const double refined = (low + high) * 0.5;
                MdkrCameraLensNarrowHit refined_hit;
                if (!isfinite(refined) || refined <= low || refined >= high) {
                    return MDKR_CAMERA_SWEEP_INVALID;
                }
                if (telemetry != NULL) {
                    mdkr_camera_rounded_lens_telemetry_increment(
                        &telemetry->contact_refinement_tests);
                }
                status = mdkr_camera_rounded_lens_profiled_triangle_test(
                    guard, mdkr_camera_position_at(start, delta, refined), a, b, c,
                    &refined_hit, NULL, telemetry, stationary_test_limit);
                if (status == MDKR_CAMERA_SWEEP_INVALID) {
                    return status;
                }
                if (status == MDKR_CAMERA_SWEEP_HIT) {
                    high = refined;
                    high_hit = refined_hit;
                } else if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                    low = refined;
                } else {
                    return MDKR_CAMERA_SWEEP_INVALID;
                }
            }
            *out_fraction = high;
            *out_narrow_hit = high_hit;
            *out_started_overlapping = 0U;
            return MDKR_CAMERA_SWEEP_HIT;
        }
        if (status != MDKR_CAMERA_SWEEP_CLEAR || !isfinite(middle_clearance) ||
            middle_clearance <= 0.0) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        /* LIFO: push the later half first so the earlier half is proved first. */
        intervals[interval_count++] = (MdkrCameraLensClearInterval){
            middle, interval.high_fraction, middle_clearance, interval.high_clearance,
        };
        intervals[interval_count++] = (MdkrCameraLensClearInterval){
            interval.low_fraction, middle, interval.low_clearance, middle_clearance,
        };
    }
    return MDKR_CAMERA_SWEEP_CLEAR;
}

/*
 * Conservative advancement is safe because triangle-to-convex-lens distance is
 * 1-Lipschitz under translation. A sampled HIT brackets the first contact and
 * is refined deterministically; an exhausted advance budget is INVALID rather
 * than a quietly missed grazing contact.
 */
static MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_triangle(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraDVec3 start,
    MdkrCameraDVec3 delta,
    double travel_distance,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    double *out_fraction,
    MdkrCameraLensNarrowHit *out_narrow_hit,
    uint8_t *out_started_overlapping,
    MdkrCameraRoundedLensSweepTelemetry *telemetry,
    uint64_t stationary_test_limit,
    int analytic_enabled) {
    MdkrCameraLensNarrowHit current_hit;
    MdkrCameraSweepStatus status;
    double current_fraction = 0.0;
    double current_clearance;
    double clearance_uncertainty;
    double analytic_fraction;
    double analytic_exit_fraction;
    int analytic_intersects;
    unsigned int iteration;

    if (out_fraction == NULL || out_narrow_hit == NULL || out_started_overlapping == NULL ||
        !isfinite(travel_distance) || travel_distance < 0.0) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    clearance_uncertainty = MDKR_CAMERA_LENS_CLEARANCE_UNCERTAINTY_RATIO *
                            fmax(1.0, fmax((double)guard->broadphase_radius, travel_distance));
    if (!isfinite(clearance_uncertainty) || clearance_uncertainty <= 0.0) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (analytic_enabled) {
        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &telemetry->analytic_swept_sat_tests);
        }
        if (!mdkr_camera_rounded_lens_swept_sat_interval(
                guard, start, delta, a, b, c, &analytic_intersects,
                &analytic_fraction, &analytic_exit_fraction)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (!analytic_intersects) {
            return MDKR_CAMERA_SWEEP_CLEAR;
        }
        status = mdkr_camera_rounded_lens_profiled_triangle_test(
            guard, mdkr_camera_position_at(start, delta, analytic_fraction),
            a, b, c, &current_hit, &current_clearance, telemetry,
            stationary_test_limit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return status;
        }
        if (status == MDKR_CAMERA_SWEEP_HIT) {
            *out_fraction = analytic_fraction;
            *out_narrow_hit = current_hit;
            *out_started_overlapping =
                analytic_fraction == 0.0 ? current_hit.overlapping : 0U;
            return status;
        }
        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &telemetry->analytic_revalidation_misses);
        }
        if (travel_distance <= DBL_MIN) {
            return MDKR_CAMERA_SWEEP_CLEAR;
        }
        return mdkr_camera_rounded_lens_bounded_interval_sweep(
            guard, start, delta, travel_distance, a, b, c, analytic_fraction,
            analytic_exit_fraction, current_clearance, clearance_uncertainty,
            out_fraction, out_narrow_hit, out_started_overlapping, telemetry,
            stationary_test_limit);
    }
    status = mdkr_camera_rounded_lens_profiled_triangle_test(
        guard, start, a, b, c, &current_hit, &current_clearance, telemetry,
        stationary_test_limit);
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (status == MDKR_CAMERA_SWEEP_HIT) {
        *out_fraction = 0.0;
        *out_narrow_hit = current_hit;
        *out_started_overlapping = current_hit.overlapping;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    if (status != MDKR_CAMERA_SWEEP_CLEAR || !isfinite(current_clearance) ||
        current_clearance <= 0.0) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (travel_distance <= DBL_MIN) {
        return MDKR_CAMERA_SWEEP_CLEAR;
    }

    for (iteration = 0U; iteration < MDKR_CAMERA_LENS_ADVANCE_ITERATIONS; iteration++) {
        const double remaining = 1.0 - current_fraction;
        const double advance = current_clearance / travel_distance;
        double next_fraction;
        MdkrCameraLensNarrowHit next_hit;
        double next_clearance;

        if (!isfinite(remaining) || remaining < 0.0 || !isfinite(advance) || advance <= 0.0) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        next_fraction = advance >= remaining ? 1.0 : current_fraction + advance;
        if (!isfinite(next_fraction) || next_fraction <= current_fraction) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &telemetry->conservative_advance_iterations);
        }
        status = mdkr_camera_rounded_lens_profiled_triangle_test(
            guard, mdkr_camera_position_at(start, delta, next_fraction), a, b, c, &next_hit,
            &next_clearance, telemetry, stationary_test_limit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_HIT) {
            double low = current_fraction;
            double high = next_fraction;
            MdkrCameraLensNarrowHit high_hit = next_hit;
            unsigned int bisection;

            for (bisection = 0U; bisection < MDKR_CAMERA_LENS_BISECTION_ITERATIONS; bisection++) {
                const double middle = (low + high) * 0.5;
                MdkrCameraLensNarrowHit middle_hit;
                if (!isfinite(middle) || middle <= low || middle >= high ||
                    !mdkr_camera_dvec3_finite(mdkr_camera_position_at(start, delta, middle))) {
                    return MDKR_CAMERA_SWEEP_INVALID;
                }
                if (telemetry != NULL) {
                    mdkr_camera_rounded_lens_telemetry_increment(
                        &telemetry->contact_refinement_tests);
                }
                status = mdkr_camera_rounded_lens_profiled_triangle_test(
                    guard, mdkr_camera_position_at(start, delta, middle), a, b, c, &middle_hit,
                    NULL, telemetry, stationary_test_limit);
                if (status == MDKR_CAMERA_SWEEP_INVALID) {
                    return MDKR_CAMERA_SWEEP_INVALID;
                }
                if (status == MDKR_CAMERA_SWEEP_HIT) {
                    high = middle;
                    high_hit = middle_hit;
                } else if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                    low = middle;
                } else {
                    return MDKR_CAMERA_SWEEP_INVALID;
                }
            }
            *out_fraction = high;
            *out_narrow_hit = high_hit;
            *out_started_overlapping = 0U;
            return MDKR_CAMERA_SWEEP_HIT;
        }
        if (status != MDKR_CAMERA_SWEEP_CLEAR || !isfinite(next_clearance) ||
            next_clearance <= 0.0) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (next_clearance <= clearance_uncertainty ||
            current_clearance <= clearance_uncertainty) {
            break;
        }
        if (next_fraction >= 1.0) {
            return MDKR_CAMERA_SWEEP_CLEAR;
        }
        current_fraction = next_fraction;
        current_hit = next_hit;
        current_clearance = next_clearance;
    }
    {
        MdkrCameraLensNarrowHit previous_hit;
        MdkrCameraSweepStatus previous_status;
        double previous_clearance;
        int saw_ambiguous_interval = 0;
        unsigned int subdivision;

        if (telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(&telemetry->interval_fallbacks);
        }
        previous_status = mdkr_camera_rounded_lens_profiled_triangle_test(
            guard, start, a, b, c, &previous_hit, &previous_clearance, telemetry,
            stationary_test_limit);
        if (previous_status == MDKR_CAMERA_SWEEP_INVALID ||
            (previous_status == MDKR_CAMERA_SWEEP_CLEAR &&
             (!isfinite(previous_clearance) || previous_clearance <= 0.0))) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (previous_status == MDKR_CAMERA_SWEEP_HIT) {
            *out_fraction = 0.0;
            *out_narrow_hit = previous_hit;
            *out_started_overlapping = previous_hit.overlapping;
            return MDKR_CAMERA_SWEEP_HIT;
        }
        for (subdivision = 1U; subdivision <= MDKR_CAMERA_LENS_INTERVAL_SUBDIVISIONS;
             subdivision++) {
            const double fraction = (double)subdivision /
                                    (double)MDKR_CAMERA_LENS_INTERVAL_SUBDIVISIONS;
            const double interval = 1.0 / (double)MDKR_CAMERA_LENS_INTERVAL_SUBDIVISIONS;
            MdkrCameraLensNarrowHit sample_hit;
            MdkrCameraSweepStatus sample_status;
            double sample_clearance;

            if (telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(&telemetry->interval_samples);
            }
            sample_status = mdkr_camera_rounded_lens_profiled_triangle_test(
                guard, mdkr_camera_position_at(start, delta, fraction), a, b, c, &sample_hit,
                &sample_clearance, telemetry, stationary_test_limit);
            if (sample_status == MDKR_CAMERA_SWEEP_INVALID) {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (sample_status == MDKR_CAMERA_SWEEP_HIT) {
                double low = (double)(subdivision - 1U) /
                             (double)MDKR_CAMERA_LENS_INTERVAL_SUBDIVISIONS;
                double high = fraction;
                MdkrCameraLensNarrowHit high_hit = sample_hit;
                unsigned int bisection;

                for (bisection = 0U; bisection < MDKR_CAMERA_LENS_BISECTION_ITERATIONS;
                     bisection++) {
                    const double middle = (low + high) * 0.5;
                    MdkrCameraLensNarrowHit middle_hit;
                    MdkrCameraSweepStatus middle_status;
                    if (!isfinite(middle) || middle <= low || middle >= high) {
                        return MDKR_CAMERA_SWEEP_INVALID;
                    }
                    if (telemetry != NULL) {
                        mdkr_camera_rounded_lens_telemetry_increment(
                            &telemetry->contact_refinement_tests);
                    }
                    middle_status = mdkr_camera_rounded_lens_profiled_triangle_test(
                        guard, mdkr_camera_position_at(start, delta, middle), a, b, c,
                        &middle_hit, NULL, telemetry, stationary_test_limit);
                    if (middle_status == MDKR_CAMERA_SWEEP_INVALID) {
                        return MDKR_CAMERA_SWEEP_INVALID;
                    }
                    if (middle_status == MDKR_CAMERA_SWEEP_HIT) {
                        high = middle;
                        high_hit = middle_hit;
                    } else {
                        low = middle;
                    }
                }
                *out_fraction = high;
                *out_narrow_hit = high_hit;
                *out_started_overlapping = 0U;
                return MDKR_CAMERA_SWEEP_HIT;
            }
            if (sample_status != MDKR_CAMERA_SWEEP_CLEAR || !isfinite(sample_clearance) ||
                sample_clearance <= 0.0) {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (0.5 * (previous_clearance + sample_clearance - travel_distance * interval) <= 0.0) {
                saw_ambiguous_interval = 1;
                if (telemetry != NULL) {
                    mdkr_camera_rounded_lens_telemetry_increment(
                        &telemetry->ambiguous_intervals);
                }
            }
            previous_clearance = sample_clearance;
        }
        if (saw_ambiguous_interval) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
    }
    return MDKR_CAMERA_SWEEP_CLEAR;
}

static MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_impl(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry,
    uint64_t stationary_test_limit,
    int analytic_enabled) {
    MdkrCameraDVec3 start;
    MdkrCameraDVec3 desired;
    MdkrCameraDVec3 delta;
    double travel_distance_squared;
    double travel_distance;
    double outward_radius;
    MdkrCameraRoundedLensSweepCandidate best;
    size_t triangle_index;

    if (out_telemetry != NULL) {
        memset(out_telemetry, 0, sizeof(*out_telemetry));
    }
    if (out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    mdkr_camera_set_invalid_hit(out_hit);
    if (world == NULL || input == NULL || !mdkr_camera_rounded_lens_guard_valid(&input->guard) ||
        !mdkr_camera_vec3_finite(input->start_eye) || !mdkr_camera_vec3_finite(input->desired_eye) ||
        world->triangle_count > SIZE_MAX / 3U ||
        (world->triangle_count != 0U &&
         (world->vertices == NULL || world->indices == NULL || world->triangles == NULL))) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    start = mdkr_camera_dvec3(input->start_eye);
    desired = mdkr_camera_dvec3(input->desired_eye);
    delta = mdkr_camera_sub(desired, start);
    travel_distance_squared = mdkr_camera_length_squared(delta);
    if (!mdkr_camera_dvec3_finite(delta) || !isfinite(travel_distance_squared)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    travel_distance = sqrt(travel_distance_squared);
    if (!isfinite(travel_distance)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (!mdkr_camera_rounded_lens_guard_conservative_radius(&input->guard, &outward_radius)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(&best, 0, sizeof(best));

    for (triangle_index = 0U; triangle_index < world->triangle_count; triangle_index++) {
        const size_t index_offset = triangle_index * 3U;
        const MdkrCameraOcclusionTriangle *metadata = &world->triangles[triangle_index];
        MdkrCameraVec3 a;
        MdkrCameraVec3 b;
        MdkrCameraVec3 c;
        MdkrCameraDVec3 triangle_normal;
        MdkrCameraRoundedLensSweepCandidate candidate;
        MdkrCameraSweepStatus status;
        uint8_t candidate_started_overlapping;
        int may_overlap;

        if (out_telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &out_telemetry->triangles_seen);
        }

        if (world->indices[index_offset] >= world->vertex_count ||
            world->indices[index_offset + 1U] >= world->vertex_count ||
            world->indices[index_offset + 2U] >= world->vertex_count) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        a = world->vertices[world->indices[index_offset]];
        b = world->vertices[world->indices[index_offset + 1U]];
        c = world->vertices[world->indices[index_offset + 2U]];
        if (!mdkr_camera_vec3_finite(a) || !mdkr_camera_vec3_finite(b) ||
            !mdkr_camera_vec3_finite(c) ||
            mdkr_camera_triangle_is_degenerate(mdkr_camera_dvec3(a), mdkr_camera_dvec3(b),
                                               mdkr_camera_dvec3(c), &triangle_normal)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if ((input->mask != 0U && (metadata->mask & input->mask) == 0U) ||
            (input->ignored_object_generation != 0U &&
             metadata->object_generation == input->ignored_object_generation)) {
            if (out_telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(
                    &out_telemetry->triangles_filtered);
            }
            continue;
        }
        if (!mdkr_camera_rounded_lens_swept_aabb_may_overlap(
                start, desired, outward_radius, a, b, c, &may_overlap)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (!may_overlap) {
            if (out_telemetry != NULL) {
                mdkr_camera_rounded_lens_telemetry_increment(
                    &out_telemetry->triangles_aabb_rejected);
            }
            continue;
        }
        if (out_telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &out_telemetry->triangles_narrowed);
        }
        memset(&candidate, 0, sizeof(candidate));
        status = mdkr_camera_rounded_lens_sweep_triangle(
            &input->guard, start, delta, travel_distance, a, b, c, &candidate.fraction,
            &candidate.narrow_hit, &candidate_started_overlapping, out_telemetry,
            stationary_test_limit, analytic_enabled);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            continue;
        }
        if (status != MDKR_CAMERA_SWEEP_HIT || !isfinite(candidate.fraction) ||
            candidate.fraction < 0.0 || candidate.fraction > 1.0 ||
            candidate_started_overlapping > 1U ||
            !isfinite(candidate.narrow_hit.clearance) ||
            !isfinite(candidate.narrow_hit.penetration_depth) ||
            !mdkr_camera_vec3_finite(candidate.narrow_hit.point) ||
            !mdkr_camera_vec3_finite(candidate.narrow_hit.normal)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        candidate.stable_id = metadata->stable_id;
        candidate.kind = metadata->kind;
        candidate.triangle_index = triangle_index;
        candidate.started_overlapping = candidate_started_overlapping;
        candidate.valid = 1;
        if (mdkr_camera_rounded_lens_candidate_better(&candidate, &best)) {
            best = candidate;
        }
    }
    if (!best.valid) {
        mdkr_camera_set_clear_hit(out_hit);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    {
        float published_fraction = (float)best.fraction;
        const size_t index_offset = best.triangle_index * 3U;
        MdkrCameraLensNarrowHit published_hit;
        MdkrCameraSweepStatus published_status;

        if ((double)published_fraction < best.fraction) {
            published_fraction = nextafterf(published_fraction, INFINITY);
        }
        if (!isfinite(published_fraction) || published_fraction < 0.0f || published_fraction > 1.0f) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (out_telemetry != NULL) {
            mdkr_camera_rounded_lens_telemetry_increment(
                &out_telemetry->publication_revalidations);
        }
        published_status = mdkr_camera_rounded_lens_profiled_triangle_test(
            &input->guard, mdkr_camera_position_at(start, delta, published_fraction),
            world->vertices[world->indices[index_offset]],
            world->vertices[world->indices[index_offset + 1U]],
            world->vertices[world->indices[index_offset + 2U]], &published_hit, NULL,
            out_telemetry, stationary_test_limit);
        if (published_status != MDKR_CAMERA_SWEEP_HIT) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        best.narrow_hit = published_hit;
        out_hit->fraction = published_fraction;
    }
    out_hit->clearance = best.started_overlapping ? best.narrow_hit.clearance : 0.0f;
    out_hit->penetration_depth = best.started_overlapping ? best.narrow_hit.penetration_depth : 0.0f;
    out_hit->point = best.narrow_hit.point;
    out_hit->normal = best.narrow_hit.normal;
    out_hit->kind = best.kind;
    out_hit->stable_id = best.stable_id;
    out_hit->feature = best.narrow_hit.feature;
    out_hit->started_overlapping = best.started_overlapping;
    return MDKR_CAMERA_SWEEP_HIT;
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_profiled(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry) {
    return mdkr_camera_rounded_lens_sweep_impl(
        world, input, out_hit, out_telemetry, UINT64_MAX, 1);
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_profiled_limited(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry,
    uint64_t stationary_test_limit) {
    if (out_telemetry == NULL || stationary_test_limit == 0U) {
        if (out_hit != NULL) mdkr_camera_set_invalid_hit(out_hit);
        if (out_telemetry != NULL) memset(out_telemetry, 0, sizeof(*out_telemetry));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    return mdkr_camera_rounded_lens_sweep_impl(
        world, input, out_hit, out_telemetry, stationary_test_limit, 1);
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_reference(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry) {
    return mdkr_camera_rounded_lens_sweep_impl(
        world, input, out_hit, out_telemetry, UINT64_MAX, 0);
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    return mdkr_camera_rounded_lens_sweep_profiled(world, input, out_hit, NULL);
}

MdkrCameraSweepStatus mdkr_camera_sweep(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraDVec3 start;
    MdkrCameraDVec3 desired;
    MdkrCameraDVec3 delta;
    MdkrCameraCandidate best;
    const double radius = input != NULL ? input->guard.radius : 0.0;
    size_t triangle_index;

    if (out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    mdkr_camera_set_invalid_hit(out_hit);
    if (world == NULL || input == NULL ||
        input->guard.kind != MDKR_CAMERA_LENS_GUARD_SPHERE || !isfinite(radius) || radius < 0.0 ||
        !mdkr_camera_vec3_finite(input->start_eye) || !mdkr_camera_vec3_finite(input->desired_eye) ||
        /* Validate the indexed-triangle representation before any offset arithmetic. */
        world->triangle_count > SIZE_MAX / 3U ||
        (world->triangle_count != 0 &&
         (world->vertices == NULL || world->indices == NULL || world->triangles == NULL))) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    start = mdkr_camera_dvec3(input->start_eye);
    desired = mdkr_camera_dvec3(input->desired_eye);
    delta = mdkr_camera_sub(desired, start);
    if (!mdkr_camera_dvec3_finite(delta)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(&best, 0, sizeof(best));

    for (triangle_index = 0; triangle_index < world->triangle_count; triangle_index++) {
        const size_t index_offset = triangle_index * 3U;
        const MdkrCameraOcclusionTriangle *metadata = &world->triangles[triangle_index];
        MdkrCameraDVec3 a;
        MdkrCameraDVec3 b;
        MdkrCameraDVec3 c;
        MdkrCameraDVec3 triangle_normal;
        MdkrCameraDVec3 start_point;
        MdkrCameraDVec3 nearest_point;
        MdkrCameraDVec3 start_offset;
        MdkrCameraSweepFeature start_feature;
        double start_distance_squared;
        double start_distance;

        if (world->indices[index_offset] >= world->vertex_count ||
            world->indices[index_offset + 1U] >= world->vertex_count ||
            world->indices[index_offset + 2U] >= world->vertex_count) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        a = mdkr_camera_dvec3(world->vertices[world->indices[index_offset]]);
        b = mdkr_camera_dvec3(world->vertices[world->indices[index_offset + 1U]]);
        c = mdkr_camera_dvec3(world->vertices[world->indices[index_offset + 2U]]);
        if (!mdkr_camera_dvec3_finite(a) || !mdkr_camera_dvec3_finite(b) ||
            !mdkr_camera_dvec3_finite(c) ||
            mdkr_camera_triangle_is_degenerate(a, b, c, &triangle_normal)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if ((input->mask != 0U && (metadata->mask & input->mask) == 0U) ||
            (input->ignored_object_generation != 0U &&
             metadata->object_generation == input->ignored_object_generation)) {
            continue;
        }

        start_point = start;
        nearest_point = mdkr_camera_closest_point_triangle(start_point, a, b, c, &start_feature);
        start_offset = mdkr_camera_sub(start_point, nearest_point);
        start_distance_squared = mdkr_camera_length_squared(start_offset);
        if (!isfinite(start_distance_squared)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        start_distance = sqrt(fmax(0.0, start_distance_squared));
        /* A stationary lens touching a sheet is still a contact, not a clear path. */
        if (start_distance <= radius) {
            MdkrCameraCandidate overlap;
            memset(&overlap, 0, sizeof(overlap));
            overlap.fraction = 0.0;
            overlap.point = nearest_point;
            if (!mdkr_camera_normalize(start_offset, &overlap.normal)) {
                overlap.normal = mdkr_camera_fallback_normal(triangle_normal, delta);
            }
            overlap.stable_id = metadata->stable_id;
            overlap.kind = metadata->kind;
            overlap.triangle_index = triangle_index;
            overlap.feature = start_feature;
            overlap.valid = 1;
            if (mdkr_camera_candidate_better(&overlap, &best)) {
                best = overlap;
            }
            continue;
        }

        mdkr_camera_consider_face_sweep(&best, start, delta, a, b, c, triangle_normal,
                                        radius, metadata->stable_id, metadata->kind, triangle_index);
        mdkr_camera_consider_edge_sweep(&best, a, b, start, delta, a, b, c, triangle_normal,
                                        radius, metadata->stable_id, metadata->kind, triangle_index);
        mdkr_camera_consider_edge_sweep(&best, b, c, start, delta, a, b, c, triangle_normal,
                                        radius, metadata->stable_id, metadata->kind, triangle_index);
        mdkr_camera_consider_edge_sweep(&best, c, a, start, delta, a, b, c, triangle_normal,
                                        radius, metadata->stable_id, metadata->kind, triangle_index);
    }

    if (!best.valid) {
        mdkr_camera_set_clear_hit(out_hit);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    out_hit->fraction = (float)best.fraction;
    out_hit->clearance = 0.0f;
    out_hit->penetration_depth = 0.0f;
    out_hit->point = mdkr_camera_vec3(best.point);
    out_hit->normal = mdkr_camera_vec3(best.normal);
    out_hit->kind = best.kind;
    out_hit->stable_id = best.stable_id;
    out_hit->feature = best.feature;
    out_hit->started_overlapping = 0;

    /* Re-evaluate t=0 for the selected triangle to publish overlap depth. */
    {
        const size_t offset = best.triangle_index * 3U;
        const MdkrCameraDVec3 a = mdkr_camera_dvec3(world->vertices[world->indices[offset]]);
        const MdkrCameraDVec3 b = mdkr_camera_dvec3(world->vertices[world->indices[offset + 1U]]);
        const MdkrCameraDVec3 c = mdkr_camera_dvec3(world->vertices[world->indices[offset + 2U]]);
        MdkrCameraSweepFeature feature;
        const MdkrCameraDVec3 point = mdkr_camera_closest_point_triangle(start, a, b, c, &feature);
        const double distance = sqrt(fmax(0.0, mdkr_camera_length_squared(mdkr_camera_sub(start, point))));
        if (best.fraction == 0.0 && distance < radius) {
            out_hit->started_overlapping = 1;
            out_hit->clearance = (float)(distance - radius);
            out_hit->penetration_depth = (float)(radius - distance);
        }
    }
    return MDKR_CAMERA_SWEEP_HIT;
}
