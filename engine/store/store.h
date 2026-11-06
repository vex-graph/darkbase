#ifndef DARKBASE_STORE_STORE_H
#define DARKBASE_STORE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "c23/constructor.h"
#include "page/page.h"
#include "wal/wal.h"
#include "nio/growable_mapped_file.h"

// One file: immutable bootstrap page, caller-sized embedded redo region, data pages.
// Single caller-owned exclusion domain, stable trusted path, no external mutation
// or truncation for the entire open lifetime. This is not a multi-process lock.
// All operations are cold: file faults/sync have no bounded latency guarantee.
#define STORE_FORMAT_VERSION 1u
#define STORE_BOOTSTRAP_BYTES 64u
#define STORE_REDO_BYTES 32u
#define STORE_NO_PAGE 0u

typedef enum StoreCode {
    STORE_OK, STORE_INVALID, STORE_IO, STORE_CORRUPT, STORE_FULL,
    STORE_CLOSED, STORE_RECOVERY_REQUIRED, STORE_TRUNCATED
} StoreCode;

// Observation only; never reenter Store. Tests may terminate their disposable
// child at these actual commit boundaries. No correctness decision uses a callback.
typedef void (*StoreCommitObserver)(void *context, unsigned phase);
#define STORE_AFTER_LOG_SYNC 1u
#define STORE_AFTER_PAGE_WRITE 2u
#define STORE_AFTER_DATA_SYNC 3u

typedef struct Store {
    ReGrowableMappedFile *mapping; // resident R2 owner, dropped last
    Wal *wal;                     // descriptor into this SAME file's redo region
    Page *page;                   // Page bound to scratch, never mapping-write alias
    uint8_t *scratch;             // one full page, owned engine allocation
    uint8_t *redo;                // STORE_REDO_BYTES + pageSize, owned allocation
    size_t reserved;              // immutable virtual reservation
    size_t journalBytes;          // immutable caller-selected redo capacity
    size_t dataOffset;            // first data-page byte
    uint32_t pageSize;            // immutable power-of-two geometry
    uint32_t pageCount;           // high-water page id, not live page count
    uint32_t freeHead;            // reusable page chain, 0 means empty
    uint64_t sequence;            // last applied full redo record
    uint64_t refusals;            // capacity refusals in this open epoch
    StoreCode code;              // last operation result
    bool open;                   // admission complete
    bool blocked;                // uncertain commit requires close/reopen
    StoreCommitObserver observer; // optional non-reentrant observation
    void *observerContext;       // borrowed context interpreted by observer
} Store;

Store *Store_0(void);
Store *Store_1(const char *path); // reopen; geometry comes from verified bootstrap
Store *Store_4(const char *path, size_t reserved, uint32_t pageSize, size_t journalBytes);
#define Store(...) CONSTRUCTOR_DISPATCH(Store, __VA_ARGS__)
#define Store_zero() Store_0()
void Store_free(Store *self); // no implicit sync or acknowledgement

// Single-page atomic redo unit, not a table/schema/row API. Input is copied and
// sealed OFF the mapping before the journal is synced. Outputs publish on success.
// On pre-log rejection data/state are unchanged. After a possible log write,
// failure blocks admission; reopen resolves it, and a failed call MAY be replayed.
StoreCode Store_add(Store *self, uint32_t kind, const uint8_t *bytes, uint32_t length,
                    uint32_t *outPageId);
StoreCode Store_page_write(Store *self, uint32_t pageId, uint32_t kind,
                           const uint8_t *bytes, uint32_t length);
StoreCode Store_page_release(Store *self, uint32_t pageId);
StoreCode Store_page_read(Store *self, uint32_t pageId, uint8_t *dest, size_t capacity,
                          uint32_t *outLength, uint32_t *outKind, bool *outTruncated);
StoreCode Store_sync(Store *self); // data sync only; never retires embedded redo

bool Store_isOpen(const Store *self);
bool Store_isBlocked(const Store *self);
StoreCode Store_code(const Store *self);
uint32_t Store_pageSize(const Store *self);
uint32_t Store_pageCount(const Store *self);
uint32_t Store_freeHead(const Store *self);
size_t Store_reserved(const Store *self);
size_t Store_journalBytes(const Store *self);
uint64_t Store_sequence(const Store *self);
uint64_t Store_exhaustionCount(const Store *self);
void Store_setCommitObserver(Store *self, StoreCommitObserver observer, void *context);
StoreCommitObserver Store_getCommitObserver(const Store *self);
void *Store_getObserverContext(const Store *self);
const char *Store_codeName(StoreCode code);
bool Store_toString(const Store *self, char *dest, size_t capacity, bool *outTruncated);
bool Store_toStringStruct(const Store *self, char *dest, size_t capacity, bool *outTruncated);

// Geometry, owner pointers, counts and sequence are lifecycle-derived, not raw
// settable: the Conflict Triage Law + Single Class Per File Law (Java Law).
// Embedded redo is retained, NOT recycled; when it fills new writes reject loudly.
// No checkpoint compaction, catalog, index, row addressing, schema or Database
// mutation adapter, hostile-writer sandbox, directory durability, or OS lock.
#endif
