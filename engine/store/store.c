#include "vex.h"
#include "store/store.h"
#include <stdio.h>
#include <string.h>
#include "io/file/file.h"
#include "nio/mem.h"
#include "oop/darkbase_type.h"
#include "nio/darkbase_memory.h"

;;DEFINITION
/* Store is the first page-level durable owner, not a replacement spelling for the
 * legacy Database snapshot. Rust retains one fixed virtual reservation; Page seals
 * detached bytes; Wal writes full-page redo into a fixed region of that SAME file.
 * Only after Wal_sync succeeds may the mapped data change. Each record contains the
 * page image and allocation state, so reopening can rebuild the high-water count and
 * free list while repairing a torn data write. The immutable bootstrap is checked
 * before journal admission. Initial creation is not an atomic directory transaction:
 * an interrupted bootstrap leaves a refused artifact for explicit cleanup.
 *
 * Every mutation is one page, including release/reuse. No mutable pointer escapes.
 * The caller serializes all users and excludes external writers/truncation. After
 * uncertain IO the owner blocks further admission: a failed call is not proof that
 * its record was absent; reopening resolves the durable prefix. Free pages are reused
 * without growing the mapping; embedded journal space is NOT reclaimed in this slice.
 * Caller-selected journal/reservation boundaries reject and count instead of silently
 * losing work. This is not catalog, table transaction or power-loss hardware proof. */
;;OVERVIEW
/* ============================================================================
 * CLASS: Store (store/store.c)
 * STRUCT FIELDS (Mirroring store/store.h), declaration order:
 *   ReGrowableMappedFile *mapping, resident reservation owner, dropped last
 *   Wal *wal, embedded redo descriptor in the same physical file
 *   Page *page, detached page builder/validator over scratch
 *   u8 *scratch, owned pageSize-byte scratch
 *   u8 *redo, owned STORE_REDO_BYTES + pageSize record
 *   bytes reserved, immutable virtual extent
 *   bytes journalBytes, caller-selected retained journal bound
 *   bytes dataOffset, checked first data-page offset
 *   u32 pageSize, immutable validated geometry
 *   u32 pageCount, allocation high-water id
 *   u32 freeHead, reusable free-page chain
 *   u64 sequence, last applied redo sequence
 *   u64 refusals, observable epoch capacity refusals
 *   StoreCode code, last public result
 *   bool open, fully admitted owner
 *   bool blocked, uncertain commit, close/reopen required
 *   StoreCommitObserver observer, optional non-reentrant observer
 *   any observerContext, borrowed observer-owned context
 * FUNCTION REGISTRY (exports):
 *   Store *Store_0(); Store *Store_1(const utf8 *path);
 *   Store *Store_4(const utf8 *path, bytes reserved, u32 pageSize,
 *                  bytes journalBytes); fn Store_free(Store *self);
 *   StoreCode Store_add(Store*, u32, const u8*, u32, u32*);
 *   StoreCode Store_page_write(Store*, u32, u32, const u8*, u32);
 *   StoreCode Store_page_release(Store*, u32);
 *   StoreCode Store_page_read(Store*, u32, u8*, bytes, u32*,
 *                             u32*, bool*); StoreCode Store_sync(Store*);
 *   bool Store_isOpen(const Store*); bool Store_isBlocked(const Store*);
 *   StoreCode Store_code(const Store*); u32 Store_pageSize(const Store*);
 *   u32 Store_pageCount(const Store*); u32 Store_freeHead(const Store*);
 *   bytes Store_reserved(const Store*); bytes Store_journalBytes(const Store*);
 *   u64 Store_sequence(const Store*); u64 Store_exhaustionCount(const Store*);
 *   fn Store_setCommitObserver(Store*, StoreCommitObserver, any);
 *   StoreCommitObserver Store_getCommitObserver(const Store*);
 *   any Store_getObserverContext(const Store*); const utf8 *Store_codeName(StoreCode);
 *   bool Store_toString(const Store*, utf8*, bytes, bool*);
 *   bool Store_toStringStruct(const Store*, utf8*, bytes, bool*);
 * PRIVATE (static): read32/read64/put32/put64 native byte copies; reject/full/admit
 * admission; geometry/buffers construction; loadPage/pageOffset bounds; apply redo
 * validation/application; replay visitor; build/commit detached sealing and ordered
 * durable publication; observe optional notification; project bounded formatting.
 * No private helper records. OWNERSHIP: no mapping aliases escape; borrowed input
 * is copied before use. Read output and page ids stay untouched on rejection except
 * the explicit truncation flag. IO uncertainty blocks; no implicit sync in free.
 * ========================================================================== */
;;INTENTION("derived geometry/backing/count/sequence change only through lifecycle and durable redo; raw setters would forge ownership per the Conflict Triage Law + Single Class Per File Law (Java Law)")

/**
 * INTENTIONAL(vex): persisted integers are copied in native memory order on the
 * little-endian host floor; there is no byte swapping or parallel schema format.
 */
static const u8 storeMagic[8] = {'V','E','X','D','B','0','2',0};
#define STORE_REDO_MAGIC 0x31524453u
#define STORE_ALLOCATE 1u
#define STORE_REPLACE 2u
#define STORE_RELEASE 3u

static u32 read32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, sizeof v);
    return v;
}
static u64 read64(const u8 *p)
{
    u64 v;
    memcpy(&v, p, sizeof v);
    return v;
}
static fn put32(u8 *p, u32 v)
{
    memcpy(p, &v, sizeof v);
}
static fn put64(u8 *p, u64 v)
{
    memcpy(p, &v, sizeof v);
}

;;GETTER
static inline u32 WalRecord_op(const WalRecord *r)
{
    return (*r).op;
}
;;GETTER
static inline u64 WalRecord_sequence(const WalRecord *r)
{
    return (*r).sequence;
}
;;GETTER
static inline u32 WalRecord_length(const WalRecord *r)
{
    return (*r).length;
}
;;GETTER
static inline const u8 *WalRecord_payload(const WalRecord *r)
{
    return (*r).payload;
}
;;GETTER
static inline ReGrowableMappedFile *Store_mapping(const Store *self)
{
    return (*self).mapping;
}
;;GETTER
static inline ReGrowableMappedFile **Store_mappingMut(Store *self)
{
    return &(*self).mapping;
}
;;GETTER
static inline Wal *Store_wal(const Store *self)
{
    return (*self).wal;
}
;;SETTER
static inline fn Store_setWal(Store *self, Wal *wal)
{
    (*self).wal = wal;
}
;;GETTER
static inline Page *Store_page(const Store *self)
{
    return (*self).page;
}
;;SETTER
static inline fn Store_setPage(Store *self, Page *page)
{
    (*self).page = page;
}
;;GETTER
static inline u8 *Store_scratch(const Store *self)
{
    return (*self).scratch;
}
;;SETTER
static inline fn Store_setScratch(Store *self, u8 *scratch)
{
    (*self).scratch = scratch;
}
;;GETTER
static inline u8 *Store_redo(const Store *self)
{
    return (*self).redo;
}
;;SETTER
static inline fn Store_setRedo(Store *self, u8 *redo)
{
    (*self).redo = redo;
}
;;SETTER
static inline fn Store_setReserved(Store *self, bytes reserved)
{
    (*self).reserved = reserved;
}
;;SETTER
static inline fn Store_setJournalBytes(Store *self, bytes journalBytes)
{
    (*self).journalBytes = journalBytes;
}
;;GETTER
static inline bytes Store_dataOffset(const Store *self)
{
    return (*self).dataOffset;
}
;;SETTER
static inline fn Store_setDataOffset(Store *self, bytes dataOffset)
{
    (*self).dataOffset = dataOffset;
}
;;SETTER
static inline fn Store_setPageSize(Store *self, u32 pageSize)
{
    (*self).pageSize = pageSize;
}
;;SETTER
static inline fn Store_setPageCount(Store *self, u32 pageCount)
{
    (*self).pageCount = pageCount;
}
;;SETTER
static inline fn Store_setFreeHead(Store *self, u32 freeHead)
{
    (*self).freeHead = freeHead;
}
;;SETTER
static inline fn Store_setSequence(Store *self, u64 sequence)
{
    (*self).sequence = sequence;
}
;;GETTER
static inline u64 Store_refusals(const Store *self)
{
    return (*self).refusals;
}
;;SETTER
static inline fn Store_setRefusals(Store *self, u64 refusals)
{
    (*self).refusals = refusals;
}
;;SETTER
static inline fn Store_setCode(Store *self, StoreCode code)
{
    (*self).code = code;
}
;;GETTER
static inline bool Store_open(const Store *self)
{
    return (*self).open;
}
;;SETTER
static inline fn Store_setOpen(Store *self, bool open)
{
    (*self).open = open;
}
;;GETTER
static inline bool Store_blocked(const Store *self)
{
    return (*self).blocked;
}
;;SETTER
static inline fn Store_setBlocked(Store *self, bool blocked)
{
    (*self).blocked = blocked;
}
;;GETTER
static inline StoreCommitObserver Store_observer(const Store *self)
{
    return (*self).observer;
}
;;SETTER
static inline fn Store_setObserver(Store *self, StoreCommitObserver obs)
{
    (*self).observer = obs;
}
;;GETTER
static inline any Store_observerContext(const Store *self)
{
    return (*self).observerContext;
}
;;SETTER
static inline fn Store_setObserverContext(Store *self, any ctx)
{
    (*self).observerContext = ctx;
}

static StoreCode reject(Store *self, StoreCode code, const utf8 *why) {
    if (self != nullptr)
        Store_setCode(self, code);
    THROW("Store: %s", why);
    return code;
}

static StoreCode full(Store *self, bytes request, bytes bound) {
    u64 ref = Store_refusals(self);
    if (ref == 0u)
        THROW("Store exhausted: request %zu bytes, bound %zu bytes", request, bound);
    if (ref != UINT64_MAX)
        Store_setRefusals(self, ref + 1u);
    Store_setCode(self, STORE_FULL);
    return STORE_FULL;
}

static bool admit(Store *self) {
    if (self == nullptr || !Store_open(self)) {
        reject(self, STORE_CLOSED, "owner is closed");
        return false;
    }
    if (Store_blocked(self)) {
        reject(self, STORE_RECOVERY_REQUIRED, "uncertain commit; close and reopen");
        return false;
    }
    return true;
}

static bool geometry(bytes reserved, u32 size, bytes journal) {
    return Page_sizeValid(size) && reserved <= INT64_MAX && reserved % size == 0u &&
           journal % size == 0u && journal >= (bytes) size * 2u &&
           journal <= reserved && reserved - journal >= (bytes) size * 2u &&
           (reserved - journal - size) / size <= UINT32_MAX;
}

Store *Store_0() {
    Store *self = (Store*) Memory_alloc(TYPE_DB_STORE_SINGLETON, Darkbase_arena(), sizeof(Store));
    if (self == nullptr) {
        THROW("Store: owner allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

fn Store_free(Store *self) {
    if (self == nullptr)
        return;
    Wal_free(Store_wal(self));
    Page_free(Store_page(self));
    u8 *scratch = Store_scratch(self);
    if (scratch != nullptr)
        Memory_free(scratch);
    u8 *redo = Store_redo(self);
    if (redo != nullptr)
        Memory_free(redo);
    re_gmap_drop(Store_mapping(self));
    Memory_free(self);
}

;;CONSTRUCTOR
static bool buffers(Store *self) {
    u32 pageSize = Store_pageSize(self);
    u8 *scratch = (u8*) Memory_alloc(0u, Darkbase_arena(), pageSize);
    u8 *redo = (u8*) Memory_alloc(0u, Darkbase_arena(), STORE_REDO_BYTES + pageSize);
    Store_setScratch(self, scratch);
    Store_setRedo(self, redo);
    if (scratch == nullptr || redo == nullptr)
        return false;
    Page *page = Page_2(scratch, pageSize);
    Store_setPage(self, page);
    return page != nullptr;
}

static bytes pageOffset(const Store *self, u32 id) {
    return Store_dataOffset(self) + (bytes) (id - 1u) * Store_pageSize(self);
}

static StoreCode loadPage(Store *self, u32 id) {
    if (id == 0u || id > Store_pageCount(self))
        return reject(self, STORE_INVALID, "page id outside allocation range");
    bool cut = false;
    u32 pageSize = Store_pageSize(self);
    if (re_gmap_read(Store_mapping(self), pageOffset(self, id), pageSize,
                     Store_scratch(self), pageSize, &cut) != 0u)
        return reject(self, STORE_IO, "mapped page read failed");
    Page *page = Store_page(self);
    if (Page_load(page) != PAGE_OK) {
        Store_setCode(self, STORE_CORRUPT);
        return STORE_CORRUPT;
    }
    if (Page_id(page) != id)
        return reject(self, STORE_CORRUPT, "page id does not match its physical slot");
    return STORE_OK;
}

static bool apply(Store *self, const WalRecord *record) {
    const u8 *p = WalRecord_payload(record);
    u64 seq = Store_sequence(self);
    u64 recSeq = WalRecord_sequence(record);
    u32 pageSize = Store_pageSize(self);
    if (seq == UINT64_MAX || recSeq != seq + 1u ||
        WalRecord_op(record) != WAL_OP_UPDATE || WalRecord_length(record) != STORE_REDO_BYTES + pageSize ||
        read32(p) != STORE_REDO_MAGIC || read32(p + 24u) != 0u || read32(p + 28u) != 0u)
        return false;
    u32 op = read32(p + 4u), id = read32(p + 8u);
    u32 count = read32(p + 12u), head = read32(p + 16u);
    bytes dataOffset = Store_dataOffset(self);
    if (id == 0u || count == 0u || id > count || head > count || read32(p + 20u) != 0u ||
        count > (Store_reserved(self) - dataOffset) / pageSize)
        return false;
    u32 freeHead = Store_freeHead(self);
    u32 pageCount = Store_pageCount(self);
    Page *page = Store_page(self);
    if (op == STORE_ALLOCATE) {
        if (freeHead == 0u) {
            if (pageCount == UINT32_MAX || id != pageCount + 1u || count != id || head != 0u)
                return false;
        } else {
            if (id != freeHead || count != pageCount || head == id ||
                loadPage(self, id) != STORE_OK || Page_kind(page) != PAGE_KIND_FREE ||
                Page_used(page) != sizeof(u32) || read32(Page_payloadOf(page)) != head)
                return false;
        }
    } else if (op == STORE_REPLACE) {
        if (count != pageCount || head != freeHead)
            return false;
    } else if (op == STORE_RELEASE) {
        if (count != pageCount || head != id || id == freeHead)
            return false;
    } else
        return false;
    if (op != STORE_ALLOCATE && (loadPage(self, id) != STORE_OK || Page_kind(page) == PAGE_KIND_FREE))
        return false;
    u8 *scratch = Store_scratch(self);
    memcpy(scratch, p + STORE_REDO_BYTES, pageSize);
    if (Page_load(page) != PAGE_OK || Page_id(page) != id)
        return false;
    if (op == STORE_RELEASE) {
        if (Page_kind(page) != PAGE_KIND_FREE || Page_used(page) != sizeof(u32) ||
            read32(Page_payloadOf(page)) != freeHead)
            return false;
    } else if (Page_kind(page) == PAGE_KIND_FREE)
        return false;
    bytes end = dataOffset + (bytes) count * pageSize;
    bytes current = 0u;
    ReGrowableMappedFile *mapping = Store_mapping(self);
    if (re_gmap_len(mapping, &current) != 0u ||
        (end > current && re_gmap_grow_to(mapping, end) != 0u) ||
        re_gmap_write(mapping, pageOffset(self, id), scratch, pageSize) != 0u)
        return false;
    Store_setPageCount(self, count);
    Store_setFreeHead(self, head);
    Store_setSequence(self, recSeq);
    return true;
}

static bool replay(any context, const WalRecord *record)
{
    return apply((Store*) context, record);
}

Store *Store_4(const utf8 *path, bytes reserved, u32 pageSize, bytes journalBytes) {
    if (path == nullptr || path[0] == '\0' || !geometry(reserved, pageSize, journalBytes)) {
        reject(nullptr, STORE_INVALID, "invalid path or reservation/journal geometry");
        return nullptr;
    }
    Store *self = Store_0();
    if (self == nullptr)
        return nullptr;
    bytes dataOffset = pageSize + journalBytes;
    Store_setReserved(self, reserved);
    Store_setJournalBytes(self, journalBytes);
    Store_setPageSize(self, pageSize);
    Store_setDataOffset(self, dataOffset);
    if (!buffers(self) || re_gmap_create(path, reserved, dataOffset, Store_mappingMut(self)) != 0u)
        goto failure;
    u8 bootstrap[STORE_BOOTSTRAP_BYTES] = {0};
    memcpy(bootstrap, storeMagic, sizeof storeMagic);
    put32(bootstrap + 8u, STORE_FORMAT_VERSION);
    put32(bootstrap + 12u, pageSize);
    put64(bootstrap + 16u, reserved);
    put64(bootstrap + 24u, journalBytes);
    put64(bootstrap + 32u, dataOffset);
    Page *page = Store_page(self);
    ReGrowableMappedFile *mapping = Store_mapping(self);
    if (Page_format(page, 0u, PAGE_KIND_CATALOG) != PAGE_OK ||
        Page_append(page, bootstrap, sizeof bootstrap) != PAGE_OK ||
        Page_seal(page) != PAGE_OK ||
        re_gmap_write(mapping, 0u, Store_scratch(self), pageSize) != 0u)
        goto failure;
    Wal *wal = Wal_3(path, pageSize, journalBytes);
    Store_setWal(self, wal);
    if (wal == nullptr || !Wal_isOpen(wal) || Wal_code(wal) != WAL_OK ||
        Wal_sync(wal) != WAL_OK || re_gmap_sync(mapping) != 0u)
        goto failure;
    Store_setOpen(self, true);
    return self;
failure:
    reject(self, STORE_IO, "creation failed; any new artifact is retained for explicit recovery");
    Store_free(self);
    return nullptr;
}

Store *Store_1(const utf8 *path) {
    if (path == nullptr || path[0] == '\0') {
        reject(nullptr, STORE_INVALID, "empty reopen path");
        return nullptr;
    }
    u8 prefix[PAGE_HEADER_BYTES + STORE_BOOTSTRAP_BYTES];
    File *file = File_open(path, FILE_MODE_READ);
    if (file == nullptr) {
        reject(nullptr, STORE_IO, "bootstrap file could not be opened");
        return nullptr;
    }
    i64 n = File_read(file, prefix, sizeof prefix);
    bool closed = File_close(file);
    if (!closed || n != sizeof prefix || memcmp(prefix + PAGE_HEADER_BYTES, storeMagic, sizeof storeMagic) != 0) {
        reject(nullptr, STORE_CORRUPT, "bootstrap absent or malformed");
        return nullptr;
    }
    const u8 *b = prefix + PAGE_HEADER_BYTES;
    u32 size = read32(b + 12u);
    u64 reserved = read64(b + 16u), journal = read64(b + 24u);
    if (reserved > SIZE_MAX || journal > SIZE_MAX || read32(b + 8u) != STORE_FORMAT_VERSION ||
        !geometry((bytes) reserved, size, (bytes) journal) || read64(b + 32u) != size + journal) {
        reject(nullptr, STORE_CORRUPT, "bootstrap geometry/version rejected");
        return nullptr;
    }
    Store *self = Store_0();
    if (self == nullptr)
        return nullptr;
    bytes dataOffset = size + (bytes) journal;
    Store_setReserved(self, (bytes) reserved);
    Store_setJournalBytes(self, (bytes) journal);
    Store_setPageSize(self, size);
    Store_setDataOffset(self, dataOffset);
    bool cut = false;
    if (!buffers(self) || re_gmap_open(path, (bytes) reserved, dataOffset, Store_mappingMut(self)) != 0u ||
        re_gmap_read(Store_mapping(self), 0u, size, Store_scratch(self), size, &cut) != 0u ||
        Page_load(Store_page(self)) != PAGE_OK || Page_id(Store_page(self)) != 0u ||
        Page_kind(Store_page(self)) != PAGE_KIND_CATALOG || Page_used(Store_page(self)) != STORE_BOOTSTRAP_BYTES)
        goto failure;
    u8 *scratch = Store_scratch(self);
    for (bytes i = 40u; i < STORE_BOOTSTRAP_BYTES; ++i)
        if (scratch[PAGE_HEADER_BYTES + i] != 0u)
            goto failure;
    Wal *wal = Wal_3(path, size, (bytes) journal);
    Store_setWal(self, wal);
    if (wal == nullptr || !Wal_isOpen(wal) || Wal_baseSequence(wal) != 0u ||
        (Wal_code(wal) != WAL_OK && Wal_code(wal) != WAL_TRUNCATED))
        goto failure;
    WalCode code = Wal_replay(wal, Store_redo(self), STORE_REDO_BYTES + size, replay, self);
    if ((code != WAL_OK && code != WAL_TRUNCATED) || Store_sequence(self) != Wal_sequence(wal) ||
        re_gmap_sync(Store_mapping(self)) != 0u)
        goto failure;
    u32 next = Store_freeHead(self);
    u32 pageCount = Store_pageCount(self);
    Page *page = Store_page(self);
    u32 visits = 0u;
    while (next != 0u) {
        if (++visits > pageCount || loadPage(self, next) != STORE_OK ||
            Page_kind(page) != PAGE_KIND_FREE || Page_used(page) != sizeof(u32))
            goto failure;
        next = read32(Page_payloadOf(page));
    }
    Store_setOpen(self, true);
    return self;
failure:
    reject(self, STORE_CORRUPT, "reopen/recovery rejected; no owner admitted");
    Store_free(self);
    return nullptr;
}

static fn observe(Store *self, u32 phase) {
    StoreCommitObserver obs = Store_observer(self);
    if (obs != nullptr)
        obs(Store_observerContext(self), phase);
}

static StoreCode commit(Store *self, u32 op, u32 id, u32 count, u32 head) {
    u8 *r = Store_redo(self);
    u32 pageSize = Store_pageSize(self);
    Wal *wal = Store_wal(self);
    memset(r, 0, STORE_REDO_BYTES);
    put32(r, STORE_REDO_MAGIC);
    put32(r + 4u, op);
    put32(r + 8u, id);
    put32(r + 12u, count);
    put32(r + 16u, head);
    memcpy(r + STORE_REDO_BYTES, Store_scratch(self), pageSize);
    u64 sequence = 0u;
    WalCode code = Wal_append(wal, WAL_OP_UPDATE, r, STORE_REDO_BYTES + pageSize, &sequence);
    if (code == WAL_FULL)
        return full(self, STORE_REDO_BYTES + pageSize, Store_journalBytes(self));
    if (code != WAL_OK || Wal_sync(wal) != WAL_OK) {
        Store_setBlocked(self, true);
        return reject(self, STORE_RECOVERY_REQUIRED, "journal outcome uncertain");
    }
    observe(self, STORE_AFTER_LOG_SYNC);
    WalRecord record = {WAL_OP_UPDATE, sequence, r, STORE_REDO_BYTES + pageSize};
    if (!apply(self, &record)) {
        Store_setBlocked(self, true);
        return reject(self, STORE_RECOVERY_REQUIRED, "redo application failed");
    }
    observe(self, STORE_AFTER_PAGE_WRITE);
    if (re_gmap_sync(Store_mapping(self)) != 0u) {
        Store_setBlocked(self, true);
        return reject(self, STORE_RECOVERY_REQUIRED, "data sync failed after durable redo");
    }
    observe(self, STORE_AFTER_DATA_SYNC);
    Store_setCode(self, STORE_OK);
    return STORE_OK;
}

static bool build(Store *self, u32 id, u32 kind, const u8 *bytes, u32 length) {
    Page *page = Store_page(self);
    return Page_format(page, id, kind) == PAGE_OK &&
           Page_append(page, bytes, length) == PAGE_OK && Page_seal(page) == PAGE_OK;
}

StoreCode Store_add(Store *self, u32 kind, const u8 *bytes, u32 length, u32 *outPageId) {
    if (!admit(self))
        return Store_code(self);
    u32 pageSize = Store_pageSize(self);
    if (outPageId == nullptr || kind == PAGE_KIND_FREE || kind > PAGE_KIND_CATALOG ||
        (bytes == nullptr && length != 0u) || length > pageSize - PAGE_HEADER_BYTES)
        return reject(self, STORE_INVALID, "invalid page append input");
    u32 id = Store_freeHead(self), head = 0u, count = Store_pageCount(self);
    Page *page = Store_page(self);
    if (id != 0u) {
        StoreCode code = loadPage(self, id);
        if (code != STORE_OK)
            return code;
        if (Page_kind(page) != PAGE_KIND_FREE || Page_used(page) != sizeof(u32))
            return reject(self, STORE_CORRUPT, "free-head page is not a free record");
        head = read32(Page_payloadOf(page));
        if (head > count || head == id)
            return reject(self, STORE_CORRUPT, "free-head successor invalid");
    } else {
        if (count >= (Store_reserved(self) - Store_dataOffset(self)) / pageSize || count == UINT32_MAX)
            return full(self, pageSize, Store_reserved(self));
        id = count + 1u;
        count = id;
    }
    if (!build(self, id, kind, bytes, length))
        return STORE_INVALID;
    StoreCode code = commit(self, STORE_ALLOCATE, id, count, head);
    if (code == STORE_OK)
        *outPageId = id;
    return code;
}

StoreCode Store_page_write(Store *self, u32 id, u32 kind, const u8 *bytes, u32 length) {
    if (!admit(self))
        return Store_code(self);
    if (kind == PAGE_KIND_FREE || kind > PAGE_KIND_CATALOG || (bytes == nullptr && length != 0u) ||
        length > Store_pageSize(self) - PAGE_HEADER_BYTES)
        return reject(self, STORE_INVALID, "invalid replacement page");
    StoreCode code = loadPage(self, id);
    if (code != STORE_OK)
        return code;
    if (Page_kind(Store_page(self)) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "cannot replace a released page");
    if (!build(self, id, kind, bytes, length))
        return STORE_INVALID;
    return commit(self, STORE_REPLACE, id, Store_pageCount(self), Store_freeHead(self));
}

StoreCode Store_page_release(Store *self, u32 id) {
    if (!admit(self))
        return Store_code(self);
    StoreCode code = loadPage(self, id);
    if (code != STORE_OK)
        return code;
    if (Page_kind(Store_page(self)) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "page already released");
    u32 next = Store_freeHead(self);
    if (!build(self, id, PAGE_KIND_FREE, (const u8*) &next, sizeof next))
        return STORE_INVALID;
    return commit(self, STORE_RELEASE, id, Store_pageCount(self), id);
}

StoreCode Store_page_read(Store *self, u32 id, u8 *dest, bytes capacity,
                          u32 *outLength, u32 *outKind, bool *outTruncated) {
    if (!admit(self))
        return Store_code(self);
    if (dest == nullptr || outLength == nullptr || outKind == nullptr || outTruncated == nullptr)
        return reject(self, STORE_INVALID, "missing read destination/output");
    StoreCode code = loadPage(self, id);
    if (code != STORE_OK)
        return code;
    Page *page = Store_page(self);
    if (Page_kind(page) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "page is released");
    u32 length = Page_used(page);
    if (capacity < length) {
        *outTruncated = true;
        return reject(self, STORE_TRUNCATED, "read destination too small; no bytes copied");
    }
    if (length != 0u)
        memcpy(dest, Page_payloadOf(page), length);
    *outLength = length;
    *outKind = Page_kind(page);
    *outTruncated = false;
    Store_setCode(self, STORE_OK);
    return STORE_OK;
}

StoreCode Store_sync(Store *self) {
    if (!admit(self))
        return Store_code(self);
    if (re_gmap_sync(Store_mapping(self)) != 0u) {
        Store_setBlocked(self, true);
        return reject(self, STORE_RECOVERY_REQUIRED, "explicit data sync failed");
    }
    Store_setCode(self, STORE_OK);
    return STORE_OK;
}

bool Store_isOpen(const Store *self)
{
    return self != nullptr && Store_open(self);
}
bool Store_isBlocked(const Store *self)
{
    return self != nullptr && Store_blocked(self);
}
StoreCode Store_code(const Store *self)
{
    return self == nullptr ? STORE_CLOSED : (*self).code;
}
u32 Store_pageSize(const Store *self)
{
    return self == nullptr ? 0u : (*self).pageSize;
}
u32 Store_pageCount(const Store *self)
{
    return self == nullptr ? 0u : (*self).pageCount;
}
u32 Store_freeHead(const Store *self)
{
    return self == nullptr ? 0u : (*self).freeHead;
}
bytes Store_reserved(const Store *self)
{
    return self == nullptr ? 0u : (*self).reserved;
}
bytes Store_journalBytes(const Store *self)
{
    return self == nullptr ? 0u : (*self).journalBytes;
}
u64 Store_sequence(const Store *self)
{
    return self == nullptr ? 0u : (*self).sequence;
}
u64 Store_exhaustionCount(const Store *self)
{
    return self == nullptr ? 0u : Store_refusals(self);
}
fn Store_setCommitObserver(Store *self, StoreCommitObserver observer, any context) {
    if (self != nullptr) {
        Store_setObserver(self, observer);
        Store_setObserverContext(self, context);
    }
}
StoreCommitObserver Store_getCommitObserver(const Store *self)
{
    return self == nullptr ? nullptr : Store_observer(self);
}
any Store_getObserverContext(const Store *self)
{
    return self == nullptr ? nullptr : Store_observerContext(self);
}
const utf8 *Store_codeName(StoreCode code) {
    switch (code) {
        case STORE_OK: return "ok";
        case STORE_INVALID: return "invalid";
        case STORE_IO: return "io";
        case STORE_CORRUPT: return "corrupt";
        case STORE_FULL: return "full";
        case STORE_CLOSED: return "closed";
        case STORE_RECOVERY_REQUIRED: return "recovery-required";
        case STORE_TRUNCATED: return "truncated";
        default: return "unknown";
    }
}

static bool project(const Store *self, bool structure, utf8 *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    i32 n;
    if (self == nullptr)
        n = snprintf(dest, capacity, "nullptr");
    else if (!structure)
        n = snprintf(dest, capacity, "Store(open=%d,pages=%u,sequence=%llu,blocked=%d)",
            Store_open(self), Store_pageCount(self), (u64) Store_sequence(self), Store_blocked(self));
    else
        n = snprintf(dest, capacity,
            "Store{mapping=%s,wal=%s,page=%s,scratch=%s,redo=%s,reserved=%zu,journalBytes=%zu,"
            "dataOffset=%zu,pageSize=%u,pageCount=%u,freeHead=%u,sequence=%llu,refusals=%llu,"
            "code=%s,open=%d,blocked=%d,observer=%s,observerContext=redacted}",
            Store_mapping(self) ? "resident" : "nullptr", Store_wal(self) ? "embedded" : "nullptr",
            Store_page(self) ? "scratch" : "nullptr", Store_scratch(self) ? "owned" : "nullptr",
            Store_redo(self) ? "owned" : "nullptr", Store_reserved(self), Store_journalBytes(self),
            Store_dataOffset(self), Store_pageSize(self), Store_pageCount(self), Store_freeHead(self),
            (u64) Store_sequence(self), (u64) Store_refusals(self),
            Store_codeName(Store_code(self)), Store_open(self), Store_blocked(self),
            Store_observer(self) ? "set" : "nullptr");
    bool cut = n < 0 || (bytes) n >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
bool Store_toString(const Store *self, utf8 *dest, bytes capacity, bool *outTruncated) {
    return project(self, false, dest, capacity, outTruncated);
}
bool Store_toStringStruct(const Store *self, utf8 *dest, bytes capacity, bool *outTruncated) {
    return project(self, true, dest, capacity, outTruncated);
}
