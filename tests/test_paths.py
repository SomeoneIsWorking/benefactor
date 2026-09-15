"""What "beside this repository" means, including from inside a worktree.

Every shared checkout is found by looking next to this repository, and a linked
git worktree is not next to them: it sits under `.claude/worktrees/<name>`, so
the lookup pointed at a directory that holds worktrees and nothing else. The
visible cost was a verify run that could not find `lucent` and skipped the C++
gates, which is the quiet kind of failure this project does not accept. The
rule that fixes it is three lines of path arithmetic, so it gets a test.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.paths import _main_checkout


class MainCheckoutTests(unittest.TestCase):
    def test_an_ordinary_checkout_is_its_own(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "benefactor"
            (root / ".git").mkdir(parents=True)
            self.assertEqual(_main_checkout(root), root)

    def test_a_worktree_resolves_to_the_checkout_it_was_made_from(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            main = Path(directory) / "benefactor"
            worktree = main / ".claude" / "worktrees" / "some-name"
            worktree.mkdir(parents=True)
            (main / ".git" / "worktrees" / "some-name").mkdir(parents=True)
            (worktree / ".git").write_text(
                f"gitdir: {main}/.git/worktrees/some-name\n", encoding="utf-8"
            )
            self.assertEqual(_main_checkout(worktree), main)

    def test_a_gitdir_file_in_a_layout_we_do_not_know_is_left_alone(self) -> None:
        """A submodule's `.git` is also a file, and its parent is not a worktree."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "vendored"
            root.mkdir(parents=True)
            (root / ".git").write_text(
                f"gitdir: {directory}/outer/.git/modules/vendored\n", encoding="utf-8"
            )
            self.assertEqual(_main_checkout(root), root)

    def test_a_checkout_with_no_git_at_all_is_its_own(self) -> None:
        """A release tarball or an exported tree still has to resolve to something."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "exported"
            root.mkdir(parents=True)
            self.assertEqual(_main_checkout(root), root)


if __name__ == "__main__":
    unittest.main()
