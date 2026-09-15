/* src/port/pause_menu_draw.c — the pause menu's pixels.
 *
 * Every page is the same picture: the frame dimmed by half, a bordered panel
 * centred on it, and one row per line inside it. What the rows SAY is decided
 * in src/port/pause_menu.c and handed over as a PcPauseView snapshot (see
 * pause_menu_view.h); this file turns that into fills and glyphs and knows
 * nothing else about the menu.
 *
 * Called from hw_present_frame (src/engine/hw.c) after native_render_frame has
 * composed the game's frame, the same hook point as pc_level_select_overlay.
 */

#include "common/version.h"
#include "port/config.h"
#include "port/overlay_ui.h"
#include "port/pause_menu_view.h"
#include "port/update_check.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb);
extern int pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb);

/* Text width in pixels: the overlay font is a fixed 6px cell. */
static int text_width(const char *text) {
    return (int)strlen(text) * 6;
}

static void draw_panel(uint32_t *fb, int px, int py, int pw, int ph, const char *title) {
    pc_fill_rect(fb, px, py, pw, ph, 0xFF101830);
    pc_fill_rect(fb, px, py, pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px, py + ph - 1, pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px, py, 1, ph, 0xFFFFD040);
    pc_fill_rect(fb, px + pw - 1, py, 1, ph, 0xFFFFD040);
    pc_draw_text(fb, px + 8, py + 6, title, 1, 0xFFFFE070);
    /* Which build this is, where a player reporting a problem will see it. */
    char version[32];
    snprintf(version, sizeof version, "v%s", pc_version());
    pc_draw_text(fb, px + pw - 8 - text_width(version), py + 6, version, 1, 0xFF90A0D0);
}

static void draw_row(uint32_t *fb, int px, int y, int selected, const char *label,
                     const char *value) {
    uint32_t colour = selected ? 0xFFFFFFFF : 0xFFB0B0C0;
    if (selected) {
        pc_draw_text(fb, px + 6, y, ">", 1, 0xFFFFE070);
    }
    pc_draw_text(fb, px + 16, y, label, 1, colour);
    if (value) {
        pc_draw_text(fb, px + 150, y, value, 1, selected ? 0xFFFFE070 : 0xFF90A0D0);
    }
}

/* Like draw_row but rendered dimmed — for rows that are inert in the current
 * context (e.g. GPU effects when the HARDWARE renderer is off). */
static void draw_row_disabled(uint32_t *fb, int px, int y, int selected, const char *label,
                              const char *value) {
    if (selected) {
        pc_draw_text(fb, px + 6, y, ">", 1, 0xFF707058);
    }
    pc_draw_text(fb, px + 16, y, label, 1, 0xFF606070);
    if (value) {
        pc_draw_text(fb, px + 150, y, value, 1, 0xFF505060);
    }
}

/* Right-pointing triangle at the right edge of a row — marks a row that OPENS A
 * SUBMENU (vs one that cycles a value in place). Drawn right-aligned inside the
 * panel; `pw` is the panel width, `y` the row's text baseline. 7px tall to match
 * the glyph height; the left edge is vertical and it narrows to a tip on the
 * right (the "there's more this way" affordance). */
static void draw_submenu_arrow(uint32_t *fb, int px, int pw, int y, int selected) {
    uint32_t argb = selected ? 0xFFFFE070 : 0xFF90A0D0;
    int rx = px + pw - 12;
    for (int r = 0; r < 7; r++) {
        int wdt = (r <= 3) ? (r + 1) : (7 - r);
        pc_fill_rect(fb, rx, y + r, wdt, 1, argb);
    }
}

/* The update check's state as one line under the panel. A check that could not
 * run says so: "up to date" is a claim this port only makes when it has an
 * answer, and the player can turn the check off, in which case there is no
 * line at all. */
static void draw_update_status(uint32_t *fb) {
    if (!pc_cfg_bool("update_check", 1)) {
        return;
    }
    const char *line = pc_update_line();
    if (line == NULL || line[0] == '\0') {
        return;
    }
    const int ow = pc_overlay_w(), oh = pc_overlay_h();
    /* A failure carries its reason, which can be longer than the screen; the
     * sentence is clipped to what fits rather than drawn off the edge. */
    char clipped[64];
    const int max_chars = (ow - 16) / 6;
    snprintf(clipped, sizeof clipped, "%s", line);
    if ((int)strlen(clipped) > max_chars) {
        clipped[max_chars] = '\0';
        if (max_chars >= 3) {
            clipped[max_chars - 3] = '.';
            clipped[max_chars - 2] = '.';
            clipped[max_chars - 1] = '.';
        }
    }
    line = clipped;
    const int width = text_width(line);
    const int x = (ow - width) / 2;
    const int y = oh - 22;
    pc_fill_rect(fb, x - 6, y - 4, width + 12, 15, 0xFF101830);
    pc_fill_rect(fb, x - 6, y - 4, width + 12, 1, 0xFF35516A);
    pc_fill_rect(fb, x - 6, y + 10, width + 12, 1, 0xFF35516A);
    const PcUpdateState state = pc_update_state();
    uint32_t colour = 0xFF90A0D0;
    if (state == PC_UPDATE_AVAILABLE) {
        colour = 0xFFFFD040;
    } else if (state == PC_UPDATE_FAILED) {
        colour = 0xFFE08080;
    } else if (state == PC_UPDATE_CURRENT) {
        colour = 0xFF80D090;
    }
    pc_draw_text(fb, x, y, line, 1, colour);
}

/* Panel width per page. Each is sized for the widest thing its page draws, so
 * the number belongs with the drawing and not with the row model:
 *   MAIN     — the longest label, "EXIT TO MAIN MENU" (17ch)
 *   OPTIONS  — the value column at x+150 must fit "WIDESCREEN 16:9" (15ch)
 *   BIND_*   — room for multi-chord defaults ("Z, LCtrl, Space, Return") */
static int panel_width(int page) {
    switch (page) {
    case PG_MAIN:
        return 160;
    case PG_OPTIONS:
        return 264;
    case PG_GRAPHICS:
        return 240;
    case PG_CONTROLS:
        return 264;
    case PG_EXTRA:
        return 230;
    default:
        return 300;
    }
}

/* Dim the background by overlaying ~50%-black across the whole frame. Use the live
 * overlay target size (the wide output), so the dim spans the full widescreen view. */
static void dim_background(uint32_t *fb) {
    const int ow = pc_overlay_w(), oh = pc_overlay_h();
    for (int i = 0; i < ow * oh; i++) {
        uint32_t p = fb[i];
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        r >>= 1;
        g >>= 1;
        b >>= 1;
        fb[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

static void draw_pause_page(uint32_t *fb, const PcPauseView *view) {
    dim_background(fb);

    const int row_h = 11;
    const int ow = pc_overlay_w(), oh = pc_overlay_h();
    const int pw = panel_width(view->page);
    const int ph = 22 + view->row_count * row_h + 8;
    const int px = (ow - pw) / 2, py = (oh - ph) / 2;
    draw_panel(fb, px, py, pw, ph, view->title);

    for (int i = 0; i < view->row_count; i++) {
        const PcPauseRow *row = &view->rows[i];
        const char *value = row->has_value ? row->value : NULL;
        const int selected = (i == view->cursor);
        const int y = py + 22 + i * row_h;
        if (row->disabled) {
            draw_row_disabled(fb, px, y, selected, row->label, value);
        } else {
            draw_row(fb, px, y, selected, row->label, value);
        }
        if (row->submenu) {
            draw_submenu_arrow(fb, px, pw, y, selected);
        }
    }
}

/* The page's own drawing, then the one line that is not part of any page. */
void pc_pause_menu_overlay(uint32_t *fb) {
    PcPauseView view;
    if (!pc_pause_view_build(&view)) {
        return;
    }
    draw_pause_page(fb, &view);
    draw_update_status(fb);
}
