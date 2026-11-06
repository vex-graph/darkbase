# darkbase — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal constitution: `gist.md` is the compressed law list; `PREFERENCES.md` is its rationale.

## 0. Constitution Link (supreme)
- [gist.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a#file-gist-md) — the compressed law list, mirrored in this workspace at `../../../metadata/gists/preferences/gist.md`. It is the only normative law text; `../../../metadata/gists/preferences/PREFERENCES.md` carries the rationale and is never ingested whole. Numbers are slice addresses inside one file, never law identity: cite by Title.
- All universal laws bind this repository. Read one on demand from the workspace root: `./tools/prefs law "<title>"`, `./tools/prefs section "<section>"`, `./tools/prefs repo <name>`.
- This document codifies **exclusive** preferences for `darkbase` (R3 Database Driver). Database semantics and persistence remain R3; allocation/storage and native span search belong to Relational Engine R2, with Vexspoke supplying CPU computation/behavior. Either R2 public contract may be borrowed. This blueprint has no implemented engine integration; migration and durability require owner proof.
- darkbase is a **native vex store, not a SQL engine**. The schema is C reflection; the native query contract is structured, and any text/SQL parser is an optional `cli/` convenience or a foreign driver's private translation.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws inherit from the compressed constitution (`../../../metadata/gists/preferences/gist.md`); the rows below are repo-specific additions.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **Entity Model Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Reactive Program Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Store Versus Executable Separation Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Byte-Native Persistence Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Zero-Alloc Relational Table Scan Law** | R3 Database Driver | Mandatory for `darkbase` |
| **B-Tree Page Consistency Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Row Borrow and Owned Copy Law** | R3 Database row ownership | Mandatory for `darkbase` |
| **Durable Page Store Boundary Law** | Single-file page-level Store | Embedded redo ordering, reopen, refusal and reuse owner proof |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### Entity Model Law
**Definition.** darkbase defines no schema types; it consumes R2 reflection: `struct` = `Struct` (a table: ordered `Field` list, `size` = entity row stride), `class` = `Class` (`Struct` plus constructor and `Method`s), `field` = `Field`, `function` = `Method`; `Field` carries behavior (name + read/set + target) and physical layout (typeId, offset, size, flags); plain values are named typed bindings in the same scope — one shared C-types-as-schema vocabulary serving reflection, persistence and export without drift. **Rule.** 1. **Reflection IS the schema** — no ad-hoc schema language, no darkbase-owned descriptor duplicating `Field`. 2. **Field owns the physical layout** — persistence/export read offset, size, typeId from the `Field` (or `Struct` row size), never a hand-maintained table; Vexspoke's reflection `Field` is the single source of truth. 3. **One binding namespace** — Structs, Methods and plain values resolve through the same scope and naming grammar.

### Row Borrow and Owned Copy Law
**Definition.** `Database_insert` stores a borrowed row pointer; the caller keeps row and schema alive for the Database lifetime. `Database_insertOwned` copies one exact `Struct.size` byte row into engine-owned memory. One ownership mode per entity — borrowed and owned never mix; loaded rows use owned mode, released with the Database — the zero-copy path with a clear release owner. **Rule.** 1. Reject a copy-in whose byte length is not exactly the registered schema stride; count and ownership mode unchanged. 2. Reject mode mixing on one entity; owned insertion copies bytes, never retains the caller's address. 3. Free each owned row exactly once with its Database; never free borrowed rows; schemas stay borrowed, outlive the Database. 4. Snapshot compatibility loading validates the full input and stages all owned rows before publishing any entity — all-or-nothing, limited to that legacy loader; the M2 flat dump is not the specified persistence design (pages plus WAL).

### Reactive Program Law
**Definition.** The reactive layer is the SQL analog of triggers, views and stored procedures — first-class named programs bound to store events (`beforeInsert`, `afterInsert`, `beforeUpdate`, `afterDelete`, `onOpen`, `onCommit`, `onRollback`); persistence hooks ship with the data and export with it. **Rule.** 1. **Programs fire on the cold admission path only**, never per element on a hot read (Cold-Strict, Hot-Minimal Validation Law; Cold-Only Reflection Law). 2. **Programs are data plus behavior** — exported as a descriptor; code rebinds by name on load, never by a stored address. 3. **Views are derived Structs** — rows computed by a program against the authoritative store, not a second synchronized copy.

### Store Versus Executable Separation Law
**Definition.** The R3 repository is the `Database` interface and native vex store (future name `darkbase-db`); the R5 executable `darkbase` is an interactive host that opens a `.vexdb`. Conflating them couples the store to a UI and blocks other consumers. **Rule.** 1. **The store never includes R5** — hosts borrow an opaque `Database*` plus callbacks (Vertical Integration Law). 2. **The executable is a separate application** owning no store internals. 3. **Naming is deliberate:** store = `darkbase-db` (future), executable = `darkbase`.

### Byte-Native Persistence Law
**Definition.** Values are raw byte spans (`uint8_t[]`), written in memory order and read back the same with **no byte-order machinery** — no endianness detection, byte swapping or text encoding; `toString` is a cold converter for humans, never storage. All supported hosts are little-endian (Apple Silicon arm64 floor; Windows x86_64), so memory order beats a swap layer. **Rule.** 1. **Bytes, not text, for storage** — the schema (`Field.typeId` + offset/size) interprets durable spans; no string encoding in a file. 2. **No endianness code** — no swap helpers or byte-order fields; a `// INTENTIONAL(vex)` note states this at the format definition. 3. **`toString` is a cold converter** — debugging only (the toString Law). 4. **Pointer-free rows** — persisted row bytes carry no raw pointers; references and functions rebind by name/index on load. 5. **Canonical variable** — `{ uint8_t name[24], void* pointer }` (the engine's `VariableSlot` shape): names 24 bytes, the `void*` value a pointer to its bytes.

### Zero-Alloc Relational Table Scan Law
**Definition.** Table scans, range queries and projections run directly across memory-mapped relational row slices via strided record offsets — zero heap allocation during query execution; strided memory-mapped scanning reaches near-memory-bus throughput. **Rule.** 1. **Strided Projections** — query iterators yield pointers to existing record spans. 2. **Zero Row Allocs** — never allocate temporary row wrappers in scan loops.

### B-Tree Page Consistency Law
**Definition.** B-tree splits, leaf balance and index updates run strictly within fixed-size page blocks from dedicated database memory pools — predictable index geometry bounds traversal depth and prevents fragmentation. **Rule.** 1. **Page Bounded** — fixed-size power-of-two blocks. 2. **Atomic Splits** — page balance updates commit transactionally before updating parent pointers.

## 3. Repo-Local Extensions (managed, per the Conflict Triage Law)

### Durable Page Store Boundary Law

`store/store.{h,c}` owns a page-level durable owner over R2's real Rust growable mapping, Page and Wal. One physical file: immutable bootstrap page, caller-sized embedded redo region, data pages — no sidecar or C mapping clone. Full detached sealed page images enter redo; the commit marker syncs before mapped data changes, and data sync precedes success. Reopen admits only after replay and free-chain validation; no checkpoint baseline other than zero. Uncertain IO blocks admission until close/reopen; a failed call may be recovered as committed. Separate primitive: not a durable `Database`/Headless integration, schema/catalog or ordered row index. Release/reuse on logical page ids; no stale row-handle generation promise. Embedded history retained: journal recycling/checkpoint compaction unimplemented; writes refuse/count when its explicit capacity or the data reservation is exhausted; `Store_sync` never truncates or retires this history.

**Bridge** (`Database_saveStore`/`Database_loadStore`): dedicated reflected catalog encoding + raw owned-row spans split into page chains, published through one catalog root-page replacement — not the legacy M2 snapshot parser. Loading requires the same schemas registered first and stages all owned rows before publication. Whole-image persistence, not incremental row addressing or a multi-page transaction. Old catalog/data pages and redo records are retained; capacity refusal before root publication preserves the previously selected image; uncertain root commit requires close/reopen. No checkpointing, schema migration, structured indexes, or thread-safe Store operations.

- `Database_queryEqual` — first structured query: resolve one reflected Field cold, validate its nonzero span against the entity row stride, compare exact bytes across in-memory rows. Cursor owns only its temporary row-pointer list; matching row values remain borrowed, cursor freed before Database. Not numeric comparison, projection, ordering, persistent catalog lookup, or an index; results remain caller-serialized/owner-affine.
- `Database_queryInt32Range` — interpret one exact-width field span as native `int32_t`, rows within inclusive signed bounds; no type coercion — use only where the field's row bytes carry that representation. Reversed bounds and invalid spans reject loudly, Database state unchanged. Uses the declared int32 index when one covers the field, row-list scan otherwise — same matches either way.
- `Database_indexBuild` — in-memory int32 index for one field; admits only a Field carrying `REFLECT_FIELD_INDEXED` whose span is exactly `int32_t` (the schema's flag is the single declaration point). Records: a key-sorted array of borrowed row pointers; rows are append-only, so an insert only marks the records stale and the next query rebuilds them — admission stays allocation-free, free of partial state. While an index exists, the indexed field of a bound row must not be mutated in place; the Database does not detect that.
- `Database_saveStore` persists each declared index as `{int32_t key; uint32_t rowIndex}` records after the entity's row span, inside the same catalog payload and so the same single root-page publication: rows and index cannot tear apart. `Database_loadStore` re-validates every record before publishing — key ordering, `rowIndex` range, each key against the bytes of the row it names — and refuses the whole image when the registered schema no longer declares that exact indexed span. Addresses are never persisted; records re-point at the loaded rows. An entity whose image carries no index keeps its local declaration and rebuilds on the next query. Still a whole-image rewrite, not incremental index pages: no page-level index, no ordered scan of other field types, no multi-field index.
- `Database_fieldSpan` resolves one field NAME to its in-row byte span (`{code, offset, size}`), letting a consumer without a C struct — headless service, tool, schema-driven reader — read a field without walking the reflection tree and folding names itself. Name folded (case irrelevant); null, unknown or ambiguous names reject explicitly, and a declared span that would leave the row is refused rather than handed to a reader. Read-only: allocates nothing, disturbs no index. Named-field resolution, not projection: no field list, no computed values, no row shaping. catalog index after validating the root, page chain, checksum and record spans. The value contains name, row stride and row count, borrowing no Store memory. Discovery only; loading data still requires the caller to register matching reflected schemas.
- **Caller contract:** one serialization/exclusion domain, stable trusted paths and all external alias/truncation exclusion are caller-owned; no OS lock or hostile-writer sandbox inferred. Geometry, owned pointers, page count, free head, sequence and failure state mutate only through construction, admission and durable operations; validated getters, not lifetime-forging setters, expose them — a scoped managed accessor exception under the Conflict Triage Law and Single Class Per File Law (Java Law). A non-reentrant observation callback may observe commit boundaries, but production correctness never depends on it. Initial creation is not crash-atomic and may retain a rejected artifact for recovery.
- **Proof:** `../../../tests/darkbase-engine/store/run.py` (workspace root command) — mirrored Store/Page/Wal owners, fresh native C dependencies, real Rust archive, optimized assertions, C ASan/UBSan. Power loss, Rust sanitizer instrumentation, allocator failure, full OS-error and other-platform/runtime-floor proof remain explicit gaps. The global graph's post-migration ownership-ratchet failure is separate and not silently bypassed.

;;INTENTION("R3 Database Driver: native vex entity store over the shared reflection vocabulary (Struct=entity, Field=entity field with physical layout, Method=function) with reactive store-event programs; zero-alloc table scans; strict page consistency; structured native query, not a SQL engine.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [darkbase](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-darkbase-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
# darkbase-headless — Repo-Local Living Preferences

## Constitution Link

The complete [constitution](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a)
governs this repository. The binding law list is `gist.md`
(`../../../metadata/gists/preferences/gist.md`); `../../../metadata/gists/preferences/PREFERENCES.md` is its full
rationale and is never ingested whole. Read the law first, then this file, the proof
lawbook `../../../tests/test-preferences.md` (slice one law with
`./tools/prefs test "<title>"`), the evidence ledger
`../../../metadata/gists/test-checklist.md`, and the actual implementation.

## Law Index (Binding Matrix)

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| Headless Service Boundary Law | R4 service scope and dependencies | Owner/build checks; no store internals, no UI |
| Client Never Opens The File Law | Ownership of the database file | Service is the only opener; clients hold connections |
| Session Token Law | Identity, tokens and credential handling | Token lifetime/revocation/redaction owner tests |
| Injected Transport Law | Socket ownership and bounded progress | fn-table seam, host-driven bounded polls, no threads |
| Request and Send Law | Wire shape, versioning and failure channel | Framing/status/version-mismatch owner tests |
| Admin Out-of-Band Law | Setup and schema modification | Setup absent from consumer code; admin ops owned here |
| Entity-Scoped Permission Law | Authorization model | Grants live on entities; role resolution and deny-by-default owner tests |
| Step-Up For Destructive Operations Law | Admin/destructive authorization | Password re-entry per destructive request; no cached credentials |
| Sensitive Field Law | Field-level protection | Sensitivity declared on the field; service-side enforcement; explicit withholding |
| Elevation Law (Time-Boxed Step-Up) | Presence proof and its window | Per-session elevation, bounded window, rate-limited, recovery codes |
| Authenticator Enrolment Law (TOTP) | Second factor and its primitives | RFC 6238 interop, gated enrolment, recoverable secret, replay rejection |

### Headless Service Boundary Law

R4 service for darkbase: owns **sessions, identity, the request/send surface and
the transport seam**; the only component that opens a database file. Borrows R2
contracts and the R3 store; never included by R3, never implements storage,
paging or the log — `darkbase-engine` (R3) owns those. No user interface; an R5
application consumes it. One-way dependencies: the store never learns what a
session is, the service never learns how a page is laid out.

### Client Never Opens The File Law

**The service is the only thing that opens the database.** A client holds a
**connection descriptor** — endpoint, credential, database id — never a path, even
on the same machine: the service's lifetime must stay independent of any client's. A
UI reading a `.vеxdb` directly is a defect: it makes the client's lifetime own the
store's and reduces this repository to decoration.

### Session Token Law

- **Authenticate once; verify a session per request:** the session token is checked
  per request, never a password — per-request hashing is a denial-of-service vector
  placing credentials on every message.
- **Two tokens, two lifetimes:** a short-lived **access token** in RAM dies with
  the process; a durable, long-lived (e.g. 90 days), revocable **refresh token**
  means a machine restart does not log everyone out.
- **The RAM token table is the hot set:** sessions and residency in one structure —
  pin on login, release on expiry or disconnect.
- **A token is an opaque value, never a pointer** (pointers are live-process
  addresses, not portable identifiers — the Stable Row and Variable Binding Law).
- **Never store a password:** store a salted, deliberately slow hash from the R2 CPU
  crypto contract (never invented) — and only the refresh token's hash.
- **Redaction.** Tokens and credential material never appear in logs, diagnostics or
  `toString` projections; comparisons of secret material do not early-exit.

### Entity-Scoped Permission Law

**Definition:** Authorization is **entity-scoped**: a **role is only a label** (no
rights), a **grant** — a `(roleId, entity, rights)` row with rights a bitmask (at
minimum READ, WRITE, DELETE) — is the only place a permission lives; a request
is authorized by resolving user → roles → grants for the target entity against the
right the operation needs. (Rationale: contradicting role and overwrite grants make
every audit a resolver simulation.)

**The Rule:**
1. Rights attach to entities, never to roles; adding a right to a role is a defect.
2. Deny by default: unknown user, role, entity or right rejects with an explicit
   status, never a silent allow.
3. One grant per `(role, entity)`; re-registering replaces, never accumulates.
4. Every catalog object (database, entity, user, role) carries a declared, stable
   id distinct from its name — renaming never breaks a grant or audit trail.
5. Field sensitivity is not a right: a flag on the field (the Sensitive Field Law)
   requiring elevation; grants stay on entities — never a second source of truth.
6. Authorization is per request, from the session's user, never cached across a
   grant change.

### Step-Up For Destructive Operations Law

**Definition:** Destructive or shape-changing operations require the **password
again** in that request; a session token is never sufficient alone. (Rationale: the
token proves *who you are*; the password proves *you are still the one holding
it*.)

**The Rule:**
1. Password in the request is required at minimum for: dropping an entity, deleting
   rows, changing grants, user creation/removal, token rotation/revocation.
2. Verify, then discard: checked against the stored slow hash, dropped immediately —
   never into the session, log, projection or diagnostic.
3. A failed step-up refuses with an explicit status and is auditable; repeated
   failures are rate-limited; a burst is a signal.
4. Reads and ordinary writes never require it — the cost is paid where the damage
   is.
5. The transport decision is load-bearing: a step-up password crosses the wire, so
   plaintext transport is *worse* than no step-up; step-up requires TLS or an
   explicit, documented trusted-network-only claim.
6. Admin overhead is by design — the cost of a breach being survivable.

### Sensitive Field Law

**Definition:** A field may be declared **sensitive**; reading or writing it
requires an **elevated** session (the Elevation Law). (Rationale: the schema is the
single answer to "is this protected?" — a client-side hide is a display choice, not
a control.)

**The Rule:**
1. Sensitivity is a flag on the `Field`, like `offset`, `size` and `KEY` — never a
   per-request option or client-side filter.
2. The service enforces it: non-elevated sensitive bytes never leave the process.
3. A direct read without elevation refuses with a *distinct* status, so a UI can
   ask for elevation rather than a generic denial.
4. A whole-row projection never silently drops it: it refuses, or includes the
   field **explicitly marked as withheld** — a reader must never mistake "withheld"
   for "empty" (silent redaction is the same defect as silent exhaustion).
5. Sensitivity gates writing too, by the same rule.
6. No per-row sensitivity in v1; a future version extends this law — the
   declaration stays on the field, not a second table.

### Elevation Law (Time-Boxed Step-Up)

**Definition:** **Elevation** is a per-session, time-boxed state proving a human is
present *now*: password plus authenticator code, a bounded window (**15 minutes**
default, configurable). (Rationale: the bounded window is the same trade `sudo`
makes; presence is what a stolen token cannot fake.)

**The Rule:**
1. Per-session, never global; elevating one session must not elevate another.
2. Password + authenticator, verified once, then discarded (Step-Up For Destructive
   Operations Law); the code is checked against a window, never stored.
3. Time-boxed, default 15 minutes, expiring silently to ordinary access; the code
   rotates on its own schedule — the window is the elevation's, not the code's.
4. Required for destructive operations (drop, delete, grants, users, tokens) and
   sensitive-field read or write.
5. Not required for ordinary reads and writes.
6. Failed attempts are rate-limited and auditable; a burst is a signal.
7. Recovery must exist: one-time backup codes issued at enrolment — "I lost my
   phone" must not mean "I lost my database".
8. The authenticator secret is recoverable by the service (unlike a password
   hash), so the store is part of the trust boundary — stated wherever described.

### Authenticator Enrolment Law (TOTP)

**Definition:** The second factor is **TOTP** (RFC 6238 over RFC 4226): a shared
per-user secret plus a time-derived 6-digit code. **No vendor integration**;
enrolment shows the setup URI once (as QR and as base32 text) and verification
recomputes the code from the stored secret. (Rationale: the interop surface is the
RFC and the setup URI, stable because it is owned by no one.)

**The Rule:**
1. Interop is the RFC, not a vendor: HMAC-SHA1, 6 digits, 30-second step, the
   `otpauth://totp/<issuer>:<account>?secret=<base32>&issuer=<issuer>` setup URI —
   no per-app code path, ever.
2. Enrolment is itself protected: enrolling or replacing an authenticator requires
   the password (Step-Up For Destructive Operations Law) — a stolen session that
   could silently enrol an attacker's device would permanently *own* step-up for
   the account.
3. The secret is stored so the service can read it (unlike a password hash): the
   store is part of the trust boundary — stored **encrypted**, key outside the
   database file, stated wherever described.
4. Recovery codes are issued at enrolment, shown exactly once, stored only as
   **hashes**, each **single-use**.
5. Verification accepts a bounded window (default ±1 step, for clock skew) and
   **rejects replay**: the last accepted timestep per user is remembered; a code
   at or before it is refused for its validity — a 30-second code with
   tolerance is otherwise reusable for up to ~90 seconds.
6. Failed verifications are rate-limited and auditable, like every elevation
   failure.
7. Primitives are borrowed, never invented: HMAC and the constant-time compare
   come from the R2 CPU crypto contract; base32 and the QR *encode* are pure
   format algorithms (Pure Format Algorithm Law); drawing the QR
   belongs to whichever surface shows it (terminal cells, or an image).
8. No connector is required: an SMS or e-mail fallback *would* be an R3 connector
   shape, weaker than TOTP — never the default path.

### Injected Transport Law

The transport is an **opaque handle plus a `{connect, poll, send, close}`
fn-table**, driven by the host's bounded pump — no owned sockets, no dedicated
threads, no unbounded waits (the Bounded Wait Law). In-process/embedded use only
skips the network; every client shares one request path. R3 may not include this
repository; R5 consumes it. A transport that blocks is a defect: a persistent push
connection must never become a polling loop.

### Request and Send Law

Requests and responses are **length-prefixed**, carry an explicit **protocol
version**, and answer with an **explicit status code** as the observable failure
channel; a version mismatch is refused loudly, never silently reinterpreted. **One
request at a time per connection** — multiple connections instead of multiplexing,
until ordering guarantees are designed.

First envelope: `protocol/headless_protocol.{h,c}`, normative layout in
`docs/wire-protocol.md`; V1: four-byte length prefix, then
version/kind/operation/status/request-id and opaque payload, explicitly
little-endian. Frames are bounded at 65536 bytes (hostile input safety), host
backing selected cold; a connection owner admits one frame and retains coalesced
tail bytes with the caller. Malformed/oversize/truncated streams fail closed until
an explicit new-stream reset; never scan for a new header. An admitted envelope is
not authentication or authority to execute; statuses are vocabulary with no
dispatcher or admin action connected yet. The host sends a full response before
release and owns deadlines, transport security and connection authorization; no
real socket or TLS proof is implied.

Frame backing, counts and ready/failure state mutate only through
constructor/feed/release/reset — a scoped managed accessor exception under the
Conflict Triage Law and Single Class Per File Law (Java Law), preserving
initialized-byte and borrow truth. Safe getters and redacted projections exist;
direct field mutation is outside contract; capacity refusals report once/count
always until reset.

### Admin Out-of-Band Law

Setup and schema modification — create database, create entity, add user, browse
the catalog — are operations of **this service**, reachable over a connection and
by a CLI needing no GUI, never **embedded in a consuming project's code**: an
application contains no schema setup, migration script or file path, and uses
what is already there.

## Current Proof Boundary

`src/service/service.{h,c}` is an initial offline service owner: one engine
Database; private admin row schema registered before loading; request admission
closed at startup; one-time bootstrap, password + TOTP login, TOTP replay-step
persistence, forced password replacement and explicit request-gate enable/disable.
The TOTP secret crosses a borrowed secret-store callback only; that callback must
be backed by secure OS storage, no plaintext fallback. Registered owner
`../../../tests/darkbase-headless/service/service_test.c` passed on macOS arm64
through the workspace graph; claims are scoped to that test, and save/load failure
injection and Keychain behavior are unproved.

It uses the engine's flat snapshot API for admin/replay/gate state; unimplemented:
WAL-backed mutation/audit direction, network transport, per-client
access/refresh sessions, CRUD, triggers and redacted audit — a process-local
authenticated boolean is not a client session or network authorization. Gaps:
Keychain integration, startup/save failure injection, diagnostic assertions,
sanitizers, non-macOS platforms; no push, live provider call, socket acquisition or
real credential use is implied.

The transport-independent protocol owner adds byte-level framing proof separately
(strict optimized and ASan/UBSan: hostile headers, splits, EOF, bounds/exhaustion,
preservation, recovery); transport and real network request execution remain
unimplemented.
