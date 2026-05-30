/* pc_level_select_ui.c — on-screen level-select overlay.
 *
 * Renders a panel into the SDL framebuffer (s_fb, ARGB8888, 352x282) AFTER
 * the game's native_render_frame composes the title menu / gameplay. Lives
 * fully on the PC side — does NOT touch g_mem, does NOT involve the game's
 * own glyph blitter. The game still reads $20.w naturally when fire is
 * pressed (handled by native_overlay_loader_reloc); this overlay is just
 * the player-facing feedback.
 *
 * Toggled by F1 from hw_handle_key. F2/F3 cycle the level (which updates
 * g_pc_start_level via pc_set_start_level — same call the harness uses).
 * Fire from the title menu starts the chosen level normally; the overlay
 * stays out of the engine's input path.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern void pc_level_split(int level, int *world, int *liw);
extern const char *pc_world_name(int world);
extern const char *pc_static_level_name(int level);
extern int pc_get_start_level(void);

int g_level_select_visible = 0;

/* Minimal 5x7 bitmap font (uppercase + digits + a few punctuation).
 * Each glyph: 7 rows, low 5 bits = pixels. Bit 4 = leftmost column. */
static const uint8_t s_font[][7] = {
    /* SPACE (idx 0) */     {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* ! */                 {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    /* " */                 {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* ' */                 {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* , */                 {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    /* - */                 {0x00,0x00,0x00,0x0E,0x00,0x00,0x00},
    /* . */                 {0x00,0x00,0x00,0x00,0x00,0x04,0x00},
    /* 0 */                 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 1 */                 {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */                 {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 3 */                 {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */                 {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 5 */                 {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */                 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 7 */                 {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */                 {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 9 */                 {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : */                 {0x00,0x04,0x00,0x00,0x04,0x00,0x00},
    /* ? */                 {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* A */                 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* B */                 {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */                 {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* D */                 {0x1E,0x09,0x09,0x09,0x09,0x09,0x1E},
    /* E */                 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* F */                 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */                 {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* H */                 {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */                 {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* J */                 {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */                 {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */                 {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */                 {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* N */                 {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    /* O */                 {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* P */                 {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */                 {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* R */                 {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */                 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    /* T */                 {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */                 {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* V */                 {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */                 {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* X */                 {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */                 {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},
    /* Z */                 {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

/* Map char to glyph index, -1 = unknown (drawn as space). */
static int glyph_idx(char c)
{
    if (c == ' ') return 0;
    if (c == '!') return 1;
    if (c == '"') return 2;
    if (c == '\'') return 3;
    if (c == ',') return 4;
    if (c == '-') return 5;
    if (c == '.') return 6;
    if (c >= '0' && c <= '9') return 7 + (c - '0');
    if (c == ':') return 17;
    if (c == '?') return 18;
    if (c >= 'A' && c <= 'Z') return 19 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 19 + (c - 'a');
    return -1;
}

#define FB_W 352
#define FB_H 282

static void put_pixel(uint32_t *fb, int x, int y, uint32_t argb)
{
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;
    fb[y * FB_W + x] = argb;
}

/* Exposed for pc_overrides_title.c (the native title menu). */
void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb);
int  pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb);

static void fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= FB_H) continue;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= FB_W) continue;
            fb[y * FB_W + x] = argb;
        }
    }
}

/* Draw a string at (x, y) scaled by `scale` (1 or 2). Returns advance x. */
static int draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb)
{
    if (scale < 1) scale = 1;
    for (; *s; s++) {
        int idx = glyph_idx(*s);
        if (idx >= 0) {
            for (int row = 0; row < 7; row++) {
                uint8_t bits = s_font[idx][row];
                for (int col = 0; col < 5; col++) {
                    if (bits & (1 << (4 - col))) {
                        for (int dy = 0; dy < scale; dy++) {
                            for (int dx = 0; dx < scale; dx++) {
                                put_pixel(fb, x + col * scale + dx, y + row * scale + dy, argb);
                            }
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
    return x;
}

/* Public wrappers used by pc_overrides_title.c. */
void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb) {
    fill_rect(fb, x0, y0, w, h, argb);
}
int pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb) {
    return draw_text(fb, x, y, s, scale, argb);
}

void pc_level_select_overlay(uint32_t *fb)
{
    if (!g_level_select_visible) return;

    int level = pc_get_start_level();
    int world = 0, liw = 0;
    pc_level_split(level, &world, &liw);
    /* Defensive: pc_level_split reads the engine's table at $57782E. If
     * that table isn't valid yet at runtime (e.g. overlay not in chip RAM
     * at this state), `world` can come back as garbage like $FFFF, which
     * would render ~60 "-1. ?" rows and an off-screen panel. Clamp. */
    if (world < 0 || world > 6) world = 0;
    if (liw   < 0 || liw   > 9) liw   = 0;
    const char *wn = pc_world_name(world);

    /* Count levels in the current world by scanning the 60-entry level
     * table. Worlds 0/1 have 9, worlds 2..5 have 10, world 6 has 2. */
    int liw_count = 0;
    for (int n = 1; n <= 60; n++) {
        int w_, l_; pc_level_split(n, &w_, &l_);
        if (w_ == world) liw_count++;
    }
    if (liw_count <= 0 || liw_count > 10) {
        /* Hardcoded fallback if the table is unreadable. */
        static const int fallback[7] = { 9, 9, 10, 10, 10, 10, 2 };
        liw_count = fallback[world];
    }
    if (liw >= liw_count) liw = liw_count - 1;

    /* Panel: centered, large enough for header + world name + up to 10 rows. */
    const int pw = 280;
    const int row_h = 11;
    const int ph = 36 + liw_count * row_h + 18;   /* header + rows + footer */
    const int px = (FB_W - pw) / 2;
    const int py = (FB_H - ph) / 2;

    /* Background + border. */
    fill_rect(fb, px, py, pw, ph, 0xFF101830);
    fill_rect(fb, px,           py,            pw, 1, 0xFFFFD040);
    fill_rect(fb, px,           py + ph - 1,   pw, 1, 0xFFFFD040);
    fill_rect(fb, px,           py,            1,  ph, 0xFFFFD040);
    fill_rect(fb, px + pw - 1,  py,            1,  ph, 0xFFFFD040);

    /* Header line. */
    draw_text(fb, px + 8, py + 5, "LEVEL SELECT", 1, 0xFFB0C0FF);

    /* World name + LEFT/RIGHT arrows hint. */
    char wbuf[64];
    snprintf(wbuf, sizeof wbuf, "< %s >", wn);
    /* Center the world line. */
    int w_text_len = (int)strlen(wbuf) * 6;
    int wx = px + (pw - w_text_len) / 2;
    draw_text(fb, wx, py + 18, wbuf, 1, 0xFFFFD040);

    /* Level list: rows are 1..liw_count, with cursor marker on row==liw. */
    int list_y0 = py + 33;
    /* World offsets for the global level number (mirrors the engine's
     * table at $57782E — worlds 0+1 have 9 levels each, 2..5 have 10,
     * 6 has 2). Used as fallback when pc_level_split can't resolve. */
    static const int wstart_fallback[7] = { 0, 9, 18, 28, 38, 48, 58 };
    for (int i = 0; i < liw_count; i++) {
        int row_level = -1;
        /* Try the engine's table first. */
        for (int n = 1; n <= 60; n++) {
            int w_, l_; pc_level_split(n, &w_, &l_);
            if (w_ == world && l_ == i) { row_level = n; break; }
        }
        /* If table unreadable, fall back to the known layout. */
        if (row_level < 1) row_level = wstart_fallback[world] + i + 1;
        uint32_t col = (i == liw) ? 0xFFFFD040 : 0xFFB0B0B0;
        int ry = list_y0 + i * row_h;
        if (i == liw) draw_text(fb, px + 8, ry, ">", 1, col);
        char rbuf[64];
        const char *nm = pc_static_level_name(row_level);
        snprintf(rbuf, sizeof rbuf, "%2d. %s", row_level, nm);
        draw_text(fb, px + 20, ry, rbuf, 1, col);
    }

    /* Footer. */
    draw_text(fb, px + 8, py + ph - 12,
              "FIRE = START   UP/DN LEVEL   L/R WORLD", 1, 0xFF808080);
}
