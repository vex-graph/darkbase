# darkbase — the R5 consumer

The host that **uses** the [darkbase-engine](../../drivers/darkbase-engine) store.
The engine owns the `Database` contract and the `.vexdb` format; this repository
owns the consuming application: it decides what an entity *means*.

## Current State

**Role:** R5 consumer of the R3 native vex store. The seam is one-way — this repo
borrows the engine's public contract (an opaque `Database *` and its functions);
the engine never learns what an application entity is.

Implemented and proven on macOS arm64: `src/main.c` registers a reflection `Struct`
as an entity, binds two caller-owned `Message` rows, writes a `.vexdb`, reopens it
in a fresh `Database` and reads the rows back byte-for-byte. Built by `sh build.sh`;
14 checks, exit nonzero on any failure. The runtime build links the archives the
workspace build graph produces (`setup_darkbase` in `tools/workspace.c`), so the
engine's source set is declared in exactly one place.

This is the consumer's **first slice**, not an application: there is no UI, no
identity, no schema evolution, no transactions and no network. It exists so the
engine is no longer an island — before it, nothing in the ecosystem called the
store at all.

## Scope and Limitations

- **Covers:** schema registration, live-row binding, insert, count, row lookup,
  `Database_save` / `Database_load` round trip, and the store's cold rejection
  codes surfaced to the caller.
- **Does not cover:** any UI or terminal surface; users/servers/channels/messages
  as product concepts (that is Primeagen's direction, not this repo's); migrations;
  concurrent access; the mmap/paged store, transactions/WAL, reactive programs or
  external drivers, which the engine's README lists as unimplemented.
- **Limits:** owner-affine single-threaded use; rows are caller-owned and must
  outlive the `Database`; the file is native-endian by design (the Byte-Native
  Persistence Law), so it is not portable across endianness.
- **Platform:** proven on this macOS arm64 lab only. Windows/Linux are unproven.
- **Lawbook:** no `darkbase-preferences.md` yet — a known gap recorded in the
  workspace reading map, not an exemption from the universal laws.
