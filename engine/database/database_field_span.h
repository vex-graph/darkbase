#ifndef DARKBASE_DATABASE_DATABASE_FIELD_SPAN_H
#define DARKBASE_DATABASE_DATABASE_FIELD_SPAN_H

#include <stdint.h>

#include "database/database.h"

// database/database_field_span.h — one field's resolved in-row byte span.
//
// The receipt a generic caller needs: given an entity and a field NAME, where
// do that field's Bytes live inside a row, and how many are they? A caller that
// has the C `struct` gets this from the compiler; a headless service, a tool or
// any schema-driven consumer does not, so it asks here instead of walking the
// reflection tree and folding names itself.
//
// This is a value record: it owns nothing, borrows nothing, and stays valid
// independently of the Database. `code` is DATABASE_OK when the field resolved,
// DATABASE_ABSENT for an unknown entity or field, and DATABASE_INVALID for a
// null or malformed argument, an ambiguous folded name, or a span that would
// leave the row — a span the caller could not safely read is never returned.
typedef struct DatabaseFieldSpan {
    int32_t code;   // DATABASE_OK, or the rejection code
    uint32_t offset; // in-row byte offset of the field's span
    uint32_t size;   // Bytes in the field's span
} DatabaseFieldSpan;

#endif
