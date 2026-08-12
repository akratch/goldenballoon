#include "joypad.h"

#ifdef NATIVE_PORT
void input_rollback_compute_edges(
    const MdkrInputSample *current, const MdkrInputSample *previous,
    u16 button_mask, u16 *pressed, u16 *released) {
    u16 current_buttons;
    u16 previous_buttons;
    if (pressed == NULL || released == NULL) return;
    *pressed = 0u;
    *released = 0u;
    if (current == NULL || previous == NULL) return;
    current_buttons = current->present ? current->buttons : 0u;
    previous_buttons = previous->present ? previous->buttons : 0u;
    *pressed = ((current_buttons ^ previous_buttons) & current_buttons) &
        button_mask;
    *released = ((current_buttons ^ previous_buttons) & previous_buttons) &
        button_mask;
}
#endif
