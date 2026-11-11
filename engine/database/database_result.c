// database/database_result.c — the DatabaseResult row cursor.

#include "database/database_result.h"

#include <stdio.h>

#include "annotation/annotation.h"
#include "oop/type.h"
#include "exception/throw.h"
#include "nio/mem.h"
#include "oop/type.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: DatabaseResult
 * ============================================================================
 * DatabaseResult is the dest-last cursor over one entity's live rows. It
 * borrows an entity's row-pointer ChunkedList or owns a query's temporary
 * pointer-list container, and holds a
 * count snapshot plus an integer cursor, so stepping never allocates and never
 * copies a row: next() reads the stable row slot and writes the bound pointer
 * into the caller's destination. Rows are the caller's own live struct
 * instances; this class never owns or frees row values. Cold path only (a query
 * cursor is a cold rendezvous, the Cold-Only Reflection Law). Lifetime: free a
 * result before its Database and never race mutation of either list.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: DatabaseResult (database/database_result.c)
 * ============================================================================
 * the DatabaseResult row cursor.
 *
 * STRUCT FIELDS (Mirroring database/database_result.h):
 * ----------------------------------------------------------------------------
 *   DatabaseResult {
 *     ChunkedList *rows; // row-pointer list; ownership of container is explicit
 *     u32 count;    // published rows snapshot
 *     u32 cursor;   // next row index
 *     u32 ownsRows; // owns list container only, never row values
 *   }
 *
 * PRIVATE HELPERS (kept file-local, pure logic, no behavior of their own): none.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h) DatabaseResult_0(), _1(source), _2(source, ownsSource)
 * Public Core Functions: (.h) _kind, _check, _next, _count, _position, _rewind
 * Public String Projections: (.h) _toString / _toStringStruct
 * ============================================================================
 */

// CONSTRUCTORS

/**
 * Allocates an empty cursor with no row source; returns nullptr if allocation
 * fails.
 */
DatabaseResult *DatabaseResult_0(void) {
    DatabaseResult *self = (DatabaseResult*) Memory_alloc(TYPE_DB_DATABASE_RESULT_SINGLETON, sizeof(DatabaseResult));
    if (self == nullptr)
        return nullptr;
    (*self).rows = nullptr;
    (*self).count = 0u;
    (*self).cursor = 0u;
    (*self).ownsRows = 0u;
    return self;
}

/**
 * Allocates a cursor borrowing the source row list and snapshots its current
 * length; a null source is rejected and returns nullptr.
 */
DatabaseResult *DatabaseResult_1(ChunkedList *source) {
    return DatabaseResult_2(source, false);
}

/** Allocates a cursor and optionally owns only its row-pointer list container. */
DatabaseResult *DatabaseResult_2(ChunkedList *source, bool ownsSource) {
    if (source == nullptr) {
        THROW("DatabaseResult_2: null row source");
        return nullptr;
    }
    DatabaseResult *self = DatabaseResult_0();
    if (self == nullptr)
        return nullptr;
    (*self).rows = source;
    (*self).count = ChunkedList_size(source);
    (*self).ownsRows = ownsSource ? 1u : 0u;
    return self;
}

/**
 * Releases the cursor and its list container only when ownsRows is set; row
 * values are always borrowed.
 */
void DatabaseResult_free(DatabaseResult *self) {
    if (self != nullptr) {
        if ((*self).ownsRows != 0u)
            ChunkedList_free((*self).rows);
        Memory_free(self);
    }
}

// CORE FUNCTIONS

/**
 * Returns the allocation's runtime type id, or zero for a null result.
 */
u64 DatabaseResult_kind(const DatabaseResult *self) {
    if (self == nullptr)
        return 0u;
    return Memory_type((void*) self);
}

/**
 * Reports whether a non-null result allocation has the requested type id.
 */
bool DatabaseResult_check(const DatabaseResult *self, u64 typeId) {
    if (self == nullptr)
        return false;
    return Memory_type((void*) self) == typeId;
}

/**
 * Writes the next snapshotted row pointer to outRow and advances on success;
 * returns false at exhaustion or for invalid/unavailable arguments without
 * changing the cursor or destination.
 */
bool DatabaseResult_next(DatabaseResult *self, void **outRow) {
    if (self == nullptr || outRow == nullptr || (*self).rows == nullptr)
        return false;
    u32 index = (*self).cursor;
    if (index >= (*self).count)
        return false;
    u8 *slot = ChunkedList_slot((*self).rows, index);
    if (slot == nullptr)
        return false;
    (*self).cursor = index + 1u;
    *outRow = *(void**) slot;
    return true;
}

/**
 * Returns the row count captured at construction, or zero for nullptr.
 */
u32 DatabaseResult_count(const DatabaseResult *self) {
    return self ? (*self).count : 0u;
}

/**
 * Returns the index of the next row to be read, or zero for nullptr.
 */
u32 DatabaseResult_position(const DatabaseResult *self) {
    return self ? (*self).cursor : 0u;
}

/**
 * Resets a non-null cursor to the first row; a null cursor is ignored.
 */
void DatabaseResult_rewind(DatabaseResult *self) {
    if (self != nullptr)
        (*self).cursor = 0u;
}

// STRING PROJECTIONS (the toString Law)

/**
 * Formats a bounded cursor-position summary; null self formats as "nullptr"
 * and truncation is reported through outTruncated when supplied.
 */
void DatabaseResult_toString(const DatabaseResult *self, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap, "DatabaseResult(%u/%u)", (*self).cursor, (*self).count);
    if (written < 0 || (bytes) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}

/**
 * Formats the cursor's own fields into a bounded structure summary; null self
 * formats as "nullptr" and truncation is reported when possible.
 */
void DatabaseResult_toStringStruct(const DatabaseResult *self, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap,
        "DatabaseResult { rows=0x%llx, count=%u, cursor=%u, ownsRows=%u }",
        (unsigned long long) (uintptr_t) (*self).rows, (*self).count,
        (*self).cursor, (*self).ownsRows);
    if (written < 0 || (bytes) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}
