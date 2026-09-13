/* Android on-screen controls.  Presentation happens at output resolution from
 * the SDL present backend (present_backend_set_frame_overlay), not into the
 * game's low-resolution frame, and the shared touch-ui subsystem owns the
 * layout, the hit regions, and the contact claiming.  This file supplies only
 * the title's part: which controls exist and what each action means. */
#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SDL event pump hook. Returns 1 when the on-screen controls consumed the
 * event: a touch inside a control is never also a tap for the game. */
int touch_controls_handle_sdl_event(const SDL_Event *event);

/* Draw the controls over the presented frame. Registered with the present
 * backend by hw.c; a backend without an SDL renderer never calls it. */
void touch_controls_present(SDL_Renderer *renderer, SDL_Window *window);

#ifdef __cplusplus
}
#endif
