#ifndef DARKBASE_DATABASE_DATABASE_H
#define DARKBASE_DATABASE_DATABASE_H

#include "vex.h"
#include "database/database_catalog_entry.h"
#include "database/database_field_span.h"
#include "database/database_result.h"
#include "reflection/struct.h"
#include "struct/chunked_list.h"

/**
 * database/database.h, the L2 Database: the entity/row switchboard.
 *
 * A Database is the native vex store's in-memory switchboard. It registers
 * reflection Structs as entities (tables) and binds rows to them. A schema IS a
 * Struct (the Entity Model Law): its Fields carry both the read/write behavior
 * and the physical layout. Database_insert stores a caller-owned borrowed
 * pointer; Database_insertOwned and Database_load store individually
 * Memory_alloc'd row copies owned by Database_free. Ownership is per entity, so
 * one entity never mixes borrowed and owned rows.
 *
 * Vocabulary: a Struct is an entity, a Field is an entity field, a Method is a
 * function. Database adds no schema type of its own.
 *
 * Name grammar: an entity is named by its Struct's folded 24-byte atom name
 * (the same grammar VariableSlot_foldName owns). Duplicate entity names reject.
 *
 * Errors: the last cold rejection is recorded on the instance
 * (Database_errorCode / Database_lastError) and reported once through THROW
 * (the Cold-Strict, Hot-Minimal Validation Law; the THROW Law).
 *
 * Thread affinity: owner-affine (a single writer/reader at a time). Borrowed
 * rows and every schema must outlive the Database; owned row copies do not.
 */

#define DB_ENTITY_NAME_MAX 23u
#define DB_ERROR_MESSAGE_MAX 128u

/** Cold rejection codes (Database_errorCode). DATABASE_OK is success. */
typedef enum DatabaseCode {
    DATABASE_OK = 0,
    DATABASE_INVALID = 1, /** null or illegal argument */
    DATABASE_DUPLICATE = 2, /** an entity with the name is already registered */
    DATABASE_ABSENT = 3, /** no such entity / row */
    DATABASE_EXHAUSTED = 4, /** row-list allocation failed */
    DATABASE_IO = 5, /** durable page-store operation failed */
    DATABASE_CORRUPT = 6, /** durable catalog/page stream rejected */
} DatabaseCode;

typedef struct Store Store;

#define VEX_DB_MAGIC "VEXDB01"
#define VEX_DB_VERSION 1u
#define VEX_DB_HEADER_BYTES 64u
#define VEX_DB_TRAILER_BYTES 4u

typedef struct Database {
    ChunkedList *entities;
    u32 chunkBytes;
    i32 lastCode;
    utf8 lastError[DB_ERROR_MESSAGE_MAX];
} Database;

/**
 * Private entity slot record owned by Database.
 * @field name Folded entity name using atom grammar.
 * @field owned Nonzero when every row in the entity is Database-owned.
 * @field pad Explicit alignment padding.
 * @field schema Borrowed reflection Struct layout.
 * @field rows ChunkedList of row pointers (any) with stable addresses.
 * @field indexField Folded indexed field name, or empty string when none.
 * @field indexOffset In-row byte offset of the indexed span.
 * @field indexDirty Nonzero when the row list changed since the last rebuild.
 * @field indexRecords Key-sorted ChunkedList of DbIndexRecord rows.
 */
typedef struct DbEntity {
    utf8 name[DB_ENTITY_NAME_BYTES];
    u32 owned;
    u32 pad;
    Struct *schema;
    ChunkedList *rows;
    utf8 indexField[DB_ENTITY_NAME_BYTES];
    u32 indexOffset;
    u32 indexDirty;
    ChunkedList *indexRecords;
} DbEntity;

/**
 * Private helper record for an in-memory int32 index entry.
 * @field key Indexed int32 value read from row bytes.
 * @field rowIndex Position of the row inside the entity's row list.
 * @field row Borrowed bound row pointer carrying the key.
 */
typedef struct DbIndexRecord {
    i32 key;
    u32 rowIndex;
    any row;
} DbIndexRecord;

/**
 * Private helper record for the resolved byte span of one requested field.
 * @field code DATABASE_OK or a rejection code.
 * @field field Borrowed matched Field, or nullptr unless code is DATABASE_OK.
 * @field offset In-row byte offset of the field.
 * @field size Byte size of the field.
 */
typedef struct DbFieldSpan {
    i32 code;
    Field *field;
    u32 offset;
    u32 size;
} DbFieldSpan;

/**
 * Private staging record used during atomic database load commits.
 * @field entity Target entity left untouched until all rows are staged.
 * @field rows Complete temporary owned row list.
 * @field indexRecords Persisted index records re-pointed at staged rows.
 * @field indexField Persisted folded index field name.
 * @field indexOffset Persisted in-row byte offset for the indexed field.
 */
typedef struct DbLoadStage {
    DbEntity *entity;
    ChunkedList *rows;
    ChunkedList *indexRecords;
    utf8 indexField[DB_ENTITY_NAME_BYTES];
    u32 indexOffset;
} DbLoadStage;

/**
 * Fixed 64-byte flat snapshot file header.
 * @field magic Magic signature ("VEXDB01").
 * @field version Snapshot format version (VEX_DB_VERSION).
 * @field entityCount Number of entity records following the header.
 * @field rowCount Total rows across all entities.
 * @field reserved0 Reserved field.
 * @field reserved1 Reserved field.
 * @field pad Zero padding to 64 bytes.
 */
typedef struct VexDbHeader {
    utf8 magic[8];
    u32 version;
    u32 entityCount;
    u64 rowCount;
    u32 reserved0;
    u32 reserved1;
    u8 pad[32];
} VexDbHeader;
_Static_assert(sizeof(VexDbHeader) == VEX_DB_HEADER_BYTES, "VexDbHeader must stay 64 Bytes");

/**
 * Fixed 32-byte flat snapshot entity descriptor.
 * @field name Folded entity name.
 * @field stride Entity row byte stride.
 * @field rowCount Number of raw row records following this descriptor.
 */
typedef struct VexDbEntity {
    utf8 name[DB_ENTITY_NAME_BYTES];
    u32 stride;
    u32 rowCount;
} VexDbEntity;
_Static_assert(sizeof(VexDbEntity) == 32u, "VexDbEntity must stay 32 Bytes");

/**
 * Single-page publication root for a page-store snapshot (32 bytes).
 * @field magic Root page magic signature.
 * @field version Store snapshot format version.
 * @field firstPage First data page id in the linked chain.
 * @field length Total byte length of the serialized catalog stream.
 * @field crc CRC32 over the serialized catalog stream.
 * @field reserved Must remain zero.
 */
typedef struct DatabaseStoreRoot {
    utf8 magic[8];
    u32 version;
    u32 firstPage;
    u64 length;
    u32 crc;
    u32 reserved;
} DatabaseStoreRoot;
_Static_assert(sizeof(DatabaseStoreRoot) == 32u, "DatabaseStoreRoot must stay 32 Bytes");

/**
 * Linked data-page chunk header in the page store (16 bytes).
 * @field nextPage Logical id of the next page, or zero at chain end.
 * @field chunkBytes Payload byte count in this page.
 * @field offset Contiguous byte offset within the serialized catalog stream.
 */
typedef struct DatabaseStoreLink {
    u32 nextPage;
    u32 chunkBytes;
    u64 offset;
} DatabaseStoreLink;
_Static_assert(sizeof(DatabaseStoreLink) == 16u, "DatabaseStoreLink must stay 16 Bytes");

/**
 * Serialized catalog byte-stream header (16 bytes).
 * @field magic Catalog magic signature.
 * @field version Catalog format version.
 * @field entityCount Number of persisted entity descriptors.
 */
typedef struct DatabaseStoreCatalog {
    utf8 magic[8];
    u32 version;
    u32 entityCount;
} DatabaseStoreCatalog;
_Static_assert(sizeof(DatabaseStoreCatalog) == 16u,
               "DatabaseStoreCatalog must stay 16 Bytes");

/**
 * Persisted entity descriptor inside a Store catalog (64 bytes).
 * @field name Folded entity name.
 * @field stride Entity row byte stride.
 * @field rowCount Number of raw rows following this descriptor.
 * @field indexField Folded indexed field name, or empty string when none.
 * @field indexOffset In-row byte offset of the indexed span.
 * @field indexCount Number of persisted index records following the rows.
 */
typedef struct DatabaseStoreEntity {
    utf8 name[DB_ENTITY_NAME_BYTES];
    u32 stride;
    u32 rowCount;
    utf8 indexField[DB_ENTITY_NAME_BYTES];
    u32 indexOffset;
    u32 indexCount;
} DatabaseStoreEntity;
_Static_assert(sizeof(DatabaseStoreEntity) == 64u,
               "DatabaseStoreEntity must stay 64 Bytes");

/**
 * Persisted index entry pairing a sorted key with its row position (8 bytes).
 * @field key Indexed int32 value.
 * @field rowIndex Zero-based position of the row within the entity's row list.
 */
typedef struct DatabaseStoreIndexRecord {
    i32 key;
    u32 rowIndex;
} DatabaseStoreIndexRecord;
_Static_assert(sizeof(DatabaseStoreIndexRecord) == 8u,
               "DatabaseStoreIndexRecord must stay 8 Bytes");

/**
 * --- Constructors ---
 * _0() uses the default leaf budget; _1(chunkBytes) overrides it. Arena-allocated.
 */
Database *Database_0();
Database *Database_1(u32 chunkBytes);
#define Database(...) CONSTRUCTOR_DISPATCH(Database, __VA_ARGS__)
/**
 * Release an arena-allocated Database and every per-entity row list. Rows are
 * borrowed (never freed here). Never call on an inline instance.
 */
fn Database_free(Database *self);

u64 Database_kind(const Database *self);
bool Database_check(const Database *self, u64 typeId);

/**
 * --- Core functions ---
 * Register a Struct as an entity. Returns its index, or -1 on a null/illegal
 * schema or a duplicate name. The schema is borrowed for the Database's life.
 */
i32 Database_define(Database *self, Struct *schema);
/** Register index for a folded name, or -1 when absent/invalid (null-safe). */
i32 Database_entityIndex(const Database *self, const utf8 *entity);
/** Live entity count. */
u32 Database_entityCount(const Database *self);
/** The borrowed schema for an entity, or null (null-safe). */
Struct *Database_schema(const Database *self, const utf8 *entity);

/** Bind a caller-owned live row to an entity. Returns the row index, or -1. */
i32 Database_insert(Database *self, const utf8 *entity, any row);
/**
 * Copy one exact schema-stride row into Database ownership. An entity is either
 * entirely borrowed (Database_insert) or owned (loaded / insertOwned); rows do
 * not mix ownership modes. Returns the row index, or -1 without mutation.
 */
i32 Database_insertOwned(Database *self, const utf8 *entity, const_any rowBytes,
                             bytes rowBytesLength);
/** The row pointer at an index, or null (null-safe). */
any Database_row(const Database *self, const utf8 *entity, u32 index);
/** Live rows bound to an entity (0 when absent). */
u32 Database_count(const Database *self, const utf8 *entity);
/** A fresh dest-last cursor over an entity's rows, or null when absent. */
DatabaseResult *Database_select(Database *self, const utf8 *entity);
/**
 * Cold raw-byte equality predicate over one reflected Field. The Field must
 * have a nonzero in-row size and a range contained in Struct.size. Result owns
 * its pointer-list container but borrows the matching row values; free it before
 * Database. Equal values use exact byte equality, not numeric conversion.
 */
DatabaseResult *Database_queryEqual(Database *self, const utf8 *entity,
                                    const utf8 *field, const_any value,
                                    bytes valueLength);
/**
 * Inclusive signed int32 range query: the field bytes are interpreted as a
 * native i32 regardless of reflected typeId. The field must have exactly
 * sizeof(i32) bytes within Struct.size. Result borrows matching rows. Uses
 * the entity's declared int32 index when one covers this field (see
 * Database_indexBuild) and otherwise scans the row list; both return the same
 * matches, though the indexed path yields them in key order.
 */
DatabaseResult *Database_queryInt32Range(Database *self, const utf8 *entity,
                                         const utf8 *field, i32 minimum,
                                         i32 maximum);

/**
 * --- Named field resolution ---
 * Where the NAMED field's Bytes live inside this entity's rows, so a caller
 * without the C struct can read one. Resolves against the registered schema and
 * folds the requested name, so case does not matter; a null or ambiguous name
 * rejects rather than guessing, and a span that would leave the row is refused
 * rather than returned. Read-only: it neither allocates nor disturbs an index.
 * Result code is DATABASE_OK, DATABASE_ABSENT (unknown entity or field) or
 * DATABASE_INVALID (null/malformed/ambiguous/out-of-row).
 */
DatabaseFieldSpan Database_fieldSpan(const Database *self, const utf8 *entity,
                                     const utf8 *field);

/**
 * --- In-memory integer index ---
 * Build the int32 index for one field of an entity: the Field must carry
 * REFLECT_FIELD_INDEXED and span exactly sizeof(i32) within Struct.size, so
 * the schema's flag is the opt-in declaration. Cold path. Rows are append-only,
 * so inserts only mark the records stale and the next query rebuilds them; while
 * an index exists the indexed field of a bound row must not be mutated in place,
 * which the Database does not detect. Records are in-memory only: nothing is
 * persisted, and a load drops them while the definition survives for the next
 * query. Returns a DATABASE_* code.
 */
i32 Database_indexBuild(Database *self, const utf8 *entity, const utf8 *field);
/** True when the entity declares an index for that field (records may be stale). */
bool Database_indexed(const Database *self, const utf8 *entity, const utf8 *field);

/**
 * --- Persistence (M2, .vexdb) ---
 * Serialize the registry and bound rows to a native-endian .vexdb file, or load
 * rows back from one. load requires the same Structs registered and their
 * entities empty; loaded rows become arena-owned and are freed on Database_free.
 * A trailing CRC32 covers the payload; a bad header/CRC rejects before mutating.
 * Returns a DATABASE_* code. Flat native-endian row Bytes; big-endian is out of
 * scope for this slice.
 */
i32 Database_save(Database *self, const utf8 *path);
i32 Database_load(Database *self, const utf8 *path);
/**
 * Page-store persistence is a separate v1 path; it does not replace the legacy
 * flat snapshot API. Caller owns Store and supplies the same reflected schemas
 * before load. Each complete database image is published by one root-page write;
 * a failed pre-publication save leaves the prior image readable. Rows and schema
 * metadata are copied into page-owned bytes; live Database rows are unchanged.
 */
i32 Database_saveStore(Database *self, Store *store);
i32 Database_loadStore(Database *self, Store *store);
/**
 * Read one committed catalog record by zero-based position. The returned value
 * owns no storage and remains valid independently of Store lifetime. Invalid or
 * corrupt catalogs return a non-OK code; out-of-range indices report ABSENT.
 */
DatabaseCatalogEntry Database_storeCatalogEntry(Store *store, u32 index);

/** --- Diagnostics --- */
i32 Database_errorCode(const Database *self);
/** Bounded copy of the last error message (dest-last); returns length or -1. */
i32 Database_lastError(const Database *self, utf8 *out, bytes outCap);
/** Static text for a DATABASE_* code (never null). */
const utf8 *Database_errorText(i32 code);

fn Database_toString(const Database *self, utf8 *dest, bytes cap, bool *outTruncated);
fn Database_toStringStruct(const Database *self, utf8 *dest, bytes cap, bool *outTruncated);

#endif
