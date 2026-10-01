/*
 * Presentation replay of a replacement sprite model, by behaviour.
 *
 * Links the PRODUCTION game/src/camera.c (dead-stripped to the replay path),
 * the snapshot store and the presentation packet census. A high-refresh
 * display replays each retained sprite-model recipe at the interpolated pose
 * between two authored ticks; nothing else in the suite runs that branch
 * against a known answer.
 *
 *  - The replayed matrix is the model matrix at the interpolated position and
 *    the shortest-arc interpolated phase (a banana crossing its 255 -> 0 seam).
 *  - The tick-agreement check runs once per replayed recipe (audit A1 #5): a
 *    second evaluation double-counted the census the smoothing gates read.
 *  - A recipe from a tick the snapshot pair cannot name suppresses the
 *    residual, once, and still draws the interpolated pose.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "presentation_snapshot.h"
#include "fast3d/gfx_presentation_packet.h"
#include "fast3d/gfx_shadow_frame.h"
#include "sprite_model_mesh.h"

bool mdkr_camera_replay_object_world(const GfxPresentationMatrixOwner *owner,
                                     uint64_t numerator, uint64_t denominator,
                                     float outWorld[4][4]);

static int failures;
#define EXPECT(cond, ...)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

/* The host frame counter, read only by snapshot diagnostics. */
int g_frameCounter;

/* The replay notes each recomposed pose for the model census. */
static unsigned sReplayNotes;
static float sLastPhase;
void mdkr_sprite_models_note_pose(unsigned sprite, float phase, int attached, int replayed) {
    (void)sprite; (void)attached;
    if (replayed) {
        ++sReplayNotes;
        sLastPhase = phase;
    }
}

static unsigned char sObject[64];

static PresentationObjectEntry entry(float x, uint8_t phase) {
    PresentationObjectEntry e;
    memset(&e, 0, sizeof e);
    e.address = sObject;
    (void)presentation_snapshot_identity_generation(sObject, &e.generation);
    e.position[0] = x;
    e.position[1] = 4.0f;
    e.position[2] = -2.0f;
    e.scale = 1.0f;
    e.rotation_y = 0x1000;
    e.animation_id = -1;
    e.sprite_phase = phase;
    e.model_index = 0;
    e.opacity = 255;
    return e;
}

static void stats(uint64_t *checks, uint64_t *suppressed) {
    GfxPresentationPacketStats s;
    gfx_presentation_packet_get_stats(&s);
    *checks = s.owner_tick_checks;
    *suppressed = s.owner_tick_suppressed;
}

static bool same_matrix(float a[4][4], float b[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (fabsf(a[r][c] - b[r][c]) > 1e-4f) return false;
    return true;
}

static GfxPresentationMatrixOwner owner_at(uint64_t capture_tick, uint64_t generation) {
    GfxPresentationMatrixOwner owner;
    memset(&owner, 0, sizeof owner);
    owner.valid = true;
    owner.address = sObject;
    owner.generation = generation;
    owner.matrix_class = GFX_PRESENTATION_MATRIX_SPRITE_MODEL;
    owner.capture_tick = capture_tick;
    /* The recipe was authored at the first snapshot's pose: no residual. */
    owner.source_position[0] = 0.0f;
    owner.source_position[1] = 4.0f;
    owner.source_position[2] = -2.0f;
    owner.source_scale = 1.0f;
    owner.source_rotation[0] = 0x1000;
    owner.sprite_model_id = 156; /* banana: its phase is its spin */
    owner.sprite_model_phase = 250.0f;
    for (int i = 0; i < 4; ++i) owner.parent_world[i][i] = 1.0f;
    return owner;
}

int main(void) {
    uint64_t generation = 0, checks0, suppressed0, checks1, suppressed1, endpoint = 0;
    float world[4][4], expected[4][4];
    PresentationObjectEntry sample;

    presentation_snapshot_shutdown();
    presentation_snapshot_set_enabled(true);
    presentation_snapshot_note_spawn(sObject);
    EXPECT(presentation_snapshot_identity_generation(sObject, &generation) && generation != 0,
           "the banana has a lifetime");
    /* Authored tick 10 at x = 0, phase 250; tick 11 at x = 10, phase 4: the
     * spin crosses the 255 -> 0 seam, a 10-step turn, not 246 back. */
    presentation_snapshot_capture_begin_authored(10);
    sample = entry(0.0f, 250);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin_authored(11);
    sample = entry(10.0f, 4);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    EXPECT(presentation_snapshot_authored_endpoint_tick(&endpoint) && endpoint == 10,
           "the replay residual is measured against tick 10 (got %llu)", (unsigned long long)endpoint);

    /* Halfway between the ticks: x = 5, phase 255. */
    {
        MdkrSpriteModelPose pose;
        memset(&pose, 0, sizeof pose);
        pose.position[0] = 5.0f; pose.position[1] = 4.0f; pose.position[2] = -2.0f;
        pose.scale = 1.0f;
        pose.rotation[0] = 0x1000;
        pose.sprite = 156;
        pose.phase = 255.0f;
        EXPECT(mdkr_sprite_model_matrix(&pose, expected), "expected pose builds");
    }

    GfxPresentationMatrixOwner owner = owner_at(10, generation);
    stats(&checks0, &suppressed0);
    EXPECT(mdkr_camera_replay_object_world(&owner, 1, 2, world), "a same-tick recipe replays");
    stats(&checks1, &suppressed1);
    EXPECT(checks1 - checks0 == 1, "one tick-agreement check per replayed recipe (counted %llu)",
           (unsigned long long)(checks1 - checks0));
    EXPECT(suppressed1 == suppressed0, "an agreeing recipe is not suppressed");
    EXPECT(same_matrix(world, expected), "the replay draws the interpolated position and shortest-arc spin");
    EXPECT(sReplayNotes == 1 && fabsf(sLastPhase - 255.0f) < 1e-3f,
           "the census sees one replayed pose at phase 255 (saw %u at %g)", sReplayNotes, sLastPhase);

    /* A recipe from a tick the pair cannot name: suppressed, counted once. */
    owner = owner_at(9, generation);
    stats(&checks0, &suppressed0);
    EXPECT(mdkr_camera_replay_object_world(&owner, 1, 2, world), "a stale recipe still replays");
    stats(&checks1, &suppressed1);
    EXPECT(checks1 - checks0 == 1 && suppressed1 - suppressed0 == 1,
           "a stale recipe is checked and suppressed once (checked %llu, suppressed %llu)",
           (unsigned long long)(checks1 - checks0), (unsigned long long)(suppressed1 - suppressed0));
    EXPECT(same_matrix(world, expected), "a suppressed recipe draws the interpolated pose itself");

    presentation_snapshot_shutdown();
    if (failures) {
        fprintf(stderr, "sprite_model_replay: %d failure(s)\n", failures);
        return 1;
    }
    puts("sprite_model_replay: interpolated pose and spin; one tick-agreement check per recipe");
    return 0;
}
