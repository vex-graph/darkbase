# SECURITY.md — darkbase-engine

> **Internal invariant set, not a public disclosure channel.** See the
> [vexgraph security baseline](../../../SECURITY.md) for everything inherited
> here. This file states only this repository's own surface.

**Role:** the R3 `Database` interface and its native vex store — the thing that
opens a `.vexdb` file, and the executable that will sit on top of it. Its whole
attack surface is a **local file** and the **bytes inside it**.

## Surface

- **A local database file** — opened by path, read through the page `Store` over
  a Rust mapping, with an embedded redo region replayed before admission.
- **Row bytes** — raw, native-order spans whose meaning comes entirely from the
  reflected schema the caller registered.

## Path trust

- The caller owns the **serialization/exclusion domain, stable trusted paths, and
  all external alias/truncation exclusion**. There is **no OS lock and no
  hostile-writer sandbox**: a same-user or privileged actor, an ancestor symlink,
  or a concurrent writer is outside the contract, and this is stated rather than
  implied.
- `PreallocatedFile` requires an existing trusted parent and rejects existing
  destinations and final symlinks, but that is a stable-parent contract, **not**
  TOCTOU defence.

## File contents are data, never privilege

- Row bytes are copied in, checked against the registered struct stride, and
  released once. **Nothing in a database file is executed or interpreted as
  code**, and no field value grants a right.
- A **persisted index is re-validated before publication** — key ordering,
  `rowIndex` range, and every key against the bytes of the row it names — and an
  image whose registered schema no longer declares that exact indexed span is
  refused rather than trusted. Addresses are never persisted.
- A `DatabaseCatalogEntry` carries only name, stride and row count; it is
  metadata inspection, never schema reconstruction.

## Limits

- Corrupt or malformed images are refused loudly, but the store is **not** a
  crash journal, a signature/authentication scheme, or a hostile-file sandbox.
- Uncertain IO blocks further admission until close/reopen; a failed call may
  have been committed. Whole-image saves retain old pages and redo records —
  journal recycling and checkpoint compaction are unimplemented.
- The R5 `darkbase` executable is **not implemented**; when it exists, its
  surface is the store's plus whatever it adds, and this file must be updated in
  the same cycle.
