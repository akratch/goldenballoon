/* enh_draw_distance.h — how far the world is drawn, and at what detail.
 *
 * Two settings live here, and both are declared MDKR_ENH_PRESENTATION in
 * platform/enhancement_registry.c. That declaration is the design constraint,
 * and for THIS enhancement it is a sharp one, because DKR's draw distance is
 * the kind of thing that is easy to move to the wrong side of the line.
 *
 * WHY THIS IS NOT SIMPLY "RAISE THE THRESHOLD". The authored predicate is
 * check_if_in_draw_range() in game/src/tracks.c, and in this port it is called
 * from TWO places with completely different authority:
 *
 *   game/src/tracks.c scene_authoritative_render_tick()  — the FIXED TICK. It
 *       writes obj->opacity through that call, and obj->opacity is hashed by
 *       the [SIMHASH] v3 stream (platform/sim_hash.c). Admission there also
 *       gates obj_authoritative_texture_tick(). Widening the threshold here
 *       would make "how far you can see" an input to the simulation, which is
 *       the definition of a GAMEPLAY enhancement.
 *
 *   game/src/tracks.c render_level_geometry_and_objects()  — the DRAW. It
 *       replays the tick's per-viewport admission out of the route cache and
 *       writes nothing back.
 *
 * So the multiplier is threaded through the draw only, as a parameter, and the
 * tick keeps passing 1.0f — a value the arithmetic skips outright, so the
 * authoritative path is not merely equivalent to the authored one, it is the
 * same instructions. Nothing in this module writes to an Object.
 *
 * WHY IT IS LATCHED. The scale is read from the config once per drawn frame
 * (mdkr_enh_draw_distance_begin_frame) rather than per object. A live setting
 * that changed between two objects of the same frame would cull half a frame
 * at one radius and half at another, and the seam would move every frame.
 */
#ifndef MDKR64_ENH_DRAW_DISTANCE_H
#define MDKR64_ENH_DRAW_DISTANCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The schema's range for Enhancements.DrawDistance, as a percentage of the
 * authored per-object distance; see platform/video_config.c. 1600 is the
 * schema's "Maximum" notch: CAMERA_FAR (game/src/camera.h) is 15000, and the
 * only multiplayer reduction halves the authored distance in the
 * four-player layout (game/src/tracks.c). The worst case needing saturation
 * is the smallest finite header distance (2000) taking that halving --
 * 1000 effective units -- which needs scale >= 15000/1000 = 1500% to reach
 * the far plane. 1600% clears that bound with headroom and keeps the s32
 * arithmetic in check_if_in_draw_range_impl trivially safe (6000 * 16 =
 * 96000). */
#define MDKR_DRAW_DISTANCE_MIN_PERCENT 100
#define MDKR_DRAW_DISTANCE_MAX_PERCENT 1600

/* The values Enhancements.LodBias takes. */
enum {
    MDKR_LOD_BIAS_AUTHORED = 0,
    MDKR_LOD_BIAS_HIGH = 1,
    MDKR_LOD_BIAS_MAX = 2
};

/* Latch both settings for the frame about to be drawn. Called once from
 * render_scene(), before any viewport. */
void mdkr_enh_draw_distance_begin_frame(void);

/* The latched multiplier on the authored draw distance: exactly 1.0f at the
 * authored default, up to 4.0f. Callers compare against 1.0f to take the
 * authored path unchanged. */
float mdkr_enh_draw_distance_scale(void);

/* `modelIndex` is the level of detail the authored distance ladder chose, 0
 * being the most detailed; [firstModel, lastModel] is the range of models this
 * object actually carries. Returns `modelIndex` unchanged at
 * MDKR_LOD_BIAS_AUTHORED — by returning early, before even the clamp, so bias 0
 * is the authored index bit for bit rather than the authored index put through
 * arithmetic that happens to be the identity.
 *
 * Biasing the INDEX rather than the distance fed to the ladder is a measured
 * choice, not a stylistic one. DKR's band tables are per vehicle AND per
 * viewport layout, and the split-screen tables open with zero-width bands: no
 * distance, however small, selects model 0 or 1 in a two- or four-player race,
 * so a setting that only moved the distance would be inert in exactly the mode
 * where reduced detail is most visible. Subtracting from the chosen index
 * crosses those bands.
 *
 * Clamping both the authored and the biased index into the object's real range
 * before comparing them is what lets this report whether the player will
 * actually see a different mesh, rather than whether an integer changed on its
 * way into a clamp that erased the difference. */
int mdkr_enh_lod_bias_apply(int modelIndex, int firstModel, int lastModel);

/* Diagnostic, silent unless MDKR_DRAWDIST_TRACE=1. The draw tallies how many
 * objects the fixed tick admitted and how many more the player's setting
 * added; mdkr_enh_draw_distance_begin_frame() prints the completed tally as one
 * [DRAWDIST] line and resets it. Counting the two separately is the whole
 * point: it is what lets a test watch the DRAWN count move while the live
 * object count in [SIMHASH] stands still. The same line carries `lodShifted`,
 * the number of level-of-detail choices the bias CHANGED this frame — changed,
 * not was consulted about, so a bias that is read and then erased by the
 * model-range clamp reads as the zero it is. */
void mdkr_enh_draw_distance_count(int authored, int extended);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENH_DRAW_DISTANCE_H */
