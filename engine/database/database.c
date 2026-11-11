// database/database.c — the L2 Database: the entity/row switchboard.

#include "database/database.h"
#include "util/crc32.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "annotation/annotation.h"
#include "oop/type.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "oop/type.h"
#include "reflection/struct.h"
#include "relational/variable_slot.h"
#include "store/store.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Database
 * ============================================================================
 * Database is the native vex store's in-memory switchboard and the R3 entry to
 * persistence. It registers reflection Structs as entities (tables) and binds
 * either caller-owned borrowed rows or Database-owned copies. This is the
 * "everything is a pointer" thesis applied to storage: an entity IS a Struct,
 * its columns ARE Fields (behavior + physical layout), a row is addressed by a
 * stable pointer, and Database is the name -> entity -> rows index.
 *
 * The entity registry is a never-moved ChunkedList of private DbEntity slot
 * records (name, borrowed schema, per-entity row list). Each row list is a
 * ChunkedList of void* with stable addresses, so a row pointer handed to a
 * DatabaseResult stays valid across growth until the Database is freed.
 *
 * Each entity has one ownership mode: borrowed rows from Database_insert, or
 * individually Memory_alloc'd rows from Database_insertOwned/Database_load.
 * The latter two share one canonical row type and Database_free releases them.
 * Cold rejections (null/illegal name, duplicate, absent) are recorded on the
 * instance and reported once through THROW (the Cold-Strict, Hot-Minimal
 * Validation Law; the THROW Law). The class is owner-affine: one writer at a
 * time; a caller excludes concurrent mutation while a DatabaseResult walks.
 *
 * Lifetime: arena-allocated (a constructor result). Database_free releases the
 * per-entity row lists and the registry, then the block; borrowed rows and
 * schemas are never freed here. Cold path only for the reflective name lookups
 * (the Cold-Only Reflection Law): consumers that need a bind repeatedly resolve
 * once and hold the index/slot.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Database (database/database.c)
 * ============================================================================
 * the L2 Database entity/row switchboard.
 *
 * STRUCT FIELDS (Mirroring database/database.h):
 * ----------------------------------------------------------------------------
 *   Database {
 *     ChunkedList *entities;           // DbEntity rows
 *     u32 chunkBytes;             // leaf budget for per-entity row lists
 *     i32 lastCode;                // last DATABASE_* code
 *     char lastError[DB_ERROR_MESSAGE_MAX]; // last rejection text
 *   }
 *
 * PRIVATE HELPERS (kept file-local, pure logic):
 *   findEntity(self, folded)   // linear name lookup over the entity rows (static)
 *   recordError(self, code, message) // store the last result + text (static)
 *   indexDrop(entity)          // release the index records (static)
 *   indexInvalidate(entity)    // stale-mark after a row-list replacement (static)
 *   indexAt/indexSwap/indexSiftDown/indexSort(records) // heapsort by key (static)
 *   indexRebuild(self, entity) // rebuild the key-sorted records (static)
 *   resolveField(entity, name) // resolve a field name to its span, by value (static)
 *
 * SLOT RECORD (owned by Database, behaviorless; all behavior hangs off Database):
 *   DbEntity {
 *     char name[24];        // folded entity name (atom grammar)
 *     u32 owned;       // 1 = every row is Database-owned
 *     u32 pad;         // explicit padding
 *     Struct *schema;       // borrowed reflection layout (the entity)
 *     ChunkedList *rows;    // row pointers (void*); stable addresses
 *     char indexField[24];  // folded indexed field name ("" = no index declared)
 *     u32 indexOffset; // in-row byte offset of the indexed span
 *     u32 indexDirty;  // 1 = the row list changed since the last rebuild
 *     ChunkedList *indexRecords; // DbIndexRecord rows, key-sorted
 *   }
 * PRIVATE HELPER RECORD:
 *   DbIndexRecord {
 *     i32 key;   // indexed int32 value admitted from the row bytes
 *     u32 pad;  // explicit padding
 *     void *row;     // borrowed bound row carrying that value
 *   }
 *   DbFieldSpan {
 *     i32 code;  // DATABASE_OK, or the rejection code
 *     Field *field;  // borrowed matched Field (null unless code is DATABASE_OK)
 *     u32 offset, size; // resolved in-row span of the requested field
 *   }
 *   DbLoadStage {
 *     DbEntity *entity;       // target untouched entity during staged loading
 *     ChunkedList *rows;      // temporary complete owned-row list
 *     ChunkedList *indexRecords; // persisted records re-pointed at the staged rows
 *     char indexField[24];    // persisted index definition
 *     u32 indexOffset;
 *   }
 *   DatabaseStoreRoot {
 *     char magic[8]; u32 version, firstPage; u64 length;
 *     u32 crc, reserved; // snapshot publication root, 32 Bytes
 *   }
 *   DatabaseStoreLink {
 *     u32 nextPage, chunkBytes; u64 offset;
 *     // linked data chunk; offsets must be contiguous while loading
 *   }
 *   DatabaseStoreCatalog { char magic[8]; u32 version, entityCount }
 *     catalog byte-stream header (version 2 adds the persisted index section).
 *   DatabaseStoreEntity { char name[24]; u32 stride, rowCount;
 *                         char indexField[24]; u32 indexOffset, indexCount }
 *     one reflected entity descriptor followed by its raw row bytes and then its
 *     indexCount DatabaseStoreIndexRecord entries.
 *   DatabaseStoreIndexRecord { i32 key; u32 rowIndex }
 *     one persisted index record: the key and the row position carrying it.
 *
 * PRIVATE HELPERS (persistence, pure logic):
 *   writeAll(file, src, length)             // short-write-safe (static)
 *   readAll(file, dest, length)             // short-read-safe (static)
 *   syncFile(file)                          // sync mapped file data (static)
 *   loadPayload(self, bytes, len, count)         // validate snapshot records (static)
 *   loadCommit(self, bytes, len, count)          // stage rows then publish atomically (static)
 *   freeOwnedRows(rows)                          // free staged owned copies (static)
 *   databaseStoreCatalogBytes(self, outLength)   // serialize catalog + rows + index (static)
 *   databaseStoreReadRoot(store, pageBytes, outRoot) // validate root page (static)
 *   databaseStoreEnsureRoot(store)               // seed/validate page id 1 (static)
 *   databaseStoreLoadCommit(self, bytes, length)  // stage and publish rows atomically (static)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h) Database_0(), _1(chunkBytes), _init, _free
 * Public Core Functions: (.h) _kind, _check, _define, _entityIndex, _entityCount,
 *                             _schema, _insert, _insertOwned, _row, _count, _select,
 *                             _queryEqual, _queryInt32Range, _indexBuild, _indexed
 *                             _fieldSpan
 * Public Persistence: (.h) _save, _load
 *                      _saveStore, _loadStore (caller-owned page Store)
 *   i32 Database_saveStore(Database *self, Store *store)
 *   i32 Database_loadStore(Database *self, Store *store)
 *   DatabaseCatalogEntry Database_storeCatalogEntry(Store *store, u32 index)
 * Public Structured Query:
 *   DatabaseResult *Database_queryEqual(Database *self, const char *entity,
 *       const char *field, const void *value, bytes valueLength)
 *   DatabaseResult *Database_queryInt32Range(Database *self, const char *entity,
 *       const char *field, i32 minimum, i32 maximum)
 * Store catalog discovery returns a copied DatabaseCatalogEntry value; its
 * name/stride/count remain valid after Store close. A requested out-of-range
 * position returns DATABASE_ABSENT and a cold diagnostic.
 * Public Diagnostics: (.h) _errorCode, _lastError, _errorText
 * Public String Projections: (.h) _toString / _toStringStruct
 * CONTRACT: schemas and borrowed rows outlive Database; owned rows are copied
 * at exact Struct.size and freed once by Database_free. An entity cannot mix
 * ownership modes. Load stages all owned rows before publishing any entity;
 * rejection preserves every target row list and ownership flag.
 * Page-store snapshot publication is one root-page write after all chained data
 * pages commit. This v1 bridge is owner-affine and retains old pages; it is not
 * multi-page transactions, indexing, or journal recycling. A failed data-chain
 * write leaves the prior root intact; uncertain root-commit failure requires
 * Store close/reopen before further use.
 * queryEqual validates one reflected Field span and scans rows for exact byte
 * equality. It allocates an owned pointer-list container, borrows matched rows,
 * and returns no numeric coercion, ordering, planner or persistent index.
 * queryInt32Range admits an exact i32 byte span and interprets those bytes
 * as native i32 values with inclusive signed comparisons. It uses the
 * entity's declared int32 index when one covers the field and scans otherwise,
 * returning the same matches either way (key order when indexed).
 * indexBuild admits only a REFLECT_FIELD_INDEXED span of exactly i32 width:
 * the records are an in-memory key-sorted array of borrowed row pointers,
 * refreshed after a row-list change and never persisted. The indexed field of a
 * bound row must not be mutated in place while an index exists.
 * ============================================================================
 */

// PRIVATE SLOT RECORD

typedef struct DbEntity {
    char name[DB_ENTITY_NAME_BYTES]; // folded entity name (atom grammar)
    u32 owned;                  // 1 = every row is Database-owned
    u32 pad;                    // explicit padding
    Struct *schema;                  // borrowed reflection layout (the entity)
    ChunkedList *rows;               // row pointers (void*); stable addresses
    char indexField[DB_ENTITY_NAME_BYTES]; // folded indexed field name ("" = none)
    u32 indexOffset;            // in-row byte offset of the indexed span
    u32 indexDirty;             // 1 = the row list changed since the rebuild
    ChunkedList *indexRecords;       // DbIndexRecord rows, key-sorted
} DbEntity;

// PRIVATE HELPER RECORD (owned by Database; behaviorless)
typedef struct DbIndexRecord {
    i32 key;       // indexed int32 value admitted from the row bytes
    u32 rowIndex; // position of that row inside the entity's row list
    void *row;         // borrowed bound row carrying that value
} DbIndexRecord;

// PRIVATE HELPER RECORD (owned by Database; behaviorless) — the resolved span
// of one requested field, returned by value (the Value Return Law).
typedef struct DbFieldSpan {
    i32 code;  // DATABASE_OK, or the rejection code
    Field *field;  // borrowed matched Field (null unless code is DATABASE_OK)
    u32 offset;
    u32 size;
} DbFieldSpan;

// PRIVATE HELPER RECORD
typedef struct DbLoadStage {
    DbEntity *entity;          // target entity, unchanged until every row is ready
    ChunkedList *rows;         // complete temporary owned row list
    ChunkedList *indexRecords; // persisted records, re-pointed at the staged rows
    char indexField[DB_ENTITY_NAME_BYTES]; // persisted index definition
    u32 indexOffset;
} DbLoadStage;

// PRIVATE HELPERS

/**
 * Stores the latest error code and bounded message when the Database exists.
 * A null message clears the stored text; a null Database is ignored.
 */
static void recordError(Database *self, i32 code, const char *message) {
    if (self == nullptr)
        return;
    (*self).lastCode = code;
    if (message == nullptr) {
        (*self).lastError[0] = '\0';
        return;
    }
    bytes len = strlen(message);
    bytes cap = sizeof((*self).lastError);
    bytes copy = len < cap - 1u ? len : cap - 1u;
    memcpy((*self).lastError, message, copy);
    (*self).lastError[copy] = '\0';
}

/**
 * Finds an entity by its already-folded name, returning its registry index or
 * -1 when the inputs, registry, or matching entity are absent.
 */
static i32 findEntity(const Database *self, const char *folded) {
    if (self == nullptr || folded == nullptr || (*self).entities == nullptr)
        return -1;
    u32 n = ChunkedList_size((*self).entities);
    for (u32 i = 0u; i < n; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        if (entity != nullptr && strcmp((*entity).name, folded) == 0)
            return (i32) i;
    }
    return -1;
}

// --- In-memory int32 index (cold path; the Data-Oriented Storage Law) ---
// The index is a key-sorted array of borrowed row pointers layered over the
// entity's row list. Rows are append-only, so a row-list change only ever needs
// a refresh: inserts mark the records stale and the next query rebuilds them,
// which keeps the admission path allocation-free and free of partial state.

/** Release an entity's index records, leaving no index behind. */
static void indexDrop(DbEntity *entity) {
    if (entity == nullptr || (*entity).indexRecords == nullptr)
        return;
    ChunkedList_free((*entity).indexRecords);
    (*entity).indexRecords = nullptr;
}

/** Invalidate the records after the row list changed, keeping the definition. */
static void indexInvalidate(DbEntity *entity) {
    if (entity == nullptr || (*entity).indexField[0] == '\0')
        return;
    indexDrop(entity);
    (*entity).indexDirty = 1u;
}

/** Borrow one record (slot(i) resolves for every i below the list's size). */
static DbIndexRecord *indexAt(ChunkedList *records, u32 index) {
    return (DbIndexRecord*) ChunkedList_slot(records, index);
}

/** Exchange two index records in place. */
static void indexSwap(ChunkedList *records, u32 a, u32 b) {
    DbIndexRecord *ra = indexAt(records, a);
    DbIndexRecord *rb = indexAt(records, b);
    DbIndexRecord kept = *ra;
    *ra = *rb;
    *rb = kept;
}

/** Restore the max-heap property below root: heapsort's only primitive. */
static void indexSiftDown(ChunkedList *records, u32 root, u32 count) {
    while (root * 2u + 1u < count) {
        u32 child = root * 2u + 1u;
        if (child + 1u < count &&
            (*indexAt(records, child + 1u)).key > (*indexAt(records, child)).key)
            child++;
        if ((*indexAt(records, root)).key >= (*indexAt(records, child)).key)
            return;
        indexSwap(records, root, child);
        root = child;
    }
}

/** Heapsort the records by key: O(n log n), one list, no extra allocation. */
static void indexSort(ChunkedList *records) {
    u32 count = ChunkedList_size(records);
    for (u32 start = count / 2u; start > 0u; start--)
        indexSiftDown(records, start - 1u, count);
    for (u32 end = count - 1u; end > 0u; end--) {
        indexSwap(records, 0u, end);
        indexSiftDown(records, 0u, end);
    }
}

/** Rebuild the key-sorted records from the live row list (cold path only). */
static bool indexRebuild(Database *self, DbEntity *entity) {
    indexDrop(entity);
    u32 count = ChunkedList_size((*entity).rows);
    ChunkedList *records = ChunkedList_3(ID_DB_DATABASE_ROW,
        (u32) sizeof(DbIndexRecord), (*self).chunkBytes);
    if (records == nullptr)
        return false;
    for (u32 i = 0u; i < count; i++) {
        u8 *rowSlot = ChunkedList_slot((*entity).rows, i);
        void *row = rowSlot == nullptr ? nullptr : *((void**) rowSlot);
        u8 *slot = row == nullptr ? nullptr : ChunkedList_addSlot(records);
        if (slot == nullptr) {
            ChunkedList_free(records);
            return false;
        }
        DbIndexRecord *record = (DbIndexRecord*) slot;
        i32 key = 0;
        memcpy(&key, (const u8*) row + (*entity).indexOffset, sizeof(key));
        (*record).key = key;
        (*record).rowIndex = i;
        (*record).row = row;
    }
    indexSort(records);
    (*entity).indexRecords = records;
    (*entity).indexDirty = 0u;
    return true;
}

/** Resolve one requested field name to its span within the entity's schema. */
static DbFieldSpan resolveField(const DbEntity *entity, const char *fieldName) {
    DbFieldSpan span = { DATABASE_ABSENT, nullptr, 0u, 0u };
    if (entity == nullptr || (*entity).schema == nullptr)
        return span;
    char folded[REFLECT_FIELD_NAME_BYTES];
    if (fieldName == nullptr || !VariableSlot_foldName(fieldName, folded)) {
        span.code = DATABASE_INVALID;
        return span;
    }
    Field *matched = nullptr;
    u32 fieldCount = Struct_count((*entity).schema);
    for (u32 i = 0u; i < fieldCount; i++) {
        Field *candidate = Struct_get((*entity).schema, i);
        const char *candidateName = Field_getName(candidate);
        char candidateFolded[REFLECT_FIELD_NAME_BYTES];
        if (candidateName != nullptr &&
            VariableSlot_foldName(candidateName, candidateFolded) &&
            strcmp(candidateFolded, folded) == 0) {
            if (matched != nullptr) {
                span.code = DATABASE_INVALID; // ambiguous
                return span;
            }
            matched = candidate;
        }
    }
    if (matched == nullptr)
        return span; // ABSENT
    span.code = DATABASE_OK;
    span.field = matched;
    span.offset = Field_getOffset(matched);
    span.size = Field_getSize(matched);
    return span;
}

// CONSTRUCTORS

/**
 * Initializes caller-provided storage and its entity registry. A zero or too-
 * small chunk budget selects the default; failure returns false.
 */
bool Database_init(Database *self, u32 chunkBytes) {
    if (self == nullptr)
        return false;
    memset(self, 0, sizeof(*self));
    u32 budget = chunkBytes == 0u ? VEX_CHUNKED_BYTES_DEFAULT : chunkBytes;
    if (budget < 16u)
        budget = VEX_CHUNKED_BYTES_DEFAULT;
    (*self).entities = ChunkedList_3(ID_DB_DATABASE, (u32) sizeof(DbEntity), budget);
    if ((*self).entities == nullptr)
        return false;
    (*self).chunkBytes = budget;
    (*self).lastCode = DATABASE_OK;
    (*self).lastError[0] = '\0';
    return true;
}

/**
 * Allocates and initializes a Database using the requested per-entity chunk
 * budget, returning nullptr if allocation or initialization fails.
 */
Database *Database_1(u32 chunkBytes) {
    Database *self = (Database*) Memory_alloc(TYPE_DB_DATABASE_SINGLETON, sizeof(Database));
    if (self == nullptr)
        return nullptr;
    if (!Database_init(self, chunkBytes)) {
        Memory_free(self);
        return nullptr;
    }
    return self;
}

/**
 * Allocates and initializes a Database with the default chunk budget.
 */
Database *Database_0(void) {
    return Database_1(VEX_CHUNKED_BYTES_DEFAULT);
}

/**
 * Releases entity row lists and the Database allocation; only rows loaded from
 * persistence are owned and freed, while schemas and live rows are borrowed.
 */
void Database_free(Database *self) {
    if (self == nullptr)
        return;
    if ((*self).entities != nullptr) {
        u32 n = ChunkedList_size((*self).entities);
        for (u32 i = 0u; i < n; i++) {
            DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
            if (entity == nullptr || (*entity).rows == nullptr)
                continue;
            if ((*entity).owned != 0u) {
                u32 rows = ChunkedList_size((*entity).rows);
                for (u32 r = 0u; r < rows; r++) {
                    u8 *slot = ChunkedList_slot((*entity).rows, r);
                    if (slot != nullptr)
                        Memory_free(*(void**) slot);
                }
            }
            indexDrop(entity);
            ChunkedList_free((*entity).rows);
        }
        ChunkedList_free((*self).entities);
        (*self).entities = nullptr;
    }
    Memory_free(self);
}

// CORE FUNCTIONS

/**
 * Returns the allocation's runtime type id, or zero for a null Database.
 */
u64 Database_kind(const Database *self) {
    if (self == nullptr)
        return 0u;
    return Memory_type((void*) self);
}

/**
 * Reports whether a non-null Database allocation has the requested type id.
 */
bool Database_check(const Database *self, u64 typeId) {
    if (self == nullptr)
        return false;
    return Memory_type((void*) self) == typeId;
}

/**
 * Registers a borrowed reflection schema under its folded name and creates an
 * empty row list. Returns its index, or -1 with a recorded/reported error.
 */
i32 Database_define(Database *self, Struct *schema) {
    if (self == nullptr || schema == nullptr) {
        recordError(self, DATABASE_INVALID, "define: null database or schema");
        THROW("Database_define: null database or schema");
        return -1;
    }
    const char *name = Struct_getName(schema);
    if (name == nullptr) {
        recordError(self, DATABASE_INVALID, "define: schema has no valid name");
        THROW("Database_define: schema has no valid name");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(name, folded)) {
        recordError(self, DATABASE_INVALID, "define: illegal entity name");
        THROW("Database_define: illegal entity name");
        return -1;
    }
    if (findEntity(self, folded) >= 0) {
        recordError(self, DATABASE_DUPLICATE, "define: entity already registered");
        THROW("Database_define: duplicate entity");
        return -1;
    }
    u8 *slot = ChunkedList_addSlot((*self).entities);
    if (slot == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "define: entity allocation failed");
        THROW("Database_define: entity allocation failed");
        return -1;
    }
    DbEntity *entity = (DbEntity*) slot;
    memcpy((*entity).name, folded, strlen(folded) + 1u);
    (*entity).schema = schema;
    (*entity).rows = ChunkedList_3(ID_DB_DATABASE_ROW, (u32) sizeof(void*), (*self).chunkBytes);
    if ((*entity).rows == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "define: row list allocation failed");
        THROW("Database_define: row list allocation failed");
        return -1;
    }
    (*entity).indexField[0] = '\0';
    (*entity).indexOffset = 0u;
    (*entity).indexDirty = 0u;
    (*entity).indexRecords = nullptr;
    recordError(self, DATABASE_OK, "");
    return (i32) (ChunkedList_size((*self).entities) - 1u);
}

/**
 * Resolves a valid entity name to its registry index, or returns -1 when the
 * Database/name is null, malformed, or not registered.
 */
i32 Database_entityIndex(const Database *self, const char *entity) {
    if (self == nullptr || entity == nullptr)
        return -1;
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded))
        return -1;
    return findEntity(self, folded);
}

/**
 * Returns the number of registered entities, or zero for an unavailable
 * registry.
 */
u32 Database_entityCount(const Database *self) {
    if (self == nullptr || (*self).entities == nullptr)
        return 0u;
    return ChunkedList_size((*self).entities);
}

/**
 * Returns the borrowed schema for a registered entity, or nullptr if lookup
 * fails.
 */
Struct *Database_schema(const Database *self, const char *entity) {
    i32 index = Database_entityIndex(self, entity);
    if (index < 0)
        return nullptr;
    DbEntity *row = (DbEntity*) ChunkedList_slot((*self).entities, (u32) index);
    return row ? (*row).schema : nullptr;
}

/**
 * Binds a caller-owned row pointer to a live entity and returns its row index.
 * Rejections leave the row unbound and return -1 with a recorded error.
 */
i32 Database_insert(Database *self, const char *entity, void *row) {
    if (self == nullptr || entity == nullptr || row == nullptr) {
        recordError(self, DATABASE_INVALID, "insert: null argument");
        THROW("Database_insert: null argument");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded)) {
        recordError(self, DATABASE_INVALID, "insert: illegal entity name");
        THROW("Database_insert: illegal entity name");
        return -1;
    }
    i32 index = findEntity(self, folded);
    if (index < 0) {
        recordError(self, DATABASE_ABSENT, "insert: no such entity");
        THROW("Database_insert: no such entity");
        return -1;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (u32) index);
    if (target == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "insert: entity unavailable");
        THROW("Database_insert: entity unavailable");
        return -1;
    }
    if ((*target).owned != 0u) {
        recordError(self, DATABASE_INVALID, "insert: entity holds loaded rows");
        THROW("Database_insert: cannot bind a live row into a file-loaded entity");
        return -1;
    }
    u8 *slot = ChunkedList_addSlot((*target).rows);
    if (slot == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "insert: row allocation failed");
        THROW("Database_insert: row allocation failed");
        return -1;
    }
    *((void**) slot) = row;
    (*target).indexDirty = 1u;
    recordError(self, DATABASE_OK, "");
    return (i32) (ChunkedList_size((*target).rows) - 1u);
}

/**
 * Copies one exact-stride row into Database ownership. The entity must be empty
 * or already own all its rows; rejection frees any staged copy and preserves
 * both row count and ownership mode.
 */
i32 Database_insertOwned(Database *self, const char *entity, const void *rowBytes,
                             bytes rowBytesLength) {
    if (self == nullptr || entity == nullptr || rowBytes == nullptr) {
        recordError(self, DATABASE_INVALID, "insertOwned: null argument");
        THROW("Database_insertOwned: null argument");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded)) {
        recordError(self, DATABASE_INVALID, "insertOwned: illegal entity name");
        THROW("Database_insertOwned: illegal entity name");
        return -1;
    }
    i32 index = findEntity(self, folded);
    if (index < 0) {
        recordError(self, DATABASE_ABSENT, "insertOwned: no such entity");
        THROW("Database_insertOwned: no such entity");
        return -1;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (u32) index);
    if (target == nullptr || (*target).schema == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "insertOwned: entity unavailable");
        THROW("Database_insertOwned: entity unavailable");
        return -1;
    }
    u32 stride = Struct_getSize((*target).schema);
    if (stride == 0u || rowBytesLength != (bytes) stride) {
        recordError(self, DATABASE_INVALID, "insertOwned: row length must equal schema stride");
        THROW("Database_insertOwned: row length does not equal schema stride");
        return -1;
    }
    u32 count = ChunkedList_size((*target).rows);
    if ((*target).owned == 0u && count != 0u) {
        recordError(self, DATABASE_INVALID, "insertOwned: entity already has borrowed rows");
        THROW("Database_insertOwned: cannot mix borrowed and owned rows");
        return -1;
    }
    void *copy = Memory_alloc(TYPE_DB_DATABASE_ROW_SINGLETON, rowBytesLength);
    if (copy == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "insertOwned: row allocation failed");
        THROW("Database_insertOwned: row allocation failed");
        return -1;
    }
    memcpy(copy, rowBytes, rowBytesLength);
    u8 *slot = ChunkedList_addSlot((*target).rows);
    if (slot == nullptr) {
        Memory_free(copy);
        recordError(self, DATABASE_EXHAUSTED, "insertOwned: row-list allocation failed");
        THROW("Database_insertOwned: row-list allocation failed");
        return -1;
    }
    *((void**) slot) = copy;
    (*target).owned = 1u;
    (*target).indexDirty = 1u;
    recordError(self, DATABASE_OK, "");
    return (i32) (ChunkedList_size((*target).rows) - 1u);
}

/**
 * Returns the borrowed row pointer at an entity's index, or nullptr when the
 * entity, row list, index, or stored pointer is unavailable.
 */
void *Database_row(const Database *self, const char *entity, u32 index) {
    i32 e = Database_entityIndex(self, entity);
    if (e < 0)
        return nullptr;
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (u32) e);
    if (target == nullptr || (*target).rows == nullptr)
        return nullptr;
    u8 *slot = ChunkedList_slot((*target).rows, index);
    return slot ? *((void**) slot) : nullptr;
}

/**
 * Returns the number of rows bound to an entity, or zero when lookup fails.
 */
u32 Database_count(const Database *self, const char *entity) {
    i32 e = Database_entityIndex(self, entity);
    if (e < 0)
        return 0u;
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (u32) e);
    return target && (*target).rows ? ChunkedList_size((*target).rows) : 0u;
}

/**
 * Creates a cursor over an entity's current row list; absent/unavailable
 * entities return nullptr and update the Database error state.
 */
DatabaseResult *Database_select(Database *self, const char *entity) {
    i32 e = Database_entityIndex(self, entity);
    if (e < 0) {
        recordError(self, DATABASE_ABSENT, "select: no such entity");
        THROW("Database_select: no such entity");
        return nullptr;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (u32) e);
    if (target == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "select: entity unavailable");
        THROW("Database_select: entity unavailable");
        return nullptr;
    }
    recordError(self, DATABASE_OK, "");
    return DatabaseResult_1((*target).rows);
}

/** Returns a cold exact-byte equality cursor over one admitted reflected Field. */
DatabaseResult *Database_queryEqual(Database *self, const char *entityName,
                                    const char *fieldName, const void *value,
                                    bytes valueLength) {
    if (self == nullptr || entityName == nullptr || fieldName == nullptr ||
        value == nullptr || valueLength == 0u) {
        recordError(self, DATABASE_INVALID, "queryEqual: null or empty argument");
        THROW("Database_queryEqual: null or empty argument");
        return nullptr;
    }
    i32 entityIndex = Database_entityIndex(self, entityName);
    if (entityIndex < 0) {
        recordError(self, DATABASE_ABSENT, "queryEqual: no such entity");
        THROW("Database_queryEqual: no such entity");
        return nullptr;
    }
    DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities,
                                                     (u32) entityIndex);
    if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "queryEqual: entity unavailable");
        THROW("Database_queryEqual: entity unavailable");
        return nullptr;
    }
    char foldedField[REFLECT_FIELD_NAME_BYTES];
    if (!VariableSlot_foldName(fieldName, foldedField)) {
        recordError(self, DATABASE_INVALID, "queryEqual: invalid field name");
        THROW("Database_queryEqual: invalid field name");
        return nullptr;
    }
    Field *matched = nullptr;
    u32 fieldCount = Struct_count((*entity).schema);
    for (u32 i = 0u; i < fieldCount; i++) {
        Field *candidate = Struct_get((*entity).schema, i);
        const char *candidateName = Field_getName(candidate);
        char candidateFolded[REFLECT_FIELD_NAME_BYTES];
        if (candidateName != nullptr &&
            VariableSlot_foldName(candidateName, candidateFolded) &&
            strcmp(candidateFolded, foldedField) == 0) {
            if (matched != nullptr) {
                recordError(self, DATABASE_INVALID, "queryEqual: ambiguous field name");
                THROW("Database_queryEqual: ambiguous field name");
                return nullptr;
            }
            matched = candidate;
        }
    }
    if (matched == nullptr) {
        recordError(self, DATABASE_ABSENT, "queryEqual: no such field");
        THROW("Database_queryEqual: no such field");
        return nullptr;
    }
    u32 rowSize = Struct_getSize((*entity).schema);
    u32 offset = Field_getOffset(matched);
    u32 fieldSize = Field_getSize(matched);
    if (fieldSize == 0u || valueLength != fieldSize || offset > rowSize ||
        fieldSize > rowSize - offset) {
        recordError(self, DATABASE_INVALID, "queryEqual: field span or value size is invalid");
        THROW("Database_queryEqual: field span or value size is invalid");
        return nullptr;
    }
    ChunkedList *selected = ChunkedList_3(ID_DB_DATABASE_ROW,
        (u32) sizeof(void*), (*self).chunkBytes);
    if (selected == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "queryEqual: result allocation failed");
        THROW("Database_queryEqual: result allocation failed");
        return nullptr;
    }
    u32 rowCount = ChunkedList_size((*entity).rows);
    for (u32 i = 0u; i < rowCount; i++) {
        u8 *rowSlot = ChunkedList_slot((*entity).rows, i);
        void *row = rowSlot == nullptr ? nullptr : *((void**) rowSlot);
        if (row == nullptr) {
            ChunkedList_free(selected);
            recordError(self, DATABASE_CORRUPT, "queryEqual: null row slot");
            THROW("Database_queryEqual: null row slot");
            return nullptr;
        }
        if (memcmp((const u8*) row + offset, value, fieldSize) == 0) {
            u8 *selectedSlot = ChunkedList_addSlot(selected);
            if (selectedSlot == nullptr) {
                ChunkedList_free(selected);
                recordError(self, DATABASE_EXHAUSTED, "queryEqual: result growth failed");
                THROW("Database_queryEqual: result growth failed");
                return nullptr;
            }
            *((void**) selectedSlot) = row;
        }
    }
    DatabaseResult *result = DatabaseResult_2(selected, true);
    if (result == nullptr) {
        ChunkedList_free(selected);
        recordError(self, DATABASE_EXHAUSTED, "queryEqual: cursor allocation failed");
        THROW("Database_queryEqual: cursor allocation failed");
        return nullptr;
    }
    recordError(self, DATABASE_OK, "");
    return result;
}

/** Inclusive signed range scan for one exactly typed reflected int32 field. */
DatabaseResult *Database_queryInt32Range(Database *self, const char *entityName,
                                         const char *fieldName, i32 minimum,
                                         i32 maximum) {
    if (self == nullptr || entityName == nullptr || fieldName == nullptr ||
        minimum > maximum) {
        recordError(self, DATABASE_INVALID, "queryInt32Range: invalid argument or range");
        THROW("Database_queryInt32Range: invalid argument or range");
        return nullptr;
    }
    i32 entityIndex = Database_entityIndex(self, entityName);
    if (entityIndex < 0) {
        recordError(self, DATABASE_ABSENT, "queryInt32Range: no such entity");
        THROW("Database_queryInt32Range: no such entity");
        return nullptr;
    }
    DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities,
                                                     (u32) entityIndex);
    if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "queryInt32Range: entity unavailable");
        THROW("Database_queryInt32Range: entity unavailable");
        return nullptr;
    }
    char foldedField[REFLECT_FIELD_NAME_BYTES];
    if (!VariableSlot_foldName(fieldName, foldedField)) {
        recordError(self, DATABASE_INVALID, "queryInt32Range: invalid field name");
        THROW("Database_queryInt32Range: invalid field name");
        return nullptr;
    }
    Field *matched = nullptr;
    u32 fieldCount = Struct_count((*entity).schema);
    for (u32 i = 0u; i < fieldCount; i++) {
        Field *candidate = Struct_get((*entity).schema, i);
        const char *candidateName = Field_getName(candidate);
        char candidateFolded[REFLECT_FIELD_NAME_BYTES];
        if (candidateName != nullptr &&
            VariableSlot_foldName(candidateName, candidateFolded) &&
            strcmp(candidateFolded, foldedField) == 0) {
            if (matched != nullptr) {
                recordError(self, DATABASE_INVALID, "queryInt32Range: ambiguous field name");
                THROW("Database_queryInt32Range: ambiguous field name");
                return nullptr;
            }
            matched = candidate;
        }
    }
    if (matched == nullptr) {
        recordError(self, DATABASE_ABSENT, "queryInt32Range: no such field");
        THROW("Database_queryInt32Range: no such field");
        return nullptr;
    }
    u32 rowSize = Struct_getSize((*entity).schema);
    u32 offset = Field_getOffset(matched);
    u32 fieldSize = Field_getSize(matched);
    if (fieldSize != sizeof(i32) || offset > rowSize ||
        fieldSize > rowSize - offset) {
        recordError(self, DATABASE_INVALID, "queryInt32Range: field is not a valid int32 span");
        THROW("Database_queryInt32Range: field is not a valid int32 span");
        return nullptr;
    }
    ChunkedList *selected = ChunkedList_3(ID_DB_DATABASE_ROW,
        (u32) sizeof(void*), (*self).chunkBytes);
    if (selected == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "queryInt32Range: result allocation failed");
        THROW("Database_queryInt32Range: result allocation failed");
        return nullptr;
    }
    // Prefer the declared int32 index when one covers this exact field; the scan
    // below stays the fallback, and both paths must return the same matches.
    bool indexed = (*entity).indexRecords != nullptr &&
        (*entity).indexField[0] != '\0' &&
        strcmp((*entity).indexField, foldedField) == 0;
    if (indexed && (*entity).indexDirty != 0u)
        indexed = indexRebuild(self, entity);
    if (indexed) {
        u32 records = ChunkedList_size((*entity).indexRecords);
        u32 low = 0u;
        u32 high = records;
        while (low < high) { // first record whose key is >= minimum
            u32 mid = low + (high - low) / 2u;
            if ((*indexAt((*entity).indexRecords, mid)).key < minimum)
                low = mid + 1u;
            else
                high = mid;
        }
        for (u32 i = low; i < records; i++) {
            DbIndexRecord *record = indexAt((*entity).indexRecords, i);
            if ((*record).key > maximum)
                break;
            u8 *selectedSlot = ChunkedList_addSlot(selected);
            if (selectedSlot == nullptr) {
                ChunkedList_free(selected);
                recordError(self, DATABASE_EXHAUSTED, "queryInt32Range: result growth failed");
                THROW("Database_queryInt32Range: result growth failed");
                return nullptr;
            }
            *((void**) selectedSlot) = (*record).row;
        }
    } else {
        u32 rowCount = ChunkedList_size((*entity).rows);
        for (u32 i = 0u; i < rowCount; i++) {
            u8 *rowSlot = ChunkedList_slot((*entity).rows, i);
            void *row = rowSlot == nullptr ? nullptr : *((void**) rowSlot);
            if (row == nullptr) {
                ChunkedList_free(selected);
                recordError(self, DATABASE_CORRUPT, "queryInt32Range: null row slot");
                THROW("Database_queryInt32Range: null row slot");
                return nullptr;
            }
            i32 value = 0;
            memcpy(&value, (const u8*) row + offset, sizeof(value));
            if (value >= minimum && value <= maximum) {
                u8 *selectedSlot = ChunkedList_addSlot(selected);
                if (selectedSlot == nullptr) {
                    ChunkedList_free(selected);
                    recordError(self, DATABASE_EXHAUSTED, "queryInt32Range: result growth failed");
                    THROW("Database_queryInt32Range: result growth failed");
                    return nullptr;
                }
                *((void**) selectedSlot) = row;
            }
        }
    }
    DatabaseResult *result = DatabaseResult_2(selected, true);
    if (result == nullptr) {
        ChunkedList_free(selected);
        recordError(self, DATABASE_EXHAUSTED, "queryInt32Range: cursor allocation failed");
        THROW("Database_queryInt32Range: cursor allocation failed");
        return nullptr;
    }
    recordError(self, DATABASE_OK, "");
    return result;
}

// IN-MEMORY INTEGER INDEX

/**
 * Declares and builds the int32 index for one field of an entity. The Field must
 * carry REFLECT_FIELD_INDEXED and span exactly sizeof(i32) inside the row
 * stride, so the index is opt-in per field per entity and the schema stays the
 * single declaration point. Cold path. The definition outlives a row-list
 * replacement (a load drops the records and the next query rebuilds them).
 */
i32 Database_indexBuild(Database *self, const char *entityName, const char *fieldName) {
    if (self == nullptr || entityName == nullptr || fieldName == nullptr) {
        recordError(self, DATABASE_INVALID, "indexBuild: null or empty argument");
        THROW("Database_indexBuild: null or empty argument");
        return DATABASE_INVALID;
    }
    char foldedEntity[DB_ENTITY_NAME_BYTES];
    char foldedField[REFLECT_FIELD_NAME_BYTES];
    if (!VariableSlot_foldName(entityName, foldedEntity) ||
        !VariableSlot_foldName(fieldName, foldedField)) {
        recordError(self, DATABASE_INVALID, "indexBuild: invalid or unknown field");
        THROW("Database_indexBuild: invalid or unknown field");
        return DATABASE_INVALID;
    }
    i32 entityIndex = findEntity(self, foldedEntity);
    if (entityIndex < 0) {
        recordError(self, DATABASE_ABSENT, "indexBuild: no such entity");
        THROW("Database_indexBuild: no such entity");
        return DATABASE_ABSENT;
    }
    DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, (u32) entityIndex);
    if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "indexBuild: no such entity");
        THROW("Database_indexBuild: no such entity");
        return DATABASE_ABSENT;
    }
    DbFieldSpan span = resolveField(entity, fieldName);
    if (span.code != DATABASE_OK) {
        recordError(self, span.code, "indexBuild: invalid or unknown field");
        THROW("Database_indexBuild: invalid or unknown field");
        return span.code;
    }
    u32 rowSize = Struct_getSize((*entity).schema);
    if (!Field_isIndexed(span.field) || span.size != sizeof(i32) ||
        span.offset > rowSize || span.size > rowSize - span.offset) {
        recordError(self, DATABASE_INVALID, "indexBuild: field is not a declared int32 index");
        THROW("Database_indexBuild: field is not a declared int32 index");
        return DATABASE_INVALID;
    }
    memcpy((*entity).indexField, foldedField, strlen(foldedField) + 1u);
    (*entity).indexOffset = span.offset;
    (*entity).indexDirty = 1u;
    if (!indexRebuild(self, entity)) {
        (*entity).indexField[0] = '\0'; // no half-declared index survives
        recordError(self, DATABASE_EXHAUSTED, "indexBuild: index allocation failed");
        THROW("Database_indexBuild: index allocation failed");
        return DATABASE_EXHAUSTED;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/**
 * Reports whether an entity declares an index for that field. This is the
 * declaration, not the records: after a load the records are dropped and the
 * next query rebuilds them, so the answer stays true meanwhile.
 */
bool Database_indexed(const Database *self, const char *entityName, const char *fieldName) {
    if (self == nullptr || entityName == nullptr || fieldName == nullptr)
        return false;
    char foldedEntity[DB_ENTITY_NAME_BYTES];
    char foldedField[REFLECT_FIELD_NAME_BYTES];
    if (!VariableSlot_foldName(entityName, foldedEntity) ||
        !VariableSlot_foldName(fieldName, foldedField))
        return false;
    i32 entityIndex = findEntity(self, foldedEntity);
    if (entityIndex < 0)
        return false;
    DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, (u32) entityIndex);
    if (entity == nullptr || (*entity).indexField[0] == '\0')
        return false;
    return strcmp((*entity).indexField, foldedField) == 0;
}

// NAMED FIELD RESOLUTION

/**
 * Resolve a field NAME to its in-row byte span, for a consumer that has no C
 * struct to take the offset from. Read-only: it allocates nothing, disturbs no
 * index, and preserves prior state on every rejection.
 */
DatabaseFieldSpan Database_fieldSpan(const Database *self, const char *entityName,
                                     const char *fieldName) {
    DatabaseFieldSpan span = { DATABASE_INVALID, 0u, 0u };
    if (self == nullptr || entityName == nullptr || fieldName == nullptr) {
        THROW("Database_fieldSpan: null argument");
        return span;
    }
    i32 index = Database_entityIndex(self, entityName);
    if (index < 0) {
        span.code = DATABASE_ABSENT;
        THROW("Database_fieldSpan: no such entity");
        return span;
    }
    const DbEntity *entity = (const DbEntity*) ChunkedList_slot((*self).entities,
                                                               (u32) index);
    if (entity == nullptr || (*entity).schema == nullptr) {
        span.code = DATABASE_ABSENT;
        THROW("Database_fieldSpan: no such entity");
        return span;
    }
    DbFieldSpan resolved = resolveField(entity, fieldName);
    if (resolved.code != DATABASE_OK) {
        span.code = resolved.code;
        THROW("Database_fieldSpan: field did not resolve");
        return span;
    }
    // Never hand back a span that leaves the row: the caller will read it.
    u32 stride = Struct_getSize((*entity).schema);
    if (resolved.size == 0u || resolved.offset > stride ||
        resolved.size > stride - resolved.offset) {
        span.code = DATABASE_INVALID;
        THROW("Database_fieldSpan: field span leaves the row");
        return span;
    }
    span.code = DATABASE_OK;
    span.offset = resolved.offset;
    span.size = resolved.size;
    return span;
}

// DIAGNOSTICS

/**
 * Returns the latest Database error code, using DATABASE_INVALID for nullptr.
 */
i32 Database_errorCode(const Database *self) {
    return self ? (*self).lastCode : DATABASE_INVALID;
}

/**
 * Copies the last error text into a bounded destination and returns copied
 * bytes, or -1 for invalid arguments; truncation is NUL-terminated.
 */
int Database_lastError(const Database *self, char *out, bytes outCap) {
    if (self == nullptr || out == nullptr || outCap == 0u)
        return -1;
    bytes len = strlen((*self).lastError);
    bytes copy = len < outCap - 1u ? len : outCap - 1u;
    memcpy(out, (*self).lastError, copy);
    out[copy] = '\0';
    return (int) copy;
}

/**
 * Maps a Database error code to static human-readable text, including unknown
 * values.
 */
const char *Database_errorText(i32 code) {
    switch (code) {
        case DATABASE_OK:        return "ok";
        case DATABASE_INVALID:   return "invalid argument";
        case DATABASE_DUPLICATE: return "duplicate entity";
        case DATABASE_ABSENT:    return "absent entity";
        case DATABASE_EXHAUSTED: return "allocation exhausted";
        case DATABASE_IO:        return "durable store IO failure";
        case DATABASE_CORRUPT:   return "durable store data rejected";
        default:                 return "unknown";
    }
}

// PERSISTENCE (M2, .vexdb)

#define VEX_DB_MAGIC "VEXDB01"
#define VEX_DB_VERSION 1u
#define VEX_DB_HEADER_BYTES 64u
#define VEX_DB_TRAILER_BYTES 4u

// INTENTIONAL(vex): NO ENDIANNESS NEEDED!! We just write the value bytes in
// memory order (left to right) and call it a day — not laziness, but because a
// byte[] right-to-the-point is more native than byte-swap machinery and needs
// no complication. Every supported host is little-endian (Apple Silicon arm64
// floor; Windows x86_64), so there is no cross-endian peer to serve. Values are
// raw u8[] spans — nothing is encoded as text. toString() is a cold
// converter for humans, never a storage format.

// File header (64 Bytes). A CRC32 of the payload (every byte between the header
// and the trailer) is written as a 4-byte trailer, so a reader validates the
// whole file before mutating the registry.
typedef struct VexDbHeader {
    char magic[8];         // "VEXDB01\0"
    u32 version;      // VEX_DB_VERSION
    u32 entityCount;  // entity records that follow
    u64 rowCount;     // total rows across entities
    u32 reserved0;
    u32 reserved1;
    u8 pad[32];       // to 64
} VexDbHeader;
_Static_assert(sizeof(VexDbHeader) == VEX_DB_HEADER_BYTES, "VexDbHeader must stay 64 Bytes");

// One entity record (32 Bytes): rowCount * stride raw row Bytes follow it.
typedef struct VexDbEntity {
    char name[DB_ENTITY_NAME_BYTES]; // folded entity name
    u32 stride;                 // entity row byte stride
    u32 rowCount;               // rows that follow
} VexDbEntity;
_Static_assert(sizeof(VexDbEntity) == 32u, "VexDbEntity must stay 32 Bytes");

// DatabaseStoreRoot is the single-page publication point for a page-chain
// snapshot. Data pages are durable before this record is replaced.
typedef struct DatabaseStoreRoot {
    char magic[8];
    u32 version;
    u32 firstPage;
    u64 length;
    u32 crc;
    u32 reserved;
} DatabaseStoreRoot;
_Static_assert(sizeof(DatabaseStoreRoot) == 32u, "DatabaseStoreRoot must stay 32 Bytes");

typedef struct DatabaseStoreLink {
    u32 nextPage;
    u32 chunkBytes;
    u64 offset;
} DatabaseStoreLink;
_Static_assert(sizeof(DatabaseStoreLink) == 16u, "DatabaseStoreLink must stay 16 Bytes");

typedef struct DatabaseStoreCatalog {
    char magic[8];
    u32 version;
    u32 entityCount;
} DatabaseStoreCatalog;
_Static_assert(sizeof(DatabaseStoreCatalog) == 16u,
               "DatabaseStoreCatalog must stay 16 Bytes");

typedef struct DatabaseStoreEntity {
    char name[DB_ENTITY_NAME_BYTES];
    u32 stride;
    u32 rowCount;
    char indexField[DB_ENTITY_NAME_BYTES]; // folded indexed field name ("" = none)
    u32 indexOffset;                  // in-row byte offset of the indexed span
    u32 indexCount;                   // persisted index records after the rows
} DatabaseStoreEntity;
_Static_assert(sizeof(DatabaseStoreEntity) == 64u,
               "DatabaseStoreEntity must stay 64 Bytes");

// One persisted index record: the key and the position of the row carrying it.
// The rowIndex, not an address, is what makes the records reload-addressable.
typedef struct DatabaseStoreIndexRecord {
    i32 key;
    u32 rowIndex;
} DatabaseStoreIndexRecord;
_Static_assert(sizeof(DatabaseStoreIndexRecord) == 8u,
               "DatabaseStoreIndexRecord must stay 8 Bytes");

#define DATABASE_STORE_ROOT_ID 1u
#define DATABASE_STORE_VERSION 2u
#define DATABASE_STORE_ROOT_MAGIC "DBROOT01"
#define DATABASE_STORE_CATALOG_MAGIC "DBCAT001"

/**
 * Writes the full byte span, retrying short writes; returns false on a
 * non-progressing or failed write.
 */
static bool writeAll(File *file, const void *src, bytes length) {
    const u8 *p = (const u8*) src;
    bytes done = 0u;
    while (done < length) {
        int64_t n = File_write(file, p + done, (int64_t) (length - done));
        if (n <= 0)
            return false;
        done += (bytes) n;
    }
    return true;
}

/**
 * Reads the requested byte span, retrying short reads; returns false on EOF or
 * a failed read before completion.
 */
static bool readAll(File *file, void *dest, bytes length) {
    u8 *p = (u8*) dest;
    bytes done = 0u;
    while (done < length) {
        int64_t n = File_read(file, p + done, (int64_t) (length - done));
        if (n <= 0)
            return false;
        done += (bytes) n;
    }
    return true;
}

/**
 * Flushes the File stream and fsyncs its OS handle before publication. This is
 * implemented for the macOS/POSIX floor; Windows durability is not provided.
 */
static bool syncFile(File *file) {
    if (file == nullptr || File_flush(file) != true)
        return false;
    FILE *handle = File_handle(file);
    if (handle == nullptr)
        return false;
    return fsync(fileno(handle)) == 0;
}

/** Checks payload records against registered, empty entities without mutation. */
static i32 loadPayload(Database *self, const u8 *payload, bytes length,
                           u32 entityCount) {
    bytes offset = 0u;
    for (u32 e = 0u; e < entityCount; e++) {
        if (offset > length || length - offset < sizeof(VexDbEntity))
            return DATABASE_INVALID;
        VexDbEntity record;
        memcpy(&record, payload + offset, sizeof(record));
        offset += sizeof(record);
        if (record.stride == 0u)
            return DATABASE_INVALID;
        if (memchr(record.name, '\0', sizeof(record.name)) == nullptr)
            return DATABASE_INVALID;
        bytes rowBytes = (bytes) record.stride * (bytes) record.rowCount;
        if (rowBytes > length || offset > length - rowBytes)
            return DATABASE_INVALID;
        i32 index = Database_entityIndex(self, record.name);
        if (index < 0)
            return DATABASE_ABSENT;
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, (u32) index);
        if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr)
            return DATABASE_ABSENT;
        if (Struct_getSize((*entity).schema) != record.stride)
            return DATABASE_INVALID;
        if (ChunkedList_size((*entity).rows) != 0u)
            return DATABASE_DUPLICATE;
        for (u32 prior = 0u; prior < e; prior++) {
            bytes previousOffset = 0u;
            VexDbEntity previous = {0};
            for (u32 p = 0u; p <= prior; p++) {
                memcpy(&previous, payload + previousOffset, sizeof(previous));
                if (p == prior)
                    break;
                previousOffset += sizeof(previous) +
                    (bytes) previous.stride * (bytes) previous.rowCount;
            }
            if (memchr(previous.name, '\0', sizeof(previous.name)) == nullptr)
                return DATABASE_INVALID;
            if (Database_entityIndex(self, previous.name) == index)
                return DATABASE_DUPLICATE;
        }
        offset += rowBytes;
    }
    return offset == length ? DATABASE_OK : DATABASE_INVALID;
}

/** Frees every owned row in a temporary list and then releases the list. */
static void freeOwnedRows(ChunkedList *rows) {
    if (rows == nullptr)
        return;
    u32 count = ChunkedList_size(rows);
    for (u32 i = 0u; i < count; i++) {
        u8 *slot = ChunkedList_slot(rows, i);
        if (slot != nullptr)
            Memory_free(*(void**) slot);
    }
    ChunkedList_free(rows);
}

/**
 * Builds every entity's rows off to the side, then swaps all lists only after
 * every allocation succeeds. Rejection leaves every target entity untouched.
 */
static i32 loadCommit(Database *self, const u8 *payload, bytes length,
                          u32 entityCount) {
    if (entityCount == 0u)
        return length == 0u ? DATABASE_OK : DATABASE_INVALID;
    if ((bytes) entityCount > SIZE_MAX / sizeof(DbLoadStage))
        return DATABASE_EXHAUSTED;
    bytes stageBytes = (bytes) entityCount * sizeof(DbLoadStage);
    DbLoadStage *stages = (DbLoadStage*) Memory_alloc(0u, stageBytes);
    if (stages == nullptr)
        return DATABASE_EXHAUSTED;
    memset(stages, 0, stageBytes);

    i32 status = DATABASE_OK;
    u32 staged = 0u;
    bytes offset = 0u;
    for (u32 e = 0u; e < entityCount && status == DATABASE_OK; e++) {
        VexDbEntity record;
        memcpy(&record, payload + offset, sizeof(record));
        offset += sizeof(record);
        i32 index = Database_entityIndex(self, record.name);
        DbEntity *entity = index < 0 ? nullptr : (DbEntity*) ChunkedList_slot(
            (*self).entities, (u32) index);
        if (entity == nullptr) {
            status = DATABASE_ABSENT;
            break;
        }
        for (u32 p = 0u; p < staged; p++)
            if (stages[p].entity == entity)
                status = DATABASE_DUPLICATE;
        if (status != DATABASE_OK)
            break;

        ChunkedList *rows = ChunkedList_3(ID_DB_DATABASE_ROW,
            (u32) sizeof(void*), (*self).chunkBytes);
        if (rows == nullptr) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        stages[staged].entity = entity;
        stages[staged].rows = rows;
        staged++;
        if (!ChunkedList_reserve(rows, record.rowCount)) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        for (u32 r = 0u; r < record.rowCount; r++) {
            void *copy = Memory_alloc(TYPE_DB_DATABASE_ROW_SINGLETON, record.stride);
            if (copy == nullptr) {
                status = DATABASE_EXHAUSTED;
                break;
            }
            memcpy(copy, payload + offset + (bytes) r * record.stride, record.stride);
            u8 *slot = ChunkedList_addSlot(rows);
            if (slot == nullptr) {
                Memory_free(copy);
                status = DATABASE_EXHAUSTED;
                break;
            }
            *((void**) slot) = copy;
        }
        offset += (bytes) record.stride * (bytes) record.rowCount;
    }
    if (status == DATABASE_OK && offset != length)
        status = DATABASE_INVALID;

    if (status == DATABASE_OK) {
        for (u32 i = 0u; i < staged; i++) {
            DbEntity *entity = stages[i].entity;
            ChunkedList *oldRows = (*entity).rows;
            (*entity).rows = stages[i].rows;
            (*entity).owned = 1u;
            stages[i].rows = nullptr;
            ChunkedList_free(oldRows);
            if (stages[i].indexRecords != nullptr) {
                // The image carried an index: adopt its definition and records,
                // which already point at the rows just published.
                indexDrop(entity);
                memcpy((*entity).indexField, stages[i].indexField,
                       sizeof((*entity).indexField));
                (*entity).indexOffset = stages[i].indexOffset;
                (*entity).indexDirty = 0u;
                (*entity).indexRecords = stages[i].indexRecords;
                stages[i].indexRecords = nullptr;
            } else {
                indexInvalidate(entity); // changed rows; the next query rebuilds
            }
        }
    }
    for (u32 i = 0u; i < staged; i++) {
        freeOwnedRows(stages[i].rows);
        if (stages[i].indexRecords != nullptr)
            ChunkedList_free(stages[i].indexRecords);
    }
    Memory_free(stages);
    return status;
}

/**
 * Writes a CRC-protected native-endian snapshot to a temporary sibling, syncs
 * it, then renames it over the destination; failures preserve the old file.
 */
i32 Database_save(Database *self, const char *path) {
    if (self == nullptr || path == nullptr) {
        recordError(self, DATABASE_INVALID, "save: null database or path");
        THROW("Database_save: null database or path");
        return DATABASE_INVALID;
    }
    u32 entities = Database_entityCount(self);
    u64 rowTotal = 0u;
    for (u32 i = 0u; i < entities; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        if (entity == nullptr || (*entity).schema == nullptr || Struct_getSize((*entity).schema) == 0u) {
            recordError(self, DATABASE_INVALID, "save: entity has no row stride");
            THROW("Database_save: entity has no row stride");
            return DATABASE_INVALID;
        }
        rowTotal += (*entity).rows ? ChunkedList_size((*entity).rows) : 0u;
    }
    // Publish atomically: write a temporary sibling, fsync it, then rename it
    // over the destination. A failed or interrupted write leaves the previous
    // snapshot intact; the destination is only ever the old or the new file.
    char tmp[FILE_PATH_MAX + 32];
    int tmpLen = snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long) getpid());
    if (tmpLen < 0 || (bytes) tmpLen >= sizeof(tmp)) {
        recordError(self, DATABASE_INVALID, "save: path too long");
        THROW("Database_save: path too long");
        return DATABASE_INVALID;
    }
    File *file = File_open(tmp, FILE_MODE_WRITE | FILE_MODE_CREATE | FILE_MODE_TRUNCATE);
    if (file == nullptr) {
        recordError(self, DATABASE_INVALID, "save: cannot open temp file");
        THROW("Database_save: cannot open temp file");
        return DATABASE_INVALID;
    }
    VexDbHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, VEX_DB_MAGIC, 7u);
    header.version = VEX_DB_VERSION;
    header.entityCount = entities;
    header.rowCount = rowTotal;
    u32 crc = CRC32_SEED;
    bool ok = writeAll(file, &header, sizeof(header));
    for (u32 i = 0u; ok && i < entities; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        u32 stride = Struct_getSize((*entity).schema);
        u32 count = (*entity).rows ? ChunkedList_size((*entity).rows) : 0u;
        VexDbEntity record;
        memset(&record, 0, sizeof(record));
        memcpy(record.name, (*entity).name, DB_ENTITY_NAME_BYTES);
        record.stride = stride;
        record.rowCount = count;
        ok = writeAll(file, &record, sizeof(record));
        crc = Crc32_update(crc, &record, sizeof(record));
        for (u32 r = 0u; ok && r < count; r++) {
            u8 *slot = ChunkedList_slot((*entity).rows, r);
            void *row = slot ? *(void**) slot : nullptr;
            if (row == nullptr) {
                ok = false;
                break;
            }
            ok = writeAll(file, row, stride);
            crc = Crc32_update(crc, row, stride);
        }
    }
    u32 trailer = Crc32_finalize(crc);
    if (ok)
        ok = writeAll(file, &trailer, VEX_DB_TRAILER_BYTES);
    if (ok)
        ok = syncFile(file);
    File_close(file);
    if (!ok) {
        remove(tmp);
        recordError(self, DATABASE_EXHAUSTED, "save: write failed");
        THROW("Database_save: write failed");
        return DATABASE_EXHAUSTED;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        recordError(self, DATABASE_EXHAUSTED, "save: publish failed");
        THROW("Database_save: publish rename failed");
        return DATABASE_EXHAUSTED;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/**
 * Validates a snapshot header, payload checksum, registered schemas, and empty
 * target entities before loading rows as Database-owned arena allocations.
 */
i32 Database_load(Database *self, const char *path) {
    if (self == nullptr || path == nullptr) {
        recordError(self, DATABASE_INVALID, "load: null database or path");
        THROW("Database_load: null database or path");
        return DATABASE_INVALID;
    }
    File *file = File_open(path, FILE_MODE_READ);
    if (file == nullptr) {
        recordError(self, DATABASE_INVALID, "load: cannot open file");
        THROW("Database_load: cannot open file");
        return DATABASE_INVALID;
    }
    VexDbHeader header;
    if (!readAll(file, &header, sizeof(header)) ||
        memcmp(header.magic, VEX_DB_MAGIC, 7u) != 0 ||
        header.version != VEX_DB_VERSION) {
        File_close(file);
        recordError(self, DATABASE_INVALID, "load: bad header");
        THROW("Database_load: bad header");
        return DATABASE_INVALID;
    }
    int64_t size = File_size(file);
    int64_t payloadLen = size - (int64_t) VEX_DB_HEADER_BYTES - (int64_t) VEX_DB_TRAILER_BYTES;
    if (size < (int64_t) (VEX_DB_HEADER_BYTES + VEX_DB_TRAILER_BYTES) || payloadLen < 0) {
        File_close(file);
        recordError(self, DATABASE_INVALID, "load: truncated file");
        THROW("Database_load: truncated file");
        return DATABASE_INVALID;
    }
    u8 *payload = nullptr;
    if (payloadLen > 0) {
        payload = (u8*) Memory_alloc(0u, (bytes) payloadLen);
        if (payload == nullptr) {
            File_close(file);
            recordError(self, DATABASE_EXHAUSTED, "load: payload allocation failed");
            THROW("Database_load: payload allocation failed");
            return DATABASE_EXHAUSTED;
        }
        if (!readAll(file, payload, (bytes) payloadLen)) {
            Memory_free(payload);
            File_close(file);
            recordError(self, DATABASE_INVALID, "load: payload read failed");
            THROW("Database_load: payload read failed");
            return DATABASE_INVALID;
        }
    }
    u32 trailer = 0u;
    bool haveTrailer = readAll(file, &trailer, VEX_DB_TRAILER_BYTES);
    File_close(file);
    if (!haveTrailer) {
        if (payload)
            Memory_free(payload);
        recordError(self, DATABASE_INVALID, "load: missing checksum");
        THROW("Database_load: missing checksum");
        return DATABASE_INVALID;
    }
    u32 crc = Crc32_finalize(Crc32_update(CRC32_SEED, payload, (bytes) payloadLen));
    if (crc != trailer) {
        if (payload)
            Memory_free(payload);
        recordError(self, DATABASE_INVALID, "load: checksum mismatch");
        THROW("Database_load: checksum mismatch");
        return DATABASE_INVALID;
    }
    i32 status = loadPayload(self, payload, (bytes) payloadLen, header.entityCount);
    if (status == DATABASE_OK)
        status = loadCommit(self, payload, (bytes) payloadLen, header.entityCount);
    if (payload)
        Memory_free(payload);
    if (status != DATABASE_OK) {
        recordError(self, status, "load: payload rejected");
        THROW("Database_load: payload rejected");
        return status;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/** Serializes an explicit reflected catalog followed by entity row spans. */
static u8 *databaseStoreCatalogBytes(Database *self, bytes *outLength) {
    if (outLength != nullptr)
        *outLength = 0u;
    u32 entityCount = Database_entityCount(self);
    bytes length = sizeof(DatabaseStoreCatalog);
    for (u32 e = 0u; e < entityCount; e++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, e);
        if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr)
            return nullptr;
        u32 stride = Struct_getSize((*entity).schema);
        u32 count = ChunkedList_size((*entity).rows);
        if (stride == 0u || (bytes) count > (SIZE_MAX - sizeof(DatabaseStoreEntity)) / stride)
            return nullptr;
        bytes rowBytes = (bytes) count * stride;
        if ((*entity).indexField[0] != '\0') {
            // A declared index is refreshed here so the image never carries stale
            // keys; a refresh failure fails the save instead of dropping quietly.
            if (((*entity).indexRecords == nullptr || (*entity).indexDirty != 0u) &&
                !indexRebuild(self, entity))
                return nullptr;
            if (ChunkedList_size((*entity).indexRecords) != count)
                return nullptr; // an index must cover every row
        }
        bytes indexBytes = (*entity).indexField[0] == '\0' ? 0u
            : (bytes) count * sizeof(DatabaseStoreIndexRecord);
        if (rowBytes > SIZE_MAX - indexBytes - sizeof(DatabaseStoreEntity))
            return nullptr;
        bytes region = sizeof(DatabaseStoreEntity) + rowBytes + indexBytes;
        if (length > SIZE_MAX - region)
            return nullptr;
        length += region;
    }
    u8 *bytes = (u8*) Memory_alloc(0u, length);
    if (bytes == nullptr)
        return nullptr;
    DatabaseStoreCatalog catalog;
    memset(&catalog, 0, sizeof(catalog));
    memcpy(catalog.magic, DATABASE_STORE_CATALOG_MAGIC, sizeof(catalog.magic));
    catalog.version = DATABASE_STORE_VERSION;
    catalog.entityCount = entityCount;
    memcpy(bytes, &catalog, sizeof(catalog));
    bytes offset = sizeof(catalog);
    for (u32 e = 0u; e < entityCount; e++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, e);
        u32 stride = Struct_getSize((*entity).schema);
        u32 count = ChunkedList_size((*entity).rows);
        u32 indexCount = 0u;
        if ((*entity).indexField[0] != '\0') {
            if (((*entity).indexRecords == nullptr || (*entity).indexDirty != 0u) &&
                !indexRebuild(self, entity)) {
                Memory_free(bytes);
                return nullptr;
            }
            indexCount = ChunkedList_size((*entity).indexRecords);
            if (indexCount != count) {
                Memory_free(bytes);
                return nullptr;
            }
        }
        DatabaseStoreEntity record;
        memset(&record, 0, sizeof(record));
        memcpy(record.name, (*entity).name, sizeof(record.name));
        record.stride = stride;
        record.rowCount = count;
        memcpy(record.indexField, (*entity).indexField, sizeof(record.indexField));
        record.indexOffset = indexCount == 0u ? 0u : (*entity).indexOffset;
        record.indexCount = indexCount;
        memcpy(bytes + offset, &record, sizeof(record));
        offset += sizeof(record);
        for (u32 r = 0u; r < count; r++) {
            u8 *slot = ChunkedList_slot((*entity).rows, r);
            void *row = slot == nullptr ? nullptr : *((void**) slot);
            if (row == nullptr) {
                Memory_free(bytes);
                return nullptr;
            }
            memcpy(bytes + offset, row, stride);
            offset += stride;
        }
        for (u32 r = 0u; r < indexCount; r++) {
            DbIndexRecord *source = indexAt((*entity).indexRecords, r);
            DatabaseStoreIndexRecord entry;
            entry.key = (*source).key;
            entry.rowIndex = (*source).rowIndex;
            memcpy(bytes + offset, &entry, sizeof(entry));
            offset += sizeof(entry);
        }
    }
    if (outLength != nullptr)
        *outLength = offset;
    return bytes;
}

/** Reads and validates the fixed catalog publication point. */
static bool databaseStoreReadRoot(Store *store, u8 *pageBytes,
                                  DatabaseStoreRoot *outRoot) {
    u32 length = 0u;
    u32 kind = 0u;
    bool truncated = false;
    StoreCode code = Store_page_read(store, DATABASE_STORE_ROOT_ID, pageBytes,
        Store_pageSize(store), &length, &kind, &truncated);
    if (code != STORE_OK || truncated || kind != PAGE_KIND_CATALOG ||
        length != sizeof(*outRoot))
        return false;
    memcpy(outRoot, pageBytes, sizeof(*outRoot));
    return memcmp((*outRoot).magic, DATABASE_STORE_ROOT_MAGIC, 8u) == 0 &&
        (*outRoot).version == DATABASE_STORE_VERSION && (*outRoot).reserved == 0u;
}

/** Ensures a fresh Store has the fixed, empty root page at logical page id one. */
static bool databaseStoreEnsureRoot(Store *store) {
    if (Store_pageCount(store) != 0u) {
        u32 pageSize = Store_pageSize(store);
        u8 *pageBytes = (u8*) Memory_alloc(0u, pageSize);
        if (pageBytes == nullptr)
            return false;
        DatabaseStoreRoot existing;
        bool valid = databaseStoreReadRoot(store, pageBytes, &existing);
        Memory_free(pageBytes);
        return valid;
    }
    DatabaseStoreRoot root;
    memset(&root, 0, sizeof(root));
    memcpy(root.magic, DATABASE_STORE_ROOT_MAGIC, 8u);
    root.version = DATABASE_STORE_VERSION;
    u32 pageId = 0u;
    return Store_add(store, PAGE_KIND_CATALOG, (const u8*) &root,
                     (u32) sizeof(root), &pageId) == STORE_OK &&
        pageId == DATABASE_STORE_ROOT_ID;
}

/** Validates a Store catalog and atomically publishes owned row lists. */
static i32 databaseStoreLoadCommit(Database *self, const u8 *bytes,
                                       bytes length) {
    if (length < sizeof(DatabaseStoreCatalog))
        return DATABASE_CORRUPT;
    DatabaseStoreCatalog catalog;
    memcpy(&catalog, bytes, sizeof(catalog));
    if (memcmp(catalog.magic, DATABASE_STORE_CATALOG_MAGIC, sizeof(catalog.magic)) != 0 ||
        catalog.version != DATABASE_STORE_VERSION ||
        (bytes) catalog.entityCount > (length - sizeof(catalog)) /
            sizeof(DatabaseStoreEntity))
        return DATABASE_CORRUPT;

    bytes offset = sizeof(catalog);
    for (u32 e = 0u; e < catalog.entityCount; e++) {
        if (offset > length || length - offset < sizeof(DatabaseStoreEntity))
            return DATABASE_CORRUPT;
        DatabaseStoreEntity record;
        memcpy(&record, bytes + offset, sizeof(record));
        offset += sizeof(record);
        if (record.stride == 0u || memchr(record.name, '\0', sizeof(record.name)) == nullptr ||
            (bytes) record.rowCount > SIZE_MAX / record.stride)
            return DATABASE_CORRUPT;
        bytes rowBytes = (bytes) record.rowCount * record.stride;
        if (rowBytes > length - offset)
            return DATABASE_CORRUPT;
        i32 index = Database_entityIndex(self, record.name);
        if (index < 0)
            return DATABASE_ABSENT;
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, (u32) index);
        if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr)
            return DATABASE_ABSENT;
        if (Struct_getSize((*entity).schema) != record.stride)
            return DATABASE_INVALID;
        if (record.indexCount != 0u) {
            if (record.indexField[0] == '\0' || record.indexCount != record.rowCount)
                return DATABASE_CORRUPT;
            if (record.indexOffset > record.stride ||
                sizeof(i32) > record.stride - record.indexOffset)
                return DATABASE_CORRUPT;
            // The registered schema must still declare that exact indexed span:
            // a schema that drifted from the image is rejected, never adapted.
            DbFieldSpan span = resolveField(entity, record.indexField);
            if (span.code != DATABASE_OK || span.offset != record.indexOffset ||
                span.size != sizeof(i32) || !Field_isIndexed(span.field))
                return DATABASE_CORRUPT;
        }
        bytes indexBytes = (bytes) record.indexCount *
            sizeof(DatabaseStoreIndexRecord);
        if (indexBytes > length - offset - rowBytes)
            return DATABASE_CORRUPT;
        if (record.indexCount != 0u) {
            const u8 *rows = bytes + offset;
            const u8 *entries = rows + rowBytes;
            i32 lastKey = 0;
            for (u32 r = 0u; r < record.indexCount; r++) {
                DatabaseStoreIndexRecord entry;
                memcpy(&entry, entries + (bytes) r * sizeof(entry), sizeof(entry));
                if (entry.rowIndex >= record.rowCount)
                    return DATABASE_CORRUPT;
                i32 key = 0;
                memcpy(&key, rows + (bytes) entry.rowIndex * record.stride +
                    record.indexOffset, sizeof(key));
                if (key != entry.key || (r != 0u && entry.key < lastKey))
                    return DATABASE_CORRUPT; // keys must be sorted and match their rows
                lastKey = entry.key;
            }
        }
        if (ChunkedList_size((*entity).rows) != 0u)
            return DATABASE_DUPLICATE;
        bytes priorOffset = sizeof(catalog);
        for (u32 prior = 0u; prior < e; prior++) {
            DatabaseStoreEntity previous;
            memcpy(&previous, bytes + priorOffset, sizeof(previous));
            if (strcmp(previous.name, record.name) == 0)
                return DATABASE_DUPLICATE;
            priorOffset += sizeof(previous) +
                (bytes) previous.rowCount * previous.stride +
                (bytes) previous.indexCount * sizeof(DatabaseStoreIndexRecord);
        }
        offset += rowBytes + indexBytes;
    }
    if (offset != length)
        return DATABASE_CORRUPT;
    if (catalog.entityCount == 0u)
        return DATABASE_OK;
    if ((bytes) catalog.entityCount > SIZE_MAX / sizeof(DbLoadStage))
        return DATABASE_EXHAUSTED;
    bytes stageBytes = (bytes) catalog.entityCount * sizeof(DbLoadStage);
    DbLoadStage *stages = (DbLoadStage*) Memory_alloc(0u, stageBytes);
    if (stages == nullptr)
        return DATABASE_EXHAUSTED;
    memset(stages, 0, stageBytes);
    offset = sizeof(catalog);
    i32 status = DATABASE_OK;
    u32 staged = 0u;
    for (u32 e = 0u; e < catalog.entityCount && status == DATABASE_OK; e++) {
        DatabaseStoreEntity record;
        memcpy(&record, bytes + offset, sizeof(record));
        offset += sizeof(record);
        i32 index = Database_entityIndex(self, record.name);
        DbEntity *entity = index < 0 ? nullptr : (DbEntity*) ChunkedList_slot(
            (*self).entities, (u32) index);
        if (entity == nullptr) {
            status = DATABASE_ABSENT;
            break;
        }
        ChunkedList *rows = ChunkedList_3(ID_DB_DATABASE_ROW,
            (u32) sizeof(void*), (*self).chunkBytes);
        if (rows == nullptr) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        stages[staged].entity = entity;
        stages[staged].rows = rows;
        staged++;
        if (!ChunkedList_reserve(rows, record.rowCount)) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        for (u32 r = 0u; r < record.rowCount; r++) {
            void *copy = Memory_alloc(TYPE_DB_DATABASE_ROW_SINGLETON, record.stride);
            if (copy == nullptr) {
                status = DATABASE_EXHAUSTED;
                break;
            }
            memcpy(copy, bytes + offset + (bytes) r * record.stride, record.stride);
            u8 *slot = ChunkedList_addSlot(rows);
            if (slot == nullptr) {
                Memory_free(copy);
                status = DATABASE_EXHAUSTED;
                break;
            }
            *((void**) slot) = copy;
        }
        if (status == DATABASE_OK && record.indexCount != 0u) {
            ChunkedList *records = ChunkedList_3(ID_DB_DATABASE_ROW,
                (u32) sizeof(DbIndexRecord), (*self).chunkBytes);
            if (records == nullptr || !ChunkedList_reserve(records, record.indexCount)) {
                ChunkedList_free(records);
                status = DATABASE_EXHAUSTED;
                break;
            }
            for (u32 r = 0u; r < record.indexCount; r++) {
                DatabaseStoreIndexRecord entry;
                memcpy(&entry, bytes + offset +
                    (bytes) record.rowCount * record.stride +
                    (bytes) r * sizeof(entry), sizeof(entry));
                u8 *rowSlot = ChunkedList_slot(rows, entry.rowIndex);
                u8 *slot = rowSlot == nullptr ? nullptr : ChunkedList_addSlot(records);
                if (slot == nullptr) {
                    ChunkedList_free(records);
                    status = DATABASE_EXHAUSTED;
                    break;
                }
                DbIndexRecord *target = (DbIndexRecord*) slot;
                (*target).key = entry.key;
                (*target).rowIndex = entry.rowIndex;
                (*target).row = *((void**) rowSlot);
            }
            if (status != DATABASE_OK)
                break;
            stages[staged - 1u].indexRecords = records;
            memcpy(stages[staged - 1u].indexField, record.indexField,
                   sizeof(record.indexField));
            stages[staged - 1u].indexOffset = record.indexOffset;
        }
        offset += (bytes) record.rowCount * record.stride +
            (bytes) record.indexCount * sizeof(DatabaseStoreIndexRecord);
    }
    if (status == DATABASE_OK) {
        for (u32 i = 0u; i < staged; i++) {
            DbEntity *entity = stages[i].entity;
            ChunkedList *oldRows = (*entity).rows;
            (*entity).rows = stages[i].rows;
            (*entity).owned = 1u;
            stages[i].rows = nullptr;
            ChunkedList_free(oldRows);
            if (stages[i].indexRecords != nullptr) {
                // The image carried an index: adopt its definition and records,
                // which already point at the rows just published.
                indexDrop(entity);
                memcpy((*entity).indexField, stages[i].indexField,
                       sizeof((*entity).indexField));
                (*entity).indexOffset = stages[i].indexOffset;
                (*entity).indexDirty = 0u;
                (*entity).indexRecords = stages[i].indexRecords;
                stages[i].indexRecords = nullptr;
            } else {
                indexInvalidate(entity); // changed rows; the next query rebuilds
            }
        }
    }
    for (u32 i = 0u; i < staged; i++) {
        freeOwnedRows(stages[i].rows);
        if (stages[i].indexRecords != nullptr)
            ChunkedList_free(stages[i].indexRecords);
    }
    Memory_free(stages);
    return status;
}

/** Publishes page-chain bytes, then atomically replaces the root page. */
i32 Database_saveStore(Database *self, Store *store) {
    if (self == nullptr || store == nullptr || !Store_isOpen(store)) {
        recordError(self, DATABASE_INVALID, "saveStore: invalid database or closed store");
        THROW("Database_saveStore: invalid database or closed store");
        return DATABASE_INVALID;
    }
    if (!databaseStoreEnsureRoot(store)) {
        recordError(self, DATABASE_IO, "saveStore: root page admission failed");
        THROW("Database_saveStore: root page admission failed");
        return DATABASE_IO;
    }
    bytes length = 0u;
    u8 *bytes = databaseStoreCatalogBytes(self, &length);
    if (bytes == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "saveStore: catalog serialization failed");
        THROW("Database_saveStore: catalog serialization failed");
        return DATABASE_EXHAUSTED;
    }
    u32 pageSize = Store_pageSize(store);
    bytes chunkCapacity = (bytes) pageSize - PAGE_HEADER_BYTES - sizeof(DatabaseStoreLink);
    u32 nextPage = 0u;
    bytes end = length;
    bool ok = chunkCapacity > 0u;
    while (ok && end > 0u) {
        bytes start = end > chunkCapacity ? end - chunkCapacity : 0u;
        u32 chunkBytes = (u32) (end - start);
        bytes payloadLength = sizeof(DatabaseStoreLink) + chunkBytes;
        u8 *payload = (u8*) Memory_alloc(0u, payloadLength);
        if (payload == nullptr) {
            ok = false;
            break;
        }
        DatabaseStoreLink link = { nextPage, chunkBytes, start };
        memcpy(payload, &link, sizeof(link));
        memcpy(payload + sizeof(link), bytes + start, chunkBytes);
        u32 pageId = 0u;
        StoreCode added = Store_add(store, PAGE_KIND_LEAF, payload,
                                    (u32) payloadLength, &pageId);
        Memory_free(payload);
        if (added != STORE_OK) {
            ok = false;
            break;
        }
        nextPage = pageId;
        end = start;
    }
    if (!ok) {
        Memory_free(bytes);
        recordError(self, DATABASE_IO, "saveStore: data-page write failed before publication");
        THROW("Database_saveStore: data-page write failed before publication");
        return DATABASE_IO;
    }
    DatabaseStoreRoot root;
    memset(&root, 0, sizeof(root));
    memcpy(root.magic, DATABASE_STORE_ROOT_MAGIC, 8u);
    root.version = DATABASE_STORE_VERSION;
    root.firstPage = nextPage;
    root.length = length;
    root.crc = Crc32_finalize(Crc32_update(CRC32_SEED, bytes, length));
    bool published = Store_page_write(store, DATABASE_STORE_ROOT_ID, PAGE_KIND_CATALOG,
        (const u8*) &root, (u32) sizeof(root)) == STORE_OK;
    Memory_free(bytes);
    if (!published) {
        recordError(self, DATABASE_IO, "saveStore: root publication failed; reopen to resolve");
        THROW("Database_saveStore: root publication failed; reopen to resolve");
        return DATABASE_IO;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/** Loads the root-selected page chain, validates bytes, then stages rows atomically. */
i32 Database_loadStore(Database *self, Store *store) {
    if (self == nullptr || store == nullptr || !Store_isOpen(store)) {
        recordError(self, DATABASE_INVALID, "loadStore: invalid database or closed store");
        THROW("Database_loadStore: invalid database or closed store");
        return DATABASE_INVALID;
    }
    u32 pageSize = Store_pageSize(store);
    u8 *pageBytes = (u8*) Memory_alloc(0u, pageSize);
    if (pageBytes == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "loadStore: page buffer allocation failed");
        THROW("Database_loadStore: page buffer allocation failed");
        return DATABASE_EXHAUSTED;
    }
    DatabaseStoreRoot root;
    bool rootOk = databaseStoreReadRoot(store, pageBytes, &root);
    u32 pageCount = Store_pageCount(store);
    bytes chunkCapacity = (bytes) pageSize - PAGE_HEADER_BYTES - sizeof(DatabaseStoreLink);
    bytes maxCatalogBytes = pageCount > 1u ?
        (bytes) (pageCount - 1u) * chunkCapacity : 0u;
    if (!rootOk || root.length == 0u || (u64) (bytes) root.length != root.length ||
        root.length > maxCatalogBytes) {
        Memory_free(pageBytes);
        recordError(self, DATABASE_CORRUPT, "loadStore: root page is absent or invalid");
        THROW("Database_loadStore: root page is absent or invalid");
        return DATABASE_CORRUPT;
    }
    bytes length = (bytes) root.length;
    u8 *bytes = (u8*) Memory_alloc(0u, length);
    if (bytes == nullptr) {
        Memory_free(pageBytes);
        recordError(self, DATABASE_EXHAUSTED, "loadStore: snapshot buffer allocation failed");
        THROW("Database_loadStore: snapshot buffer allocation failed");
        return DATABASE_EXHAUSTED;
    }
    bytes offset = 0u;
    u32 pageId = root.firstPage;
    u32 visited = 0u;
    bool valid = pageId != 0u;
    while (valid && offset < length) {
        if (pageId == 0u || pageId > Store_pageCount(store) ||
            visited++ >= Store_pageCount(store)) {
            valid = false;
            break;
        }
        u32 used = 0u;
        u32 kind = 0u;
        bool truncated = false;
        StoreCode readCode = Store_page_read(store, pageId, pageBytes, pageSize,
            &used, &kind, &truncated);
        if (readCode != STORE_OK || truncated || kind != PAGE_KIND_LEAF ||
            used < sizeof(DatabaseStoreLink)) {
            valid = false;
            break;
        }
        DatabaseStoreLink link;
        memcpy(&link, pageBytes, sizeof(link));
        if (link.offset != offset || link.chunkBytes != used - sizeof(link) ||
            link.chunkBytes == 0u || link.chunkBytes > length - offset) {
            valid = false;
            break;
        }
        memcpy(bytes + offset, pageBytes + sizeof(link), link.chunkBytes);
        offset += link.chunkBytes;
        pageId = link.nextPage;
    }
    valid = valid && offset == length && pageId == 0u &&
        Crc32_finalize(Crc32_update(CRC32_SEED, bytes, length)) == root.crc;
    Memory_free(pageBytes);
    if (!valid) {
        Memory_free(bytes);
        recordError(self, DATABASE_CORRUPT, "loadStore: page chain or snapshot checksum rejected");
        THROW("Database_loadStore: page chain or snapshot checksum rejected");
        return DATABASE_CORRUPT;
    }
    i32 status = databaseStoreLoadCommit(self, bytes, length);
    Memory_free(bytes);
    if (status != DATABASE_OK) {
        recordError(self, status, "loadStore: registered schemas rejected snapshot");
        THROW("Database_loadStore: registered schemas rejected snapshot");
        return status;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/** Reads one validated persisted entity descriptor as an owned value record. */
DatabaseCatalogEntry Database_storeCatalogEntry(Store *store, u32 wantedIndex) {
    DatabaseCatalogEntry result;
    memset(&result, 0, sizeof(result));
    result.code = DATABASE_INVALID;
    if (store == nullptr || !Store_isOpen(store)) {
        THROW("Database_storeCatalogEntry: invalid or closed store");
        return result;
    }
    u32 pageSize = Store_pageSize(store);
    u32 pageCount = Store_pageCount(store);
    bytes chunkCapacity = (bytes) pageSize - PAGE_HEADER_BYTES -
        sizeof(DatabaseStoreLink);
    bytes maxBytes = pageCount > 1u ? (bytes) (pageCount - 1u) * chunkCapacity : 0u;
    u8 *pageBytes = (u8*) Memory_alloc(0u, pageSize);
    if (pageBytes == nullptr) {
        result.code = DATABASE_EXHAUSTED;
        THROW("Database_storeCatalogEntry: page buffer allocation failed");
        return result;
    }
    DatabaseStoreRoot root;
    bool rootValid = databaseStoreReadRoot(store, pageBytes, &root);
    if (!rootValid || root.length == 0u ||
        (u64) (bytes) root.length != root.length || root.length > maxBytes) {
        Memory_free(pageBytes);
        result.code = DATABASE_CORRUPT;
        THROW("Database_storeCatalogEntry: root page is absent or invalid");
        return result;
    }
    bytes length = (bytes) root.length;
    u8 *bytes = (u8*) Memory_alloc(0u, length);
    if (bytes == nullptr) {
        Memory_free(pageBytes);
        result.code = DATABASE_EXHAUSTED;
        THROW("Database_storeCatalogEntry: catalog allocation failed");
        return result;
    }
    bytes offset = 0u;
    u32 pageId = root.firstPage;
    u32 visited = 0u;
    bool valid = pageId != 0u;
    while (valid && offset < length) {
        if (pageId == 0u || pageId > pageCount || visited++ >= pageCount) {
            valid = false;
            break;
        }
        u32 used = 0u;
        u32 kind = 0u;
        bool truncated = false;
        StoreCode code = Store_page_read(store, pageId, pageBytes, pageSize,
            &used, &kind, &truncated);
        if (code != STORE_OK || truncated || kind != PAGE_KIND_LEAF ||
            used < sizeof(DatabaseStoreLink)) {
            valid = false;
            break;
        }
        DatabaseStoreLink link;
        memcpy(&link, pageBytes, sizeof(link));
        if (link.offset != offset || link.chunkBytes != used - sizeof(link) ||
            link.chunkBytes == 0u || link.chunkBytes > length - offset) {
            valid = false;
            break;
        }
        memcpy(bytes + offset, pageBytes + sizeof(link), link.chunkBytes);
        offset += link.chunkBytes;
        pageId = link.nextPage;
    }
    Memory_free(pageBytes);
    valid = valid && offset == length && pageId == 0u &&
        Crc32_finalize(Crc32_update(CRC32_SEED, bytes, length)) == root.crc;
    if (!valid || length < sizeof(DatabaseStoreCatalog)) {
        Memory_free(bytes);
        result.code = DATABASE_CORRUPT;
        THROW("Database_storeCatalogEntry: page chain or checksum rejected");
        return result;
    }
    DatabaseStoreCatalog catalog;
    memcpy(&catalog, bytes, sizeof(catalog));
    if (memcmp(catalog.magic, DATABASE_STORE_CATALOG_MAGIC,
            sizeof(catalog.magic)) != 0 || catalog.version != DATABASE_STORE_VERSION ||
        (bytes) catalog.entityCount > (length - sizeof(catalog)) /
            sizeof(DatabaseStoreEntity)) {
        Memory_free(bytes);
        result.code = DATABASE_CORRUPT;
        THROW("Database_storeCatalogEntry: catalog header rejected");
        return result;
    }
    bytes recordOffset = sizeof(catalog);
    bool found = false;
    for (u32 i = 0u; i < catalog.entityCount; i++) {
        if (recordOffset > length || length - recordOffset < sizeof(DatabaseStoreEntity)) {
            valid = false;
            break;
        }
        DatabaseStoreEntity record;
        memcpy(&record, bytes + recordOffset, sizeof(record));
        recordOffset += sizeof(record);
        if (record.stride == 0u ||
            memchr(record.name, '\0', sizeof(record.name)) == nullptr ||
            (bytes) record.rowCount > SIZE_MAX / record.stride) {
            valid = false;
            break;
        }
        bytes rowBytes = (bytes) record.rowCount * record.stride;
        if (rowBytes > length - recordOffset) {
            valid = false;
            break;
        }
        bytes indexBytes = (bytes) record.indexCount *
            sizeof(DatabaseStoreIndexRecord);
        if (record.indexCount != 0u &&
            (record.indexField[0] == '\0' || record.indexCount != record.rowCount ||
             record.indexOffset > record.stride ||
             sizeof(i32) > record.stride - record.indexOffset)) {
            valid = false;
            break;
        }
        if (indexBytes > length - recordOffset - rowBytes) {
            valid = false;
            break;
        }
        if (i == wantedIndex) {
            memcpy(result.name, record.name, sizeof(result.name));
            result.rowStride = record.stride;
            result.rowCount = record.rowCount;
            result.found = true;
            found = true;
        }
        recordOffset += rowBytes + indexBytes;
    }
    valid = valid && recordOffset == length;
    Memory_free(bytes);
    if (!valid) {
        memset(&result, 0, sizeof(result));
        result.code = DATABASE_CORRUPT;
        THROW("Database_storeCatalogEntry: catalog records rejected");
        return result;
    }
    if (!found) {
        memset(&result, 0, sizeof(result));
        result.code = DATABASE_ABSENT;
        THROW("Database_storeCatalogEntry: catalog index is absent");
        return result;
    }
    result.code = DATABASE_OK;
    return result;
}

// STRING PROJECTIONS (the toString Law)

/**
 * Formats a bounded value summary of the Database; null self formats as
 * "nullptr" and reports truncation through outTruncated when supplied.
 */
void Database_toString(const Database *self, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap, "Database(entities=%u)", Database_entityCount(self));
    if (written < 0 || (bytes) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}

/**
 * Formats the Database's own fields into a bounded structure summary; null
 * self formats as "nullptr" and reports truncation when possible.
 */
void Database_toStringStruct(const Database *self, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap, "Database { entities=0x%llx, entityCount=%u, chunkBytes=%u, lastCode=%d }",
                           (unsigned long long) (uintptr_t) (*self).entities, Database_entityCount(self),
                           (*self).chunkBytes, (*self).lastCode);
    if (written < 0 || (bytes) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}
