#include "camera_obstruction_query.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void mdkr_camera_obstruction_query_set_invalid(MdkrCameraSweepHit *out_hit) {
    memset(out_hit, 0, sizeof(*out_hit));
}

static void mdkr_camera_obstruction_query_set_clear(MdkrCameraSweepHit *out_hit) {
    memset(out_hit, 0, sizeof(*out_hit));
    out_hit->fraction = 1.0f;
    out_hit->clearance = INFINITY;
}

static int mdkr_camera_obstruction_query_hit_valid(const MdkrCameraSweepHit *hit) {
    const double normal_length_squared =
        (double)hit->normal.x * hit->normal.x +
        (double)hit->normal.y * hit->normal.y +
        (double)hit->normal.z * hit->normal.z;

    return isfinite(hit->fraction) && hit->fraction >= 0.0f && hit->fraction <= 1.0f &&
           isfinite(hit->clearance) && isfinite(hit->penetration_depth) &&
           hit->penetration_depth >= 0.0f &&
           isfinite(hit->point.x) && isfinite(hit->point.y) && isfinite(hit->point.z) &&
           isfinite(hit->normal.x) && isfinite(hit->normal.y) && isfinite(hit->normal.z) &&
           isfinite(normal_length_squared) && normal_length_squared > 0.0 &&
           hit->feature >= MDKR_CAMERA_SWEEP_FEATURE_FACE &&
           hit->feature <= MDKR_CAMERA_SWEEP_FEATURE_VERTEX &&
           hit->started_overlapping <= 1U;
}

static int mdkr_camera_obstruction_query_fraction_earlier(
    const MdkrCameraSweepHit *candidate,
    const MdkrCameraSweepHit *best) {
    return (double)candidate->fraction < (double)best->fraction -
        MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON;
}

static int mdkr_camera_obstruction_query_fraction_tied(
    const MdkrCameraSweepHit *candidate,
    const MdkrCameraSweepHit *best) {
    return fabs((double)candidate->fraction - (double)best->fraction) <=
        MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON;
}

MdkrCameraSweepStatus mdkr_camera_obstruction_combined_sweep(
    const MdkrCameraObstructionCombinedQuery *query,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepHit best;
    int have_best = 0;
    size_t source_index;

    if (out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    mdkr_camera_obstruction_query_set_invalid(out_hit);
    if (query == NULL || input == NULL || query->source_count > query->source_capacity ||
        query->source_capacity > SIZE_MAX / sizeof(*query->sources) ||
        (query->source_count != 0U && query->sources == NULL)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }

    for (source_index = 0U; source_index < query->source_count; source_index++) {
        const MdkrCameraObstructionQuerySource *source = &query->sources[source_index];
        MdkrCameraSweepHit hit;
        MdkrCameraSweepStatus status;

        if (source->sweep == NULL) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        memset(&hit, 0, sizeof(hit));
        status = source->sweep(source->context, input, &hit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            continue;
        }
        if (status != MDKR_CAMERA_SWEEP_HIT || !mdkr_camera_obstruction_query_hit_valid(&hit)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (!have_best || mdkr_camera_obstruction_query_fraction_earlier(&hit, &best)) {
            best = hit;
            have_best = 1;
            continue;
        }
        if (mdkr_camera_obstruction_query_fraction_tied(&hit, &best)) {
            if (hit.stable_id == best.stable_id) {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (hit.stable_id < best.stable_id) {
                best = hit;
            }
        }
    }

    if (!have_best) {
        mdkr_camera_obstruction_query_set_clear(out_hit);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = best;
    return MDKR_CAMERA_SWEEP_HIT;
}

MdkrCameraSweepStatus mdkr_camera_obstruction_combined_sweep_adapter(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    return mdkr_camera_obstruction_combined_sweep(
        (const MdkrCameraObstructionCombinedQuery *)context, input, out_hit);
}

MdkrCameraSweepStatus mdkr_camera_obstruction_combined_rounded_lens_sweep(
    const MdkrCameraObstructionRoundedLensCombinedQuery *query,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepHit best;
    int have_best = 0;
    size_t source_index;

    if (out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    mdkr_camera_obstruction_query_set_invalid(out_hit);
    if (query == NULL || input == NULL || query->source_count > query->source_capacity ||
        query->source_capacity > SIZE_MAX / sizeof(*query->sources) ||
        (query->source_count != 0U && query->sources == NULL)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }

    for (source_index = 0U; source_index < query->source_count; source_index++) {
        const MdkrCameraObstructionRoundedLensQuerySource *source = &query->sources[source_index];
        MdkrCameraSweepHit hit;
        MdkrCameraSweepStatus status;

        if (source->sweep == NULL) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        memset(&hit, 0, sizeof(hit));
        status = source->sweep(source->context, input, &hit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            continue;
        }
        if (status != MDKR_CAMERA_SWEEP_HIT || !mdkr_camera_obstruction_query_hit_valid(&hit)) {
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (!have_best || mdkr_camera_obstruction_query_fraction_earlier(&hit, &best)) {
            best = hit;
            have_best = 1;
            continue;
        }
        if (mdkr_camera_obstruction_query_fraction_tied(&hit, &best)) {
            if (hit.stable_id == best.stable_id) {
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (hit.stable_id < best.stable_id) {
                best = hit;
            }
        }
    }

    if (!have_best) {
        mdkr_camera_obstruction_query_set_clear(out_hit);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = best;
    return MDKR_CAMERA_SWEEP_HIT;
}

MdkrCameraSweepStatus mdkr_camera_obstruction_combined_rounded_lens_sweep_adapter(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    return mdkr_camera_obstruction_combined_rounded_lens_sweep(
        (const MdkrCameraObstructionRoundedLensCombinedQuery *)context, input, out_hit);
}

MdkrCameraSweepStatus mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    const MdkrCameraSweepInput *sphere_input,
    const MdkrCameraRoundedLensGuard *exact_guard,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraObstructionTwoPhaseDecision *out_decision) {
    MdkrCameraSweepInput conservative_input;
    MdkrCameraRoundedLensSweepInput exact_input;
    MdkrCameraSweepHit sphere_hit;
    MdkrCameraSweepHit exact_hit;
    MdkrCameraSweepStatus status;
    double exact_radius;
    float conservative_radius;

    if (out_hit == NULL || out_decision == NULL) {
        if (out_hit != NULL) {
            mdkr_camera_obstruction_query_set_invalid(out_hit);
        }
        if (out_decision != NULL) {
            memset(out_decision, 0, sizeof(*out_decision));
            out_decision->sphere_status = MDKR_CAMERA_SWEEP_INVALID;
            out_decision->exact_status = MDKR_CAMERA_SWEEP_INVALID;
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    mdkr_camera_obstruction_query_set_invalid(out_hit);
    memset(out_decision, 0, sizeof(*out_decision));
    out_decision->sphere_status = MDKR_CAMERA_SWEEP_INVALID;
    out_decision->exact_status = MDKR_CAMERA_SWEEP_INVALID;
    if (sphere_input == NULL || exact_guard == NULL ||
        sphere_input->guard.kind != MDKR_CAMERA_LENS_GUARD_SPHERE ||
        !isfinite(sphere_input->guard.radius) || sphere_input->guard.radius < 0.0f ||
        !mdkr_camera_rounded_lens_guard_conservative_radius(exact_guard, &exact_radius) ||
        !isfinite(exact_radius) || exact_radius > FLT_MAX) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }

    conservative_radius = (float)exact_radius;
    if ((double)conservative_radius < exact_radius) {
        conservative_radius = nextafterf(conservative_radius, INFINITY);
    }
    if (!isfinite(conservative_radius)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    conservative_input = *sphere_input;
    if (conservative_input.guard.radius < conservative_radius) {
        conservative_input.guard.radius = conservative_radius;
    }
    status = mdkr_camera_obstruction_combined_sweep(
        sphere_query, &conservative_input, &sphere_hit);
    out_decision->sphere_status = status;
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        mdkr_camera_obstruction_query_set_clear(out_hit);
        out_decision->outcome = MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_SPHERE_CLEAR;
        return status;
    }
    if (status != MDKR_CAMERA_SWEEP_HIT) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }

    memset(&exact_input, 0, sizeof(exact_input));
    exact_input.guard = *exact_guard;
    exact_input.start_eye = sphere_input->start_eye;
    exact_input.desired_eye = sphere_input->desired_eye;
    exact_input.mask = sphere_input->mask;
    exact_input.ignored_object_generation = sphere_input->ignored_object_generation;
    out_decision->exact_invoked = 1U;
    status = mdkr_camera_obstruction_combined_rounded_lens_sweep(
        exact_query, &exact_input, &exact_hit);
    out_decision->exact_status = status;
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        *out_hit = sphere_hit;
        out_decision->outcome =
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK;
        out_decision->degraded = 1U;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        *out_hit = exact_hit;
        out_decision->outcome = MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR;
        return status;
    }
    if (status == MDKR_CAMERA_SWEEP_HIT) {
        *out_hit = exact_hit;
        out_decision->outcome = MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_HIT;
        return status;
    }
    return MDKR_CAMERA_SWEEP_INVALID;
}
