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
int pc_run(void);
int pc_step(void);  /* single frame, returns 0=continue, 1=quit */
void pc_fini(void);
/* When set, pc_run skips host-rate pacing (harness drives stepping itself). */
void pc_set_harness_mode(int on);

/* Debug helpers (gameplay only). complete_level = the teleport WIN (sets the
 * $586928 win flag the object loop tests); game_over = death (bit15 of $10AC). */
void pc_debug_complete_level(void);
void pc_debug_game_over(void);
