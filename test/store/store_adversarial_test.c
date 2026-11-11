// Store adversarial owner: hostile committed redo metadata, live page damage,
// rejected read outputs, and bootstrap admission. Test-owned files only; no
// concurrency, external attacker, hardware power-loss, or production readiness claim.
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "store/store.h"
#include "nio/mem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_PAGE 512u
#define TEST_JOURNAL 65536u
#define TEST_RESERVATION 131072u

// MODULE MAP:
//   fixture — unique files in one private temporary directory
//   writeAt/readAt — exact bounded edits to test-owned database bytes
//   committedWalLengthMutation — published marker plus altered record length rejects
//   livePageDamageAndRecovery — public read rejects damage without publishing outputs,
//                               then reopen replays the retained redo image
//   rejectedReadPreservesOutputs — nullable parameters and invalid IDs are atomic
//   malformedBootstrapAdmission — damaged seal and resealed unsupported version reject
//   main — runs each independent cold-admission scenario and removes all fixtures

static char directory[1024];
static unsigned fixtureId;

static void fixture(char path[1024]) {
    int n = snprintf(path, 1024u, "%s/store-adversarial-%u.vexdb", directory, ++fixtureId);
    assert(n > 0 && n < 1024);
}

static void writeAt(const char *path, long offset, const void *bytes, bytes length) {
    FILE *file = fopen(path, "r+b.sh");
    assert(file != nullptr);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fwrite(bytes, 1u, length, file) == length);
    assert(fflush(file) == 0);
    assert(fclose(file) == 0);
}

static void readAt(const char *path, long offset, void *bytes, bytes length) {
    FILE *file = fopen(path, "rb");
    assert(file != nullptr);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fread(bytes, 1u, length, file) == length);
    assert(fclose(file) == 0);
}

static Store *create(const char *path) {
    Store *store = Store_4(path, TEST_RESERVATION, TEST_PAGE, TEST_JOURNAL);
    assert(store != nullptr && Store_isOpen(store));
    return store;
}

static void committedWalLengthMutation(void) {
    char path[1024];
    fixture(path);
    Store *store = create(path);
    u32 id = 0u;
    const u8 payload[] = "committed record";
    assert(Store_add(store, PAGE_KIND_LEAF, payload, sizeof payload, &id) == STORE_OK);
    assert(id == 1u);
    Store_free(store);

    // V3 layout: region header, leading commit marker, 20-byte record prefix,
    // payload, CRC. Alter a committed record's length while leaving its marker.
    const long lengthOffset = (long) TEST_PAGE + (long) WAL_HEADER_BYTES +
                              (long) WAL_COMMIT_BYTES + 16L;
    u32 recordedLength = 0u;
    readAt(path, lengthOffset, &recordedLength, sizeof recordedLength);
    assert(recordedLength == STORE_REDO_BYTES + TEST_PAGE);
    --recordedLength;
    writeAt(path, lengthOffset, &recordedLength, sizeof recordedLength);

    // A present commit marker makes this a malformed committed record, not a torn
    // trailing append. Reopen must refuse rather than silently drop the record.
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);
}

static void livePageDamageAndRecovery(void) {
    char path[1024];
    fixture(path);
    Store *store = create(path);
    const u8 original[] = "redo repairs the damaged page";
    u32 id = 0u;
    assert(Store_add(store, PAGE_KIND_LEAF, original, sizeof original, &id) == STORE_OK);

    // Deliberately simulate backing-store damage through a test-owned file handle.
    // No concurrent actor is present; this is an adversarial corruption probe outside
    // the Store's stable-file precondition, not a hostile-writer safety claim.
    long dataOffset = (long) TEST_PAGE + (long) TEST_JOURNAL + (long) PAGE_HEADER_BYTES;
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

    // The immutable bootstrap starts Store state at zero; the committed full-page
    // redo remains authoritative and repairs the damaged mapped page on reopen.
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

static void rejectedReadPreservesOutputs(void) {
    char path[1024];
    fixture(path);
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

static void malformedBootstrapAdmission(void) {
    char path[1024];
    fixture(path);
    Store *store = create(path);
    Store_free(store);

    // Corrupt an unused bootstrap payload byte without resealing the page. The
    // bootstrap checksum covers only its header and used bytes, including this byte.
    u8 nonzero = 1u;
    writeAt(path, (long) PAGE_HEADER_BYTES + 40L, &nonzero, sizeof nonzero);
    assert(Store_1(path) == nullptr);
    assert(unlink(path) == 0);

    fixture(path);
    store = create(path);
    Store_free(store);

    // Rebuild a page with a valid page seal but an unsupported bootstrap version:
    // this distinguishes semantic bootstrap validation from CRC-only validation.
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

int main(void) {
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(directory, sizeof directory, "%s/store-adversarial-XXXXXX",
                     tmp != nullptr ? tmp : "/tmp");
    assert(n > 0 && (bytes) n < sizeof directory && mkdtemp(directory) != nullptr);
    committedWalLengthMutation();
    livePageDamageAndRecovery();
    rejectedReadPreservesOutputs();
    malformedBootstrapAdmission();
    assert(rmdir(directory) == 0);
    Memory_freeAll();
    puts("store_adversarial_test: PASS (committed WAL, damaged page, read rejection, bootstrap)");
    return 0;
}
