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
    const char *wn = pc_world_name(world);
    const char *ln = pc_static_level_name(level);

    /* Panel: centered, 280×80 with dark-translucent fill + bright border. */
    const int pw = 280, ph = 80;
    const int px = (FB_W - pw) / 2;
    const int py = (FB_H - ph) / 2;

    /* Translucent dark fill (we don't have alpha; just paint solid dark blue). */
    fill_rect(fb, px, py, pw, ph, 0xFF101830);
    /* Border (yellow). */
    fill_rect(fb, px,           py,            pw, 1, 0xFFFFD040);
    fill_rect(fb, px,           py + ph - 1,   pw, 1, 0xFFFFD040);
    fill_rect(fb, px,           py,            1,  ph, 0xFFFFD040);
    fill_rect(fb, px + pw - 1,  py,            1,  ph, 0xFFFFD040);

    /* Header: "LEVEL SELECT  F2/F3  FIRE TO START" small white. */
    draw_text(fb, px + 8, py + 6,
              "LEVEL SELECT   F2/F3   FIRE = START",
              1, 0xFFB0C0FF);

    /* Level number line: "LEVEL NN" large yellow. */
    char buf[64];
    snprintf(buf, sizeof buf, "LEVEL %d", level);
    draw_text(fb, px + 8, py + 20, buf, 2, 0xFFFFD040);

    /* World name. */
    draw_text(fb, px + 8, py + 42, wn, 1, 0xFFFFFFFF);
    /* Level name. */
    draw_text(fb, px + 8, py + 56, ln, 1, 0xFFA0FFA0);
}
