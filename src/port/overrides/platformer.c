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
#include "port/input.h"
#include <strings.h>

/* RAW input, NOT the engine's decoded $f80(a5): the engine sets the
 * input-disable gate ($1093 bit0) while airborne, so $f80 reads ZERO during
 * the whole flight — that gate IS how vanilla enforces "no air control". */
extern int hw_joy_left(void), hw_joy_right(void), hw_joy_up(void);
extern int hw_get_hop(void), hw_get_fire(void);

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
        || a == 0x579A62u || a == 0x579F3Au || a == 0x579E02u;
}

/* 8.8 fixed-point tunables (cfg-overridable for feel iteration). */
static int t_gravity(void)  { return pc_cfg_int("pf_gravity",   60);  } /* px/f^2 *256 */
static int t_jump_vy(void)  { return pc_cfg_int("pf_jump_vy", -768);  } /* initial vy.
                                  -640 (~11px apex) felt like "barely leaves the
                                  ground" (user, 2026-06-12); -768 ≈ 18px. */
static int t_air_acc(void)  { return pc_cfg_int("pf_air_accel", 256); } /* px/f^2 *256.
                                  32 was imperceptible: the arc moves ~2px/f, so steer
                                  needs to reach ~cap within a few frames to be felt.
                                  128 still fought momentum (user: "air speed very hard
                                  to control", 2026-06-12) -> 256 = cap in 2 frames. */
static int t_air_drag(void) { return pc_cfg_int("pf_air_drag",  192); } /* px/f^2 *256.
                                  Super Metroid-style: on a NEUTRAL stick the air
                                  velocity decays toward 0 (here ~3 frames from cap)
                                  instead of persisting, so X motion tracks the stick
                                  almost directly. 0 = old pure-momentum behaviour. */
static int t_vx_max(void)   { return pc_cfg_int("pf_vx_max",   512);  } /* px/f *256   */

/* SUPER-SHOES boost: the vanilla long jump reads its per-frame X step from
 * table $2ad0(a5), but with the SHOES in the carry slot it uses the faster
 * flat table $2b3c(a5) instead (and the $6dd4(a5) "boing" grunt, which
 * takeoff_grunt already honours). The gate is `cmpi.w #$1, $1098(a5)` at
 * $579A7E — item type 1 = shoes EXACTLY, not any carried item (the laser
 * glasses are type 2 in the same slot and get neither boost nor boing).
 * Mirror the engine's own rule: scale the native vx cap by the two tables'
 * summed arc distance (both 27 entries, $2ad0..$2b06). Engine-derived, no
 * magic constant; vanilla measured 44px -> 54px, same 1.2x ratio. */
static int pf_vx_cap(M68KCtx *ctx)
{
    int cap = t_vx_max();
    if (MR16(ctx->A[5] + 0x1098u) == 1) {
        int sn = 0, sc = 0;
        for (uint32_t i = 0; i < 27; i++) {
            sn += (int16_t)MR16(ctx->A[5] + 0x2AD0u + 2u * i);
            sc += (int16_t)MR16(ctx->A[5] + 0x2B3Cu + 2u * i);
        }
        if (sn > 0 && sc > sn) cap = cap * sc / sn;
    }
    return cap;
}
static int t_vy_max(void)   { return pc_cfg_int("pf_fall_max", 1024); } /* terminal vy */

static int s_vx, s_vy;        /* 8.8 px/frame */
static int s_steer;           /* 8.8 air-control velocity ON TOP of a tracked arc */
static int s_fly;             /* native flight model owns the current air time */
static int s_track;           /* TRACK-mode arc active (trampoline/abort arcs) */
static int s_cut_done;
static int s_y_acc, s_x_acc;  /* sub-pixel accumulators */
static int s_d5;              /* flight anim phase (our own) */
static int s_roll;            /* horizontal jump: play the long-jump ROLL anim */
static int s_air_frames;      /* frames since takeoff (tap-jump detection) */
static int s_fire_jump;       /* jump initiated by vanilla fire+dir: FIRE is
                                 the held button for the variable-height cut */
static int s_takeoff;         /* trigger committed a native jump this frame */
static int s_jump_prev;       /* JUMP edge latch */
static int s_x_expect, s_x_expect_valid;  /* wall feedback (see wall_feedback) */

int pc_platformer_on(void) { return pc_cfg_bool("platformer_physics", 0); }

/* Never engage during DEMO playback/record ($1e.w==8): the RLE input stream
 * cannot represent JUMP and the recorded inputs assume vanilla physics. */
static int pf_active(M68KCtx *ctx)
{
    return pc_platformer_on() && MR16(0x1Eu) != 8;
}

/* JUMP input, PER DEVICE: the dedicated binding, plus — on a device running
 * VANILLA controls (which has no jump button) — that device's Up, as on
 * hardware. The old `!pc_modern_any() && hw_joy_up()` gate killed jumping
 * outright for a vanilla keyboard whenever the controller flag was modern
 * (user, 2026-06-12). */
static int vanilla_up(void)
{
    return (!pc_modern_kb()  && pc_input_active_dev(PI_DEV_KB,  PI_UP))
        || (!pc_modern_pad() && pc_input_active_dev(PI_DEV_PAD, PI_UP))
        || (!pc_modern_any() && hw_joy_up());   /* harness-injected raw input */
}
static int vanilla_fire(void)
{
    return (!pc_modern_kb()  && pc_input_active_dev(PI_DEV_KB,  PI_FIRE))
        || (!pc_modern_pad() && pc_input_active_dev(PI_DEV_PAD, PI_FIRE))
        || (!pc_modern_any() && hw_get_fire()); /* harness-injected raw input */
}
static int jump_input(void)
{
    return hw_get_hop() || vanilla_up();
}
/* For the modern-UP diagonal-freeze guard in native_gameplay_input. */
int pc_pf_vanilla_up(void) { return vanilla_up(); }

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
    s_x_expect_valid = 0;
    s_cut_done = 0;
    s_y_acc = s_x_acc = 0;
    s_steer = 0;
    s_d5 = 0;
    s_vy = t_jump_vy();
    if (!keep_vx) {
        /* Ground velocity isn't tracked yet (Stage 4): a held direction means
         * "running jump" — full cap, momentum-equivalent. */
        int cap = pf_vx_cap(ctx);
        if (hw_joy_right())      s_vx =  cap;
        else if (hw_joy_left())  s_vx = -cap;
        else                     s_vx = 0;
    }
    s_roll = (s_vx != 0);
    s_air_frames = 0;
    s_fire_jump = 0;
    takeoff_grunt(ctx);
}

static void phys_jump_cut(void)
{
    if (s_cut_done || s_vy >= 0) return;
    if (!(s_fire_jump ? vanilla_fire() : jump_input())) {
        if (s_air_frames <= 4) {    /* TAP: a genuinely small hop — cut the
                                     * rise hard AND shed horizontal speed,
                                     * or the full-cap vx carries a "minimal"
                                     * jump way too far (user, 2026-06-12) */
            s_vy /= 3;
            s_vx /= 2;
        } else {
            s_vy /= 2;              /* variable height: release halves the rise */
        }
        s_cut_done = 1;
    }
}

/* Body geometry relative to d2. d2 is the FEET line — the terrain pass
 * compares it directly against the ground surface ($57AA8A `cmp d0,d2`), and
 * a standing player has d2 == surface. */
#define BODY_LO   2     /* lower-body probe: 2px above the feet  */
#define BODY_HI  16     /* upper-body probe: 16px above the feet */

/* Horizontal: integrate air control and emit the whole-pixel step. WALLS ARE
 * THE TERRAIN PASS'S JOB: it re-resolves X every frame pixel-wise against the
 * per-column HEIGHT PROFILES (slopes, step-up) — a flat `tile word != 0`
 * probe cannot reproduce that: the SURFACE row itself is a nonzero profile
 * tile, so any leading-edge probe at body height reads "solid" while merely
 * standing on the ground and zeroes vx (the "jump barely moves sideways"
 * bug, user 2026-06-12; the first fix attempt — raising the probe offsets
 * off the floor — still hit the surface-row profile tile). Instead the
 * caller detects the pass's clamp (actual X != the X we emitted last frame)
 * and zeroes vx then. */
static int air_vel_step(int v, int cap)
{
    if (hw_joy_right())     v += t_air_acc();
    else if (hw_joy_left()) v -= t_air_acc();
    else {
        int drag = t_air_drag();
        if      (v >  drag) v -= drag;
        else if (v < -drag) v += drag;
        else                v  = 0;
    }
    if (v >  cap) v =  cap;
    if (v < -cap) v = -cap;
    return v;
}

static int phys_dx(M68KCtx *ctx, int x, int y)
{
    (void)x; (void)y;
    s_vx = air_vel_step(s_vx, pf_vx_cap(ctx));

    s_x_acc += s_vx;
    int dx = s_x_acc >> 8;
    s_x_acc -= dx << 8;
    return dx;
}

/* Wall feedback: compare this frame's incoming X against the X our previous
 * flight frame emitted — a mismatch means the terrain pass clamped us into a
 * wall; kill the velocity so we don't grind against it. */
static void wall_feedback(int x_in)
{
    if (s_x_expect_valid && x_in != s_x_expect) { s_vx = 0; s_x_acc = 0; }
}

/* Flight animation: a HORIZONTAL jump plays the long-jump ROLL cells
 * ($2a9a(a5), 26 frames, looped for flights longer than the vanilla arc);
 * a vertical jump plays the hop arc's own cells ($309c(a5), last in-arc
 * pose held). Runs through rise AND descent so the roll carries to landing,
 * exactly like the vanilla long jump. */
static void flight_anim(M68KCtx *ctx)
{
    uint32_t tab = ctx->A[5] + (s_roll ? 0x2A9Au : 0x309Cu);
    int16_t cell = (int16_t)MR16(tab + (uint32_t)s_d5);
    if (cell == 0) {
        if (s_roll) { s_d5 = 0; cell = (int16_t)MR16(tab); }  /* loop the roll */
        else          cell = 0;                               /* hold last pose */
    }
    if (cell != 0)
        ctx->D[3] = (ctx->D[3] & 0xFFFF0000u) | (uint16_t)cell;
    if (s_roll || s_d5 < 0x14) s_d5 += 2;
    ctx->D[5] = (ctx->D[5] & 0xFFFF0000u) | (uint16_t)s_d5;
}

/* ── RISE: native body for state $579D84 ──────────────────────────────────── */
static void flight_rise(M68KCtx *ctx)
{
    int16_t x = (int16_t)(uint16_t)ctx->D[1];
    int16_t y = (int16_t)(uint16_t)ctx->D[2];

    wall_feedback(x);
    s_air_frames++;
    phys_jump_cut();
    s_vy += t_gravity();
    s_y_acc += s_vy;
    int dy = s_y_acc >> 8;
    s_y_acc -= dy << 8;
    /* NO head-bonk probe: the engine has no player ceiling collision — the
     * vanilla arcs never test upward (the hop trigger only gates on tile-attr
     * bit4, the terrain pass skips rising frames), and a flat tile!=0 probe
     * false-bonks on nonzero PROFILE tiles (decor/surface rows), which cut
     * the rise to ~6px. Rising through overhangs = the engine's own model
     * (and gives one-way platforms for free). */
    y = (int16_t)(y + dy);
    ctx->D[2] = (ctx->D[2] & 0xFFFF0000u) | (uint16_t)y;

    flight_anim(ctx);

    /* Carried-MM trail record, as the arc does ($579D88..A2, table $5d02). */
    if (ctx->D[4] & (1u << 14)) {
        uint32_t slot = 0x5A4526u
                      + (uint32_t)((MR16(ctx->A[5] + 0xFA2u) & 0xFu) << 6);
        MW16(slot, (uint16_t)(y - (int16_t)MR16(ctx->A[5] + 0x5D02u + (uint32_t)s_d5)));
    }

    int dx = phys_dx(ctx, x, y);
    ctx->D[1] = (ctx->D[1] & 0xFFFF0000u) | (uint16_t)(int16_t)(x + dx);
    s_x_expect = x + dx; s_x_expect_valid = 1;

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

    wall_feedback(x);

    /* bclr #5,d4 ($579F3A) */
    ctx->D[4] &= ~(1u << 5);

    /* $f6e fall-time meter, +2/frame — the landing state's fall-damage input.
     * Past ramp index 6 vanilla clears the anim cell ($579F50). */
    uint16_t f6e = MR16(ctx->A[5] + 0xF6Eu);
    if (s_roll && s_fly)
        flight_anim(ctx);                       /* roll through the descent */
    else if ((uint16_t)(f6e + 2) >= 6)
        ctx->D[3] = (ctx->D[3] & 0xFFFF0000u);  /* vanilla fall anim clear */
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
    s_x_expect = x + dx; s_x_expect_valid = 1;
}

/* ── State overrides ──────────────────────────────────────────────────────── */

/* $579D84 — entered by: our JUMP trigger (takeoff mark), the vanilla UP-hop
 * commit ($57E526, $f78==0 = from grounded -> SUPPRESS), or the bounce pad
 * ($57AB96, from the fall state -> native bounce). */
void native_pf_hop(M68KCtx *ctx)
{
    extern int g_pickup_log;
    if (g_pickup_log)
        fprintf(stderr, "[pf] hop  prev=%08X takeoff=%d fly=%d vy=%d\n",
                MR32(ctx->A[5] + 0xF78u), s_takeoff, s_fly, s_vy);
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

/* $579A62 — the vanilla fire+dir long jump; player-initiated only. On a
 * MODERN device the dedicated JUMP button is the one jump and FIRE stays free
 * for interactions: revert to grounded. On a VANILLA device (no jump button)
 * fire+dir IS the jump (user, 2026-06-12): the commit becomes a native
 * takeoff, with FIRE as the held button for the variable-height cut. */
void native_pf_lj(M68KCtx *ctx)
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579A62(ctx); return; }
    if (vanilla_fire()) {
        flight_start(ctx, 0);
        s_fire_jump = 1;
        flight_rise(ctx);
        return;
    }
    MW32(ctx->A[0], 0);
}

/* $579E02 — the vanilla UP+direction DIAGONAL hop arc (third jump family,
 * committers $57E7B6/$57EA0E, both grounded; tables anim $3058 / vy $2d62 /
 * vx $2d20, grunt at d5==$14). Player-initiated only: suppress like the
 * long jump. (Missed in the first BENMOTION pass — "direction + up still
 * hops", user 2026-06-12.) */
extern void gfn_gpl_579E02(M68KCtx *ctx);
void native_pf_diag(M68KCtx *ctx)
{
    extern int g_pickup_log;
    if (g_pickup_log)
        fprintf(stderr, "[pf] diag prev=%08X takeoff=%d fly=%d\n",
                MR32(ctx->A[5] + 0xF78u), s_takeoff, s_fly);
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579E02(ctx); return; }
    MW32(ctx->A[0], 0);
}

/* $579F3A — fall. Fresh entries (walk off a ledge, knockback writers) seed the
 * model; a hand-off from our rise keeps vx/vy. */
void native_pf_fall(M68KCtx *ctx)
{
    if (!pf_active(ctx)) { s_fly = s_track = 0; gfn_gpl_579F3A(ctx); return; }

    if (!s_fly) {
        s_fly = 1; s_track = 0;
        s_x_expect_valid = 0;
        s_cut_done = 1;
        s_y_acc = s_x_acc = 0;
        s_vy = 0; s_steer = 0;
        s_roll = 0; s_d5 = 0;
        /* No ground-velocity tracking yet (Stage 4): a held direction carries
         * the walk into the fall. */
        int cap = pf_vx_cap(ctx);
        if (hw_joy_right())      s_vx =  cap;
        else if (hw_joy_left())  s_vx = -cap;
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

    s_steer = air_vel_step(s_steer, t_vx_max());
    s_x_acc += s_steer;
    int sdx = s_x_acc >> 8;
    s_x_acc -= sdx << 8;
    if (sdx) {
        int x_arc = x_in + dxv;
        int16_t y = (int16_t)(uint16_t)ctx->D[2];
        int lead = x_arc + sdx + (sdx > 0 ? 8 : -8);
        if (tile_solid(ctx, lead, y - BODY_LO) || tile_solid(ctx, lead, y - BODY_HI)) {
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

/* ── FALL DAMAGE scaling ("fall_damage": vanilla | light | none) ──────────────
 * Vanilla applies fall damage in exactly two places, both wrapped here: the
 * landing-impact state $579F86 (table $3b9a(a5)[$f6e/2] subtracted from the
 * energy word $1c.w) and the terrain pass's inline carry-arc contact
 * ($57AAE6, same table). The wrappers measure the energy delta across the
 * recompiled body and give back half (light) or all (none) of it — the
 * body's anim/SFX/state side effects stay untouched. Independent of the
 * platformer knob (it's an accessibility option for vanilla physics too). */
static int fall_dmg_mode(void)   /* 0 = vanilla, 1 = light, 2 = none */
{
    char buf[16];
    if (!pc_cfg_show("fall_damage", buf, sizeof buf, NULL) || !buf[0]) return 0;
    if (!strcasecmp(buf, "light")) return 1;
    if (!strcasecmp(buf, "none"))  return 2;
    return 0;
}

static void fall_dmg_scale(M68KCtx *ctx, int mode, uint16_t energy_before)
{
    int dmg = (int16_t)(energy_before - (uint16_t)MR16(0x1Cu));
    if (dmg <= 0) return;                       /* no damage (or a refill) */
    int keep = (mode == 1) ? (dmg + 1) / 2 : 0;
    MW16(0x1Cu, (uint16_t)(energy_before - keep));
}

extern void gfn_gpl_579F86(M68KCtx *ctx);
void native_pf_landing_impact(M68KCtx *ctx)
{
    int mode = fall_dmg_mode();
    uint16_t before = MR16(0x1Cu);
    gfn_gpl_579F86(ctx);
    if (mode) fall_dmg_scale(ctx, mode, before);
}

/* ── JUMP trigger: wraps the per-frame terrain pass $57A934 ───────────────── */
void native_pf_collision(M68KCtx *ctx)
{
    int dmg_mode = fall_dmg_mode();
    uint16_t energy_before = MR16(0x1Cu);
    gfn_gpl_57A934(ctx);
    if (dmg_mode) fall_dmg_scale(ctx, dmg_mode, energy_before);

    int j = jump_input();
    int edge = j && !s_jump_prev;
    s_jump_prev = j;

    extern int g_pickup_log;
    if (g_pickup_log)
        fprintf(stderr, "[pf] trig j=%d edge=%d st=%08X\n",
                j, edge, MR32(ctx->A[5] + 0xF70u));

    if (!pf_active(ctx)) { s_takeoff = 0; return; }

    uint32_t st = MR32(ctx->A[5] + 0xF70u);
    if (!is_air_state(st)) { s_fly = s_track = 0; }   /* landed / left the air */

    if (!edge) return;
    /* Grounded only. One tolerated exception: a vanilla jump commit (UP-hop
     * $579D84 or UP+dir diagonal $579E02) can land in the same frame BEFORE
     * this trigger (UP-as-jump on a vanilla device) — normalize it to our
     * canonical rise state and mark the takeoff ours so the suppress
     * branches don't eat it. */
    if ((st == ST_HOP || st == 0x579E02u) && !s_fly) {
        MW32(ctx->A[5] + 0xF70u, ST_HOP);
        s_takeoff = 1;
        return;
    }
    if (st != 0) return;
    if (MR16(ctx->A[5] + 0xF82u) == 0x14) return;     /* vanilla's own gate */
    int x = (int16_t)MR16(ctx->A[5] + 0x10A6u);
    int y = (int16_t)MR16(ctx->A[5] + 0x10A8u);
    if (tile_nojump(ctx, x, y)) return;               /* no-jump tile (water etc.) */

    MW32(ctx->A[5] + 0xF70u, ST_HOP);
    s_takeoff = 1;
}
