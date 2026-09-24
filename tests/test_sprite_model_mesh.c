/* Synthetic geometry only. No ROM or replacement art is needed for this test. */
#include "sprite_model_mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int yes, const char *message) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static int close_to(float a, float b) { return fabsf(a-b) < .001f; }
static void u32(unsigned char *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void f32(unsigned char *p, float v) {
    uint32_t word; memcpy(&word, &v, 4); u32(p, word);
}
static void test_format(void) {
    unsigned char bytes[124] = {0}; /* header, three 32-byte vertices, three indices */
    struct GfxModernMesh mesh = {0};
    memcpy(bytes, "DKRPM001", 8); u32(bytes+8, 3); u32(bytes+12, 3);
    f32(bytes+16+32, .5f); f32(bytes+16+64+4, 1.0f);
    u32(bytes+116, 1); u32(bytes+120, 2);
    check(mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "valid little-endian triangle");
    check(mesh.vtx_count == 3 && mesh.idx_count == 3 && mesh.cutout,
          "mesh has triangle counts and alpha cutout");
    {
        const float *owned = mesh.vtx;
        check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh) && mesh.vtx == owned,
              "refuses to overwrite owned geometry");
    }
    if (mesh.vtx) {
        const float *v = mesh.vtx;
        const unsigned char *rgba = (const unsigned char *)&v[8];
        check(v[9] == .5f && v[19] == 1.0f, "converts 32-byte vertices to renderer stride");
        check(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 && rgba[3] == 0,
              "baked base color is not tinted or covered with snow");
    }
    mdkr_sprite_model_mesh_free(&mesh);
    check(!mesh.vtx && !mesh.idx, "free clears ownership");
    mdkr_sprite_model_mesh_free(&mesh);
    {
        size_t length;
        for (length = 0; length < sizeof bytes; ++length) {
            /* Allocate only the advertised prefix so ASan can detect reads
             * beyond it, rather than hiding them inside the complete fixture. */
            unsigned char *prefix = malloc(length ? length : 1);
            check(prefix != NULL, "allocate bounded truncation fixture");
            if (!prefix) continue;
            memcpy(prefix, bytes, length);
            check(!mdkr_sprite_model_parse(prefix, length, &mesh),
                  "every truncated mesh prefix is rejected");
            check(!mesh.vtx && !mesh.idx && !mesh.vtx_count && !mesh.idx_count,
                  "truncated parse leaves empty output unchanged");
            mdkr_sprite_model_mesh_free(&mesh);
            free(prefix);
        }
        {
            unsigned char extended[sizeof bytes + 1];
            memcpy(extended, bytes, sizeof bytes);
            extended[sizeof bytes] = 0;
            check(!mdkr_sprite_model_parse(extended, sizeof extended, &mesh),
                  "trailing payload is rejected rather than ignored");
            mdkr_sprite_model_mesh_free(&mesh);
        }
    }
    check(!mdkr_sprite_model_parse(NULL, sizeof bytes, &mesh), "rejects null data");
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, NULL), "rejects null output");
    check(!mdkr_sprite_model_parse(bytes, 15, &mesh), "rejects short header");
    u32(bytes+12, 2);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects non-triangle index count");
    u32(bytes+12, 3);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes-1, &mesh), "rejects truncated index");
    bytes[0] = 'X';
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects other formats");
    bytes[0] = 'D'; u32(bytes+8, 0xffffffff);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects excessive vertex count");
    u32(bytes+8, 3); u32(bytes+120, 3);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects out-of-range index");
    u32(bytes+120, 2); f32(bytes+16, NAN);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects nonfinite geometry");
    f32(bytes+16, 0); f32(bytes+16+12, 4);
    check(!mdkr_sprite_model_parse(bytes, sizeof bytes, &mesh), "rejects invalid normal");
    check(!mesh.vtx && !mesh.idx, "rejected inputs do not leave partial meshes");
}
static void test_placement(void) {
    /* Dimensions/foot positions from the source port's tuned model placements. */
    static const float placements[][5] = {
        {107,2,122,122,-6}, {108,2,113,113,0}, {113,2,115,115,0},
        {114,2,139,139,-69.5}, {115,2,66,66,2}, {116,2,78,60,-30},
        {147,17,43,66,4}, {148,17,43,66,6}, {149,17,43,66,6},
        {150,17,43,66,6}, {151,17,43,66,6}, {154,77,43,48,22},
        {155,77,43,50,21}, {156,32,50,50,9}
    };
    MdkrSpriteModelPose p = {.position={100,200,300}, .scale=2};
    float m[4][4]; unsigned i;
    for (i=0; i<sizeof placements/sizeof placements[0]; ++i) {
        const float *v = placements[i];
        p.sprite = (unsigned)v[0];
        check(mdkr_sprite_model_supported(p.sprite,1,(int)v[1],(int)v[1]), "admits actual world sprite");
        check(!mdkr_sprite_model_supported(p.sprite,0,(int)v[1],(int)v[1]), "rejects non-billboard object");
        check(!mdkr_sprite_model_supported(p.sprite,1,(int)v[1],99), "rejects wrong object behavior");
        check(mdkr_sprite_model_matrix(&p,m) && close_to(m[0][0],v[2]*2) &&
              close_to(m[1][1],v[3]*2) && close_to(m[3][1],200+v[4]*2) &&
              m[3][0] == 100 && m[3][2] == 300 && m[3][3] == 1,
              "preserves source dimensions and foot placement");
    }
    check(mdkr_sprite_model_supported(154,1,50,50), "admits cutscene gold balloon");
    check(!mdkr_sprite_model_supported(155,1,50,50), "rejects cutscene silver mismatch");
    check(!mdkr_sprite_model_supported(81,1,77,77) &&
          !mdkr_sprite_model_supported(82,1,77,77) &&
          !mdkr_sprite_model_supported(176,1,17,17), "keeps HUD icons and balloon burst sprites");
    p.sprite=156; p.phase=64;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[0][0],0) && close_to(m[0][2],-100),
          "banana phase rotates around world up");
    p.phase=256;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[0][0],100) && close_to(m[0][2],0),
          "interpolated banana wrap is continuous");
    p.phase=0; p.rotation[0]=16384;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[0][2],-100), "object yaw is preserved");
    p.rotation[0]=0; p.rotation[1]=16384;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[1][2],100), "object pitch is preserved");
    p.rotation[1]=0; p.rotation[2]=16384;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[0][1],100), "object roll is preserved");
    p.rotation[2]=0; p.sprite=114; p.scale=1; p.attached=1;
    p.attachment[0]=10; p.attachment[1]=20; p.attachment[2]=30;
    check(mdkr_sprite_model_matrix(&p,m) && close_to(m[3][0],11.39f) &&
          close_to(m[3][1],-52.5f) && close_to(m[3][2],29.6525f),
          "attached canopy uses bark tip and tuned offsets");
    p.scale=NAN;
    check(!mdkr_sprite_model_matrix(&p,m), "rejects invalid transform");
}
static void test_attachment(void) {
    const float anchor[3]={0,100,0}; float tip[3];
    const float vertices[]={0,99,0, 0,99,0, -2,100,0, 2,100,0, 1000,1000,1000};
    const float scattered[]={-30,100,0, 30,100,0, 0,130,0};
    const float signed_zero_duplicates[]={0.0f,100,0.0f, -0.0f,100,0.0f,
                                           0.0f,100,-0.0f};
    check(mdkr_sprite_model_trunk_tip(vertices,5,anchor,tip) &&
          close_to(tip[0],0) && close_to(tip[1],99.666667f),
          "trunk attachment ignores duplicates and remote vertices");
    check(!mdkr_sprite_model_trunk_tip(vertices,2,anchor,tip), "does not attach to duplicate-only tip");
    check(!mdkr_sprite_model_trunk_tip(signed_zero_duplicates,3,anchor,tip),
          "signed zero variants are not three distinct trunk vertices");
    check(!mdkr_sprite_model_trunk_tip(scattered,3,anchor,tip), "does not bridge separate trunks");
    /* The runtime feeds this incrementally across level batches. Invalid
     * vertices must not poison the retained nearest points, and batch order
     * must not change a tip when the nearest set is unambiguous. */
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        MdkrSpriteModelTrunkCandidates candidates;
        const float invalid[][3] = {{NAN,100,0}, {0,INFINITY,0}, {0,100,-INFINITY}};
        mdkr_sprite_model_trunk_candidates_init(&candidates);
        for (unsigned k = 0; k < 5; ++k) {
            unsigned index = reverse ? 4-k : k;
            mdkr_sprite_model_trunk_candidates_add(&candidates, vertices + index*3, anchor);
            for (unsigned bad = 0; bad < 3; ++bad)
                mdkr_sprite_model_trunk_candidates_add(&candidates, invalid[bad], anchor);
        }
        check(mdkr_sprite_model_trunk_candidates_finish(&candidates, tip) &&
              close_to(tip[0],0) && close_to(tip[1],99.666667f),
              "streaming trunk scan ignores nonfinite input and batch order");
    }
    {
        const float radius_edge[] = {-1,99,0, 1,99,0, 0,0,0};
        const float just_outside[] = {-1,99,0, 1,99,0, 0,-0.01f,0};
        MdkrSpriteModelTrunkCandidates candidates;
        mdkr_sprite_model_trunk_candidates_init(&candidates);
        for (unsigned k = 0; k < 3; ++k)
            mdkr_sprite_model_trunk_candidates_add(&candidates, radius_edge+k*3, anchor);
        check(candidates.found == 3,
              "trunk search includes exact 100-unit radius boundary");
        mdkr_sprite_model_trunk_candidates_init(&candidates);
        for (unsigned k = 0; k < 3; ++k)
            mdkr_sprite_model_trunk_candidates_add(&candidates, just_outside+k*3, anchor);
        check(candidates.found == 2,
              "trunk search rejects points beyond its radius");
    }
}
int main(void) {
    test_format(); test_placement(); test_attachment();
    if (failures) return 1;
    puts("sprite_model_mesh: all checks passed"); return 0;
}
