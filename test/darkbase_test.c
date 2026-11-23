/**
 * Consolidated automated owner test suite for vex/darkbase.
 * Usage: darkbase_test [database_result|database|page|crc32|wal|modifier|headless_protocol|service|store_adversarial|store]
 */

#include "vex.h"
#include "engine/oop/darkbase_type.h"
#include "database/database.h"
#include "database/database_result.h"
#include "nio/mem.h"
#include "reflection/struct.h"
#include "struct/chunked_list.h"
#include "test_support.h"
#include "test/database/database_test.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "reflection/field.h"
#include "store/store.h"
#include "page/page.h"
#include <assert.h>
#include "util/crc32.h"
#include "wal/wal.h"
#include "oop/modifier.h"
#include "protocol/headless_protocol.h"
#include "service/service.h"
#include "test/service/service_test.h"
#include "security/password.h"
#include "security/totp.h"
#include "time/datetime.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include "test/darkbase_test.h"

/** Suite: database_result (from database/database_result_test.c). */
/** darkbase-engine/test/database/database_result_test.c, owner test for database/database_result.c. */



/**
 * ============================================================================
 * MODULE: DatabaseResultTest (darkbase-engine/test/database/database_result_test.c)
 * LEVEL: L3, Module Code (headless verification harness)
 * ============================================================================
 * Owner test for the dest-last row cursor: a DatabaseResult borrows one
 * entity's row-pointer list and walks it without copying; next() writes into a
 * caller destination, rewind restarts, and the count is the construction
 * snapshot. Also proves the empty cursor and the null-source rejection.
 *
 * STRUCT FIELDS: none, procedural test harness.
 *
 * FUNCTION REGISTRY:
 *   - main()
 *   - testOwnedPointerList() // owns pointer-list container, borrows row values
 * ============================================================================
 */

static i32 s_database_result_g_fail = 0;
static i32 s_database_result_g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        s_database_result_g_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_database_result_g_fail++;                                                     \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
        }                                                                 \
    } while (0)


/** Verifies an empty cursor snapshots zero rows and handles null inputs safely. */
static fn testEmptyCursor() {
    printf("[1] empty cursor + null-safety\n");

    DatabaseResult *r = DatabaseResult();
    CHECK(r != nullptr);
    if (!r)
        return;
    CHECK(DatabaseResult_check(r, TYPE_DB_DATABASE_RESULT_SINGLETON));
    CHECK(DatabaseResult_count(r) == 0u);
    CHECK(DatabaseResult_position(r) == 0u);

    any row = (any) (pointer) 1u;
    CHECK(DatabaseResult_next(r, &row) == false);
    CHECK(row == (any) (pointer) 1u);
    CHECK(DatabaseResult_next(r, nullptr) == false);
    CHECK(DatabaseResult_next(nullptr, &row) == false);

    DatabaseResult_rewind(nullptr);
    CHECK(DatabaseResult_count(nullptr) == 0u);
    DatabaseResult_free(nullptr);

    CHECK(DatabaseResult_1(nullptr) == nullptr);

    DatabaseResult_free(r);
}

/** Verifies cursor iteration, exhaustion, rewind, and borrowed row identity. */
static fn testWalk() {
    printf("[2] walk an entity's rows dest-last, rewind, no copies\n");

    Database *db = Database();
    Struct *player = Struct("player");
    CHECK(db && player);
    if (!db || !player) {
        Database_free(db);
        Struct_free(player);
        return;
    }
    CHECK(Database_define(db, player) == 0);

    Row2 rows[8];
    for (u32 i = 0u; i < 8u; i++) {
        rows[i].v = i * 10u;
        CHECK(Database_insert(db, "player", &rows[i]) == (i32) i);
    }

    DatabaseResult *r = Database_select(db, "player");
    CHECK(r != nullptr);
    if (!r) {
        Database_free(db);
        Struct_free(player);
        return;
    }
    CHECK(DatabaseResult_count(r) == 8u);

    for (u32 i = 0u; i < 8u; i++) {
        any got = nullptr;
        CHECK(DatabaseResult_next(r, &got));
        CHECK(got == (any) &rows[i]);
        CHECK((*((Row2*) got)).v == i * 10u);
    }
    CHECK(DatabaseResult_position(r) == 8u);
    any past = (any) (pointer) 2u;
    CHECK(DatabaseResult_next(r, &past) == false);
    CHECK(past == (any) (pointer) 2u);

    DatabaseResult_rewind(r);
    CHECK(DatabaseResult_position(r) == 0u);
    any first = nullptr;
    CHECK(DatabaseResult_next(r, &first) && first == (any) &rows[0]);

    CHECK(Database_select(db, "ghost") == nullptr);
    CHECK(Database_errorCode(db) == DATABASE_ABSENT);

    DatabaseResult_free(r);
    Database_free(db);
    Struct_free(player);
}

/** Verifies an owned result list is released without taking row-value ownership. */
static fn testOwnedPointerList() {
    printf("[3] owned match-list container, borrowed row values\n");
    ChunkedList *rows = ChunkedList_3(ID_DB_DATABASE_ROW,
        (u32) sizeof(any), 256u);
    Row2 row = {77u};
    CHECK(rows != nullptr);
    if (rows == nullptr)
        return;
    u8 *slot = ChunkedList_addSlot(rows);
    CHECK(slot != nullptr);
    if (slot == nullptr) {
        ChunkedList_free(rows);
        return;
    }
    *((any*) slot) = &row;
    DatabaseResult *result = DatabaseResult_2(rows, true);
    CHECK(result != nullptr);
    if (result != nullptr) {
        any found = nullptr;
        CHECK(DatabaseResult_next(result, &found) && found == &row);
        DatabaseResult_free(result);
        CHECK(row.v == 77u);
    } else {
        ChunkedList_free(rows);
    }
}

/** Verifies cursor string projections and bounded-buffer behavior. */
static fn testStrings() {
    printf("[4] string projections\n");

    DatabaseResult *r = DatabaseResult();
    CHECK(r != nullptr);
    if (!r)
        return;
    utf8 buf[96];
    bool truncated = true;
    DatabaseResult_toString(r, buf, sizeof(buf), &truncated);
    CHECK(truncated == false && strcmp(buf, "DatabaseResult(0/0)") == 0);

    utf8 tiny[4];
    DatabaseResult_toString(r, tiny, sizeof(tiny), &truncated);
    CHECK(truncated == true);

    DatabaseResult_toString(nullptr, buf, sizeof(buf), &truncated);
    CHECK(truncated == false && strcmp(buf, "nullptr") == 0);

    DatabaseResult_free(r);
}

static i32 suite_database_result() {
    printf("=== DatabaseResult Test Suite ===\n\n");

    testEmptyCursor();
    testWalk();
    testOwnedPointerList();
    testStrings();

    printf("\n%d checks, %d failed\n", s_database_result_g_checks, s_database_result_g_fail);
    if (s_database_result_g_fail == 0)
        printf("=== DatabaseResult Test Suite: ALL PASS ===\n");
    return s_database_result_g_fail == 0 ? 0 : 1;
}
#undef CHECK

/** Suite: database (from database/database_test.c). */
/** darkbase-engine/test/database/database_test.c, owner test for database/database.c. */



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

static i32 s_database_g_fail = 0;
static i32 s_database_g_checks = 0;


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
        s_database_g_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_database_g_fail++;                                                     \
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

static i32 suite_database() {
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

    printf("\n%d checks, %d failed\n", s_database_g_checks, s_database_g_fail);
    if (s_database_g_fail == 0)
        printf("=== Database Test Suite: ALL PASS ===\n");
    return s_database_g_fail == 0 ? 0 : 1;
}
#undef CHECK

/** Suite: page (from page/page_test.c). */
/**
 * Owner: page/page.c, the page: layout, seals, capacity, and the corruption matrix.
 *
 * The point of this file is the last third: a page whose bytes changed in ANY of the
 * sealed fields must fail verification, including the page's own ID and the store's
 * page size. That is what makes a misplaced or mis-sized page detectable instead of
 * silently plausible. Storage is a stack buffer; the page owns nothing.
 */



#define SIZE PAGE_SIZE_MIN          /* 512: plenty, and fast */
static u8 storage[SIZE];

/* The raw header offsets the format defines; asserted as bytes, not through getters. */
static u32 rawU32(bytes offset) {
    return (u32) storage[offset] | ((u32) storage[offset + 1] << 8) |
           ((u32) storage[offset + 2] << 16) | ((u32) storage[offset + 3] << 24);
}

/* Each rejected admission preserves every cached field and every supplied byte.
 * Capture the cold diagnostic as well as its return; normal admission is quiet.
 * External serialization is the contract; racing writes and dead buffers are not. */
;;GETTER
static inline u8 *Page_bytes(const Page *page)
{
    return (*page).bytes;
}
;;SETTER
static inline fn Page_setUsed(Page *page, u32 used)
{
    (*page).used = used;
}
;;SETTER
static inline fn Page_setSize(Page *page, u32 size)
{
    (*page).size = size;
}

static fn checkAdmission(Page *page, PageCode expected, const utf8 *message) {
    Page before;
    u8 bytesBefore[SIZE];
    if (page != nullptr) {
        memcpy(&before, page, sizeof before);
        if (Page_bytes(page) != nullptr)
            memcpy(bytesBefore, Page_bytes(page), sizeof bytesBefore);
    }
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    assert(fflush(stderr) == 0);
    i32 saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    assert(Page_verify(page) == expected);
    assert(Page_load(page) == expected);
    assert(fflush(stderr) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    utf8 output[1024];
    bytes length = fread(output, 1u, sizeof output - 1u, capture);
    output[length] = '\0';
    assert(!ferror(capture));
    assert(fclose(capture) == 0);
    if (expected == PAGE_OK)
        assert(length == 0u);
    else {
        u32 lines = 0u;
        for (bytes i = 0u; i < length; ++i)
            if (output[i] == '\n')
                ++lines;
        assert(lines == 2u && strstr(output, "[vex] ") != nullptr);
        assert(strstr(output, message) != nullptr);
        if (page != nullptr)
            assert(memcmp(page, &before, sizeof before) == 0);
    }
    if (page != nullptr && Page_bytes(page) != nullptr)
        assert(memcmp(Page_bytes(page), bytesBefore, sizeof bytesBefore) == 0);
}

static fn restoredAdmission() {
    Page *writer = Page_2(storage, SIZE);
    assert(writer != nullptr);
    u8 payload[SIZE - PAGE_HEADER_BYTES];
    memset(payload, 0xA5, sizeof payload);
    const u32 lengths[] = {0u, 1u, 13u, sizeof payload};
    for (u32 kind = PAGE_KIND_FREE; kind <= PAGE_KIND_CATALOG; ++kind) {
        for (bytes i = 0u; i < sizeof lengths / sizeof lengths[0]; ++i) {
            assert(Page_format(writer, 91u, kind) == PAGE_OK);
            assert(Page_append(writer, payload, lengths[i]) == PAGE_OK);
            assert(Page_seal(writer) == PAGE_OK);
            u8 original[SIZE];
            memcpy(original, storage, sizeof original);
            Page *reader = Page_2(storage, SIZE);
            assert(reader != nullptr && memcmp(original, storage, SIZE) == 0);
            assert(Page_used(reader) == 0u && !Page_isSealed(reader));
            checkAdmission(reader, PAGE_OK, nullptr);
            Page_setUsed(reader, UINT32_MAX);
            Page snapshot;
            memcpy(&snapshot, reader, sizeof snapshot);
            assert(Page_verify(reader) == PAGE_OK);
            assert(memcmp(reader, &snapshot, sizeof snapshot) == 0);
            checkAdmission(reader, PAGE_OK, nullptr);
            assert(Page_id(reader) == 91u && Page_kind(reader) == kind);
            assert(Page_used(reader) == lengths[i] && Page_code(reader) == PAGE_OK);
            assert(Page_isSealed(reader) && !Page_isDirty(reader));
            assert(memcmp(Page_payloadOf(reader), payload, lengths[i]) == 0);
            utf8 text[256];
            bool truncated = true;
            assert(Page_toStringStruct(reader, text, sizeof text, &truncated));
            assert(!truncated && strstr(text, "id=91") != nullptr);
            assert(strstr(text, "sealed=1, dirty=0") != nullptr);
            assert(strstr(text, "lastCode=ok") != nullptr && strstr(text, "bytes=") != nullptr);
            checkAdmission(reader, PAGE_OK, nullptr);
            Page_free(reader);
        }
    }

    Page *reader = Page_2(storage, SIZE);
    assert(reader != nullptr);
    assert(Page_format(reader, 3u, PAGE_KIND_BRANCH) == PAGE_OK);
    assert(Page_append(reader, payload, 1u) == PAGE_OK);
    assert(Page_append(reader, nullptr, 1u) == PAGE_INVALID);
    assert(Page_format(writer, 91u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_append(writer, payload, 13u) == PAGE_OK);
    assert(Page_seal(writer) == PAGE_OK);
    u8 valid[SIZE];
    memcpy(valid, storage, sizeof valid);
    const struct { bytes offset; PageCode code; const utf8 *message; } cases[] = {
        {0u, PAGE_INVALID, "magic"},
        {4u, PAGE_BAD_SEAL, "seal"},
        {8u, PAGE_INVALID, "kind"},
        {16u, PAGE_INVALID, "size"},
        {20u, PAGE_INVALID, "reserved"},
        {24u, PAGE_INVALID, "reserved"},
        {PAGE_SEAL_OFFSET, PAGE_BAD_SEAL, "seal"},
        {PAGE_HEADER_BYTES, PAGE_BAD_SEAL, "seal"},
    };
    for (bytes i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        storage[cases[i].offset] ^= 0x5Au;
        checkAdmission(reader, cases[i].code, cases[i].message);
        memcpy(storage, valid, sizeof valid);
    }
    u32 invalidUsed[] = {SIZE - PAGE_HEADER_BYTES + 1u, UINT32_MAX};
    for (bytes i = 0u; i < sizeof invalidUsed / sizeof invalidUsed[0]; ++i) {
        memcpy(storage + 12u, &invalidUsed[i], sizeof invalidUsed[i]);
        checkAdmission(reader, PAGE_INVALID, "length");
        memcpy(storage, valid, sizeof valid);
    }
    u32 sizes[] = {0u, PAGE_HEADER_BYTES - 1u, SIZE - 1u, PAGE_SIZE_MAX + 1u};
    for (bytes i = 0u; i < sizeof sizes / sizeof sizes[0]; ++i) {
        Page_setSize(reader, sizes[i]);
        checkAdmission(reader, PAGE_INVALID, "storage");
    }
    Page_setSize(reader, SIZE);
    checkAdmission(reader, PAGE_OK, nullptr);
    assert(Page_id(reader) == 91u && Page_used(reader) == 13u);
    assert(Page_kind(reader) == PAGE_KIND_LEAF && !Page_isDirty(reader));
    Page_free(reader);
    Page_free(writer);
    checkAdmission(nullptr, PAGE_INVALID, "storage");
    Page *bare = Page_0();
    assert(bare != nullptr);
    checkAdmission(bare, PAGE_INVALID, "storage");
    Page_free(bare);
}

static i32 suite_page() {
    restoredAdmission();
    assert(Page_sizeValid(PAGE_SIZE_MIN) && Page_sizeValid(PAGE_SIZE_DEFAULT));
    assert(Page_sizeValid(1024u) && Page_sizeValid(PAGE_SIZE_MAX));
    assert(!Page_sizeValid(0u));
    assert(!Page_sizeValid(256u));
    assert(!Page_sizeValid(PAGE_SIZE_MAX + 1u));
    assert(!Page_sizeValid(300u));
    assert(!Page_sizeValid(1000u));

    Page *bare = Page_0();
    assert(bare != nullptr);
    assert(Page_size(bare) == 0u && Page_payload(bare) == nullptr);
    assert(Page_format(bare, 1u, PAGE_KIND_LEAF) == PAGE_INVALID);
    assert(Page_append(bare, (const u8*) "x", 1u) == PAGE_INVALID);
    assert(Page_seal(bare) == PAGE_INVALID);
    assert(Page_verify(bare) == PAGE_INVALID);
    Page_free(bare);

    assert(Page_2(nullptr, SIZE) == nullptr);
    assert(Page_2(storage, 300u) == nullptr);

    Page *page = Page_2(storage, SIZE);
    assert(page != nullptr);
    assert(Page_size(page) == SIZE && Page_capacity(page) == SIZE - PAGE_HEADER_BYTES);

    assert(Page_format(page, 7u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_code(page) == PAGE_OK);
    assert(Page_id(page) == 7u && Page_kind(page) == PAGE_KIND_LEAF);
    assert(Page_used(page) == 0u && Page_freeSpace(page) == Page_capacity(page));
    assert(!Page_isSealed(page) && Page_isDirty(page) && Page_sealValue(page) == 0u);
    assert(rawU32(0u) == PAGE_MAGIC);
    assert(rawU32(4u) == 7u);
    assert(rawU32(8u) == (u32) PAGE_KIND_LEAF);
    assert(rawU32(12u) == 0u);
    assert(rawU32(16u) == SIZE);
    assert(rawU32(PAGE_SEAL_OFFSET) == 0u);

    assert(Page_append(page, (const u8*) "alpha", 5u) == PAGE_OK);
    assert(Page_used(page) == 5u && Page_freeSpace(page) == Page_capacity(page) - 5u);
    assert(rawU32(12u) == 5u);
    assert(memcmp(Page_payloadOf(page), "alpha", 5u) == 0);
    assert(Page_append(page, nullptr, 0u) == PAGE_OK);
    assert(Page_used(page) == 5u);
    assert(Page_append(page, (const u8*) "x", 0u) == PAGE_OK);

    u8 filler[SIZE];
    memset(filler, 0x5A, sizeof filler);
    u32 remaining = Page_freeSpace(page);
    assert(Page_append(page, filler, remaining) == PAGE_OK);
    assert(Page_freeSpace(page) == 0u && Page_used(page) == Page_capacity(page));
    u8 before[8];
    memcpy(before, Page_payloadOf(page), sizeof before);
    assert(Page_append(page, (const u8*) "!", 1u) == PAGE_FULL);
    assert(Page_code(page) == PAGE_FULL);
    assert(Page_used(page) == Page_capacity(page));
    assert(memcmp(Page_payloadOf(page), before, sizeof before) == 0);
    assert(Page_append(page, nullptr, 1u) == PAGE_INVALID);

    assert(Page_seal(page) == PAGE_OK);
    assert(Page_isSealed(page) && !Page_isDirty(page));
    u32 seal = Page_sealValue(page);
    assert(seal != 0u && rawU32(PAGE_SEAL_OFFSET) == seal);
    assert(Page_verify(page) == PAGE_OK);
    assert(Page_append(page, (const u8*) "x", 1u) == PAGE_SEALED);
    assert(Page_reset(page, PAGE_KIND_BRANCH) == PAGE_OK);
    assert(Page_id(page) == 7u);
    assert(Page_kind(page) == PAGE_KIND_BRANCH);
    assert(Page_used(page) == 0u && !Page_isSealed(page));
    checkAdmission(page, PAGE_BAD_SEAL, "seal");

    assert(Page_format(page, 42u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_append(page, (const u8*) "payload-bytes", 13u) == PAGE_OK);
    assert(Page_seal(page) == PAGE_OK);
    assert(Page_verify(page) == PAGE_OK);

    struct { const utf8 *what; bytes offset; } cases[] = {
        { "a flipped payload byte", PAGE_HEADER_BYTES + 3u },
        { "the page id",            4u },
        { "the used length",        12u },
    };
    for (bytes i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        u8 saved = storage[cases[i].offset];
        storage[cases[i].offset] ^= 0x5Au;
        assert(Page_verify(page) == PAGE_BAD_SEAL);
        storage[cases[i].offset] = saved;
        assert(Page_verify(page) == PAGE_OK);
    }

    u8 savedMagic = storage[0];
    storage[0] ^= 0xFFu;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[0] = savedMagic;
    u8 savedSize = storage[16];
    storage[16] ^= 0x01u;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[16] = savedSize;
    assert(Page_verify(page) == PAGE_OK);

    u8 savedUsedLow = storage[12];
    u8 savedUsedHigh = storage[13];
    storage[12] = 0xFFu;
    storage[13] = 0xFFu;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[12] = savedUsedLow;
    storage[13] = savedUsedHigh;
    assert(Page_verify(page) == PAGE_OK);

    u8 zeros[SIZE];
    memset(zeros, 0, sizeof zeros);
    Page *foreign = Page_2(zeros, SIZE);
    assert(foreign != nullptr);
    assert(Page_verify(foreign) == PAGE_INVALID);
    Page_free(foreign);

    assert(strcmp(Page_codeName(PAGE_OK), "ok") == 0);
    assert(strcmp(Page_codeName(PAGE_INVALID), "invalid") == 0);
    assert(strcmp(Page_codeName(PAGE_BAD_SEAL), "bad-seal") == 0);
    assert(strcmp(Page_codeName(PAGE_FULL), "full") == 0);
    assert(strcmp(Page_codeName(PAGE_NOT_SEALED), "not-sealed") == 0);
    assert(strcmp(Page_codeName(PAGE_SEALED), "sealed") == 0);
    assert(strcmp(Page_codeName((PageCode) 99), "unknown") == 0);
    assert(strcmp(Page_kindName(PAGE_KIND_FREE), "free") == 0);
    assert(strcmp(Page_kindName(PAGE_KIND_LEAF), "leaf") == 0);
    assert(strcmp(Page_kindName(PAGE_KIND_BRANCH), "branch") == 0);
    assert(strcmp(Page_kindName(PAGE_KIND_OVERFLOW), "overflow") == 0);
    assert(strcmp(Page_kindName(PAGE_KIND_CATALOG), "catalog") == 0);
    assert(strcmp(Page_kindName(77u), "unknown") == 0);
    assert(Page_format(page, 1u, 99u) == PAGE_INVALID);
    assert(Page_codeName(Page_code(page)) != nullptr);

    utf8 text[256];
    bool truncated = true;
    assert(Page_toString(page, text, sizeof text, &truncated));
    assert(!truncated && strstr(text, "Page(id=") != nullptr);
    assert(Page_toStringStruct(page, text, sizeof text, &truncated));
    assert(strstr(text, "seal=") != nullptr && strstr(text, "kind=") != nullptr);
    utf8 tiny[4];
    truncated = false;
    assert(!Page_toString(page, tiny, sizeof tiny, &truncated));
    assert(truncated);
    assert(!Page_toStringStruct(page, nullptr, 0u, nullptr));
    assert(Page_toString(nullptr, text, sizeof text, nullptr));
    assert(strcmp(text, "nullptr") == 0);

    assert(Page_id(nullptr) == 0u && Page_kind(nullptr) == 0u && Page_size(nullptr) == 0u);
    assert(Page_payload(nullptr) == nullptr && Page_payloadOf(nullptr) == nullptr);
    assert(!Page_isSealed(nullptr) && !Page_isDirty(nullptr));
    assert(Page_code(nullptr) == PAGE_INVALID);
    Page_markClean(nullptr);
    Page_free(nullptr);

    assert(Page_format(page, 3u, PAGE_KIND_OVERFLOW) == PAGE_OK);
    assert(Page_isDirty(page));
    Page_markClean(page);
    assert(!Page_isDirty(page));

    Page_free(page);
    printf("page_test: PASS\n");
    return 0;
}
#undef SIZE

/** Suite: crc32 (from util/crc32_test.c). */
/**
 * Owner: darkbase-engine/src/util/crc32.c, reflected IEEE CRC32.
 * Proves the published check vector, incremental equivalence, and empty-span
 * identity; database and WAL owners cover its stored-format call sites.
 */



static i32 suite_crc32() {
    static const u8 vector[] = "123456789";
    assert(Crc32_of(vector, sizeof vector - 1u) == 0xCBF43926u);

    u32 crc = CRC32_SEED;
    crc = Crc32_update(crc, vector, 4u);
    crc = Crc32_update(crc, vector + 4u, sizeof vector - 1u - 4u);
    assert(Crc32_finalize(crc) == Crc32_of(vector, sizeof vector - 1u));
    assert(Crc32_of(nullptr, 0u) == 0u);
    assert(Crc32_finalize(Crc32_update(CRC32_SEED, nullptr, 0u)) == 0u);
    return 0;
}

/** Suite: wal (from wal/wal_test.c). */
/**
 * Owner: wal/wal.c, the write-ahead log: ordering, checksums, torn tails, recovery.
 *
 * The test writes a private log under TMPDIR and never touches a real store file.
 * It proves the three things the log exists for: a record is recoverable after a
 * clean sync, an incomplete TRAILING record is dropped rather than obeyed, and a
 * complete record with a bad checksum is reported as corruption rather than skipped.
 *
 * Region contract matrix: Wal_3 and Wal(...3) bind existing regular storage only;
 * pristine/header/bounds admission, exact fill/full/reopen epochs, once-only
 * counted refusal, replay/callback rejection with append-position preservation,
 * checkpoint refusal, every partial/full uncommitted body of a 37-byte record,
 * each torn leading-marker prefix (fail closed), committed length corruption, i16
 * region tail, corrupt committed payload, overflow and surrounding canaries.
 * A shorter retry after a discarded tail must survive reopen. Constructors 0/1
 * and existing standalone checkpoint/replay behavior are retained below.
 * Gaps: OS write/fsync failure injection, actual power-loss durability, hostile
 * aliases (excluded by contract), other platforms, and legal host concurrency
 * (all operations require external serialization, no internal concurrency claim).
 */



#define RECORDS 8u

static utf8 path[512];
static u32 seenOps[RECORDS];
static u64 seenSeq[RECORDS];
static u8 seenPayload[RECORDS][64];
static u32 seenLength[RECORDS];
static u32 seenCount = 0u;

;;GETTER
static inline u32 WalRecord_op(const WalRecord *r)
{
    return (*r).op;
}
;;GETTER
static inline u64 WalRecord_sequence(const WalRecord *r)
{
    return (*r).sequence;
}
;;GETTER
static inline u32 WalRecord_length(const WalRecord *r)
{
    return (*r).length;
}
;;GETTER
static inline const u8 *WalRecord_payload(const WalRecord *r)
{
    return (*r).payload;
}

static bool collect(any context, const WalRecord *record) {
    (discard) context;
    assert(seenCount < RECORDS);
    seenOps[seenCount] = WalRecord_op(record);
    seenSeq[seenCount] = WalRecord_sequence(record);
    u32 len = WalRecord_length(record);
    seenLength[seenCount] = len;
    if (len > 0u && len <= sizeof seenPayload[0])
        memcpy(seenPayload[seenCount], WalRecord_payload(record), len);
    ++seenCount;
    return true;
}

static u32 stopCalls = 0u;
static bool stopAfterOne(any context, const WalRecord *record) {
    (discard) context;
    assert(WalRecord_sequence(record) == 1u);
    ++stopCalls;
    return false;
}

static fn resetSeen()
{
    seenCount = 0u;
    memset(seenPayload, 0, sizeof seenPayload);
}

static fn makePath() {
    const utf8 *dir = getenv("TMPDIR");
    (discard) snprintf(path, sizeof path, "%s/re-wal-%d.log", dir != nullptr ? dir : "/tmp", (i32) getpid());
    (discard) unlink(path);
}

/* Overwrite bytes at an absolute offset without going through the log. */
static fn poke(bytes offset, const u8 *data, bytes length) {
    FILE *f = fopen(path, "r+b.sh");
    assert(f != nullptr);
    assert(fseek(f, (i64) offset, SEEK_SET) == 0);
    assert(fwrite(data, 1, length, f) == length);
    assert(fclose(f) == 0);
}

/* Append raw bytes at the end, as a crash mid-append would leave them. */
static fn appendRaw(const u8 *data, bytes length) {
    FILE *f = fopen(path, "ab");
    assert(f != nullptr);
    assert(fwrite(data, 1, length, f) == length);
    assert(fclose(f) == 0);
}

static i64 fileSize() {
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    assert(fseek(f, 0, SEEK_END) == 0);
    i64 size = ftell(f);
    (discard) fclose(f);
    return size;
}

#define REGION_OFFSET 128u
#define REGION_CAPACITY 256u
#define REGION_FILE_BYTES 512u

static fn regionFile() {
    FILE *f = fopen(path, "wb");
    assert(f != nullptr);
    u8 raw[REGION_FILE_BYTES] = {0};
    memset(raw, 0x5a, REGION_OFFSET);
    memset(raw + REGION_OFFSET + REGION_CAPACITY, 0x6b,
           REGION_FILE_BYTES - REGION_OFFSET - REGION_CAPACITY);
    assert(fwrite(raw, 1u, sizeof raw, f) == sizeof raw);
    assert(fclose(f) == 0);
}

static fn regionGuards() {
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    u8 raw[REGION_FILE_BYTES];
    assert(fread(raw, 1u, sizeof raw, f) == sizeof raw);
    assert(fclose(f) == 0);
    for (bytes i = 0u; i < REGION_OFFSET; ++i)
        assert(raw[i] == 0x5a);
    for (bytes i = REGION_OFFSET + REGION_CAPACITY; i < sizeof raw; ++i)
        assert(raw[i] == 0x6b);
    assert(fileSize() == REGION_FILE_BYTES);
}

static fn regionCases() {
    makePath();
    Wal *missing = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(missing != nullptr && !Wal_isOpen(missing) && Wal_code(missing) == WAL_IO);
    assert(access(path, F_OK) != 0);
    Wal_free(missing);
    assert(Wal_3(nullptr, 0u, REGION_CAPACITY) == nullptr);
    assert(Wal_3(path, UINT64_MAX, REGION_CAPACITY) == nullptr);
    assert(Wal_3(path, 0u, WAL_HEADER_BYTES - 1u) == nullptr);
    regionFile();
    Wal *outside = Wal_3(path, REGION_OFFSET, REGION_FILE_BYTES);
    assert(outside != nullptr && !Wal_isOpen(outside));
    Wal_free(outside);
    Wal *log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_isOpen(log) && Wal_code(log) == WAL_OK);
    assert(Wal_remaining(log) == REGION_CAPACITY - WAL_HEADER_BYTES);
    assert(Wal_exhaustionCount(log) == 0u);
    assert(Wal_append(log, WAL_OP_UPDATE, (const u8*) "alpha", 5u, nullptr) == WAL_OK);
    assert(Wal_sync(log) == WAL_OK);
    u64 remaining = Wal_remaining(log);
    u8 scratch[256];
    resetSeen();
    assert(Wal_replay(log, scratch, sizeof scratch, collect, nullptr) == WAL_OK);
    assert(seenCount == 1u && memcmp(seenPayload[0], "alpha", 5u) == 0);
    assert(Wal_replay(log, scratch, sizeof scratch, stopAfterOne, nullptr) == WAL_INVALID);
    assert(Wal_remaining(log) == remaining && Wal_sequence(log) == 1u);
    assert(Wal_checkpoint(log) == WAL_INVALID);
    assert(Wal_remaining(log) == remaining);
    u8 payload[REGION_CAPACITY] = {0};
    u64 seq = 99u;
    FILE *errors = tmpfile();
    assert(errors != nullptr);
    i32 saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(errors), STDERR_FILENO) >= 0);
    assert(Wal_append(log, WAL_OP_UPDATE, payload, sizeof payload, &seq) == WAL_FULL);
    assert(Wal_append(log, WAL_OP_UPDATE, payload, sizeof payload, &seq) == WAL_FULL);
    assert(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) >= 0);
    assert(close(saved) == 0);
    rewind(errors);
    utf8 diagnostic[512];
    bytes used = fread(diagnostic, 1u, sizeof diagnostic - 1u, errors);
    diagnostic[used] = '\0';
    assert(strstr(diagnostic, "region exhausted, request") != nullptr);
    utf8 *line = strchr(diagnostic, '\n');
    assert(line != nullptr && line[1] == '\0');
    assert(fclose(errors) == 0);
    assert(Wal_exhaustionCount(log) == 2u && seq == 99u);
    assert(Wal_remaining(log) == remaining && Wal_sequence(log) == 1u);
    assert(Wal_append(log, WAL_OP_UPDATE, payload,
                      (u32) (remaining - WAL_RECORD_FIXED - 4u - WAL_COMMIT_BYTES), nullptr) == WAL_OK);
    assert(Wal_remaining(log) == 0u && Wal_sync(log) == WAL_OK);
    Wal_free(log);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_isOpen(log) && Wal_count(log) == 2u && Wal_remaining(log) == 0u);
    assert(Wal_exhaustionCount(log) == 0u);
    assert(Wal_append(log, WAL_OP_UPDATE, nullptr, 0u, nullptr) == WAL_FULL);
    Wal_free(log);
    regionGuards();

    regionFile();
    log = Wal(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_append(log, WAL_OP_UPDATE, (const u8*) "alpha", 5u, nullptr) == WAL_OK);
    assert(Wal_sync(log) == WAL_OK);
    Wal_free(log);
    enum { RECORD_BYTES = WAL_RECORD_FIXED + 5u + 4u + WAL_COMMIT_BYTES };
    u8 record[RECORD_BYTES];
    FILE *f = fopen(path, "rb");
    assert(f != nullptr && fseek(f, REGION_OFFSET + WAL_HEADER_BYTES, SEEK_SET) == 0);
    assert(fread(record, 1u, sizeof record, f) == sizeof record && fclose(f) == 0);
    for (bytes cut = 1u; cut <= sizeof record - WAL_COMMIT_BYTES; ++cut) {
        regionFile();
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        Wal_free(log);
        poke(REGION_OFFSET + WAL_HEADER_BYTES + WAL_COMMIT_BYTES,
             record + WAL_COMMIT_BYTES, cut);
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        assert(Wal_isOpen(log) && Wal_code(log) == WAL_TRUNCATED);
        assert(Wal_count(log) == 0u && Wal_sequence(log) == 0u);
        resetSeen();
        assert(Wal_replay(log, scratch, sizeof scratch, collect, nullptr) == WAL_TRUNCATED);
        assert(seenCount == 0u);
        assert(Wal_append(log, WAL_OP_UPDATE, nullptr, 0u, &seq) == WAL_OK && seq == 1u);
        assert(Wal_sync(log) == WAL_OK);
        Wal_free(log);
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        assert(Wal_isOpen(log) && Wal_count(log) == 1u && Wal_sequence(log) == 1u);
        Wal_free(log);
        regionGuards();
    }
    for (bytes cut = 1u; cut < WAL_COMMIT_BYTES; ++cut) {
        regionFile();
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        Wal_free(log);
        poke(REGION_OFFSET + WAL_HEADER_BYTES + WAL_COMMIT_BYTES,
             record + WAL_COMMIT_BYTES, sizeof record - WAL_COMMIT_BYTES);
        poke(REGION_OFFSET + WAL_HEADER_BYTES, record, cut);
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        assert(!Wal_isOpen(log) && Wal_code(log) == WAL_CORRUPT);
        Wal_free(log);
        regionGuards();
    }
    regionFile();
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    Wal_free(log);
    record[WAL_COMMIT_BYTES + WAL_RECORD_FIXED] ^= 1u;
    poke(REGION_OFFSET + WAL_HEADER_BYTES, record, sizeof record);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_code(log) == WAL_CORRUPT && !Wal_isOpen(log));
    Wal_free(log);
    regionGuards();
    record[WAL_COMMIT_BYTES + WAL_RECORD_FIXED] ^= 1u;
    static const u32 corruptLengths[] = {UINT32_MAX, 0u, 1u, WAL_PAYLOAD_MAX + 1u};
    for (bytes i = 0u; i < sizeof corruptLengths / sizeof corruptLengths[0]; ++i) {
        regionFile();
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        Wal_free(log);
        poke(REGION_OFFSET + WAL_HEADER_BYTES, record, sizeof record);
        u32 wrongLength = corruptLengths[i];
        poke(REGION_OFFSET + WAL_HEADER_BYTES + WAL_COMMIT_BYTES + 16u,
             (const u8*) &wrongLength, sizeof wrongLength);
        log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
        assert(!Wal_isOpen(log) && Wal_code(log) == WAL_CORRUPT);
        Wal_free(log);
        regionGuards();
    }
    regionFile();
    poke(REGION_OFFSET + WAL_HEADER_BYTES, record, 7u);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_code(log) == WAL_CORRUPT && !Wal_isOpen(log));
    Wal_free(log);
    regionFile();
    log = Wal_3(path, REGION_OFFSET, WAL_HEADER_BYTES + 7u);
    assert(Wal_isOpen(log));
    Wal_free(log);
    poke(REGION_OFFSET + WAL_HEADER_BYTES, record, 7u);
    log = Wal_3(path, REGION_OFFSET, WAL_HEADER_BYTES + 7u);
    assert(!Wal_isOpen(log) && Wal_code(log) == WAL_CORRUPT);
    Wal_free(log);
    regionFile();
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    Wal_free(log);
    u8 header[WAL_HEADER_BYTES];
    f = fopen(path, "rb");
    assert(f != nullptr && fseek(f, REGION_OFFSET, SEEK_SET) == 0);
    assert(fread(header, 1u, sizeof header, f) == sizeof header && fclose(f) == 0);
    memset(header + 8u, 0xff, 8u);
    u32 crc = Crc32_of(header, 20u);
    memcpy(header + 20u, &crc, sizeof crc);
    poke(REGION_OFFSET, header, sizeof header);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_isOpen(log) && Wal_sequence(log) == UINT64_MAX);
    remaining = Wal_remaining(log);
    seq = 99u;
    assert(Wal_append(log, WAL_OP_UPDATE, nullptr, 0u, &seq) == WAL_INVALID);
    assert(seq == 99u && Wal_sequence(log) == UINT64_MAX && Wal_remaining(log) == remaining);
    Wal_free(log);
    header[6] = 1u;
    crc = Crc32_of(header, 20u);
    memcpy(header + 20u, &crc, sizeof crc);
    poke(REGION_OFFSET, header, sizeof header);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(!Wal_isOpen(log) && Wal_code(log) == WAL_CORRUPT);
    Wal_free(log);
    regionFile();
    const u8 bad = 1u;
    poke(REGION_OFFSET + WAL_HEADER_BYTES + 40u, &bad, 1u);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_code(log) == WAL_CORRUPT && !Wal_isOpen(log));
    Wal_free(log);
    assert(unlink(path) == 0);
}

static i32 suite_wal() {
    Wal *closed = Wal_0();
    assert(closed != nullptr && !Wal_isOpen(closed));
    assert(Wal_append(closed, WAL_OP_INSERT, (const u8*) "x", 1u, nullptr) == WAL_CLOSED);
    assert(Wal_replay(closed, nullptr, 0u, nullptr, nullptr) == WAL_CLOSED);
    assert(Wal_sync(closed) == WAL_CLOSED);
    assert(Wal_checkpoint(closed) == WAL_CLOSED);
    assert(Wal_sequence(closed) == 0u && Wal_count(closed) == 0u);
    Wal_free(closed);
    assert(Wal_1(nullptr) == nullptr);
    assert(!Wal_isOpen(nullptr) && Wal_sequence(nullptr) == 0u);
    Wal_free(nullptr);

    assert(strcmp(Wal_codeName(WAL_OK), "ok") == 0);
    assert(strcmp(Wal_codeName(WAL_INVALID), "invalid") == 0);
    assert(strcmp(Wal_codeName(WAL_IO), "io") == 0);
    assert(strcmp(Wal_codeName(WAL_CORRUPT), "corrupt") == 0);
    assert(strcmp(Wal_codeName(WAL_TRUNCATED), "truncated") == 0);
    assert(strcmp(Wal_codeName(WAL_CLOSED), "closed") == 0);
    assert(strcmp(Wal_codeName((WalCode) 77), "unknown") == 0);

    makePath();
    Wal *log = Wal_1(path);
    assert(log != nullptr && Wal_isOpen(log));
    assert(Wal_code(log) == WAL_OK);
    assert(strcmp(Wal_path(log), path) == 0);
    assert(Wal_sequence(log) == 0u && Wal_count(log) == 0u);
    assert(fileSize() == (i64) WAL_HEADER_BYTES);

    assert(Wal_append(log, WAL_OP_INSERT, (const u8*) "alpha", 5u, nullptr) == WAL_OK);
    assert(Wal_append(log, WAL_OP_UPDATE, (const u8*) "beta!", 5u, nullptr) == WAL_OK);
    u64 third = 0u;
    assert(Wal_append(log, WAL_OP_DELETE, (const u8*) "gamma", 5u, &third) == WAL_OK);
    assert(third == 3u);
    assert(Wal_sequence(log) == 3u && Wal_count(log) == 3u);

    assert(Wal_sync(log) == WAL_OK);
    i64 afterThree = fileSize();
    assert(afterThree == (i64) (WAL_HEADER_BYTES + 3u * (WAL_RECORD_FIXED + 5u + 4u)));

    assert(Wal_append(log, 0u, (const u8*) "x", 1u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, 99u, (const u8*) "x", 1u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, WAL_OP_INSERT, nullptr, 4u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, WAL_OP_INSERT, (const u8*) "x", WAL_PAYLOAD_MAX + 1u, nullptr) == WAL_INVALID);
    assert(fileSize() == afterThree);
    assert(Wal_code(log) == WAL_INVALID);
    assert(Wal_append(log, WAL_OP_INSERT, nullptr, 0u, nullptr) == WAL_OK);

    assert(Wal_sync(log) == WAL_OK);
    assert(Wal_code(log) == WAL_OK);
    Wal_free(log);

    Wal *reopened = Wal_1(path);
    assert(reopened != nullptr && Wal_isOpen(reopened));
    assert(Wal_sequence(reopened) == 4u && Wal_count(reopened) == 4u);
    assert(!Wal_tailTorn(reopened));

    u8 scratch[64];
    resetSeen();
    assert(Wal_replay(reopened, scratch, sizeof scratch, collect, nullptr) == WAL_OK);
    assert(seenCount == 4u && Wal_recovered(reopened) == 4u);
    assert(seenOps[0] == WAL_OP_INSERT && seenSeq[0] == 1u && seenLength[0] == 5u);
    assert(memcmp(seenPayload[0], "alpha", 5u) == 0);
    assert(seenOps[1] == WAL_OP_UPDATE && seenSeq[1] == 2u && memcmp(seenPayload[1], "beta!", 5u) == 0);
    assert(seenOps[2] == WAL_OP_DELETE && seenSeq[2] == 3u && memcmp(seenPayload[2], "gamma", 5u) == 0);
    assert(seenLength[3] == 0u);

    stopCalls = 0u;
    assert(Wal_replay(reopened, scratch, sizeof scratch, stopAfterOne, nullptr) == WAL_OK);
    assert(stopCalls == 1u);

    assert(Wal_replay(reopened, nullptr, 0u, collect, nullptr) == WAL_INVALID);
    assert(Wal_append(reopened, WAL_OP_INSERT, (const u8*) "0123456789", 10u, nullptr) == WAL_OK);
    u8 tiny[4];
    assert(Wal_replay(reopened, tiny, sizeof tiny, collect, nullptr) == WAL_INVALID);
    Wal_free(reopened);

    static const u8 torn[7] = { 'V', 'E', 'X', 'W', 1u, 0u, 0u };
    appendRaw(torn, sizeof torn);
    Wal *tornLog = Wal_1(path);
    assert(tornLog != nullptr);
    assert(Wal_tailTorn(tornLog));
    assert(Wal_sequence(tornLog) == 5u && Wal_count(tornLog) == 5u);
    assert(Wal_code(tornLog) == WAL_TRUNCATED);
    resetSeen();
    assert(Wal_replay(tornLog, scratch, sizeof scratch, collect, nullptr) == WAL_TRUNCATED);
    assert(seenCount == 5u);
    Wal_free(tornLog);

    assert(truncate(path, afterThree) == 0);
    Wal *clean = Wal_1(path);
    assert(clean != nullptr && !Wal_tailTorn(clean));
    assert(Wal_sequence(clean) == 3u && Wal_count(clean) == 3u);
    assert(Wal_sync(clean) == WAL_OK);
    assert(Wal_checkpoint(clean) == WAL_OK);
    assert(Wal_count(clean) == 0u && Wal_sequence(clean) == 3u && Wal_baseSequence(clean) == 3u);
    assert(fileSize() == (i64) WAL_HEADER_BYTES);
    assert(Wal_append(clean, WAL_OP_INSERT, (const u8*) "delta", 5u, &third) == WAL_OK);
    assert(third == 4u);
    assert(Wal_sync(clean) == WAL_OK);
    Wal_free(clean);

    Wal *afterCheckpoint = Wal_1(path);
    assert(afterCheckpoint != nullptr);
    assert(Wal_baseSequence(afterCheckpoint) == 3u && Wal_sequence(afterCheckpoint) == 4u);
    assert(Wal_count(afterCheckpoint) == 1u);
    resetSeen();
    assert(Wal_replay(afterCheckpoint, scratch, sizeof scratch, collect, nullptr) == WAL_OK);
    assert(seenCount == 1u && seenSeq[0] == 4u && memcmp(seenPayload[0], "delta", 5u) == 0);
    Wal_free(afterCheckpoint);

    assert(fileSize() > (i64) (WAL_HEADER_BYTES + WAL_RECORD_FIXED));
    u8 flipped[1];
    {
        FILE *f = fopen(path, "rb");
        assert(f != nullptr);
        assert(fseek(f, (i64) (WAL_HEADER_BYTES + WAL_RECORD_FIXED), SEEK_SET) == 0);
        assert(fread(flipped, 1, 1, f) == 1u);
        (discard) fclose(f);
    }
    flipped[0] ^= 0x5Au;
    poke(WAL_HEADER_BYTES + WAL_RECORD_FIXED, flipped, 1u);

    Wal *corrupt = Wal_1(path);
    assert(corrupt != nullptr);
    assert(Wal_code(corrupt) == WAL_CORRUPT);
    assert(Wal_replay(corrupt, scratch, sizeof scratch, collect, nullptr) == WAL_CORRUPT);
    Wal_free(corrupt);

    static const u8 badMagic[4] = { 0u, 0u, 0u, 0u };
    poke(0u, badMagic, sizeof badMagic);
    Wal *badHeader = Wal_1(path);
    assert(badHeader != nullptr);
    assert(Wal_code(badHeader) == WAL_CORRUPT);
    Wal_free(badHeader);

    makePath();
    Wal *final = Wal_1(path);
    assert(final != nullptr);
    utf8 text[96];
    bool truncated = true;
    assert(Wal_toString(final, text, sizeof text, &truncated));
    assert(!truncated && strstr(text, "Wal(") != nullptr);
    utf8 structText[1024];
    assert(Wal_toStringStruct(final, structText, sizeof structText, &truncated));
    assert(!truncated && strstr(structText, "path=") != nullptr);
    assert(strstr(structText, "baseSequence=") != nullptr && strstr(structText, "tailTorn=") != nullptr);
    utf8 tinyText[6];
    truncated = false;
    assert(!Wal_toString(final, tinyText, sizeof tinyText, &truncated));
    assert(truncated);
    assert(!Wal_toStringStruct(final, nullptr, 0u, nullptr));
    Wal_free(final);
    assert(unlink(path) == 0);

    regionCases();
    Memory_freeAll();
    printf("wal_test: PASS\n");
    return 0;
}
#undef RECORDS
#undef REGION_OFFSET
#undef REGION_CAPACITY
#undef REGION_FILE_BYTES

/** Suite: modifier (from oop/modifier_test.c). */
/**
 * Owner: oop/modifier.c, the frozen field-modifier keyword set.
 *
 * Proves the three axes validate, the 80 declaration presets and the i16
 * forms agree with the orthogonal constructor, the terminal parser accepts the
 * same words the C schema uses (free order, defaults by absence) and rejects
 * unknown/duplicate/overlong input with exactly one cold diagnostic, and both
 * projections round-trip. A Modifier is a value record: nothing is allocated,
 * so there is no lifetime to leak.
 */



/**
 * --- A captured cold diagnostic must be exactly one "[vex]" line. -----------
 * A function type is spelled `typedef Modifier Name();`, never
 * `Modifier (*Name)()`, because the constructor macro `Modifier(...)`
 * would otherwise expand on the `Modifier (` sequence.
 */
typedef Modifier ModifierProbe();

static bytes captureProbe(ModifierProbe probe, utf8 *out, bytes cap) {
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    assert(fflush(stderr) == 0);
    i32 saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    (discard) probe();
    assert(fflush(stderr) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    bytes n = fread(out, 1u, cap - 1u, capture);
    out[n] = '\0';
    fclose(capture);
    return n;
}

static bytes lineCount(const utf8 *text) {
    bytes lines = 0u;
    for (const utf8 *p = text; *p != '\0'; ++p)
        if (*p == '\n')
            ++lines;
    return lines;
}

static Modifier probeNull()
{
    return Modifier_parse(nullptr);
}
static Modifier probeUnknown()
{
    return Modifier_parse("bogus");
}
static Modifier probeDuplicate()
{
    return Modifier_parse("public public");
}
static Modifier probeOverlong() {
    return Modifier_parse("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
}
static Modifier probeApplyUnknown() {
    return Modifier_applyToken(Modifier_0(), "bogus");
}
static Modifier probeQuiet()
{
    return Modifier_parse("public hashed nullable");
}

static fn checkReject(ModifierProbe probe, ModifierCode expected, const utf8 *fragment) {
    utf8 text[512];
    bytes n = captureProbe(probe, text, sizeof text);
    assert(n > 0u);
    assert(lineCount(text) == 1u);
    assert(strncmp(text, "[vex] ", 6u) == 0);
    assert(strstr(text, fragment) != nullptr);
    (discard) expected;
}

static fn checkQuiet(ModifierProbe probe) {
    utf8 text[512];
    bytes n = captureProbe(probe, text, sizeof text);
    assert(n == 0u);
}

static i32 suite_modifier() {
    Modifier def = Modifier_0();
    assert(Modifier_equals(def, MODIFIER_DEFAULT));
    assert(Modifier_equals(Modifier(), def));
    assert(Modifier_isPrivate(def) && !Modifier_isPublic(def));
    assert(!Modifier_isProtected(def) && !Modifier_isTransient(def) && !Modifier_isShared(def));
    assert(Modifier_code(def) == MODIFIER_OK);

    Modifier three = Modifier(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED,
                              MODIFIER_NULLABLE);
    assert(Modifier_getAccess(three) == MODIFIER_ACCESS_PUBLIC);
    assert(Modifier_getProtection(three) == MODIFIER_PROTECTION_HASHED);
    assert(Modifier_getNullability(three) == MODIFIER_NULLABLE);

    Modifier five = Modifier(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED,
                             MODIFIER_NONNULL, true, true);
    assert(Modifier_getNullability(five) == MODIFIER_NONNULL);
    assert(Modifier_isTransient(five) && Modifier_isShared(five));
    assert(Modifier_isAdmissible(five));

    assert(Modifier_equals(MODIFIER_PUBLIC_HASHED, MODIFIER_PUBLIC_HASHED_NULLABLE));
    assert(Modifier_equals(MODIFIER_PUBLIC_HASHED, three));
    assert(Modifier_equals(MODIFIER_PUBLIC_SERVER_SEALED_NONNULL_TRANSIENT_STATIC, five));
    assert(Modifier_equals(MODIFIER_PRIVATE, MODIFIER_PRIVATE_PLAIN_NULLABLE));
    assert(Modifier_equals(MODIFIER_PUBLIC, MODIFIER_PUBLIC_PLAIN_NULLABLE));
    Modifier extremes = MODIFIER_PRIVATE_TRUNCATED_NONNULL_TRANSIENT_STATIC;
    assert(Modifier_isPrivate(extremes) && Modifier_isLossy(extremes));
    assert(Modifier_getNullability(extremes) == MODIFIER_NONNULL);
    assert(Modifier_isTransient(extremes) && Modifier_isShared(extremes));

    Modifier badAccess = def;
    badAccess.access = 9u;
    assert(Modifier_validate(badAccess) == MODIFIER_BAD_ACCESS);
    assert(!Modifier_isAdmissible(badAccess));
    Modifier badProtection = def;
    badProtection.protection = 9u;
    assert(Modifier_validate(badProtection) == MODIFIER_BAD_PROTECTION);
    Modifier badNullability = def;
    badNullability.nullability = 9u;
    assert(Modifier_validate(badNullability) == MODIFIER_BAD_NULLABILITY);
    assert(Modifier_validate(def) == MODIFIER_OK);

    assert(Modifier_isOneWay(MODIFIER_PUBLIC_HASHED));
    assert(Modifier_isReversible(MODIFIER_PRIVATE_CLIENT_SEALED));
    assert(Modifier_isReversible(MODIFIER_PRIVATE_SERVER_SEALED));
    assert(Modifier_isLossy(MODIFIER_PUBLIC_TRUNCATED));
    assert(Modifier_isProtected(MODIFIER_PUBLIC_TRUNCATED));
    assert(!Modifier_isProtected(def));
    assert(!Modifier_isOneWay(MODIFIER_PRIVATE_CLIENT_SEALED));

    assert(Modifier_warnings(MODIFIER_PUBLIC_HASHED) &
           (u32) MODIFIER_WARNING_PUBLIC_VERIFIER);
    assert(Modifier_warnings(MODIFIER_PRIVATE_HASHED_NULLABLE_TRANSIENT) &
           (u32) MODIFIER_WARNING_TRANSIENT_PROTECTED);
    assert(Modifier_warnings(MODIFIER_PUBLIC_PLAIN_NULLABLE_TRANSIENT_STATIC) &
           (u32) MODIFIER_WARNING_SHARED_TRANSIENT);
    assert(Modifier_warnings(MODIFIER_PRIVATE_TRUNCATED) &
           (u32) MODIFIER_WARNING_LOSSY);
    assert(Modifier_warnings(def) == MODIFIER_WARNING_NONE);

    assert(strcmp(Modifier_accessName(MODIFIER_ACCESS_PUBLIC), "public") == 0);
    assert(strcmp(Modifier_protectionName(MODIFIER_PROTECTION_CLIENT_SEALED),
                  "client-sealed") == 0);
    assert(strcmp(Modifier_nullabilityName(MODIFIER_NONNULL), "nonnull") == 0);
    assert(strcmp(Modifier_codeName(MODIFIER_DUPLICATE), "DUPLICATE") == 0);
    assert(strcmp(Modifier_warningName(MODIFIER_WARNING_LOSSY), "lossy") == 0);
    assert(strcmp(Modifier_accessName((ModifierAccess) 9), "unknown") == 0);

    assert(Modifier_equals(Modifier_parse(""), def));
    assert(Modifier_equals(Modifier_parse("   \t\r\n "), def));
    assert(Modifier_equals(Modifier_parse("public hashed nullable"), three));
    assert(Modifier_equals(Modifier_parse("nullable public hashed"), three));
    assert(Modifier_equals(Modifier_parse("public server-sealed nonnull transient static"), five));
    assert(Modifier_equals(Modifier_parse("private truncated nonnull transient static"), extremes));
    assert(Modifier_code(Modifier_parse("public hashed")) == MODIFIER_OK);

    Modifier bad = Modifier_parse("bogus");
    assert(Modifier_code(bad) == MODIFIER_BAD_TOKEN);
    assert(Modifier_equals(bad, def));
    assert(Modifier_code(Modifier_parse("public public")) == MODIFIER_DUPLICATE);
    assert(Modifier_code(Modifier_parse(nullptr)) == MODIFIER_INVALID);
    assert(Modifier_code(Modifier_applyToken(Modifier_0(), "bogus")) == MODIFIER_BAD_TOKEN);
    assert(Modifier_equals(Modifier_applyToken(Modifier_0(), "public"), MODIFIER_PUBLIC));

    checkReject(probeNull, MODIFIER_INVALID, "null modifier text");
    checkReject(probeUnknown, MODIFIER_BAD_TOKEN, "unknown modifier token");
    checkReject(probeDuplicate, MODIFIER_DUPLICATE, "named twice");
    checkReject(probeOverlong, MODIFIER_BAD_TOKEN, "over 31 bytes");
    checkReject(probeApplyUnknown, MODIFIER_BAD_TOKEN, "unknown modifier token");
    checkQuiet(probeQuiet);

    utf8 text[256];
    bool cut = true;
    assert(Modifier_toString(three, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "public:hashed:nullable") == 0);
    assert(Modifier_toString(five, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "public:server-sealed:nonnull:transient:static") == 0);
    assert(Modifier_toString(def, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "private:plain:nullable") == 0);
    assert(!Modifier_toString(five, text, 5u, &cut) && cut);
    assert(!Modifier_toString(five, nullptr, 0u, &cut) && cut);

    assert(Modifier_toStringStruct(five, text, sizeof text, &cut) && !cut);
    assert(strstr(text, "access=public") != nullptr);
    assert(strstr(text, "protection=server-sealed") != nullptr);
    assert(strstr(text, "nullability=nonnull") != nullptr);
    assert(strstr(text, "transient=1") != nullptr);
    assert(strstr(text, "shared=1") != nullptr);
    assert(strstr(text, "lastCode=OK") != nullptr);
    assert(!Modifier_toStringStruct(five, text, 4u, &cut) && cut);

    printf("modifier_test: OK\n");
    return 0;
}

/** Suite: headless_protocol (from protocol/headless_protocol_test.c). */
/**
 * HeadlessProtocol owner: byte-level wire oracle, adversarial framing and recovery.
 * No sockets or credentials; borrowed spans are live/disjoint, calls serialized.
 */


static u32 diagnostics;
#define REFUSES(expression) do { ++diagnostics; assert(!(expression)); } while (false)

/** Known byte oracle, all stream splits, retained tail, request/response and busy. */
static fn roundtrip() {
    const u8 payload[] = {0u, 255u, 's', 'e', 'c', 'r', 'e', 't'};
    const u8 golden[] = {20,0,0,0, 1,0, 1,0, 2,0, 0,0, 4,3,2,1,
                              0,255,'s','e','c','r','e','t'};
    u8 encoded[128], backing[128];
    bytes length = SIZE_MAX;
    bool truncated = true;
    assert(HeadlessProtocol_encode(HEADLESS_WIRE_REQUEST, 2u, HEADLESS_WIRE_OK,
        0x01020304u, payload, sizeof payload, encoded, sizeof encoded, &length, &truncated));
    assert(length == sizeof golden && !truncated && memcmp(encoded, golden, length) == 0);
    for (bytes split = 0u; split <= length; ++split) {
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        bytes used = SIZE_MAX;
        assert(HeadlessProtocol_feed(&protocol, encoded, split, &used) && used == split);
        if (split != length)
            assert(HeadlessProtocol_feed(&protocol, encoded + split, length - split, &used));
        assert(HeadlessProtocol_isReady(&protocol));
        assert(HeadlessProtocol_getCapacity(&protocol) == sizeof backing);
        assert(HeadlessProtocol_getUsed(&protocol) == length);
        assert(HeadlessProtocol_getExpected(&protocol) == length);
        assert(!HeadlessProtocol_isFailed(&protocol));
        assert(HeadlessProtocol_getKind(&protocol) == HEADLESS_WIRE_REQUEST);
        assert(HeadlessProtocol_getOperation(&protocol) == 2u);
        assert(HeadlessProtocol_getStatus(&protocol) == HEADLESS_WIRE_OK);
        assert(HeadlessProtocol_getRequestId(&protocol) == 0x01020304u);
        assert(HeadlessProtocol_getPayloadLength(&protocol) == sizeof payload);
        assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, sizeof payload) == 0);
        assert(HeadlessProtocol_finish(&protocol));
        REFUSES(HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(used == 0u && HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_BUSY);
        assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, sizeof payload) == 0);
        assert(HeadlessProtocol_release(&protocol));
        for (bytes i = 0u; i < length; ++i)
            assert(backing[i] == 0u);
        assert(HeadlessProtocol_getPayload(&protocol) == nullptr);
    }
    memcpy(encoded + length, golden, sizeof golden);
    HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
    bytes used = 0u;
    assert(HeadlessProtocol_feed(&protocol, encoded, length * 2u, &used));
    assert(used == length);
    assert(HeadlessProtocol_release(&protocol));
    assert(HeadlessProtocol_feed(&protocol, encoded + used, length, &used));
    assert(HeadlessProtocol_isReady(&protocol));
    HeadlessProtocol_reset(&protocol);
    for (u16 status = HEADLESS_WIRE_OK; status <= HEADLESS_WIRE_INTERNAL_ERROR; ++status) {
        assert(HeadlessProtocol_encode(HEADLESS_WIRE_RESPONSE, UINT16_MAX, status,
            UINT32_MAX, nullptr, 0u, encoded, sizeof encoded, &length, &truncated));
        for (bytes i = 0u; i < length; ++i) {
            assert(HeadlessProtocol_feed(&protocol, encoded + i, 1u, &used));
            assert(used == 1u);
        }
        assert(HeadlessProtocol_getStatus(&protocol) == status);
        assert(HeadlessProtocol_getOperation(&protocol) == UINT16_MAX);
        assert(HeadlessProtocol_getRequestId(&protocol) == UINT32_MAX);
        assert(HeadlessProtocol_getPayloadLength(&protocol) == 0u);
        assert(HeadlessProtocol_release(&protocol));
    }
}

/** Every truncation, each header constraint, explicit terminal latch and reset. */
static fn hostile() {
    u8 encoded[64], backing[64];
    bytes length, used;
    bool truncated;
    assert(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u,
                                   encoded, sizeof encoded, &length, &truncated));
    for (bytes cut = 1u; cut < length; ++cut) {
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        assert(HeadlessProtocol_feed(&protocol, encoded, cut, &used));
        REFUSES(HeadlessProtocol_finish(&protocol));
        assert(HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_TRUNCATED);
        assert(HeadlessProtocol_isFailed(&protocol));
        assert(!HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(used == 0u);
        HeadlessProtocol_reset(&protocol);
        assert(HeadlessProtocol_finish(&protocol));
        assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(HeadlessProtocol_isReady(&protocol));
    }
    const u32 offsets[] = {0u, 0u, 0u, 4u, 6u, 8u, 10u, 12u};
    const u8 values[] = {0u, 11u, 255u, 2u, 3u, 0u, 1u, 0u};
    for (bytes i = 0u; i < sizeof values; ++i) {
        u8 bad[64];
        memcpy(bad, encoded, length);
        bad[offsets[i]] = values[i];
        if (i == 2u)
            memset(bad, 255, 4u);
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        REFUSES(HeadlessProtocol_feed(&protocol, bad, length, &used));
        assert(HeadlessProtocol_isFailed(&protocol));
        assert(!HeadlessProtocol_isReady(&protocol));
        assert(HeadlessProtocol_getPayload(&protocol) == nullptr);
        if (i == 3u)
            assert(HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_VERSION);
    }
    HeadlessProtocol protocol = HeadlessProtocol(backing, HEADLESS_WIRE_HEADER_BYTES);
    encoded[0] = 13u;
    REFUSES(HeadlessProtocol_feed(&protocol, encoded, 4u, &used));
    assert(used == 4u && HeadlessProtocol_getExhaustionCount(&protocol) == 1u);
    assert(!HeadlessProtocol_feed(&protocol, encoded, 4u, &used));
    assert(used == 0u && HeadlessProtocol_getExhaustionCount(&protocol) == 2u);
    HeadlessProtocol_reset(&protocol);
    assert(HeadlessProtocol_getExhaustionCount(&protocol) == 0u);
    encoded[0] = 12u;
    assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
    assert(HeadlessProtocol_isReady(&protocol));
}

/** Exact protocol maximum with full payload comparison and canary; output atomicity. */
static fn boundaries() {
    static u8 payload[HEADLESS_WIRE_MAX_BYTES];
    static u8 encoded[HEADLESS_WIRE_MAX_BYTES + 1u];
    static u8 backing[HEADLESS_WIRE_MAX_BYTES + 1u];
    for (bytes i = 0u; i < sizeof payload; ++i)
        payload[i] = (u8) (i * 37u);
    bytes length;
    bool truncated;
    bytes maximumPayload = HEADLESS_WIRE_MAX_BYTES - HEADLESS_WIRE_HEADER_BYTES;
    memset(encoded, 0xa5, sizeof encoded);
    memset(backing, 0xa5, sizeof backing);
    assert(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload,
                                   encoded, HEADLESS_WIRE_MAX_BYTES, &length, &truncated));
    HeadlessProtocol protocol = HeadlessProtocol(backing, HEADLESS_WIRE_MAX_BYTES);
    bytes used;
    assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
    assert(used == HEADLESS_WIRE_MAX_BYTES && HeadlessProtocol_isReady(&protocol));
    assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, maximumPayload) == 0);
    assert(backing[HEADLESS_WIRE_MAX_BYTES] == 0xa5 && encoded[HEADLESS_WIRE_MAX_BYTES] == 0xa5);
    HeadlessProtocol_reset(&protocol);
    memset(encoded, 0xa5, sizeof encoded);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload,
        encoded, HEADLESS_WIRE_MAX_BYTES - 1u, &length, &truncated));
    assert(length == 0u && truncated);
    for (bytes i = 0u; i < sizeof encoded; ++i)
        assert(encoded[i] == 0xa5);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload + 1u,
                                   encoded, sizeof encoded, &length, &truncated));
    assert(!truncated && length == 0u);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, SIZE_MAX,
                                   encoded, sizeof encoded, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u,
                                   encoded, 0u, &length, &truncated));
    assert(truncated && length == 0u);
}

/** Null/defaults, API misuse preservation, every public constructor/projection. */
static fn defaults() {
    u8 backing[64];
    bytes used = SIZE_MAX, length;
    bool truncated;
    HeadlessProtocol protocol = HeadlessProtocol();
    HeadlessProtocol zero = HeadlessProtocol_zero();
    assert(!HeadlessProtocol_isReady(&zero));
    REFUSES(HeadlessProtocol_feed(&protocol, nullptr, 0u, &used));
    REFUSES(HeadlessProtocol_finish(&protocol));
    REFUSES(HeadlessProtocol_release(&protocol));
    REFUSES(HeadlessProtocol_feed(nullptr, nullptr, 0u, &used));
    REFUSES(HeadlessProtocol_finish(nullptr));
    REFUSES(HeadlessProtocol_release(nullptr));
    HeadlessProtocol_reset(nullptr);
    assert(HeadlessProtocol_getCode(nullptr) == HEADLESS_PROTOCOL_INVALID);
    assert(!HeadlessProtocol_isReady(nullptr) && !HeadlessProtocol_isFailed(nullptr));
    assert(HeadlessProtocol_getExhaustionCount(nullptr) == 0u);
    assert(HeadlessProtocol_getCapacity(nullptr) == 0u);
    assert(HeadlessProtocol_getUsed(nullptr) == 0u);
    assert(HeadlessProtocol_getExpected(nullptr) == 0u);
    assert(HeadlessProtocol_getKind(nullptr) == 0u);
    assert(HeadlessProtocol_getOperation(nullptr) == 0u);
    assert(HeadlessProtocol_getStatus(nullptr) == 0u);
    assert(HeadlessProtocol_getRequestId(nullptr) == 0u);
    assert(HeadlessProtocol_getPayload(nullptr) == nullptr);
    assert(HeadlessProtocol_getPayloadLength(nullptr) == 0u);
    ++diagnostics;
    protocol = HeadlessProtocol(nullptr, sizeof backing);
    assert(HeadlessProtocol_isFailed(&protocol));
    ++diagnostics;
    protocol = HeadlessProtocol(backing, HEADLESS_WIRE_HEADER_BYTES - 1u);
    assert(HeadlessProtocol_isFailed(&protocol));
    ++diagnostics;
    protocol = HeadlessProtocol(backing, SIZE_MAX);
    assert(HeadlessProtocol_isFailed(&protocol));
    protocol = HeadlessProtocol(backing, sizeof backing);
    REFUSES(HeadlessProtocol_feed(&protocol, nullptr, 1u, &used));
    REFUSES(HeadlessProtocol_feed(&protocol, backing, 0u, nullptr));
    assert(!HeadlessProtocol_isFailed(&protocol));
    assert(HeadlessProtocol_feed(&protocol, nullptr, 0u, &used) && used == 0u);
    REFUSES(HeadlessProtocol_encode(0u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 0u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 1u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(2u, 1u, 7u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 0u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 1u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, nullptr, 0u, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, nullptr, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, nullptr));
    utf8 text[256];
    assert(HeadlessProtocol_toString(nullptr, text, sizeof text, &truncated));
    assert(!truncated && strcmp(text, "nullptr") == 0);
    assert(HeadlessProtocol_toStringStruct(nullptr, text, sizeof text, &truncated));
    assert(HeadlessProtocol_toString(&protocol, text, sizeof text, &truncated));
    assert(HeadlessProtocol_toStringStruct(&protocol, text, sizeof text, &truncated));
    assert(strstr(text, "buffer:<redacted>") != nullptr);
    assert(!HeadlessProtocol_toStringStruct(&protocol, text, 1u, &truncated));
    assert(truncated && text[0] == '\0');
    assert(!HeadlessProtocol_toString(&protocol, nullptr, 0u, &truncated));
    assert(truncated);
    assert(HeadlessProtocol_toString(&protocol, text, sizeof text, nullptr));
}

static i32 suite_headless_protocol() {
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    i32 saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    roundtrip();
    hostile();
    boundaries();
    defaults();
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    u32 count = 0u;
    utf8 line[512];
    bool sawCapacity = false, sawVersion = false;
    while (fgets(line, sizeof line, capture) != nullptr) {
        assert(strstr(line, "[vex] ") == line);
        assert(strstr(line, "secret") == nullptr);
        sawCapacity |= strstr(line, "frame request 17 exceeds backing bound 16") != nullptr;
        sawVersion |= strstr(line, "rejected code 4") != nullptr;
        ++count;
    }
    fclose(capture);
    assert(count == diagnostics && sawCapacity && sawVersion);
    printf("HeadlessProtocol: all split/boundary/negative/recovery checks passed; %u expected diagnostics\n", count);
    return 0;
}
#undef REFUSES

/** Suite: service (from service/service_test.c). */
/** Owner test for the HeadlessService startup, admin gate and recovery contract. */



/**
 * ============================================================================
 * MODULE: HeadlessServiceTest (darkbase-headless/test/service/service_test.c)
 * CONTRACT: the R4 service owns the database lifetime; startup always closes
 * request admission. First boot emits one TOTP setup secret and creates the
 * one-time admin. Password + TOTP login is replay-protected, bootstrap password
 * replacement is mandatory, and explicit enable is persisted before admission.
 * STRUCT FIELDS: none, procedural owner harness.
 * FUNCTION REGISTRY: main, secretStore, secretLoad, secretRemove, check.
 * ============================================================================
 */


static u32 checks;
static u32 failures;

#define CHECK(value) do { \
    checks++; \
    if (!(value)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); \
    } \
} while (false)

;;GETTER
static inline const u8 *TestSecretContext_secret(const TestSecretContext *c)
{
    return (*c).secret;
}
;;GETTER
static inline u8 *TestSecretContext_secretMut(TestSecretContext *c)
{
    return (*c).secret;
}
;;GETTER
static inline bool TestSecretContext_present(const TestSecretContext *c)
{
    return (*c).present;
}
;;SETTER
static inline fn TestSecretContext_setPresent(TestSecretContext *c, bool present)
{
    (*c).present = present;
}

static bool secretStore(any opaqueContext, const utf8 *service,
                        const utf8 *account, const u8 *secret, bytes length) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (context == nullptr || service == nullptr || account == nullptr || secret == nullptr ||
        length != sizeof(((TestSecretContext) {0}).secret))
        return false;
    memcpy(TestSecretContext_secretMut(context), secret, length);
    TestSecretContext_setPresent(context, true);
    return true;
}

static bool secretLoad(any opaqueContext, const utf8 *service,
                       const utf8 *account, u8 *dest, bytes capacity,
                       bytes *outLength) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (outLength != nullptr)
        *outLength = 0u;
    if (context == nullptr || service == nullptr || account == nullptr || dest == nullptr ||
        outLength == nullptr || !TestSecretContext_present(context) ||
        capacity < sizeof(((TestSecretContext) {0}).secret))
        return false;
    memcpy(dest, TestSecretContext_secret(context), sizeof(((TestSecretContext) {0}).secret));
    *outLength = sizeof(((TestSecretContext) {0}).secret);
    return true;
}

static bool secretRemove(any opaqueContext, const utf8 *service,
                         const utf8 *account) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (context == nullptr || service == nullptr || account == nullptr)
        return false;
    memset(TestSecretContext_secretMut(context), 0, sizeof(((TestSecretContext) {0}).secret));
    TestSecretContext_setPresent(context, false);
    return true;
}

static u32 codeAt(const TestSecretContext *context, u64 seconds) {
    return Totp_code(TestSecretContext_secret(context), sizeof(((TestSecretContext) {0}).secret), seconds,
                     TOTP_DIGITS_DEFAULT, TOTP_STEP_SECONDS_DEFAULT);
}

static i32 suite_service() {
    CHECK(HeadlessService_getPasswordIterations(nullptr) == 0u);
    CHECK(!HeadlessService_setPasswordIterations(nullptr, PASSWORD_ITERATIONS_MIN));
    HeadlessService *emptyService = HeadlessService();
    CHECK(emptyService != nullptr);
    if (emptyService != nullptr) {
        CHECK(!HeadlessService_isAcceptingRequests(emptyService));
        CHECK(!HeadlessService_isRequestAllowed(emptyService));
        CHECK(HeadlessService_getPasswordIterations(emptyService) == PASSWORD_ITERATIONS_DEFAULT);
        utf8 projection[256];
        bool truncated = true;
        CHECK(HeadlessService_toString(nullptr, projection, sizeof projection, &truncated));
        CHECK(!truncated && strcmp(projection, "nullptr") == 0);
        CHECK(HeadlessService_toString(emptyService, projection, sizeof projection, &truncated));
        CHECK(!truncated && strstr(projection, "HeadlessService{closed}") != nullptr);
        CHECK(HeadlessService_toStringStruct(emptyService, projection, sizeof projection,
                                             &truncated));
        CHECK(!truncated && strstr(projection, "passwordIterations:") != nullptr);
        CHECK(!HeadlessService_toString(emptyService, projection, 1u, &truncated));
        CHECK(truncated);
        HeadlessService_free(emptyService);
    }
    CHECK(HeadlessService_1(nullptr) == nullptr);

    utf8 directory[] = "/tmp/darkbase-headless-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    if (failures != 0u)
        return EXIT_FAILURE;
    utf8 databasePath[256];
    i32 pathLength = snprintf(databasePath, sizeof databasePath, "%s/admin.vexdb", directory);
    CHECK(pathLength > 0 && (bytes) pathLength < sizeof databasePath);
    if (pathLength <= 0 || (bytes) pathLength >= sizeof databasePath)
        return EXIT_FAILURE;

    TestSecretContext secrets = {0};
    HeadlessService *service = HeadlessService(databasePath);
    CHECK(service != nullptr);
    if (service == nullptr)
        return EXIT_FAILURE;
    CHECK(HeadlessService_getPasswordIterations(service) == PASSWORD_ITERATIONS_DEFAULT);
    CHECK(!HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN - 1u));
    CHECK(HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN));
    CHECK(HeadlessService_getPasswordIterations(service) == PASSWORD_ITERATIONS_MIN);
    utf8 setupSecret[64];
    bytes setupLength = 0u;
    CHECK(!HeadlessService_start(service, nullptr, nullptr, nullptr, nullptr,
                                 setupSecret, sizeof setupSecret, &setupLength));
    CHECK(setupLength == 0u);
    CHECK(HeadlessService_start(service, secretStore, secretLoad, secretRemove,
                                &secrets, setupSecret, sizeof setupSecret,
                                &setupLength));
    CHECK(setupLength == 32u && setupSecret[setupLength] == '\0');
    CHECK(secrets.present);
    CHECK(!HeadlessService_isRequestAllowed(service));
    CHECK(!HeadlessService_setAcceptRequests(service, true));

    u64 firstTime = 600u;
    CHECK(HeadlessService_adminLoginAt(service, "not-admin", "Password123!",
                                       codeAt(&secrets, firstTime), firstTime) == false);
    CHECK(HeadlessService_adminLoginAt(service, "admin", "wrong-password",
                                       codeAt(&secrets, firstTime), firstTime) == false);
    CHECK(HeadlessService_adminLoginAt(service, "admin", "Password123!",
                                       codeAt(&secrets, firstTime), firstTime));
    CHECK(!HeadlessService_setAcceptRequests(service, true));

    u64 nextTime = 630u;
    CHECK(HeadlessService_changeAdminPasswordAt(service, "Password123!", "correct horse battery",
        codeAt(&secrets, nextTime), nextTime));
    CHECK(HeadlessService_setAcceptRequests(service, true));
    CHECK(HeadlessService_isRequestAllowed(service));
    CHECK(!HeadlessService_adminLoginAt(service, "admin", "correct horse battery",
        codeAt(&secrets, nextTime), nextTime));
    CHECK(HeadlessService_setAcceptRequests(service, false));
    CHECK(!HeadlessService_isRequestAllowed(service));
    HeadlessService_free(service);

    service = HeadlessService(databasePath);
    CHECK(service != nullptr);
    if (service != nullptr) {
        CHECK(HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN));
        setupLength = SIZE_MAX;
        CHECK(HeadlessService_start(service, secretStore, secretLoad, secretRemove,
                                    &secrets, setupSecret, sizeof setupSecret,
                                    &setupLength));
        CHECK(setupLength == 0u);
        CHECK(!HeadlessService_isRequestAllowed(service));
        CHECK(!HeadlessService_setAcceptRequests(service, true));
        u64 laterTime = 660u;
        CHECK(HeadlessService_adminLoginAt(service, "admin", "correct horse battery",
            codeAt(&secrets, laterTime), laterTime));
        CHECK(HeadlessService_setAcceptRequests(service, true));
        DateTime now = {0};
        DateTime_set(&now);
        i64 nowMillis = DateTime_epochMillis(&now);
        CHECK(nowMillis >= 0);
        if (nowMillis >= 0) {
            u64 nowSeconds = (u64) nowMillis / 1000u;
            CHECK(HeadlessService_adminLogin(service, "admin", "correct horse battery",
                                              codeAt(&secrets, nowSeconds)));
            CHECK(!HeadlessService_changeAdminPassword(service, "correct horse battery",
                "correct horse battery staple", codeAt(&secrets, nowSeconds)));
        }
        HeadlessService_free(service);
    }

    memset(&secrets, 0, sizeof secrets);
    unlink(databasePath);
    rmdir(directory);
    printf("HeadlessService: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
#undef CHECK

/** Suite: store_adversarial (from store/store_adversarial_test.c). */
/**
 * Store adversarial owner: hostile committed redo metadata, live page damage,
 * rejected read outputs, and bootstrap admission. Test-owned files only; no
 * concurrency, external attacker, hardware power-loss, or production readiness claim.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1


#define TEST_PAGE 512u
#define TEST_JOURNAL 65536u
#define TEST_RESERVATION 131072u

/**
 * MODULE MAP:
 * s_store_adversarial_fixture, unique files in one private temporary s_store_adversarial_directory
 * writeAt/readAt, exact bounded edits to test-owned database bytes
 * committedWalLengthMutation, published marker plus altered record length rejects
 * livePageDamageAndRecovery, public read rejects damage without publishing outputs,
 * then reopen replays the retained redo image
 * rejectedReadPreservesOutputs, nullable parameters and invalid IDs are atomic
 * malformedBootstrapAdmission, damaged seal and resealed unsupported version reject
 * main, runs each independent cold-admission scenario and removes all fixtures
 */

static utf8 s_store_adversarial_directory[1024];
static u32 s_store_adversarial_fixtureId;

static fn s_store_adversarial_fixture(utf8 path[1024]) {
    i32 n = snprintf(path, 1024u, "%s/store-adversarial-%u.vexdb", s_store_adversarial_directory, ++s_store_adversarial_fixtureId);
    assert(n > 0 && n < 1024);
}

static fn writeAt(const utf8 *path, i64 offset, const_any data, bytes length) {
    FILE *file = fopen(path, "r+b.sh");
    assert(file != nullptr);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fwrite(data, 1u, length, file) == length);
    assert(fflush(file) == 0);
    assert(fclose(file) == 0);
}

static fn readAt(const utf8 *path, i64 offset, any data, bytes length) {
    FILE *file = fopen(path, "rb");
    assert(file != nullptr);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fread(data, 1u, length, file) == length);
    assert(fclose(file) == 0);
}

static Store *create(const utf8 *path) {
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr && Store_isOpen(store));
    return store;
}

static fn committedWalLengthMutation() {
    utf8 path[1024];
    s_store_adversarial_fixture(path);
    Store *store = create(path);
    u32 id = 0u;
    const u8 payload[] = "committed record";
    assert(Store_add(store, PAGE_KIND_LEAF, payload, sizeof payload, &id) == STORE_OK);
    assert(id == 1u);
    Store_free(store);

    const i64 lengthOffset = (i64) TEST_PAGE + (i64) WAL_HEADER_BYTES +
                              (i64) WAL_COMMIT_BYTES + 16L;
    u32 recordedLength = 0u;
    readAt(path, lengthOffset, &recordedLength, sizeof recordedLength);
    assert(recordedLength == STORE_REDO_BYTES + TEST_PAGE);
    --recordedLength;
    writeAt(path, lengthOffset, &recordedLength, sizeof recordedLength);

    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

static fn livePageDamageAndRecovery() {
    utf8 path[1024];
    s_store_adversarial_fixture(path);
    Store *store = create(path);
    const u8 original[] = "redo repairs the damaged page";
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, original, sizeof original, &id) == STORE_OK);

    i64 dataOffset = (i64) TEST_PAGE + (i64) TEST_JOURNAL + (i64) PAGE_HEADER_BYTES;
    u8 damaged = (u8) (original[0] ^ 0x5Au);
    writeAt(path, dataOffset, &damaged, sizeof damaged);

    u8 destination[64];
    memset(destination, 0xA5, sizeof destination);
    u32 length = 0x12345678u;
    u32 kind = 0x87654321u;
    bool truncated = true;
    assert(Store_page_read(store, id, destination, sizeof destination, &length, &kind,
                           &truncated) == STORE_CORRUPT);
    for (bytes i = 0u; i < sizeof destination; ++i)
        assert(destination[i] == 0xA5u);
    assert(length == 0x12345678u && kind == 0x87654321u && truncated);
    Store_free(store);

    store = Store_1(path);
    assert(store != nullptr && Store_sequence(store) == 1u && Store_pageCount(store) == 1u);
    memset(destination, 0xA5, sizeof destination);
    length = 0u;
    kind = 0u;
    truncated = true;
    assert(Store_page_read(store, id, destination, sizeof destination, &length, &kind,
                           &truncated) == STORE_OK);
    assert(!truncated && length == sizeof original && kind == PAGE_KIND_LEAF);
    assert(memcmp(destination, original, sizeof original) == 0);
    assert(destination[sizeof original] == 0xA5u);
    Store_free(store);
    assert(unlink(path) == 0);
}

static fn rejectedReadPreservesOutputs() {
    utf8 path[1024];
    s_store_adversarial_fixture(path);
    Store *store = create(path);
    const u8 payload[] = "out-parameters";
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, payload, sizeof payload, &id) == STORE_OK);

    u8 destination[32];
    memset(destination, 0xC3, sizeof destination);
    u32 length = 0x11223344u;
    u32 kind = 0x55667788u;
    bool truncated = true;

    assert(Store_page_read(store, id, nullptr, sizeof destination, &length, &kind,
                           &truncated) == STORE_INVALID);
    assert(length == 0x11223344u && kind == 0x55667788u && truncated);
    assert(Store_page_read(store, id, destination, sizeof destination, nullptr, &kind,
                           &truncated) == STORE_INVALID);
    assert(kind == 0x55667788u && truncated);
    assert(Store_page_read(store, id, destination, sizeof destination, &length, nullptr,
                           &truncated) == STORE_INVALID);
    assert(length == 0x11223344u && truncated);
    assert(Store_page_read(store, id, destination, sizeof destination, &length, &kind,
                           nullptr) == STORE_INVALID);
    assert(length == 0x11223344u && kind == 0x55667788u);

    const u32 invalidIds[] = {0u, 2u};
    for (bytes index = 0u; index < sizeof invalidIds / sizeof invalidIds[0]; ++index) {
        memset(destination, 0xC3, sizeof destination);
        length = 0x11223344u;
        kind = 0x55667788u;
        truncated = true;
        assert(Store_page_read(store, invalidIds[index], destination, sizeof destination,
                               &length, &kind, &truncated) == STORE_INVALID);
        for (bytes i = 0u; i < sizeof destination; ++i)
            assert(destination[i] == 0xC3u);
        assert(length == 0x11223344u && kind == 0x55667788u && truncated);
    }

    Store_free(store);
    assert(unlink(path) == 0);
}

static fn malformedBootstrapAdmission() {
    utf8 path[1024];
    s_store_adversarial_fixture(path);
    Store *store = create(path);
    Store_free(store);

    u8 nonzero = 1u;
    writeAt(path, (i64) PAGE_HEADER_BYTES + 40L, &nonzero, sizeof nonzero);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);

    s_store_adversarial_fixture(path);
    store = create(path);
    Store_free(store);

    u8 pageBytes[TEST_PAGE];
    readAt(path, 0L, pageBytes, sizeof pageBytes);
    Page *page = Page_2(pageBytes, TEST_PAGE);
    assert(page != nullptr && Page_load(page) == PAGE_OK);
    u8 bootstrap[STORE_BOOTSTRAP_BYTES];
    memcpy(bootstrap, Page_payloadOf(page), sizeof bootstrap);
    u32 unsupportedVersion = STORE_FORMAT_VERSION + 1u;
    memcpy(bootstrap + 8u, &unsupportedVersion, sizeof unsupportedVersion);
    assert(Page_format(page, 0u, PAGE_KIND_CATALOG) == PAGE_OK);
    assert(Page_append(page, bootstrap, sizeof bootstrap) == PAGE_OK);
    assert(Page_seal(page) == PAGE_OK);
    Page_free(page);
    writeAt(path, 0L, pageBytes, sizeof pageBytes);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

static i32 suite_store_adversarial() {
    const utf8 *tmp = getenv("TMPDIR");
    i32 n = snprintf(s_store_adversarial_directory, sizeof s_store_adversarial_directory, "%s/store-adversarial-XXXXXX",
                     tmp != nullptr ? tmp : "/tmp");
    assert(n > 0 && (bytes) n < sizeof s_store_adversarial_directory && mkdtemp(s_store_adversarial_directory) != nullptr);
    committedWalLengthMutation();
    livePageDamageAndRecovery();
    rejectedReadPreservesOutputs();
    malformedBootstrapAdmission();
    assert(rmdir(s_store_adversarial_directory) == 0);
    Memory_freeAll();
    puts("store_adversarial_test: PASS (committed WAL, damaged page, read rejection, bootstrap)");
    return 0;
}
#undef _POSIX_C_SOURCE
#undef _DARWIN_C_SOURCE
#undef TEST_PAGE
#undef TEST_JOURNAL
#undef TEST_RESERVATION

/** Suite: store (from store/store_test.c). */
/**
 * Store owner: one real file containing bootstrap, redo and mapped data pages.
 * Proves page-level durable publication/reopen, logical id reuse, retained journal
 * exhaustion, rejection preservation, and real child exits at commit boundaries.
 * Single serialized owner, no network/provider/GUI or hardware power-loss claim.
 * Resource pressure is test-owned; no host-wide disk/RAM exhaustion. No thread-safe
 * API is offered, so racing mutation/reset is outside this owner's contract.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1


#define TEST_PAGE 512u
#define TEST_JOURNAL 65536u
#define TEST_RESERVATION 131072u
#define TEST_LIVE_PAGES 48u
static utf8 s_store_directory[1024];
static u32 s_store_fixtureId;

#ifdef STORE_FAULT_TESTS
/**
 * Compile-time substitution ONLY in Store's test object. The real production
 * source contains no fault switch; these wrappers delegate to the actual R2/Wal
 * functions except for the one explicitly armed next failure.
 */
static u32 fault;
u32 StoreTest_mapWrite(ReGrowableMappedFile *owner, bytes offset, const u8 *src, bytes length) {
    if (fault == 1u) { fault = 0u; return 2u; }
    return re_gmap_write(owner, offset, src, length);
}
u32 StoreTest_mapSync(ReGrowableMappedFile *owner) {
    if (fault == 2u) { fault = 0u; return 2u; }
    return re_gmap_sync(owner);
}
u32 StoreTest_mapGrow(ReGrowableMappedFile *owner, bytes length) {
    if (fault == 3u) { fault = 0u; return 2u; }
    return re_gmap_grow_to(owner, length);
}
WalCode StoreTest_walAppend(Wal *owner, u32 op, const u8 *src, u32 length, u64 *sequence) {
    if (fault == 4u) { fault = 0u; return WAL_IO; }
    return Wal_append(owner, op, src, length, sequence);
}
WalCode StoreTest_walSync(Wal *owner) {
    if (fault == 5u) { fault = 0u; return WAL_IO; }
    return Wal_sync(owner);
}
#endif

static fn s_store_fixture(utf8 path[1024]) {
    i32 n = snprintf(path, 1024u, "%s/store-%u.vexdb", s_store_directory, ++s_store_fixtureId);
    assert(n > 0 && n < 1024);
}

static fn readEquals(Store *store, u32 id, const u8 *expected, u32 length) {
    u8 bytes[TEST_PAGE];
    memset(bytes, 0xCC, sizeof bytes);
    u32 actual = UINT32_MAX, kind = UINT32_MAX;
    bool cut = true;
    assert(Store_page_read(store, id, bytes, sizeof bytes, &actual, &kind, &cut) == STORE_OK);
    assert(!cut && actual == length && kind == PAGE_KIND_LEAF);
    assert(memcmp(bytes, expected, length) == 0);
    assert(bytes[length] == 0xCC);
}

static fn crashObserver(any context, u32 phase) {
    u32 *selected = (u32*) context;
    if (phase == *selected)
        _exit(100 + (i32) phase);
}

static fn lifecycle() {
    utf8 path[1024];
    s_store_fixture(path);
    assert(Store_1(path) == nullptr);
    Store *store = Store(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr && Store_isOpen(store) && !Store_isBlocked(store));
    assert(Store_pageSize(store) == TEST_PAGE && Store_reserved(store) == TEST_RESERVATION);
    assert(Store_journalBytes(store) == TEST_JOURNAL && Store_pageCount(store) == 0u);
    assert(Store_sequence(store) == 0u && Store_freeHead(store) == 0u);
    assert(Store_exhaustionCount(store) == 0u);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == TEST_RESERVATION);
    assert(Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL) == nullptr);
    u8 payload[TEST_PAGE - PAGE_HEADER_BYTES];
    for (bytes i = 0u; i < sizeof payload; ++i)
        payload[i] = (u8) (i * 13u);
    u32 ids[TEST_LIVE_PAGES];
    for (u32 i = 0u; i < TEST_LIVE_PAGES; ++i) {
        payload[0] = (u8) i;
        assert(Store_add(store, PAGE_KIND_LEAF, payload, sizeof payload, &ids[i]) == STORE_OK);
        assert(ids[i] == i + 1u);
    }
    assert(Store_pageCount(store) == TEST_LIVE_PAGES);
    assert(Store_sequence(store) == TEST_LIVE_PAGES);
    for (u32 i = 0u; i < TEST_LIVE_PAGES; ++i) {
        payload[0] = (u8) i;
        readEquals(store, ids[i], payload, sizeof payload);
    }
    u32 out = UINT32_MAX;
    u64 sequence = Store_sequence(store);
    assert(Store_add(store, PAGE_KIND_LEAF, payload, sizeof payload + 1u, &out) == STORE_INVALID);
    assert(out == UINT32_MAX && Store_sequence(store) == sequence);
    assert(Store_add(store, PAGE_KIND_FREE, payload, 1u, &out) == STORE_INVALID);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 1u, &out) == STORE_INVALID);
    assert(Store_add(store, PAGE_KIND_LEAF, payload, 1u, nullptr) == STORE_INVALID);
    assert(Store_page_write(store, 0u, PAGE_KIND_LEAF, payload, 1u) == STORE_INVALID);
    assert(Store_page_release(store, TEST_LIVE_PAGES + 1u) == STORE_INVALID);
    u8 small[4] = {0x55, 0x55, 0x55, 0x55};
    u32 length = 123u, kind = 124u;
    bool cut = false;
    assert(Store_page_read(store, ids[0], small, sizeof small, &length, &kind, &cut) == STORE_TRUNCATED);
    assert(cut && length == 123u && kind == 124u && small[0] == 0x55);
    assert(Store_page_read(store, ids[0], nullptr, 0u, &length, &kind, &cut) == STORE_INVALID);
    assert(Store_page_release(store, ids[1]) == STORE_OK);
    assert(Store_page_release(store, ids[1]) == STORE_INVALID);
    assert(Store_page_write(store, ids[1], PAGE_KIND_LEAF, payload, 1u) == STORE_INVALID);
    assert(Store_freeHead(store) == ids[1]);
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "reused", 6u, &out) == STORE_OK);
    assert(out == ids[1] && Store_pageCount(store) == TEST_LIVE_PAGES && Store_freeHead(store) == 0u);
    assert(Store_page_write(store, ids[0], PAGE_KIND_LEAF, (const u8*) "changed", 7u) == STORE_OK);
    assert(Store_sync(store) == STORE_OK);
    utf8 text[768];
    assert(Store_toString(store, text, sizeof text, &cut) && !cut);
    assert(strstr(text, "Store(open=1") != nullptr);
    assert(Store_toStringStruct(store, text, sizeof text, &cut) && !cut);
    assert(strstr(text, "mapping=") != nullptr && strstr(text, "observerContext=redacted") != nullptr);
    assert(!Store_toString(store, text, 2u, &cut) && cut && text[1] == '\0');
    assert(!Store_toStringStruct(store, nullptr, 0u, &cut) && cut);
    sequence = Store_sequence(store);
    Store_free(store);
    store = Store(path);
    assert(store != nullptr && Store_sequence(store) == sequence);
    assert(Store_pageCount(store) == TEST_LIVE_PAGES);
    readEquals(store, ids[0], (const u8*) "changed", 7u);
    readEquals(store, ids[1], (const u8*) "reused", 6u);
    for (u32 i = 2u; i < TEST_LIVE_PAGES; ++i) {
        payload[0] = (u8) i;
        readEquals(store, ids[i], payload, sizeof payload);
    }
    assert(Store_page_release(store, ids[2]) == STORE_OK);
    assert(Store_page_release(store, ids[3]) == STORE_OK);
    Store_free(store);
    store = Store(path);
    assert(store != nullptr && Store_freeHead(store) == ids[3]);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_OK && out == ids[3]);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_OK && out == ids[2]);
    readEquals(store, out, (const u8*) "", 0u);
    assert(Store_freeHead(store) == 0u);
    Store_free(store);
    assert(unlink(path) == 0);
}

static fn boundedResources() {
    utf8 path[1024];
    s_store_fixture(path);
    bytes journal = 8192u;
    Store *store = Store_4(path, journal + TEST_PAGE * 3u, TEST_PAGE, journal);
    assert(store != nullptr);
    u32 a = 0u, b = 0u, out = UINT32_MAX;
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "A", 1u, &a) == STORE_OK);
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "B", 1u, &b) == STORE_OK);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_FULL);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_FULL);
    assert(Store_exhaustionCount(store) == 2u && out == UINT32_MAX && !Store_isBlocked(store));
    readEquals(store, a, (const u8*) "A", 1u);
    readEquals(store, b, (const u8*) "B", 1u);
    assert(Store_page_release(store, a) == STORE_OK);
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "C", 1u, &out) == STORE_OK && out == a);
    assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_FULL);
    assert(Store_exhaustionCount(store) == 3u);
    Store_free(store);
    store = Store_1(path);
    assert(store != nullptr && Store_exhaustionCount(store) == 0u);
    readEquals(store, a, (const u8*) "C", 1u);
    Store_free(store);
    assert(unlink(path) == 0);
    s_store_fixture(path);
    store = Store_4(path, 16384u, TEST_PAGE, TEST_PAGE * 2u);
    assert(store != nullptr);
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "retained", 8u, &a) == STORE_OK);
    assert(Store_page_write(store, a, PAGE_KIND_LEAF, (const u8*) "NO", 2u) == STORE_FULL);
    assert(Store_page_write(store, a, PAGE_KIND_LEAF, (const u8*) "NO", 2u) == STORE_FULL);
    assert(Store_exhaustionCount(store) == 2u && Store_sequence(store) == 1u);
    readEquals(store, a, (const u8*) "retained", 8u);
    Store_free(store);
    store = Store_1(path);
    assert(store != nullptr);
    readEquals(store, a, (const u8*) "retained", 8u);
    Store_free(store);
    assert(unlink(path) == 0);
}

static fn interruptedCommit(u32 phase) {
    utf8 path[1024];
    s_store_fixture(path);
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr);
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "old", 3u, &id) == STORE_OK);
    Store_free(store);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        Store *writer = Store_1(path);
        assert(writer != nullptr);
        Store_setCommitObserver(writer, crashObserver, &phase);
        assert(Store_getCommitObserver(writer) == crashObserver && Store_getObserverContext(writer) == &phase);
        (discard) Store_page_write(writer, id, PAGE_KIND_LEAF, (const u8*) "durable-redo", 12u);
        _exit(9);
    }
    i32 status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == 100 + (i32) phase);
    FILE *raw = fopen(path, "r+b.sh");
    assert(raw != nullptr && fseek(raw, TEST_PAGE + TEST_JOURNAL, SEEK_SET) == 0);
    u8 torn[TEST_PAGE] = {0};
    assert(fwrite(torn, 1u, sizeof torn, raw) == sizeof torn && fclose(raw) == 0);
    store = Store_1(path);
    assert(store != nullptr && Store_sequence(store) == 2u);
    readEquals(store, id, (const u8*) "durable-redo", 12u);
    assert(Store_page_write(store, id, PAGE_KIND_LEAF, (const u8*) "retry", 5u) == STORE_OK);
    Store_free(store);
    store = Store_1(path);
    assert(store != nullptr && Store_sequence(store) == 3u);
    readEquals(store, id, (const u8*) "retry", 5u);
    Store_free(store);
    assert(unlink(path) == 0);
}

static fn invalidAdmission() {
    utf8 path[1024];
    s_store_fixture(path);
    assert(Store_4(nullptr, 4096u, TEST_PAGE, 1024u) == nullptr);
    assert(Store_4(path, 4096u, 513u, 1024u) == nullptr);
    assert(Store_4(path, 4096u, TEST_PAGE, 0u) == nullptr);
    assert(Store_4(path, SIZE_MAX, TEST_PAGE, 1024u) == nullptr);
    assert(Store_1(nullptr) == nullptr && Store_1("") == nullptr);
    assert(access(path, F_OK) != 0);
    Store *zero = Store_zero();
    assert(zero != nullptr && !Store_isOpen(zero));
    u32 id = 99u;
    assert(Store_add(zero, PAGE_KIND_LEAF, nullptr, 0u, &id) == STORE_CLOSED && id == 99u);
    assert(Store_sync(zero) == STORE_CLOSED);
    Store_free(zero);
    zero = Store();
    Store_free(zero);
    Store_free(nullptr);
    assert(!Store_isOpen(nullptr) && !Store_isBlocked(nullptr));
    assert(Store_code(nullptr) == STORE_CLOSED && Store_pageSize(nullptr) == 0u);
    assert(Store_pageCount(nullptr) == 0u && Store_freeHead(nullptr) == 0u);
    assert(Store_reserved(nullptr) == 0u && Store_journalBytes(nullptr) == 0u);
    assert(Store_sequence(nullptr) == 0u && Store_exhaustionCount(nullptr) == 0u);
    Store_setCommitObserver(nullptr, nullptr, nullptr);
    assert(Store_getCommitObserver(nullptr) == nullptr && Store_getObserverContext(nullptr) == nullptr);
    utf8 text[64];
    bool cut = true;
    assert(Store_toString(nullptr, text, sizeof text, &cut) && !cut && strcmp(text, "nullptr") == 0);
    assert(Store_toStringStruct(nullptr, text, sizeof text, &cut) && strcmp(text, "nullptr") == 0);
    for (i32 i = STORE_OK; i <= STORE_TRUNCATED; ++i)
        assert(strcmp(Store_codeName((StoreCode) i), "unknown") != 0);
    assert(strcmp(Store_codeName((StoreCode) 99), "unknown") == 0);
    s_store_fixture(path);
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr);
    Store_free(store);
    FILE *raw = fopen(path, "r+b.sh");
    assert(raw != nullptr && fseek(raw, PAGE_HEADER_BYTES + 8u, SEEK_SET) == 0);
    assert(fputc(99, raw) != EOF && fclose(raw) == 0);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

static fn forgedBaseline() {
    utf8 path[1024];
    s_store_fixture(path);
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr);
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "baseline", 8u, &id) == STORE_OK);
    Store_free(store);
    FILE *raw = fopen(path, "r+b.sh");
    assert(raw != nullptr && fseek(raw, TEST_PAGE, SEEK_SET) == 0);
    u8 header[WAL_HEADER_BYTES];
    assert(fread(header, 1u, sizeof header, raw) == sizeof header);
    u64 base = 100u;
    memcpy(header + 8u, &base, sizeof base);
    u32 crc = Crc32_of(header, 20u);
    memcpy(header + 20u, &crc, sizeof crc);
    assert(fseek(raw, TEST_PAGE, SEEK_SET) == 0 && fwrite(header, 1u, sizeof header, raw) == sizeof header);
    u8 record[8u + WAL_RECORD_FIXED + STORE_REDO_BYTES + TEST_PAGE + 4u];
    assert(fread(record, 1u, sizeof record, raw) == sizeof record);
    u64 sequence = base + 1u;
    memcpy(record + 16u, &sequence, sizeof sequence);
    crc = Crc32_of(record + 8u, sizeof record - 12u);
    memcpy(record + sizeof record - 4u, &crc, sizeof crc);
    assert(fseek(raw, TEST_PAGE + WAL_HEADER_BYTES, SEEK_SET) == 0);
    assert(fwrite(record, 1u, sizeof record, raw) == sizeof record && fclose(raw) == 0);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

#ifdef STORE_FAULT_TESTS
static fn uncertainIO() {
    for (u32 stage = 1u; stage <= 5u; ++stage) {
        utf8 path[1024];
        s_store_fixture(path);
        Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
        assert(store != nullptr);
        u32 id = 0u, out = UINT32_MAX;
        assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "survivor", 8u, &id) == STORE_OK);
        fault = stage;
        assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "uncertain", 9u, &out) == STORE_RECOVERY_REQUIRED);
        assert(fault == 0u && out == UINT32_MAX && Store_isBlocked(store));
        assert(Store_add(store, PAGE_KIND_LEAF, nullptr, 0u, &out) == STORE_RECOVERY_REQUIRED);
        assert(Store_page_write(store, id, PAGE_KIND_LEAF, nullptr, 0u) == STORE_RECOVERY_REQUIRED);
        Store_free(store);
        store = Store_1(path);
        assert(store != nullptr && !Store_isBlocked(store));
        readEquals(store, id, (const u8*) "survivor", 8u);
        if (stage == 4u)
            assert(Store_pageCount(store) == 1u && Store_sequence(store) == 1u);
        else {
            assert(Store_pageCount(store) == 2u && Store_sequence(store) == 2u);
            readEquals(store, 2u, (const u8*) "uncertain", 9u);
        }
        assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "retry", 5u, &out) == STORE_OK);
        Store_free(store);
        assert(unlink(path) == 0);
    }
}
#endif

static i32 suite_store(i32 argc, utf8 **argv) {
    bool leakSafe = argc == 2 && strcmp(argv[1], "--leak-safe") == 0;
    assert(argc == 1 || leakSafe);
    const utf8 *tmp = getenv("TMPDIR");
    i32 n = snprintf(s_store_directory, sizeof s_store_directory, "%s/store-owner-XXXXXX", tmp != nullptr ? tmp : "/tmp");
    assert(n > 0 && (bytes) n < sizeof s_store_directory && mkdtemp(s_store_directory) != nullptr);
    invalidAdmission();
    lifecycle();
    boundedResources();
    forgedBaseline();
#ifdef STORE_FAULT_TESTS
    uncertainIO();
#endif
    if (!leakSafe)
        for (u32 phase = STORE_AFTER_LOG_SYNC; phase <= STORE_AFTER_DATA_SYNC; ++phase)
            interruptedCommit(phase);
    assert(rmdir(s_store_directory) == 0);
    Memory_freeAll();
    puts(leakSafe ? "store_test: PASS (leak instrumentation: non-fork lifetime and IO-fault paths)" :
         "store_test: PASS (48 live full pages; reuse, two capacity bounds, 3 child crash boundaries)");
    return 0;
}
#undef _POSIX_C_SOURCE
#undef _DARWIN_C_SOURCE
#undef TEST_PAGE
#undef TEST_JOURNAL
#undef TEST_RESERVATION
#undef TEST_LIVE_PAGES

/** Zero-arg adapter for store suite. */
static i32 suite_store_entry() {
    utf8 *argv[] = { "darkbase_test", nullptr };
    return suite_store(1, argv);
}

/** Runs all automated vex/darkbase suites or a single named suite passed as argv[1]. */
i32 main(i32 argc, utf8 **argv) {
    const SuiteEntry suites[] = {
        { "database_result", suite_database_result },
        { "database", suite_database },
        { "page", suite_page },
        { "crc32", suite_crc32 },
        { "wal", suite_wal },
        { "modifier", suite_modifier },
        { "headless_protocol", suite_headless_protocol },
        { "service", suite_service },
        { "store_adversarial", suite_store_adversarial },
        { "store", suite_store_entry },
    };
    const utf8 *filter = argc > 1 ? argv[1] : nullptr;
    bytes ran = 0;
    i32 failed = 0;

    for (bytes i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        if (filter && strcmp(filter, suites[i].name) != 0)
            continue;
        if (suites[i].run() != 0)
            failed++;
        ran++;
    }

    if (filter && ran == 0) {
        fprintf(stderr, "darkbase_test: unknown suite '%s'\n", filter);
        return 2;
    }
    if (failed != 0) {
        fprintf(stderr, "darkbase_test: %d suite(s) FAILED\n", failed);
        return 1;
    }
    puts("darkbase_test: PASS");
    return 0;
}
