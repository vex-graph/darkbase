#ifndef DARKBASE_DATABASE_DATABASE_RESULT_H
#define DARKBASE_DATABASE_DATABASE_RESULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/constructor.h"
#include "struct/chunked_list.h"

// database/database_result.h — the DatabaseResult row cursor.
//
// A DatabaseResult walks a row-pointer list with an index cursor. Ordinary
// select results borrow the Database's list; structured query results own only
// their temporary list container. In both cases row values are borrowed and
// next() returns the matched pointer. Cold path only. Free results before the
// Database and do not mutate a borrowed source concurrently.

typedef struct DatabaseResult {
    ChunkedList *rows; // borrowed rows, or an owned list of borrowed row pointers
    u32 count;    // published rows snapshotted at construction
    u32 cursor;   // index of the next row to hand out
    u32 ownsRows;  // owns list storage only; never owns the row pointers
} DatabaseResult;

// --- Constructors ---
// _0() is the empty cursor (no source, exhausted). _1(source) borrows the row
// list; null source returns nullptr (cold rejection). Arena-allocated.
DatabaseResult *DatabaseResult_0(void);
DatabaseResult *DatabaseResult_1(ChunkedList *source);
// _2(source, ownsSource) may own the list container, never the row values.
DatabaseResult *DatabaseResult_2(ChunkedList *source, bool ownsSource);
#define DatabaseResult(...) CONSTRUCTOR_DISPATCH(DatabaseResult, __VA_ARGS__)

// Release an arena-allocated result and its list container only when ownsRows is
// set. Row values are always borrowed.
void DatabaseResult_free(DatabaseResult *self);

u64 DatabaseResult_kind(const DatabaseResult *self);
bool DatabaseResult_check(const DatabaseResult *self, u64 typeId);

// --- Core functions ---
// Advance and write the next row pointer into outRow (dest-last). False at the
// end of the result or on a null argument; outRow is untouched on false.
bool DatabaseResult_next(DatabaseResult *self, void **outRow);
// Rows snapshotted at construction.
u32 DatabaseResult_count(const DatabaseResult *self);
// Index of the next row (== consumed rows).
u32 DatabaseResult_position(const DatabaseResult *self);
// Reset the cursor to the first row.
void DatabaseResult_rewind(DatabaseResult *self);

void DatabaseResult_toString(const DatabaseResult *self, char *dest, bytes cap, bool *outTruncated);
void DatabaseResult_toStringStruct(const DatabaseResult *self, char *dest, bytes cap, bool *outTruncated);

#endif
