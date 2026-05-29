#include "input.h"
#include "../amiga/cia.h"
#include "../amiga/custom.h"

#include <stdint.h>
#include <string.h>

/*
 * Amiga keyboard raw scancodes (subset used in Benefactor).
 * Source: Amiga Hardware Reference Manual, Appendix B.
 */
#define AK_ESCAPE  0x45
#define AK_F1      0x50
#define AK_F2      0x51
#define AK_F3      0x52
#define AK_F4      0x53
#define AK_F5      0x54
#define AK_F6      0x55
#define AK_F7      0x56
#define AK_F8      0x57
#define AK_F9      0x58
#define AK_F10     0x59
#define AK_HELP    0x5F
#define AK_DEL     0x46
#define AK_RETURN  0x44
#define AK_SPACE   0x40
#define AK_UP      0x4C
#define AK_DOWN    0x4D
#define AK_LEFT    0x4F
#define AK_RIGHT   0x4E
#define AK_LSHIFT  0x60
#define AK_RSHIFT  0x61
#define AK_CTRL    0x63
#define AK_LALT    0x64
#define AK_RALT    0x65
#define AK_LAMIGA  0x66
#define AK_RAMIGA  0x67
#define AK_P       0x19
#define AK_S       0x21

/* ── SDL scancode → Amiga keycode table ──────────────────────────────────── */

static uint8_t sdl_to_amiga(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:    return AK_ESCAPE;
    case SDL_SCANCODE_F1:        return AK_F1;
    case SDL_SCANCODE_F2:        return AK_F2;
    case SDL_SCANCODE_F3:        return AK_F3;
    case SDL_SCANCODE_F4:        return AK_F4;
    case SDL_SCANCODE_F5:        return AK_F5;
    case SDL_SCANCODE_F6:        return AK_F6;
    case SDL_SCANCODE_F7:        return AK_F7;
    case SDL_SCANCODE_F8:        return AK_F8;
    case SDL_SCANCODE_F9:        return AK_F9;
    case SDL_SCANCODE_F10:       return AK_F10;
    case SDL_SCANCODE_DELETE:    return AK_DEL;
    case SDL_SCANCODE_RETURN:    return AK_RETURN;
    case SDL_SCANCODE_SPACE:     return AK_SPACE;
    case SDL_SCANCODE_UP:        return AK_UP;
    case SDL_SCANCODE_DOWN:      return AK_DOWN;
    case SDL_SCANCODE_LEFT:      return AK_LEFT;
    case SDL_SCANCODE_RIGHT:     return AK_RIGHT;
    case SDL_SCANCODE_LSHIFT:    return AK_LSHIFT;
    case SDL_SCANCODE_RSHIFT:    return AK_RSHIFT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:     return AK_CTRL;
    case SDL_SCANCODE_LALT:      return AK_LALT;
    case SDL_SCANCODE_RALT:      return AK_RALT;
    case SDL_SCANCODE_LGUI:      return AK_LAMIGA;
    case SDL_SCANCODE_RGUI:      return AK_RAMIGA;
    default:                     return 0xFF;  /* unmapped */
    }
}

/* ── Joystick state ──────────────────────────────────────────────────────── */

static int joy_up, joy_down, joy_left, joy_right, joy_fire;

/* JOY0DAT uses quadrature encoding; for digital joystick we fake it. */
static uint16_t make_joydat(int up, int down, int left, int right)
{
    uint16_t v = 0;
    /* Vertical: bits 9:8 (Y counter).  Up = decrement, down = increment.
     * Amiga reads XOR of consecutive Y bits.  Simplification: set bit 0 of
     * the Y field to indicate motion. */
    if (up)    v |= 0x0100;
    if (down)  v |= 0x0200;
    if (left)  v |= 0x0001;
    if (right) v |= 0x0002;
    return v;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void input_init(void)
{
    joy_up = joy_down = joy_left = joy_right = joy_fire = 0;
}

void input_fini(void) {}

int input_handle_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_QUIT:
        return 1;

    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int pressed = (ev->type == SDL_KEYDOWN);
        SDL_Scancode sc = ev->key.keysym.scancode;

        /* Toggle fullscreen on Alt+Enter */
        if (pressed && sc == SDL_SCANCODE_RETURN &&
            (SDL_GetModState() & (KMOD_LALT | KMOD_RALT)))
            return 2;  /* signal caller to toggle fullscreen */

        /* Joystick emulation via cursor keys */
        switch (sc) {
        case SDL_SCANCODE_UP:    joy_up    = pressed; break;
        case SDL_SCANCODE_DOWN:  joy_down  = pressed; break;
        case SDL_SCANCODE_LEFT:  joy_left  = pressed; break;
        case SDL_SCANCODE_RIGHT: joy_right = pressed; break;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_Z:
        case SDL_SCANCODE_SPACE: joy_fire  = pressed; break;
        default: break;
        }

        /* Forward to CIA keyboard handler */
        uint8_t ak = sdl_to_amiga(sc);
        if (ak != 0xFF)
            cia_key_event(ak, pressed);
        break;
    }

    default:
        break;
    }
    return 0;
}

void input_update(void)
{
    uint16_t joy0dat = make_joydat(joy_up, joy_down, joy_left, joy_right);
    cia_joy_update(joy0dat, 0,
                   joy_fire ? 0 : 1,   /* active-low: 0 = pressed */
                   1);

    /* Also write JOY0DAT into the custom chip shadow for direct reads */
    extern void custom_write(uint32_t reg, uint16_t val);
    custom_write(JOY0DAT, joy0dat);
}
