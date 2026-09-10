#include "engine/hw_testrun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "engine/hw.h"
#include "port/port_internal.h"
#include "runtime/guest_runtime.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static void ensure_scratch_directory(void) {
#ifdef _WIN32
    (void)_mkdir("scratch");
#else
    (void)mkdir("scratch", 0755);
#endif
}

/* Save the composed output surface as a bitmap, if this is the asked-for frame. */
void hw_testrun_capture(int frame, const uint32_t *surface, int width, int height) {
    {
        static int dump_frame = -2;
        if (dump_frame == -2)
            dump_frame = pc_cfg_int("dump_frame", -1);
        if (dump_frame >= 0 && (uint32_t)frame == (uint32_t)dump_frame) {
            /* Dump the composed OUTPUT surface (s_out) — what is actually
             * presented, including the widescreen margins and overlays — not the
             * 352px content fb. hw_compose_output() already ran this frame.
             * Artifacts go under gitignored scratch/, never the repo root. */
            const char *path = "scratch/frame_dump.bmp";
            ensure_scratch_directory(); /* ok if it already exists */
            FILE *fp = fopen(path, "wb");
            if (fp) {
                int W = width, H = height;
                /* BMP row stride must be 4-byte aligned (already: W*4) */
                int row_bytes = W * 4;
                int data_size = row_bytes * H;
                int file_size = 54 + data_size;
                /* BMP file header (14 bytes) */
                uint8_t hdr[54] = {0};
                hdr[0] = 'B';
                hdr[1] = 'M';
                hdr[2] = file_size & 0xFF;
                hdr[3] = (file_size >> 8) & 0xFF;
                hdr[4] = (file_size >> 16) & 0xFF;
                hdr[5] = (file_size >> 24) & 0xFF;
                hdr[10] = 54; /* pixel data offset */
                /* BITMAPINFOHEADER (40 bytes) */
                hdr[14] = 40; /* header size */
                hdr[18] = W & 0xFF;
                hdr[19] = (W >> 8) & 0xFF;
                hdr[22] = H & 0xFF;
                hdr[23] = (H >> 8) & 0xFF;
                /* (negative height = top-down; use positive = bottom-up) */
                int negH = -H;
                hdr[22] = negH & 0xFF;
                hdr[23] = (negH >> 8) & 0xFF;
                hdr[24] = (negH >> 16) & 0xFF;
                hdr[25] = (negH >> 24) & 0xFF;
                hdr[26] = 1;  /* planes */
                hdr[28] = 32; /* bits per pixel */
                fwrite(hdr, 1, 54, fp);
                /* Pixel data: s_out is ARGB (0xAARRGGBB) → BMP needs BGRA */
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint32_t c = surface[y * W + x];
                        uint8_t px[4] = {c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0xFF};
                        fwrite(px, 1, 4, fp);
                    }
                }
                fclose(fp);
                benefactor_log_write(BENEFACTOR_LOG_DEBUG, "hardware", "frame %d saved to %s",
                                     dump_frame, path);
            }
        }
    }
}

/* A frame-indexed fire timeline: presses="7200:8,7450:8,7700:8", each entry a
 * frame to press fire on and how many frames to hold it. Reaching gameplay
 * takes three presses (credits -> menu -> level intro -> play), and comparing
 * gameplay against the reference only means something if both products press
 * on the SAME frame — which wall-clock input cannot promise. */
#define TESTRUN_PRESSES_MAX 16

static void hw_testrun_fire_timeline(int frame) {
    static struct {
        int at;
        int frames;
    } presses[TESTRUN_PRESSES_MAX];
    static int count = -1;

    if (count < 0) {
        count = 0;
        char spec[256];
        if (pc_cfg_string("presses", "", spec, sizeof spec) && spec[0]) {
            for (char *entry = strtok(spec, ","); entry && count < TESTRUN_PRESSES_MAX;
                 entry = strtok(NULL, ",")) {
                int at = 0;
                int frames = 4;
                if (sscanf(entry, "%d:%d", &at, &frames) >= 1 && at >= 0) {
                    presses[count].at = at;
                    presses[count].frames = frames > 0 ? frames : 1;
                    count++;
                }
            }
            benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "fire timeline: %d press(es)", count);
        }
    }

    for (int i = 0; i < count; i++) {
        if (frame == presses[i].at) {
            hw_set_fire(1);
            hw_set_mouse_lmb(1);
            benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "fire pressed at frame %d", frame);
        } else if (frame == presses[i].at + presses[i].frames) {
            hw_set_fire(0);
            hw_set_mouse_lmb(0);
            benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "fire released at frame %d", frame);
        }
    }
}

/* Timed input, the end-of-run memory dump and the frame limit. */
void hw_testrun_script(int frame) {
    hw_testrun_fire_timeline(frame);
    {
        static int test_frames = -1;
        static int press_frame = -1;
        static int release_frame = -1;
        static int dump_parsed = 0;
        static int test_done = 0;
        static int frame_limit = 0;

        if (!dump_parsed) {
            dump_parsed = 1;
            test_frames = pc_cfg_int("test", test_frames);
            press_frame = pc_cfg_int("press", press_frame);
            release_frame = pc_cfg_int("release", release_frame);
            frame_limit = pc_cfg_int("limit", frame_limit);
            if (frame_limit > 0)
                hw_set_frame_limit(frame_limit);
        }

        if (press_frame >= 0 && (uint32_t)frame == (uint32_t)press_frame) {
            hw_set_fire(1);
            hw_set_mouse_lmb(1);
            benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "fire pressed at frame %d", frame);
        }
        if (release_frame >= 0 && (uint32_t)frame == (uint32_t)release_frame) {
            hw_set_fire(0);
            hw_set_mouse_lmb(0);
            benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "fire released at frame %d", frame);
        }

        if (test_frames > 0 && (uint32_t)frame >= (uint32_t)test_frames && !test_done) {
            test_done = 1;
            /* Dump configured memory regions after the scripted test run. */
            char dump_spec[1024];
            if (pc_cfg_string("dump", "", dump_spec, sizeof dump_spec) && dump_spec[0]) {
                char buf[1024];
                strncpy(buf, dump_spec, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *tok = strtok(buf, ",");
                while (tok) {
                    unsigned long addr = 0;
                    int len = 16;
                    if (sscanf(tok, "%lx:%d", &addr, &len) >= 1) {
                        addr &= 0xFFFFFF;
                        if (len > 256)
                            len = 256;
                        benefactor_log_write(BENEFACTOR_LOG_DEBUG, "memory", "$%06lX %d bytes",
                                             addr, len);
                        for (int i = 0; i < len; i += 16) {
                            char line[96];
                            int used = snprintf(line, sizeof line, "%06lX:", addr + i);
                            for (int j = 0; j < 16 && i + j < len; j++) {
                                if (addr + i + j < RT_MEM_SIZE)
                                    used += snprintf(line + used, sizeof line - (size_t)used,
                                                     " %02X", g_mem[addr + i + j]);
                                else
                                    used +=
                                        snprintf(line + used, sizeof line - (size_t)used, " --");
                            }
                            benefactor_log_write(BENEFACTOR_LOG_DEBUG, "memory", "%s", line);
                        }
                    }
                    tok = strtok(NULL, ",");
                }
            }
            hw_running = 0;
        }
    }
}
