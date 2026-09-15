/* pause_menu_view.h — the seam between the pause menu's state and its drawing.
 *
 * src/port/pause_menu.c owns the menu: which page is open, where the cursor
 * is, what each row is worth right now. src/port/pause_menu_draw.c owns the
 * pixels. They meet here, and only here: the drawing half never sees the
 * menu's statics, and reads one flat snapshot instead.
 *
 * The snapshot is a value, not a view onto live state — labels and values are
 * copied in, so a row that is built from a scratch buffer (the pad count, a
 * binding string) stays readable for as long as the frame is being drawn.
 *
 * Internal to the two pause-menu translation units. The overlay entry point
 * the present path calls is pc_pause_menu_overlay, declared in overlay_ui.h.
 */
#pragma once

#include <stdint.h>

/* Pages, in the order they were added. The drawing half sizes its panel per
 * page, so it needs these names; the per-page ROW ids stay private to
 * pause_menu.c, which resolves them into labels and values below. */
enum { PG_MAIN = 0, PG_OPTIONS, PG_GRAPHICS, PG_CONTROLS, PG_EXTRA, PG_BIND_KB, PG_BIND_PAD };

#define PC_PAUSE_MAX_ROWS 14
#define PC_PAUSE_LABEL_MAX 32
#define PC_PAUSE_VALUE_MAX 64 /* a bindings row: "Z, LCtrl, Space, Return" */

typedef struct {
    char label[PC_PAUSE_LABEL_MAX];
    char value[PC_PAUSE_VALUE_MAX]; /* empty when the row has no value column */
    int has_value;
    int submenu;  /* opens another page → draw the arrow, not a value */
    int disabled; /* inert in this context → draw dimmed (GPU effects off HARDWARE) */
} PcPauseRow;

typedef struct {
    int page;                       /* PG_* */
    char title[PC_PAUSE_LABEL_MAX]; /* panel heading */
    int cursor;                     /* already clamped into [0, row_count) */
    int row_count;
    PcPauseRow rows[PC_PAUSE_MAX_ROWS];
} PcPauseView;

/* Snapshot the menu as it stands. Returns 0 (and leaves `view` untouched) when
 * the menu is closed, which is the drawing half's whole visibility test. */
int pc_pause_view_build(PcPauseView *view);
