#ifndef DARKBASE_DATABASE_DATABASE_CATALOG_ENTRY_H
#define DARKBASE_DATABASE_DATABASE_CATALOG_ENTRY_H

#include <stdbool.h>
#include <stdint.h>

#define DB_ENTITY_NAME_BYTES 24u

// One reflected entity's persisted catalog metadata. This is a value record;
// it owns no Store, page, schema, or row memory.
typedef struct DatabaseCatalogEntry {
    int32_t code; // DATABASE_OK when found; otherwise the admission failure
    bool found;
    char name[DB_ENTITY_NAME_BYTES];
    uint32_t rowStride;
    uint32_t rowCount;
} DatabaseCatalogEntry;

#endif
