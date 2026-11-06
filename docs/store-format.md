# darkbase store format — v1 specification

**Status: page-level Store and an initial whole-Database page-chain bridge are
implemented; relational indexes, multi-page transactions and journal reclamation
remain unimplemented.** This supersedes the M2 dump format
(`VEXDB01`: 64-byte header, flat rows by `Struct.size`, trailing CRC32), which cannot
represent a message longer than one fixed slot and rewrites the whole store on every
save. Its legacy implementation remains available for compatibility; no migration
from that dump to the page store is offered.

## 1. One file, one database

- **One file per database.** The store is the system of record, not a cache.
- The file is opened inside a **fixed virtual reservation** chosen at create and mapped
  once, so growth never re-maps and pointers survive it (Growable File Mapping Law).
- Segregation — by country, tenant, or whatever the developer wants — means **separate
  files**, each with its own writer. It is a deployment choice, not a format feature.
- **One writer at a time.** Readers are excluded before destructive operations.

## 2. Pages

- **Fixed size, power of two**, chosen at create and recorded in the header (4 KiB
  default).
- Every page carries a **checksum**. Corruption names a *page*, not the whole file.
- Pages are **appended at the end inside the reservation**. Nothing existing moves.
- A **free list** of released pages lets the file plateau instead of ratcheting to a
  permanent high-water mark.

## 3. Header

Magic `VEXDB02`, format version, page size, reservation size, committed length, page
count, root page, free-list head.

## 4. Entities and fields

- An **entity IS a reflection `Struct`**; the **row stride IS `Struct.size`** (Entity
  Model Law). No parallel schema language.
- Every `Field` declares its own facts, independently, in any order and at any time —
  nothing accumulates, nothing depends on call sequence:

  | on the field | meaning |
  | :--- | :--- |
  | name | 24-byte folded atom — the canonical identity (cold lookup) |
  | `offset` | **location**: byte offset within the row |
  | `size` | byte width of the value |
  | `typeId` | the type of the **value** (not of the field) |
  | `flags` | includes `REFLECT_FIELD_KEY` — "part of the identity" |
  | `fieldId` | optional, **declared, stable** alias (compact; hot path) |

- **Identity is a flag, not a call sequence.** There is no `addKey`.
- **Key precedence is field order (offset).** The C struct's own layout decides which
  keyed field leads; reordering the struct changes precedence loudly, never silently.
- **`fieldId` is a contract.** It is declared, unique within the entity, and **stable** —
  never derived from position, or reordering the struct would silently rewrite the ids
  that indexes and saved queries refer to.
- **Fields are reachable two ways**: by **name** (canonical; resolve once on a cold path
  and hold the `Field*` — Cold-Only Reflection Law) and by **`fieldId`** (compact, for
  index entries and hot paths). Name alone is enough to be correct; the id is optional.

## 5. Rows

- Fixed stride. A row is addressed logically as **`(pageId, slot)`**, always
  **re-resolved** through the tree — never a cached byte offset.
- Append-only in v1; page reuse comes from the free list.
- A returned row pointer is valid only within one open session, for a pinned/unmoved
  page.

## 6. Variable-length text

- **Chunk size: 4096 bytes** (bytes, not characters — the Byte-Native Persistence Law
  has no encoding, and one UTF-8 character is 1–4 bytes).
- The **first chunk lives inline in the owning row** — the common case costs nothing
  extra.
- Overflow chunks are **separate rows keyed `(messageId, chunkIndex)`** — *not* a linked
  list, so reading a long message is one contiguous read rather than pointer chasing.

## 7. The v1 entities (the chat shape)

```
message        key (channelId, messageId)   author, at, chunkCount, totalBytes, head chunk inline
message_chunk  key (messageId, chunkIndex)  one <=4096-byte slice
```

- Message ids are **made by the application**, not the store: time-sortable (timestamp
  high bits + counter) so messages order themselves.
- History read = *"start at this channel's newest and walk back N"* — one bounded read,
  no sort, no scan.

## 8. Durability

- **Write-ahead log**: append the intent, fsync, apply, checkpoint. Replay on open.
- **Per-page checksums** in addition to the log.
- `flush` requests writeback; `sync` requests durability. Neither is implicit.

## 9. The addressing invariant

A row's address is a **logical name re-resolved through the tree**, read from a
checksummed page, in a file opened **by descriptor (not path)**, via 64-bit
bounds-checked positional access, mutated only through the log. A cached offset is a
cache of that resolution — nothing more.

## 10. Explicitly not in v1

- No SQL. The native query contract is structured.
- No multi-statement transactions beyond single-writer commit.
- No in-place shrink or compaction (`VACUUM` later).
- No wire protocol yet — see the setup/endpoint distinction, which promotes a service
  surface from optional to required.
- No migration from the M2 dump format.

## Implemented page-store slice (not the complete relational format)

`store/store.h` exposes create/open, page append/replace/read/release and data sync.
It is deliberately separate from `Database_save`/`Database_load`; schemas, rows,
root-tree/index lookup and the chat entities above are not connected to it yet.

One physical file has three regions:

| Region | Meaning |
| :--- | :--- |
| `[0, pageSize)` | Immutable sealed Page, id 0, CATALOG kind, 64-byte bootstrap payload |
| `[pageSize, pageSize + journalBytes)` | Caller-sized embedded Wal; never a sidecar |
| Remaining reservation | Data page id `n` resolves to `dataOffset + (n-1)*pageSize` |

Bootstrap payload: magic `VEXDB02\0` at 0, u32 slice version at 8, u32 page size
at 12, u64 virtual reservation at 16, u64 journal bytes at 24, u64 data offset at
32, then 24 zero reserved bytes. The containing Page seal covers it. The full
mutable header originally specified above is not independently overwritten:
allocation high-water count, free-list head and applied sequence are reconstructed
from retained redo. The generic Store has no catalog tree or root; the Database
bridge below reserves logical data-page id 1 as its own snapshot publication root.
Native integer memory order is intentional on the little-endian host floor.

Each redo payload is 32 bytes plus a complete sealed page image: u32 redo magic
`0x31524453`, operation (1 allocate, 2 replace, 3 release), page id, resulting
page count, resulting free-list head, zero root/reserved words, then page bytes.
Only base sequence zero is supported; there is no checkpoint baseline image.
Embedded Wal v3 publishes a fixed leading commit marker only after syncing its
record body. Store syncs that marker before touching mapped data, applies the
page and syncs data before reporting success. Recovery validates and replays the
committed prefix, reconstructs allocation state, validates the free chain with a
count-derived budget, syncs repaired data, then admits callers. A zero commit
marker tail is not a committed intent; a committed malformed frame rejects.

Release writes a FREE page with one u32 successor id. Allocation pops this chain
before increasing the high-water id. Existing page ids never move. This is page
reuse, not a row-generation/stale-row-handle API or compaction.

**Current limit:** embedded redo is retained, never checkpointed/recycled. Both
journal capacity and data reservation are explicit creation-time safety bounds;
refusal preserves existing data and is counted, with one exhaustion diagnostic
per owner epoch. Releasing data pages does not reclaim redo capacity. `Store_sync`
does not checkpoint. A safe journal rollover/checkpoint format remains future
work rather than silently calling standalone Wal's destructive truncation API.

## Database page-chain bridge (initial whole-image slice)

`Database_saveStore` and `Database_loadStore` connect the reflected entity and
owned-row API to the page-level Store using a dedicated Store catalog encoding,
not the legacy M2 snapshot parser. Page id 1 is a `PAGE_KIND_CATALOG` root record.
It points to linked catalog pages whose records carry reflected entity names,
row strides and counts, followed by each entity's raw row spans. Callers register
the same reflection `Struct`s before loading; the Store loader validates the
complete catalog and schemas, then stages owned rows before publishing them.

`Database_storeCatalogEntry(store, index)` exposes one copied catalog descriptor
after validating the selected root, page chain, checksum and every record span.
Its `DatabaseCatalogEntry` value contains the entity name, row stride and row
count and remains usable after Store close. An out-of-range index returns
`DATABASE_ABSENT` loudly. This is metadata inspection, not schema reconstruction:
the caller still supplies matching reflected `Struct`s before loading rows.

Each entity's descriptor is followed by its raw row span and then, when the
entity declares an index, by one `{int32_t key; uint32_t rowIndex}` record per
row. Both live in the same catalog payload, so the single root-page publication
publishes rows and index together and they cannot tear apart. The loader
re-validates each record before publishing anything — key ordering, `rowIndex`
range, and every key against the bytes of the row it names — and refuses the whole
image if the registered schema no longer declares that exact indexed span. No
address is ever persisted; the loaded records are re-pointed at the loaded rows.
This is a whole-image bridge, so the index is rewritten on every save; it is not
a page-level incremental index. Catalog version 2 carries this section; version 1
images are refused rather than reinterpreted.

Save writes and syncs every new chunk through Store before replacing the root in
one page commit. A rejected write before root publication leaves the prior root
selected after close/reopen, although already committed unreferenced chunks can
consume data pages. Old successful snapshots are not reclaimed by this bridge;
each save rewrites the complete image, so both data reservation and retained WAL
can exhaust. This is a first Database/Store integration seam, not incremental row
storage, a multi-page transaction API, a B-tree, schema evolution, or a replacement
for the legacy `Database_save`/`Database_load` compatibility calls. A failed root
commit has Store's existing uncertain-outcome rule and requires close/reopen.

Caller exclusion covers the complete mapping/descriptor lifetime, including the
cold bootstrap path read before Rust mapping admission. No alias, OS-lock or
hostile-parent confinement is claimed. After uncertain append/sync/apply failure,
the Store blocks and requires close/reopen: failure may still have committed.
Initial bootstrap creation is not crash-atomic or parent-directory durable; an
interrupted create can leave a rejected artifact for explicit recovery.

Proof entry: `python3 tests/darkbase-engine/store/run.py` builds real Rust storage
offline/locked and fresh native C dependencies, then runs Page, Wal and Store
owners optimized and C ASan/UBSan. Store covers 48 simultaneously live full pages,
two resource boundaries, reuse/reopen, three real child-exit commit boundaries,
torn-data repair and five injected IO-stage failures. This is not actual power
loss, concurrent writer, full database workflow, or Rust sanitizer proof.
