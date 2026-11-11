#include "store/store.h"
#include <stdio.h>
#include <string.h>
#include "annotation/annotation.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "oop/type.h"

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
 *   ReGrowableMappedFile *mapping — resident reservation owner, dropped last
 *   Wal *wal — embedded redo descriptor in the same physical file
 *   Page *page — detached page builder/validator over scratch
 *   u8 *scratch — owned pageSize-byte scratch
 *   u8 *redo — owned STORE_REDO_BYTES + pageSize record
 *   bytes reserved — immutable virtual extent
 *   bytes journalBytes — caller-selected retained journal bound
 *   bytes dataOffset — checked first data-page offset
 *   u32 pageSize — immutable validated geometry
 *   u32 pageCount — allocation high-water id
 *   u32 freeHead — reusable free-page chain
 *   u64 sequence — last applied redo sequence
 *   u64 refusals — observable epoch capacity refusals
 *   StoreCode code — last public result
 *   bool open — fully admitted owner
 *   bool blocked — uncertain commit, close/reopen required
 *   StoreCommitObserver observer — optional non-reentrant observer
 *   void *observerContext — borrowed observer-owned context
 * FUNCTION REGISTRY (exports):
 *   Store *Store_0(void); Store *Store_1(const char *path);
 *   Store *Store_4(const char *path, bytes reserved, u32 pageSize,
 *                  bytes journalBytes); void Store_free(Store *self);
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
 *   void Store_setCommitObserver(Store*, StoreCommitObserver, void*);
 *   StoreCommitObserver Store_getCommitObserver(const Store*);
 *   void *Store_getObserverContext(const Store*); const char *Store_codeName(StoreCode);
 *   bool Store_toString(const Store*, char*, bytes, bool*);
 *   bool Store_toStringStruct(const Store*, char*, bytes, bool*);
 * PRIVATE (static): read32/read64/put32/put64 native byte copies; reject/full/admit
 * admission; geometry/buffers construction; loadPage/pageOffset bounds; apply redo
 * validation/application; replay visitor; build/commit detached sealing and ordered
 * durable publication; observe optional notification; project bounded formatting.
 * No private helper records. OWNERSHIP: no mapping aliases escape; borrowed input
 * is copied before use. Read output and page ids stay untouched on rejection except
 * the explicit truncation flag. IO uncertainty blocks; no implicit sync in free.
 * ========================================================================== */
;;INTENTION("derived geometry/backing/count/sequence change only through lifecycle and durable redo; raw setters would forge ownership per the Conflict Triage Law + Single Class Per File Law (Java Law)")

// INTENTIONAL(vex): persisted integers are copied in native memory order on the
// little-endian host floor; there is no byte swapping or parallel schema format.
static const u8 storeMagic[8] = {'V','E','X','D','B','0','2',0};
#define STORE_REDO_MAGIC 0x31524453u
#define STORE_ALLOCATE 1u
#define STORE_REPLACE 2u
#define STORE_RELEASE 3u

static u32 read32(const u8 *p) { u32 v; memcpy(&v, p, sizeof v); return v; }
static u64 read64(const u8 *p) { u64 v; memcpy(&v, p, sizeof v); return v; }
static void put32(u8 *p, u32 v) { memcpy(p, &v, sizeof v); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, sizeof v); }

static StoreCode reject(Store *self, StoreCode code, const char *why) {
    if (self != nullptr)
        (*self).code = code;
    THROW("Store: %s", why);
    return code;
}

static StoreCode full(Store *self, bytes request, bytes bound) {
    if ((*self).refusals == 0u)
        THROW("Store exhausted: request %zu bytes, bound %zu bytes", request, bound);
    if ((*self).refusals != UINT64_MAX)
        ++(*self).refusals;
    (*self).code = STORE_FULL;
    return STORE_FULL;
}

static bool admit(Store *self) {
    if (self == nullptr || !(*self).open) {
        reject(self, STORE_CLOSED, "owner is closed");
        return false;
    }
    if ((*self).blocked) {
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

Store *Store_0(void) {
    Store *self = (Store*) Memory_alloc(TYPE_DB_STORE_SINGLETON, sizeof(Store));
    if (self == nullptr) {
        THROW("Store: owner allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

void Store_free(Store *self) {
    if (self == nullptr)
        return;
    Wal_free((*self).wal);
    Page_free((*self).page);
    if ((*self).scratch != nullptr)
        Memory_free((*self).scratch);
    if ((*self).redo != nullptr)
        Memory_free((*self).redo);
    re_gmap_drop((*self).mapping);
    Memory_free(self);
}

static bool buffers(Store *self) {
    (*self).scratch = (u8*) Memory_alloc(0u, (*self).pageSize);
    (*self).redo = (u8*) Memory_alloc(0u, STORE_REDO_BYTES + (*self).pageSize);
    if ((*self).scratch == nullptr || (*self).redo == nullptr)
        return false;
    (*self).page = Page_2((*self).scratch, (*self).pageSize);
    return (*self).page != nullptr;
}

static bytes pageOffset(const Store *self, u32 id) {
    return (*self).dataOffset + (bytes) (id - 1u) * (*self).pageSize;
}

static StoreCode loadPage(Store *self, u32 id) {
    if (id == 0u || id > (*self).pageCount)
        return reject(self, STORE_INVALID, "page id outside allocation range");
    bool cut = false;
    if (re_gmap_read((*self).mapping, pageOffset(self, id), (*self).pageSize,
                     (*self).scratch, (*self).pageSize, &cut) != 0u)
        return reject(self, STORE_IO, "mapped page read failed");
    if (Page_load((*self).page) != PAGE_OK) {
        (*self).code = STORE_CORRUPT;
        return STORE_CORRUPT;
    }
    if (Page_id((*self).page) != id)
        return reject(self, STORE_CORRUPT, "page id does not match its physical slot");
    return STORE_OK;
}

static bool apply(Store *self, const WalRecord *record) {
    const u8 *p = (*record).payload;
    if ((*self).sequence == UINT64_MAX || (*record).sequence != (*self).sequence + 1u ||
        (*record).op != WAL_OP_UPDATE || (*record).length != STORE_REDO_BYTES + (*self).pageSize ||
        read32(p) != STORE_REDO_MAGIC || read32(p + 24u) != 0u || read32(p + 28u) != 0u)
        return false;
    u32 op = read32(p + 4u), id = read32(p + 8u);
    u32 count = read32(p + 12u), head = read32(p + 16u);
    if (id == 0u || count == 0u || id > count || head > count || read32(p + 20u) != 0u ||
        count > ((*self).reserved - (*self).dataOffset) / (*self).pageSize)
        return false;
    if (op == STORE_ALLOCATE) {
        if ((*self).freeHead == 0u) {
            if ((*self).pageCount == UINT32_MAX || id != (*self).pageCount + 1u || count != id || head != 0u)
                return false;
        } else {
            if (id != (*self).freeHead || count != (*self).pageCount || head == id ||
                loadPage(self, id) != STORE_OK || Page_kind((*self).page) != PAGE_KIND_FREE ||
                Page_used((*self).page) != sizeof(u32) || read32(Page_payloadOf((*self).page)) != head)
                return false;
        }
    } else if (op == STORE_REPLACE) {
        if (count != (*self).pageCount || head != (*self).freeHead)
            return false;
    } else if (op == STORE_RELEASE) {
        if (count != (*self).pageCount || head != id || id == (*self).freeHead)
            return false;
    } else
        return false;
    if (op != STORE_ALLOCATE && (loadPage(self, id) != STORE_OK || Page_kind((*self).page) == PAGE_KIND_FREE))
        return false;
    memcpy((*self).scratch, p + STORE_REDO_BYTES, (*self).pageSize);
    if (Page_load((*self).page) != PAGE_OK || Page_id((*self).page) != id)
        return false;
    if (op == STORE_RELEASE) {
        if (Page_kind((*self).page) != PAGE_KIND_FREE || Page_used((*self).page) != sizeof(u32) ||
            read32(Page_payloadOf((*self).page)) != (*self).freeHead)
            return false;
    } else if (Page_kind((*self).page) == PAGE_KIND_FREE)
        return false;
    bytes end = (*self).dataOffset + (bytes) count * (*self).pageSize;
    bytes current = 0u;
    if (re_gmap_len((*self).mapping, &current) != 0u ||
        (end > current && re_gmap_grow_to((*self).mapping, end) != 0u) ||
        re_gmap_write((*self).mapping, pageOffset(self, id), (*self).scratch, (*self).pageSize) != 0u)
        return false;
    (*self).pageCount = count;
    (*self).freeHead = head;
    (*self).sequence = (*record).sequence;
    return true;
}

static bool replay(void *context, const WalRecord *record) { return apply((Store*) context, record); }

Store *Store_4(const char *path, bytes reserved, u32 pageSize, bytes journalBytes) {
    if (path == nullptr || path[0] == '\0' || !geometry(reserved, pageSize, journalBytes)) {
        reject(nullptr, STORE_INVALID, "invalid path or reservation/journal geometry");
        return nullptr;
    }
    Store *self = Store_0();
    if (self == nullptr)
        return nullptr;
    (*self).reserved = reserved;
    (*self).journalBytes = journalBytes;
    (*self).pageSize = pageSize;
    (*self).dataOffset = pageSize + journalBytes;
    if (!buffers(self) || re_gmap_create(path, reserved, (*self).dataOffset, &(*self).mapping) != 0u)
        goto failure;
    u8 bootstrap[STORE_BOOTSTRAP_BYTES] = {0};
    memcpy(bootstrap, storeMagic, sizeof storeMagic);
    put32(bootstrap + 8u, STORE_FORMAT_VERSION);
    put32(bootstrap + 12u, pageSize);
    put64(bootstrap + 16u, reserved);
    put64(bootstrap + 24u, journalBytes);
    put64(bootstrap + 32u, (*self).dataOffset);
    if (Page_format((*self).page, 0u, PAGE_KIND_CATALOG) != PAGE_OK ||
        Page_append((*self).page, bootstrap, sizeof bootstrap) != PAGE_OK ||
        Page_seal((*self).page) != PAGE_OK ||
        re_gmap_write((*self).mapping, 0u, (*self).scratch, pageSize) != 0u)
        goto failure;
    (*self).wal = Wal_3(path, pageSize, journalBytes);
    if ((*self).wal == nullptr || !Wal_isOpen((*self).wal) || Wal_code((*self).wal) != WAL_OK ||
        Wal_sync((*self).wal) != WAL_OK || re_gmap_sync((*self).mapping) != 0u)
        goto failure;
    (*self).open = true;
    return self;
failure:
    reject(self, STORE_IO, "creation failed; any new artifact is retained for explicit recovery");
    Store_free(self);
    return nullptr;
}

Store *Store_1(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        reject(nullptr, STORE_INVALID, "empty reopen path");
        return nullptr;
    }
    // Stable trusted path and caller exclusion cover this cold bootstrap read and
    // Rust's subsequent retained-descriptor admission; no hostile-writer TOCTOU claim.
    u8 prefix[PAGE_HEADER_BYTES + STORE_BOOTSTRAP_BYTES];
    File *file = File_open(path, FILE_MODE_READ);
    if (file == nullptr) {
        reject(nullptr, STORE_IO, "bootstrap file could not be opened");
        return nullptr;
    }
    int64_t n = File_read(file, prefix, sizeof prefix);
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
    (*self).reserved = (bytes) reserved;
    (*self).journalBytes = (bytes) journal;
    (*self).pageSize = size;
    (*self).dataOffset = size + (bytes) journal;
    bool cut = false;
    if (!buffers(self) || re_gmap_open(path, (*self).reserved, (*self).dataOffset, &(*self).mapping) != 0u ||
        re_gmap_read((*self).mapping, 0u, size, (*self).scratch, size, &cut) != 0u ||
        Page_load((*self).page) != PAGE_OK || Page_id((*self).page) != 0u ||
        Page_kind((*self).page) != PAGE_KIND_CATALOG || Page_used((*self).page) != STORE_BOOTSTRAP_BYTES)
        goto failure;
    for (bytes i = 40u; i < STORE_BOOTSTRAP_BYTES; ++i)
        if ((*self).scratch[PAGE_HEADER_BYTES + i] != 0u)
            goto failure;
    (*self).wal = Wal_3(path, size, (*self).journalBytes);
    if ((*self).wal == nullptr || !Wal_isOpen((*self).wal) || Wal_baseSequence((*self).wal) != 0u ||
        (Wal_code((*self).wal) != WAL_OK && Wal_code((*self).wal) != WAL_TRUNCATED))
        goto failure;
    WalCode code = Wal_replay((*self).wal, (*self).redo, STORE_REDO_BYTES + size, replay, self);
    if ((code != WAL_OK && code != WAL_TRUNCATED) || (*self).sequence != Wal_sequence((*self).wal) ||
        re_gmap_sync((*self).mapping) != 0u)
        goto failure;
    // Validate the complete free chain with a count-derived budget before admission.
    u32 next = (*self).freeHead;
    u32 visits = 0u;
    while (next != 0u) {
        if (++visits > (*self).pageCount || loadPage(self, next) != STORE_OK ||
            Page_kind((*self).page) != PAGE_KIND_FREE || Page_used((*self).page) != sizeof(u32))
            goto failure;
        next = read32(Page_payloadOf((*self).page));
    }
    (*self).open = true;
    return self;
failure:
    reject(self, STORE_CORRUPT, "reopen/recovery rejected; no owner admitted");
    Store_free(self);
    return nullptr;
}

static void observe(Store *self, unsigned phase) {
    if ((*self).observer != nullptr)
        (*self).observer((*self).observerContext, phase);
}

static StoreCode commit(Store *self, u32 op, u32 id, u32 count, u32 head) {
    u8 *r = (*self).redo;
    memset(r, 0, STORE_REDO_BYTES);
    put32(r, STORE_REDO_MAGIC);
    put32(r + 4u, op);
    put32(r + 8u, id);
    put32(r + 12u, count);
    put32(r + 16u, head);
    memcpy(r + STORE_REDO_BYTES, (*self).scratch, (*self).pageSize);
    u64 sequence = 0u;
    WalCode code = Wal_append((*self).wal, WAL_OP_UPDATE, r, STORE_REDO_BYTES + (*self).pageSize, &sequence);
    if (code == WAL_FULL)
        return full(self, STORE_REDO_BYTES + (*self).pageSize, (*self).journalBytes);
    if (code != WAL_OK || Wal_sync((*self).wal) != WAL_OK) {
        (*self).blocked = true;
        return reject(self, STORE_RECOVERY_REQUIRED, "journal outcome uncertain");
    }
    observe(self, STORE_AFTER_LOG_SYNC);
    WalRecord record = {WAL_OP_UPDATE, sequence, r, STORE_REDO_BYTES + (*self).pageSize};
    if (!apply(self, &record)) {
        (*self).blocked = true;
        return reject(self, STORE_RECOVERY_REQUIRED, "redo application failed");
    }
    observe(self, STORE_AFTER_PAGE_WRITE);
    if (re_gmap_sync((*self).mapping) != 0u) {
        (*self).blocked = true;
        return reject(self, STORE_RECOVERY_REQUIRED, "data sync failed after durable redo");
    }
    observe(self, STORE_AFTER_DATA_SYNC);
    (*self).code = STORE_OK;
    return STORE_OK;
}

static bool build(Store *self, u32 id, u32 kind, const u8 *bytes, u32 length) {
    return Page_format((*self).page, id, kind) == PAGE_OK &&
           Page_append((*self).page, bytes, length) == PAGE_OK && Page_seal((*self).page) == PAGE_OK;
}

StoreCode Store_add(Store *self, u32 kind, const u8 *bytes, u32 length, u32 *outPageId) {
    if (!admit(self))
        return Store_code(self);
    if (outPageId == nullptr || kind == PAGE_KIND_FREE || kind > PAGE_KIND_CATALOG ||
        (bytes == nullptr && length != 0u) || length > (*self).pageSize - PAGE_HEADER_BYTES)
        return reject(self, STORE_INVALID, "invalid page append input");
    u32 id = (*self).freeHead, head = 0u, count = (*self).pageCount;
    if (id != 0u) {
        StoreCode code = loadPage(self, id);
        if (code != STORE_OK)
            return code;
        if (Page_kind((*self).page) != PAGE_KIND_FREE || Page_used((*self).page) != sizeof(u32))
            return reject(self, STORE_CORRUPT, "free-head page is not a free record");
        head = read32(Page_payloadOf((*self).page));
        if (head > count || head == id)
            return reject(self, STORE_CORRUPT, "free-head successor invalid");
    } else {
        if (count >= ((*self).reserved - (*self).dataOffset) / (*self).pageSize || count == UINT32_MAX)
            return full(self, (*self).pageSize, (*self).reserved);
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
        length > (*self).pageSize - PAGE_HEADER_BYTES)
        return reject(self, STORE_INVALID, "invalid replacement page");
    StoreCode code = loadPage(self, id);
    if (code != STORE_OK)
        return code;
    if (Page_kind((*self).page) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "cannot replace a released page");
    if (!build(self, id, kind, bytes, length))
        return STORE_INVALID;
    return commit(self, STORE_REPLACE, id, (*self).pageCount, (*self).freeHead);
}

StoreCode Store_page_release(Store *self, u32 id) {
    if (!admit(self))
        return Store_code(self);
    StoreCode code = loadPage(self, id);
    if (code != STORE_OK)
        return code;
    if (Page_kind((*self).page) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "page already released");
    u32 next = (*self).freeHead;
    if (!build(self, id, PAGE_KIND_FREE, (const u8*) &next, sizeof next))
        return STORE_INVALID;
    return commit(self, STORE_RELEASE, id, (*self).pageCount, id);
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
    if (Page_kind((*self).page) == PAGE_KIND_FREE)
        return reject(self, STORE_INVALID, "page is released");
    u32 length = Page_used((*self).page);
    if (capacity < length) {
        *outTruncated = true;
        return reject(self, STORE_TRUNCATED, "read destination too small; no bytes copied");
    }
    if (length != 0u)
        memcpy(dest, Page_payloadOf((*self).page), length);
    *outLength = length;
    *outKind = Page_kind((*self).page);
    *outTruncated = false;
    (*self).code = STORE_OK;
    return STORE_OK;
}

StoreCode Store_sync(Store *self) {
    if (!admit(self))
        return Store_code(self);
    if (re_gmap_sync((*self).mapping) != 0u) {
        (*self).blocked = true;
        return reject(self, STORE_RECOVERY_REQUIRED, "explicit data sync failed");
    }
    (*self).code = STORE_OK;
    return STORE_OK;
}

bool Store_isOpen(const Store *self) { return self != nullptr && (*self).open; }
bool Store_isBlocked(const Store *self) { return self != nullptr && (*self).blocked; }
StoreCode Store_code(const Store *self) { return self == nullptr ? STORE_CLOSED : (*self).code; }
u32 Store_pageSize(const Store *self) { return self == nullptr ? 0u : (*self).pageSize; }
u32 Store_pageCount(const Store *self) { return self == nullptr ? 0u : (*self).pageCount; }
u32 Store_freeHead(const Store *self) { return self == nullptr ? 0u : (*self).freeHead; }
bytes Store_reserved(const Store *self) { return self == nullptr ? 0u : (*self).reserved; }
bytes Store_journalBytes(const Store *self) { return self == nullptr ? 0u : (*self).journalBytes; }
u64 Store_sequence(const Store *self) { return self == nullptr ? 0u : (*self).sequence; }
u64 Store_exhaustionCount(const Store *self) { return self == nullptr ? 0u : (*self).refusals; }
void Store_setCommitObserver(Store *self, StoreCommitObserver observer, void *context) {
    if (self != nullptr) {
        (*self).observer = observer;
        (*self).observerContext = context;
    }
}
StoreCommitObserver Store_getCommitObserver(const Store *self) { return self == nullptr ? nullptr : (*self).observer; }
void *Store_getObserverContext(const Store *self) { return self == nullptr ? nullptr : (*self).observerContext; }
const char *Store_codeName(StoreCode code) {
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

static bool project(const Store *self, bool structure, char *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int n;
    if (self == nullptr)
        n = snprintf(dest, capacity, "nullptr");
    else if (!structure)
        n = snprintf(dest, capacity, "Store(open=%d,pages=%u,sequence=%llu,blocked=%d)",
            (*self).open, (*self).pageCount, (unsigned long long) (*self).sequence, (*self).blocked);
    else
        n = snprintf(dest, capacity,
            "Store{mapping=%s,wal=%s,page=%s,scratch=%s,redo=%s,reserved=%zu,journalBytes=%zu,"
            "dataOffset=%zu,pageSize=%u,pageCount=%u,freeHead=%u,sequence=%llu,refusals=%llu,"
            "code=%s,open=%d,blocked=%d,observer=%s,observerContext=redacted}",
            (*self).mapping ? "resident" : "nullptr", (*self).wal ? "embedded" : "nullptr",
            (*self).page ? "scratch" : "nullptr", (*self).scratch ? "owned" : "nullptr",
            (*self).redo ? "owned" : "nullptr", (*self).reserved, (*self).journalBytes,
            (*self).dataOffset, (*self).pageSize, (*self).pageCount, (*self).freeHead,
            (unsigned long long) (*self).sequence, (unsigned long long) (*self).refusals,
            Store_codeName((*self).code), (*self).open, (*self).blocked, (*self).observer ? "set" : "nullptr");
    bool cut = n < 0 || (bytes) n >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
bool Store_toString(const Store *self, char *dest, bytes capacity, bool *outTruncated) {
    return project(self, false, dest, capacity, outTruncated);
}
bool Store_toStringStruct(const Store *self, char *dest, bytes capacity, bool *outTruncated) {
    return project(self, true, dest, capacity, outTruncated);
}
