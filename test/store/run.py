#!/usr/bin/env python3
"""Bounded fresh-source proof for Store and its Page/Wal dependency owners.

MODULE: store/run.py
REGISTRY: run(argv) literal bounded child; main() builds the actual R2 Rust
staticlib offline/locked, compiles native R2 dependencies in their owning include
contexts, then executes page/wal/store owners optimized and C ASan/UBSan.
Every C object is instrumented in sanitized mode, including File/Memory/SpinLock.
Rust sanitizer instrumentation and hardware power-loss testing remain gaps.
No cached workspace archives or source-tree output; all fixtures/output disposable.
This independently registered runner does not bypass or modify the global ratchet.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from collections import Counter

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / "suite/ecosystem/relational-engine"
CPU = ROOT / "suite/ecosystem/vexspoke"
DB = ROOT / "suite/ecosystem/darkbase-engine"
SUITE = ROOT / "darkbase-engine/test"


def run(argv, *, env=None, timeout=90):
    return subprocess.run([str(x) for x in argv], cwd=ROOT, env=env,
                          check=True, timeout=timeout)


def store_diagnostics(result, injected):
    """Exact expected cold rejection counts; quiet valid flows add no diagnostics."""
    expected = Counter({
        "Store: invalid path or reservation/journal geometry": 4,
        "Store: empty reopen path": 2,
        "Store: owner is closed": 2,
        "Store: bootstrap geometry/version rejected": 1,
        "Store: bootstrap file could not be opened": 1,
        "Store: creation failed; any new artifact is retained for explicit recovery": 1,
        "Store: invalid page append input": 4,
        "Store: page id outside allocation range": 2,
        "Store: read destination too small; no bytes copied": 1,
        "Store: missing read destination/output": 1,
        "Store: page already released": 1,
        "Store: cannot replace a released page": 1,
        "Store exhausted: request 512 bytes, bound 9728 bytes": 1,
        "Wal_append: region exhausted, request 576 bytes (remaining 424)": 1,
        "Store exhausted: request 544 bytes, bound 1024 bytes": 1,
        "Store: reopen/recovery rejected; no owner admitted": 1,
    })
    if injected:
        expected.update({"Store: redo application failed": 2,
                         "Store: uncertain commit; close and reopen": 10,
                         "Store: data sync failed after durable redo": 1,
                         "Store: journal outcome uncertain": 2})
    lines = result.stderr.splitlines()
    assert all(line.startswith("[vex] ") and ": " in line for line in lines), result.stderr
    actual = Counter(line.split(": ", 1)[1] for line in lines)
    assert actual == expected, (actual - expected, expected - actual)


def execute_owner(binary, *, store=False, injected=False, adversarial=False):
    result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stdout + result.stderr
    if store:
        store_diagnostics(result, injected)
    if adversarial:
        actual = Counter(line.split(": ", 1)[1] for line in result.stderr.splitlines())
        expected = Counter({"Wal_3: region admission failed (corrupt)": 1,
                            "Store: reopen/recovery rejected; no owner admitted": 2,
                            "Page_verify: the page seal does not match its bytes": 2,
                            "Store: missing read destination/output": 4,
                            "Store: page id outside allocation range": 2,
                            "Store: bootstrap geometry/version rejected": 1})
        assert actual == expected, (actual - expected, expected - actual)
    print(result.stdout, end="", flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix="store-proof-", dir=os.environ.get("TMPDIR")) as tmp:
        target = Path(tmp)
        env = dict(os.environ, CARGO_TARGET_DIR=str(target / "cargo"), RUSTFLAGS="-D warnings")
        run(["cargo", "build", "--offline", "--locked", "--manifest-path",
             ENGINE / "rust/Cargo.toml"], env=env)
        archive = target / "cargo/debug/librelational_engine_scratchpad.a"
        for mode, sanitizer in [("optimized", []), ("sanitized", ["-fsanitize=address,undefined"])]:
            flags = ["-std=gnu23", "-Wall", "-Wextra", "-Werror", "-O2", "-UNDEBUG",
                     "-mcpu=apple-m1", "-mmacosx-version-min=14.0", *sanitizer]
            objects = []
            fault_object = target / f"{mode}-store-fault.o"
            sources = [(ENGINE / "src/nio/mem.c", ENGINE),
                       (ENGINE / "src/io/file.c", CPU),
                       (ENGINE / "src/type/type.c", ENGINE),
                       (CPU / "src/atomic/spin.c", CPU),
                       (DB / "src/page/page.c", DB),
                       (DB / "src/wal/wal.c", DB),
                       (DB / "src/util/crc32.c", DB),
                       (DB / "src/store/store.c", DB)]
            for index, (source, owner) in enumerate(sources):
                obj = target / f"{mode}-{index}.o"
                includes = ["-I" + str(owner / "src"), "-I" + str(ENGINE / "src"),
                            "-I" + str(CPU / "src"), "-I" + str(ENGINE / "rust/include")]
                run(["clang", *flags, *includes, "-c", source, "-o", obj])
                if source == DB / "src/store/store.c":
                    inject = ["-Dre_gmap_write=StoreTest_mapWrite", "-Dre_gmap_sync=StoreTest_mapSync",
                              "-Dre_gmap_grow_to=StoreTest_mapGrow", "-DWal_append=StoreTest_walAppend",
                              "-DWal_sync=StoreTest_walSync"]
                    run(["clang", *flags, *includes, *inject, "-c", source, "-o", fault_object])
                objects.append(obj)
            for name in ("page", "wal", "store", "store_adversarial"):
                binary = target / f"{mode}-{name}"
                owner_objects = objects if name.startswith("store") else objects[:-1]
                owner_directory = "store" if name.startswith("store") else name
                run(["clang", *flags, "-I" + str(DB / "src"), "-I" + str(ENGINE / "src"),
                     "-I" + str(CPU / "src"), "-I" + str(ENGINE / "rust/include"),
                     SUITE / owner_directory / f"{name}_test.c", *owner_objects, archive, "-o", binary])
                execute_owner(binary, store=name == "store", adversarial=name == "store_adversarial")
                print(f"PASS: {mode} {name}_test", flush=True)
            binary = target / f"{mode}-store-fault"
            run(["clang", *flags, "-I" + str(DB / "src"), "-I" + str(ENGINE / "src"),
                 "-I" + str(CPU / "src"), "-I" + str(ENGINE / "rust/include"),
                 "-DSTORE_FAULT_TESTS=1", SUITE / "store/store_test.c", *objects[:-1],
                 fault_object, archive, "-o", binary])
            execute_owner(binary, store=True, injected=True)
            print(f"PASS: {mode} Store five-stage IO faults and exact cold diagnostics", flush=True)
            if mode == "optimized":
                run(["/usr/bin/leaks", "-quiet", "-atExit", "--", binary, "--leak-safe"], timeout=60)
        for arity in (2, 3):
            args = ",".join(["0"] * arity)
            source = f'#include "store/store.h"\nvoid probe(void) {{ (void) Store({args}); }}\n'
            result = subprocess.run(["clang", "-std=gnu23", "-Wall", "-Wextra", "-Werror",
                                     "-fsyntax-only", "-x", "c", "-", "-I" + str(DB / "src"),
                                     "-I" + str(CPU / "src"), "-I" + str(ENGINE / "src"),
                                     "-I" + str(ENGINE / "rust/include")],
                                    input=source, capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and f"Store_{arity}" in result.stderr, result.stderr
        print("PASS: unsupported Store constructor arities reject for intended symbols", flush=True)


if __name__ == "__main__":
    main()
