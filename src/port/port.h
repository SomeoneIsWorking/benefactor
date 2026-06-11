/* pc.h – Native PC game engine public header */
#pragma once
#include <stdint.h>

/* Chip RAM pointer (set by pc_init) */
extern uint8_t *g_chip;

/* Read/write helpers for chip RAM */
static inline uint8_t  r8 (uint32_t a) { return g_chip[a]; }
static inline uint16_t r16(uint32_t a) { return (g_chip[a]<<8)|g_chip[a+1]; }
static inline uint32_t r32(uint32_t a) { return ((uint32_t)g_chip[a]<<24)|((uint32_t)g_chip[a+1]<<16)|((uint32_t)g_chip[a+2]<<8)|g_chip[a+3]; }
static inline void     w8 (uint32_t a, uint8_t  v) { g_chip[a]=v; }
static inline void     w16(uint32_t a, uint16_t v) { g_chip[a]=v>>8; g_chip[a+1]=v; }
static inline void     w32(uint32_t a, uint32_t v) { g_chip[a]=v>>24; g_chip[a+1]=v>>16; g_chip[a+2]=v>>8; g_chip[a+3]=v; }

/* Screen dimensions */
#define SCR_W 320
#define SCR_H 256

/* Single PC execution path: native boot from the original disk images. */
int pc_init_from_disk(const char **disks, int n_disks);

/* Direct-to-gameplay entry: bypass intro/title/menu, load the gameplay
 * overlay, and enter $577000 at the given level (1..60). Use this for
 * iteration / level-specific debugging — the title flow is irrelevant to
 * any gameplay work, and routing every test through 5000+ frames of intro
 * is the wrong abstraction for that work. Returns 0 on success. */
int pc_init_to_gameplay(const char **disks, int n_disks, int level);

int pc_run(void);
int pc_step(void);  /* single frame, returns 0=continue, 1=quit */
void pc_fini(void);
/* Save/restore the game coroutine + 8MB M68K memory to disk. Call between
 * pc_step()s — the coro is paused at coro_yield then, the cleanest boundary.
 * Returns 0 on success, -1 on failure. */
int pc_savestate(const char *path);
int pc_loadstate(const char *path);

/* Opt-in HTTP debug server (no-op unless BENEFACTOR_HTTP=<port> is set). */
void pc_http_debug_start(void);

/* When set, pc_run skips host-rate pacing (harness drives stepping itself). */
void pc_set_harness_mode(int on);

/* Debug helpers (gameplay only). complete_level = the teleport WIN (sets the
 * $586928 win flag the object loop tests); game_over = death (bit15 of $10AC). */
void pc_debug_complete_level(void);
void pc_debug_game_over(void);

/* ── Level / world layout & names — SINGLE SOURCE OF TRUTH ───────────────────
 * 60 levels across 7 worlds; worlds 0-1 have 9 levels, 2-5 have 10, 6 has 2
 * (mirrors the engine's level table at $57782E). Anything needing world/level
 * geometry or names MUST go through these — do NOT re-hardcode the
 * {9,9,10,10,10,10,2} split or re-extract names anywhere else. Names are
 * decoded once from the disk overlays (pc_preload_all_level_names), applying
 * the per-level name-slot permutation read from the engine's $32 table. */
#define PC_NUM_WORLDS 7          /* vanilla worlds (levels 1..60) */
#define PC_NUM_LEVELS 60
/* EXTRA LEVELS (Disk.4): the base game's world-descriptor table at $577452
 * defines 6 additional world slots (7..12, five levels each = 61..90) whose
 * names+level-data chunk lives on Disk.4 at fixed offsets. A data disk fills
 * some prefix of those slots (the fan BenDisk4 fills 2 = levels 61..70). */
#define PC_EXTRA_WORLDS 6
#define PC_MAX_LEVELS   90
int  pc_extra_worlds_available(void);  /* 0..6 contiguous filled extra worlds */
int  pc_num_worlds_ui(void);           /* 7 + extras — picker/UI world count  */
int  pc_num_levels_ui(void);           /* 60 + 5*extras                       */
int  pc_menu_continue_level(void);     /* first uncleared vanilla level (CONTINUE) */

int  pc_levels_in_world(int world);    /* # levels in world (0 if out of range) */
int  pc_world_first_level(int world);  /* 1-based global level # of world's first level (0 if OOR) */
void pc_level_split(int level, int *world_out, int *level_in_world_out);

void        pc_preload_all_level_names(void);
const char *pc_world_name(int world);
const char *pc_static_level_name(int level);  /* name of global level 1..60 */
const char *pc_current_level_name(void);      /* name of the level in $20.w */

/* Player progress (profile.json — src/port/profile.c). */
int  pc_profile_completed(int level);          /* level 1..60 won at least once  */
int  pc_profile_highest_completed(void);       /* 0 if none                      */
void pc_profile_mark_completed(int level);     /* idempotent; saves profile.json */
int  pc_profile_unlocked(int level);           /* selectable in LEVEL SELECT     */
int  pc_profile_try_select(int level);         /* set start level iff unlocked   */

/* Pending level-select choice, applied at the $150 hand-off. */
void pc_set_start_level(int n);
int  pc_get_start_level(void);
