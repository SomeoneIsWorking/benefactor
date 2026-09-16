"""Enter a level, leave to the main menu, do it again — over and over.

The reported crash lived here: enter a level through LEVEL SELECT, EXIT TO MAIN
MENU, enter one again, and the process died with no message. It took a driver
that could walk the real menus to reproduce it at all, and once it reproduced,
the fault was a native override frame outliving the game thread whose stack it
pointed at (`src/runtime/guest_runtime.cpp`, `Runtime::frames`).

So the driver stays. It is the only thing that exercises the teardown path the
crash lived on — the game thread is stopped mid-override every time EXIT TO MAIN
MENU is taken, and the picker's `for (;;) hw_vblank_wait()` is the deepest place
that happens from. A unit test cannot reach it: the bug needs a real thread, a
real override, and a real teardown.

Every fifth round it also runs the player out of lives and checks that the level
card comes back rather than the CONTINUE/GAME OVER screen, because the override
that does that is a registration one line long and has been silently lost once
already (see `die_and_expect_the_level_card`).

Run it against a build:

    uv run --frozen python -m tools.menu_soak --rounds 20

It exits non-zero if the game dies, if a step does not happen, or if a round
cannot be driven. On a crash it prints the report the game wrote — the crash log
is installed by `src/port/crash_report.c`, and this points it at a file of its
own so a soak never appends to the player's.

A pass here is evidence, not proof: the fault needs the teardown to land in a
particular place, so it appears in some rounds and not others. Measured against
a build with the fix removed, twelve rounds passed on three different seeds and
forty rounds died in the tenth. So the default is forty, and a shorter run that
comes back clean has shown less than it looks like it has.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from tools.control_port import choose_port
from tools.paths import DISK_NAMES, ROOT

PAL_FRAME_SECONDS = 1.0 / 50.0

MENU_CURSOR_ADDRESS = 0x511E - 6334
"""The title menu's option cursor, at a5-6334 in the gp bank.

Poking it and firing reaches `native_main_menu_fire_dispatch` exactly as the
player's own selection does. The menu's arrow navigation is the engine's, driven
through the emulated joystick's quadrature encoding, and a scripted press does
not move it — so the selection is made here instead of pretended at.
"""

LEVEL_SELECT = 1
"""The cursor value for LEVEL SELECT (0 is CONTINUE, 2 is OPTIONS)."""

CARD_SCREEN = "003914"
"""The copper list the card/menu bank renders from.

The level card and the CONTINUE/GAME OVER screen share it, so seeing it says
which bank is up, never which of the two. `GAMEOVER_FLAGS` tells them apart.
"""

CAVERN_SCREEN = "003484"
"""The copper list the playfield renders from."""

GAMEOVER_FLAGS = 0x57FEA5
"""$1093 absolute: bit6 marks the game-over screen, bit5 its menu phase."""

GUEST_LEVEL = 0x20
"""$20.w: the level the gameplay dispatcher at $5779AA reads."""


class GameDied(Exception):
    """The process exited while the soak was waiting for something."""


class StepMissed(Exception):
    """A step the soak asked for did not happen inside its budget."""


class Game:
    """A running game, and the control channel to drive it through."""

    def __init__(self, port: int, process: subprocess.Popen[bytes]) -> None:
        self.port = port
        self.process = process

    def get(self, path: str) -> str:
        url = f"http://127.0.0.1:{self.port}{path}"
        with urllib.request.urlopen(url, timeout=5) as response:  # noqa: S310
            return response.read().decode()

    def state(self) -> dict:
        return json.loads(self.get("/state"))

    def press(self, **buttons: int) -> None:
        self.get("/press?" + "&".join(f"{name}={value}" for name, value in buttons.items()))

    def key(self, name: str) -> None:
        self.get(f"/key?name={name}")

    def poke(self, address: int, value: int) -> None:
        self.get(f"/poke?addr={address:X}&val={value:02X}")

    def byte(self, address: int) -> int:
        return int(json.loads(self.get(f"/mem?addr={address:X}&len=1"))["hex"], 16)

    def word(self, address: int) -> int:
        return int(json.loads(self.get(f"/mem?addr={address:X}&len=2"))["hex"], 16)

    def menu(self, nav: str | None = None) -> dict:
        path = f"/menu?nav={nav}" if nav else "/menu?show=1"
        return json.loads(self.get(path))["view"]

    def open_menu(self) -> dict:
        return json.loads(self.get("/menu"))["view"]

    def until(self, what: str, ready, frames: int = 1500) -> None:
        """Wait for `ready(state)`, or say which step did not happen."""
        deadline = time.monotonic() + frames * PAL_FRAME_SECONDS
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise GameDied(f"the game died (exit {self.process.returncode}) waiting for {what}")
            if ready(self.state()):
                return
            time.sleep(0.04)
        raise StepMissed(f"{what} did not happen within {frames} frames")

    def settle(self, frames: int) -> None:
        time.sleep(frames * PAL_FRAME_SECONDS)


def start(port: int, log: Path, output: Path, *, windowed: bool) -> Game:
    disks = list(DISK_NAMES)
    missing = [name for name in disks if not (ROOT / name).exists()]
    if missing:
        raise SystemExit(f"need the disk images in {ROOT}: {', '.join(missing)}")
    executable = ROOT / "build/run/Benefactor.app/Contents/MacOS/Benefactor"
    if not executable.exists():
        executable = ROOT / "build/run/benefactor-pc"
    if not executable.exists():
        raise SystemExit(f"no built game at {executable}; build benefactor_product first")

    environment = dict(os.environ, BENEFACTOR_HTTP=str(port), BENEFACTOR_CRASH_LOG=str(log))
    command = [str(executable)]
    if not windowed:
        command.append("--headless")
    command += ["--disk", *disks]
    handle = output.open("w")
    process = subprocess.Popen(
        command, cwd=ROOT, env=environment, stdout=handle, stderr=subprocess.STDOUT
    )
    game = Game(port, process)
    for _ in range(150):
        time.sleep(0.2)
        if process.poll() is not None:
            raise SystemExit(f"the game died during startup (exit {process.returncode})")
        try:
            game.state()
            return game
        except (urllib.error.URLError, OSError, json.JSONDecodeError):
            continue
    raise SystemExit("the control channel never answered")


def reach_main_menu(game: Game) -> None:
    if game.state()["ui"]["main_menu"]:
        return
    keep_pressing(
        game,
        "the main menu to appear",
        lambda s: s["ui"]["main_menu"],
        attempts=12,
        fire=1,
        frames=4,
    )


def keep_pressing(
    game: Game, what: str, ready, attempts: int = 8, wait: int = 150, **buttons: int
) -> None:
    """Press until the thing happens, rather than once and hope.

    A single press is a guess about where in the frame the game is: the picker
    swallows the fire that opened it until the button is released, and a menu
    that has just been entered ignores input for a few frames. Pressing again
    is what a player does, and it keeps the soak measuring the crash instead of
    the timing of its own HTTP requests.
    """
    for attempt in range(attempts):
        if game.process.poll() is not None:
            raise GameDied(f"the game died (exit {game.process.returncode}) waiting for {what}")
        if attempt:
            game.settle(20)
        game.press(**buttons)
        try:
            game.until(what, ready, frames=wait)
            return
        except StepMissed:
            continue
    raise StepMissed(f"{what} did not happen after {attempts} presses")


def open_picker(game: Game) -> None:
    game.poke(MENU_CURSOR_ADDRESS, 0)
    game.poke(MENU_CURSOR_ADDRESS + 1, LEVEL_SELECT)
    keep_pressing(
        game, "the level picker to open", lambda s: s["ui"]["level_select"], fire=1, frames=4
    )
    # The cursor has to open on the level the GAME is on. It used not to: the
    # cursor wrote its choice into a host variable that EXIT TO MAIN MENU's
    # state reset then zeroed, so the picker reopened on level 1 while $20.w
    # still held the level just played — and confirming without moving started
    # that one, not the one shown. Both sides are read here because agreeing on
    # a wrong number is the only other way this can go.
    guest = game.word(GUEST_LEVEL)
    shown = game.state()["ui"]["start_level"]
    if guest and shown != guest:
        raise StepMissed(f"the picker opened on level {shown}, but the game is on {guest}")


def choose_menu_row(game: Game, label: str) -> None:
    """Move the pause menu's cursor onto a row by name, and take it.

    By name rather than by a count of keypresses: the rows differ by context
    (the greyed-out effects, the per-mode OPTIONS list), so a count silently
    selects the wrong thing when the layout moves.
    """
    view = game.open_menu() if not game.state()["ui"]["pause_menu"] else game.menu()
    labels = [row["label"] for row in view["rows"]]
    if label not in labels:
        raise StepMissed(f"no {label} row in {labels}")
    for _ in range(labels.index(label) - view["cursor"]):
        game.menu("down")
        game.settle(4)
    view = game.menu()
    landed = view["rows"][view["cursor"]]["label"]
    if landed != label:
        raise StepMissed(f"the cursor landed on {landed}, not {label}")
    game.menu("select")


def die_and_expect_the_level_card(game: Game, report) -> None:
    """Run out of lives, and check the level card comes back — not GAME OVER.

    The bypass is a native override on $59C5B0 (`native_gameover_menu`). It was
    registered once, removed as collateral in a fix for an unrelated level-card
    hang, and nobody noticed for months, because nothing here ever died. So the
    soak dies on purpose now.

    Both screens render from the same copper list, so the check is not "a card
    appeared": it is that the game-over markers in $1093 are gone and that Fire
    puts the player back in the cavern. The GAME OVER menu answers Fire with a
    menu selection, so a bypass that had stopped working would sit on $003914
    with bit6 still set and fail here rather than pass by looking similar.
    """
    keep_pressing(
        game,
        "the level card to be dismissed",
        lambda s: s["cop1lc"] != CARD_SCREEN,
        attempts=12,
        fire=1,
        frames=6,
    )
    game.get("/gameover")
    # The skull banner plays in full before the menu phase the bypass takes —
    # measured at about four seconds, so this waits far longer than that.
    game.until(
        "the card bank to come back after dying",
        lambda s: s["cop1lc"] == CARD_SCREEN,
        frames=900,
    )
    game.settle(100)
    flags = game.byte(GAMEOVER_FLAGS)
    if flags & 0x40:
        raise StepMissed(f"the CONTINUE/GAME OVER screen is up: $1093={flags:02X}")
    keep_pressing(
        game,
        "the reloaded level to start",
        lambda s: s["cop1lc"] == CAVERN_SCREEN,
        attempts=12,
        fire=1,
        frames=6,
    )
    report("died, back at the card")


def round_trip(game: Game, number: int, choose: random.Random, report) -> None:
    reach_main_menu(game)
    game.settle(40)

    # Every other round backs out of the picker with ESC first: cancelling
    # leaves the override by a different exit (rt_jump to $39BE) than
    # confirming does, and both end with the thread being torn down later.
    if number % 2 == 0:
        open_picker(game)
        game.settle(20)
        game.key("escape")
        game.until("the picker to close", lambda s: not s["ui"]["level_select"], frames=400)
        report("picker cancelled")
        game.settle(40)

    open_picker(game)
    for _ in range(choose.randint(0, 3)):
        game.press(r=1, frames=3)
        game.settle(15)
    for _ in range(choose.randint(0, 4)):
        game.press(d=1, frames=3)
        game.settle(15)
    keep_pressing(game, "the level to start", lambda s: s["gameplay_active"], fire=1, frames=4)
    report(f"in level {game.state()['level']}")
    game.settle(choose.randint(25, 200))

    for _ in range(choose.randint(0, 3)):
        game.press(**{choose.choice(("l", "r")): 1, "frames": 6})
        game.settle(10)

    if number % 5 == 0:
        die_and_expect_the_level_card(game, report)

    # A RETRY every third round: it stops and respawns the game thread too,
    # from inside gameplay rather than from the title bank.
    if number % 3 == 0:
        choose_menu_row(game, "RETRY")
        game.settle(200)
        report("retried")
        game.key("escape")
        game.settle(20)

    choose_menu_row(game, "EXIT TO MAIN MENU")
    game.until("the poster to come back", lambda s: not s["gameplay_active"], frames=1200)
    report("back at the poster")
    game.settle(60)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--rounds", type=int, default=40, help="fewer than ~20 is not enough to trust a pass"
    )
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--port", type=int, default=0, help="0 picks a free one")
    # A window by default, because headless does not reproduce the crash this
    # exists for: measured on a build with the fix removed, twenty rounds
    # headless passed and the same twenty windowed died on the eighth. The
    # present path is part of the timing that puts the game thread inside the
    # picker's override when EXIT TO MAIN MENU stops it.
    parser.add_argument(
        "--headless",
        action="store_true",
        help="no window — faster, but does NOT exercise the teardown path this checks",
    )
    parser.add_argument("--out", type=Path, default=ROOT / "build/menu-soak")
    arguments = parser.parse_args(argv)

    arguments.out.mkdir(parents=True, exist_ok=True)
    log = arguments.out / "crash.log"
    output = arguments.out / "game.log"
    log.unlink(missing_ok=True)

    port = arguments.port or choose_port(9100)
    game = start(port, log, output, windowed=not arguments.headless)
    choose = random.Random(arguments.seed)

    def report(step: str) -> None:
        state = game.state()
        print(f"    {step:26s} frame={state['frame']:6d} level={state['level']:2d}")

    failure: str | None = None
    reached = 0
    try:
        for number in range(1, arguments.rounds + 1):
            reached = number
            print(f"round {number}")
            round_trip(game, number, choose, report)
    except (GameDied, StepMissed) as problem:
        failure = str(problem)
    except (urllib.error.URLError, OSError) as problem:
        died = game.process.poll()
        failure = f"the control channel stopped answering ({problem}); exit {died}"

    if game.process.poll() is None:
        game.process.send_signal(signal.SIGTERM)
        try:
            game.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            game.process.kill()

    report_text = log.read_text() if log.exists() else ""
    if report_text.strip():
        print(f"\nthe game wrote a crash report to {log}:\n")
        print(report_text)
    if failure:
        print(f"\nFAILED in round {reached} of {arguments.rounds}: {failure}")
        print(f"the game's own output is in {output}")
        return 1
    where = "headless" if arguments.headless else "windowed"
    print(f"\n{arguments.rounds} rounds in and out of a level ({where}), no crash")
    return 0


if __name__ == "__main__":
    sys.exit(main())
