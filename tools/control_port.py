"""Where a running game's control channel is, and which port a new one takes.

`run.sh` opens the control channel every time, because the alternative is what
it used to be: a player reports something, and the one tool that could look at
it — `/state`, `/fb.ppm`, `/trace` — needs the game restarted under an
environment variable before it can be asked anything, by which time the thing
being reported is gone.

The launcher writes the port it took here, so tooling can find a game that is
already running instead of being told a number.
"""

from __future__ import annotations

import errno
import os
import socket
from pathlib import Path

from tools.paths import ROOT

DEFAULT_PORT = 8613
"""The port `run.sh` opens the control channel on when nothing says otherwise."""

PORT_SEARCH = 16
"""How many ports past the default to try, so a second game still gets one."""

PORT_FILE = ROOT / "build/control-port"
"""Where the launcher records the port of the game it started."""


def configured_port() -> int | None:
    """The port the environment asks for: `None` for the default, 0 for off."""
    raw = os.environ.get("BENEFACTOR_HTTP")
    if raw is None:
        return None
    text = raw.strip().lower()
    if text in ("", "off", "no", "false"):
        return 0
    try:
        port = int(text)
    except ValueError as error:
        raise ValueError(f"BENEFACTOR_HTTP is {raw!r}, which is not a port number") from error
    if not 0 <= port <= 65535:
        raise ValueError(f"BENEFACTOR_HTTP is {port}, which is not a port number")
    return port


def _is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(("127.0.0.1", port))
        except OSError as error:
            if error.errno in (errno.EADDRINUSE, errno.EACCES):
                return False
            raise
    return True


def choose_port(preferred: int = DEFAULT_PORT, search: int = PORT_SEARCH) -> int:
    """A free port at or just past `preferred`, so a second game still starts.

    The port is only probed, not held: the game binds it itself a moment later.
    Two launches racing for the same port is a losing game either way, and the
    game says so in its own log when it cannot listen.
    """
    for candidate in range(preferred, preferred + search):
        if _is_free(candidate):
            return candidate
    return preferred


def record_port(port: int, path: Path = PORT_FILE) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{port}\n", encoding="utf-8")


def forget_port(path: Path = PORT_FILE) -> None:
    path.unlink(missing_ok=True)


def running_port(path: Path = PORT_FILE) -> int | None:
    """The control port of a game that is running now, or `None` if none is.

    The recorded port is confirmed against something listening on it, so a file
    left behind by a game that has since exited reports nothing rather than a
    port that answers no one.
    """
    try:
        port = int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None
    return None if _is_free(port) else port


def main() -> int:
    port = running_port()
    if port is None:
        print("no running game recorded a control port")
        return 1
    print(f"http://127.0.0.1:{port}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
