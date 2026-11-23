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

#include "vex.h"
#include "wal/wal.h"
#include "util/crc32.h"
#include "nio/mem.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

i32 main() {
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
