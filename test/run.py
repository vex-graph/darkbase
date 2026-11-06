#!/usr/bin/env python3
"""Independent bounded owner runner for the R3 darkbase-engine store.

MODULE: run.py
REGISTRY: execute(argv, timeout) bounded child; main() asks the workspace build
graph for the darkbase-engine owner targets, runs them with assertions active,
parses the aggregate, and exits nonzero on any failure.

The engine's own source set is declared in the build graph (`setup_darkbase` in
`tools/workspace.c`), so the graph is the authority for how the owners link —
hand-listing vexspoke/relational-engine objects here would silently drift from
it. This runner therefore drives that graph instead of duplicating it.

Owned outputs: none on disk; the graph writes its own build state.
Platform: current macOS arm64 lab. No Linux/Windows/macOS14 runtime claim.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
B = ROOT / "tools" / "b"
OWNERS = ["database", "database_result", "crc32", "modifier", "page_test", "wal_test", "store_test", "store_adversarial_test"]


def execute(argv, timeout=900):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, cwd=ROOT,
                          env={**os.environ})


def main():
    if not B.exists():
        print("run.py: no workspace build tool at", B)
        return 1
    failures = 0
    for owner in OWNERS:
        try:
            result = execute([str(B), "test", owner])
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT {owner}")
            failures += 1
            continue
        out = result.stdout + result.stderr
        m = re.search(r"(\d+) passed, (\d+) failed, (\d+) timed out, (\d+) skipped", out)
        if (result.returncode != 0 or m is None or int(m.group(1)) == 0 or
                int(m.group(2)) != 0 or int(m.group(3)) != 0 or int(m.group(4)) != 0):
            print(f"FAIL {owner}")
            print(out[-2000:])
            failures += 1
        else:
            print(f"PASS {owner}: {m.group(1)} passed, "
                  f"{m.group(4)} skipped")
    print(("darkbase run.py: OK" if failures == 0 else f"darkbase run.py: {failures} failure(s)"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
