/* harness/input.c – Shared input state, no libretro dependencies.
 *
 * Polls SDL events once per frame and feeds the SINGLE input path in hw.c
 * (hw_handle_key). Both PUAE (via input_state_cb, which reads input_up/… below)
 * and PC (via the same hw state) therefore see identical input — there is no
 * separate harness key mapping. */
#include "harness/input.h"
#include "engine/hw.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

void input_poll(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
        if (hw_handle_sdl_event(&ev)) continue;   /* controllers, window resize */
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            int d = (ev.type == SDL_KEYDOWN);
            /* ESC is handled inside hw_handle_key — it toggles the pause
             * menu in gameplay and falls back to exit(0) elsewhere. */
            hw_handle_key(ev.key.keysym.sym, d);
        }
    }
}

/* PUAE reads these via input_state_cb; they mirror the shared hw input state. */
int input_up(void)    { return hw_joy_up(); }
int input_down(void)  { return hw_joy_down(); }
int input_left(void)  { return hw_joy_left(); }
int input_right(void) { return hw_joy_right(); }
int input_fire(void)  { return hw_get_fire(); }
int input_space(void) { return hw_get_mouse_lmb(); }
int input_esc(void)   { return 0; }

/* Programmatically force the action button (for headless harness driving).
 * Sets both joystick fire and the port-0/mouse button, since the title's
 * start condition reads port-0 fire ($BFE001 bit 7 = mouse/left button). */
void input_force_fire(int on)
{
    hw_set_fire(on ? 1 : 0);
    hw_set_mouse_lmb(on ? 1 : 0);
    hw_set_fire_vanilla(on ? 1 : 0);   /* forced fire keeps vanilla interact semantics */
}

/* Force the joystick directions, for headless harness driving. Preserves the
 * current fire state (read back from hw) so a direction change doesn't drop
 * a held fire. */
void input_force_dir(int up, int down, int left, int right)
{
    hw_set_joystick(up, down, left, right, hw_get_fire());
}
