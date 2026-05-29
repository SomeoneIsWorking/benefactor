"""
Benefactor recompiler entry point definitions.

Each entry is (address_hex, descriptive_name, output_group).
  - name:  used for the generated C function (gfn_<name>); None → gfn_XXXXXX
  - group: determines which generated .c file receives this function
"""
ENTRIES = [
    # (addr_hex, name,                   group)
    ("3000",  "program_start",           "boot"),
    # State-machine dispatch loop ($3092) + re-entry ($30C2). Screen states end
    # with `jmp $30C2` (pop a4-a6; bra $3092) to advance to the next state-table
    # entry. These are jump targets, not call targets, so static descent from
    # $3000 only emits $3092 as a label inside program_start — the `jmp $30C2`
    # then lands on no function and unwinds. Emitting them as entry points lets
    # the recompiled flow follow the dispatch chain into the next screen.
    ("30C2",  "dispatch_reentry",        "boot"),
    ("3092",  "dispatch_loop",           "boot"),
    ("311A",  None,                      "boot"),
    ("31A0",  "blitter_wait_clear",      "blitter"),
    ("31C2",  "boot_phase3_copper",      "boot"),
    ("3218",  None,                      "boot"),
    ("346E",  None,                      "boot"),
    ("366A",  "boot_title_frame",        "boot"),
    ("3488",  "game_frame",              "loop"),
    ("377A",  "sprite_playfield_setup",  "sprite"),
    ("3818",  "sprite_table_init",       "sprite"),
    ("405C",  "text_sprite_render",      "render"),
    ("40B6",  "dispatch_table",          "render"),
    ("40B8",  "item_dispatch_1",         "render"),
    ("40BA",  "item_dispatch_2",         "render"),
    ("40BC",  "item_dispatch_3",         "render"),
    ("40BE",  "item_decrement",          "render"),
    ("40CC",  "item_scroll",             "render"),
    ("4102",  "item_position",           "render"),
    ("412E",  "item_blitter",            "render"),
    ("41A4",  "sprite_blitter_setup",    "sprite"),
    ("4236",  "blit_row_callback",       "render"),
    ("4895",  None,                      "render"),
    ("52A4",  "post_blit_handler",       "render"),
    ("52F0",  None,                      "render"),
    ("531C",  None,                      "render"),
    ("5560",  None,                      "timer"),
    ("55A0",  "timer_interrupt",         "timer"),
    # Vertical-blank interrupt handler: copies the audio shadow struct ($69F6+)
    # to the Paula hardware registers (AUDxLC/LEN) every frame. Reached only via
    # the level-3 interrupt vector, so static descent never found it; without it
    # the PC port leaves AUDxLC/LEN at the one-shot note-on values instead of the
    # sustain-loop point the music driver streams into the shadow.
    ("58C2",  "audio_vbl_copy",          "timer"),
    # Intro interrupt-vector handlers ($6c=$314E level-3, $78=$3160 level-6).
    # Reached only via the autovectors / an indirect jsr, so static descent
    # never found them. $3160 = movem / bsr $55A0 (music) / ack INTREQ / rte.
    # Registering them clears the last two rt_call misses in the intro/title.
    ("3160",  "lvl6_wrapper_intro",      "timer"),
    ("314E",  "lvl3_wrapper_intro",      "timer"),
    # Title-music sub reached via indirect jsr from the music player; static
    # descent never found it → runtime "no function at $0033E2 – skipping",
    # which dropped part of the title tune. Registering it as an entry restores it.
    ("33E2",  "music_sub_33E2",          "timer"),
    ("593A",  "timer_dispatch_sub0",     "timer"),
    ("5A32",  "timer_dispatch_sub1",     "timer"),
    ("5B62",  "timer_dispatch_sub2",     "timer"),
    ("60CC",  None,                      "timer"),
    # Title object-animation handlers — reached only via indirect dispatch
    # (rt_call from a state machine + the $5F80 jmp-table), so static descent
    # missed them. Without these the $69F0 object table is never updated.
    ("5E48",  "obj_clamp_handler",       "animation"),
    ("5E5A",  "obj_min1_handler",        "animation"),
    ("5E6A",  "obj_advance_handler",     "animation"),
    ("5E82",  "obj_dec_handler",         "animation"),
    ("5EB0",  "title_anim_driver",       "animation"),
    ("5F7E",  "anim_noop",               "animation"),
    ("5F80",  "anim_state_dispatch",     "animation"),
    ("6032",  "anim_state_a",            "animation"),
    ("607C",  "anim_state_b",            "animation"),
    ("60B0",  "anim_state_c",            "animation"),
    ("74AA",  "boot_animation_init",     "animation"),
    ("74DC",  "boot_animation_step",     "animation"),
]

# Tool-discovered indirect-dispatch targets (maintained by
# tools/recomp/discover_indirect.py). These are addresses the running game
# calls via register-indirect jsr / jmp-table dispatch that static descent
# cannot follow. A runtime "no function at $X" is proof X is a real call
# target, so they are appended here automatically.
try:
    from discovered import DISCOVERED  # type: ignore
except Exception:
    try:
        from .discovered import DISCOVERED  # type: ignore
    except Exception:
        DISCOVERED = []

ENTRIES = ENTRIES + [(a, None, "discovered") for a in DISCOVERED]

# Ordered list of output groups (determines file order / include order).
# Each group → generated file  game_<group>.c
GROUPS = ["boot", "blitter", "loop", "sprite", "render", "timer", "animation", "discovered"]


def get_entry_addrs():
    return [int(e[0], 16) for e in ENTRIES]


def get_fn_name(addr):
    for e in ENTRIES:
        if int(e[0], 16) == addr:
            if e[1]:
                return e[1]
            return f"{addr:06X}"
    return f"{addr:06X}"


def get_fn_group(addr):
    for e in ENTRIES:
        if int(e[0], 16) == addr:
            return e[2] if len(e) > 2 else "misc"
    return "misc"
