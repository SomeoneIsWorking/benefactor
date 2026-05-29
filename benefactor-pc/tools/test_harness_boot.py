"""
Harness boot TDD tests.

These tests lock down the invariant:
  PUAE frame 0 == PC frame 0  (and subsequent frames)

Run from repo root:
    python3 -m pytest benefactor-pc/tools/test_harness_boot.py -v

Or build first then test:
    cd benefactor-pc/build && cmake --build . --target benefactor-harness -j$(nproc) && cd ../..
    python3 -m pytest benefactor-pc/tools/test_harness_boot.py -v
"""
import subprocess
import os
import re
import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPORT_PATH = os.path.join(REPO_ROOT, "logs", "harness_report.txt")
HEADLESS_SCRIPT = os.path.join(REPO_ROOT, "run_harness_headless.sh")


def run_harness(frames=1):
    """Run the headless harness for N frames. Returns CompletedProcess."""
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "offscreen"
    return subprocess.run(
        ["bash", HEADLESS_SCRIPT, "--frames", str(frames),
         "--report", REPORT_PATH],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )


def parse_report(path):
    """Return list of dicts: [{frame, field, puae, pc, match}, ...]"""
    diffs = []
    if not os.path.exists(path):
        return diffs
    with open(path) as f:
        content = f.read()
    # Lines like: "  cop[000] COP1LC: PUAE=$007BC8 PC=$007BC8"
    for m in re.finditer(
        r'cop\[(\d+)\]\s+(\w+):\s+PUAE=\$([0-9A-Fa-f]+)\s+PC=\$([0-9A-Fa-f]+)',
        content
    ):
        idx, field, puae_v, pc_v = m.groups()
        diffs.append({
            "idx": int(idx),
            "field": field,
            "puae": int(puae_v, 16),
            "pc":   int(pc_v, 16),
            "match": puae_v.lower() == pc_v.lower(),
        })
    return diffs


# ── Tests ───────────────────────────────────────────────────────────────────

class TestBootFrame0:
    """Frame 0 (first post-boot frame) must be identical between PUAE and PC."""

    @pytest.fixture(scope="class")
    def result(self):
        return run_harness(frames=1)

    def test_exit_code(self, result):
        """Harness exits 0 on PERFECT MATCH."""
        assert result.returncode == 0, (
            f"Harness exited {result.returncode} (DIFF detected on frame 0)\n"
            f"stdout tail:\n{result.stdout[-3000:]}"
        )

    def test_frame1_ok_in_output(self, result):
        """Stdout reports 'Frame 1: ok' (which is post-boot frame 0)."""
        assert "Frame 1: ok" in result.stdout or "PERFECT MATCH" in result.stdout, (
            f"Expected 'Frame 1: ok' in harness output.\n{result.stdout[-2000:]}"
        )

    def test_cop1lc_matches(self, result):
        """cop1lc in frame 0 must agree (both pointing at $7BC8 or $86CC)."""
        # Only meaningful if harness passed — but parse anyway for diagnostics
        diffs = parse_report(REPORT_PATH)
        cop1lc_diffs = [d for d in diffs if d["field"] == "COP1LC" and not d["match"]]
        assert not cop1lc_diffs, (
            f"COP1LC mismatch in frame 0: {cop1lc_diffs}"
        )


class TestLockstepFrames:
    """All N lockstep frames must match after a clean boot."""

    @pytest.fixture(scope="class")
    def result(self):
        return run_harness(frames=3)

    def test_exit_code(self, result):
        assert result.returncode == 0, (
            f"Harness exited {result.returncode} on 3-frame run.\n"
            f"stdout tail:\n{result.stdout[-3000:]}"
        )

    def test_perfect_match_in_output(self, result):
        assert "PERFECT MATCH" in result.stdout or (
            "Frame 1: ok" in result.stdout and
            "Frame 2: ok" in result.stdout and
            "Frame 3: ok" in result.stdout
        ), f"Not all frames matched.\n{result.stdout[-2000:]}"
