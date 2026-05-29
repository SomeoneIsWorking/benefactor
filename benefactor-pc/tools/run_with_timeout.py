#!/usr/bin/env python3
"""Run benefactor-pc with a hard timeout and capture output."""
import subprocess, sys, os

def main():
    if len(sys.argv) < 2:
        print("Usage: run_with_timeout.py <seconds> <benefactor-pc args...>")
        sys.exit(1)

    timeout_sec = float(sys.argv[1])
    cmd = sys.argv[2:]

    print(f"[harness] Running: {' '.join(cmd)}")
    print(f"[harness] Timeout: {timeout_sec}s")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
        )
        print(result.stdout, end='')
        print(result.stderr, end='')
        print(f"[harness] Exit code: {result.returncode}")
    except subprocess.TimeoutExpired as e:
        if e.stdout:
            print(e.stdout.decode('utf-8', errors='replace'), end='')
        if e.stderr:
            print(e.stderr.decode('utf-8', errors='replace'), end='')
        print(f"[harness] KILLED after {timeout_sec}s timeout")
    except Exception as e:
        print(f"[harness] Error: {e}")

if __name__ == '__main__':
    main()
