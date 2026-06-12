/* pc_overrides_platformer.c — BENMOTION: native ownership of player air physics
 * (opt-in, "platformer_physics") + ONE jump on a dedicated JUMP button.
 *
 * Engine model (RE'd, see instructions/gameplay-engine-map.md):
 *  - Dispatcher $5796A4: $f70(a5) = action-handler ptr; ZERO = GROUNDED (input
 *    dispatch via the $f2a(a5) table). It loads $10A6.. into d1=X d2=Y(top,
 *    decreasing upward) d3=ANIM d4=flags d5=phase*2, calls the handler with
 *    a0=&$f70, stores the registers back. $f78(a5) = previously RUN handler
 *    (0 right after a grounded frame).
 *  - Terrain pass $57A934 (called per-frame from the main loop, jsr -$44de(a5))
 *    owns ground contact: walk-off-ledge -> $f70=$579F3A (gap>=3, d4 bit6
 *    "jumped" exempts), falling($579F3A)+tile contact -> $f70=$579F86 (landing
 *    impact), special tiles (bounce pad writes $579D84 at $57AB96), X wall and
 *    slope resolution (walks pixel-wise from prev pos $f90 to new $10a6), and
 *    flag bookkeeping ($10b4/5/6/8, $f86). Rising frames (newY < oldY) skip the
 *    ground logic entirely ($57AA30 bcs).
 *  - Landing impact $579F86: fall damage = table $3b9a(a5)[$f6e/2] subtracted
 *    from health $1c.w, impact anim from $3ae6(a5), landing SFX a3=$6dec(a5)
 *    via $775c(a5)=$58656E, then anim player $579FE0 -> state 0. $f6e(a5) is
 *    the fall-time meter: fall adds 2/frame, arcs seed $e at their fall
 *    hand-off, landing clears it.
 *  - Vanilla jump commits: UP-hop $f70<-$579D84 only at $57E526 (grounded UP
 *    handler; ladder/door tiles checked FIRST, so suppressing the commit keeps
 *    climbing intact) and the bounce pad ($57AB96, from the fall state).
 *    Fire+dir long jump $f70<-$579A62 at $57EC14/$57ED62 (grounded only).
 *    Trampoline objects write $579DDC ($5995C8) — engine-internal, untouched.
 *  - Long-jump grunt: $579A62 fires SFX at d5==2, a3 = -$273c(a5) descriptor
 *    (or $6dd4(a5) while carrying, $1098==1).
 *
 * Platformer mode (knob ON, and never during DEMO playback/record $1e.w==8):
 *  - JUMP trigger: wraps the terrain pass; on JUMP edge while grounded
 *    ($f70==0), with vanilla's own gates ($f82!=$14, tile-attr bit4), commits
 *    $f70=$579D84 with a takeoff mark.
 *  - NATIVE FLIGHT, no super-call: rising = state $579D84 (our body: vy
 *    integration, jump-cut on release, head bonk, air control, anim from the
 *    arc's own $309c(a5) cells); vy>=0 hands off to $579F3A exactly like the
 *    arc ($f6e=$e, d4 bit6). Descent = state $579F3A (our body replicates the
 *    vanilla fall handler — $f6e+=2 fall-damage meter, d3 clear after ramp
 *    index 6, the bit3 carry-mover call $579A00 with d1 preserved, the bit14
 *    carried-MM trail write — with vy integration replacing the ramp and our
 *    vx replacing the missing X). The terrain pass keeps owning walls/landing.
 *  - Vanilla jump SUPPRESSION: a $579D84 entry from grounded without our
 *    takeoff mark (= the UP-hop commit) reverts to grounded; $579A62 always
 *    reverts (player-only entry). Bounce pads enter $579D84 from the fall
 *    state -> become a native bounce (fresh vy, kept vx).
 *  - $579D52/$579DDC (abort arcs / trampoline arc) stay in TRACK mode: vanilla
 *    arc untouched + steer on top; their fall hand-off inherits the momentum.
 *
 * All overrides are registered unconditionally and are pure passthroughs when
 * the knob is off (vanilla stays byte-identical). */
#include "port/port_internal.h"
#include "port/config.h"

/* RAW input, NOT the engine's decoded $f80(a5): the engine sets the
 * input-disable gate ($1093 bit0) while airborne, so $f80 reads ZERO during
 * the whole flight — that gate IS how vanilla enforces "no air control". */
extern int hw_joy_left(void), hw_joy_right(void), hw_joy_up(void);
extern int hw_get_hop(void);

extern void gfn_gpl_579D84(M68KCtx *ctx);   /* vertical hop arc                */
extern void gfn_gpl_579DDC(M68KCtx *ctx);   /* trampoline/bounce arc           */
extern void gfn_gpl_579D52(M68KCtx *ctx);   /* SHARED arc player: the abort-arc
                                             * variants ($579D00/0E/1C/2A/38/46)
                                             * are entry stubs that rt_jump here */
extern void gfn_gpl_579A62(M68KCtx *ctx);   /* vanilla LONG-JUMP arc (fire+dir) */
extern void gfn_gpl_579F3A(M68KCtx *ctx);   /* vanilla fall                    */
extern void gfn_gpl_57A934(M68KCtx *ctx);   /* per-frame terrain/collision pass */

#define ST_HOP   0x579D84u
#define ST_FALL  0x579F3Au

static int is_air_state(uint32_t a)
{
    return a == 0x579D00u || a == 0x579D0Eu || a == 0x579D1Cu
        || a == 0x579D2Au || a == 0x579D38u || a == 0x579D46u
        || a == 0x579D52u || a == 0x579D84u || a == 0x579DDCu
        || a == 0x579A62u || a == 0x579F3Au;
}

/* 8.8 fixed-point tunables (cfg-overridable for feel iteration). */
static int t_gravity(void)  { return pc_cfg_int("pf_gravity",   60);  } /* px/f^2 *256 */
static int t_jump_vy(void)  { return pc_cfg_int("pf_jump_vy", -640);  } /* initial vy  */
static int t_air_acc(void)  { return pc_cfg_int("pf_air_accel", 128); } /* px/f^2 *256.
                                  32 was imperceptible: the arc moves ~2px/f, so steer
                                  needs to reach ~cap within a few frames to be felt. */
static int t_vx_max(void)   { return pc_cfg_int("pf_vx_max",   512);  } /* px/f *256   */
static int t_vy_max(void)   { return pc_cfg_int("pf_fall_max", 1024); } /* terminal vy */

static int s_vx, s_vy;        /* 8.8 px/frame */
static int s_steer;           /* 8.8 air-control velocity ON TOP of a tracked arc */
static int s_fly;             /* native flight model owns the current air time */
static int s_track;           /* TRACK-mode arc active (trampoline/abort arcs) */
static int s_cut_done;
static int s_y_acc, s_x_acc;  /* sub-pixel accumulators */
static int s_d5;              /* rise anim phase (our own, table $309c) */
static int s_takeoff;         /* trigger committed a native jump this frame */
static int s_jump_prev;       /* JUMP edge latch */

int pc_platformer_on(void) { return pc_cfg_bool("platformer_physics", 0); }

/* Never engage during DEMO playback/record ($1e.w==8): the RLE input stream
 * cannot represent JUMP and the recorded inputs assume vanilla physics. */
static int pf_active(M68KCtx *ctx)
{
    return pc_platformer_on() && MR16(0x1Eu) != 8;
}

/* JUMP input: the dedicated binding; when no device runs modern controls
 * (no jump button exists) the Up direction is the jump, as in vanilla. */
static int jump_input(void)
{
    return hw_get_hop() || (!pc_modern_any() && hw_joy_up());
}

/* The jump trigger's own tile probe ($57E458..$57E484). */
static int tile_solid(M68KCtx *ctx, int x, int y)
{
    if (x < 0 || y < 0) return 1;
    uint32_t a0 = 0x5A8C7Eu + (uint32_t)((x >> 4) << 1)
                + (uint32_t)(int16_t)MR16(0x5A211Au + (uint32_t)((y >> 4) << 1));
    return MR16(a0) != 0;
}

/* Vanilla's own "can I jump on this tile?" gate ($57E50A..$57E51C): tile attr
 * record at $46cc(a5)[code], bit4 of the byte at +4 set = no jump here. */
static int tile_nojump(M68KCtx *ctx, int x, int y)
{
    if (x < 0 || y < 0) return 1;
    int16_t code = (int16_t)MR16(0x5A8C7Eu + (uint32_t)((x >> 4) << 1)
                 + (uint32_t)(int16_t)MR16(0x5A211Au + (uint32_t)((y >> 4) << 1)));
    uint32_t rec = MR32(ctx->A[5] + 0x46CCu + (uint32_t)(int32_t)code);
    return (MR8(rec + 4u) >> 4) & 1;
}

/* Takeoff grunt: exactly the long-jump arc's SFX ($579A8A/$579A94) — a3 =
 * -$273c(a5) descriptor ($6dd4(a5) while carrying), trigger $775c(a5)=$58656E.
 * Scratch registers are saved around the rt_call: we run mid-handler with the
 * dispatcher's live d1-d6. */
static void takeoff_grunt(M68KCtx *ctx)
{
    uint32_t sd[8], sa[8];
    for (int i = 0; i < 8; i++) { sd[i] = ctx->D[i]; sa[i] = ctx->A[i]; }
    ctx->A[3] = (MR16(ctx->A[5] + 0x1098u) == 1)
                    ? ctx->A[5] + 0x6DD4u
                    : MR32(ctx->A[5] - 0x273Cu);
    ctx->D[6] = 0;
    rt_call(ctx, 0x58656Eu);
    for (int i = 0; i < 8; i++) { ctx->D[i] = sd[i]; ctx->A[i] = sa[i]; }
}

static void flight_start(M68KCtx *ctx, int keep_vx)
{
    s_fly = 1; s_track = 0;
    s_cut_done = 0;
    s_y_acc = s_x_acc = 0;
    s_steer = 0;
    s_d5 = 0;
    s_vy = t_jump_vy();
    if (!keep_vx) {
        /* Ground velocity isn't tracked yet (Stage 4): a held direction means
         * "running jump" — full cap, momentum-equivalent. */
        if (hw_joy_right())      s_vx =  t_vx_max();
        else if (hw_joy_left())  s_vx = -t_vx_max();
        else                     s_vx = 0;
    }
    takeoff_grunt(ctx);
}

static void phys_jump_cut(void)
{
    if (s_cut_done || s_vy >= 0) return;
    if (!jump_input()) {            /* variable height: release halves the rise */
        s_vy /= 2;
        s_cut_done = 1;
    }
}

/* Horizontal: integrate air control, probe walls at two body heights on the
 * leading edge, return the whole-pixel step (0 when blocked). The terrain
 * pass re-validates X afterwards (slopes, step-up), so these probes only stop
 * us from burying velocity into a wall. */
static int phys_dx(M68KCtx *ctx, int x, int y)
{
    int acc = t_air_acc(), cap = t_vx_max();
    if (hw_joy_right())     s_vx += acc;
    else if (hw_joy_left()) s_vx -= acc;
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

/* ── RISE: native body for state $579D84 ──────────────────────────────────── */
static void flight_rise(M68KCtx *ctx)
{
    int16_t x = (int16_t)(uint16_t)ctx->D[1];
    int16_t y = (int16_t)(uint16_t)ctx->D[2];

    phys_jump_cut();
    s_vy += t_gravity();
    s_y_acc += s_vy;
    int dy = s_y_acc >> 8;
    s_y_acc -= dy << 8;
    if (dy < 0 && tile_solid(ctx, x, y + dy - 2)) {   /* head bonk */
        dy = 0; s_vy = 0; s_y_acc = 0;
    }
    y = (int16_t)(y + dy);
    ctx->D[2] = (ctx->D[2] & 0xFFFF0000u) | (uint16_t)y;

    /* Anim from the hop arc's own cell table $309c(a5); hold the last in-arc
     * pose for ascents longer than the table. */
    int16_t cell = (int16_t)MR16(ctx->A[5] + 0x309Cu + (uint32_t)s_d5);
    if (cell != 0)
        ctx->D[3] = (ctx->D[3] & 0xFFFF0000u) | (uint16_t)cell;
    if (s_d5 < 0x14) s_d5 += 2;
    ctx->D[5] = (ctx->D[5] & 0xFFFF0000u) | (uint16_t)s_d5;

    /* Carried-MM trail record, as the arc does ($579D88..A2, table $5d02). */
    if (ctx->D[4] & (1u << 14)) {
        uint32_t slot = 0x5A4526u
                      + (uint32_t)((MR16(ctx->A[5] + 0xFA2u) & 0xFu) << 6);
        MW16(slot, (uint16_t)(y - (int16_t)MR16(ctx->A[5] + 0x5D02u + (uint32_t)s_d5)));
    }

    int dx = phys_dx(ctx, x, y);
    ctx->D[1] = (ctx->D[1] & 0xFFFF0000u) | (uint16_t)(int16_t)(x + dx);

    if (s_vy >= 0) {
        /* Apex: hand off to the fall state exactly like the arc's own tail
         * ($579DD2 + the $579DB6 bookkeeping). */
        MW32(ctx->A[0], ST_FALL);
        MW16(ctx->A[5] + 0xF6Eu, 0xE);
        ctx->D[4] |= 1u << 6;
    } else {
        MW32(ctx->A[0], ST_HOP);
    }
}

/* ── DESCENT: native body for state $579F3A (mirrors the vanilla fall) ─────── */
static void flight_fall(M68KCtx *ctx)
{
    int16_t x = (int16_t)(uint16_t)ctx->D[1];
    int16_t y = (int16_t)(uint16_t)ctx->D[2];

    /* bclr #5,d4 ($579F3A) */
    ctx->D[4] &= ~(1u << 5);

    /* $f6e fall-time meter, +2/frame — the landing state's fall-damage input.
     * Past ramp index 6 vanilla clears the anim cell ($579F50). */
    uint16_t f6e = MR16(ctx->A[5] + 0xF6Eu);
    if ((uint16_t)(f6e + 2) >= 6)
        ctx->D[3] = (ctx->D[3] & 0xFFFF0000u);
    MW16(ctx->A[5] + 0xF6Eu, (uint16_t)(f6e + 2));

    /* Vertical: vy integration replaces the $3b2e ramp. */
    s_vy += t_gravity();
    if (s_vy > t_vy_max()) s_vy = t_vy_max();
    s_y_acc += s_vy;
    int dy = s_y_acc >> 8;
    s_y_acc -= dy << 8;
    y = (int16_t)(y + dy);
    ctx->D[2] = (ctx->D[2] & 0xFFFF0000u) | (uint16_t)y;

    /* Carry-arc mover ($579F5A..64): call $579A00 for its state side effects,
     * with d1 saved/restored exactly as vanilla does. */
    if (ctx->D[4] & (1u << 3)) {
        uint32_t d1 = ctx->D[1];
        rt_call(ctx, 0x579A00u);
        ctx->D[1] = (ctx->D[1] & 0xFFFF0000u) | (uint16_t)d1;
    }

    /* Carried-MM trail record ($579F66..80). */
    if (ctx->D[4] & (1u << 14)) {
        uint32_t slot = 0x5A4526u
                      + (uint32_t)((MR16(ctx->A[5] + 0xFA2u) & 0xFu) << 6);
        MW16(slot, (uint16_t)(y - 6));
    }

    /* Horizontal: the X that vanilla drops (the "h-speed instantly zero" flaw). */
    int dx = phys_dx(ctx, x, y);
    ctx->D[1] = (ctx->D[1] & 0xFFFF0000u) | (uint16_t)(int16_t)(x + dx);
}

/* ── State overrides ──────────────────────────────────────────────────────── */

/* $579D84 — entered by: our JUMP trigger (takeoff mark), the vanilla UP-hop
 * commit ($57E526, $f78==0 = from grounded -> SUPPRESS), or the bounce pad
 * ($57AB96, from the fall state -> native bounce). */
void native_pf_hop(M68KCtx *ctx)
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579D84(ctx); return; }

    uint32_t prev = MR32(ctx->A[5] + 0xF78u);
    if (prev == 0 || !is_air_state(prev)) {
        if (!s_takeoff) { MW32(ctx->A[0], 0); return; }  /* vanilla UP-hop: suppress */
        s_takeoff = 0;
        flight_start(ctx, 0);
    } else if (!s_fly || s_vy >= 0) {
        /* Engine-internal re-entry from the air (bounce pad): fresh rise,
         * momentum kept if our model was live. */
        flight_start(ctx, s_fly || s_track);
    }
    flight_rise(ctx);
}

/* $579A62 — the vanilla fire+dir long jump; player-initiated only. One jump,
 * one button: revert to grounded (FIRE stays free for interactions). */
void native_pf_lj(M68KCtx *ctx)
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579A62(ctx); return; }
    MW32(ctx->A[0], 0);
}

/* $579F3A — fall. Fresh entries (walk off a ledge, knockback writers) seed the
 * model; a hand-off from our rise keeps vx/vy. */
void native_pf_fall(M68KCtx *ctx)
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579F3A(ctx); return; }

    if (!s_fly) {
        s_fly = 1; s_track = 0;
        s_cut_done = 1;
        s_y_acc = s_x_acc = 0;
        s_vy = 0; s_steer = 0;
        /* No ground-velocity tracking yet (Stage 4): a held direction carries
         * the walk into the fall. */
        if (hw_joy_right())      s_vx =  t_vx_max();
        else if (hw_joy_left())  s_vx = -t_vx_max();
        else                     s_vx = 0;
    }
    flight_fall(ctx);
}

/* TRACK mode ($579D52 abort arcs + $579DDC trampoline arc): the vanilla arc
 * runs UNTOUCHED — pixel-faithful movement, animation, SFX. We only add a
 * steering velocity on top (same wall probes) and track the arc's own X move
 * into s_vx so the fall hand-off inherits the momentum. */
static void air_track(M68KCtx *ctx, void (*super)(M68KCtx *))
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; super(ctx); return; }

    int fresh = !s_track;
    int16_t  x_in     = (int16_t)(uint16_t)ctx->D[1];
    uint32_t state_in = MR32(ctx->A[5] + 0xF70u);

    super(ctx);

    if (fresh) {
        s_track = 1; s_fly = 0; s_cut_done = 1;
        s_x_acc = s_y_acc = 0; s_vy = 0; s_vx = 0; s_steer = 0;
    }
    int dxv = (int16_t)(uint16_t)ctx->D[1] - x_in;

    int acc = t_air_acc(), cap = t_vx_max();
    if (hw_joy_right())     s_steer += acc;
    else if (hw_joy_left()) s_steer -= acc;
    if (s_steer >  cap) s_steer =  cap;
    if (s_steer < -cap) s_steer = -cap;
    s_x_acc += s_steer;
    int sdx = s_x_acc >> 8;
    s_x_acc -= sdx << 8;
    if (sdx) {
        int x_arc = x_in + dxv;
        int16_t y = (int16_t)(uint16_t)ctx->D[2];
        int lead = x_arc + sdx + (sdx > 0 ? 8 : -8);
        if (tile_solid(ctx, lead, y + 4) || tile_solid(ctx, lead, y + 18)) {
            s_steer = 0; s_x_acc = 0; sdx = 0;
        }
    }
    if (sdx)
        ctx->D[1] = (ctx->D[1] & 0xFFFF0000u)
                  | (uint16_t)(int16_t)(x_in + dxv + sdx);

    if (dxv || sdx) s_vx = (dxv << 8) + s_steer;  /* combined momentum for fall */

    /* The arc's own hand-off to fall is kept; when it fires, mark the model
     * live so the fall entry preserves the tracked momentum. */
    if (MR32(ctx->A[5] + 0xF70u) == ST_FALL) { s_fly = 1; s_track = 0; }
    (void)state_in;
}

void native_pf_arc(M68KCtx *ctx)      { air_track(ctx, gfn_gpl_579D52); }
void native_pf_longjump(M68KCtx *ctx) { air_track(ctx, gfn_gpl_579DDC); }

/* ── JUMP trigger: wraps the per-frame terrain pass $57A934 ───────────────── */
void native_pf_collision(M68KCtx *ctx)
{
    gfn_gpl_57A934(ctx);

    int j = jump_input();
    int edge = j && !s_jump_prev;
    s_jump_prev = j;

    if (!pf_active(ctx)) { s_takeoff = 0; return; }

    uint32_t st = MR32(ctx->A[5] + 0xF70u);
    if (!is_air_state(st)) { s_fly = s_track = 0; }   /* landed / left the air */

    if (!edge) return;
    /* Grounded only. One tolerated exception: the vanilla UP-hop commit can
     * land in the same frame BEFORE this trigger (UP-as-jump on a vanilla
     * device) — the state is already $579D84; just mark the takeoff ours so
     * the suppress branch doesn't eat it. */
    if (st == ST_HOP && !s_fly) { s_takeoff = 1; return; }
    if (st != 0) return;
    if (MR16(ctx->A[5] + 0xF82u) == 0x14) return;     /* vanilla's own gate */
    int x = (int16_t)MR16(ctx->A[5] + 0x10A6u);
    int y = (int16_t)MR16(ctx->A[5] + 0x10A8u);
    if (tile_nojump(ctx, x, y)) return;               /* no-jump tile (water etc.) */

    MW32(ctx->A[5] + 0xF70u, ST_HOP);
    s_takeoff = 1;
}
