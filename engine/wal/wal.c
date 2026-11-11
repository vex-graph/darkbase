#include "wal/wal.h"
#include "lifetime/platform.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if defined(VEX_PLATFORM_WINDOWS)
#include <io.h>
#else
#include <unistd.h>
#endif
#include "annotation/annotation.h"
#include "oop/type.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "util/crc32.h"

;;DEFINITION
/** The write-ahead log. A mutation is recorded here BEFORE the store touches its
 * data, and the log is synced BEFORE the mutation is acknowledged; on recovery the
 * records replay in order so work whose data pages never landed can be redone. That
 * ordering — record, sync, then mutate — is the whole of crash safety, and it is the
 * caller's discipline to keep, which is why `append` deliberately does not sync.
 *
 * An incomplete trailing record is DROPPED: a torn tail is exactly what a crash
 * mid-append looks like, and obeying it would apply half an intent. A complete
 * record whose checksum fails is CORRUPTION and is reported, never skipped — a hole
 * in a log destroys the order the log exists to guarantee.
 *
 * Bytes are written in memory order through explicit little-endian encoders rather
 * than by copying a struct, so the on-disk layout is the format definition itself
 * and cannot drift with struct padding. The host floor is little-endian, so no
 * byte-order detection or swapping exists anywhere in this file.
 *
 * IO is the engine's `File`; the checksum is the engine's `Crc32`. Neither is
 * reimplemented here, and append/replay allocate nothing: append writes the record straight
 * from the caller's span, and replay streams payloads through a buffer the CALLER
 * supplies, refusing a record too large for it instead of truncating it.
 *
 * Embedded v3 mode borrows an existing regular-file extent, never creates or
 * truncates. Header admission initializes only an entirely zero region. Each
 * record's zero leading marker + prefix/payload/CRC is synced before its fixed
 * leading commit marker is published;
 * Wal_sync must make that marker durable before the caller applies redo. A missing
 * marker is discarded on reopen, while a committed checksum failure closes
 * admission. Failed IO poisons the owner until reopen. The trusted host excludes
 * aliases for the full lifetime; this is not a hostile-writer sandbox. All IO,
 * including fsync, may block and stays off realtime/bounded-join paths. */
;;OVERVIEW
/** ========================================================================
 * CLASS: Wal (wal/wal.c)
 * STRUCT FIELDS (Mirroring wal/wal.h)
 *   void *file            engine File* owner (void* keeps this header IO-free)
 *   char path[1024]       the log's path, copied from the caller
 *   u64 baseSequence highest sequence that a checkpoint has retired
 *   u64 sequence     highest sequence written or recovered
 *   u32 count        records appended since the last checkpoint
 *   u32 recovered    records delivered by the last successful replay
 *   i32 lastCode      last WalCode refusal
 *   bool tailTorn         recovery found an incomplete trailing record
 *   bool open             owner holds a live File
 *   u64 offset       absolute start of embedded extent; standalone zero
 *   u64 capacity     caller-selected finite extent including header
 *   u64 end          relative append cursor, open/append adoption only
 *   u64 exhaustionCount refused capacity admissions in this owner epoch
 *   bool region           v3 embedded format rather than v1 standalone
 *   bool poisoned         IO/corruption requires reopening before append
 * LAYOUT (byte-native, little-endian, memory order)
 *   header 24B: magic u32, version u16, reserved u16, baseSequence u64,
 *               reserved u32, crc u32 over the first 20
 *   record:     magic u32, op u32, sequence u64, length u32, payload[length],
 *               crc u32 over the 20 prefix bytes plus the payload
 *   embedded v3: same header with version 3; record begins with marker u64,
 *               prefix20 at offset8, payload at28, CRC at28 + payload length.
 *               total record bytes = 32 + payload length; header occupies 24B.
 * FUNCTION REGISTRY (exports)
 *   Constructors: Wal *Wal_0(void), Wal *Wal_1(const char *path)
 *                 Wal *Wal_3(const char *path, u64 offset, u64 capacity)
 *   Lifecycle:    void Wal_free(Wal *self)
 *                 bool Wal_isOpen(const Wal *self)
 *                 const char *Wal_path(const Wal *self)
 *                 WalCode Wal_code(const Wal *self)
 *   Appends:      WalCode Wal_append(Wal *self, u32 op,
 *                   const u8 *payload, u32 length, u64 *outSequence)
 *   Durability:   WalCode Wal_sync(Wal *self), WalCode Wal_checkpoint(Wal *self)
 *   Recovery:     WalCode Wal_replay(Wal *self, u8 *scratch, bytes scratchCap,
 *                   WalVisitFn visit, void *context)
 *   Queries:      u64 Wal_sequence(const Wal *self)
 *                 u64 Wal_baseSequence(const Wal *self)
 *                 u32 Wal_count(const Wal *self)
 *                 u32 Wal_recovered(const Wal *self)
 *                 bool Wal_tailTorn(const Wal *self)
 *                 const char *Wal_codeName(WalCode code)
 *                 u64 Wal_remaining(const Wal *self)
 *                 u64 Wal_exhaustionCount(const Wal *self)
 *   Projections:  bool Wal_toString(const Wal *self, char *dest, bytes capacity,
 *                   bool *outTruncated)
 *                 bool Wal_toStringStruct(const Wal *self, char *dest,
 *                   bytes capacity, bool *outTruncated)
 * PRIVATE HELPERS
 *   putU32/putU64/getU32/getU64   explicit memory-order encoders (the format)
 *   encodeHeader/writeHeader      header bytes + checksum
 *   readExact/writeExact          short-read/short-write loops over File
 *   scanHeader(self)              verify and adopt the file header
 *   scanRecords(self, scratch, cap, visit, ctx)   the one recovery walk
 *   syncFile(self)                flush plus the host's fsync
 *   regionRead(self, at, dest, size) extent-bounded seek/read
 *   zeroBytes(bytes, size)         pristine byte-span admission
 *   scanRegion(self, scratch, cap, visit, context, adopt) committed v3 walk
 *   allocWal(path), writeNull(dest, capacity, outTruncated) allocation/projection
 *   quotePath(path, dest)          bounded owned-path escaping for projections
 * CONTRACT: null/closed owners and illegal arguments are refused through THROW with
 * the owner untouched. Argument refusal never leaves a half-written record: the record is
 * validated before the first byte is written. `checkpoint` requires the caller to
 * have made the data durable first; the sequence never restarts after one.
 * Region checkpoint refuses unchanged. Capacity refusal reports once and counts
 * always, reset by constructing a new owner. Replay never adopts an append cursor;
 * region callback false returns INVALID, including on the final record. Actual IO
 * failure may leave an uncommitted tail, poisons admission, and requires reopen;
 * validation/capacity refusal preserves disk, sequence and output parameters.
 * Derived backing/cursors/identity mutate only through lifecycle/append/replay,
 * not arbitrary setters (Conflict Triage Law + Single Class Per File Law).
 * ======================================================================== */

;;INTENTION("derived WAL extent/cursor/backing and publication state change only through lifecycle operations; raw setters would forge committed journal truth, per the Conflict Triage Law + Single Class Per File Law (Java Law)")

#define WAL_READ_CHUNK 4096u

static void putU32(u8 *out, u32 value) {
    out[0] = (u8) value;
    out[1] = (u8) (value >> 8);
    out[2] = (u8) (value >> 16);
    out[3] = (u8) (value >> 24);
}

static void putU64(u8 *out, u64 value) {
    for (unsigned i = 0u; i < 8u; i++)
        out[i] = (u8) (value >> (i * 8u));
}

static u32 getU32(const u8 *in) {
    return (u32) in[0] | ((u32) in[1] << 8) | ((u32) in[2] << 16) |
           ((u32) in[3] << 24);
}

static u64 getU64(const u8 *in) {
    u64 value = 0u;
    for (unsigned i = 0u; i < 8u; i++)
        value |= (u64) in[i] << (i * 8u);
    return value;
}

static bool readExact(File *file, void *dest, bytes length) {
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

static bool writeExact(File *file, const void *src, bytes length) {
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

static bool syncFile(Wal *self) {
    File *file = (File*) (*self).file;
    if (!File_flush(file))
        return false;
#if defined(VEX_PLATFORM_WINDOWS)
    return _commit(_fileno(File_handle(file))) == 0;
#else
    return fsync(fileno(File_handle(file))) == 0;
#endif
}

static void encodeHeader(const Wal *self, u8 out[WAL_HEADER_BYTES]) {
    putU32(&out[0], WAL_MAGIC);
    out[4] = (u8) ((*self).region ? WAL_REGION_VERSION : WAL_VERSION);
    out[5] = 0u;
    out[6] = 0u;
    out[7] = 0u;
    putU64(&out[8], (*self).baseSequence);
    putU32(&out[16], 0u);
    putU32(&out[20], Crc32_of(out, 20u));
}

static bool writeHeader(Wal *self) {
    u8 header[WAL_HEADER_BYTES];
    encodeHeader(self, header);
    if (!File_seek((File*) (*self).file, (int64_t) (*self).offset))
        return false;
    return writeExact((File*) (*self).file, header, WAL_HEADER_BYTES);
}

static WalCode scanHeader(Wal *self) {
    u8 header[WAL_HEADER_BYTES];
    if (!File_seek((File*) (*self).file, 0))
        return WAL_IO;
    int64_t size = File_size((File*) (*self).file);
    if (size == 0) {
        // A brand-new log: write the header and start from sequence zero.
        (*self).baseSequence = 0u;
        (*self).sequence = 0u;
        (*self).count = 0u;
        if (!writeHeader(self))
            return WAL_IO;
        return WAL_OK;
    }
    if (size < (int64_t) WAL_HEADER_BYTES || !readExact((File*) (*self).file, header, WAL_HEADER_BYTES))
        return WAL_CORRUPT;
    if (getU32(&header[0]) != WAL_MAGIC || header[4] != (u8) WAL_VERSION)
        return WAL_CORRUPT;
    if (getU32(&header[20]) != Crc32_of(header, 20u))
        return WAL_CORRUPT;
    (*self).baseSequence = getU64(&header[8]);
    (*self).sequence = (*self).baseSequence;
    (*self).count = 0u;
    return WAL_OK;
}

/**
 * The single recovery walk. With a null visitor it only validates and counts; with a
 * visitor it delivers each payload through `scratch`, which must hold the largest
 * record. Every complete record is checksum-verified and its sequence must follow
 * the previous one exactly, because a gap means the order cannot be trusted.
 */
static WalCode scanRecords(Wal *self, u8 *scratch, bytes scratchCap,
                           WalVisitFn visit, void *context, bool adopt) {
    File *file = (File*) (*self).file;
    u64 expected = (*self).baseSequence;
    u32 seen = 0u;
    u32 delivered = 0u;
    if (!File_seek(file, (int64_t) WAL_HEADER_BYTES))
        return WAL_IO;
    for (;;) {
        u8 prefix[WAL_RECORD_FIXED];
        u8 trailer[4];
        int64_t prefixBytes = File_read(file, prefix, (int64_t) WAL_RECORD_FIXED);
        if (prefixBytes <= 0)
            break;                       // clean end at a record boundary
        if (prefixBytes < (int64_t) WAL_RECORD_FIXED) {
            (*self).tailTorn = true;     // a crash tore this record's prefix
            break;
        }
        if (getU32(&prefix[0]) != WAL_MAGIC) {
            (*self).tailTorn = true;     // only the final prefix can be this broken
            break;
        }
        u32 op = getU32(&prefix[4]);
        u64 sequence = getU64(&prefix[8]);
        u32 length = getU32(&prefix[16]);
        if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT || length > WAL_PAYLOAD_MAX)
            return WAL_CORRUPT;
        if (expected == UINT64_MAX || sequence != expected + 1u)
            return WAL_CORRUPT;          // a gap or a repeat is not a valid log
        u32 crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
        u32 remaining = length;
        bytes offset = 0u;
        while (remaining > 0u) {
            u8 chunk[WAL_READ_CHUNK];
            u32 want = remaining < WAL_READ_CHUNK ? remaining : WAL_READ_CHUNK;
            if (!readExact(file, chunk, want)) {
                (*self).tailTorn = true;
                break;
            }
            crc = Crc32_update(crc, chunk, want);
            if (scratch != nullptr && offset + want <= scratchCap) {
                memcpy(scratch + offset, chunk, want);
                offset += want;
            }
            remaining -= want;
        }
        if ((*self).tailTorn)
            break;
        if (!readExact(file, trailer, 4u)) {
            (*self).tailTorn = true;
            break;
        }
        if (Crc32_finalize(crc) != getU32(trailer))
            return WAL_CORRUPT;          // complete record, bad checksum: never skip
        if (visit != nullptr) {
            if (length > scratchCap)
                return WAL_INVALID;      // refused, not truncated
            WalRecord record = { op, sequence, scratch, length };
            ++delivered;
            if (!visit(context, &record))
                break;
        }
        expected = sequence;
        ++seen;
        // Only opening and recovery move the APPEND position. A replay is a
        // read-only walk: if it moved the position, a replay refused for a small
        // buffer would make the next append reuse a sequence.
        if (adopt) {
            (*self).sequence = sequence;
            (*self).count = seen;
        }
    }
    if (visit != nullptr)
        (*self).recovered = delivered;   // how many the visitor actually received
    return (*self).tailTorn ? WAL_TRUNCATED : WAL_OK;
}

static bool regionRead(Wal *self, u64 at, void *dest, bytes size) {
    return at <= (*self).capacity && size <= (*self).capacity - at &&
           File_seek((File*) (*self).file, (int64_t) ((*self).offset + at)) &&
           readExact((File*) (*self).file, dest, size);
}

static bool zeroBytes(const u8 *bytes, bytes size) {
    for (bytes i = 0u; i < size; ++i)
        if (bytes[i] != 0u)
            return false;
    return true;
}

// This walk never publishes its cursor except during initial open. A missing
// marker is an uncommitted tail, not a redo intent. A present marker requires CRC.
static WalCode scanRegion(Wal *self, u8 *scratch, bytes cap,
                          WalVisitFn visit, void *context, bool adopt) {
    u64 at = WAL_HEADER_BYTES;
    u64 expected = (*self).baseSequence;
    u32 seen = 0u;
    (*self).recovered = 0u;
    (*self).tailTorn = false;
    while (at < (*self).capacity) {
        u8 marker[WAL_COMMIT_BYTES] = {0};
        u64 room = (*self).capacity - at;
        bytes n = room < WAL_COMMIT_BYTES ? (bytes) room : WAL_COMMIT_BYTES;
        if (!regionRead(self, at, marker, n))
            return WAL_IO;
        if (zeroBytes(marker, n)) {
            // Zero publication marker means no committed record, regardless of
            // untrusted body bytes. Distinguish pristine space from a torn body.
            u8 bytes[WAL_READ_CHUNK];
            for (u64 pos = at + n; pos < (*self).capacity;) {
                bytes take = (*self).capacity - pos < sizeof bytes ?
                              (bytes) ((*self).capacity - pos) : sizeof bytes;
                if (!regionRead(self, pos, bytes, take))
                    return WAL_IO;
                if (!zeroBytes(bytes, take)) {
                    (*self).tailTorn = true;
                    break;
                }
                pos += take;
            }
            break;
        }
        if (n != WAL_COMMIT_BYTES || getU64(marker) != WAL_COMMIT_MAGIC ||
            room < WAL_COMMIT_BYTES + WAL_RECORD_FIXED + 4u)
            return WAL_CORRUPT;
        u8 prefix[WAL_RECORD_FIXED];
        if (!regionRead(self, at + WAL_COMMIT_BYTES, prefix, sizeof prefix))
            return WAL_IO;
        u32 length = getU32(prefix + 16);
        u64 total = WAL_RECORD_FIXED + (u64) length + 4u + WAL_COMMIT_BYTES;
        if (total > room)
            return WAL_CORRUPT;
        u32 op = getU32(prefix + 4);
        u64 sequence = getU64(prefix + 8);
        if (getU32(prefix) != WAL_MAGIC || length > WAL_PAYLOAD_MAX ||
            op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT || expected == UINT64_MAX ||
            sequence != expected + 1u || seen == UINT32_MAX)
            return WAL_CORRUPT;
        u32 crc = Crc32_update(CRC32_SEED, prefix, sizeof prefix);
        for (u32 pos = 0u; pos < length;) {
            u8 bytes[WAL_READ_CHUNK];
            bytes take = length - pos < sizeof bytes ? length - pos : sizeof bytes;
            if (!regionRead(self, at + WAL_COMMIT_BYTES + WAL_RECORD_FIXED + pos, bytes, take))
                return WAL_IO;
            crc = Crc32_update(crc, bytes, take);
            if (scratch != nullptr && length <= cap)
                memcpy(scratch + pos, bytes, take);
            pos += (u32) take;
        }
        u8 trailer[4];
        if (!regionRead(self, at + WAL_COMMIT_BYTES + WAL_RECORD_FIXED + length, trailer, sizeof trailer))
            return WAL_IO;
        if (getU32(trailer) != Crc32_finalize(crc))
            return WAL_CORRUPT;
        if (visit != nullptr) {
            if (length > cap)
                return WAL_INVALID;
            WalRecord record = {op, sequence, scratch, length};
            if (!visit(context, &record))
                return WAL_INVALID;
            ++(*self).recovered;
        }
        at += total;
        expected = sequence;
        ++seen;
    }
    if (adopt) {
        (*self).end = at;
        (*self).sequence = expected;
        (*self).count = seen;
    }
    return (*self).tailTorn ? WAL_TRUNCATED : WAL_OK;
}

static Wal *allocWal(const char *path) {
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    bytes length = strlen(path);
    if (length == 0u || length >= WAL_PATH_MAX) {
        THROW("Wal: path is empty or too long");
        Memory_free(self);
        return nullptr;
    }
    memcpy((*self).path, path, length + 1u);
    return self;
}

Wal *Wal_0(void) {
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal_0: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

Wal *Wal_1(const char *path) {
    if (path == nullptr) {
        THROW("Wal_1: null path");
        return nullptr;
    }
    Wal *self = allocWal(path);
    if (self == nullptr)
        return nullptr;
    File *file = File_open((*self).path, FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE);
    if (file == nullptr) {
        THROW("Wal_1: the log could not be opened");
        (*self).lastCode = WAL_IO;
        (*self).file = nullptr;
        return self;
    }
    (*self).file = file;
    (*self).open = true;
    WalCode code = scanHeader(self);
    if (code == WAL_OK)
        code = scanRecords(self, nullptr, 0u, nullptr, nullptr, true);
    (*self).lastCode = code;
    if (code == WAL_CORRUPT) {
        // The tail scan cannot corrupt anything, but a bad header cannot be used.
        THROW("Wal_1: the log header or a record is corrupt");
    }
    return self;
}

Wal *Wal_3(const char *path, u64 offset, u64 capacity) {
    if (path == nullptr || capacity < WAL_HEADER_BYTES || offset > INT64_MAX ||
        capacity > (u64) INT64_MAX - offset) {
        THROW("Wal_3: invalid path or region extent");
        return nullptr;
    }
    Wal *self = allocWal(path);
    if (self == nullptr)
        return nullptr;
    (*self).region = true;
    (*self).offset = offset;
    (*self).capacity = capacity;
    (*self).end = WAL_HEADER_BYTES;
    File *file = File_open(path, FILE_MODE_READ | FILE_MODE_WRITE);
    (*self).file = file;
    struct stat status;
    WalCode code = WAL_IO;
    if (file != nullptr && fstat(fileno(File_handle(file)), &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_size >= 0 &&
        (u64) status.st_size >= offset + capacity) {
        u8 header[WAL_HEADER_BYTES];
        if (regionRead(self, 0u, header, sizeof header)) {
            code = WAL_CORRUPT;
            if (zeroBytes(header, sizeof header)) {
                // Validate the WHOLE pristine region before initializing anything.
                (*self).open = true;
                code = scanRegion(self, nullptr, 0u, nullptr, nullptr, true);
                if (code == WAL_TRUNCATED)
                    code = WAL_CORRUPT;
                if (code == WAL_OK && (!writeHeader(self) || !syncFile(self)))
                    code = WAL_IO;
            } else if (getU32(header) == WAL_MAGIC && header[4] == WAL_REGION_VERSION &&
                       header[5] == 0u && header[6] == 0u && header[7] == 0u &&
                       getU32(header + 16) == 0u &&
                       getU32(header + 20) == Crc32_of(header, 20u)) {
                (*self).baseSequence = getU64(header + 8);
                code = scanRegion(self, nullptr, 0u, nullptr, nullptr, true);
            }
        }
    }
    (*self).lastCode = code;
    (*self).open = code == WAL_OK || code == WAL_TRUNCATED;
    (*self).poisoned = !(*self).open;
    if (!(*self).open)
        THROW("Wal_3: region admission failed (%s)", Wal_codeName(code));
    return self;
}

void Wal_free(Wal *self) {
    if (self == nullptr)
        return;
    if ((*self).file != nullptr)
        File_close((File*) (*self).file);
    Memory_free(self);
}

bool Wal_isOpen(const Wal *self) { return self != nullptr && (*self).open; }
const char *Wal_path(const Wal *self) { return self == nullptr ? "" : (*self).path; }
WalCode Wal_code(const Wal *self) { return self == nullptr ? WAL_INVALID : (WalCode) (*self).lastCode; }
u64 Wal_sequence(const Wal *self) { return self == nullptr ? 0u : (*self).sequence; }
u64 Wal_baseSequence(const Wal *self) { return self == nullptr ? 0u : (*self).baseSequence; }
u32 Wal_count(const Wal *self) { return self == nullptr ? 0u : (*self).count; }
u32 Wal_recovered(const Wal *self) { return self == nullptr ? 0u : (*self).recovered; }
bool Wal_tailTorn(const Wal *self) { return self != nullptr && (*self).tailTorn; }
u64 Wal_remaining(const Wal *self) {
    return self == nullptr || !(*self).open ? 0u :
           ((*self).region ? (*self).capacity - (*self).end : UINT64_MAX);
}
u64 Wal_exhaustionCount(const Wal *self) {
    return self == nullptr ? 0u : (*self).exhaustionCount;
}

WalCode Wal_append(Wal *self, u32 op, const u8 *payload, u32 length,
                   u64 *outSequence) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_append: the owner is not open");
        if (self != nullptr)
            (*self).lastCode = WAL_CLOSED;
        return WAL_CLOSED;
    }
    if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT ||
        (payload == nullptr && length != 0u) || length > WAL_PAYLOAD_MAX) {
        THROW("Wal_append: unknown op, null payload or an over-large payload");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    File *file = (File*) (*self).file;
    if ((*self).poisoned || (*self).sequence == UINT64_MAX || (*self).count == UINT32_MAX) {
        THROW("Wal_append: poisoned owner or sequence/count overflow");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    u64 total = WAL_RECORD_FIXED + (u64) length + 4u + WAL_COMMIT_BYTES;
    if ((*self).region && total > Wal_remaining(self)) {
        if ((*self).exhaustionCount++ == 0u)
            THROW("Wal_append: region exhausted, request %llu bytes (remaining %llu)",
                  (unsigned long long) total, (unsigned long long) Wal_remaining(self));
        (*self).lastCode = WAL_FULL;
        return WAL_FULL;
    }
    u64 sequence = (*self).sequence + 1u;
    u8 prefix[WAL_RECORD_FIXED];
    putU32(&prefix[0], WAL_MAGIC);
    putU32(&prefix[4], op);
    putU64(&prefix[8], sequence);
    putU32(&prefix[16], length);
    u32 crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
    crc = Crc32_update(crc, payload, length);
    u8 trailer[4];
    putU32(trailer, Crc32_finalize(crc));
    // Whole record validated above; only now does the file get touched. The end is
    // REFRESHED, not read from the cached size: after a checkpoint truncated the log
    // a stale size would position this record past the new end and leave a hole.
    if ((*self).region && (*self).tailTorn) {
        // Clear discarded bytes before a shorter replacement can expose stale markers.
        u8 zeros[WAL_READ_CHUNK] = {0};
        bool cleared = File_seek(file, (int64_t) ((*self).offset + (*self).end));
        for (u64 pos = (*self).end; cleared && pos < (*self).capacity;) {
            bytes take = (*self).capacity - pos < sizeof zeros ?
                          (bytes) ((*self).capacity - pos) : sizeof zeros;
            cleared = writeExact(file, zeros, take);
            pos += take;
        }
        if (!cleared || !syncFile(self)) {
            (*self).poisoned = true;
            (*self).lastCode = WAL_IO;
            THROW("Wal_append: discarded tail could not be cleared");
            return WAL_IO;
        }
    }
    int64_t position = (*self).region ? (int64_t) ((*self).offset + (*self).end) : File_refreshSize(file);
    u8 zeroMarker[WAL_COMMIT_BYTES] = {0};
    if (!File_seek(file, position) ||
        ((*self).region && !writeExact(file, zeroMarker, sizeof zeroMarker)) ||
        !writeExact(file, prefix, WAL_RECORD_FIXED) ||
        (length > 0u && !writeExact(file, payload, length)) || !writeExact(file, trailer, 4u)) {
        THROW("Wal_append: the record could not be written");
        (*self).lastCode = WAL_IO;
        (*self).poisoned = (*self).region;
        return WAL_IO;
    }
    if ((*self).region) {
        u8 marker[WAL_COMMIT_BYTES];
        putU64(marker, WAL_COMMIT_MAGIC);
        if (!syncFile(self) || !File_seek(file, position) || !writeExact(file, marker, sizeof marker)) {
            (*self).poisoned = true;
            (*self).lastCode = WAL_IO;
            THROW("Wal_append: record sync or marker publication failed");
            return WAL_IO;
        }
        (*self).end += total;
        (*self).tailTorn = false;
    }
    ++(*self).count;
    (*self).sequence = sequence;
    if (outSequence != nullptr)
        *outSequence = sequence;
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

WalCode Wal_sync(Wal *self) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_sync: the owner is not open");
        return WAL_CLOSED;
    }
    if ((*self).poisoned) {
        THROW("Wal_sync: poisoned owner requires reopen");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    if (!syncFile(self)) {
        THROW("Wal_sync: the log could not be made durable");
        (*self).lastCode = WAL_IO;
        (*self).poisoned = (*self).region;
        return WAL_IO;
    }
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

WalCode Wal_replay(Wal *self, u8 *scratch, bytes scratchCap,
                   WalVisitFn visit, void *context) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_replay: the owner is not open");
        if (self != nullptr)
            (*self).lastCode = WAL_CLOSED;
        return WAL_CLOSED;
    }
    if (visit != nullptr && scratch == nullptr) {
        THROW("Wal_replay: a visitor was given no replay buffer");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    bool retainedTail = (*self).tailTorn;
    (*self).tailTorn = false;
    // No header re-read and no adoption here: the append position was established at
    // open, and a replay must leave it exactly as it found it.
    WalCode code = (*self).region ? scanRegion(self, scratch, scratchCap, visit, context, false) :
                   scanRecords(self, scratch, scratchCap, visit, context, false);
    if ((*self).region)
        (*self).tailTorn = (*self).tailTorn || retainedTail;
    if ((*self).region && code != WAL_OK && code != WAL_TRUNCATED) {
        if (code == WAL_CORRUPT || code == WAL_IO)
            (*self).poisoned = true;
        THROW("Wal_replay: region walk refused (%s)", Wal_codeName(code));
    }
    (*self).lastCode = code;
    return code;
}

WalCode Wal_checkpoint(Wal *self) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_checkpoint: the owner is not open");
        return WAL_CLOSED;
    }
    if ((*self).region) {
        THROW("Wal_checkpoint: embedded region cannot be checkpointed or truncated");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    if (!syncFile(self)) {
        THROW("Wal_checkpoint: the log could not be synced");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    // The caller has already made the DATA durable; only then is the log retired.
    (*self).baseSequence = (*self).sequence;
    (*self).count = 0u;
    if (!writeHeader(self)) {
        THROW("Wal_checkpoint: the header could not be rewritten");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    File *file = (File*) (*self).file;
#if defined(VEX_PLATFORM_WINDOWS)
    if (_chsize_s(_fileno(File_handle(file)), (long long) WAL_HEADER_BYTES) != 0) {
#else
    if (ftruncate(fileno(File_handle(file)), (off_t) WAL_HEADER_BYTES) != 0) {
#endif
        THROW("Wal_checkpoint: the log could not be truncated");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    // The truncation changed the file's length behind the handle's back.
    (void) File_refreshSize(file);
    if (!File_seek(file, (int64_t) WAL_HEADER_BYTES) || !syncFile(self)) {
        THROW("Wal_checkpoint: the truncated log could not be made durable");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

const char *Wal_codeName(WalCode code) {
    switch (code) {
        case WAL_OK:        return "ok";
        case WAL_INVALID:   return "invalid";
        case WAL_IO:        return "io";
        case WAL_CORRUPT:   return "corrupt";
        case WAL_TRUNCATED: return "truncated";
        case WAL_CLOSED:    return "closed";
        case WAL_FULL:      return "full";
        default:            return "unknown";
    }
}

static bool writeNull(char *dest, bytes capacity, bool *outTruncated) {
    const char *text = "nullptr";
    bytes length = strlen(text);
    bool cut = length + 1u > capacity;
    bytes copy = cut ? capacity - 1u : length;
    if (capacity > 0u) {
        memcpy(dest, text, copy);
        dest[copy] = '\0';
    }
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

bool Wal_toString(const Wal *self, char *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    int written = snprintf(dest, capacity, "Wal(open=%d, sequence=%llu, count=%u, base=%llu)",
                           self != nullptr && (*self).open ? 1 : 0,
                           (unsigned long long) Wal_sequence(self), Wal_count(self),
                           (unsigned long long) Wal_baseSequence(self));
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

static void quotePath(const char *path, char dest[WAL_PATH_MAX * 4u + 3u]) {
    static const char hex[] = "0123456789abcdef";
    bytes used = 0u;
    dest[used++] = '"';
    for (bytes i = 0u; i < WAL_PATH_MAX && path[i] != '\0'; ++i) {
        u8 byte = (u8) path[i];
        if (byte == '"' || byte == '\\') {
            dest[used++] = '\\';
            dest[used++] = (char) byte;
        } else if (byte < 32u || byte == 127u) {
            dest[used++] = '\\';
            dest[used++] = 'x';
            dest[used++] = hex[byte >> 4];
            dest[used++] = hex[byte & 15u];
        } else {
            dest[used++] = (char) byte;
        }
    }
    dest[used++] = '"';
    dest[used] = '\0';
}

bool Wal_toStringStruct(const Wal *self, char *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    char path[WAL_PATH_MAX * 4u + 3u];
    quotePath((*self).path, path);
    int written = snprintf(dest, capacity,
                            "Wal{file=<owned>, path=%s, baseSequence=%llu, sequence=%llu, count=%u, "
                            "recovered=%u, lastCode=%s, tailTorn=%d, open=%d, offset=%llu, "
                            "capacity=%llu, end=%llu, exhaustionCount=%llu, region=%d, poisoned=%d}",
                            path,
                            (unsigned long long) Wal_baseSequence(self),
                            (unsigned long long) Wal_sequence(self), Wal_count(self),
                            Wal_recovered(self), Wal_codeName((*self).lastCode),
                            (*self).tailTorn ? 1 : 0, (*self).open ? 1 : 0,
                            (unsigned long long) (*self).offset,
                            (unsigned long long) (*self).capacity,
                            (unsigned long long) (*self).end,
                            (unsigned long long) (*self).exhaustionCount,
                            (*self).region ? 1 : 0, (*self).poisoned ? 1 : 0);
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
