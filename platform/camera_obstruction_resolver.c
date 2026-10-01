#include "camera_obstruction_resolver.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define MDKR_CAMERA_RESOLVER_DEG_TO_RAD 0.01745329251994329576923690768489f

static int mdkr_camera_resolver_vec3_finite(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

int mdkr_camera_recovery_transport(MdkrCameraVec3 previous_eye,
    MdkrCameraVec3 previous_pivot, MdkrCameraVec3 previous_desired,
    MdkrCameraVec3 pivot, MdkrCameraVec3 desired, MdkrCameraVec3 *out_eye) {
    if (!out_eye || !mdkr_camera_resolver_vec3_finite(previous_eye) ||
        !mdkr_camera_resolver_vec3_finite(previous_pivot) ||
        !mdkr_camera_resolver_vec3_finite(previous_desired) ||
        !mdkr_camera_resolver_vec3_finite(pivot) ||
        !mdkr_camera_resolver_vec3_finite(desired)) return 0;
    double old_x = (double)previous_desired.x - previous_pivot.x;
    double old_z = (double)previous_desired.z - previous_pivot.z;
    double new_x = (double)desired.x - pivot.x;
    double new_z = (double)desired.z - pivot.z;
    const double old_length = hypot(old_x, old_z), new_length = hypot(new_x, new_z);
    if (!(old_length > 0) || !(new_length > 0)) return 0;
    old_x /= old_length; old_z /= old_length;
    new_x /= new_length; new_z /= new_length;
    const double dx = (double)previous_eye.x - previous_desired.x;
    const double dz = (double)previous_eye.z - previous_desired.z;
    const double lateral = dx * old_z - dz * old_x;
    const double longitudinal = dx * old_x + dz * old_z;
    const double x = desired.x + lateral * new_z + longitudinal * new_x;
    const double y = (double)desired.y + previous_eye.y - previous_desired.y;
    const double z = desired.z - lateral * new_x + longitudinal * new_z;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
        fabs(x) > FLT_MAX || fabs(y) > FLT_MAX || fabs(z) > FLT_MAX) return 0;
    *out_eye = (MdkrCameraVec3){(float)x, (float)y, (float)z};
    return 1;
}

static MdkrCameraVec3 mdkr_camera_resolver_lerp(
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    float fraction) {
    MdkrCameraVec3 result = {
        a.x + (b.x - a.x) * fraction,
        a.y + (b.y - a.y) * fraction,
        a.z + (b.z - a.z) * fraction,
    };
    return result;
}

static float mdkr_camera_resolver_distance(MdkrCameraVec3 a, MdkrCameraVec3 b) {
    const double x = (double)b.x - (double)a.x;
    const double y = (double)b.y - (double)a.y;
    const double z = (double)b.z - (double)a.z;
    const double squared = x * x + y * y + z * z;

    if (!isfinite(squared) || squared > (double)FLT_MAX * (double)FLT_MAX) {
        return INFINITY;
    }
    return (float)sqrt(squared);
}

static int mdkr_camera_resolver_config_valid(
    const MdkrCameraObstructionResolverConfig *config) {
    return config != NULL &&
           (config->policy == MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN ||
            config->policy == MDKR_CAMERA_OBSTRUCTION_POLICY_LEGACY ||
            config->policy == MDKR_CAMERA_OBSTRUCTION_POLICY_CENTER_RAY) &&
           isfinite(config->clearance_skin) && config->clearance_skin >= 0.0f &&
           isfinite(config->recovery_speed) && config->recovery_speed > 0.0f &&
           isfinite(config->max_recovery_step) && config->max_recovery_step > 0.0f &&
           isfinite(config->max_recovery_fraction) &&
           config->max_recovery_fraction >= 0.0f && config->max_recovery_fraction <= 1.0f;
}

static int mdkr_camera_resolver_input_valid(
    const MdkrCameraObstructionResolverInput *input) {
    return input != NULL && input->query.sweep != NULL && input->projection != NULL &&
           mdkr_camera_resolver_vec3_finite(input->pivot) &&
           mdkr_camera_resolver_vec3_finite(input->start_safe_eye) &&
           mdkr_camera_resolver_vec3_finite(input->desired_eye) &&
           isfinite(input->fixed_delta_seconds) && input->fixed_delta_seconds >= 0.0f &&
           isfinite(input->projection->near_plane) &&
           isfinite(input->projection->vertical_fov) &&
           isfinite(input->projection->aspect) &&
           input->projection->near_plane > 0.0f && input->projection->aspect > 0.0f &&
           input->projection->vertical_fov > 0.0f &&
           input->projection->vertical_fov < 180.0f &&
           input->projection->generation != 0U;
}

static MdkrCameraSweepStatus mdkr_camera_resolver_stationary_sweep(
    MdkrCameraObstructionQuery query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 eye,
    uint32_t mask,
    uint32_t ignored_object_generation,
    MdkrCameraSweepHit *out_hit) {
    const MdkrCameraSweepInput input = {
        .guard = guard,
        .start_eye = eye,
        .desired_eye = eye,
        .mask = mask,
        .ignored_object_generation = ignored_object_generation,
    };
    return query.sweep(query.context, &input, out_hit);
}

MdkrCameraSweepStatus mdkr_camera_occlusion_world_sweep(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    return mdkr_camera_sweep((const MdkrCameraOcclusionWorld *)context, input, out_hit);
}

static void mdkr_camera_resolver_fail(
    MdkrCameraObstructionResolverResult *out_result,
    MdkrCameraObstructionResolverStatus status,
    MdkrCameraVec3 fallback_eye,
    MdkrCameraLensGuard guard) {
    memset(out_result, 0, sizeof(*out_result));
    out_result->status = status;
    out_result->resolved_eye = fallback_eye;
    out_result->guard = guard;
}

void mdkr_camera_obstruction_resolver_reset(
    MdkrCameraObstructionResolverState *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int mdkr_camera_release_hold_step(uint8_t *retraction_latched,
                                  uint32_t *clear_run_ticks,
                                  int contact,
                                  int near_contact,
                                  float fixed_delta_seconds,
                                  uint32_t release_hold_ticks,
                                  uint32_t near_contact_extension_ticks) {
    if (retraction_latched == NULL || clear_run_ticks == NULL) {
        return 0;
    }
    if (contact) {
        /* Engage on contact, always this tick: the hold gates release only. */
        *retraction_latched = 1U;
        *clear_run_ticks = 0U;
        return 0;
    }
    if (!*retraction_latched) {
        return 1;
    }
    /* Paused/projection-only revalidation proves geometry, not an elapsed
     * authored tick. It must not spend the release hold. */
    if (fixed_delta_seconds > 0.0f && *clear_run_ticks < UINT32_MAX) {
        (*clear_run_ticks)++;
    }
    if (*clear_run_ticks >= release_hold_ticks &&
        (!near_contact ||
         *clear_run_ticks - release_hold_ticks >= near_contact_extension_ticks)) {
        *retraction_latched = 0U;
        *clear_run_ticks = 0U;
        return 1;
    }
    return 0;
}

MdkrCameraObstructionResolverStatus mdkr_camera_obstruction_resolve(
    const MdkrCameraObstructionResolverConfig *config,
    const MdkrCameraObstructionResolverInput *input,
    MdkrCameraObstructionResolverState *state,
    MdkrCameraObstructionResolverResult *out_result) {
    MdkrCameraLensGuard guard;
    MdkrCameraVec3 anchor;
    MdkrCameraVec3 candidate;
    MdkrCameraSweepHit hit;
    /* Written only when desired_blocked and read only under the same flag;
     * zero-initialized because that pairing is a data-flow invariant the
     * compiler cannot see, and GCC rejects the unproven read under -Werror. */
    MdkrCameraSweepHit obstruction_hit = {0};
    MdkrCameraSweepStatus sweep_status;
    float distance;
    int projection_changed;
    int used_last_safe = 0;
    int desired_blocked;
    int fully_recovered = 0;
    /*
     * Release hysteresis, resolved from the state at entry and committed only
     * on the accepted path below. A tick that returns INVALID or FAILSAFE
     * published no validated pose, so it must not count as a clear tick
     * against the hold: leaving the latch untouched is the conservative
     * reading, and it matches how last_safe_eye is already handled.
     */
    uint8_t retraction_latched;
    uint32_t clear_run_ticks;
    int release_held = 0;

    if (out_result == NULL) {
        return MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
    }
    memset(out_result, 0, sizeof(*out_result));
    memset(&guard, 0, sizeof(guard));
    if (state == NULL || !mdkr_camera_resolver_config_valid(config) ||
        !mdkr_camera_resolver_input_valid(input)) {
        mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID,
                                  (input != NULL) ? input->start_safe_eye : (MdkrCameraVec3){ 0 }, guard);
        return out_result->status;
    }
    if (!mdkr_camera_lens_guard_from_projection(
            input->projection->near_plane,
            input->projection->vertical_fov * MDKR_CAMERA_RESOLVER_DEG_TO_RAD,
            input->projection->aspect, 0.0f, &guard)) {
        mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID,
                                  input->start_safe_eye, guard);
        return out_result->status;
    }
    if (config->policy == MDKR_CAMERA_OBSTRUCTION_POLICY_CENTER_RAY) {
        guard.radius = 0.0f;
    }
    if (config->policy == MDKR_CAMERA_OBSTRUCTION_POLICY_LEGACY) {
        memset(out_result, 0, sizeof(*out_result));
        out_result->status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR;
        out_result->resolved_eye = input->desired_eye;
        out_result->guard = guard;
        out_result->accepted = 1;
        return out_result->status;
    }

    projection_changed = state->last_safe_valid &&
                         state->projection_generation != input->projection->generation;
    if (input->discontinuity) {
        /* A teleport must never recover from an unrelated previous camera shot. */
        state->last_safe_valid = 0;
        /* ...nor hold a retraction latched by geometry it no longer sits in. */
        state->retraction_latched = 0;
        state->clear_run_ticks = 0U;
    }
    retraction_latched = (uint8_t)(state->retraction_latched != 0);
    clear_run_ticks = state->clear_run_ticks;

    /* The caller's previous resolved eye is preferred, but always revalidated. */
    sweep_status = mdkr_camera_resolver_stationary_sweep(
        input->query, guard, input->start_safe_eye, input->mask,
        input->ignored_object_generation, &hit);
    if (sweep_status == MDKR_CAMERA_SWEEP_CLEAR) {
        anchor = input->start_safe_eye;
    } else {
        if (state->last_safe_valid && !input->discontinuity) {
            sweep_status = mdkr_camera_resolver_stationary_sweep(
                input->query, guard, state->last_safe_eye, input->mask,
                input->ignored_object_generation, &hit);
            if (sweep_status == MDKR_CAMERA_SWEEP_CLEAR) {
                anchor = state->last_safe_eye;
                used_last_safe = 1;
            }
        }
        if (sweep_status != MDKR_CAMERA_SWEEP_CLEAR) {
            /* A tight pivot is a deterministic emergency candidate, not a ray origin. */
            sweep_status = mdkr_camera_resolver_stationary_sweep(
                input->query, guard, input->pivot, input->mask,
                input->ignored_object_generation, &hit);
            if (sweep_status == MDKR_CAMERA_SWEEP_CLEAR) {
                anchor = input->pivot;
            }
        }
        if (sweep_status != MDKR_CAMERA_SWEEP_CLEAR) {
            mdkr_camera_resolver_fail(out_result,
                                      sweep_status == MDKR_CAMERA_SWEEP_INVALID ?
                                          MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID :
                                          MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE,
                                      state->last_safe_valid ? state->last_safe_eye : input->start_safe_eye,
                                      guard);
            out_result->path_was_blocked = 1;
            out_result->projection_revalidated = (uint8_t)projection_changed;
            return out_result->status;
        }
    }

    {
        const MdkrCameraSweepInput sweep_input = {
            .guard = guard,
            .start_eye = anchor,
            .desired_eye = input->desired_eye,
            .mask = input->mask,
            .ignored_object_generation = input->ignored_object_generation,
        };
        sweep_status = input->query.sweep(input->query.context, &sweep_input, &hit);
    }
    if (sweep_status == MDKR_CAMERA_SWEEP_INVALID) {
        mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID,
                                  anchor, guard);
        out_result->used_last_safe = (uint8_t)used_last_safe;
        out_result->projection_revalidated = (uint8_t)projection_changed;
        return out_result->status;
    }
    desired_blocked = sweep_status == MDKR_CAMERA_SWEEP_HIT;
    if (desired_blocked) {
        obstruction_hit = hit;
    }
    if (sweep_status == MDKR_CAMERA_SWEEP_HIT && hit.started_overlapping) {
        mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE,
                                  anchor, guard);
        out_result->path_was_blocked = 1;
        out_result->used_last_safe = (uint8_t)used_last_safe;
        out_result->projection_revalidated = (uint8_t)projection_changed;
        return out_result->status;
    }

    distance = mdkr_camera_resolver_distance(anchor, input->desired_eye);
    if (!isfinite(distance)) {
        mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID,
                                  anchor, guard);
        return out_result->status;
    }
    if (sweep_status == MDKR_CAMERA_SWEEP_HIT) {
        float safe_fraction = hit.fraction;
        if (distance > 0.0f) {
            safe_fraction -= config->clearance_skin / distance;
        }
        if (safe_fraction < 0.0f) {
            safe_fraction = 0.0f;
        }
        candidate = mdkr_camera_resolver_lerp(anchor, input->desired_eye, safe_fraction);
        (void)mdkr_camera_release_hold_step(
            &retraction_latched, &clear_run_ticks, 1, 0,
            input->fixed_delta_seconds, config->release_hold_ticks,
            config->release_near_contact_extension_ticks);
    } else if (distance > 0.0f) {
        float recovery;

        release_held = !mdkr_camera_release_hold_step(
            &retraction_latched, &clear_run_ticks, 0,
            input->corridor_near_contact != 0, input->fixed_delta_seconds,
            config->release_hold_ticks,
            config->release_near_contact_extension_ticks);
        if (release_held) {
            /*
             * Hold the boom where the caller anchored it. The anchor is
             * pivot-relative, so a held boom still tracks the subject: it
             * simply does not grow toward desired while the contact that
             * caused the retraction is still unproven gone.
             */
            candidate = anchor;
        } else {
            recovery = config->recovery_speed * input->fixed_delta_seconds;
            if (!isfinite(recovery)) {
                mdkr_camera_resolver_fail(out_result, MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID,
                                          anchor, guard);
                return out_result->status;
            }
            if (recovery > config->max_recovery_step) {
                recovery = config->max_recovery_step;
            }
            if (config->max_recovery_fraction > 0.0f) {
                /* At large world coordinates a fraction of a one-ULP error
                 * can round to the same eye forever. Settle only at numerical
                 * precision, still subject to speed/step caps and the normal
                 * final safety query. A paused solve has recovery == 0. */
                float scale = fmaxf(1.0f, fmaxf(fabsf(anchor.x), fabsf(input->desired_eye.x)));
                scale = fmaxf(scale, fmaxf(fabsf(anchor.y), fabsf(input->desired_eye.y)));
                scale = fmaxf(scale, fmaxf(fabsf(anchor.z), fabsf(input->desired_eye.z)));
                const float settle = 4.0f * FLT_EPSILON * scale;
                if (distance > settle) {
                    recovery = fminf(recovery, config->max_recovery_fraction * distance);
                }
            }
            if (recovery > distance) {
                recovery = distance;
            }
            fully_recovered = recovery >= distance;
            candidate = fully_recovered ? input->desired_eye :
                                        mdkr_camera_resolver_lerp(anchor, input->desired_eye,
                                                                  recovery / distance);
        }
    } else {
        /* Already sitting on desired: there is no retraction left to release. */
        fully_recovered = 1;
        candidate = anchor;
        retraction_latched = 0;
        clear_run_ticks = 0U;
    }

    /* Do not publish a numerically marginal retraction or recovery candidate. */
    sweep_status = mdkr_camera_resolver_stationary_sweep(
        input->query, guard, candidate, input->mask,
        input->ignored_object_generation, &hit);
    if (sweep_status != MDKR_CAMERA_SWEEP_CLEAR) {
        mdkr_camera_resolver_fail(out_result,
                                  sweep_status == MDKR_CAMERA_SWEEP_INVALID ?
                                      MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID :
                                      MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE,
                                  anchor, guard);
        out_result->path_was_blocked = 1;
        out_result->used_last_safe = (uint8_t)used_last_safe;
        out_result->projection_revalidated = (uint8_t)projection_changed;
        return out_result->status;
    }

    state->last_safe_eye = candidate;
    state->projection_generation = input->projection->generation;
    state->last_safe_valid = 1;
    state->retraction_latched = retraction_latched;
    state->clear_run_ticks = clear_run_ticks;
    memset(out_result, 0, sizeof(*out_result));
    /*
     * A held tick reports RETRACTED because that is what the published eye is:
     * a boom kept short of the authored desired pose by the obstruction
     * correction. path_was_blocked stays the ground truth about this tick's
     * sweep, and release_held says which of the two produced the status.
     */
    out_result->status = (desired_blocked || release_held) ?
                             MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED :
                         fully_recovered ? MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR :
                                           MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING;
    out_result->resolved_eye = candidate;
    out_result->guard = guard;
    if (desired_blocked) {
        out_result->blocker_kind = obstruction_hit.kind;
        out_result->blocker_stable_id = obstruction_hit.stable_id;
        out_result->blocker_normal = obstruction_hit.normal;
        out_result->blocker_point = obstruction_hit.point;
        out_result->blocker_fraction = obstruction_hit.fraction;
    }
    out_result->accepted = 1;
    out_result->path_was_blocked = (uint8_t)desired_blocked;
    out_result->release_held = (uint8_t)release_held;
    out_result->used_last_safe = (uint8_t)used_last_safe;
    out_result->projection_revalidated = (uint8_t)projection_changed;
    return out_result->status;
}
