#!/usr/bin/env python3
"""Fresh, bounded owner build for the Darkbase Database persistence seam.

MODULE: database/run.py
Builds the real Relational Engine Rust mapping archive and a strict C23 client
from the current owner sources; generated inputs and binaries stay disposable.
The workspace Vexspoke ownership ratchet remains untouched and independently
blocks the normal graph until its own baseline is repaired.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from collections import Counter

ROOT = Path(__file__).resolve().parents[3]
DB = ROOT / "suite/ecosystem/darkbase-engine"
CPU = ROOT / "suite/ecosystem/vexspoke"
ENGINE = ROOT / "suite/ecosystem/relational-engine"
TEST = ROOT / "darkbase-engine/test/database/database_test.c"
RESULT_TEST = ROOT / "darkbase-engine/test/database/database_result_test.c"


def run(argv, *, cwd=ROOT, env=None, capture=False, timeout=120, check=True):
    return subprocess.run([str(value) for value in argv], cwd=cwd, env=env,
                          check=check, capture_output=capture, text=capture,
                          timeout=timeout)


def main():
    with tempfile.TemporaryDirectory(prefix="darkbase-database-",
                                     dir=os.environ.get("TMPDIR")) as temp:
        build = Path(temp)
        cargo_env = dict(os.environ, CARGO_TARGET_DIR=str(build / "cargo"),
                         RUSTFLAGS="-D warnings")
        run(["cargo", "build", "--offline", "--locked", "--manifest-path",
             ENGINE / "rust/Cargo.toml"], env=cargo_env)
        archive = build / "cargo/debug/librelational_engine_scratchpad.a"
        sources = [
            (ENGINE / "src/nio/mem.c", ENGINE),
            (ENGINE / "src/io/file.c", CPU),
            (ENGINE / "src/type/type.c", ENGINE),
            (CPU / "src/atomic/spin.c", CPU),
            (CPU / "src/oop/stride.c", CPU),
            (CPU / "src/reflection/field.c", CPU),
            (CPU / "src/reflection/method.c", CPU),
            (CPU / "src/reflection/class.c", CPU),
            (CPU / "src/reflection/variable.c", CPU),
            (CPU / "src/reflection/struct.c", CPU),
            (CPU / "src/relational/variable_slot.c", CPU),
            (CPU / "src/struct/collection.c", CPU),
            (CPU / "src/struct/exhaustion.c", CPU),
            (CPU / "src/struct/chunked_list.c", CPU),
            (DB / "src/util/crc32.c", DB),
            (DB / "src/page/page.c", DB),
            (DB / "src/wal/wal.c", DB),
            (DB / "src/store/store.c", DB),
            (DB / "src/database/database_result.c", DB),
            (DB / "src/database/database.c", DB),
        ]
        flags = ["-std=gnu23", "-Wall", "-Wextra", "-Werror", "-O2", "-UNDEBUG",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-mcpu=apple-m1", "-mmacosx-version-min=14.0"]
        objects = []
        for index, (source, owner) in enumerate(sources):
            obj = build / f"{index}.o"
            include_roots = [owner / "src", DB / "src", CPU / "src",
                             ENGINE / "src", ENGINE / "rust/include"]
            includes = []
            for include_root in include_roots:
                option = "-I" + str(include_root)
                if option not in includes:
                    includes.append(option)
            run(["clang", *flags, *includes, "-c", source, "-o", obj])
            objects.append(obj)
        binary = build / "database_test"
        run(["clang", *flags, "-I" + str(DB / "src"),
             "-I" + str(CPU / "src"), "-I" + str(ENGINE / "src"),
             "-I" + str(ENGINE / "rust/include"), "-I" + str(ROOT / "tests"),
             TEST, *objects, archive, "-o", binary])
        result = run([binary], capture=True, timeout=60, check=False)
        if result.stdout:
            print(result.stdout, end="")
        if result.returncode != 0:
            if result.stderr:
                print(result.stderr, end="", file=__import__("sys").stderr)
            raise AssertionError(f"Database owner failed with exit {result.returncode}")
        expected = Counter({
            "Database_define: duplicate entity": 1,
            "Database_insert: no such entity": 1,
            "Database_insert: null argument": 1,
            "Database_insertOwned: row length does not equal schema stride": 2,
            "Database_insertOwned: cannot mix borrowed and owned rows": 1,
            "Database_insert: cannot bind a live row into a file-loaded entity": 2,
            "Database_define: null database or schema": 1,
            "Database_load: payload rejected": 1,
            "Database_load: checksum mismatch": 1,
            "Database_save: entity has no row stride": 1,
            "Wal_append: region exhausted, request 576 bytes (remaining 296)": 1,
            "Store exhausted: request 544 bytes, bound 2048 bytes": 1,
            "Database_saveStore: data-page write failed before publication": 1,
            "Database_loadStore: registered schemas rejected snapshot": 2,
            "Database_queryEqual: field span or value size is invalid": 2,
            "Database_queryEqual: no such field": 1,
            "Database_queryEqual: no such entity": 1,
            "Database_queryEqual: null or empty argument": 1,
            "Database_queryInt32Range: invalid argument or range": 1,
            "Database_queryInt32Range: field is not a valid int32 span": 2,
            "Database_indexBuild: null or empty argument": 1,
            "Database_indexBuild: no such entity": 1,
            "Database_indexBuild: invalid or unknown field": 1,
            "Database_indexBuild: field is not a declared int32 index": 1,
            "Database_fieldSpan: null argument": 3,
            "Database_fieldSpan: no such entity": 1,
            "Database_fieldSpan: field did not resolve": 1,
            "Database_fieldSpan: field span leaves the row": 1,
            "Database_storeCatalogEntry: catalog index is absent": 1,
        })
        actual = Counter(line.split(": ", 1)[1] for line in result.stderr.splitlines())
        if actual != expected:
            raise AssertionError(f"Database diagnostic mismatch: {actual - expected}; "
                                 f"missing {expected - actual}")
        result_binary = build / "database_result_test"
        run(["clang", *flags, "-I" + str(DB / "src"),
             "-I" + str(CPU / "src"), "-I" + str(ENGINE / "src"),
             "-I" + str(ENGINE / "rust/include"), "-I" + str(ROOT / "tests"),
             RESULT_TEST, *objects, archive, "-o", result_binary])
        cursor = run([result_binary], capture=True, timeout=60, check=False)
        if cursor.stdout:
            print(cursor.stdout, end="")
        if cursor.returncode != 0:
            raise AssertionError(f"DatabaseResult owner failed with exit {cursor.returncode}")
        cursor_expected = Counter({
            "DatabaseResult_2: null row source": 1,
            "Database_select: no such entity": 1,
        })
        cursor_actual = Counter(line.split(": ", 1)[1]
                                for line in cursor.stderr.splitlines())
        if cursor_actual != cursor_expected:
            raise AssertionError(f"DatabaseResult diagnostic mismatch: "
                f"{cursor_actual - cursor_expected}; missing {cursor_expected - cursor_actual}")
        print("PASS: strict optimized C ASan/UBSan Database and DatabaseResult owners")


if __name__ == "__main__":
    main()
