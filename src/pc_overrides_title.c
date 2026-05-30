/* pc_overrides_title.c — full-native title menu.
 *
 * Replaces gfn_gp_003872 (the recompiled title menu loop) with a pure C
 * menu that runs against the SDL framebuffer. Three options:
 *
 *   PLAY GAME       — start the selected level (writes $20.w, jmp $150).
 *   ENTER PASSWORD  — TODO: native password entry. For now, delegates to
 *                     the original recompiled menu so the password screen
 *                     still works.
 *   LEVEL SELECT    — replaces "LOAD EXTRA LEVELS". Opens the existing
 *                     level-select overlay (cycle 1..60 with up/down,
 *                     fire confirms and loads).
 *
 * The native loop yields one frame per iteration via hw_vblank_wait so the
 * coroutine scheduler / pacing / present cycle keeps running normally; the
 * SDL overlay paints into s_fb each frame.
 *
 * The native_render_frame still draws whatever copper the engine set up
 * (the title screen art with BENEFACTOR logo + beach scene), so the
 * overlay just stacks on top — we keep the original artwork for now.
 */
#include "pc_internal.h"
#include "generated/game.h"

extern void hw_vblank_wait(void);
extern int  hw_joy_up(void);
extern int  hw_joy_down(void);
extern int  hw_get_fire(void);
extern int  g_level_select_visible;
extern void pc_set_start_level(int);
extern int  pc_get_start_level(void);

#define FB_W 352
#define FB_H 282
extern void pc_level_select_overlay(uint32_t *fb);   /* level-select panel renderer */
/* hw_get_framebuffer is declared const in hw.h; cast away for our overlay. */

/* 5x7 font + draw helpers live in pc_level_select_ui.c; expose them here. */
extern int pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb);
extern void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb);

/* Menu items. Index 2 was "LOAD EXTRA LEVELS" in the original menu — now
 * "LEVEL SELECT". The string-to-action mapping is what makes this menu
 * "native": the original recompiled menu had this as a third disk-swap
 * option that we don't support (and would rt-miss). */
enum { MI_PLAY = 0, MI_PASSWORD, MI_LEVEL_SELECT, MI_COUNT };
static const char *s_menu_labels[MI_COUNT] = {
    "PLAY GAME",
    "ENTER PASSWORD",
    "LEVEL SELECT",
};

static int s_menu_sel = 0;

/* Draw a centered, bordered panel with the three menu items + cursor.
 * Stacks on top of whatever the engine's copper composed. */
static void draw_menu_overlay(uint32_t *fb, int show_levelsel_hint)
{
    const int pw = 240, ph = 96;
    const int px = (FB_W - pw) / 2;
    const int py = (FB_H - ph) / 2 + 30;   /* slightly below center so logo stays visible */

    pc_fill_rect(fb, px, py, pw, ph, 0xFF101830);
    pc_fill_rect(fb, px,           py,            pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,           py + ph - 1,   pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,           py,            1,  ph, 0xFFFFD040);
    pc_fill_rect(fb, px + pw - 1,  py,            1,  ph, 0xFFFFD040);

    pc_draw_text(fb, px + 8, py + 6, "BENEFACTOR  PC PORT", 1, 0xFFB0C0FF);

    for (int i = 0; i < MI_COUNT; i++) {
        int ly = py + 24 + i * 18;
        uint32_t col = (i == s_menu_sel) ? 0xFFFFD040 : 0xFFB0B0B0;
        if (i == s_menu_sel) pc_draw_text(fb, px + 6, ly, ">", 1, col);
        pc_draw_text(fb, px + 16, ly, s_menu_labels[i], 1, col);
    }

    if (show_levelsel_hint) {
        pc_draw_text(fb, px + 8, py + ph - 12,
                     "F2/F3 LEVEL    FIRE TO START", 1, 0xFF808080);
    }
}

/* Replacement for gfn_gp_003872. Runs as a native override on the disk-boot
 * coroutine — hw_vblank_wait() yields each iteration so the SDL window
 * pumps events, frames present, audio ticks, etc. */
void native_title_menu(M68KCtx *ctx)
{
    /* Edge-trigger nav so a held-down direction doesn't fly past every
     * option in one frame. */
    int prev_up = 0, prev_down = 0, prev_fire = 0;
    s_menu_sel = 0;   /* always start at PLAY GAME */

    for (;;) {
        hw_vblank_wait();   /* yield one frame */

        int u = hw_joy_up(), d = hw_joy_down(), f = hw_get_fire();

        if (u && !prev_up)   s_menu_sel = (s_menu_sel + MI_COUNT - 1) % MI_COUNT;
        if (d && !prev_down) s_menu_sel = (s_menu_sel + 1) % MI_COUNT;
        prev_up = u; prev_down = d;

        /* Draw the overlay AFTER vblank — native_render_frame already
         * composed the title art into s_fb at the start of the frame. */
        uint32_t *fb = (uint32_t *)hw_get_framebuffer();
        if (fb) draw_menu_overlay(fb, s_menu_sel == MI_LEVEL_SELECT);
        /* The level-select panel ALSO paints (if g_level_select_visible),
         * stacking on top of our menu — that's the per-level chooser. */
        if (fb) pc_level_select_overlay(fb);

        /* Fire edge: act on the chosen item. */
        if (f && !prev_fire) {
            if (s_menu_sel == MI_PLAY) {
                /* Start the currently-selected level — pc_set_start_level
                 * has already been written by F2/F3 if the player changed
                 * it; native_overlay_loader_reloc will apply $20.w at
                 * the $150 hand-off. */
                rt_jump(ctx, 0x150u);
                return;
            }
            if (s_menu_sel == MI_LEVEL_SELECT) {
                /* Open the level-select panel; user cycles with F2/F3
                 * and presses fire again to commit. */
                if (!g_level_select_visible) {
                    g_level_select_visible = 1;
                    prev_fire = f;
                    continue;
                }
                /* Already open + fire = commit -> jmp $150. */
                g_level_select_visible = 0;
                rt_jump(ctx, 0x150u);
                return;
            }
            if (s_menu_sel == MI_PASSWORD) {
                /* TODO: native password entry. Until then, delegate to
                 * the original recompiled menu so the password screen
                 * still works. Calls the original $003872 directly via
                 * the GameFn — which will run the engine's menu loop. */
                extern void gfn_gp_003872(M68KCtx *ctx);
                gfn_gp_003872(ctx);
                return;
            }
        }
        prev_fire = f;
    }
}
