/* pc_overrides_platformer.c — PLATFORMER jump physics (opt-in, "platformer_physics").
 *
 * Vanilla air movement is table-driven and register-pure: the per-frame player
 * dispatcher ($5796A4) loads the player struct words $10A6..$10B0 into d1-d6
 * (d1 = X, d2 = Y(top, decreasing upward), d3 = ANIMATION cell, d4 = flags,
 * d5 = phase*2, d6 = ?), calls the action handler at $f70(a5), and stores the
 * registers back. The air handlers (live-traced 2026-06-11):
 *   $579D84 hop arc       — d2 -= vtable[d5]; d3 = anim[d5] (0 -> fall);
 *                           ~8px tall, ~17 frames: the "flat jump".
 *   $579DDC long-jump arc — same shape + moves d1 from its own table.
 *   $579F3A fall          — d2 += gravity ramp $3B2E[$f6e]; NO X mover at all
 *                           (that is the "h-speed instantly zero": the jump
 *                           arc ends, fall takes over, X simply stops).
 * No air handler samples left/right input — hence no air control. Arcs are
 * collision-FREE: the jump trigger ($57E43C) pre-validates the fixed arc via
 * the tile map, so free-form physics must bring its own probes:
 *   tile(x,y) = word at $5A8C7E + (x>>4)*2 + rowtab($5A211A)[y>>4],
 *   nonzero = solid (the trigger's own headroom test is `tst.w (a0)`).
 *
 * Platformer mode SUPER-CALLS the recompiled body (animation, SFX, phase and
 * transition side effects stay vanilla) and then re-shapes d1/d2 from a
 * native velocity model (8.8 fixed point):
 *   - ascent: own vy + gravity, jump-cut on UP/FIRE release (variable
 *     height), head-bonk probe; the arc phase d5 is held at its last in-arc
 *     value so longer jumps keep a valid pose, and the vanilla hop->fall
 *     hand-off is overridden until OUR vy turns downward;
 *   - descent: the engine's own fall handler runs unmodified vertically (its
 *     ramp is a sane gravity and its tail owns landing) — we only add the X
 *     that vanilla drops;
 *   - horizontal: own vx, seeded from the vanilla handler's own first-frame
 *     X move (hop=0 -> input kick, long-jump inherits its momentum), air
 *     acceleration toward the held direction, wall probes at body height.
 *
 * All three overrides are registered unconditionally and are pure
 * passthroughs when the knob is off. */
#include "port/port_internal.h"
#include "port/config.h"

extern void gfn_gpl_579D84(M68KCtx *ctx);   /* hop arc       */
extern void gfn_gpl_579DDC(M68KCtx *ctx);   /* long-jump arc */
extern void gfn_gpl_579F3A(M68KCtx *ctx);   /* fall          */

/* 8.8 fixed-point tunables (cfg-overridable for feel iteration). */
static int t_gravity(void)  { return pc_cfg_int("pf_gravity",   60);  } /* px/f^2 *256 */
static int t_jump_vy(void)  { return pc_cfg_int("pf_jump_vy", -640);  } /* initial vy  */
static int t_air_acc(void)  { return pc_cfg_int("pf_air_accel", 32);  } /* px/f^2 *256 */
static int t_vx_max(void)   { return pc_cfg_int("pf_vx_max",   512);  } /* px/f *256   */

static int s_vx, s_vy;        /* 8.8 px/frame */
static int s_airborne;
static int s_cut_done;
static int s_y_acc, s_x_acc;  /* sub-pixel accumulators */

int pc_platformer_on(void) { return pc_cfg_bool("platformer_physics", 0); }

/* The jump trigger's own tile probe ($57E458..$57E484). */
static int tile_solid(M68KCtx *ctx, int x, int y)
{
    if (x < 0 || y < 0) return 1;
    uint32_t a0 = 0x5A8C7Eu + (uint32_t)((x >> 4) << 1)
                + (uint32_t)(int16_t)MR16(0x5A211Au + (uint32_t)((y >> 4) << 1));
    return MR16(a0) != 0;
}

static void phys_enter(M68KCtx *ctx, int vanilla_dx, int jumping)
{
    s_airborne = 1;
    s_cut_done = 0;
    s_y_acc = s_x_acc = 0;
    s_vy = jumping ? t_jump_vy() : 0;
    s_vx = vanilla_dx << 8;       /* long-jump momentum / walk-off momentum */
    uint16_t in = MR16(ctx->A[5] + 0xF80u);
    if (s_vx == 0) {              /* vertical hop + held direction: gentle kick */
        if (in & 0x04u)      s_vx =  t_vx_max() / 3;
        else if (in & 0x10u) s_vx = -t_vx_max() / 3;
    }
}

static void phys_jump_cut(M68KCtx *ctx)
{
    if (s_cut_done || s_vy >= 0) return;
    uint16_t in = MR16(ctx->A[5] + 0xF80u);
    if (!(in & 0x28u)) {          /* $08=up, $20=fire both released */
        s_vy /= 2;
        s_cut_done = 1;
    }
}

/* Horizontal: integrate air control, probe walls at two body heights on the
 * leading edge, return the whole-pixel step (0 when blocked). */
static int phys_dx(M68KCtx *ctx, int x, int y)
{
    uint16_t in = MR16(ctx->A[5] + 0xF80u);
    int acc = t_air_acc(), cap = t_vx_max();
    if (in & 0x04u)      s_vx += acc;
    else if (in & 0x10u) s_vx -= acc;
    if (s_vx >  cap) s_vx =  cap;
    if (s_vx < -cap) s_vx = -cap;

    s_x_acc += s_vx;
    int dx = s_x_acc >> 8;
    s_x_acc -= dx << 8;
    if (dx == 0) return 0;

    int lead = x + dx + (dx > 0 ? 8 : -8);
    if (tile_solid(ctx, lead, y + 4) || tile_solid(ctx, lead, y + 18)) {
        s_vx = 0; s_x_acc = 0;
        return 0;
    }
    return dx;
}

static void air_state(M68KCtx *ctx, void (*super)(M68KCtx *), uint32_t self,
                      int jumping)
{
    if (!pc_platformer_on()) { s_airborne = 0; super(ctx); return; }

    /* Entry detection via the dispatcher's previous-handler slot $f78(a5):
     * a FRESH flight (from a grounded/rope/... state) seeds the model; an
     * air-to-air hand-off (hop -> fall) PRESERVES vx/vy — re-seeding there
     * would zero the momentum, which is the very vanilla flaw this mode
     * removes. */
    uint32_t prev = MR32(ctx->A[5] + 0xF78u);
    int prev_air = (prev == 0x579D84u || prev == 0x579DDCu || prev == 0x579F3Au);
    int entered  = (prev != self) || !s_airborne;
    int fresh    = entered && (!prev_air || !s_airborne);
    int16_t x_in = (int16_t)(uint16_t)ctx->D[1];
    int16_t y_in = (int16_t)(uint16_t)ctx->D[2];

    /* Hold the arc phase at its last in-arc value: a longer (physics) ascent
     * must not run the table off its end (garbage pose) or hit the arc's
     * fixed-phase bookkeeping early. */
    if (jumping && !entered && (uint16_t)ctx->D[5] > 0x14u)
        ctx->D[5] = (ctx->D[5] & 0xFFFF0000u) | 0x14u;

    super(ctx);

    if (fresh)
        phys_enter(ctx, (int16_t)(uint16_t)ctx->D[1] - x_in, jumping);

    phys_jump_cut(ctx);

    int16_t y = y_in;
    if (jumping) {
        /* Ascent: replace the table vertical with our integration. */
        s_vy += t_gravity();
        s_y_acc += s_vy;
        int dy = s_y_acc >> 8;
        s_y_acc -= dy << 8;
        y = (int16_t)(y_in + dy);
        if (dy < 0 && tile_solid(ctx, x_in, y - 2)) {   /* head bonk */
            y = y_in; s_vy = 0; s_y_acc = 0;
        }
        ctx->D[2] = (ctx->D[2] & 0xFFFF0000u) | (uint16_t)y;
        /* WE decide the hop->fall hand-off: stay in this state while rising,
         * fall once vy turns downward (mirror the vanilla late-arc flags). */
        if (s_vy < 0) {
            MW32(ctx->A[0], self);
        } else {
            MW32(ctx->A[0], 0x579F3Au);
            MW16(ctx->A[5] + 0xF6Eu, 0xE);   /* seed the fall ramp like the arc */
            ctx->D[4] |= 1u << 6;            /* the arc's "jumped" flag */
        }
    } else {
        /* Descent: vanilla vertical (its tail owns landing); y for probes. */
        y = (int16_t)(uint16_t)ctx->D[2];
    }

    int dx = phys_dx(ctx, x_in, y);
    ctx->D[1] = (ctx->D[1] & 0xFFFF0000u) | (uint16_t)(int16_t)(x_in + dx);
}

void native_pf_hop(M68KCtx *ctx)      { air_state(ctx, gfn_gpl_579D84, 0x579D84u, 1); }
void native_pf_longjump(M68KCtx *ctx) { air_state(ctx, gfn_gpl_579DDC, 0x579DDCu, 1); }
void native_pf_fall(M68KCtx *ctx)     { air_state(ctx, gfn_gpl_579F3A, 0x579F3Au, 0); }
