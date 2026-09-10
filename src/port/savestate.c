/* savestate.c — save and restore a gameplay session to a file.
 *
 * Split out of game_loop.c, which owns the game thread and the frame step; this
 * file owns only the on-disk format and the two moments it is legal to use it.
 * The parked guest context is g_state's own (common/game_state.h); the one
 * thing it reaches back into the loop for is the re-entry that restarts the
 * gameplay cycle, pc_resume_gameplay_thread, declared in port/port_internal.h.
 */
#include "port/port_internal.h"

/* Savestate: snapshot the image-qualified interpreter state and guest memory
 * between frame steps, while the execution owner is parked at $577114. */

#define PC_SAVESTATE_MAGIC 0x42454E53u /* 'BENS' */
#define PC_SAVESTATE_VER 8u            /* v8: opaque amigaport CPU snapshot */

/* Whether a savestate can be taken right now. Only the steady-gameplay frame loop
 * is resumable: the game thread must be parked at the resumable main-loop wait
 * ($577114), as reported by the shared execution owner. Everywhere else — menus, intro, the level
 * card, and mid-transition loads/death animations — has no resumable suspension point, so a save
 * there could not be cleanly restored. Returns 1 if allowed; otherwise 0 and *reason gets a short
 * human-readable explanation (for the on-screen toast). */
int pc_savestate_allowed(const char **reason) {
    if (!g_gameplay_active) {
        if (reason)
            *reason = "Can only save during gameplay";
        return 0;
    }
    if (!rt_is_resume_point(&s_game_ctx, s_game_ctx.image, 0x00577114u)) {
        if (reason)
            *reason = "Can't save here (level transition)";
        return 0;
    }
    return 1;
}

/* Savestate format (v8): one g_state blob, the opaque amigaport CPU snapshot,
 * and g_mem. NO native/host state is saved
 * (no stack, no RIP/RSP) — the only suspended state is the M68K context (incl.
 * CPU view) and chip RAM, both inside g_state/g_mem. Load re-enters the
 * gameplay cycle at $577114 (see pc_resume_gameplay_thread), so a save round-trips
 * across process restarts.
 *   uint32_t magic, ver, sizeof(g_state), RT_MEM_SIZE, runtime_state_size
 *   GameState g_state
 *   uint8_t   amigaport::CpuState runtime snapshot
 *   uint8_t   g_mem[RT_MEM_SIZE] */

int pc_savestate(const char *path) {
    if (!g_mem || !path)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] savestate: open %s failed\n", path);
        return -1;
    }
    size_t runtime_size = rt_state_blob_size();
    uint8_t *runtime_blob = malloc(runtime_size);
    if (!runtime_blob || rt_state_blob_save(runtime_blob, runtime_size) != 0) {
        free(runtime_blob);
        fclose(f);
        return -1;
    }
    uint32_t hdr[5] = {PC_SAVESTATE_MAGIC, PC_SAVESTATE_VER, (uint32_t)sizeof g_state,
                       (uint32_t)RT_MEM_SIZE, (uint32_t)runtime_size};
    int ok = 1;
    ok &= fwrite(hdr, sizeof hdr, 1, f) == 1;
    ok &= fwrite(&g_state, sizeof g_state, 1, f) == 1;
    ok &= fwrite(runtime_blob, runtime_size, 1, f) == 1;
    ok &= fwrite(g_mem, RT_MEM_SIZE, 1, f) == 1;
    free(runtime_blob);
    fclose(f);
    if (!ok) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] savestate: short write\n");
        return -1;
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[pc] savestate -> %s (gameplay_active=%d overlay=%d)\n", path,
                         g_gameplay_active, g_overlay_active);
    return 0;
}

int pc_loadstate(const char *path) {
    if (!g_mem || !path)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] loadstate: open %s failed\n", path);
        return -1;
    }
    uint32_t hdr[5];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != PC_SAVESTATE_MAGIC ||
        hdr[1] != PC_SAVESTATE_VER || hdr[2] != (uint32_t)sizeof g_state ||
        hdr[3] != (uint32_t)RT_MEM_SIZE || hdr[4] != (uint32_t)rt_state_blob_size()) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                             "[pc] loadstate: bad/incompatible header in %s\n", path);
        fclose(f);
        return -1;
    }
    int ok = 1;
    ok &= fread(&g_state, sizeof g_state, 1, f) == 1;
    uint8_t *runtime_blob = malloc(hdr[4]);
    if (!runtime_blob)
        ok = 0;
    if (ok)
        ok &= fread(runtime_blob, hdr[4], 1, f) == 1;
    if (ok)
        ok &= rt_state_blob_load(runtime_blob, hdr[4]) == 0;
    free(runtime_blob);
    ok &= fread(g_mem, RT_MEM_SIZE, 1, f) == 1;
    fclose(f);
    if (!ok) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[pc] loadstate: short read\n");
        return -1;
    }
    rt_activate_image(&s_game_ctx, g_gameplay_active ? BENEFACTOR_IMAGE_GAMEPLAY
                                                     : (g_credits_active ? BENEFACTOR_IMAGE_CREDITS
                                                                         : BENEFACTOR_IMAGE_TITLE));
    /* Native-side render caches are NOT part of the savestate: drop the wsobj
     * committed-page map so persisted objects re-seed from the RESTORED engine
     * state instead of shadowing it with pre-load entries. */
    {
        native_wsobj_commit_reset();
    }
    /* g_state (including the CPU view) + g_mem are now loaded. The old game
     * thread (parked at its own wait on the PRE-load memory) is stale: discard it
     * and spawn a fresh thread that re-enters the steady-gameplay cycle at $577114.
     * The shared execution owner resumes from $577114, so the cycle continues
     * from the restored guest memory and CPU state (no native stack restored).
     * Saves are only ever taken in that state (pc_savestate_allowed), so every valid
     * savestate loads this way. */
    int at_resume = rt_is_resume_point(&s_game_ctx, s_game_ctx.image, 0x00577114u);
    if (g_gameplay_active && at_resume) {
        pc_resume_gameplay_thread();
    } else {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                             "[pc] loadstate: WARNING — not a steady-gameplay state "
                             "(gameplay=%d at_resume=%d); cannot cleanly resume\n",
                             g_gameplay_active, at_resume);
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[pc] loadstate <- %s (gameplay_active=%d overlay=%d at_resume=%d)\n",
                         path, g_gameplay_active, g_overlay_active, at_resume);
    return 0;
}
