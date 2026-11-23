#ifndef DARKBASE_STORE_STORE_H
#define DARKBASE_STORE_STORE_H

#include "vex.h"
#include "page/page.h"
#include "wal/wal.h"
#include "relational_engine/growable_mapped_file.h"

/**
 * One file: immutable bootstrap page, caller-sized embedded redo region, data pages.
 * Single caller-owned exclusion domain, stable trusted path, no external mutation
 * or truncation for the entire open lifetime. This is not a multi-process lock.
 * All operations are cold: file faults/sync have no bounded latency guarantee.
 */
#define STORE_FORMAT_VERSION 1u
#define STORE_BOOTSTRAP_BYTES 64u
#define STORE_REDO_BYTES 32u
#define STORE_NO_PAGE 0u

typedef enum StoreCode {
    STORE_OK, STORE_INVALID, STORE_IO, STORE_CORRUPT, STORE_FULL,
    STORE_CLOSED, STORE_RECOVERY_REQUIRED, STORE_TRUNCATED
} StoreCode;

/**
 * Observation only; never reenter Store. Tests may terminate their disposable
 * child at these actual commit boundaries. No correctness decision uses a callback.
 */
typedef fn (*StoreCommitObserver)(any context, u32 phase);
#define STORE_AFTER_LOG_SYNC 1u
#define STORE_AFTER_PAGE_WRITE 2u
#define STORE_AFTER_DATA_SYNC 3u

typedef struct Store {
    ReGrowableMappedFile *mapping;
    Wal *wal;
    Page *page;
    u8 *scratch;
    u8 *redo;
    bytes reserved;
    bytes journalBytes;
    bytes dataOffset;
    u32 pageSize;
    u32 pageCount;
    u32 freeHead;
    u64 sequence;
    u64 refusals;
    StoreCode code;
    bool open;
    bool blocked;
    StoreCommitObserver observer;
    any observerContext;
} Store;

Store *Store_0();
Store *Store_1(string path); /** reopen; geometry comes from verified bootstrap */
Store *Store_4(string path, bytes reserved, u32 pageSize, bytes journalBytes);
#define Store(...) CONSTRUCTOR_DISPATCH(Store, __VA_ARGS__)
#define Store_zero() Store_0()
fn Store_free(Store *self); /** no implicit sync or acknowledgement */

/**
 * Single-page atomic redo unit, not a table/schema/row API. Input is copied and
 * sealed OFF the mapping before the journal is synced. Outputs publish on success.
 * On pre-log rejection data/state are unchanged. After a possible log write,
 * failure blocks admission; reopen resolves it, and a failed call MAY be replayed.
 */
StoreCode Store_add(Store *self, u32 kind, const u8 *bytes, u32 length,
                    u32 *outPageId);
StoreCode Store_page_write(Store *self, u32 pageId, u32 kind,
                           const u8 *bytes, u32 length);
StoreCode Store_page_release(Store *self, u32 pageId);
StoreCode Store_page_read(Store *self, u32 pageId, u8 *dest, bytes capacity,
                          u32 *outLength, u32 *outKind, bool *outTruncated);
StoreCode Store_sync(Store *self); /** data sync only; never retires embedded redo */

bool Store_isOpen(const Store *self);
bool Store_isBlocked(const Store *self);
StoreCode Store_code(const Store *self);
u32 Store_pageSize(const Store *self);
u32 Store_pageCount(const Store *self);
u32 Store_freeHead(const Store *self);
bytes Store_reserved(const Store *self);
bytes Store_journalBytes(const Store *self);
u64 Store_sequence(const Store *self);
u64 Store_exhaustionCount(const Store *self);
fn Store_setCommitObserver(Store *self, StoreCommitObserver observer, any context);
StoreCommitObserver Store_getCommitObserver(const Store *self);
any Store_getObserverContext(const Store *self);
string Store_codeName(StoreCode code);
bool Store_toString(const Store *self, utf8 *dest, bytes capacity, bool *outTruncated);
bool Store_toStringStruct(const Store *self, utf8 *dest, bytes capacity, bool *outTruncated);

/**
 * Geometry, owner pointers, counts and sequence are lifecycle-derived, not raw
 * settable: the Conflict Triage Law + Single Class Per File Law (Java Law).
 * Embedded redo is retained, NOT recycled; when it fills new writes reject loudly.
 * No checkpoint compaction, catalog, index, row addressing, schema or Database
 * mutation adapter, hostile-writer sandbox, directory durability, or OS lock.
 */
#endif
