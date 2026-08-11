#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gfx_presentation_packet.h"
#include "gfx_deformation_shape.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static GfxPresentationMatrixOwner make_owner(const void *address,
                                             uint64_t generation) {
    GfxPresentationMatrixOwner owner;
    memset(&owner, 0, sizeof(owner));
    owner.address = address;
    owner.generation = generation;
    owner.matrix_class = GFX_PRESENTATION_MATRIX_BILLBOARD;
    owner.source_scale = 1.0f;
    owner.scale_y = 1.0f;
    owner.valid = true;
    return owner;
}

static GfxPresentationUvScroll make_scroll(int32_t du, int32_t dv,
                                           uint32_t count, uint16_t moved_u,
                                           uint16_t moved_v) {
    GfxPresentationUvScroll scroll;
    memset(&scroll, 0, sizeof(scroll));
    scroll.du = du;
    scroll.dv = dv;
    scroll.triangle_count = count;
    scroll.moved_u = moved_u;
    scroll.moved_v = moved_v;
    return scroll;
}

/* Stage one authored tick's census: open a capture, offer the batches, publish
 * the table under `tick`, and take the freeze the lookup path requires. This is
 * the order gfx_dkr_capture_future_deformations runs in, condensed. */
static void publish_uv_tick(uint64_t tick, const void *key_a,
                            const GfxPresentationUvScroll *a,
                            const void *key_b,
                            const GfxPresentationUvScroll *b) {
    gfx_presentation_packet_capture_begin(tick);
    if (a != NULL) {
        (void)gfx_presentation_packet_capture_uv_scroll(key_a, a);
    }
    if (b != NULL) {
        (void)gfx_presentation_packet_capture_uv_scroll(key_b, b);
    }
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_publish_uv_scroll(tick);
}

/*
 * The authored UV-scroll contract, which until now had no unit caller at all:
 * the architecture note recorded a ROM-driven integration arm as its only
 * evidence. Everything asserted here is a decision the replay makes for every
 * scrolling surface on every interpolated present, and every refusal it makes
 * is a surface that visibly steps at the tick rate while its neighbours glide
 * -- so which clause refused, and how often, is the whole disposition of
 * artifact class C8.
 */
static void check_uv_scroll(void) {
    static const unsigned char batch_a[4] = { 0, 0, 0, 0 };
    static const unsigned char batch_b[4] = { 0, 0, 0, 0 };
    GfxPresentationUvScroll scroll = make_scroll(4, 0, 2u, 0x3u, 0u);
    GfxPresentationUvScroll faster = make_scroll(8, 0, 2u, 0x3u, 0u);
    GfxPresentationUvScroll wider = make_scroll(4, 0, 3u, 0x7u, 0u);
    GfxPresentationUvScroll out;
    GfxPresentationPacketStats stats;

    gfx_presentation_packet_shutdown();

    /* Capture is a census-only operation. Outside one there is no tick to
     * stage against, and offering a batch anyway must be refused rather than
     * land in whatever table happens to be live. */
    expect(!gfx_presentation_packet_capture_uv_scroll(batch_a, &scroll),
           "UV scroll refuses capture outside an authoring census");

    gfx_presentation_packet_capture_begin(10u);
    expect(!gfx_presentation_packet_capture_uv_scroll(NULL, &scroll) &&
               !gfx_presentation_packet_capture_uv_scroll(batch_a, NULL),
           "UV scroll refuses a null key or record");
    expect(!gfx_presentation_packet_capture_uv_scroll(
               batch_a, &(GfxPresentationUvScroll){ 0 }),
           "UV scroll refuses a batch that did not move");
    {
        GfxPresentationUvScroll huge = make_scroll(
            4, 0, GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES + 1u, 0x1u, 0u);
        expect(!gfx_presentation_packet_capture_uv_scroll(batch_a, &huge),
               "UV scroll refuses more triangles than a G_TRIN batch holds");
    }
    gfx_presentation_packet_capture_abort();

    /* One published tick is not a confirmation. The first present that draws
     * a scroller therefore holds its authored phase, and says which clause
     * held it: nothing has been seen at T-1 yet. The empty tick 10 exists so
     * the pair IS adjacent -- a non-adjacent pair is refused by the table's
     * own guard before any clause runs, and would prove nothing about them. */
    publish_uv_tick(10u, NULL, NULL, NULL, NULL);
    publish_uv_tick(11u, batch_a, &scroll, NULL, NULL);
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 11u, 2u, &out),
           "a scroller's first published tick does not confirm");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.uv_scroll_hold_unpublished == 1u &&
               stats.uv_scroll_holds == 1u,
           "the unconfirmed first tick is attributed to the missing {T-1}");

    /* Two adjacent ticks agreeing is the confirmation, and it is the only
     * thing that reaches the screen. */
    publish_uv_tick(12u, batch_a, &scroll, NULL, NULL);
    memset(&out, 0, sizeof(out));
    expect(gfx_presentation_packet_lookup_uv_scroll(batch_a, 12u, 2u, &out) &&
               out.du == 4 && out.dv == 0 && out.moved_u == 0x3u,
           "two adjacent ticks agreeing confirm the displacement");

    /* The published table is tick-exact in both directions: a replay that
     * asks for any tick but the one on the table gets nothing, and that
     * refusal is not a scroller's hold. */
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 11u, 2u, &out) &&
               !gfx_presentation_packet_lookup_uv_scroll(
                   batch_a, 13u, 2u, &out) &&
               !gfx_presentation_packet_lookup_uv_scroll(batch_a, 0u, 2u,
                                                         &out),
           "UV scroll refuses any tick but the published current one");

    /* A batch the census never saw is not a scroller. Its authored bytes are
     * already exact, so refusing it must not be counted as a hold. */
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    {
        const uint64_t before = stats.uv_scroll_holds;
        expect(!gfx_presentation_packet_lookup_uv_scroll(
                   batch_b, 12u, 2u, &out),
               "an unregistered batch is not a scroller");
        memset(&stats, 0, sizeof(stats));
        gfx_presentation_packet_get_stats(&stats);
        expect(stats.uv_scroll_holds == before,
               "a non-scroller's refusal is not counted as a phase hold");
    }

    /* The triangle count the replay has in hand has to be the count the
     * census measured, or the correspondence is between different geometry. */
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 12u, 3u, &out),
           "a batch whose triangle count moved cannot confirm");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.uv_scroll_hold_shape == 1u,
           "a topology disagreement is attributed to the shape clause");

    /* Two ticks that disagree about the displacement is the wrap the resolver
     * could not undo. Holding is the fail-closed answer and the phase clause
     * is what names it. */
    publish_uv_tick(13u, batch_a, &faster, NULL, NULL);
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 13u, 2u, &out),
           "a displacement that changed between ticks holds");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.uv_scroll_hold_phase == 1u,
           "a displacement disagreement is attributed to the phase clause");

    /* Two different batches at one address inside a single census: the replay
     * cannot tell them apart, so the key is poisoned rather than letting the
     * later one decide the earlier one's phase. */
    gfx_presentation_packet_capture_begin(14u);
    expect(gfx_presentation_packet_capture_uv_scroll(batch_a, &faster),
           "the first observation of a key in a census registers");
    expect(gfx_presentation_packet_capture_uv_scroll(batch_a, &faster),
           "an identical repeat observation is idempotent");
    expect(!gfx_presentation_packet_capture_uv_scroll(batch_a, &wider),
           "a second, different batch at the same address is refused");
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_publish_uv_scroll(14u);
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 14u, 2u, &out),
           "a poisoned key holds even when both ticks agree");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.uv_scroll_hold_ambiguous == 1u &&
               stats.uv_scroll_collisions == 1u,
           "an ambiguous key is attributed to the collision clause");

    /* A census that publishes nothing discards its staged table rather than
     * leaving the previous tick to masquerade as this one. */
    publish_uv_tick(15u, batch_a, &scroll, NULL, NULL);
    publish_uv_tick(16u, batch_a, &scroll, NULL, NULL);
    expect(gfx_presentation_packet_lookup_uv_scroll(batch_a, 16u, 2u, &out),
           "a fresh adjacent pair confirms again after a poisoned tick");
    gfx_presentation_packet_capture_begin(17u);
    (void)gfx_presentation_packet_capture_uv_scroll(batch_a, &scroll);
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_publish_uv_scroll(0u);
    expect(gfx_presentation_packet_lookup_uv_scroll(batch_a, 16u, 2u, &out),
           "a discarded census leaves the last published pair in place");

    gfx_presentation_packet_invalidate();
    expect(!gfx_presentation_packet_lookup_uv_scroll(batch_a, 16u, 2u, &out),
           "stage invalidation drops every published UV-scroll table");
    gfx_presentation_packet_shutdown();
}

/*
 * The authored-rate path, and the reason it exists.
 *
 * obj_loop_texscroll advances a level texture through a two-bit accumulator,
 * so an authored rate below four quarter-units a tick emits ZERO whole units
 * on some ticks and N on others. Measured against its own result that scroller
 * can never confirm: half its ticks publish nothing at all and the other half
 * disagree with their neighbour. It holds on every present, forever, and the
 * texture keeps a 30 Hz cadence beside a world drawn at the host rate.
 *
 * A record that carries the rate ITSELF is not subject to any of that, and
 * this is where that is asserted -- including the part that is easy to get
 * wrong: the record must survive a tick whose emitted displacement is zero.
 */
static void check_uv_scroll_authored(void) {
    static const unsigned char slow_batch[4] = { 0, 0, 0, 0 };
    static const unsigned char measured_batch[4] = { 0, 0, 0, 0 };
    GfxPresentationUvScroll authored;
    GfxPresentationUvScroll measured = make_scroll(4, 0, 2u, 0x3u, 0u);
    GfxPresentationUvScroll out;
    GfxPresentationPacketStats stats;

    gfx_presentation_packet_shutdown();

    /* Rate 2 quarter-units a tick: the accumulator emits 0, 1, 0, 1 ... */
    memset(&authored, 0, sizeof(authored));
    authored.triangle_count = 2u;
    authored.moved_u = 0x3u;
    authored.authored = true;
    authored.rate_u = 2;
    authored.phase_u = 0;
    authored.du = 0;              /* this tick emits nothing at all */

    /* The table's own adjacency guard is separate from the confirmation rule
     * and is NOT relaxed: the empty tick 19 is what makes 20 the successor of
     * a published tick, exactly as the measured case needs. */
    publish_uv_tick(19u, NULL, NULL, NULL, NULL);
    publish_uv_tick(20u, slow_batch, &authored, NULL, NULL);
    memset(&out, 0, sizeof(out));
    expect(gfx_presentation_packet_lookup_uv_scroll(slow_batch, 20u, 2u,
                                                    &out) &&
               out.authored && out.rate_u == 2 && out.phase_u == 0,
           "an authored rate confirms on its FIRST published tick");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.uv_scroll_holds == 0u,
           "an authored rate needs no second observation, so it never holds "
           "for want of one");

    /* The next tick carries the residue forward and still emits nothing. A
     * measured record with du=0 is static geometry and is rightly refused;
     * an authored one is a scroller mid-quarter and must not be. */
    authored.phase_u = 2;
    publish_uv_tick(21u, slow_batch, &authored, NULL, NULL);
    memset(&out, 0, sizeof(out));
    expect(gfx_presentation_packet_lookup_uv_scroll(slow_batch, 21u, 2u,
                                                    &out) &&
               out.phase_u == 2,
           "a zero-displacement authored tick is published, not dropped");

    /* Consecutive authored ticks disagree by construction -- that is what a
     * sub-unit rate DOES -- and the disagreement must not refuse them. */
    authored.phase_u = 0;
    authored.du = 1;
    publish_uv_tick(22u, slow_batch, &authored, NULL, NULL);
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(gfx_presentation_packet_lookup_uv_scroll(slow_batch, 22u, 2u,
                                                    &out) &&
               stats.uv_scroll_hold_phase == 0u,
           "an authored rate is not refused for disagreeing with its own "
           "previous tick");

    /* What the authored path does NOT relax. Shape and ambiguity still refuse:
     * they say the record does not describe this batch, which is a different
     * question from whether the displacement can be trusted. */
    expect(!gfx_presentation_packet_lookup_uv_scroll(slow_batch, 22u, 3u,
                                                     &out),
           "an authored record still refuses a batch of another shape");
    gfx_presentation_packet_capture_begin(23u);
    expect(gfx_presentation_packet_capture_uv_scroll(slow_batch, &authored),
           "the first authored observation of a key registers");
    {
        GfxPresentationUvScroll other = authored;
        other.rate_u = 6;
        expect(!gfx_presentation_packet_capture_uv_scroll(slow_batch, &other),
               "two authored rates at one address are refused");
    }
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_publish_uv_scroll(23u);
    expect(!gfx_presentation_packet_lookup_uv_scroll(slow_batch, 23u, 2u,
                                                     &out),
           "an authored record on a poisoned key still holds");

    /* And the measured contract is untouched: a measured batch alongside an
     * authored one still needs its previous tick to agree. */
    gfx_presentation_packet_shutdown();
    publish_uv_tick(29u, NULL, NULL, NULL, NULL);
    publish_uv_tick(30u, measured_batch, &measured, NULL, NULL);
    expect(!gfx_presentation_packet_lookup_uv_scroll(measured_batch, 30u, 2u,
                                                     &out),
           "a measured scroller still does not confirm on one tick");
    publish_uv_tick(31u, measured_batch, &measured, NULL, NULL);
    expect(gfx_presentation_packet_lookup_uv_scroll(measured_batch, 31u, 2u,
                                                    &out) &&
               !out.authored && out.du == 4,
           "a measured scroller still confirms on an agreeing pair");
    {
        GfxPresentationUvScroll still;
        memset(&still, 0, sizeof(still));
        still.triangle_count = 2u;
        still.moved_u = 0x3u;
        gfx_presentation_packet_capture_begin(32u);
        expect(!gfx_presentation_packet_capture_uv_scroll(measured_batch,
                                                          &still),
               "a measured batch that did not move is still refused");
        gfx_presentation_packet_capture_abort();
    }
    gfx_presentation_packet_shutdown();
}

/* Build one Vertex-shaped (stride 10: s16 x @0, s16 y @2, s16 z @4, RGBA @6)
 * element into `buf + index*10`. Y and color are left zeroed -- the reorder
 * test only reads X/Z. */
static void set_shadow_vertex(unsigned char *buf, unsigned index, int16_t x,
                              int16_t z) {
    unsigned char *element = buf + (size_t)index * 10u;
    memcpy(element + 0, &x, sizeof(x));
    memcpy(element + 4, &z, sizeof(z));
}

/*
 * Task 9: gfx_presentation_packet_deformation_reordered.
 *
 * A regenerated projected-shadow decal has no discrete topology "variant" a
 * game-side note could name (see waves.c's LOD key); the only evidence
 * available is whether index i still names the same authored point across
 * two published ticks, judged in X/Z. This proves the low-level detector
 * directly against synthetic bindings, then once more through the real
 * capture/freeze/lookup pipeline so the exact function the choke point calls
 * (gfx_presentation_packet_lookup_deformation) is what feeds it.
 */
static void check_shadow_reorder_detection(void) {
    unsigned char previous[30];
    unsigned char current_shifted[30];
    unsigned char current_reordered[30];
    unsigned char current_tied[30];
    GfxPresentationDeformationBinding binding;
    GfxPresentationMatrixOwner shadow_owner;

    memset(&binding, 0, sizeof(binding));

    /* ---- low-level function, synthetic bindings ---- */

    /* Three vertices on a line: (0,0), (100,0), (200,0). */
    memset(previous, 0, sizeof(previous));
    set_shadow_vertex(previous, 0, 0, 0);
    set_shadow_vertex(previous, 1, 100, 0);
    set_shadow_vertex(previous, 2, 200, 0);

    expect(!gfx_presentation_packet_deformation_reordered(NULL, 0u, 4u),
           "shadow reorder: a null binding is not a reorder");

    binding.previous_bytes = previous;
    binding.current_bytes = previous;
    binding.count = 1u;
    binding.stride = 10u;
    expect(!gfx_presentation_packet_deformation_reordered(&binding, 0u, 4u),
           "shadow reorder: a single vertex has nothing to reorder against");

    binding.count = 3u;
    /* x_offset=8 does NOT overrun (8 + sizeof(int16_t) == stride, not
     * greater) -- current_bytes still equals previous_bytes at this point
     * in the test (see the earlier `binding.current_bytes = previous;`),
     * so this call returns false trivially (every vertex matches itself)
     * and is here only as a same-call baseline. z_offset=9 DOES overrun
     * (9 + sizeof(int16_t) == 11 > stride == 10) and is the call that
     * actually exercises the bounds guard. */
    expect(!gfx_presentation_packet_deformation_reordered(&binding, 8u, 4u) &&
               !gfx_presentation_packet_deformation_reordered(&binding, 0u,
                                                               9u),
           "shadow reorder: an offset that overruns the stride fails closed "
           "(the z_offset=9 call; x_offset=8 is an in-bounds baseline)");

    /* Ordinary motion: every vertex drifts +5 in X, staying nearest its own
     * previous-tick position. Not a reorder. */
    memset(current_shifted, 0, sizeof(current_shifted));
    set_shadow_vertex(current_shifted, 0, 5, 0);
    set_shadow_vertex(current_shifted, 1, 105, 0);
    set_shadow_vertex(current_shifted, 2, 205, 0);
    binding.current_bytes = current_shifted;
    expect(!gfx_presentation_packet_deformation_reordered(&binding, 0u, 4u),
           "shadow reorder: ordinary per-slot motion is not a reorder");

    /* Authored vertex order changed: this tick's slots 0 and 1 carry what
     * were slots 1 and 0 last tick (same topology -- same count, same
     * triangle indices elsewhere -- different authoring order). Vertex 0's
     * nearest previous-tick neighbour is now slot 1, not slot 0. */
    memset(current_reordered, 0, sizeof(current_reordered));
    set_shadow_vertex(current_reordered, 0, 100, 0); /* was slot 1's spot */
    set_shadow_vertex(current_reordered, 1, 0, 0);   /* was slot 0's spot */
    set_shadow_vertex(current_reordered, 2, 200, 0);
    binding.current_bytes = current_reordered;
    expect(gfx_presentation_packet_deformation_reordered(&binding, 0u, 4u),
           "shadow reorder: a two-slot swap with identical topology is "
           "caught even though this is exactly the case a per-index "
           "magnitude guard (owner->max_vertex_delta) cannot see -- each "
           "individual slot's own delta (100 units) is unremarkable, only "
           "the CORRESPONDENCE is wrong");

    /* Mutation check: a detector that always answered "identity permutation"
     * (the naive/broken form this guards against) would pass every case
     * above except this one -- it is the one assertion that actually
     * exercises the nearest-neighbour search rather than a fixed-false
     * return, so a stubbed-out detector fails exactly here. */

    /* Degenerate but legitimate: two vertices land on the exact same X/Z
     * both ticks (a decal seam). Ties must not manufacture a false reorder:
     * each slot's own previous position is an equally good match, and the
     * detector must prefer the identity assignment on a tie. */
    memset(current_tied, 0, sizeof(current_tied));
    set_shadow_vertex(previous, 0, 50, 0);
    set_shadow_vertex(previous, 1, 50, 0); /* duplicate position */
    set_shadow_vertex(previous, 2, 200, 0);
    set_shadow_vertex(current_tied, 0, 50, 0);
    set_shadow_vertex(current_tied, 1, 50, 0);
    set_shadow_vertex(current_tied, 2, 200, 0);
    binding.current_bytes = current_tied;
    expect(!gfx_presentation_packet_deformation_reordered(&binding, 0u, 4u),
           "shadow reorder: a duplicate-position tie resolves to the "
           "identity assignment, not a false positive");

    /* ---- integration: the real capture/freeze/lookup pipeline the choke
     * point actually calls (gfx_presentation_packet_lookup_deformation) ---- */

    gfx_presentation_packet_shutdown();
    shadow_owner = make_owner(current_shifted, 41u);
    shadow_owner.matrix_class = GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;

    memset(previous, 0, sizeof(previous));
    set_shadow_vertex(previous, 0, 0, 0);
    set_shadow_vertex(previous, 1, 100, 0);
    set_shadow_vertex(previous, 2, 200, 0);
    gfx_presentation_packet_capture_begin(6000u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 1, 7u, previous, sizeof(previous), 3u, 10u),
           "shadow reorder integration: first tick's decal captures");
    gfx_presentation_packet_freeze();

    gfx_presentation_packet_capture_begin(6001u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 1, 7u, current_reordered,
               sizeof(current_reordered), 3u, 10u),
           "shadow reorder integration: second tick's reordered decal "
           "captures");
    gfx_presentation_packet_freeze();

    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 1, 7u, 6001u, 3u, 10u, &binding),
           "shadow reorder integration: the published pair resolves");
    expect(gfx_presentation_packet_deformation_reordered(&binding, 0u, 4u),
           "shadow reorder integration: the same reorder is caught when fed "
           "through the real lookup path the gfx_pc_dkr.c choke point uses, "
           "not just a hand-built binding");

    gfx_presentation_packet_shutdown();
}

int main(void) {
    unsigned char matrix[64];
    unsigned char vertex[10];
    unsigned char next_matrix[64];
    unsigned char particle_vertex[20];
    unsigned char projected_shadow_vertex[20];
    unsigned char projected_shadow_future_vertex[20];
    unsigned char deform_previous[20];
    unsigned char deform_current[20];
    unsigned char deform_skipped[20];
    unsigned char effect_previous_a[20];
    unsigned char effect_previous_b[20];
    unsigned char effect_current_a[20];
    unsigned char effect_current_b[20];
    unsigned char retained_identity[64];
    unsigned char retained_observation[64];
    unsigned char future_previous[20];
    unsigned char future_current[20];
    GfxPresentationMatrixOwner owner;
    GfxPresentationMatrixOwner effect_a;
    GfxPresentationMatrixOwner effect_b;
    GfxPresentationMatrixOwner shadow_owner;
    GfxPresentationMatrixOwner future_shadow_owner;
    GfxPresentationPacketBinding binding;
    GfxPresentationDeformationBinding deformation;
    GfxPresentationPacketStats stats;
    uint64_t simulated_live_tick;

    memset(matrix, 0x11, sizeof(matrix));
    memset(vertex, 0x22, sizeof(vertex));
    memset(next_matrix, 0x33, sizeof(next_matrix));
    owner = make_owner(matrix, 7u);

    expect(!gfx_deformation_shape_matches(
               UINT32_MAX, 2u, 20u, UINT32_MAX) &&
               !gfx_deformation_shape_matches(
                   UINT32_MAX, UINT32_MAX, 20u, UINT32_MAX) &&
               gfx_deformation_shape_matches(2u, 10u, 20u, UINT32_MAX),
           "wasm32 deformation multiplication guard rejects wrapped shapes");

    expect(!gfx_presentation_packet_register_matrix(
               NULL, sizeof(matrix), 0, &owner),
           "null matrix key fails closed");
    expect(!gfx_presentation_packet_register_vertex(
               vertex, GFX_PRESENTATION_PACKET_MAX_KEY_BYTES + 1u, 0, &owner),
           "oversized retained key fails closed");
    expect(gfx_presentation_packet_register_matrix(
               matrix, sizeof(matrix), 2, &owner),
           "matrix dependency registers");
    owner.generation = 8u;
    expect(gfx_presentation_packet_register_matrix(
               matrix, sizeof(matrix), 3, &owner),
           "latest tenant replaces a repeated matrix address");
    expect(gfx_presentation_packet_register_vertex(
               vertex, sizeof(vertex), 1, &owner),
           "vertex dependency registers");
    owner = make_owner(particle_vertex, 10u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES;
    expect(gfx_presentation_packet_register_vertex_identity(
               particle_vertex, 2, &owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   particle_vertex, &binding) &&
               binding.owner.generation == 10u && binding.viewport == 2 &&
               binding.key_size == 0u,
           "stable particle vertex buffer registers by identity");
    expect(gfx_presentation_packet_has_live_vertex(vertex) &&
               !gfx_presentation_packet_has_frozen_vertex(vertex),
           "vertex recipe is identifiable on the live side before freeze");

    expect(!gfx_presentation_packet_frozen(),
           "live registrations are not visible before publication");
    expect(gfx_presentation_packet_note_walked_matrix(
               matrix, matrix, sizeof(matrix)) &&
               gfx_presentation_packet_note_walked_vertex(
                   vertex, vertex, sizeof(vertex)),
           "real-walk dependency bytes replace the earlier build-time image");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_frozen(),
           "complete dependency packet publishes atomically");
    expect(!gfx_presentation_packet_has_live_vertex(vertex) &&
               gfx_presentation_packet_has_frozen_vertex(vertex) &&
               gfx_presentation_packet_has_frozen_vertex(particle_vertex),
           "vertex recipe identity rotates to the frozen side");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding) &&
               binding.owner.generation == 8u && binding.viewport == 3,
           "frozen matrix carries the latest owner by value");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(vertex, &binding) &&
               binding.owner.generation == 8u && binding.viewport == 1,
           "frozen vertex carries owner and viewport by value");

    matrix[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding) &&
               binding.stale && binding.key_size == sizeof(matrix) &&
               binding.key_bytes[0] == 0x11u,
           "rewritten transient matrix resolves the frozen real-walk bytes");
    gfx_presentation_packet_note_stale_hold(true, true);
    matrix[0] ^= 1u;
    vertex[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(vertex, &binding) &&
               binding.stale && binding.key_size == sizeof(vertex) &&
               binding.key_bytes[0] == 0x22u,
           "rewritten billboard anchor resolves the frozen real-walk bytes");
    gfx_presentation_packet_note_stale_hold(false, true);
    vertex[0] ^= 1u;

    owner = make_owner(next_matrix, 9u);
    expect(gfx_presentation_packet_register_matrix(
               next_matrix, sizeof(next_matrix), 0, &owner),
           "next tick can build while the frozen packet remains visible");
    expect(!gfx_presentation_packet_lookup_matrix(next_matrix, &binding),
           "live next-tick dependency does not leak into frozen lookup");
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding),
           "frozen dependency remains immutable during next-tick build");

    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_matrix(next_matrix, &binding) &&
               !gfx_presentation_packet_lookup_matrix(matrix, &binding),
           "next atomic freeze replaces rather than merges packets");

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.matrix_registrations == 3u &&
               stats.vertex_registrations == 2u &&
               stats.particle_vertex_registrations == 1u &&
               stats.freezes == 2u &&
               stats.freeze_failures == 0u && stats.stale_keys == 2u &&
               stats.stale_matrix_holds == 1u &&
               stats.stale_vertex_holds == 1u &&
               stats.unsafe_stale_fallbacks == 0u &&
               stats.matrix_peak == 1u && stats.vertex_peak == 2u,
           "packet census reports ownership, bounds and stale refusal");

    memset(projected_shadow_vertex, 0x2A, sizeof(projected_shadow_vertex));
    owner = make_owner(projected_shadow_vertex, 11u);
    owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_vertex, 0, 4u, &owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   projected_shadow_vertex, &binding) &&
               binding.owner.matrix_class ==
                   GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES &&
               binding.viewport == 0 && binding.ordinal == 4u &&
               binding.key_size == 0u,
           "projected shadow vertex batch retains object identity and ordinal");
    expect(!gfx_presentation_packet_register_projected_shadow_vertex(
               NULL, 0, 4u, &owner),
           "projected shadow registration rejects a missing vertex identity");
    gfx_presentation_packet_freeze();
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.ordinal == 4u && binding.owner.generation == 11u,
           "projected shadow ownership freezes by value");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.projected_shadow_vertex_registrations == 1u,
           "projected shadow registration has dedicated telemetry");

    /* Deformation history is indexed by stable object generation plus the
     * deterministic viewport/batch ordinal—not by the transient vertex
     * pointer. It only resolves an adjacent pair with identical topology. */
    memset(deform_previous, 0x41, sizeof(deform_previous));
    memset(deform_current, 0x52, sizeof(deform_current));
    memset(deform_skipped, 0x63, sizeof(deform_skipped));
    owner = make_owner(next_matrix, 19u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    gfx_presentation_packet_capture_begin(11u);
    expect(!gfx_presentation_packet_capture_deformation(
               &owner, 2, 2u, deform_previous, sizeof(deform_previous),
               UINT32_MAX, 2u) &&
               !gfx_presentation_packet_capture_deformation(
                   &owner, 2, 2u, deform_previous, sizeof(deform_previous),
                   UINT32_MAX, UINT32_MAX),
           "hostile deformation count-stride products fail closed");
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_previous, sizeof(deform_previous),
               2u, 10u),
           "first deformation batch captures by value");
    gfx_presentation_packet_freeze();
    memset(&deformation, 0, sizeof(deformation));
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 11u, 2u, 10u, &deformation),
           "one captured tick cannot manufacture deformation history");

    gfx_presentation_packet_capture_begin(12u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_current, sizeof(deform_current),
               2u, 10u),
           "second deformation batch captures");
    gfx_presentation_packet_freeze();
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 12u, 2u, 10u, &deformation) &&
               deformation.byte_size == sizeof(deform_current) &&
               memcmp(deformation.previous_bytes, deform_previous,
                      sizeof(deform_previous)) == 0 &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "adjacent compatible deformation pair resolves exact retained bytes");
    /* The host counter may advance before this older task is walked. Lookup
     * uses the immutable task token, not that mutable counter. */
    simulated_live_tick = 99u;
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, 12u, 2u, 10u, &deformation) &&
               deformation.previous_bytes == deformation.current_bytes &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "task-authored token resolves after the live counter advances");
    expect(!gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, simulated_live_tick, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 11u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 13u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 4u, 12u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 12u, 1u, 10u, &deformation),
           "retained hold refuses wrong task tokens and nonmatching topology");
    expect(!gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, 12u, UINT32_MAX, UINT32_MAX, &deformation),
           "retained hold rejects hostile count-stride products");
    gfx_presentation_packet_note_deformation_override();
    gfx_presentation_packet_note_particle_deformation(false);
    gfx_presentation_packet_note_particle_deformation(true);
    gfx_presentation_packet_note_deformation_color(false, false);
    gfx_presentation_packet_note_deformation_color(false, true);
    gfx_presentation_packet_note_deformation_color(true, true);
    gfx_presentation_packet_note_primitive_alpha(false, 200u, 200u);
    gfx_presentation_packet_note_primitive_alpha(false, 200u, 201u);
    gfx_presentation_packet_note_primitive_alpha(true, 200u, 190u);
    gfx_presentation_packet_note_projected_shadow_primitive_alpha(false);
    gfx_presentation_packet_note_projected_shadow_primitive_alpha(true);
    gfx_presentation_packet_note_phase_hold(false);
    gfx_presentation_packet_note_endpoint_semantic(
        deform_current, deform_current, sizeof(deform_current));
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 4u, 12u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &owner, 2, 3u, 12u, 1u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &owner, 2, 3u, 13u, 2u, 10u, &deformation),
           "ordinal, topology and tick mismatches fail closed");

    gfx_presentation_packet_capture_begin(14u); /* intentionally skip 13 */
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "post-gap deformation batch still captures");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 14u, 2u, 10u, &deformation),
           "a skipped tick is never interpolated across");

    gfx_presentation_packet_capture_begin(15u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               !gfx_presentation_packet_capture_deformation(
                   &owner, 2, 3u, deform_skipped, sizeof(deform_skipped),
                   2u, 10u),
           "duplicate stable deformation key is detected, not overwritten");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 15u, 2u, 10u, &deformation),
           "ambiguous deformation key is poisoned for replay");

    owner = make_owner(particle_vertex, 21u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES;
    gfx_presentation_packet_capture_begin(16u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &owner, 0, 0u, deform_current, sizeof(deform_current),
                   2u, 10u),
           "identical multi-viewport particle submissions are idempotent");
    expect(!gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "conflicting repeated particle bytes still poison the key");
    gfx_presentation_packet_freeze();

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.deformation_registrations == 5u &&
               stats.deformation_hits == 1u &&
               stats.deformation_holds == 1u &&
               stats.deformation_phase_holds == 1u &&
               stats.deformation_overrides == 1u &&
               stats.deformation_misses == 6u &&
               stats.deformation_collisions == 2u &&
               stats.particle_deformation_hits == 2u &&
               stats.particle_deformation_overrides == 1u &&
               stats.deformation_color_hits == 3u &&
               stats.deformation_color_overrides == 2u &&
               stats.particle_color_hits == 1u &&
               stats.particle_color_overrides == 1u &&
               stats.primitive_alpha_hits == 3u &&
               stats.primitive_alpha_overrides == 2u &&
               stats.projected_shadow_primitive_alpha_hits == 2u &&
               stats.projected_shadow_primitive_alpha_overrides == 1u &&
               stats.particle_primitive_alpha_hits == 1u &&
               stats.particle_primitive_alpha_overrides == 1u &&
               stats.endpoint_vertex_checks == 1u &&
               stats.endpoint_vertex_mismatches == 0u &&
               stats.endpoint_expected_hash != 0u &&
               stats.endpoint_expected_hash == stats.endpoint_actual_hash &&
               stats.deformation_peak == 1u,
           "deformation census reports captures, exact hits and refusals");

    /* A shield and magnet can be drawn around the same racer in one tick.
     * Their stable recipe therefore has two lifetimes: the primary racer and
     * the secondary effect object. The secondary key must keep those histories
     * independent even at the same viewport and ordinal. */
    memset(effect_previous_a, 0x71, sizeof(effect_previous_a));
    memset(effect_previous_b, 0x72, sizeof(effect_previous_b));
    memset(effect_current_a, 0x81, sizeof(effect_current_a));
    memset(effect_current_b, 0x82, sizeof(effect_current_b));
    effect_a = make_owner(next_matrix, 30u);
    effect_a.matrix_class = GFX_PRESENTATION_MATRIX_EFFECT;
    effect_a.secondary_address = matrix;
    effect_a.secondary_generation = 31u;
    effect_b = effect_a;
    effect_b.secondary_address = vertex;
    effect_b.secondary_generation = 32u;

    gfx_presentation_packet_capture_begin(17u);
    expect(gfx_presentation_packet_capture_deformation(
               &effect_a, 0, 0u, effect_previous_a,
               sizeof(effect_previous_a), 1u, sizeof(effect_previous_a)) &&
               gfx_presentation_packet_capture_deformation(
                   &effect_b, 0, 0u, effect_previous_b,
                   sizeof(effect_previous_b), 1u, sizeof(effect_previous_b)),
           "effect history accepts two secondary identities on one racer");
    gfx_presentation_packet_freeze();

    gfx_presentation_packet_capture_begin(18u);
    expect(gfx_presentation_packet_capture_deformation(
               &effect_a, 0, 0u, effect_current_a,
               sizeof(effect_current_a), 1u, sizeof(effect_current_a)) &&
               gfx_presentation_packet_capture_deformation(
                   &effect_b, 0, 0u, effect_current_b,
                   sizeof(effect_current_b), 1u, sizeof(effect_current_b)),
           "effect history retains both secondary identities on the next tick");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_deformation(
               &effect_a, 0, 0u, 18u, 1u, sizeof(effect_current_a),
               &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_a,
                      sizeof(effect_previous_a)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_a,
                      sizeof(effect_current_a)) == 0,
           "effect history resolves the first secondary identity exactly");
    gfx_presentation_packet_note_effect_override();
    gfx_presentation_packet_note_phase_hold(true);
    expect(gfx_presentation_packet_lookup_deformation(
               &effect_b, 0, 0u, 18u, 1u, sizeof(effect_current_b),
               &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_b,
                      sizeof(effect_previous_b)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_b,
                      sizeof(effect_current_b)) == 0,
           "effect history resolves the second secondary identity exactly");
    gfx_presentation_packet_note_effect_override();

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.deformation_registrations == 5u &&
               stats.deformation_hits == 1u &&
               stats.deformation_holds == 1u &&
               stats.deformation_phase_holds == 1u &&
               stats.deformation_overrides == 1u &&
               stats.deformation_misses == 6u &&
               stats.deformation_collisions == 2u &&
               stats.effect_registrations == 4u &&
               stats.effect_hits == 2u && stats.effect_overrides == 2u &&
               stats.effect_phase_holds == 1u &&
               stats.effect_misses == 0u && stats.effect_collisions == 0u &&
               stats.endpoint_vertex_checks == 1u &&
               stats.endpoint_vertex_mismatches == 0u &&
               stats.endpoint_expected_hash == stats.endpoint_actual_hash &&
               stats.deformation_peak == 2u,
           "effect census is separate and two-identity histories do not collide");

    /* A future-only publication advances exact deformation history without
     * replacing the matrix/vertex ownership packet for the task being
     * replayed. The lookup key remains the real-walk address, while immutable
     * bytes from a private retained arena drive stale-tenancy validation. */
    memset(retained_identity, 0x91, sizeof(retained_identity));
    memcpy(retained_observation, retained_identity,
           sizeof(retained_observation));
    memset(future_previous, 0xA1, sizeof(future_previous));
    memset(future_current, 0xB2, sizeof(future_current));
    owner = make_owner(retained_identity, 40u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    expect(gfx_presentation_packet_register_matrix(
               retained_identity, sizeof(retained_identity), 1, &owner),
           "forward-pair control matrix registers");
    gfx_presentation_packet_capture_begin(19u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, future_previous, sizeof(future_previous),
               2u, 10u),
           "task T deformation captures before the ordinary freeze");
    gfx_presentation_packet_freeze();
    retained_identity[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix_observed(
               retained_identity, retained_observation, &binding) &&
               !binding.stale,
           "private observed bytes validate under the original ownership key");

    gfx_presentation_packet_capture_begin(20u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, future_current, sizeof(future_current),
               2u, 10u) &&
               gfx_presentation_packet_publish_deformation(),
           "already-authored task T+1 publishes deformation only");
    gfx_presentation_packet_note_future_capture(true);
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 1, 7u, 20u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, future_previous,
                      sizeof(future_previous)) == 0 &&
               memcmp(deformation.current_bytes, future_current,
                      sizeof(future_current)) == 0,
           "future publication exposes the exact adjacent T/T+1 pair");
    gfx_presentation_packet_capture_begin(21u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "failed-scan control stages a partial future task");
    gfx_presentation_packet_capture_abort();
    expect(!gfx_presentation_packet_publish_deformation() &&
               gfx_presentation_packet_lookup_deformation(
                   &owner, 1, 7u, 20u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, future_previous,
                      sizeof(future_previous)) == 0 &&
               memcmp(deformation.current_bytes, future_current,
                      sizeof(future_current)) == 0,
           "aborted future scan leaves the last complete pair intact");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix_observed(
               retained_identity, retained_observation, &binding) &&
               !binding.stale,
           "future publication leaves the replay task's owner packet intact");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.future_captures == 1u && stats.future_failures == 0u,
           "future publication has explicit success telemetry");

    /* A racer's projected decal and its first model batch can both be ordinal
     * zero in viewport zero. Matrix class and topology signature namespace the
     * histories so neither poisons the other. */
    owner = make_owner(next_matrix, 22u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    shadow_owner = owner;
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x123456789abcdef0);
    gfx_presentation_packet_capture_begin(22u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_previous, sizeof(deform_previous),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &shadow_owner, 0, 0u, effect_previous_a,
                   sizeof(effect_previous_a), 2u, 10u),
           "model and projected-shadow ordinal zero capture independently");
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_capture_begin(23u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &shadow_owner, 0, 0u, effect_current_a,
                   sizeof(effect_current_a), 2u, 10u),
           "both same-ordinal histories advance on the adjacent tick");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 0, 0u, 23u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, deform_previous,
                      sizeof(deform_previous)) == 0 &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "model ordinal zero resolves its own forward pair");
    expect(gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 0u, 23u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_a,
                      sizeof(effect_previous_a)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_a,
                      sizeof(effect_current_a)) == 0,
           "projected-shadow ordinal zero resolves its own forward pair");
    shadow_owner.geometry_signature++;
    expect(!gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 0u, 23u, 2u, 10u, &deformation),
           "projected-shadow topology changes refuse interpolation");

    /* A receiver topology change between adjacent authored ticks must not
     * borrow the prior decal batch, even when its object and ordinal match. */
    shadow_owner = make_owner(next_matrix, 23u);
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x1111111111111111);
    future_shadow_owner = shadow_owner;
    future_shadow_owner.geometry_signature = UINT64_C(0x2222222222222222);
    gfx_presentation_packet_capture_begin(24u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 1u, effect_previous_a,
               sizeof(effect_previous_a), 2u, 10u),
           "projected-shadow topology A captures at task T");
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_capture_begin(25u);
    expect(gfx_presentation_packet_capture_deformation(
               &future_shadow_owner, 0, 1u, effect_current_a,
               sizeof(effect_current_a), 2u, 10u),
           "projected-shadow topology B captures at task T+1");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 1u, 25u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &future_shadow_owner, 0, 1u, 25u, 2u, 10u,
                   &deformation),
           "adjacent projected-shadow topology signatures never form a pair");

    /* Future direct-shadow VTX registrations use a fresh heap address, but
     * publishing their deformation history must not replace T's frozen owner
     * binding while that task is still being presented. */
    memset(projected_shadow_vertex, 0xC3, sizeof(projected_shadow_vertex));
    memset(projected_shadow_future_vertex, 0xD4,
           sizeof(projected_shadow_future_vertex));
    shadow_owner = make_owner(projected_shadow_vertex, 24u);
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x3333333333333333);
    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_vertex, 0, 6u, &shadow_owner),
           "task T projected-shadow VTX binding registers");
    gfx_presentation_packet_capture_begin(26u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 6u, effect_previous_b,
               sizeof(effect_previous_b), 2u, 10u),
           "task T projected-shadow deformation captures");
    gfx_presentation_packet_freeze();
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.owner.generation == shadow_owner.generation &&
               binding.owner.geometry_signature ==
                   shadow_owner.geometry_signature &&
               binding.ordinal == 6u,
           "task T projected-shadow binding freezes by value");

    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_future_vertex, 0, 6u, &shadow_owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   projected_shadow_future_vertex, &binding),
           "task T+1 projected-shadow VTX uses its new live address");
    gfx_presentation_packet_capture_begin(27u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 6u, effect_current_b,
               sizeof(effect_current_b), 2u, 10u) &&
               gfx_presentation_packet_publish_deformation(),
           "task T+1 projected-shadow deformation publishes without a freeze");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.owner.generation == shadow_owner.generation &&
               binding.owner.geometry_signature ==
                   shadow_owner.geometry_signature &&
               binding.ordinal == 6u &&
               !gfx_presentation_packet_lookup_vertex(
                   projected_shadow_future_vertex, &binding),
           "future projected-shadow publication preserves task T's frozen binding");

    owner = make_owner(projected_shadow_vertex, 22u);
    owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    gfx_presentation_packet_capture_begin(21u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 5u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &owner, 0, 5u, deform_current, sizeof(deform_current),
                   2u, 10u),
           "identical multi-viewport projected-shadow submissions are idempotent");
    gfx_presentation_packet_note_projected_shadow_deformation(true);
    gfx_presentation_packet_capture_abort();
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.projected_shadow_deformation_hits == 1u &&
               stats.projected_shadow_deformation_overrides == 1u,
           "projected shadow interpolation has dedicated telemetry");

    /* An authoring pass that never submits its list. The bindings it left on
     * the live side describe a list nothing will ever walk, so the next
     * authoring lifetime drops them -- otherwise they survive into the next
     * freeze still stamped with the older tick and replay measures their
     * residual against a newer pose. The frozen side, which belongs to the
     * last list that WAS walked, must be untouched by the discard. */
    memset(matrix, 0x44, sizeof(matrix));
    memset(next_matrix, 0x55, sizeof(next_matrix));
    owner = make_owner(matrix, 31u);
    owner.capture_tick = 100u;
    expect(gfx_presentation_packet_register_matrix(
               matrix, sizeof(matrix), 0, &owner),
           "a list that will be walked registers on the live side");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding) &&
               binding.owner.capture_tick == 100u,
           "the walked list's binding freezes with its capture tick");

    owner = make_owner(next_matrix, 32u);
    owner.capture_tick = 101u;
    expect(gfx_presentation_packet_register_matrix(
               next_matrix, sizeof(next_matrix), 0, &owner),
           "an abandoned authoring pass still registers on the live side");
    owner.matrix_class = GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES;
    expect(gfx_presentation_packet_register_vertex_identity(
               particle_vertex, 0, &owner),
           "the same abandoned pass registers its vertex recipes too");
    gfx_presentation_packet_discard_live_registrations();
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding),
           "discarding live registrations leaves the frozen packet intact");
    expect(!gfx_presentation_packet_has_live_vertex(particle_vertex),
           "discarded live vertex registrations are gone before the freeze");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_matrix(next_matrix, &binding) &&
               !gfx_presentation_packet_lookup_vertex(
                   particle_vertex, &binding),
           "an abandoned pass's bindings cannot reach the next frozen packet");

    gfx_presentation_packet_invalidate();
    expect(!gfx_presentation_packet_frozen() &&
               !gfx_presentation_packet_lookup_matrix(next_matrix, &binding),
           "lifecycle invalidation makes retained dependencies unavailable");
    gfx_presentation_packet_shutdown();
    memset(&stats, 0xFF, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.matrix_registrations == 0u && stats.freezes == 0u,
           "shutdown releases storage and resets ownership telemetry");

    check_uv_scroll();
    check_uv_scroll_authored();
    check_shadow_reorder_detection();

    if (failures != 0) {
        fprintf(stderr, "presentation_packet: %d failure(s)\n", failures);
        return 1;
    }
    puts("presentation_packet: all checks passed");
    return 0;
}
