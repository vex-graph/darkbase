#ifndef DARKBASE_DATABASE_DATABASE_H
#define DARKBASE_DATABASE_DATABASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c23/constructor.h"
#include "database/database_catalog_entry.h"
#include "database/database_field_span.h"
#include "database/database_result.h"
#include "reflection/struct.h"
#include "struct/chunked_list.h"

// database/database.h — the L2 Database: the entity/row switchboard.
//
// A Database is the native vex store's in-memory switchboard. It registers
// reflection Structs as entities (tables) and binds rows to them. A schema IS a
// Struct (the Entity Model Law): its Fields carry both the read/write behavior
// and the physical layout. Database_insert stores a caller-owned borrowed
// pointer; Database_insertOwned and Database_load store individually
// Memory_alloc'd row copies owned by Database_free. Ownership is per entity, so
// one entity never mixes borrowed and owned rows.
//
// Vocabulary: a Struct is an entity, a Field is an entity field, a Method is a
// function. Database adds no schema type of its own.
//
// Name grammar: an entity is named by its Struct's folded 24-byte atom name
// (the same grammar VariableSlot_foldName owns). Duplicate entity names reject.
//
// Errors: the last cold rejection is recorded on the instance
// (Database_errorCode / Database_lastError) and reported once through THROW
// (the Cold-Strict, Hot-Minimal Validation Law; the THROW Law).
//
// Thread affinity: owner-affine (a single writer/reader at a time). Borrowed
// rows and every schema must outlive the Database; owned row copies do not.

#define DB_ENTITY_NAME_MAX 23u
#define DB_ERROR_MESSAGE_MAX 128u

// Cold rejection codes (Database_errorCode). DATABASE_OK is success.
typedef enum DatabaseCode {
    DATABASE_OK = 0,
    DATABASE_INVALID = 1,   // null or illegal argument
    DATABASE_DUPLICATE = 2, // an entity with the name is already registered
    DATABASE_ABSENT = 3,    // no such entity / row
    DATABASE_EXHAUSTED = 4, // row-list allocation failed
    DATABASE_IO = 5,        // durable page-store operation failed
    DATABASE_CORRUPT = 6,   // durable catalog/page stream rejected
} DatabaseCode;

typedef struct Store Store;

typedef struct Database {
    ChunkedList *entities;              // DbEntity rows (layout private to database.c)
    uint32_t chunkBytes;                // leaf byte budget for per-entity row lists
    int32_t lastCode;                   // last DATABASE_* code
    char lastError[DB_ERROR_MESSAGE_MAX];
} Database;

// --- Constructors ---
// _0() uses the default leaf budget; _1(chunkBytes) overrides it. Arena-allocated.
Database *Database_0(void);
Database *Database_1(uint32_t chunkBytes);
#define Database(...) CONSTRUCTOR_DISPATCH(Database, __VA_ARGS__)
// Release an arena-allocated Database and every per-entity row list. Rows are
// borrowed (never freed here). Never call on an inline instance.
void Database_free(Database *self);

uint64_t Database_kind(const Database *self);
bool Database_check(const Database *self, uint64_t typeId);

// --- Core functions ---
// Register a Struct as an entity. Returns its index, or -1 on a null/illegal
// schema or a duplicate name. The schema is borrowed for the Database's life.
int32_t Database_define(Database *self, Struct *schema);
// Register index for a folded name, or -1 when absent/invalid (null-safe).
int32_t Database_entityIndex(const Database *self, const char *entity);
// Live entity count.
uint32_t Database_entityCount(const Database *self);
// The borrowed schema for an entity, or null (null-safe).
Struct *Database_schema(const Database *self, const char *entity);

// Bind a caller-owned live row to an entity. Returns the row index, or -1.
int32_t Database_insert(Database *self, const char *entity, void *row);
// Copy one exact schema-stride row into Database ownership. An entity is either
// entirely borrowed (Database_insert) or owned (loaded / insertOwned); rows do
// not mix ownership modes. Returns the row index, or -1 without mutation.
int32_t Database_insertOwned(Database *self, const char *entity, const void *rowBytes,
                             size_t rowBytesLength);
// The row pointer at an index, or null (null-safe).
void *Database_row(const Database *self, const char *entity, uint32_t index);
// Live rows bound to an entity (0 when absent).
uint32_t Database_count(const Database *self, const char *entity);
// A fresh dest-last cursor over an entity's rows, or null when absent.
DatabaseResult *Database_select(Database *self, const char *entity);
// Cold raw-byte equality predicate over one reflected Field. The Field must
// have a nonzero in-row size and a range contained in Struct.size. Result owns
// its pointer-list container but borrows the matching row values; free it before
// Database. Equal values use exact byte equality, not numeric conversion.
DatabaseResult *Database_queryEqual(Database *self, const char *entity,
                                    const char *field, const void *value,
                                    size_t valueLength);
// Inclusive signed int32 range query: the field bytes are interpreted as a
// native int32_t regardless of reflected typeId. The field must have exactly
// sizeof(int32_t) bytes within Struct.size. Result borrows matching rows. Uses
// the entity's declared int32 index when one covers this field (see
// Database_indexBuild) and otherwise scans the row list; both return the same
// matches, though the indexed path yields them in key order.
DatabaseResult *Database_queryInt32Range(Database *self, const char *entity,
                                         const char *field, int32_t minimum,
                                         int32_t maximum);

// --- Named field resolution ---
// Where the NAMED field's Bytes live inside this entity's rows, so a caller
// without the C struct can read one. Resolves against the registered schema and
// folds the requested name, so case does not matter; a null or ambiguous name
// rejects rather than guessing, and a span that would leave the row is refused
// rather than returned. Read-only: it neither allocates nor disturbs an index.
// Result code is DATABASE_OK, DATABASE_ABSENT (unknown entity or field) or
// DATABASE_INVALID (null/malformed/ambiguous/out-of-row).
DatabaseFieldSpan Database_fieldSpan(const Database *self, const char *entity,
                                     const char *field);

// --- In-memory integer index ---
// Build the int32 index for one field of an entity: the Field must carry
// REFLECT_FIELD_INDEXED and span exactly sizeof(int32_t) within Struct.size, so
// the schema's flag is the opt-in declaration. Cold path. Rows are append-only,
// so inserts only mark the records stale and the next query rebuilds them; while
// an index exists the indexed field of a bound row must not be mutated in place,
// which the Database does not detect. Records are in-memory only: nothing is
// persisted, and a load drops them while the definition survives for the next
// query. Returns a DATABASE_* code.
int32_t Database_indexBuild(Database *self, const char *entity, const char *field);
// True when the entity declares an index for that field (records may be stale).
bool Database_indexed(const Database *self, const char *entity, const char *field);

// --- Persistence (M2, .vexdb) ---
// Serialize the registry and bound rows to a native-endian .vexdb file, or load
// rows back from one. load requires the same Structs registered and their
// entities empty; loaded rows become arena-owned and are freed on Database_free.
// A trailing CRC32 covers the payload; a bad header/CRC rejects before mutating.
// Returns a DATABASE_* code. Flat native-endian row Bytes; big-endian is out of
// scope for this slice.
int32_t Database_save(Database *self, const char *path);
int32_t Database_load(Database *self, const char *path);
// Page-store persistence is a separate v1 path; it does not replace the legacy
// flat snapshot API. Caller owns Store and supplies the same reflected schemas
// before load. Each complete database image is published by one root-page write;
// a failed pre-publication save leaves the prior image readable. Rows and schema
// metadata are copied into page-owned bytes; live Database rows are unchanged.
int32_t Database_saveStore(Database *self, Store *store);
int32_t Database_loadStore(Database *self, Store *store);
// Read one committed catalog record by zero-based position. The returned value
// owns no storage and remains valid independently of Store lifetime. Invalid or
// corrupt catalogs return a non-OK code; out-of-range indices report ABSENT.
DatabaseCatalogEntry Database_storeCatalogEntry(Store *store, uint32_t index);

// --- Diagnostics ---
int32_t Database_errorCode(const Database *self);
// Bounded copy of the last error message (dest-last); returns length or -1.
int Database_lastError(const Database *self, char *out, size_t outCap);
// Static text for a DATABASE_* code (never null).
const char *Database_errorText(int32_t code);

void Database_toString(const Database *self, char *dest, size_t cap, bool *outTruncated);
void Database_toStringStruct(const Database *self, char *dest, size_t cap, bool *outTruncated);

#endif
