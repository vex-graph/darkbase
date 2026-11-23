/** darkbase-engine/test/database/database_test.c, owner test for database/database.c. */

#include "vex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "engine/oop/darkbase_type.h"
#include "database/database.h"
#include "nio/mem.h"
#include "engine/oop/darkbase_type.h"
#include "reflection/field.h"
#include "reflection/struct.h"
#include "store/store.h"
#include "test_support.h"
#include "database_test.h"

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: DatabaseTest (darkbase-engine/test/database/database_test.c)
 * LEVEL: L3, Module Code (headless verification harness)
 * ============================================================================
 * Owner test for the L2 Database switchboard: register a reflection Struct as
 * an entity, bind caller-owned live rows, look them up, and report cold
 * rejections (duplicate/absent/invalid) through the recorded code. Rows are
 * borrowed pointers; the test proves they are returned unchanged and that their
 * slots stay stable across growth.
 *
 * STRUCT FIELDS: none, procedural test harness.
 *
 * PRIVATE HELPER RECORD:
 *   DatabaseCrashProbe { u32 commits; } // completed redo-marker callbacks
 *
 * FUNCTION REGISTRY:
 *   - crashAtDatabaseRoot(context, phase) // child exits at root redo boundary
 *   - testPageStorePersistence() // catalog/row roundtrip, schema reject, refusal preservation
 *   - testPageStoreRecovery() // public Database recovery after child exit
 *   - testStructuredEqualityQuery() // raw equality, signed range cursors and the declared int32 index
 *   - testPersistedIndex() // persisted index roundtrip and same-stride schema drift
 *   - main() // all existing Database and persistence owners
 * CONTRACT: test-owned files are disposable; the recovery child is bounded by
 * the runner watchdog and exits without relying on normal process teardown.
 * ============================================================================
 */

static i32 g_fail = 0;
static i32 g_checks = 0;


;;GETTER
static inline u32 DatabaseCrashProbe_commits(const DatabaseCrashProbe *probe)
{
    return (*probe).commits;
}
;;SETTER
static inline fn DatabaseCrashProbe_setCommits(DatabaseCrashProbe *probe, u32 commits)
{
    (*probe).commits = commits;
}
;;GETTER
static inline u32 Row_id(const Row *r)
{
    return (*r).id;
}
;;GETTER
static inline i32 Row_health(const Row *r)
{
    return (*r).health;
}
;;SETTER
static inline fn Row_setHealth(Row *r, i32 health)
{
    (*r).health = health;
}

/** Terminates after the root redo marker is durable but before page apply. */
static fn crashAtDatabaseRoot(any context, u32 phase) {
    DatabaseCrashProbe *probe = (DatabaseCrashProbe*) context;
    if (phase == STORE_AFTER_LOG_SYNC) {
        u32 next = DatabaseCrashProbe_commits(probe) + 1u;
        DatabaseCrashProbe_setCommits(probe, next);
        if (next == 3u)
            _exit(77);
    }
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fail++;                                                     \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
        }                                                                 \
    } while (0)


/** Verifies database construction, type identity, null-safe access, and destruction. */
static fn testConstructorAndNull() {
    printf("[1] construction, identity, null-safety\n");

    Database *db = Database();
    CHECK(db != nullptr);
    if (!db)
        return;
    CHECK(Database_check(db, TYPE_DB_DATABASE_SINGLETON));
    CHECK(Database_kind(db) == TYPE_DB_DATABASE_SINGLETON);
    CHECK(!Database_check(db, TYPE_DB_DATABASE_RESULT_SINGLETON));
    CHECK(Database_entityCount(db) == 0u);
    CHECK(Database_errorCode(db) == DATABASE_OK);

    CHECK(Database_entityCount(nullptr) == 0u);
    CHECK(Database_errorCode(nullptr) == DATABASE_INVALID);
    CHECK(Database_entityIndex(nullptr, "x") == -1);
    CHECK(Database_schema(nullptr, "x") == nullptr);
    CHECK(Database_row(nullptr, "x", 0u) == nullptr);
    CHECK(Database_count(nullptr, "x") == 0u);
    CHECK(Database_lastError(nullptr, nullptr, 0u) == -1);

    Database_free(nullptr);
    Database_free(db);

    utf8 buf[64];
    bool truncated = true;
    Database_toString(nullptr, buf, sizeof(buf), &truncated);
    CHECK(truncated == false && strcmp(buf, "nullptr") == 0);
}

/** Verifies entity registration, case-folded lookup, duplicate rejection, and cleanup. */
static fn testDefineAndLookup() {
    printf("[2] define entities, index/count/schema lookups\n");

    Database *db = Database();
    Struct *player = Struct("player");
    Struct *item = Struct("item");
    CHECK(db && player && item);
    if (!db || !player || !item) {
        Database_free(db);
        Struct_free(player);
        Struct_free(item);
        return;
    }

    CHECK(Database_define(db, player) == 0);
    CHECK(Database_define(db, item) == 1);
    CHECK(Database_entityCount(db) == 2u);
    CHECK(Database_entityIndex(db, "player") == 0);
    CHECK(Database_entityIndex(db, "item") == 1);
    CHECK(Database_entityIndex(db, "Player") == 0);
    CHECK(Database_entityIndex(db, "absent") == -1);
    CHECK(Database_schema(db, "player") == player);
    CHECK(Database_schema(db, "absent") == nullptr);

    Struct *player2 = Struct("player");
    CHECK(player2 != nullptr);
    CHECK(Database_define(db, player2) == -1);
    CHECK(Database_errorCode(db) == DATABASE_DUPLICATE);
    Struct_free(player2);

    Struct_free(player);
    Struct_free(item);
    Database_free(db);
}

/** Verifies borrowed row insertion, stable indexed reads, growth, and rejected inserts. */
static fn testInsertAndRead() {
    printf("[3] bind live rows, read them back, stable across growth\n");

    Database *db = Database();
    Struct *player = Struct("player");
    CHECK(db && player);
    if (!db || !player) {
        Database_free(db);
        Struct_free(player);
        return;
    }
    CHECK(Database_define(db, player) == 0);

    Row rows[64];
    for (u32 i = 0u; i < 64u; i++) {
        rows[i].id = 100u + i;
        rows[i].health = (i32) i;
    }

    CHECK(Database_count(db, "player") == 0u);
    for (u32 i = 0u; i < 64u; i++)
        CHECK(Database_insert(db, "player", &rows[i]) == (i32) i);
    CHECK(Database_count(db, "player") == 64u);
    CHECK(Database_errorCode(db) == DATABASE_OK);

    any got = Database_row(db, "player", 37u);
    CHECK(got == (any) &rows[37]);
    CHECK((*((Row*) got)).id == 137u);
    CHECK(Database_row(db, "player", 64u) == nullptr);
    CHECK(Database_row(db, "player", 0u) == (any) &rows[0]);

    CHECK(Database_insert(db, "ghost", &rows[0]) == -1);
    CHECK(Database_errorCode(db) == DATABASE_ABSENT);
    CHECK(Database_insert(db, "player", nullptr) == -1);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);

    Database_free(db);
    Struct_free(player);
}

/** Verifies owned copy-in stride checks and the entity-wide ownership mode. */
static fn testInsertOwned() {
    printf("[4] owned row copy-in, exact stride, and per-entity ownership\n");

    Database *db = Database();
    Struct *player = Struct("player");
    CHECK(db && player);
    if (!db || !player) {
        Database_free(db);
        Struct_free(player);
        return;
    }
    Struct_setSize(player, (u32) sizeof(Row));
    CHECK(Database_define(db, player) == 0);

    Row borrowed = { 17u, 23 };
    CHECK(Database_insertOwned(db, "player", &borrowed, sizeof borrowed - 1u) == -1);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);
    CHECK(Database_count(db, "player") == 0u);
    u8 oversized[sizeof(Row) + 1u] = {0};
    CHECK(Database_insertOwned(db, "player", oversized, sizeof oversized) == -1);
    CHECK(Database_count(db, "player") == 0u);
    CHECK(Database_insert(db, "player", &borrowed) == 0);
    CHECK(Database_insertOwned(db, "player", &borrowed, sizeof borrowed) == -1);
    CHECK(Database_count(db, "player") == 1u);

    Database_free(db);
    Struct_free(player);

    db = Database();
    player = Struct("player");
    CHECK(db && player);
    if (!db || !player) {
        Database_free(db);
        Struct_free(player);
        return;
    }
    Struct_setSize(player, (u32) sizeof(Row));
    CHECK(Database_define(db, player) == 0);
    Row input = { 29u, 31 };
    CHECK(Database_insertOwned(db, "player", &input, sizeof input) == 0);
    input.health = 99;
    Row *stored = (Row*) Database_row(db, "player", 0u);
    CHECK(stored != nullptr && stored != &input);
    CHECK(stored != nullptr && Row_id(stored) == 29u && Row_health(stored) == 31);
    CHECK(Database_insert(db, "player", &borrowed) == -1);
    CHECK(Database_count(db, "player") == 1u);

    Database_free(db);
    Struct_free(player);
}

/** Verifies recorded database errors and bounded value/structure projections. */
static fn testDiagnostics() {
    printf("[4] recorded rejection is legible and bounded\n");

    Database *db = Database();
    CHECK(db != nullptr);
    if (!db)
        return;

    CHECK(Database_define(db, nullptr) == -1);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);

    utf8 buf[128];
    i32 len = Database_lastError(db, buf, sizeof(buf));
    CHECK(len > 0 && (bytes) len == strlen(buf));
    CHECK(strstr(buf, "define") != nullptr);

    utf8 tiny[4];
    i32 tinyLen = Database_lastError(db, tiny, sizeof(tiny));
    CHECK(tinyLen == 3 && tiny[3] == '\0');

    CHECK(strcmp(Database_errorText(DATABASE_INVALID), "invalid argument") == 0);
    CHECK(strcmp(Database_errorText(DATABASE_DUPLICATE), "duplicate entity") == 0);
    CHECK(strcmp(Database_errorText(999), "unknown") == 0);

    utf8 proj[96];
    bool truncated = true;
    Database_toStringStruct(db, proj, sizeof(proj), &truncated);
    CHECK(truncated == false && strstr(proj, "Database {") != nullptr);

    Database_free(db);
}

/** Verifies save/load round trips, loaded-row ownership, and corrupt-file rejection. */
static fn testPersistence() {
    printf("[5] .vexdb save/load round-trip, ownership, checksum, atomicity\n");

    const utf8 *tmp = getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp";
    utf8 path[256];
    snprintf(path, sizeof(path), "%sdarkbase_test_XXXXXX", tmp);
    i32 fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);

    Database *db = Database();
    Struct *player = Struct("player");
    CHECK(db && player);
    if (!db || !player) {
        remove(path);
        Database_free(db);
        Struct_free(player);
        return;
    }
    Struct_setSize(player, (u32) sizeof(Row));
    CHECK(Database_define(db, player) == 0);

    Row rows[4];
    for (u32 i = 0u; i < 4u; i++) {
        rows[i].id = 7u + i;
        rows[i].health = (i32) (i + 1u);
        CHECK(Database_insert(db, "player", &rows[i]) == (i32) i);
    }
    CHECK(Database_save(db, path) == DATABASE_OK);

    Database *db2 = Database();
    Struct *player2 = Struct("player");
    CHECK(db2 && player2);
    if (!db2 || !player2) {
        remove(path);
        Database_free(db2);
        Struct_free(player2);
        Database_free(db);
        Struct_free(player);
        return;
    }
    Struct_setSize(player2, (u32) sizeof(Row));
    CHECK(Database_define(db2, player2) == 0);
    CHECK(Database_load(db2, path) == DATABASE_OK);
    CHECK(Database_count(db2, "player") == 4u);
    for (u32 i = 0u; i < 4u; i++) {
        Row *got = (Row*) Database_row(db2, "player", i);
        CHECK(got != nullptr && Row_id(got) == 7u + i && Row_health(got) == (i32) (i + 1u));
    }

    CHECK(Database_insert(db2, "player", &rows[0]) == -1);
    CHECK(Database_errorCode(db2) == DATABASE_INVALID);

    Row appended = { 88u, 99 };
    CHECK(Database_insertOwned(db2, "player", &appended, sizeof appended) == 4);
    appended.health = 0;
    Row *owned = (Row*) Database_row(db2, "player", 4u);
    CHECK(owned != nullptr && owned != &appended && Row_health(owned) == 99);
    CHECK(Database_count(db2, "player") == 5u);

    CHECK(Database_load(db2, path) == DATABASE_DUPLICATE);
    CHECK(Database_count(db2, "player") == 5u);

    FILE *fp = fopen(path, "r+b.sh");
    CHECK(fp != nullptr);
    if (fp) {
        u8 last = 0u;
        fseek(fp, -1L, SEEK_END);
        if (fread(&last, 1u, 1u, fp) == 1u) {
            last ^= 0xFFu;
            fseek(fp, -1L, SEEK_END);
            fwrite(&last, 1u, 1u, fp);
        }
        fclose(fp);
    }
    Database *db3 = Database();
    Struct *player3 = Struct("player");
    CHECK(db3 && player3);
    if (db3 && player3) {
        Struct_setSize(player3, (u32) sizeof(Row));
        CHECK(Database_define(db3, player3) == 0);
        CHECK(Database_load(db3, path) == DATABASE_INVALID);
        CHECK(Database_count(db3, "player") == 0u);
        Database_free(db3);
    }
    Struct_free(player3);

    remove(path);
    Database_free(db2);
    Struct_free(player2);
    Database_free(db);
    Struct_free(player);
}

/** Verifies atomic save replacement and preservation of the previous file on rejection. */
static fn testSafeSave() {
    printf("[6] atomic save publication (temp write + fsync + rename)\n");

    const utf8 *tmp = getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp";
    utf8 path[256];
    snprintf(path, sizeof(path), "%sdarkbase_save_XXXXXX", tmp);
    i32 fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);

    utf8 tpath[300];
    snprintf(tpath, sizeof(tpath), "%s.%lld.tmp", path, (i64) getpid());

    Database *a = Database();
    Struct *sa = Struct("player");
    Struct_setSize(sa, (u32) sizeof(Row));
    CHECK(Database_define(a, sa) == 0);
    Row ra[4];
    for (u32 i = 0u; i < 4u; i++) {
        ra[i].id = 1u;
        ra[i].health = (i32) i;
        Database_insert(a, "player", &ra[i]);
    }
    CHECK(Database_save(a, path) == DATABASE_OK);
    CHECK(access(tpath, F_OK) != 0);

    Database *b = Database();
    Struct *sb = Struct("player");
    Struct_setSize(sb, (u32) sizeof(Row));
    CHECK(Database_define(b, sb) == 0);
    Row rb[6];
    for (u32 i = 0u; i < 6u; i++) {
        rb[i].id = 2u;
        rb[i].health = (i32) (10 + i);
        Database_insert(b, "player", &rb[i]);
    }
    CHECK(Database_save(b, path) == DATABASE_OK);
    CHECK(access(tpath, F_OK) != 0);

    Database *c = Database();
    Struct *sc = Struct("player");
    Struct_setSize(sc, (u32) sizeof(Row));
    Database_define(c, sc);
    CHECK(Database_load(c, path) == DATABASE_OK);
    CHECK(Database_count(c, "player") == 6u);
    CHECK((*((Row*) Database_row(c, "player", 0u))).id == 2u);

    Database *d = Database();
    Struct *sd = Struct("empty");
    Database_define(d, sd);
    CHECK(Database_save(d, path) == DATABASE_INVALID);
    CHECK(access(tpath, F_OK) != 0);

    Database *e = Database();
    Struct *se = Struct("player");
    Struct_setSize(se, (u32) sizeof(Row));
    Database_define(e, se);
    CHECK(Database_load(e, path) == DATABASE_OK);
    CHECK(Database_count(e, "player") == 6u);

    remove(path);
    Database_free(a);
    Struct_free(sa);
    Database_free(b);
    Struct_free(sb);
    Database_free(c);
    Struct_free(sc);
    Database_free(d);
    Struct_free(sd);
    Database_free(e);
    Struct_free(se);
}

/** Verifies reflection rows publish through Store and load after a real reopen. */
static fn testPageStorePersistence() {
    printf("[7] page-store database persistence and reopen\n");
    const utf8 *tmp = getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp/";
    utf8 path[256];
    snprintf(path, sizeof(path), "%sdarkbase_pages_XXXXXX", tmp);
    i32 fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    remove(path);

    Store *store = Store(path, 1024u * 1024u, 512u, 2048u);
    Database *db = Database();
    Struct *schema = Struct("player");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    if (store == nullptr || db == nullptr || schema == nullptr) {
        Store_free(store);
        Database_free(db);
        Struct_free(schema);
        remove(path);
        return;
    }
    Struct_setSize(schema, (u32) sizeof(Row));
    Field *idField = Field("id");
    CHECK(idField != nullptr);
    if (idField != nullptr) {
        Field_setOffset(idField, (u32) offsetof(Row, id));
        Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
        CHECK(Struct_add(schema, idField) == 0u);
        Field_free(idField);
    }
    CHECK(Database_define(db, schema) == 0);
    Row source[3] = {{41u, 51}, {42u, 52}, {43u, 53}};
    for (u32 i = 0u; i < 3u; i++)
        CHECK(Database_insertOwned(db, "player", &source[i], sizeof(Row)) == (i32) i);
    CHECK(Database_saveStore(db, store) == DATABASE_OK);
    CHECK(Store_pageCount(store) >= 2u);
    Row *changed = (Row*) Database_row(db, "player", 0u);
    CHECK(changed != nullptr);
    if (changed != nullptr)
        Row_setHealth(changed, 999);
    CHECK(Database_saveStore(db, store) == DATABASE_IO);
    Database_free(db);
    Struct_free(schema);
    Store_free(store);

    store = Store(path);
    db = Database();
    schema = Struct("player");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    Database *mismatch = Database();
    Struct *wrongSchema = Struct("player");
    CHECK(mismatch != nullptr && wrongSchema != nullptr);
    if (mismatch != nullptr && wrongSchema != nullptr && store != nullptr) {
        Struct_setSize(wrongSchema, (u32) sizeof(Row) + 1u);
        CHECK(Database_define(mismatch, wrongSchema) == 0);
        u8 priorBytes[sizeof(Row) + 1u] = {0u};
        Row priorRow = {71u, 81};
        memcpy(priorBytes, &priorRow, sizeof(priorRow));
        priorBytes[sizeof(Row)] = 0x5Au;
        CHECK(Database_insertOwned(mismatch, "player", priorBytes,
                                   sizeof(priorBytes)) == 0);
        CHECK(Database_loadStore(mismatch, store) == DATABASE_INVALID);
        CHECK(Database_count(mismatch, "player") == 1u);
        Row *preserved = (Row*) Database_row(mismatch, "player", 0u);
        CHECK(preserved != nullptr && Row_id(preserved) == priorRow.id &&
              Row_health(preserved) == priorRow.health);
        u8 *preservedBytes = (u8*) preserved;
        CHECK(preservedBytes != nullptr &&
              preservedBytes[sizeof(Row)] == 0x5Au);
    }
    Database_free(mismatch);
    Struct_free(wrongSchema);
    DatabaseCatalogEntry retainedEntry;
    memset(&retainedEntry, 0, sizeof(retainedEntry));
    if (store != nullptr && db != nullptr && schema != nullptr) {
        Struct_setSize(schema, (u32) sizeof(Row));
        idField = Field("id");
        CHECK(idField != nullptr);
        if (idField != nullptr) {
            Field_setOffset(idField, (u32) offsetof(Row, id));
            Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
            CHECK(Struct_add(schema, idField) == 0u);
            Field_free(idField);
        }
        CHECK(Database_define(db, schema) == 0);
        CHECK(Database_loadStore(db, store) == DATABASE_OK);
        CHECK(Database_count(db, "player") == 3u);
        DatabaseCatalogEntry catalogEntry = Database_storeCatalogEntry(store, 0u);
        retainedEntry = catalogEntry;
        CHECK(catalogEntry.code == DATABASE_OK && catalogEntry.found);
        CHECK(strcmp(catalogEntry.name, "player") == 0 &&
              catalogEntry.rowStride == sizeof(Row) && catalogEntry.rowCount == 3u);
        DatabaseCatalogEntry absentEntry = Database_storeCatalogEntry(store, 1u);
        CHECK(absentEntry.code == DATABASE_ABSENT && !absentEntry.found);
        for (u32 i = 0u; i < 3u; i++) {
            Row *row = (Row*) Database_row(db, "player", i);
            CHECK(row != nullptr && Row_id(row) == source[i].id &&
                  Row_health(row) == source[i].health);
        }
        pointer wanted = 42u;
        DatabaseResult *query = Database_queryEqual(db, "player", "id",
                                                     &wanted, sizeof(wanted));
        CHECK(query != nullptr && DatabaseResult_count(query) == 1u);
        any queriedRow = nullptr;
        CHECK(query != nullptr && DatabaseResult_next(query, &queriedRow));
        CHECK(queriedRow != nullptr && (*((Row*) queriedRow)).id == wanted);
        DatabaseResult_free(query);
        Database_free(db);
        Struct_free(schema);
    } else {
        Database_free(db);
        Struct_free(schema);
    }
    Store_free(store);
    CHECK(retainedEntry.code == DATABASE_OK && retainedEntry.found &&
          strcmp(retainedEntry.name, "player") == 0 &&
          retainedEntry.rowStride == sizeof(Row) && retainedEntry.rowCount == 3u);
    remove(path);
}

/** Proves the public Database image is recovered from a torn root-page commit. */
static fn testPageStoreRecovery() {
    printf("[8] page-store database root recovery after process exit\n");
    const utf8 *tmp = getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp/";
    utf8 path[256];
    snprintf(path, sizeof(path), "%sdarkbase_recovery_XXXXXX", tmp);
    i32 fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    remove(path);

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        Store *childStore = Store(path, 1024u * 1024u, 512u, 128u * 1024u);
        Database *childDb = Database();
        Struct *childSchema = Struct("player");
        if (childStore == nullptr || childDb == nullptr || childSchema == nullptr)
            _exit(80);
        Struct_setSize(childSchema, (u32) sizeof(Row));
        if (Database_define(childDb, childSchema) != 0)
            _exit(81);
        Row source = {91u, 101};
        if (Database_insertOwned(childDb, "player", &source, sizeof(source)) != 0)
            _exit(82);
        DatabaseCrashProbe probe = {0u};
        Store_setCommitObserver(childStore, crashAtDatabaseRoot, &probe);
        if (Database_saveStore(childDb, childStore) != DATABASE_OK)
            _exit(83);
        _exit(84);
    }
    if (child < 0) {
        remove(path);
        return;
    }
    i32 childStatus = 0;
    CHECK(waitpid(child, &childStatus, 0) == child);
    CHECK(WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 77);

    Store *store = Store(path);
    Database *db = Database();
    Struct *schema = Struct("player");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    if (store != nullptr && db != nullptr && schema != nullptr) {
        Struct_setSize(schema, (u32) sizeof(Row));
        CHECK(Database_define(db, schema) == 0);
        CHECK(Database_loadStore(db, store) == DATABASE_OK);
        CHECK(Database_count(db, "player") == 1u);
        Row *row = (Row*) Database_row(db, "player", 0u);
        CHECK(row != nullptr && Row_id(row) == 91u && Row_health(row) == 101);
    }
    Database_free(db);
    Struct_free(schema);
    Store_free(store);
    remove(path);
}

/** Verifies structured equality uses reflected byte spans and returns matching rows. */
static fn testStructuredEqualityQuery() {
    printf("[9] reflected raw-byte equality query and cursor\n");
    Database *db = Database();
    Struct *schema = Struct("player");
    Field *idField = Field("id");
    Field *healthField = Field("health");
    Field *badField = Field("bad");
    CHECK(db != nullptr && schema != nullptr && idField != nullptr &&
          healthField != nullptr && badField != nullptr);
    if (db == nullptr || schema == nullptr || idField == nullptr ||
        healthField == nullptr || badField == nullptr) {
        Database_free(db);
        Struct_free(schema);
        Field_free(idField);
        Field_free(healthField);
        Field_free(badField);
        return;
    }
    Struct_setSize(schema, (u32) sizeof(Row));
    Field_setOffset(idField, (u32) offsetof(Row, id));
    Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
    Field_setOffset(healthField, (u32) offsetof(Row, health));
    Field_setSize(healthField, (u32) sizeof((*((Row*) nullptr)).health));
    Field_setFlags(healthField, REFLECT_FIELD_INDEXED);
    Field_setOffset(badField, (u32) sizeof(Row) - 1u);
    Field_setSize(badField, 2u);
    CHECK(Struct_add(schema, idField) == 0u);
    CHECK(Struct_add(schema, healthField) == 1u);
    CHECK(Struct_add(schema, badField) == 2u);
    Field_free(idField);
    Field_free(healthField);
    Field_free(badField);
    CHECK(Database_define(db, schema) == 0);
    Row source[6] = {{5u, 11}, {7u, 13}, {5u, 17}, {9u, 19},
                     {11u, INT32_MIN}, {13u, INT32_MAX}};
    for (u32 i = 0u; i < 6u; i++)
        CHECK(Database_insertOwned(db, "player", &source[i], sizeof(Row)) == (i32) i);

    pointer wanted = 5u;
    DatabaseResult *matches = Database_queryEqual(db, "player", "id",
                                                   &wanted, sizeof(wanted));
    CHECK(matches != nullptr && DatabaseResult_count(matches) == 2u);
    if (matches != nullptr) {
        any first = nullptr;
        any second = nullptr;
        CHECK(DatabaseResult_next(matches, &first));
        CHECK(DatabaseResult_next(matches, &second));
        CHECK((*((Row*) first)).health == 11 && (*((Row*) second)).health == 17);
        CHECK(DatabaseResult_next(matches, &first) == false);
        DatabaseResult_free(matches);
    }
    pointer absent = 101u;
    matches = Database_queryEqual(db, "player", "id", &absent, sizeof(absent));
    CHECK(matches != nullptr && DatabaseResult_count(matches) == 0u);
    DatabaseResult_free(matches);
    CHECK(Database_queryEqual(db, "player", "id", &wanted, sizeof(u16)) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);
    CHECK(Database_queryEqual(db, "player", "bad", &wanted, sizeof(u32)) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);
    CHECK(Database_queryEqual(db, "player", "missing", &wanted, sizeof(wanted)) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_ABSENT);
    CHECK(Database_queryEqual(db, "ghost", "id", &wanted, sizeof(wanted)) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_ABSENT);
    CHECK(Database_queryEqual(db, "player", "id", nullptr, sizeof(wanted)) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);

    DatabaseResult *range = Database_queryInt32Range(db, "player", "health", 13, 17);
    CHECK(range != nullptr && DatabaseResult_count(range) == 2u);
    if (range != nullptr) {
        any low = nullptr;
        any high = nullptr;
        CHECK(DatabaseResult_next(range, &low) && (*((Row*) low)).health == 13);
        CHECK(DatabaseResult_next(range, &high) && (*((Row*) high)).health == 17);
        CHECK(!DatabaseResult_next(range, &low));
        DatabaseResult_free(range);
    }
    range = Database_queryInt32Range(db, "player", "health", INT32_MIN, INT32_MAX);
    CHECK(range != nullptr && DatabaseResult_count(range) == 6u);
    DatabaseResult_free(range);
    range = Database_queryInt32Range(db, "player", "health", INT32_MIN, INT32_MIN);
    CHECK(range != nullptr && DatabaseResult_count(range) == 1u);
    DatabaseResult_free(range);
    range = Database_queryInt32Range(db, "player", "health", INT32_MAX, INT32_MAX);
    CHECK(range != nullptr && DatabaseResult_count(range) == 1u);
    DatabaseResult_free(range);
    range = Database_queryInt32Range(db, "player", "health", 20, 30);
    CHECK(range != nullptr && DatabaseResult_count(range) == 0u);
    DatabaseResult_free(range);
    CHECK(Database_queryInt32Range(db, "player", "health", 18, 17) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);
    CHECK(Database_queryInt32Range(db, "player", "id", 0, 10) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);
    CHECK(Database_queryInt32Range(db, "player", "bad", 0, 10) == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_INVALID);

    CHECK(!Database_indexed(db, "player", "health"));
    CHECK(Database_indexBuild(db, "player", "health") == DATABASE_OK);
    CHECK(Database_indexed(db, "player", "health"));
    CHECK(!Database_indexed(db, "player", "id"));
    CHECK(!Database_indexed(db, "player", "absent"));
    range = Database_queryInt32Range(db, "player", "health", 13, 17);
    CHECK(range != nullptr && DatabaseResult_count(range) == 2u);
    if (range != nullptr) {
        any first = nullptr;
        any second = nullptr;
        CHECK(DatabaseResult_next(range, &first) && (*((Row*) first)).health == 13);
        CHECK(DatabaseResult_next(range, &second) && (*((Row*) second)).health == 17);
        CHECK(!DatabaseResult_next(range, &first));
        DatabaseResult_free(range);
    }
    range = Database_queryInt32Range(db, "player", "health", INT32_MIN, INT32_MAX);
    CHECK(range != nullptr && DatabaseResult_count(range) == 6u);
    DatabaseResult_free(range);
    Row extra = {15u, 14};
    CHECK(Database_insertOwned(db, "player", &extra, sizeof(Row)) == 6);
    range = Database_queryInt32Range(db, "player", "health", 13, 17);
    CHECK(range != nullptr && DatabaseResult_count(range) == 3u);
    if (range != nullptr) {
        any row = nullptr;
        CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 13);
        CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 14);
        CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 17);
        CHECK(!DatabaseResult_next(range, &row));
        DatabaseResult_free(range);
    }
    CHECK(Database_indexBuild(db, nullptr, "health") == DATABASE_INVALID);
    CHECK(Database_indexBuild(db, "ghost", "health") == DATABASE_ABSENT);
    CHECK(Database_indexBuild(db, "player", "nope") == DATABASE_ABSENT);
    CHECK(Database_indexBuild(db, "player", "id") == DATABASE_INVALID);
    CHECK(Database_indexed(db, "player", "health"));

    DatabaseFieldSpan span = Database_fieldSpan(db, "player", "health");
    CHECK(span.code == DATABASE_OK);
    CHECK(span.offset == (u32) offsetof(Row, health));
    CHECK(span.size == (u32) sizeof((*((Row*) nullptr)).health));
    span = Database_fieldSpan(db, "PLAYER", "Health");
    CHECK(span.code == DATABASE_OK && span.offset == (u32) offsetof(Row, health));
    span = Database_fieldSpan(db, "player", "id");
    CHECK(span.code == DATABASE_OK);
    any freshRow = Database_row(db, "player", 0u);
    CHECK(freshRow != nullptr);
    if (freshRow != nullptr && span.code == DATABASE_OK) {
        u32 id = 0u;
        memcpy(&id, (const u8*) freshRow + span.offset, sizeof(id));
        CHECK(id == 5u);
    }
    CHECK(Database_fieldSpan(db, "player", "bad").code == DATABASE_INVALID);
    CHECK(Database_fieldSpan(db, nullptr, "id").code == DATABASE_INVALID);
    CHECK(Database_fieldSpan(nullptr, "player", "id").code == DATABASE_INVALID);
    CHECK(Database_fieldSpan(db, "player", nullptr).code == DATABASE_INVALID);
    CHECK(Database_fieldSpan(db, "ghost", "id").code == DATABASE_ABSENT);
    CHECK(Database_fieldSpan(db, "player", "nope").code == DATABASE_ABSENT);
    CHECK(Database_indexed(db, "player", "health"));

    Database_free(db);
    Struct_free(schema);
}

/** Verifies the declared int32 index survives a save/reopen through the Store. */
static fn testPersistedIndex() {
    printf("[10] persisted int32 index roundtrip and schema drift\n");
    const utf8 *tmp = getenv("TMPDIR");
    if (tmp == nullptr)
        tmp = "/tmp/";
    utf8 path[256];
    snprintf(path, sizeof(path), "%sdarkbase_index_XXXXXX", tmp);
    i32 fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    remove(path);

    Store *store = Store(path, 1024u * 1024u, 512u, 2048u);
    Database *db = Database();
    Struct *schema = Struct("scores");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    if (store != nullptr && db != nullptr && schema != nullptr) {
        Struct_setSize(schema, (u32) sizeof(Row));
        Field *idField = Field("id");
        Field *healthField = Field("health");
        CHECK(idField != nullptr && healthField != nullptr);
        if (idField != nullptr && healthField != nullptr) {
            Field_setOffset(idField, (u32) offsetof(Row, id));
            Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
            Field_setOffset(healthField, (u32) offsetof(Row, health));
            Field_setSize(healthField, (u32) sizeof((*((Row*) nullptr)).health));
            Field_setFlags(healthField, REFLECT_FIELD_INDEXED);
            CHECK(Struct_add(schema, idField) == 0u);
            CHECK(Struct_add(schema, healthField) == 1u);
        }
        if (idField != nullptr)
            Field_free(idField);
        if (healthField != nullptr)
            Field_free(healthField);
        CHECK(Database_define(db, schema) == 0);
        Row source[4] = {{1u, 40}, {2u, 10}, {3u, 30}, {4u, 20}};
        for (u32 i = 0u; i < 4u; i++)
            CHECK(Database_insertOwned(db, "scores", &source[i], sizeof(Row)) == (i32) i);
        CHECK(Database_indexBuild(db, "scores", "health") == DATABASE_OK);
        CHECK(Database_saveStore(db, store) == DATABASE_OK);
    }
    Database_free(db);
    Struct_free(schema);
    Store_free(store);

    store = Store(path);
    db = Database();
    schema = Struct("scores");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    if (store != nullptr && db != nullptr && schema != nullptr) {
        Struct_setSize(schema, (u32) sizeof(Row));
        Field *idField = Field("id");
        Field *healthField = Field("health");
        CHECK(idField != nullptr && healthField != nullptr);
        if (idField != nullptr && healthField != nullptr) {
            Field_setOffset(idField, (u32) offsetof(Row, id));
            Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
            Field_setOffset(healthField, (u32) offsetof(Row, health));
            Field_setSize(healthField, (u32) sizeof((*((Row*) nullptr)).health));
            Field_setFlags(healthField, REFLECT_FIELD_INDEXED);
            CHECK(Struct_add(schema, idField) == 0u);
            CHECK(Struct_add(schema, healthField) == 1u);
        }
        if (idField != nullptr)
            Field_free(idField);
        if (healthField != nullptr)
            Field_free(healthField);
        CHECK(Database_define(db, schema) == 0);
        CHECK(Database_loadStore(db, store) == DATABASE_OK);
        CHECK(Database_count(db, "scores") == 4u);
        CHECK(Database_indexed(db, "scores", "health"));
        DatabaseResult *range = Database_queryInt32Range(db, "scores", "health", 20, 40);
        CHECK(range != nullptr && DatabaseResult_count(range) == 3u);
        if (range != nullptr) {
            any row = nullptr;
            CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 20);
            CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 30);
            CHECK(DatabaseResult_next(range, &row) && (*((Row*) row)).health == 40);
            CHECK(!DatabaseResult_next(range, &row));
            DatabaseResult_free(range);
        }
    }
    Database_free(db);
    Struct_free(schema);
    Store_free(store);

    store = Store(path);
    db = Database();
    schema = Struct("scores");
    CHECK(store != nullptr && Store_isOpen(store));
    CHECK(db != nullptr && schema != nullptr);
    if (store != nullptr && db != nullptr && schema != nullptr) {
        Struct_setSize(schema, (u32) sizeof(Row));
        Field *idField = Field("id");
        Field *movedField = Field("health");
        CHECK(idField != nullptr && movedField != nullptr);
        if (idField != nullptr && movedField != nullptr) {
            Field_setOffset(idField, (u32) offsetof(Row, id));
            Field_setSize(idField, (u32) sizeof((*((Row*) nullptr)).id));
            Field_setOffset(movedField, (u32) offsetof(Row, id));
            Field_setSize(movedField, (u32) sizeof(i32));
            Field_setFlags(movedField, REFLECT_FIELD_INDEXED);
            CHECK(Struct_add(schema, idField) == 0u);
            CHECK(Struct_add(schema, movedField) == 1u);
        }
        if (idField != nullptr)
            Field_free(idField);
        if (movedField != nullptr)
            Field_free(movedField);
        CHECK(Database_define(db, schema) == 0);
        CHECK(Database_loadStore(db, store) == DATABASE_CORRUPT);
        CHECK(Database_count(db, "scores") == 0u);
    }
    Database_free(db);
    Struct_free(schema);
    Store_free(store);
    remove(path);
}

i32 main() {
    printf("=== Database Test Suite ===\n\n");

    testConstructorAndNull();
    testDefineAndLookup();
    testInsertAndRead();
    testInsertOwned();
    testDiagnostics();
    testPersistence();
    testSafeSave();
    testPageStorePersistence();
    testPageStoreRecovery();
    testStructuredEqualityQuery();
    testPersistedIndex();

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail == 0)
        printf("=== Database Test Suite: ALL PASS ===\n");
    return g_fail == 0 ? 0 : 1;
}
