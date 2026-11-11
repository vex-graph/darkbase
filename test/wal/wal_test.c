// Owner: wal/wal.c — the write-ahead log: ordering, checksums, torn tails, recovery.
//
// The test writes a private log under TMPDIR and never touches a real store file.
// It proves the three things the log exists for: a record is recoverable after a
// clean sync, an incomplete TRAILING record is dropped rather than obeyed, and a
// complete record with a bad checksum is reported as corruption rather than skipped.
//
// Region contract matrix: Wal_3 and Wal(...3) bind existing regular storage only;
// pristine/header/bounds admission, exact fill/full/reopen epochs, once-only
// counted refusal, replay/callback rejection with append-position preservation,
// checkpoint refusal, every partial/full uncommitted body of a 37-byte record,
// each torn leading-marker prefix (fail closed), committed length corruption, short
// region tail, corrupt committed payload, overflow and surrounding canaries.
// A shorter retry after a discarded tail must survive reopen. Constructors 0/1
// and existing standalone checkpoint/replay behavior are retained below.
// Gaps: OS write/fsync failure injection, actual power-loss durability, hostile
// aliases (excluded by contract), other platforms, and legal host concurrency
// (all operations require external serialization, no internal concurrency claim).
#include "wal/wal.h"
#include "util/crc32.h"
#include "nio/mem.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RECORDS 8u

static char path[512];
static u32 seenOps[RECORDS];
static u64 seenSeq[RECORDS];
static u8 seenPayload[RECORDS][64];
static u32 seenLength[RECORDS];
static u32 seenCount = 0u;

static bool collect(void *context, const WalRecord *record) {
    (void) context;
    assert(seenCount < RECORDS);
    seenOps[seenCount] = (*record).op;
    seenSeq[seenCount] = (*record).sequence;
    seenLength[seenCount] = (*record).length;
    if ((*record).length > 0u && (*record).length <= sizeof seenPayload[0])
        memcpy(seenPayload[seenCount], (*record).payload, (*record).length);
    ++seenCount;
    return true;
}

static u32 stopCalls = 0u;
static bool stopAfterOne(void *context, const WalRecord *record) {
    (void) context;
    assert((*record).sequence == 1u);   // it was handed the FIRST record, then stopped
    ++stopCalls;
    return false;   // the visitor may stop the walk; the log itself is still intact
}

static void resetSeen(void) { seenCount = 0u; memset(seenPayload, 0, sizeof seenPayload); }

static void makePath(void) {
    const char *dir = getenv("TMPDIR");
    (void) snprintf(path, sizeof path, "%s/re-wal-%d.log", dir != nullptr ? dir : "/tmp", (int) getpid());
    (void) unlink(path);
}

/* Overwrite bytes at an absolute offset without going through the log. */
static void poke(bytes offset, const u8 *bytes, bytes length) {
    FILE *f = fopen(path, "r+b.sh");
    assert(f != nullptr);
    assert(fseek(f, (long) offset, SEEK_SET) == 0);
    assert(fwrite(bytes, 1, length, f) == length);
    assert(fclose(f) == 0);
}

/* Append raw bytes at the end, as a crash mid-append would leave them. */
static void appendRaw(const u8 *bytes, bytes length) {
    FILE *f = fopen(path, "ab");
    assert(f != nullptr);
    assert(fwrite(bytes, 1, length, f) == length);
    assert(fclose(f) == 0);
}

static long fileSize(void) {
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    (void) fclose(f);
    return size;
}

#define REGION_OFFSET 128u
#define REGION_CAPACITY 256u
#define REGION_FILE_BYTES 512u

static void regionFile(void) {
    FILE *f = fopen(path, "wb");
    assert(f != nullptr);
    u8 bytes[REGION_FILE_BYTES] = {0};
    memset(bytes, 0x5a, REGION_OFFSET);
    memset(bytes + REGION_OFFSET + REGION_CAPACITY, 0x6b,
           REGION_FILE_BYTES - REGION_OFFSET - REGION_CAPACITY);
    assert(fwrite(bytes, 1u, sizeof bytes, f) == sizeof bytes);
    assert(fclose(f) == 0);
}

static void regionGuards(void) {
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    u8 bytes[REGION_FILE_BYTES];
    assert(fread(bytes, 1u, sizeof bytes, f) == sizeof bytes);
    assert(fclose(f) == 0);
    for (bytes i = 0u; i < REGION_OFFSET; ++i)
        assert(bytes[i] == 0x5a);
    for (bytes i = REGION_OFFSET + REGION_CAPACITY; i < sizeof bytes; ++i)
        assert(bytes[i] == 0x6b);
    assert(fileSize() == REGION_FILE_BYTES);
}

static void regionCases(void) {
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
    // Capture the once-per-epoch refusal diagnostic and assert no second line.
    FILE *errors = tmpfile();
    assert(errors != nullptr);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(errors), STDERR_FILENO) >= 0);
    assert(Wal_append(log, WAL_OP_UPDATE, payload, sizeof payload, &seq) == WAL_FULL);
    assert(Wal_append(log, WAL_OP_UPDATE, payload, sizeof payload, &seq) == WAL_FULL);
    assert(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) >= 0);
    assert(close(saved) == 0);
    rewind(errors);
    char diagnostic[512];
    bytes used = fread(diagnostic, 1u, sizeof diagnostic - 1u, errors);
    diagnostic[used] = '\0';
    assert(strstr(diagnostic, "region exhausted, request") != nullptr);
    char *line = strchr(diagnostic, '\n');
    assert(line != nullptr && line[1] == '\0');
    assert(fclose(errors) == 0);
    assert(Wal_exhaustionCount(log) == 2u && seq == 99u);
    assert(Wal_remaining(log) == remaining && Wal_sequence(log) == 1u);
    // Fill exactly to the capacity boundary, reopen, and refuse one more record.
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

    // Every body write cut has a ZERO leading marker and is ignored/overwritten.
    // Obtain real format bytes once; publish body bytes without its leading marker.
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
    // Publication itself may tear; any nonzero invalid fixed marker fails closed.
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
    // A committed leading marker makes EVERY malformed length corruption, never
    // a discardable crash tail. Cover both out-of-region and in-region misframing.
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
    // Headerless non-pristine data is NOT a fresh log, even if it is a torn tail.
    regionFile();
    poke(REGION_OFFSET + WAL_HEADER_BYTES, record, 7u);
    log = Wal_3(path, REGION_OFFSET, REGION_CAPACITY);
    assert(Wal_code(log) == WAL_CORRUPT && !Wal_isOpen(log));
    Wal_free(log);
    // A physically short remaining extent cannot cause a read outside its region.
    regionFile();
    log = Wal_3(path, REGION_OFFSET, WAL_HEADER_BYTES + 7u);
    assert(Wal_isOpen(log));
    Wal_free(log);
    poke(REGION_OFFSET + WAL_HEADER_BYTES, record, 7u);
    log = Wal_3(path, REGION_OFFSET, WAL_HEADER_BYTES + 7u);
    assert(!Wal_isOpen(log) && Wal_code(log) == WAL_CORRUPT);
    Wal_free(log);
    // Forge only the documented on-disk header to reach sequence exhaustion.
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

int main(void) {
    /* An unopened owner is CLOSED on every operation; arguments are a different code. */
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
    assert(fileSize() == (long) WAL_HEADER_BYTES);   // a fresh log is header-only

    assert(Wal_append(log, WAL_OP_INSERT, (const u8*) "alpha", 5u, nullptr) == WAL_OK);
    assert(Wal_append(log, WAL_OP_UPDATE, (const u8*) "beta!", 5u, nullptr) == WAL_OK);
    u64 third = 0u;
    assert(Wal_append(log, WAL_OP_DELETE, (const u8*) "gamma", 5u, &third) == WAL_OK);
    assert(third == 3u);
    assert(Wal_sequence(log) == 3u && Wal_count(log) == 3u);

    /* The durability boundary is also the point at which ANOTHER handle can see the
     * bytes: stdio buffers writes, so an external size measurement is only honest
     * after a flush. Sync first, then measure. */
    assert(Wal_sync(log) == WAL_OK);
    long afterThree = fileSize();
    assert(afterThree == (long) (WAL_HEADER_BYTES + 3u * (WAL_RECORD_FIXED + 5u + 4u)));

    /* Argument refusals never write; the file grows only for a legal record. */
    assert(Wal_append(log, 0u, (const u8*) "x", 1u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, 99u, (const u8*) "x", 1u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, WAL_OP_INSERT, nullptr, 4u, nullptr) == WAL_INVALID);
    assert(Wal_append(log, WAL_OP_INSERT, (const u8*) "x", WAL_PAYLOAD_MAX + 1u, nullptr) == WAL_INVALID);
    assert(fileSize() == afterThree);         // every refusal left the log untouched
    assert(Wal_code(log) == WAL_INVALID);     // the last refusal is recorded
    assert(Wal_append(log, WAL_OP_INSERT, nullptr, 0u, nullptr) == WAL_OK);   // empty payload is legal

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
    assert(stopCalls == 1u);              // stopped after the first, and that is not a failure

    assert(Wal_replay(reopened, nullptr, 0u, collect, nullptr) == WAL_INVALID);
    /* A record larger than the caller's buffer is refused, never truncated. */
    assert(Wal_append(reopened, WAL_OP_INSERT, (const u8*) "0123456789", 10u, nullptr) == WAL_OK);
    u8 tiny[4];
    assert(Wal_replay(reopened, tiny, sizeof tiny, collect, nullptr) == WAL_INVALID);
    Wal_free(reopened);

    /* A TORN TAIL is dropped, not obeyed: a crash mid-append looks exactly like this. */
    static const u8 torn[7] = { 'V', 'E', 'X', 'W', 1u, 0u, 0u };   // a partial prefix
    appendRaw(torn, sizeof torn);
    Wal *tornLog = Wal_1(path);
    assert(tornLog != nullptr);
    assert(Wal_tailTorn(tornLog));
    assert(Wal_sequence(tornLog) == 5u && Wal_count(tornLog) == 5u);   // the real records only
    assert(Wal_code(tornLog) == WAL_TRUNCATED);
    resetSeen();
    assert(Wal_replay(tornLog, scratch, sizeof scratch, collect, nullptr) == WAL_TRUNCATED);
    assert(seenCount == 5u);
    Wal_free(tornLog);

    /* Cut the torn bytes away, then prove CHECKPOINT retires the log without ever
     * restarting the sequence. */
    assert(truncate(path, afterThree) == 0);
    Wal *clean = Wal_1(path);
    assert(clean != nullptr && !Wal_tailTorn(clean));
    assert(Wal_sequence(clean) == 3u && Wal_count(clean) == 3u);
    assert(Wal_sync(clean) == WAL_OK);
    assert(Wal_checkpoint(clean) == WAL_OK);
    assert(Wal_count(clean) == 0u && Wal_sequence(clean) == 3u && Wal_baseSequence(clean) == 3u);
    assert(fileSize() == (long) WAL_HEADER_BYTES);              // header-only again
    assert(Wal_append(clean, WAL_OP_INSERT, (const u8*) "delta", 5u, &third) == WAL_OK);
    assert(third == 4u);                                        // the sequence NEVER restarts
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

    /* A COMPLETE record with a bad checksum is CORRUPTION, never a skip: a hole in a
     * log destroys the order the log exists to guarantee. */
    assert(fileSize() > (long) (WAL_HEADER_BYTES + WAL_RECORD_FIXED));
    u8 flipped[1];
    {
        FILE *f = fopen(path, "rb");
        assert(f != nullptr);
        assert(fseek(f, (long) (WAL_HEADER_BYTES + WAL_RECORD_FIXED), SEEK_SET) == 0);
        assert(fread(flipped, 1, 1, f) == 1u);
        (void) fclose(f);
    }
    flipped[0] ^= 0x5Au;
    poke(WAL_HEADER_BYTES + WAL_RECORD_FIXED, flipped, 1u);

    Wal *corrupt = Wal_1(path);
    assert(corrupt != nullptr);
    assert(Wal_code(corrupt) == WAL_CORRUPT);
    assert(Wal_replay(corrupt, scratch, sizeof scratch, collect, nullptr) == WAL_CORRUPT);
    Wal_free(corrupt);

    /* A corrupt HEADER cannot be used either. */
    static const u8 badMagic[4] = { 0u, 0u, 0u, 0u };
    poke(0u, badMagic, sizeof badMagic);
    Wal *badHeader = Wal_1(path);
    assert(badHeader != nullptr);
    assert(Wal_code(badHeader) == WAL_CORRUPT);
    Wal_free(badHeader);

    /* Projections are bounded and report truncation rather than cutting silently. */
    makePath();
    Wal *final = Wal_1(path);
    assert(final != nullptr);
    char text[96];
    bool truncated = true;
    assert(Wal_toString(final, text, sizeof text, &truncated));
    assert(!truncated && strstr(text, "Wal(") != nullptr);
    char structText[1024];   // the struct dump includes the (long) temp path
    assert(Wal_toStringStruct(final, structText, sizeof structText, &truncated));
    assert(!truncated && strstr(structText, "path=") != nullptr);
    assert(strstr(structText, "baseSequence=") != nullptr && strstr(structText, "tailTorn=") != nullptr);
    char tinyText[6];
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
