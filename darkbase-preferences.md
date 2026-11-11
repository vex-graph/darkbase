# darkbase — Repo-Local Living Preferences

## Constitution Link

gist.md (`../../../metadata/gists/preferences/gist.md`) is the compressed law list; all universal laws bind this repository. Read one on demand: `./tools/prefs law "<title>"`. Universal laws are inherited, never restated.

Tier: core (R3 Database Driver). Owns: Database interface, native vex store schema. Borrows: Relational Engine reflection/memory/storage, Vexspoke CPU computation/behavior.

## Exclusive Laws

| Law Title | Scope | Enforcement |
| **Entity Model Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Row Borrow and Owned Copy Law** | R3 Database row ownership | Mandatory for `darkbase` |
| **Reactive Program Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Store Versus Executable Separation Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Byte-Native Persistence Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Zero-Alloc Relational Table Scan Law** | R3 Database Driver | Mandatory for `darkbase` |
| **B-Tree Page Consistency Law** | R3 Database Driver | Mandatory for `darkbase` |

### Entity Model Law

* Reflection IS the schema — no ad-hoc schema language, no darkbase-owned descriptor duplicating `Field`.
* `Field` owns the physical layout: persistence/export read offset, size, typeId from the `Field` (or `Struct` row size), never a hand-maintained table; Vexspoke's reflection `Field` is the single source of truth.
* One binding namespace: Structs, Methods and plain values resolve through the same scope and naming grammar.

### Row Borrow and Owned Copy Law

* `Database_insert` stores a borrowed row pointer; caller keeps row and schema alive for the Database lifetime.
* `Database_insertOwned` copies one exact `Struct.size` byte row into engine-owned memory.
* One ownership mode per entity — borrowed and owned never mix; loaded rows use owned mode, released with the Database.
* Reject a copy-in whose byte length is not exactly the registered schema stride.
* Reject mode mixing on one entity; owned insertion copies bytes, never retains the caller's address.
* Free each owned row exactly once with its Database; never free borrowed rows; schemas stay borrowed and outlive the Database.
* Snapshot compatibility loading validates the full input and stages all owned rows before publishing any entity — all-or-nothing.

### Reactive Program Law

* Reactive programs fire on the cold admission path only, never per element on a hot read.
* Programs are data plus behavior — exported as a descriptor; code rebinds by name on load, never by a stored address.
* Views are derived Structs — rows computed by a program against the authoritative store, not a second synchronized copy.

### Store Versus Executable Separation Law

* The store never includes R5 — hosts borrow an opaque `Database*` plus callbacks.
* The executable is a separate application owning no store internals.
* Store = `darkbase-db` (future), executable = `darkbase`.

### Byte-Native Persistence Law

* Values are raw byte spans (`u8[]`), written in memory order and read back the same — no byte-order machinery (no endianness detection, byte swapping or text encoding).
* `toString` is a cold converter for humans, never storage.
* Persisted row bytes carry no raw pointers; references and functions rebind by name/index on load.
* Canonical variable: `{ u8 name[24], void* pointer }` — names 24 bytes, `void*` value a pointer to its bytes.

### Zero-Alloc Relational Table Scan Law

* Table scans, range queries and projections run directly across memory-mapped relational row slices via strided record offsets — zero heap allocation during query execution.
* Query iterators yield pointers to existing record spans.
* Never allocate temporary row wrappers in scan loops.

### B-Tree Page Consistency Law

* B-tree splits, leaf balance and index updates run strictly within fixed-size page blocks from dedicated database memory pools.
* Page blocks are fixed-size power-of-two.
* Page balance updates commit transactionally before updating parent pointers.

## Repo-Local Extensions

* None — no managed exceptions to universal laws.

## Readiness Cross-Reference

* Feature readiness: darkbase readiness gist.
* Open blockers: ecosystem blockers gist.
* Proof lawbook: `tests/test-preferences.md` — `./tools/prefs test "<title>"`.
* Evidence ledger: `metadata/gists/test-checklist.md` — `python3 tools/test_checklist.py check`.
