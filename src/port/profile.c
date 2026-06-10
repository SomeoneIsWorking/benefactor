/* pc_profile.c — persistent player progress (profile.json).
 *
 * Tracks which of the 60 levels have been COMPLETED (won). Stored separately
 * from benefactor.json (settings) in a flat JSON file next to the disks:
 *
 *   { "completed": [1, 2, 3, 11] }
 *
 * Path override: BENEFACTOR_PROFILE env var. Loaded lazily, saved on every
 * new completion (cheap, rare).
 *
 * Completion source: the LEVEL COMPLETE banner text routine ($5788DE) — the
 * one path that runs exactly on a WIN (lose/game-over use their own banners),
 * hooked in native_levelcomplete_text_capture (gameplay.c).
 *
 * Unlock rule (LEVEL SELECT): a level is selectable iff it has been completed,
 * or is the next one after the highest completion (level 1 always), or the
 * "unlock_all_levels" knob (OPTIONS → MORE) is on. Locked levels render as
 * ?????? and cannot be navigated to. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "port/config.h"
#include "port/port.h"   /* PC_NUM_LEVELS-style layout accessors */

#define PROFILE_MAX_LEVEL 60

static uint8_t s_completed[PROFILE_MAX_LEVEL + 1];   /* 1-based */
static int s_loaded = 0;

static const char *profile_path(void)
{
    const char *p = getenv("BENEFACTOR_PROFILE");
    return (p && *p) ? p : "profile.json";
}

static void profile_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    FILE *f = fopen(profile_path(), "rb");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *p = strstr(buf, "\"completed\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    while (*p && *p != ']') {
        while (*p && !isdigit((unsigned char)*p) && *p != ']') p++;
        if (!isdigit((unsigned char)*p)) break;
        int lv = (int)strtol(p, (char **)&p, 10);
        if (lv >= 1 && lv <= PROFILE_MAX_LEVEL) s_completed[lv] = 1;
    }
}

static void profile_save(void)
{
    FILE *f = fopen(profile_path(), "wb");
    if (!f) { fprintf(stderr, "[profile] cannot write %s\n", profile_path()); return; }
    fprintf(f, "{\n  \"completed\": [");
    int first = 1;
    for (int i = 1; i <= PROFILE_MAX_LEVEL; i++)
        if (s_completed[i]) { fprintf(f, "%s%d", first ? "" : ", ", i); first = 0; }
    fprintf(f, "]\n}\n");
    fclose(f);
}

int pc_profile_completed(int level)
{
    profile_load();
    return (level >= 1 && level <= PROFILE_MAX_LEVEL) ? s_completed[level] : 0;
}

int pc_profile_highest_completed(void)
{
    profile_load();
    for (int i = PROFILE_MAX_LEVEL; i >= 1; i--)
        if (s_completed[i]) return i;
    return 0;
}

void pc_profile_mark_completed(int level)
{
    profile_load();
    if (level < 1 || level > PROFILE_MAX_LEVEL || s_completed[level]) return;
    s_completed[level] = 1;
    profile_save();
    fprintf(stderr, "[profile] level %d completed (highest %d)\n",
            level, pc_profile_highest_completed());
}

/* Selectable in LEVEL SELECT? Completed, the next level up, or unlock-all. */
int pc_profile_unlocked(int level)
{
    if (level < 1 || level > PROFILE_MAX_LEVEL) return 0;
    if (pc_cfg_bool("unlock_all_levels", 0)) return 1;
    return pc_profile_completed(level) || level <= pc_profile_highest_completed() + 1;
}

/* LEVEL SELECT navigation helper: move the selection only if the target is
 * unlocked. (Dev paths — REPL goto / --level — call pc_set_start_level
 * directly and stay ungated.) */
int pc_profile_try_select(int level)
{
    if (!pc_profile_unlocked(level)) return 0;
    pc_set_start_level(level);
    return 1;
}
