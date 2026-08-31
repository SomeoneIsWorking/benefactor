/* Android virtual-control surface.  The implementation is Android-only; the
 * C interface keeps SDL event routing and presentation owned by hw.c. */
#pragma once

#include <SDL2/SDL_events.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int touch_controls_handle_sdl_event(const SDL_Event *event);
void touch_controls_set_controller_connected(int connected);
void touch_controls_draw(uint32_t *argb, int width, int height);

#ifdef __cplusplus
}
#endif
