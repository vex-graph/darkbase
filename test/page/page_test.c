// Owner: page/page.c — the page: layout, seals, capacity, and the corruption matrix.
//
// The point of this file is the last third: a page whose bytes changed in ANY of the
// sealed fields must fail verification, including the page's own ID and the store's
// page size. That is what makes a misplaced or mis-sized page detectable instead of
// silently plausible. Storage is a stack buffer; the page owns nothing.
#include "page/page.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SIZE PAGE_SIZE_MIN          /* 512: plenty, and fast */
static uint8_t storage[SIZE];

/* The raw header offsets the format defines; asserted as bytes, not through getters. */
static uint32_t rawU32(size_t offset) {
    return (uint32_t) storage[offset] | ((uint32_t) storage[offset + 1] << 8) |
           ((uint32_t) storage[offset + 2] << 16) | ((uint32_t) storage[offset + 3] << 24);
}

/* Each rejected admission preserves every cached field and every supplied byte.
 * Capture the cold diagnostic as well as its return; normal admission is quiet.
 * External serialization is the contract; racing writes and dead buffers are not. */
static void checkAdmission(Page *page, PageCode expected, const char *message) {
    Page before;
    uint8_t bytesBefore[SIZE];
    if (page != nullptr) {
        memcpy(&before, page, sizeof before);
        if ((*page).bytes != nullptr)
            memcpy(bytesBefore, (*page).bytes, sizeof bytesBefore);
    }
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    assert(fflush(stderr) == 0);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    assert(Page_verify(page) == expected);
    assert(Page_load(page) == expected);
    assert(fflush(stderr) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    char output[1024];
    size_t length = fread(output, 1u, sizeof output - 1u, capture);
    output[length] = '\0';
    assert(!ferror(capture));
    assert(fclose(capture) == 0);
    if (expected == PAGE_OK)
        assert(length == 0u);
    else {
        unsigned lines = 0u;
        for (size_t i = 0u; i < length; ++i)
            if (output[i] == '\n')
                ++lines;
        assert(lines == 2u && strstr(output, "[vex] ") != nullptr);
        assert(strstr(output, message) != nullptr);
        if (page != nullptr)
            assert(memcmp(page, &before, sizeof before) == 0);
    }
    if (page != nullptr && (*page).bytes != nullptr)
        assert(memcmp((*page).bytes, bytesBefore, sizeof bytesBefore) == 0);
}

static void restoredAdmission(void) {
    Page *writer = Page_2(storage, SIZE);
    assert(writer != nullptr);
    uint8_t payload[SIZE - PAGE_HEADER_BYTES];
    memset(payload, 0xA5, sizeof payload);
    const uint32_t lengths[] = {0u, 1u, 13u, sizeof payload};
    for (uint32_t kind = PAGE_KIND_FREE; kind <= PAGE_KIND_CATALOG; ++kind) {
        for (size_t i = 0u; i < sizeof lengths / sizeof lengths[0]; ++i) {
            assert(Page_format(writer, 91u, kind) == PAGE_OK);
            assert(Page_append(writer, payload, lengths[i]) == PAGE_OK);
            assert(Page_seal(writer) == PAGE_OK);
            uint8_t original[SIZE];
            memcpy(original, storage, sizeof original);
            Page *reader = Page_2(storage, SIZE);
            assert(reader != nullptr && memcmp(original, storage, SIZE) == 0);
            assert(Page_used(reader) == 0u && !Page_isSealed(reader));
            checkAdmission(reader, PAGE_OK, nullptr);
            /* Verify must be independent of even an absurd stale cached used. */
            (*reader).used = UINT32_MAX;
            Page snapshot;
            memcpy(&snapshot, reader, sizeof snapshot);
            assert(Page_verify(reader) == PAGE_OK);
            assert(memcmp(reader, &snapshot, sizeof snapshot) == 0);
            checkAdmission(reader, PAGE_OK, nullptr);
            assert(Page_id(reader) == 91u && Page_kind(reader) == kind);
            assert(Page_used(reader) == lengths[i] && Page_code(reader) == PAGE_OK);
            assert(Page_isSealed(reader) && !Page_isDirty(reader));
            assert(memcmp(Page_payloadOf(reader), payload, lengths[i]) == 0);
            char text[256];
            bool truncated = true;
            assert(Page_toStringStruct(reader, text, sizeof text, &truncated));
            assert(!truncated && strstr(text, "id=91") != nullptr);
            assert(strstr(text, "sealed=1, dirty=0") != nullptr);
            assert(strstr(text, "lastCode=ok") != nullptr && strstr(text, "bytes=") != nullptr);
            /* Repeated load is idempotent; its admission does not permit edits. */
            checkAdmission(reader, PAGE_OK, nullptr);
            Page_free(reader);
        }
    }

    /* Keep stale yet real cached metadata while replacing its borrowed bytes. */
    Page *reader = Page_2(storage, SIZE);
    assert(reader != nullptr);
    assert(Page_format(reader, 3u, PAGE_KIND_BRANCH) == PAGE_OK);
    assert(Page_append(reader, payload, 1u) == PAGE_OK);
    assert(Page_append(reader, nullptr, 1u) == PAGE_INVALID);
    assert(Page_format(writer, 91u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_append(writer, payload, 13u) == PAGE_OK);
    assert(Page_seal(writer) == PAGE_OK);
    uint8_t valid[SIZE];
    memcpy(valid, storage, sizeof valid);
    const struct { size_t offset; PageCode code; const char *message; } cases[] = {
        {0u, PAGE_INVALID, "magic"},
        {4u, PAGE_BAD_SEAL, "seal"},
        {8u, PAGE_INVALID, "kind"},
        {16u, PAGE_INVALID, "size"},
        {20u, PAGE_INVALID, "reserved"},
        {24u, PAGE_INVALID, "reserved"},
        {PAGE_SEAL_OFFSET, PAGE_BAD_SEAL, "seal"},
        {PAGE_HEADER_BYTES, PAGE_BAD_SEAL, "seal"},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        storage[cases[i].offset] ^= 0x5Au;
        checkAdmission(reader, cases[i].code, cases[i].message);
        memcpy(storage, valid, sizeof valid);
    }
    /* Exact capacity plus one and integer maximum must reject before CRC reads. */
    uint32_t invalidUsed[] = {SIZE - PAGE_HEADER_BYTES + 1u, UINT32_MAX};
    for (size_t i = 0u; i < sizeof invalidUsed / sizeof invalidUsed[0]; ++i) {
        memcpy(storage + 12u, &invalidUsed[i], sizeof invalidUsed[i]);
        checkAdmission(reader, PAGE_INVALID, "length");
        memcpy(storage, valid, sizeof valid);
    }
    uint32_t sizes[] = {0u, PAGE_HEADER_BYTES - 1u, SIZE - 1u, PAGE_SIZE_MAX + 1u};
    for (size_t i = 0u; i < sizeof sizes / sizeof sizes[0]; ++i) {
        (*reader).size = sizes[i];
        checkAdmission(reader, PAGE_INVALID, "storage");
    }
    (*reader).size = SIZE;
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

int main(void) {
    restoredAdmission();
    /* Legal page sizes are powers of two inside the advertised range. */
    assert(Page_sizeValid(PAGE_SIZE_MIN) && Page_sizeValid(PAGE_SIZE_DEFAULT));
    assert(Page_sizeValid(1024u) && Page_sizeValid(PAGE_SIZE_MAX));
    assert(!Page_sizeValid(0u));
    assert(!Page_sizeValid(256u));          // below the floor
    assert(!Page_sizeValid(PAGE_SIZE_MAX + 1u));
    assert(!Page_sizeValid(300u));          // not a power of two
    assert(!Page_sizeValid(1000u));

    /* A page with no storage refuses every operation. */
    Page *bare = Page_0();
    assert(bare != nullptr);
    assert(Page_size(bare) == 0u && Page_payload(bare) == nullptr);
    assert(Page_format(bare, 1u, PAGE_KIND_LEAF) == PAGE_INVALID);
    assert(Page_append(bare, (const uint8_t*) "x", 1u) == PAGE_INVALID);
    assert(Page_seal(bare) == PAGE_INVALID);
    assert(Page_verify(bare) == PAGE_INVALID);
    Page_free(bare);

    /* Binding refuses a null buffer and an illegal size. */
    assert(Page_2(nullptr, SIZE) == nullptr);
    assert(Page_2(storage, 300u) == nullptr);

    Page *page = Page_2(storage, SIZE);
    assert(page != nullptr);
    assert(Page_size(page) == SIZE && Page_capacity(page) == SIZE - PAGE_HEADER_BYTES);

    /* Format writes the documented layout, in bytes. */
    assert(Page_format(page, 7u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_code(page) == PAGE_OK);
    assert(Page_id(page) == 7u && Page_kind(page) == PAGE_KIND_LEAF);
    assert(Page_used(page) == 0u && Page_freeSpace(page) == Page_capacity(page));
    assert(!Page_isSealed(page) && Page_isDirty(page) && Page_sealValue(page) == 0u);
    assert(rawU32(0u) == PAGE_MAGIC);
    assert(rawU32(4u) == 7u);
    assert(rawU32(8u) == (uint32_t) PAGE_KIND_LEAF);
    assert(rawU32(12u) == 0u);
    assert(rawU32(16u) == SIZE);
    assert(rawU32(PAGE_SEAL_OFFSET) == 0u);          // unsealed until sealed

    /* Appending fills the payload; the header's `used` follows it. */
    assert(Page_append(page, (const uint8_t*) "alpha", 5u) == PAGE_OK);
    assert(Page_used(page) == 5u && Page_freeSpace(page) == Page_capacity(page) - 5u);
    assert(rawU32(12u) == 5u);
    assert(memcmp(Page_payloadOf(page), "alpha", 5u) == 0);
    assert(Page_append(page, nullptr, 0u) == PAGE_OK);      // an empty append is legal
    assert(Page_used(page) == 5u);
    assert(Page_append(page, (const uint8_t*) "x", 0u) == PAGE_OK);

    /* Fill to exactly full, then refuse the next byte WITHOUT a partial append. */
    uint8_t filler[SIZE];
    memset(filler, 0x5A, sizeof filler);
    uint32_t remaining = Page_freeSpace(page);
    assert(Page_append(page, filler, remaining) == PAGE_OK);
    assert(Page_freeSpace(page) == 0u && Page_used(page) == Page_capacity(page));
    uint8_t before[8];
    memcpy(before, Page_payloadOf(page), sizeof before);
    assert(Page_append(page, (const uint8_t*) "!", 1u) == PAGE_FULL);
    assert(Page_code(page) == PAGE_FULL);
    assert(Page_used(page) == Page_capacity(page));         // nothing was appended
    assert(memcmp(Page_payloadOf(page), before, sizeof before) == 0);
    assert(Page_append(page, nullptr, 1u) == PAGE_INVALID); // null source, real length

    /* Sealing stores the checksum; a sealed page is immutable. */
    assert(Page_seal(page) == PAGE_OK);
    assert(Page_isSealed(page) && !Page_isDirty(page));
    uint32_t seal = Page_sealValue(page);
    assert(seal != 0u && rawU32(PAGE_SEAL_OFFSET) == seal);
    assert(Page_verify(page) == PAGE_OK);
    assert(Page_append(page, (const uint8_t*) "x", 1u) == PAGE_SEALED);
    assert(Page_reset(page, PAGE_KIND_BRANCH) == PAGE_OK);
    assert(Page_id(page) == 7u);                            // reset keeps the identity
    assert(Page_kind(page) == PAGE_KIND_BRANCH);
    assert(Page_used(page) == 0u && !Page_isSealed(page));
    /* An unsealed page must NOT verify: that is what makes "seal before durable" real. */
    checkAdmission(page, PAGE_BAD_SEAL, "seal");

    /* ------------------------------------------------------------------ *
     * The corruption matrix. Every sealed field is covered, so a misplaced
     * page (wrong id), a mis-sized read (wrong size), a truncated payload
     * (wrong used) and a flipped byte each fail instead of passing.         */
    assert(Page_format(page, 42u, PAGE_KIND_LEAF) == PAGE_OK);
    assert(Page_append(page, (const uint8_t*) "payload-bytes", 13u) == PAGE_OK);
    assert(Page_seal(page) == PAGE_OK);
    assert(Page_verify(page) == PAGE_OK);

    struct { const char *what; size_t offset; } cases[] = {
        { "a flipped payload byte", PAGE_HEADER_BYTES + 3u },
        { "the page id",            4u },
        { "the used length",        12u },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t saved = storage[cases[i].offset];
        storage[cases[i].offset] ^= 0x5Au;
        assert(Page_verify(page) == PAGE_BAD_SEAL);          // always BAD_SEAL, never OK
        storage[cases[i].offset] = saved;
        assert(Page_verify(page) == PAGE_OK);                // restored, good again
    }

    /* A wrong magic or page size is not a damaged page, it is not OUR page. */
    uint8_t savedMagic = storage[0];
    storage[0] ^= 0xFFu;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[0] = savedMagic;
    uint8_t savedSize = storage[16];
    storage[16] ^= 0x01u;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[16] = savedSize;
    assert(Page_verify(page) == PAGE_OK);

    /* A used length that cannot fit the page is refused rather than read through. */
    uint8_t savedUsedLow = storage[12];
    uint8_t savedUsedHigh = storage[13];
    storage[12] = 0xFFu;
    storage[13] = 0xFFu;
    assert(Page_verify(page) == PAGE_INVALID);
    storage[12] = savedUsedLow;
    storage[13] = savedUsedHigh;
    assert(Page_verify(page) == PAGE_OK);

    /* Verify never crashes on arbitrary bytes: an all-zero buffer is simply refused. */
    uint8_t zeros[SIZE];
    memset(zeros, 0, sizeof zeros);
    Page *foreign = Page_2(zeros, SIZE);
    assert(foreign != nullptr);
    assert(Page_verify(foreign) == PAGE_INVALID);
    Page_free(foreign);

    /* Naming covers every code and kind, and unknown values do not crash. */
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
    assert(Page_format(page, 1u, 99u) == PAGE_INVALID);      // unknown kind refused
    assert(Page_codeName(Page_code(page)) != nullptr);

    /* Projections are bounded and report truncation rather than cutting silently. */
    char text[256];
    bool truncated = true;
    assert(Page_toString(page, text, sizeof text, &truncated));
    assert(!truncated && strstr(text, "Page(id=") != nullptr);
    assert(Page_toStringStruct(page, text, sizeof text, &truncated));
    assert(strstr(text, "seal=") != nullptr && strstr(text, "kind=") != nullptr);
    char tiny[4];
    truncated = false;
    assert(!Page_toString(page, tiny, sizeof tiny, &truncated));
    assert(truncated);
    assert(!Page_toStringStruct(page, nullptr, 0u, nullptr));
    assert(Page_toString(nullptr, text, sizeof text, nullptr));   // the toString Law
    assert(strcmp(text, "nullptr") == 0);

    /* Null-owner accessors are safe defaults, not crashes. */
    assert(Page_id(nullptr) == 0u && Page_kind(nullptr) == 0u && Page_size(nullptr) == 0u);
    assert(Page_payload(nullptr) == nullptr && Page_payloadOf(nullptr) == nullptr);
    assert(!Page_isSealed(nullptr) && !Page_isDirty(nullptr));
    assert(Page_code(nullptr) == PAGE_INVALID);
    Page_markClean(nullptr);
    Page_free(nullptr);

    /* markClean is the caller telling us it flushed. */
    assert(Page_format(page, 3u, PAGE_KIND_OVERFLOW) == PAGE_OK);
    assert(Page_isDirty(page));
    Page_markClean(page);
    assert(!Page_isDirty(page));

    Page_free(page);
    printf("page_test: PASS\n");
    return 0;
}
