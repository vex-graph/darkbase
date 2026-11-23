# darkbase

R3 native Vexgraph database interface and durable page-level store built over R2 reflection.

```bash
./tools/b build darkbase
python3 tests/darkbase-engine/store/run.py
python3 tests/darkbase-engine/database/run.py
```

## About

`darkbase` owns the `Database` interface and native `.vexdb` storage engine for the ecosystem (future library name `darkbase-db`, with an R5 interactive executable `darkbase` planned on top). It is **not** a SQL engine: schemas are defined directly through R2 reflection (`Struct`, `Field`, `Class`, `Method`) rather than DDL, and queries use a structured relational API. `api-haven` holds only connector/catalog descriptors and delegates execution here.

See [`docs/store-format.md`](docs/store-format.md) for the page and WAL binary layout.

## Features

- **Reflection-backed entity model (`src/database`)**: Registers R2 `Struct` schemas (`Struct.size` is row stride), supports borrowed rows (`Database_insert`) and exact-stride owned copy-in (`Database_insertOwned`), and exposes a dest-last `DatabaseResult` cursor.
- **Structured queries and indexes**:
  - `Database_queryEqual`: exact-byte match on a reflected `Field`.
  - `Database_queryInt32Range`: inclusive signed 32-bit range scan.
  - `Database_indexBuild`: key-sorted in-memory index for `REFLECT_FIELD_INDEXED` `i32` fields, persisted alongside entity row spans and re-validated on load.
- **Page-level durable store (`src/store`, `src/page`, `src/wal`)**: Single-file page store combining mapped reservation, sealed pages, and an embedded WAL redo region. Supports create/reopen, page append/replace, free-list reuse, committed redo replay, and whole-image catalog/row persistence (`Database_saveStore` / `Database_loadStore` + `Database_storeCatalogEntry`).
- **Legacy snapshot compatibility**: `Database_save` / `Database_load` still read/write the older flat `VEXDB01` CRC32 snapshot via atomic temp-write + `fsync` + rename while callers transition to the page store.

## Layout

- `src/oop/type.h`, Class registry (`PROJ_DARKBASE`).
- `src/database/`, `Database`, `DatabaseResult`, queries, indexes, and store/snapshot bridges.
- `src/store/`, `src/page/`, `src/wal/`, `src/util/`, Page-level durable store, page seals, embedded redo WAL, and CRC32.
- `../../../tests/darkbase-engine/`, Mirrored owner tests (`store/run.py`, `database/run.py`).

## Build and test

From the `worktree/` root:

```bash
./tools/b build darkbase
python3 tests/darkbase-engine/store/run.py
python3 tests/darkbase-engine/database/run.py
```

## Future

- Implement embedded WAL recycling and checkpoint compaction so journals do not fill permanently.
- Move from whole-image `Database_saveStore` rewrites to incremental page-level rows, indexes, and multi-page transactions.
- Add field projection, offset-based codecs, reactive `DbTrigger` programs, CSV/JSON/header export, and quarantined external driver dylibs (SQLite, Postgres).

## Scope and Limitations

- **Not a SQL engine**: No SQL parser, planner, or vendor client libraries in the core.
- **Single-writer, owner-affine**: Requires trusted stable paths and external serialization; no OS file locks or multi-process concurrency.
- **Journal capacity**: Embedded WAL currently retains all redo records and rejects new writes once full (`Store_sync` does not compact history).
- **Whole-image bridge**: `Database_saveStore` / `Database_loadStore` currently publish whole entity/index images per root commit rather than incremental row updates.
- **macOS arm64 only**: Owner tests and C ASan/UBSan suites are proven on Apple Silicon macOS; Windows is untested.
