#include "camera_dynamic_publication.h"

#ifdef NATIVE_PORT

#include <stddef.h>
#include <string.h>

void mdkr_camera_dynamic_publication_reset(
    MdkrCameraDynamicPublicationState *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int mdkr_camera_dynamic_publication_begin(
    MdkrCameraDynamicPublicationState *state) {
    int recovery_discontinuity;

    if (state == NULL) {
        return 0;
    }
    recovery_discontinuity = state->attempted && !state->current_valid;
    state->previous_valid = state->current_valid;
    state->current_valid = 0U;
    state->attempted = 1U;
    return recovery_discontinuity;
}

void mdkr_camera_dynamic_publication_finish(
    MdkrCameraDynamicPublicationState *state,
    int complete) {
    if (state != NULL) {
        state->current_valid = state->attempted && complete != 0;
    }
}

int mdkr_camera_dynamic_publication_current_valid(
    const MdkrCameraDynamicPublicationState *state) {
    return state != NULL && state->current_valid;
}

int mdkr_camera_dynamic_publication_previous_valid(
    const MdkrCameraDynamicPublicationState *state) {
    return state != NULL && state->previous_valid;
}

int mdkr_camera_dynamic_publication_requires_global_cut(
    const MdkrCameraDynamicPublicationState *state) {
    return state != NULL && state->attempted && !state->current_valid;
}

#endif /* NATIVE_PORT */
