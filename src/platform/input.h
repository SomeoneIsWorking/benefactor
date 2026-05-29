#pragma once
/*
 * platform/input.h  –  SDL2 keyboard / joystick input.
 *
 * Maps SDL2 key events to Amiga keyboard scancodes and joystick signals.
 * Benefactor uses joystick port 2 (CIA-A PRA bits 6–7) for the player.
 */

#include <stdint.h>
#include "SDL2/SDL.h"

void input_init(void);
void input_fini(void);

/* Process one SDL_Event.  Returns 1 if the event was a quit request. */
int  input_handle_event(const SDL_Event *ev);

/* Poll the current key state and push joystick / keyboard updates to CIA. */
void input_update(void);
