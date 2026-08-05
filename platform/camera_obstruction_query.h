/**
 * camera_obstruction_query.h -- pure composition of independent sweep sources.
 *
 * Sources own all world data (track cache, object instances, future soft
 * policy). This layer owns none: it walks a caller-owned fixed array once in
 * array order, performs no allocation, and publishes one canonical result.
 */
#ifndef MDKR64_CAMERA_OBSTRUCTION_QUERY_H
#define MDKR64_CAMERA_OBSTRUCTION_QUERY_H

#include "camera_obstruction.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fraction tolerance for source-composition ordering. It deliberately matches
 * the sweep kernels' time-tie tolerance: candidates within this distance are
 * an ordering tie, resolved by stable ID rather than incidental float noise. */
#define MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON 1.0e-9

/* Signature intentionally matches MdkrCameraObstructionQuery.sweep without
 * including resolver.h, avoiding a platform-layer include cycle. */
typedef MdkrCameraSweepStatus (*MdkrCameraObstructionQuerySweepFn)(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

typedef struct MdkrCameraObstructionQuerySource {
    MdkrCameraObstructionQuerySweepFn sweep;
    const void *context;
} MdkrCameraObstructionQuerySource;

typedef struct MdkrCameraObstructionCombinedQuery {
    const MdkrCameraObstructionQuerySource *sources;
    size_t source_count;
    size_t source_capacity;
} MdkrCameraObstructionCombinedQuery;

/* Rounded-lens sources remain a distinct type so a sphere-only source cannot
 * accidentally receive an oriented guard through a cast. */
typedef MdkrCameraSweepStatus (*MdkrCameraObstructionRoundedLensQuerySweepFn)(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);

typedef struct MdkrCameraObstructionRoundedLensQuerySource {
    MdkrCameraObstructionRoundedLensQuerySweepFn sweep;
    const void *context;
} MdkrCameraObstructionRoundedLensQuerySource;

typedef struct MdkrCameraObstructionRoundedLensCombinedQuery {
    const MdkrCameraObstructionRoundedLensQuerySource *sources;
    size_t source_count;
    size_t source_capacity;
} MdkrCameraObstructionRoundedLensCombinedQuery;

typedef enum MdkrCameraObstructionTwoPhaseOutcome {
    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_INVALID = 0,
    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_SPHERE_CLEAR,
    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR,
    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_HIT,
    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK,
} MdkrCameraObstructionTwoPhaseOutcome;

typedef struct MdkrCameraObstructionTwoPhaseDecision {
    MdkrCameraSweepStatus sphere_status;
    MdkrCameraSweepStatus exact_status;
    MdkrCameraObstructionTwoPhaseOutcome outcome;
    uint8_t exact_invoked;
    uint8_t degraded;
} MdkrCameraObstructionTwoPhaseDecision;

/**
 * Compose sources in their caller-published stable order. Child HIT stable IDs
 * must be globally unique across the supplied sources. A fraction more than
 * MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON earlier wins; fractions within
 * that tolerance tie-break by lower stable ID. Duplicate stable IDs within a
 * tolerance tie are invalid because the caller failed its global-ID contract.
 */
MdkrCameraSweepStatus mdkr_camera_obstruction_combined_sweep(
    const MdkrCameraObstructionCombinedQuery *query,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

/* Direct adapter for MdkrCameraObstructionQuery { sweep, context }. */
MdkrCameraSweepStatus mdkr_camera_obstruction_combined_sweep_adapter(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

/* Rounded-lens counterpart to mdkr_camera_obstruction_combined_sweep().
 * It has the identical fail-closed and tolerance-aware stable-ID contract. */
MdkrCameraSweepStatus mdkr_camera_obstruction_combined_rounded_lens_sweep(
    const MdkrCameraObstructionRoundedLensCombinedQuery *query,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);

/* Direct adapter for an oriented-lens resolver query { sweep, context }. */
MdkrCameraSweepStatus mdkr_camera_obstruction_combined_rounded_lens_sweep_adapter(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);

/*
 * Conservative two-phase dispatch for one fixed-basis corridor. The sphere
 * input's radius is promoted, when necessary, to an outward float enclosing
 * exact_guard. A sphere CLEAR therefore proves exact CLEAR without invoking
 * exact_query. A sphere HIT runs the complete exact query; exact INVALID keeps
 * the conservative sphere HIT and reports a degraded fallback in out_decision.
 */
MdkrCameraSweepStatus mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    const MdkrCameraSweepInput *sphere_input,
    const MdkrCameraRoundedLensGuard *exact_guard,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraObstructionTwoPhaseDecision *out_decision);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CAMERA_OBSTRUCTION_QUERY_H */
