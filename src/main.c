// personal/darkbase/src/main.c — the R5 CONSUMER of the R3 store.
//
// The engine is a library: it registers reflection entities, binds caller-owned
// rows and persists them. Nothing consumed it until now, so it was an island.
// This host is the consumer side of the seam — it registers a schema, binds real
// rows, writes a `.vexdb`, then reopens that file in a fresh Database and reads
// the rows back. If the store cannot survive that round trip, this exits nonzero.
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "database/database.h"
#include "reflection/struct.h"

// A message is an ordinary C struct; its layout IS the schema (the Entity Model
// Law) — the store copies nothing and owns no row.
#define MESSAGE_ENTITY "message"
typedef struct Message {
    uintptr_t authorId;   // who wrote it
    uint64_t at;          // when
    char body[64];        // what
} Message;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
        }                                                                 \
    } while (0)

// The entity's schema: a reflection Struct named for the entity, sized to the
// row. Fields carry layout once they are needed; size is what the store copies.
static Struct *messageSchema(void) {
    Struct *schema = Struct(MESSAGE_ENTITY);
    if (schema == nullptr)
        return nullptr;
    Struct_setSize(schema, (uint32_t) sizeof(Message));
    return schema;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "darkbase.vexdb";

    // 1 — write a store
    Database *db = Database();
    CHECK(db != nullptr);
    Struct *schema = messageSchema();
    CHECK(schema != nullptr);
    if (db == nullptr || schema == nullptr)
        return 1;
    CHECK(Database_define(db, schema) >= 0);

    Message first = { .authorId = 1u, .at = 1u };
    (void) snprintf(first.body, sizeof first.body, "hello from the R5 consumer");
    Message second = { .authorId = 2u, .at = 2u };
    (void) snprintf(second.body, sizeof second.body, "and this one persisted too");
    CHECK(Database_insert(db, MESSAGE_ENTITY, &first) >= 0);
    CHECK(Database_insert(db, MESSAGE_ENTITY, &second) >= 0);
    CHECK(Database_count(db, MESSAGE_ENTITY) == 2u);
    CHECK(Database_save(db, path) == DATABASE_OK);
    Database_free(db);
    Struct_free(schema);

    // 2 — reopen it and read the rows back
    Database *reopened = Database();
    CHECK(reopened != nullptr);
    Struct *again = messageSchema();
    CHECK(again != nullptr);
    if (reopened == nullptr || again == nullptr)
        return 1;
    CHECK(Database_define(reopened, again) >= 0);
    CHECK(Database_load(reopened, path) == DATABASE_OK);
    CHECK(Database_count(reopened, MESSAGE_ENTITY) == 2u);
    const Message *row = Database_row(reopened, MESSAGE_ENTITY, 0u);
    CHECK(row != nullptr);
    if (row != nullptr)
        CHECK(strcmp(row->body, "hello from the R5 consumer") == 0);
    printf("darkbase consumer: %u row(s) round-tripped through %s\n",
           Database_count(reopened, MESSAGE_ENTITY), path);
    Database_free(reopened);
    Struct_free(again);

    if (failures != 0) {
        printf("darkbase consumer: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    printf("darkbase consumer: OK (%d checks)\n", checks);
    return 0;
}
