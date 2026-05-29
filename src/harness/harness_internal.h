/* harness_internal.h – Shared state and functions for harness modules */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include "common_log.h"
#include "harness/puae_state.h"
#include "recomp/hw.h"   /* HW_DISPLAY_W/H */

/* State logging */
#define MAX_FRAMES 1000
#define MAX_FB_FRAMES 10
/* Framebuffer dims track the native display so the harness reads the PC
 * framebuffer (hw_get_framebuffer) at the correct stride. */
#define FB_W HW_DISPLAY_W
#define FB_H HW_DISPLAY_H
#define CHIP_RAM_SIZE (512 * 1024)

extern FrameState s_puae_log[MAX_FRAMES];
extern FrameState s_pc_log[MAX_FRAMES];

extern uint32_t s_puae_fb_log[MAX_FB_FRAMES][FB_W * FB_H];
extern uint32_t s_pc_fb_log[MAX_FB_FRAMES][FB_W * FB_H];
extern int s_puae_fb_count;
extern int s_pc_fb_count;

extern uint8_t s_puae_chipram_snap[CHIP_RAM_SIZE];
extern uint8_t s_pc_chipram_snap[CHIP_RAM_SIZE];
extern int s_puae_chipram_valid;
extern int s_pc_chipram_valid;

extern uint32_t s_puae_fb[FB_W * FB_H];

/* Pre-render BPL CRC: chip RAM BPL regions captured on the PC side
 * immediately before hw_present_frame() runs.  This is the BPL data the
 * renderer actually sees — compare with PUAE's bpl_data_crc (post-frame)
 * to detect mid-frame blitter modifications that pc_step skips. */
extern uint32_t s_pc_prerender_bpl_crc;
extern int      s_pc_prerender_bpl_crc_valid;

/* Functions from harness_compare.c */
int frames_differ(const FrameState *p, const FrameState *c);
int framebuffers_differ(int fi);
void compare_phases(int puae_frames, int pc_frames, const char *report_path);
void compare_framebuffers(int fi);
void compare_bitplanes(const FrameState *p);
/* Invoke Python root-cause analyzers inline and print results to stdout. */
void harness_on_diverge(int frame, const char *reason);
