#ifndef MDKR_CAMERA_DYNAMIC_PUBLICATION_H
#define MDKR_CAMERA_DYNAMIC_PUBLICATION_H

#ifdef NATIVE_PORT

#include <stdint.h>

typedef struct MdkrCameraDynamicPublicationState {
    uint8_t attempted;
    uint8_t current_valid;
    uint8_t previous_valid;
} MdkrCameraDynamicPublicationState;

void mdkr_camera_dynamic_publication_reset(
    MdkrCameraDynamicPublicationState *state);

/* Begins an invalid-until-proven publication. Returns true when the prior tick
 * failed and the first recovered object snapshot must cut interpolation. */
int mdkr_camera_dynamic_publication_begin(
    MdkrCameraDynamicPublicationState *state);

void mdkr_camera_dynamic_publication_finish(
    MdkrCameraDynamicPublicationState *state,
    int complete);

int mdkr_camera_dynamic_publication_current_valid(
    const MdkrCameraDynamicPublicationState *state);
int mdkr_camera_dynamic_publication_previous_valid(
    const MdkrCameraDynamicPublicationState *state);

/* A completed invalid attempt has no authoritative object interpolation source. */
int mdkr_camera_dynamic_publication_requires_global_cut(
    const MdkrCameraDynamicPublicationState *state);

#endif /* NATIVE_PORT */

#endif /* MDKR_CAMERA_DYNAMIC_PUBLICATION_H */
