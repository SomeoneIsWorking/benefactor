#include "port/state_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/game_state.h"
#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "runtime/guest_runtime.h"

#define STATE_DUMP_MAX_OFFSETS 8

static struct {
    int loaded;      /* config parsed */
    int enabled;     /* there is something to do */
    uint32_t screen; /* the cop1lc that anchors the moment */
    int anchor;      /* the frame that screen first appeared on, or -1 */
    int offsets[STATE_DUMP_MAX_OFFSETS];
    int count;
    char directory[512];
} s_dump;

/* Parse `state_dump=<cop1lc>:<offset>,<offset>,...` once. */
static void state_dump_load(void) {
    char spec[256];
    s_dump.loaded = 1;
    s_dump.anchor = -1;
    if (!pc_cfg_string("state_dump", "", spec, sizeof spec) || spec[0] == 0)
        return;
    char *colon = strchr(spec, ':');
    if (colon == NULL) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "state",
                             "state_dump=%s has no ':' — want <cop1lc>:<offset>,...", spec);
        return;
    }
    *colon = 0;
    s_dump.screen = (uint32_t)strtoul(spec, NULL, 16) & 0xFFFFFFu;
    for (char *entry = strtok(colon + 1, ","); entry && s_dump.count < STATE_DUMP_MAX_OFFSETS;
         entry = strtok(NULL, ",")) {
        s_dump.offsets[s_dump.count++] = atoi(entry);
    }
    if (!pc_cfg_string("state_dump_dir", "", s_dump.directory, sizeof s_dump.directory) ||
        s_dump.directory[0] == 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "state",
                             "state_dump needs state_dump_dir=<directory>");
        return;
    }
    s_dump.enabled = s_dump.count > 0;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "state",
                         "will snapshot guest memory at screen $%06X + %d offsets into %s",
                         s_dump.screen, s_dump.count, s_dump.directory);
}

static void state_dump_write(int offset) {
    char path[600];
    snprintf(path, sizeof path, "%s/%+d.bin", s_dump.directory, offset);
    FILE *sink = fopen(path, "wb");
    if (sink == NULL) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "state", "cannot write %s", path);
        return;
    }
    const size_t written = fwrite(g_mem, 1u, (size_t)RT_MEM_SIZE, sink);
    fclose(sink);
    /* Say the frame as well as the offset: the two products disagree about
     * frame numbers, and the whole point is that these two dumps are the same
     * MOMENT despite that. */
    benefactor_log_write(BENEFACTOR_LOG_INFO, "state", "dump: offset=%+d frame=%d bytes=%zu -> %s",
                         offset, hw_get_frame_num(), written, path);
}

void pc_note_state_dump(void) {
    if (!s_dump.loaded)
        state_dump_load();
    if (!s_dump.enabled || g_mem == NULL)
        return;
    const int frame = hw_get_frame_num();
    if (s_dump.anchor < 0) {
        if ((hw_get_cop1lc() & 0xFFFFFFu) != s_dump.screen)
            return;
        s_dump.anchor = frame;
        benefactor_log_write(BENEFACTOR_LOG_INFO, "state", "screen $%06X reached at frame %d",
                             s_dump.screen, frame);
    }
    for (int i = 0; i < s_dump.count; i++) {
        if (frame == s_dump.anchor + s_dump.offsets[i])
            state_dump_write(s_dump.offsets[i]);
    }
}
