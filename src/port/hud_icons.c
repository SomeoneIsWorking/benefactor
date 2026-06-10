/* pc_hud_icons.c — small vector-drawn HUD status icons, top-left corner.
 *
 * Drawn into the composed output surface (same overlay stage as the pause
 * menu / toast in hw_present_frame), so they ride on top of everything and
 * work at any output width. Each icon is rasterised from simple shapes
 * (triangles / rects / circles) at 2x with a 1px dark outline — crisp at the
 * native pixel scale, no asset files.
 *
 *   FAST-FORWARD (two right-pointing triangles + bar)  — while the PI_FFWD
 *     binding is held (hw_ffwd_active).
 *   CAMERA (body + lens + viewfinder bump)             — while the free cam
 *     is active (pc_freecam_active).
 */
#include <stdint.h>
#include <stddef.h>

extern int pc_overlay_w(void), pc_overlay_h(void);
extern int hw_ffwd_active(void);
extern int pc_freecam_active(void);

#define ICON_W   22
#define ICON_H   16
#define ICON_X    8
#define ICON_Y    8
#define ICON_GAP  6

#define COL_FILL    0xFFFFFFFFu
#define COL_OUTLINE 0xFF202020u

/* 1 when the icon overlay has anything to draw (hw.c: forces the blit present
 * path, since the per-sprite scene present bypasses s_out overlays). */
int pc_hud_icons_active(void)
{
    return hw_ffwd_active() || pc_freecam_active();
}

/* ── Shape coverage tests (icon-local px) ──────────────────────────────────── */

/* Fast-forward: two right triangles (4..) + a vertical bar at the right. */
static int ff_shape(int x, int y)
{
    int mid = ICON_H / 2;
    int dy = y < mid ? mid - y : y - mid;       /* distance from the mid row */
    /* triangle 0 spans x 0..7, triangle 1 spans x 8..15; each is widest at its
     * left edge and converges to the apex at its right edge (right-pointing). */
    for (int t = 0; t < 2; t++) {
        int lx = x - t * 8;
        if (lx >= 0 && lx < 8 && dy <= 7 - lx) return 1;
    }
    if (x >= 18 && x <= 19 && y >= 1 && y < ICON_H - 1) return 1;   /* end bar */
    return 0;
}

/* Camera: viewfinder bump on top, rounded body, lens ring. */
static int cam_shape(int x, int y)
{
    /* bump: x 7..14, y 0..2 */
    if (y >= 0 && y <= 2 && x >= 7 && x <= 14) return 1;
    /* body: x 0..21, y 3..15 */
    if (y >= 3 && y <= 15 && x >= 0 && x <= 21) {
        /* lens hole: ring around (11, 9), inner r2 dark hole handled by caller
         * via outline colour — here body minus inner circle */
        int dx = x - 11, dy = y - 9;
        int r2 = dx * dx + dy * dy;
        if (r2 <= 6) return 0;          /* lens hole (drawn as outline colour) */
        return 1;
    }
    return 0;
}

static int cam_hole(int x, int y)
{
    int dx = x - 11, dy = y - 9;
    return (y >= 3 && y <= 15 && x >= 0 && x <= 21 && dx * dx + dy * dy <= 6);
}

/* Draw one icon: outline = the shape dilated by 1px in dark, then the fill. */
static void draw_icon(uint32_t *fb, int ow, int oh, int ox, int oy,
                      int (*shape)(int, int), int (*hole)(int, int))
{
    for (int y = -1; y <= ICON_H; y++) {
        int py = oy + y;
        if (py < 0 || py >= oh) continue;
        for (int x = -1; x <= ICON_W; x++) {
            int px = ox + x;
            if (px < 0 || px >= ow) continue;
            if (shape(x, y)) { fb[(size_t)py * ow + px] = COL_FILL; continue; }
            if (hole && hole(x, y)) { fb[(size_t)py * ow + px] = COL_OUTLINE; continue; }
            /* outline: any 8-neighbour inside the shape */
            int near = 0;
            for (int j = -1; j <= 1 && !near; j++)
                for (int i = -1; i <= 1 && !near; i++)
                    if (shape(x + i, y + j)) near = 1;
            if (near) fb[(size_t)py * ow + px] = COL_OUTLINE;
        }
    }
}

void pc_hud_icons_overlay(uint32_t *fb)
{
    if (!pc_hud_icons_active()) return;
    int ow = pc_overlay_w(), oh = pc_overlay_h();
    int x = ICON_X;
    if (hw_ffwd_active()) {
        draw_icon(fb, ow, oh, x, ICON_Y, ff_shape, 0);
        x += ICON_W + ICON_GAP;
    }
    if (pc_freecam_active())
        draw_icon(fb, ow, oh, x, ICON_Y, cam_shape, cam_hole);
}
