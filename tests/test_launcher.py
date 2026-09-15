from __future__ import annotations

import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import control_port
from tools.launcher import control_arguments, runtime_blocker


class LauncherTests(unittest.TestCase):
    def test_names_missing_amigaport(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "amigaport"
            self.assertIn("shared/amigaport is missing", runtime_blocker(missing) or "")

    def test_present_runtime_has_title_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            present = Path(directory) / "amigaport"
            present.mkdir()
            self.assertIsNone(runtime_blocker(present))

    def test_runtime_override_uses_canonical_shared_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            present = Path(directory) / "amigaport"
            present.mkdir()
            with patch.dict("os.environ", {"BENEFACTOR_AMIGAPORT_DIR": str(present)}):
                self.assertIsNone(runtime_blocker())


class ControlChannelTests(unittest.TestCase):
    """`run.sh` opens the control channel, because a live report is the only one."""

    def test_a_plain_launch_opens_the_channel_on_a_free_port(self) -> None:
        """Which port is free is the host's business; asking for one is not."""
        with (
            patch.dict("os.environ", {}, clear=False),
            patch.object(control_port, "choose_port", return_value=8613) as choosing,
            patch("tools.launcher.choose_port", choosing),
        ):
            import os

            os.environ.pop("BENEFACTOR_HTTP", None)
            self.assertEqual(control_arguments(()), ["--http", "8613"])
        choosing.assert_called_once_with()

    def test_the_default_port_is_taken_when_it_is_free(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            free = probe.getsockname()[1]
        self.assertEqual(control_port.choose_port(free), free)

    def test_the_environment_can_name_the_port(self) -> None:
        with patch.dict("os.environ", {"BENEFACTOR_HTTP": "9100"}):
            self.assertEqual(control_arguments(()), ["--http", "9100"])

    def test_the_environment_can_close_the_channel(self) -> None:
        for off in ("0", "off", "no"):
            with self.subTest(off=off), patch.dict("os.environ", {"BENEFACTOR_HTTP": off}):
                self.assertEqual(control_arguments(()), [])

    def test_a_port_the_player_passed_is_left_alone(self) -> None:
        """Two `--http` arguments would be one argument too many."""
        with patch.dict("os.environ", {}, clear=False):
            import os

            os.environ.pop("BENEFACTOR_HTTP", None)
            self.assertEqual(control_arguments(("--http", "9999")), [])

    def test_an_unreadable_port_is_named_rather_than_ignored(self) -> None:
        with patch.dict("os.environ", {"BENEFACTOR_HTTP": "banana"}):
            with self.assertRaises(ValueError) as raised:
                control_arguments(())
            self.assertIn("banana", str(raised.exception))

    def test_a_second_game_is_given_a_port_of_its_own(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as held:
            held.bind(("127.0.0.1", 0))
            held.listen(1)
            taken = held.getsockname()[1]
            self.assertNotEqual(control_port.choose_port(taken), taken)

    def test_a_port_file_left_by_a_game_that_exited_reports_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            stale = Path(directory) / "control-port"
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.bind(("127.0.0.1", 0))
                free = probe.getsockname()[1]
            control_port.record_port(free, stale)
            self.assertIsNone(control_port.running_port(stale))

    def test_a_running_game_is_found_on_the_port_it_recorded(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listening:
            listening.bind(("127.0.0.1", 0))
            listening.listen(1)
            port = listening.getsockname()[1]
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "control-port"
                control_port.record_port(port, path)
                self.assertEqual(control_port.running_port(path), port)
                control_port.forget_port(path)
                self.assertIsNone(control_port.running_port(path))


if __name__ == "__main__":
    unittest.main()
