/* harness/input.h – Shared input polling for side-by-side mode.
 * Does NOT include libretro headers, so no macro pollution. */
#pragma once
#include <stdint.h>

/* Call once per frame to poll SDL events and update shared state.
 * Must be called before both PUAE and PC run for the frame. */
void input_poll(void);

/* Input state accessors (used by PUAE libretro callbacks and PC hw.c) */
int  input_up(void);
int  input_down(void);
int  input_left(void);
int  input_right(void);
int  input_fire(void);
int  input_space(void);
int  input_esc(void);
void input_force_fire(int on);  /* programmatic fire override (headless driving) */
