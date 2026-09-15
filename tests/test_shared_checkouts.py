from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import shared_checkouts
from tools.shared_checkouts import (
    TREES,
    SharedTree,
    _align_checkout,
    pinned_revision,
    resolve_tree,
    vendored_subtrees,
)


def _git(*arguments: str, cwd: Path) -> str:
    return subprocess.run(
        ["git", *arguments], cwd=cwd, text=True, capture_output=True, check=True
    ).stdout.strip()


def _commit(repository: Path, name: str) -> str:
    (repository / name).write_text(name, encoding="utf-8")
    _git("add", name, cwd=repository)
    _git("commit", "--quiet", "-m", name, cwd=repository)
    return _git("rev-parse", "HEAD", cwd=repository)


def _repository(path: Path) -> Path:
    path.mkdir(parents=True)
    _git("init", "--quiet", "--initial-branch", "main", ".", cwd=path)
    _git("config", "user.email", "test@example.com", cwd=path)
    _git("config", "user.name", "Test", cwd=path)
    return path


def _tree(name: str, checkout: Path) -> SharedTree:
    return SharedTree(
        name=name,
        repository="SomeoneIsWorking/lucent",
        variable="BENEFACTOR_TEST_DIR",
        candidates=(checkout,),
        provision=checkout,
    )


class PinnedRevisionTests(unittest.TestCase):
    def test_reads_the_revision_the_release_workflow_pins(self) -> None:
        revision = pinned_revision("SomeoneIsWorking/setup-ui")
        self.assertRegex(revision, r"^[0-9a-f]{40}$")

    def test_refuses_a_repository_the_workflow_does_not_pin(self) -> None:
        with self.assertRaises(RuntimeError):
            pinned_revision("someone/not-pinned")


class ResolveTests(unittest.TestCase):
    def setUp(self) -> None:
        shared_checkouts._RESOLVED.clear()
        self.addCleanup(shared_checkouts._RESOLVED.clear)

    def test_an_override_is_used_as_it_stands(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            elsewhere = Path(directory) / "elsewhere"
            elsewhere.mkdir()
            tree = _tree("lucent", Path(directory) / "unused")
            with patch.dict("os.environ", {tree.variable: str(elsewhere)}):
                self.assertEqual(resolve_tree(tree), elsewhere.resolve())

    def test_an_override_that_is_not_a_directory_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tree = _tree("lucent", Path(directory) / "unused")
            with (
                patch.dict("os.environ", {tree.variable: str(Path(directory) / "gone")}),
                self.assertRaises(RuntimeError),
            ):
                resolve_tree(tree)

    def test_a_miss_names_every_path_it_tried(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            absent = Path(directory) / "absent"
            tree = _tree("lucent", absent)
            with (
                patch.dict("os.environ", {tree.variable: ""}),
                self.assertRaises(RuntimeError) as raised,
            ):
                resolve_tree(tree, provision=False)
            self.assertIn(str(absent), str(raised.exception))


class AlignTests(unittest.TestCase):
    def test_a_clean_checkout_behind_the_pin_is_moved_onto_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = _repository(Path(directory) / "lucent")
            _commit(checkout, "first")
            pinned = _commit(checkout, "second")
            _git("reset", "--quiet", "--hard", "HEAD~1", cwd=checkout)

            _align_checkout(_tree("lucent", checkout), checkout, pinned)

            self.assertEqual(_git("rev-parse", "HEAD", cwd=checkout), pinned)
            self.assertEqual(_git("rev-parse", "--abbrev-ref", "HEAD", cwd=checkout), "main")

    def test_a_checkout_with_its_own_commits_is_left_alone(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = _repository(Path(directory) / "lucent")
            _commit(checkout, "first")
            pinned = _commit(checkout, "second")
            _git("reset", "--quiet", "--hard", "HEAD~1", cwd=checkout)
            own = _commit(checkout, "mine")

            with self.assertLogs("benefactor.shared", level="WARNING"):
                _align_checkout(_tree("lucent", checkout), checkout, pinned)

            self.assertEqual(_git("rev-parse", "HEAD", cwd=checkout), own)

    def test_an_uncommitted_change_is_left_alone(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = _repository(Path(directory) / "lucent")
            _commit(checkout, "first")
            pinned = _commit(checkout, "second")
            _git("reset", "--quiet", "--hard", "HEAD~1", cwd=checkout)
            head = _git("rev-parse", "HEAD", cwd=checkout)
            (checkout / "first").write_text("edited", encoding="utf-8")

            with self.assertLogs("benefactor.shared", level="WARNING"):
                _align_checkout(_tree("lucent", checkout), checkout, pinned)

            self.assertEqual(_git("rev-parse", "HEAD", cwd=checkout), head)
            self.assertEqual((checkout / "first").read_text(encoding="utf-8"), "edited")

    def test_a_checkout_past_the_pin_is_untouched_and_unreported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = _repository(Path(directory) / "lucent")
            pinned = _commit(checkout, "first")
            ahead = _commit(checkout, "second")

            with patch.object(shared_checkouts, "LOGGER") as logger:
                _align_checkout(_tree("lucent", checkout), checkout, pinned)

            logger.warning.assert_not_called()
            self.assertEqual(_git("rev-parse", "HEAD", cwd=checkout), ahead)


class VendoredSubtreeTests(unittest.TestCase):
    """What a first-party gate has to be told to skip."""

    def test_every_in_repository_layout_is_named(self) -> None:
        inside = set(vendored_subtrees())
        for tree in TREES:
            self.assertIn(tree.provision, inside, tree.name)

    def test_nothing_outside_the_repository_is_named(self) -> None:
        """A path beside the repository is already outside any `--root`."""
        for subtree in vendored_subtrees():
            self.assertTrue(subtree.is_relative_to(shared_checkouts.ROOT), subtree)
            self.assertNotEqual(subtree, shared_checkouts.ROOT)


if __name__ == "__main__":
    unittest.main()
