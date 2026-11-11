// Store owner: one real file containing bootstrap, redo and mapped data pages.
// Proves page-level durable publication/reopen, logical id reuse, retained journal
// exhaustion, rejection preservation, and real child exits at commit boundaries.
// Single serialized owner, no network/provider/GUI or hardware power-loss claim.
// Resource pressure is test-owned; no host-wide disk/RAM exhaustion. No thread-safe
// API is offered, so racing mutation/reset is outside this owner's contract.
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "store/store.h"
#include "nio/mem.h"
#include "util/crc32.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_PAGE 512u
#define TEST_JOURNAL 65536u
#define TEST_RESERVATION 131072u
#define TEST_LIVE_PAGES 48u
static char directory[1024];
static unsigned fixtureId;

#ifdef STORE_FAULT_TESTS
// Compile-time substitution ONLY in Store's test object. The real production
// source contains no fault switch; these wrappers delegate to the actual R2/Wal
// functions except for the one explicitly armed next failure.
static unsigned fault;
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

static void fixture(char path[1024]) {
    int n = snprintf(path, 1024u, "%s/store-%u.vexdb", directory, ++fixtureId);
    assert(n > 0 && n < 1024);
}

static void readEquals(Store *store, u32 id, const u8 *expected, u32 length) {
    u8 bytes[TEST_PAGE];
    memset(bytes, 0xCC, sizeof bytes);
    u32 actual = UINT32_MAX, kind = UINT32_MAX;
    bool cut = true;
    assert(Store_page_read(store, id, bytes, sizeof bytes, &actual, &kind, &cut) == STORE_OK);
    assert(!cut && actual == length && kind == PAGE_KIND_LEAF);
    assert(memcmp(bytes, expected, length) == 0);
    assert(bytes[length] == 0xCC);
}

static void crashObserver(void *context, unsigned phase) {
    unsigned *selected = (unsigned*) context;
    if (phase == *selected)
        _exit(100 + (int) phase);
}

static void lifecycle(void) {
    char path[1024];
    fixture(path);
    assert(Store_1(path) == nullptr); // missing is not implicit creation
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
    // Rejection does not change the output id, count, sequence or survivor bytes.
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
    // Release and reallocate the SAME logical page id without raising the high water.
    assert(Store_page_release(store, ids[1]) == STORE_OK);
    assert(Store_page_release(store, ids[1]) == STORE_INVALID);
    assert(Store_page_write(store, ids[1], PAGE_KIND_LEAF, payload, 1u) == STORE_INVALID);
    assert(Store_freeHead(store) == ids[1]);
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "reused", 6u, &out) == STORE_OK);
    assert(out == ids[1] && Store_pageCount(store) == TEST_LIVE_PAGES && Store_freeHead(store) == 0u);
    assert(Store_page_write(store, ids[0], PAGE_KIND_LEAF, (const u8*) "changed", 7u) == STORE_OK);
    assert(Store_sync(store) == STORE_OK);
    char text[768];
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
    // Persist the free chain through reopen, then consume it in LIFO order.
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

static void boundedResources(void) {
    char path[1024];
    fixture(path);
    // Exactly two data pages; journal ample for release/reuse/refill history.
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
    // Small explicit redo region: one full-page redo fits, the next must reject.
    fixture(path);
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

static void interruptedCommit(unsigned phase) {
    char path[1024];
    fixture(path);
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
        (void) Store_page_write(writer, id, PAGE_KIND_LEAF, (const u8*) "durable-redo", 12u);
        _exit(9); // must not return from the selected actual commit boundary
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == 100 + (int) phase);
    // Additionally destroy the target data frame after the child's durable redo:
    // recovery must repair a torn page, not merely adopt a lucky page-cache write.
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

static void invalidAdmission(void) {
    char path[1024];
    fixture(path);
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
    char text[64];
    bool cut = true;
    assert(Store_toString(nullptr, text, sizeof text, &cut) && !cut && strcmp(text, "nullptr") == 0);
    assert(Store_toStringStruct(nullptr, text, sizeof text, &cut) && strcmp(text, "nullptr") == 0);
    for (int i = STORE_OK; i <= STORE_TRUNCATED; ++i)
        assert(strcmp(Store_codeName((StoreCode) i), "unknown") != 0);
    assert(strcmp(Store_codeName((StoreCode) 99), "unknown") == 0);
    // Bootstrap damage fails closed instead of admitting a guessed geometry.
    fixture(path);
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr);
    Store_free(store);
    FILE *raw = fopen(path, "r+b.sh");
    assert(raw != nullptr && fseek(raw, PAGE_HEADER_BYTES + 8u, SEEK_SET) == 0);
    assert(fputc(99, raw) != EOF && fclose(raw) == 0);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

static void forgedBaseline(void) {
    char path[1024];
    fixture(path);
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr);
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, (const u8*) "baseline", 8u, &id) == STORE_OK);
    Store_free(store);
    // A CRC-valid region header and record are NOT a supported checkpoint base.
    // Store always replays from immutable bootstrap state, so base must be zero.
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
static void uncertainIO(void) {
    for (unsigned stage = 1u; stage <= 5u; ++stage) {
        char path[1024];
        fixture(path);
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

int main(int argc, char **argv) {
    bool leakSafe = argc == 2 && strcmp(argv[1], "--leak-safe") == 0;
    assert(argc == 1 || leakSafe);
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(directory, sizeof directory, "%s/store-owner-XXXXXX", tmp != nullptr ? tmp : "/tmp");
    assert(n > 0 && (bytes) n < sizeof directory && mkdtemp(directory) != nullptr);
    invalidAdmission();
    lifecycle();
    boundedResources();
    forgedBaseline();
#ifdef STORE_FAULT_TESTS
    uncertainIO();
#endif
    // Apple's atExit leak monitor cannot supervise the deliberate _exit children;
    // only that separate instrumentation invocation excludes these child cases.
    // Normal and sanitizer owner invocations ALWAYS execute all three boundaries.
    if (!leakSafe)
        for (unsigned phase = STORE_AFTER_LOG_SYNC; phase <= STORE_AFTER_DATA_SYNC; ++phase)
            interruptedCommit(phase);
    assert(rmdir(directory) == 0); // one database file, no sidecar artifacts
    Memory_freeAll();
    puts(leakSafe ? "store_test: PASS (leak instrumentation: non-fork lifetime and IO-fault paths)" :
         "store_test: PASS (48 live full pages; reuse, two capacity bounds, 3 child crash boundaries)");
    return 0;
}
