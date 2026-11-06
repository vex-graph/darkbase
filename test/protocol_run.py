#!/usr/bin/env python3
"""Bounded protocol owner: strict/optimized and ASan/UBSan real C clients.

Compiles only the transport-independent protocol and its public-header owner.
Temporary artifacts are outside source. No sockets, installs, credentials or
provider calls; standalone codec evidence is not the full service linkage proof.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "interfaces/darkbase-headless/src/protocol/headless_protocol.c"
OWNER = ROOT / "darkbase-headless/test/protocol/headless_protocol_test.c"
INCLUDES = [ROOT / "interfaces/darkbase-headless/src", ROOT / "host-behavior/vexspoke/src"]


def main():
    with tempfile.TemporaryDirectory(prefix="headless-protocol-") as directory:
        output = Path(directory) / "owner"
        base = ["clang", "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-UNDEBUG"]
        if sys.platform == "darwin":
            base += ["-arch", "arm64", "-mcpu=apple-m1", "-mmacosx-version-min=14.0"]
        for include in INCLUDES:
            base += ["-I", str(include)]
        for flags in (["-O2"], ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
            subprocess.run(base + flags + [str(SOURCE), str(OWNER), "-o", str(output)], check=True, timeout=60)
            subprocess.run([str(output)], check=True, timeout=30,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})
        for call in ("HeadlessProtocol(backing)", "HeadlessProtocol(backing, 16, 0)"):
            negative = Path(directory) / "arity.c"
            negative.write_text('#include "protocol/headless_protocol.h"\n'
                                f'int main(void) {{ uint8_t backing[16]; (void) {call}; }}\n')
            result = subprocess.run(base + ["-fsyntax-only", str(negative)], capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode != 0 and "HeadlessProtocol_" in result.stderr, result.stderr
    print("protocol_run: optimized + ASan/UBSan owners and arity negatives passed; leak/other-platform proof absent")


if __name__ == "__main__":
    main()
