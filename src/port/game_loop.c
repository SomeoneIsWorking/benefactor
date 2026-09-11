/* game_loop.c – Native host game loop, state machine, and lifecycle
 *
 * Override implementations live in src/port/overrides/.
 */
#include "engine/disk_boot.h"
#include "engine/gameplay_handoff.h"
#include "engine/hw.h"
#include "engine/overlay_load.h"
#include "harness/trace.h"
#include "port/config.h"
#include "port/frame_accounting.h"
#include "port/guest_trace.h"
#include "port/guest_vectors.h"
#include "port/input.h"
#include "port/overlay_ui.h"
#include "port/port.h"
#include "port/port_internal.h"
#include "runtime/guest_runtime.h"
#include <pthread.h>
#include <setjmp.h>

#ifdef HARNESS_BUILD
#include "harness/puae_state.h"
#endif

/* ── Globals ──────────────────────────────────────────────────────────────── */
uint8_t *g_chip = NULL;
static int s_harness_mode = 0;

/* Single source of truth for every piece of state that constitutes a savestate
 * (M68K registers, coroutine, custom-chip shadows, audio, bank-routing flags,
 * state-machine flags). All the legacy names (s_regs, s_dmacon, g_overlay_active,
 * …) are macros that alias fields on this instance — see game_state.h. */
GameState g_state;

/* Apply the non-zero defaults that the original-storage initializers used to
 * carry. Called at startup (and could be called by a "reset" sequence). */
static void pc_state_reset_defaults(void) {
    memset(&g_state, 0, sizeof g_state);
    s_diwstrt = 0x2C81;
    s_diwstop = 0x2CC1;
    g_gameplay_entry = 0x00003330u;
}

void pc_set_harness_mode(int on) {
    s_harness_mode = on;
    hw_set_no_pace(on);
}

/* Per-frame audio: one displayed frame's worth of samples (50Hz @ 22050Hz). */
#define PC_AUD_SPF 441
/* The gameplay music player ($53A2/$59BFA6) is driven by the CIA-B timer, which
 * fires ~3x per displayed frame — delivering its ISR once/frame plays the song
 * ~3x too slow (measured vs PUAE, see project-native-music-player). The intro
 * and title/menu players advance once per frame. So gameplay gets GP_MUSIC_TICKS
 * sub-frame deliveries; everything else gets one. (TODO: derive the exact count
 * from the CIA-B timer period instead of this constant.) */
#define GP_MUSIC_TICKS 3

/* INTENA bits used to decide whether an installed interrupt vector may fire,
 * mirroring the CPU's interrupt-priority logic. INTEN (bit14) is the master
 * enable; a level only fires when master AND that level's source bit are set. */
#define INTENA_MASTER 0x4000u
#define INTENA_LVL6 0x2000u /* EXTER — CIA-B timer (music/timer) */
#define INTENA_LVL3 0x0070u /* VERTB | COPER | BLIT (vblank/copper) */

/* How a piece of guest code gives control back. Entering it the wrong way is
 * silently wrong, not a type error: an RTS-terminated leaf entered as an
 * interrupt pops the exception frame's SR word as the high half of its return
 * address and jumps to $SR0000 (observed: $20000000, $27000000). */
typedef enum { GUEST_ENTRY_RTE, GUEST_ENTRY_RTS } GuestEntry;

/* Call guest code, saving/restoring all registers around it (used for per-frame
 * IRQ delivery). The IRQ runs on the MAIN thread while the game thread is parked
 * at its vblank wait, so it shares s_game_ctx/g_mem race-free; the save/restore
 * mirrors the CPU stacking registers across an interrupt. */
static void call_fn_as(M68KCtx *ctx, uint32_t addr, GuestEntry entry) {
    benefactor_log_write(BENEFACTOR_LOG_TRACE, "irq", "-> $%06X", addr);
    uint32_t sa[8], sd[8];
    uint16_t sr = ctx->sr ? *ctx->sr : 0;
    for (int i = 0; i < 8; i++)
        sa[i] = ctx->A[i];
    for (int i = 0; i < 8; i++)
        sd[i] = ctx->D[i];
    if (entry == GUEST_ENTRY_RTS)
        rt_call(ctx, ctx->image, addr);
    else
        rt_call_interrupt(ctx, ctx->image, addr);
    for (int i = 0; i < 8; i++)
        ctx->A[i] = sa[i];
    for (int i = 0; i < 8; i++)
        ctx->D[i] = sd[i];
    if (ctx->sr)
        *ctx->sr = sr;
    benefactor_log_write(BENEFACTOR_LOG_TRACE, "irq", "<- $%06X", addr);
}

/* ── Game loop (single path: disk-boot coroutine) ───────────────────────────── */

int pc_run(void) {
    PC_LOG("entering game loop\n");
    /* The standalone is nothing more than a loop over the SAME per-frame advance
     * the harness drives (pc_step). No driver-specific game logic: pc_step does
     * the complete frame (game + IRQ/music + audio) so the PC behaves identically
     * however it is driven. */
    while (hw_running) {
        if (pc_step())
            break;
    }
    PC_LOG("game loop exited\n");
    return 0;
}

int pc_step_threaded(void);
void pc_music_tick(void);

/* The single per-frame advance. Runs the game's own flow for one displayed frame
 * (presenting + delivering the vblank IRQ at its yields), then delivers the
 * level-6 music ISR at its per-screen rate and renders this frame's audio. Used
 * identically by pc_run (standalone) and the harness STEP_PC — no behavioural
 * difference between the two. */
/* Debug: force LEVEL COMPLETE (the teleport win). The win is gated by bit5 of
 * the $10AC(a5) flags byte ($0057FEBE bit5 = 0x20): the per-level main loop at
 * $5770F8 reaches `$5771DE: btst #5,$10ac; bne $5771FE`, which falls into the
 * level-complete sequence at $577218 (the LEVEL COMPLETE banner, then stop
 * audio at $5772A0, then $5772D6 "wait for fire to continue"). Setting bit5
 * makes the next main-loop pass take the win exactly as the real teleport-out
 * would. */
void pc_debug_complete_level(void) {
    if (!g_gameplay_active || !g_mem)
        return;
    g_mem[0x0057FEBEu] |= 0x20;
}

/* Debug: force GAME OVER. Death sets bit15 of the end-of-level flags word
 * $10AC(a5) = $0057FEBE; the state code routes that to the game-over handler
 * ($578C3E, $1E!=8 → banner → CONTINUE/GAME OVER menu). */
void pc_debug_game_over(void) {
    if (!g_gameplay_active || !g_mem)
        return;
    g_mem[0x0057FEBEu] |= 0x80;
}

/* Native level-select. The gameplay dispatcher at $5779AA reads $20.w
 * (low chip mem), does (level-1)*4, and indexes the 60-entry level table
 * at $57782E to pick (world, level_in_world). So $20.w == N selects level
 * N directly.
 *
 * BUT the title's password-screen code at $003A40 clamps $20.w EVERY frame
 * it runs (and at $003A04 unconditionally writes 1 when the password sub-
 * menu is entered), so any pre-fire poke to $20.w gets reverted. The right
 * moment to apply the user's selection is INSIDE native_overlay_loader_reloc
 * (the $150 override), which fires once when the title's "start game" path
 * jumps to $150 — after the title is done validating. Store the selection
 * here; the loader override reads it.
 *
 * 0 = no override (use the title's natural value, level 1 at fresh boot). */
/* g_pc_start_level moved to g_state (see game_state.h). */

void pc_set_start_level(int n) {
    if (n < 1)
        n = 1;
    int max = pc_num_levels_ui(); /* 60 + Disk.4 extras when present */
    if (n > max)
        n = max;
    g_pc_start_level = n;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[level-select] start level := %d (applied at $150 hand-off)\n", n);
}

/* Read pending level-select choice (0 = none / pass-through). */
int pc_get_start_level(void) { return g_pc_start_level > 0 ? g_pc_start_level : 1; }

/* Deferred save/load: SDLK_S / SDLK_D fire from inside hw_present_frame, on the
 * MAIN thread. The keys set these flags and the work happens at the next pc_step
 * boundary, where the game thread is parked at its vblank wait (its M68K context
 * quiescent). */
int g_pc_pending_save = 0;
int g_pc_pending_load = 0;

/* Set by pc_request_level_restart (pause-menu Retry, native game-over → level
 * card). When the restart actually fires in pc_step_threaded, re-decrunch the
 * gameplay overlay + re-pin the card-render sentinels first, so the entry starts
 * from pristine chip RAM regardless of what the previous screen (e.g. the
 * game-over menu, which overwrites the display page $38628 + the $3e/$184/$100
 * card chain) left behind. Done on the main loop with the game thread parked. */
int g_pc_restart_reinit = 0;

/* ── Game thread + ping-pong handoff ────────────────────────────────────────────
 * The game runs on its own OS thread; the SDL main thread paces frames. They
 * hand off cooperatively via a condvar so EXACTLY ONE runs at a time (no data
 * races on s_game_ctx/g_mem, deterministic frame-by-frame). This is the ucontext
 * coroutine re-expressed with real threads + a mutex, as the user requested.
 *
 *   game thread:  rt_call($3000) ... hw_vblank_wait() -> game_thread_yield()
 *                   [hands the turn to main, blocks until released]
 *   main thread:  pc_step_threaded(): release game one frame, wait until it
 *                   parks, then present (SDL must be on main) + deliver IRQ.
 *
 * s_turn: 1 = the game thread runs, 0 = the main thread runs (game parked). */
static uint32_t s_game_entry = 0x003000u; /* address the game thread runs from */
static int s_game_resume = 0;             /* 1 = re-enter via rt_resume (savestate load) */
/* s_game_done lives on g_state (macro): set when the game flow returns from rt_call */

static pthread_t s_game_thread;
static int s_game_thread_live = 0;
static pthread_mutex_t s_hand_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_hand_cv = PTHREAD_COND_INITIALIZER;
static int s_turn = 0;
static int s_game_exit_req = 0;
static __thread int s_is_game_thread = 0;

int pc_step_threaded(void);

/* The game thread's per-frame wait. Wired to hw_vblank_wait via g_hw_vblank_yield
 * (the same seam the old coroutine used). Hands the turn to main and blocks until
 * the host releases it. An IRQ handler that hits a wait runs on the MAIN thread
 * (call_fn_as) — it must NOT block here (would deadlock), so non-game threads return
 * immediately. On a restart request the parked thread exits cleanly. */
static int game_thread_yield(void) {
    pc_note_wait_reached();
    if (!s_is_game_thread) {
        pc_note_wait_refused();
        return 0;
    }
    pc_note_wait_parked();
    pthread_mutex_lock(&s_hand_mtx);
    s_turn = 0; /* hand the turn back to main */
    pthread_cond_broadcast(&s_hand_cv);
    while (s_turn == 0 && !s_game_exit_req)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    int exit_req = s_game_exit_req;
    pthread_mutex_unlock(&s_hand_mtx);
    if (exit_req)
        pthread_exit(NULL);
    return 1;
}

/* Which flow is executing guest code right now. The beam accounting needs it:
 * a frame boundary can only be taken by the parkable game flow, so a screen
 * that stalls has to say whether the guest work was on that flow at all. */
int pc_on_game_thread(void) { return s_is_game_thread; }

static void *game_thread_main(void *arg) {
    (void)arg;
    s_is_game_thread = 1;
    /* Wait for the host's first release before touching anything. */
    pthread_mutex_lock(&s_hand_mtx);
    while (s_turn == 0 && !s_game_exit_req)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    int exit_req = s_game_exit_req;
    pthread_mutex_unlock(&s_hand_mtx);
    if (exit_req)
        return NULL;

    if (s_game_resume)
        rt_resume(&s_game_ctx, s_game_ctx.image, s_game_entry);
    else
        rt_call(&s_game_ctx, s_game_ctx.image, s_game_entry);

    /* The cold-start flow is an endless state machine. A screen hand-off unwinds
     * it deliberately (the host then restarts this thread on the next image);
     * any other return is a fault, so dump the instructions that led there
     * rather than making the next run reproduce it. */
    {
        int handed_off = g_enter_gameplay || g_pc_restart_reinit || g_pc_enter_title;
        benefactor_log_write(handed_off ? BENEFACTOR_LOG_DEBUG : BENEFACTOR_LOG_WARNING, "game",
                             "[game] flow returned from $%06X (last insn $%06X)%s", s_game_entry,
                             rt_get_last_insn(), handed_off ? " — screen hand-off" : "");
        if (!handed_off)
            pc_log_retired_instructions("game");
    }
    pthread_mutex_lock(&s_hand_mtx);
    s_game_done = 1;
    s_turn = 0;
    pthread_cond_broadcast(&s_hand_cv);
    pthread_mutex_unlock(&s_hand_mtx);
    return NULL;
}

/* Spawn a fresh game thread (blocked at its initial wait until the first frame
 * release). Caller has already set s_game_ctx + s_game_entry + bank flags. */
static void game_thread_spawn(void) {
    s_game_done = 0;
    s_game_exit_req = 0;
    s_turn = 0;
    pthread_create(&s_game_thread, NULL, game_thread_main, NULL);
    s_game_thread_live = 1;
}

/* Cooperatively stop the running game thread (parked at a wait or its initial
 * wait) and join it. Only valid to call from the main thread with the game
 * parked (s_turn == 0). */
static void game_thread_stop(void) {
    if (!s_game_thread_live)
        return;
    pthread_mutex_lock(&s_hand_mtx);
    s_game_exit_req = 1;
    s_turn = 1; /* wake it so it observes the exit flag */
    pthread_cond_broadcast(&s_hand_cv);
    pthread_mutex_unlock(&s_hand_mtx);
    pthread_join(s_game_thread, NULL);
    s_game_thread_live = 0;
}

/* Release the game thread to run for exactly one frame (until its next vblank
 * wait), then block until it parks (or the flow finishes). On return the game is
 * parked and the main thread owns all shared state. */
static void game_thread_run_one_frame(void) {
    if (!s_game_thread_live)
        return;
    pthread_mutex_lock(&s_hand_mtx);
    s_turn = 1;
    pthread_cond_broadcast(&s_hand_cv);
    while (s_turn == 1 && !s_game_done)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    pthread_mutex_unlock(&s_hand_mtx);
}

/* ── Credits fire-skip ───────────────────────────────────────────────────────
 * During the victory credits, the first FIRE press shows a confirm toast; a
 * second press while OUR confirm window is still open skips the credits back
 * to the main-menu poster (pc_request_cold_restart — the same exit the pause
 * menu's EXIT TO MENU uses, and safe in the same main-thread spot). The window
 * is tracked with its own countdown (mirroring the toast's 160 frames) so an
 * unrelated toast can never arm the skip. */
static void pc_credits_skip_tick(void) {
    static int prev_fire, confirm_frames;
    int fire = pc_input_active(PI_FIRE);
    int edge = fire && !prev_fire;
    prev_fire = fire;
    if (!g_credits_active) {
        confirm_frames = 0;
        return;
    }
    if (confirm_frames > 0)
        confirm_frames--;
    if (!edge)
        return;
    if (confirm_frames > 0) {
        confirm_frames = 0;
        pc_toast_show("", 0); /* drop the confirm toast */
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] credits: fire-skip -> main menu\n");
        pc_request_cold_restart();
        return;
    }
    pc_toast_show("PRESS FIRE AGAIN TO SKIP", 0);
    confirm_frames = 160; /* match the toast lifetime */
}

/* One frame of music + audio, on REAL time (extracted from pc_step so the
 * freecam-paused freeze can keep the soundtrack running). At 1x
 * hw_audio_frame_due() is always true (one audio frame per game frame — the
 * original, deterministic path the harness compares against). At 2x/4x/turbo
 * it gates the block to the wall-clock 50Hz grid, so the music ISR keeps its
 * normal tempo and the SDL queue receives exactly what it drains — speeding
 * the game no longer speeds (or garbles) the soundtrack. */
static void pc_audio_frame(void) {
    if (!hw_audio_frame_due())
        return;

    short ab[PC_AUD_SPF * 2];
    if (g_overlay_active || g_gameplay_active || g_credits_active) {
        /* The overlay music player (menu, level card, gameplay, credits — all the
         * same CIA-timer-driven $53A2 player) advances ~3x per displayed frame;
         * one tick/frame plays it too slow. The intro crawl uses a different
         * player ($55A0) that advances once/frame (handled in the else branch).
         * Render audio between ticks so each sub-frame note is actually heard.
         * NOTE: gameplay is g_gameplay_active (overlay=0) — it MUST be included
         * here or it gets zero music ticks (no sound past the menu). */
        int ticks = GP_MUSIC_TICKS;
        int done = 0;
        for (int k = 0; k < ticks; k++) {
            pc_music_tick();
            int chunk = (PC_AUD_SPF * (k + 1) / ticks) - done;
            hw_audio_render(ab + done * 2, chunk);
            done += chunk;
        }
    } else {
        hw_audio_render(ab, PC_AUD_SPF);
    }
    hw_audio_queue(ab, PC_AUD_SPF);
}

int pc_step(void) {
    /* Service any deferred pause-menu action (Resume/Retry/ExitToMenu/Quit)
     * before anything else — we're on the MAIN thread here and the game thread
     * is parked, so it's safe to stop/respawn the game thread if needed. */
    {
        pc_pause_tick();
    }
    pc_credits_skip_tick();

    if (g_pc_pending_load) {
        g_pc_pending_load = 0;
        if (pc_loadstate("logs/savestate.bin") == 0)
            pc_toast_show("STATE LOADED", 0);
        else
            pc_toast_show("LOAD FAILED (no/old savestate)", 1);
    }
    if (!hw_running)
        return 1;

    /* While paused, freeze the game thread entirely — don't release it.
     * Still call hw_present_frame so the pause overlay stays visible and
     * SDL events keep flowing (so the user can navigate the menu). */
    if (pc_pause_active() || pc_freecam_paused()) {
        /* Both pause modes freeze the GAME but not the SOUNDTRACK — the
         * music ISR is vblank/CIA-driven, independent of the game loop (same
         * as the original's pause). */
        pc_audio_frame();
        if (g_harness_prerender_hook)
            g_harness_prerender_hook();
        /* hw_present_paused_frame, not hw_present_frame: with the guest parked
         * the beam barely moves, so the one-present-per-beam-frame rule would
         * decline nearly every call and skip the 50 Hz pacing inside it. This
         * loop would then run at host speed and take the music with it —
         * pc_audio_frame above ticks the music player once per pass. */
        if (hw_present_paused_frame() != 0)
            return 1;
        if (g_harness_frame_hook)
            g_harness_frame_hook();
        return 0;
    }

    {
        static uint64_t last_exit_cycles = 0;
        const uint64_t now = rt_get_guest_cycles();
        pc_account_iteration(last_exit_cycles ? now - last_exit_cycles : 0);
        last_exit_cycles = now;
    }
    hw_watchdog_arm("PC", 2); /* catch an infinite loop in one frame */
    uint64_t perf_t = hw_perf_now_us();
    int r = pc_step_threaded(); /* release the game thread for one frame */
    hw_perf_acc(&g_hw_perf.game_us, perf_t);
    hw_watchdog_disarm();
    if (g_pc_pending_save) {
        g_pc_pending_save = 0;
        const char *reason = NULL;
        if (!pc_savestate_allowed(&reason))
            pc_toast_show(reason ? reason : "Cannot save here", 1);
        else if (pc_savestate("logs/savestate.bin") == 0)
            pc_toast_show("STATE SAVED", 0);
        else
            pc_toast_show("SAVE FAILED", 1);
    }

    pc_audio_frame();
    return r;
}

/* ── Native disk boot (game-thread flow) ─────────────────────────────────────
 * Boot from the original disk images, no snapshot: decrunch Disk.1's crunched
 * main game (ATN!) into $3000, then run the original retail-image cold-start ($3000) on the
 * game thread — each frame wait (hw_vblank_wait) parks it and pc_step_threaded
 * releases it one frame at a time. The game drives its real flow (intro → logos
 * → title → menu → gameplay). */
/* s_game_ctx is the M68K register file (on g_state via game_state.h). */

/* Deliver the game's level-6 CIA-B timer interrupt service routines. On real
 * hardware the timer IRQ alternates two handlers on the $78 vector:
 *   $3160 → $0055A0  — music player + animation/auto-advance frame counters.
 *   $0058C2          — copies the audio shadow ($69F6+) into the Paula regs.
 * The host runtime delivers these service routines once per frame, matching
 * the rate programmed by the game. $55A0 is the RTS-terminated leaf of the
 * $3160 wrapper; both are
 * the game's own code (no fudged values). */
/* True when the game has the given interrupt level enabled (master INTEN bit +
 * that level's source bit), i.e. the real CPU would actually take the interrupt.
 * A vector still holding the PREVIOUS screen's handler (e.g. the title's $3532 /
 * $5694 during a level load) does not fire while its level is masked — the load
 * polls the beam precisely because it has interrupts off. This replaces the old
 * "is the vector in the gameplay bank?" band-aid with the real HW condition. */
static int irq_level_enabled(uint16_t levelbits) {
    uint16_t ie = hw_get_intena();
    return (ie & INTENA_MASTER) && (ie & levelbits);
}

/* Deliver the level-6 (CIA-B timer) music ISR once. Called pc_step's per-screen
 * number of times per frame. Gated on the interrupt being enabled so a stale
 * vector from a previous screen doesn't run during a masked load. */
/* Music kill-switch for the wrong-SFX investigation. When set, the LVL6 music
 * ISR is NOT delivered, so whatever music player is installed at $78 stops
 * advancing. SFX that is triggered independently of the music ISR keeps
 * playing; SFX pumped *by* the ISR goes silent — this disambiguates the two.
 * Set from BENEFACTOR_MUTE_MUSIC at bring-up; toggleable at the REPL ("mute"). */
int g_mute_music = 0;

static void coro_call_vector(PcOwner owner, uint32_t addr);

void pc_music_tick(void) {
    if (g_mute_music)
        return;
    if (!g_overlay_active && !g_gameplay_active && !g_credits_active)
        return;
    if (!irq_level_enabled(INTENA_LVL6))
        return;
    const uint32_t v6 = guest_vector_handler(GUEST_VECTOR_LEVEL6_TIMER);
    if (v6)
        /* Through the SAME door as the frame loop's own delivery. This used to
         * call the vector directly, which differed in two ways that both bit:
         * it recorded no owner, so the music player's cycles were charged to
         * the game flow and its deliveries were never counted (this path runs
         * more often than the frame loop's, so that is most of an intro
         * screen's time misattributed); and it did not lend the interrupt a
         * copy of the blitter registers, so a delivery landing between a
         * BLTxxx write and BLTSIZE could merge two blits into one runaway blit
         * — the hazard issue 0007 describes, on the busier of the two paths.
         * See docs/issues/0008. */
        coro_call_vector(PC_OWNER_LEVEL6_TIMER, v6);
}

/* Deliver one interrupt vector, accounting its guest cycles to its level. */
static void coro_call_vector_as(PcOwner owner, uint32_t addr, GuestEntry entry) {
    const uint64_t before = rt_get_guest_cycles();
    /* The blitter registers are one shared set. If the game flow parked
     * mid-sequence, this vector's own blits would overwrite its half-written
     * setup and the two would merge into one runaway blit — so lend the
     * interrupt a copy and hand the flow's back. See docs/issues/0007. */
    uint16_t blt[HW_BLT_REGS];
    const int mid_blit = hw_blit_setup_open();
    if (mid_blit)
        hw_blit_regs_save(blt);
    pc_set_running_owner(owner);
    call_fn_as(&s_game_ctx, addr, entry);
    pc_set_running_owner(PC_OWNER_FLOW);
    if (mid_blit)
        hw_blit_regs_restore(blt);
    pc_account_owner(owner, rt_get_guest_cycles() - before);
}

/* An installed vector: entered as an interrupt, returns through RTE. */
static void coro_call_vector(PcOwner owner, uint32_t addr) {
    coro_call_vector_as(owner, addr, GUEST_ENTRY_RTE);
}

/* How many links of the self-rewriting level-6 vector chain to deliver in one
 * frame. The intro's chain is two deep ($3160 -> $58C2); the bound stops a
 * handler that rewrites $78 unboundedly from spinning the frame loop. */
#define PC_IRQ6_CHAIN_MAX 4

static void coro_deliver_timer_irq(void) {
    if (g_gameplay_active || g_overlay_active || g_credits_active) {
        /* Gameplay / overlay / credits each install their own level-3 ($6c,
         * vblank) and level-6 ($78, music/timer) handlers; deliver whatever is
         * installed, but only while the game has that level enabled, so a stale
         * vector from the previous screen doesn't run during a masked load.
         * Level-3 fires once per displayed frame here; level-6 is delivered by
         * pc_music_tick at the per-screen sub-frame rate, so firing it here too
         * would over-count it. */
        uint32_t v3 = guest_vector_handler(GUEST_VECTOR_LEVEL3_VBLANK);
        if (v3 && irq_level_enabled(INTENA_LVL3))
            coro_call_vector(PC_OWNER_LEVEL3_VBLANK, v3);
        return;
    }
    /* Intro and title: deliver exactly what the game installed at the vectors.
     *
     * The level-6 handlers CHAIN BY REWRITING THEIR OWN VECTOR: $5892 ends with
     * `addi.l #$30,$78(a0)` (a0=0), moving $78 on to $58C2, the routine that
     * copies the audio shadow ($69F6+) into Paula. So the second handler is
     * reached through $78 like the first, and nothing needs to name it here.
     *
     * Do NOT call $0055A0 / $0058C2 directly in place of the vectors. $55A0 is
     * a tail-branch dispatcher whose chain reaches $5892's RTE, so entering it
     * as a subroutine returns into the game flow's own stack and hangs the boot
     * in the one-frame wait at $3732; and once a screen has been loaded over
     * those bytes they are somebody else's data ($58C2 read as $FFFF at frame
     * 900 and trapped). See docs/issues/0008. */
    const uint32_t v3 = guest_vector_handler(GUEST_VECTOR_LEVEL3_VBLANK);
    if (v3)
        coro_call_vector(PC_OWNER_LEVEL3_VBLANK, v3);
    /* Deliver the WHOLE level-6 chain, not just its first link. Because the
     * handlers chain by rewriting $78, delivering one link per frame ran the
     * music driver on every OTHER frame, and the intro tune advanced at 56% of
     * the reference's rate (145 sample-pointer moves against 259 over the
     * 6290-frame crawl). The reference calls both leaves every frame.
     *
     * Once per frame is not a rate the game asked for either: it programs CIA-B
     * timer A with latch 384 against a 709379 Hz E-clock, i.e. ~37 deliveries a
     * PAL frame, so on real hardware every link of a two-deep chain certainly
     * runs within one frame.
     *
     * Each DISTINCT vector runs at most once per frame. The chain is a ring —
     * $3160 rewrites $78 to $58C2 and $58C2 rewrites it back — so walking it
     * until the vector stops moving goes round twice and ran the music driver
     * twice a frame instead of once (525 sample-pointer moves against 259). */
    uint32_t delivered[PC_IRQ6_CHAIN_MAX];
    unsigned links = 0;
    for (;;) {
        const uint32_t v6 = guest_vector_handler(GUEST_VECTOR_LEVEL6_TIMER);
        if (!v6 || links >= PC_IRQ6_CHAIN_MAX)
            break;
        int seen = 0;
        for (unsigned i = 0; i < links && !seen; i++)
            seen = delivered[i] == v6;
        if (seen)
            break;
        delivered[links++] = v6;
        coro_call_vector(PC_OWNER_LEVEL6_TIMER, v6);
    }
}

/* Common bring-up shared between the full-boot path and the direct-to-gameplay
 * shortcut: hardware + runtime + disks + boot loader + override registration.
 * Returns 0 on success, -1 on failure. */
static int pc_common_bringup(const char **disks, int n_disks) {
    pc_state_reset_defaults(); /* zero g_state + non-zero defaults */
    if (hw_init("Benefactor (disk boot)", NULL, 0) < 0)
        return -1;
    if (rt_init(NULL, 0, 0x080000) < 0)
        return -1; /* allocate g_mem, no chip dump */
    g_chip = g_mem;
    if (disk_boot_open(disks, n_disks) < 0) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] could not open disk images\n");
        return -1;
    }
    /* Boot loader step: Load(Disk.1 @$1880, $2442E → $3000) + Decrunch($3000).
     * Shared with the bank dumper via overlay_load_main(). */
    overlay_load_main();
    pc_register_overrides();
    pc_register_wait_idioms(BENEFACTOR_IMAGE_MASK_MAIN, 0u, (uint32_t)RT_MEM_SIZE);
    g_hw_vblank_yield = game_thread_yield; /* hw_vblank_wait parks the game thread */
    g_hw_frame_audio = pc_audio_frame;     /* a frame reached inside an IRQ still owes audio */
    g_hw_pc_owns_present = 1;
    {
        g_native_render_delay = pc_cfg_int("render_delay", 1);
    } /* blitter-latency model (frames) */
    {
        g_mute_music = pc_cfg_bool("mute_music", 0);
    } /* SFX-isolation kill-switch */
    return 0;
}

/* Reset to a fresh cold start ($3000): stop any running game thread, clear the
 * M68K context, and spawn a new game thread parked at its initial wait. */
static void pc_cps_reset(void) {
    game_thread_stop();
    memset(&s_game_ctx, 0, sizeof s_game_ctx);
    rt_context_reset(&s_game_ctx, BENEFACTOR_IMAGE_MAIN);
    s_game_entry = 0x003000u;
    s_game_resume = 0;
    game_thread_spawn();
}

/* Savestate LOAD resume: re-enter the steady-gameplay cycle at $577114 with the
 * restored interpreter state. No native stack is restored; the cycle reads its
 * state from the guest memory and CPU owner. */
void pc_resume_gameplay_thread(void) {
    game_thread_stop();
    rt_reset_callstack();
    g_pc_screen = PC_SCR_GAMEPLAY;
    s_game_entry = 0x00577114u;
    s_game_resume = 1;
    game_thread_spawn();
}

/* (Re)start the flow at an explicit entry with a given register/bank setup —
 * used by exit-to-menu (title $3330) and direct-to-gameplay ($577000). Stops the
 * current game thread and spawns a fresh one parked at its initial wait. Safe to
 * call from the main thread with the game parked. */
static void pc_cps_start_at(uint32_t entry, uint32_t a5, int gameplay, uint32_t d5, uint32_t d6) {
    game_thread_stop();
    memset(&s_game_ctx, 0, sizeof s_game_ctx);
    s_game_entry = entry;
    s_game_resume = 0;
    g_pc_screen = gameplay ? PC_SCR_GAMEPLAY : PC_SCR_OVERLAY;
    rt_context_reset(&s_game_ctx, gameplay ? BENEFACTOR_IMAGE_GAMEPLAY
                                           : (g_credits_active ? BENEFACTOR_IMAGE_CREDITS
                                                               : BENEFACTOR_IMAGE_TITLE));
    s_game_ctx.A[5] = a5;
    s_game_ctx.A[6] = 0x00DFF000u;
    s_game_ctx.A[7] = 0x00080000u;
    s_game_ctx.D[5] = d5;
    s_game_ctx.D[6] = d6;
    game_thread_spawn();
}

int pc_init_from_disk(const char **disks, int n_disks) {
    if (pc_common_bringup(disks, n_disks) < 0)
        return -1;
    /* SKIP INTRO (OPTIONS → MORE): boot straight to the poster/main menu —
     * the exact entry "Exit to main menu" uses ($003330 attract, gp a5=$511E),
     * with the title overlay loaded the same way. */
    if (pc_cfg_bool("skip_intro", 0)) {
        native_overlay_load();
        pc_cps_start_at(0x00003330u, 0x0000511Eu, /*gameplay=*/0, /*d5=*/0, /*d6=*/0);
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                             "[pc] disk boot: skip_intro -> poster ($003330)\n");
        return 0;
    }
    /* The cold-start ($3000) drives the whole flow (intro → logos → title → menu
     * → gameplay) on the game thread; each frame wait (hw_vblank_wait) parks it
     * and pc_step_threaded releases it one frame at a time. */
    pc_cps_reset();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] disk boot: game-thread flow\n");
    return 0;
}

/* "Exit to main menu" from the pause menu — drop into the gp/title bank's
 * attract entry ($003330), which renders the poster ("cover art" screen).
 * No fire-skip hack: we land there directly because $003330 IS the poster
 * entry (the same address the engine reaches after the intro, and the same
 * one the menu's fade-back-to-attract path jumps to).
 *
 * Steps:
 *   1. Clear all g_state (bank flags, register file, shadow regs, audio).
 *   2. Reload the title overlay (native_overlay_load) — d0=1 equivalent.
 *      Required because if we're coming from gameplay/credits, the bytes
 *      at $3330+ have been overwritten with gameplay/credits code. Also
 *      replays the $6D714 block-copy so low-RAM engine state is fresh.
 *   3. Restart the continuation-stack flow at the title attract entry $003330
 *      (gp bank a5=$511E). The respawned game thread enters the poster. */
/* Test/debug drive into the END-GAME CREDITS, mirroring the win path's
 * `$150 d0=3` (native_overlay_loader_reloc): load the credits overlay and
 * enter $3330 with the credits bank active. Used by the harness REPL
 * (`gocredits`) so credits-side features (fire-skip) are testable without
 * winning W6L2. */
void pc_request_credits_start(void) {
    pc_state_reset_defaults();
    overlay_load_credits();
    g_pc_screen = PC_SCR_CREDITS;
    pc_cps_start_at(0x00003330u, 0x0000511Eu, /*gameplay=*/0, /*d5=*/0, /*d6=*/0);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[pc] credits drive: flow restart at $003330 (credits bank)\n");
}

void pc_request_cold_restart(void) {
    pc_state_reset_defaults(); /* zeros g_state, resets non-zero defaults */
    native_overlay_load();     /* reload title/intro overlay + block-copy */
    pc_cps_start_at(0x00003330u, 0x0000511Eu, /*gameplay=*/0, /*d5=*/0, /*d6=*/0);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[pc] exit-to-menu: flow restart at $003330 (poster)\n");
}

/* Direct-to-gameplay entry. Skips intro/title/menu entirely: the gameplay
 * overlay is loaded, $20.w is written, and the coroutine is set to enter
 * $577000 directly with the loader-handoff register state ($150 would set).
 *
 * This documents the contract the original retail-image gameplay engine expects at its
 * entry point — anything in this function is what we have to keep providing
 * as we native-port more of the engine. Currently the engine itself still
 * runs (we don't own $577000+ yet); this just removes the title machinery so
 * we can drive any level immediately and compare to PUAE cleanly. */
int pc_init_to_gameplay(const char **disks, int n_disks, int level) {

    if (pc_common_bringup(disks, n_disks) < 0)
        return -1;

    /* Gameplay-engine entry contract — observed from the natural title→$150
     * path and the original retail-image $577000 prologue (see gameplay_coro_entry):
     *   - g_mem must hold the loaded+decrunched gameplay overlay
     *     (base code at $3330, level engine at $577000, reloc table at $6E000).
     *   - $20.w  = level number (1..60) — read by $5779AA's level dispatcher.
     *   - $3e/$184 = $A68 (display pointers initialised by the real $150 body
     *     — without this, card glyph renderer overwrites $100 and hangs).
     *   - Disk-chunk dest-pointer table at $100/$104/$108 — built by
     *     native_overlay_load_d0().
     *   - All 60 level-name tables preloaded (pc_preload_all_level_names) so
     *     the title-card renderer can look them up without disk I/O reentry.
     *   - Coroutine register state at $577000: a5=$57EE12 (set by $577000
     *     itself), a6=$DFF000, a7=$80000 (reset on entry), d5=$1000, d6=$FFFF
     *     — see gameplay_coro_entry().
     */
    native_overlay_load_d0();

    /* Reproduce the retail $150 loader body's low-memory init: the $3e/$184
     * card-renderer sentinels and a valid $1e.w gameplay mode word (this dev
     * entry skips the menu, so $1e.w would otherwise be stale). */
    gameplay_handoff_prepare_low_memory();

    pc_preload_all_level_names();

    /* Pin the requested level. $20.w is the engine's level number; $5779AA
     * indexes (level-1)*4 into the 60-entry table at $57782E to pick a
     * (world, level_in_world) pair. */
    if (level < 1)
        level = 1;
    if (level > 60)
        level = 60;
    g_mem[0x20] = 0;
    g_mem[0x21] = (uint8_t)level;

    /* Enter the gameplay engine at $577000 directly. $577000 does its own
     * a5/a7/a6 init; the d5=$1000/d6=$FFFF loader-handoff values come from us. */
    g_gameplay_entry = 0x00577000u;
    pc_cps_start_at(0x00577000u, 0x0057EE12u, /*gameplay=*/1, /*d5=*/0x1000u, /*d6=*/0xFFFFu);

    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[pc] direct-to-gameplay: entering $577000 at level %d\n", level);
    return 0;
}

/* Per-frame driver. Release the game thread to run until its next vblank wait
 * (it draws this frame's content, then parks), then present the drawn frame and
 * deliver the vblank IRQ — same order as the old coroutine (present, IRQ, then
 * next frame the game resumes). Present + IRQ run on the MAIN thread while the
 * game thread is parked, so there are no races. Returns 1 to quit. */
int pc_step_threaded(void) {
    if (s_game_done && !g_enter_gameplay)
        return 1;
    const uint64_t frame_cycles_before = rt_get_guest_cycles();

    {
        uint64_t before = rt_get_guest_cycles();
        game_thread_run_one_frame(); /* run the game to its next vblank wait (parks) */
        pc_account_flow(rt_get_guest_cycles() - before);
    }

    if (g_harness_prerender_hook)
        g_harness_prerender_hook();
    pc_set_frame_cycles(rt_get_guest_cycles() - frame_cycles_before);
    /* Did this iteration actually SHOW a frame? hw_present_frame declines a beam
     * frame it has already shown, and says so only by leaving the frame counter
     * alone. The per-frame interrupts below are delivered once per DISPLAYED
     * frame, so an iteration that showed none must deliver none — otherwise the
     * music player gets a tick for a frame nobody saw. Measured: the intro's
     * volume ramp reached full a frame early (60 -> 64 in one frame against the
     * reference's 60 -> 63), and its tick counter ran one step ahead for the
     * whole run, from a single extra delivery on frame 34 (tools/lockstep.py). */
    const int frame_before_present = hw_get_frame_num();
    {
        const uint64_t before = rt_get_guest_cycles();
        const int stop = hw_present_frame();
        pc_set_present_cycles(rt_get_guest_cycles() - before);
        if (stop != 0)
            return 1;
    }
    /* Pay back the frames the boundary hold owes. A held boundary is a frame
     * that really elapsed while the guest was still mid-work; the display owes
     * it, and what it owes is the state the guest had when it finally waited —
     * which is what the oracle showed. Presenting them here, on the main thread
     * with the guest parked, keeps the frame COUNT right, so the game does not
     * run fast. Measured without it: the poster's fade was two steps ahead of
     * the oracle's by frame 7161 (engine/hw_beam.c, docs/issues/0008). */
    for (int owed = hw_boundary_take_owed(); owed > 0; owed--)
        if (hw_present_paused_frame() != 0)
            return 1;
    const int presented = hw_get_frame_num() != frame_before_present;
    if (g_harness_frame_hook)
        g_harness_frame_hook();

    /* The $150 loader override (from the title main loop OR its IRQ) may have set
     * g_enter_gameplay. Restart the game thread at the gameplay entry. Checked
     * before s_game_done because the loader call unwinds the title flow. */
    if (!g_enter_gameplay) {
        if (s_game_done)
            return 1;
        if (presented)
            coro_deliver_timer_irq(); /* level-3 vblank ISR (music via pc_music_tick) */
    }
    if (g_pc_enter_title) { /* title overlay loaded off-flow: host owns the restart */
        g_pc_enter_title = 0;
        pc_cps_start_at(0x00003330u, 0x0000511Eu, /*gameplay=*/0, /*d5=*/0, /*d6=*/0);
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                             "[game] restarting game thread at the poster $003330\n");
        return 0;
    }
    if (g_enter_gameplay) {
        g_enter_gameplay = 0;
        /* Full re-init for a level RESTART (pause Retry / native game-over). We're
         * on the main thread with the game parked, so it's safe to re-decrunch the
         * overlay + re-pin the card sentinels (same contract as pc_init_to_gameplay)
         * — this resets the chip RAM the previous screen corrupted. Preserve $20.w
         * (current level) across the reload. */
        if (g_pc_restart_reinit) {
            g_pc_restart_reinit = 0;
            uint8_t lv = g_mem[0x21];
            uint8_t extra = g_mem[0x38]; /* extra-levels (Disk.4) mode flag */
            native_overlay_load_d0();
            gameplay_handoff_prepare_low_memory();
            pc_preload_all_level_names();
            g_mem[0x20] = 0x00;
            g_mem[0x21] = lv;
            g_mem[0x38] = extra;
        }
        pc_cps_start_at(g_gameplay_entry, 0x0057EE12u, /*gameplay=*/1,
                        /*d5=*/0x00001000u, /*d6=*/0x0000FFFFu);
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                             "[game] restarting game thread into gameplay $%06X\n",
                             g_gameplay_entry);
        return 0;
    }
    return s_game_done ? 1 : 0;
}

void pc_fini(void) {
    game_thread_stop();
    g_chip = NULL;
    rt_fini();
    hw_fini();
}
