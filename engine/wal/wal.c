#include "vex.h"
#include "wal/wal.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if defined(VEX_PLATFORM_WINDOWS)
#include <io.h>
#else
#include <unistd.h>
#endif
#include "oop/darkbase_type.h"
#include "io/file/file.h"
#include "nio/mem.h"
#include "util/crc32.h"
#include "nio/darkbase_memory.h"

;;DEFINITION
/** The write-ahead log. A mutation is recorded here BEFORE the store touches its
 * data, and the log is synced BEFORE the mutation is acknowledged; on recovery the
 * records replay in order so work whose data pages never landed can be redone. That
 * ordering, record, sync, then mutate, is the whole of crash safety, and it is the
 * caller's discipline to keep, which is why `append` deliberately does not sync.
 *
 * An incomplete trailing record is DROPPED: a torn tail is exactly what a crash
 * mid-append looks like, and obeying it would apply half an intent. A complete
 * record whose checksum fails is CORRUPTION and is reported, never skipped, a hole
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
 *   any file            engine File* owner (any keeps this header IO-free)
 *   utf8 path[1024]       the log's path, copied from the caller
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
 *   Constructors: Wal *Wal_0(), Wal *Wal_1(string path)
 *                 Wal *Wal_3(string path, u64 offset, u64 capacity)
 *   Lifecycle:    fn Wal_free(Wal *self)
 *                 bool Wal_isOpen(const Wal *self)
 *                 string Wal_path(const Wal *self)
 *                 WalCode Wal_code(const Wal *self)
 *   Appends:      WalCode Wal_append(Wal *self, u32 op,
 *                   const u8 *payload, u32 length, u64 *outSequence)
 *   Durability:   WalCode Wal_sync(Wal *self), WalCode Wal_checkpoint(Wal *self)
 *   Recovery:     WalCode Wal_replay(Wal *self, u8 *scratch, bytes scratchCap,
 *                   WalVisitFn visit, any context)
 *   Queries:      u64 Wal_sequence(const Wal *self)
 *                 u64 Wal_baseSequence(const Wal *self)
 *                 u32 Wal_count(const Wal *self)
 *                 u32 Wal_recovered(const Wal *self)
 *                 bool Wal_tailTorn(const Wal *self)
 *                 string Wal_codeName(WalCode code)
 *                 u64 Wal_remaining(const Wal *self)
 *                 u64 Wal_exhaustionCount(const Wal *self)
 *   Projections:  bool Wal_toString(const Wal *self, utf8 *dest, bytes capacity,
 *                   bool *outTruncated)
 *                 bool Wal_toStringStruct(const Wal *self, utf8 *dest,
 *                   bytes capacity, bool *outTruncated)
 * PRIVATE HELPERS
 *   putU32/putU64/getU32/getU64   explicit memory-order encoders (the format)
 *   encodeHeader/writeHeader      header bytes + checksum
 *   readExact/writeExact          i16-read/i16-write loops over File
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

static fn putU32(u8 *out, u32 value) {
    out[0] = (u8) value;
    out[1] = (u8) (value >> 8);
    out[2] = (u8) (value >> 16);
    out[3] = (u8) (value >> 24);
}

static fn putU64(u8 *out, u64 value) {
    for (u32 i = 0u; i < 8u; i++)
        out[i] = (u8) (value >> (i * 8u));
}

static u32 getU32(const u8 *in) {
    return (u32) in[0] | ((u32) in[1] << 8) | ((u32) in[2] << 16) |
           ((u32) in[3] << 24);
}

static u64 getU64(const u8 *in) {
    u64 value = 0u;
    for (u32 i = 0u; i < 8u; i++)
        value |= (u64) in[i] << (i * 8u);
    return value;
}

static bool readExact(File *file, any dest, bytes length) {
    u8 *p = (u8*) dest;
    bytes done = 0u;
    while (done < length) {
        i64 n = File_read(file, p + done, (i64) (length - done));
        if (n <= 0)
            return false;
        done += (bytes) n;
    }
    return true;
}

static bool writeExact(File *file, const_any src, bytes length) {
    const u8 *p = (const u8*) src;
    bytes done = 0u;
    while (done < length) {
        i64 n = File_write(file, p + done, (i64) (length - done));
        if (n <= 0)
            return false;
        done += (bytes) n;
    }
    return true;
}

;;GETTER
static inline File *Wal_file(const Wal *self)
{
    return (File*) (*self).file;
}
;;SETTER
static inline fn Wal_setFile(Wal *self, File *file)
{
    (*self).file = file;
}
;;GETTER
static inline utf8 *Wal_pathMut(Wal *self)
{
    return (*self).path;
}
;;SETTER
static inline fn Wal_setBaseSequence(Wal *self, u64 v)
{
    (*self).baseSequence = v;
}
;;SETTER
static inline fn Wal_setSequence(Wal *self, u64 v)
{
    (*self).sequence = v;
}
;;SETTER
static inline fn Wal_setCount(Wal *self, u32 v)
{
    (*self).count = v;
}
;;SETTER
static inline fn Wal_setRecovered(Wal *self, u32 v)
{
    (*self).recovered = v;
}
;;GETTER
static inline i32 Wal_lastCode(const Wal *self)
{
    return (*self).lastCode;
}
;;SETTER
static inline fn Wal_setLastCode(Wal *self, i32 v)
{
    (*self).lastCode = v;
}
;;SETTER
static inline fn Wal_setTailTorn(Wal *self, bool v)
{
    (*self).tailTorn = v;
}
;;GETTER
static inline bool Wal_open(const Wal *self)
{
    return (*self).open;
}
;;SETTER
static inline fn Wal_setOpen(Wal *self, bool v)
{
    (*self).open = v;
}
;;GETTER
static inline u64 Wal_offset(const Wal *self)
{
    return (*self).offset;
}
;;SETTER
static inline fn Wal_setOffset(Wal *self, u64 v)
{
    (*self).offset = v;
}
;;GETTER
static inline u64 Wal_capacity(const Wal *self)
{
    return (*self).capacity;
}
;;SETTER
static inline fn Wal_setCapacity(Wal *self, u64 v)
{
    (*self).capacity = v;
}
;;GETTER
static inline u64 Wal_end(const Wal *self)
{
    return (*self).end;
}
;;SETTER
static inline fn Wal_setEnd(Wal *self, u64 v)
{
    (*self).end = v;
}
;;SETTER
static inline fn Wal_setExhaustionCount(Wal *self, u64 v)
{
    (*self).exhaustionCount = v;
}
;;GETTER
static inline bool Wal_region(const Wal *self)
{
    return (*self).region;
}
;;SETTER
static inline fn Wal_setRegion(Wal *self, bool v)
{
    (*self).region = v;
}
;;GETTER
static inline bool Wal_poisoned(const Wal *self)
{
    return (*self).poisoned;
}
;;SETTER
static inline fn Wal_setPoisoned(Wal *self, bool v)
{
    (*self).poisoned = v;
}

static bool syncFile(Wal *self) {
    File *file = Wal_file(self);
    if (!File_flush(file))
        return false;
#if defined(VEX_PLATFORM_WINDOWS)
    return _commit(_fileno(File_handle(file))) == 0;
#else
    return fsync(fileno(File_handle(file))) == 0;
#endif
}

static fn encodeHeader(const Wal *self, u8 out[WAL_HEADER_BYTES]) {
    putU32(&out[0], WAL_MAGIC);
    out[4] = (u8) (Wal_region(self) ? WAL_REGION_VERSION : WAL_VERSION);
    out[5] = 0u;
    out[6] = 0u;
    out[7] = 0u;
    putU64(&out[8], Wal_baseSequence(self));
    putU32(&out[16], 0u);
    putU32(&out[20], Crc32_of(out, 20u));
}

static bool writeHeader(Wal *self) {
    u8 header[WAL_HEADER_BYTES];
    encodeHeader(self, header);
    File *file = Wal_file(self);
    if (!File_seek(file, (i64) Wal_offset(self)))
        return false;
    return writeExact(file, header, WAL_HEADER_BYTES);
}

static WalCode scanHeader(Wal *self) {
    u8 header[WAL_HEADER_BYTES];
    File *file = Wal_file(self);
    if (!File_seek(file, 0))
        return WAL_IO;
    i64 size = File_size(file);
    if (size == 0) {
        Wal_setBaseSequence(self, 0u);
        Wal_setSequence(self, 0u);
        Wal_setCount(self, 0u);
        if (!writeHeader(self))
            return WAL_IO;
        return WAL_OK;
    }
    if (size < (i64) WAL_HEADER_BYTES || !readExact(file, header, WAL_HEADER_BYTES))
        return WAL_CORRUPT;
    if (getU32(&header[0]) != WAL_MAGIC || header[4] != (u8) WAL_VERSION)
        return WAL_CORRUPT;
    if (getU32(&header[20]) != Crc32_of(header, 20u))
        return WAL_CORRUPT;
    u64 base = getU64(&header[8]);
    Wal_setBaseSequence(self, base);
    Wal_setSequence(self, base);
    Wal_setCount(self, 0u);
    return WAL_OK;
}

/**
 * The single recovery walk. With a null visitor it only validates and counts; with a
 * visitor it delivers each payload through `scratch`, which must hold the largest
 * record. Every complete record is checksum-verified and its sequence must follow
 * the previous one exactly, because a gap means the order cannot be trusted.
 */
static WalCode scanRecords(Wal *self, u8 *scratch, bytes scratchCap,
                           WalVisitFn visit, any context, bool adopt) {
    File *file = Wal_file(self);
    u64 expected = Wal_baseSequence(self);
    u32 seen = 0u;
    u32 delivered = 0u;
    if (!File_seek(file, (i64) WAL_HEADER_BYTES))
        return WAL_IO;
    for (;;) {
        u8 prefix[WAL_RECORD_FIXED];
        u8 trailer[4];
        i64 prefixBytes = File_read(file, prefix, (i64) WAL_RECORD_FIXED);
        if (prefixBytes <= 0)
            break;
        if (prefixBytes < (i64) WAL_RECORD_FIXED) {
            Wal_setTailTorn(self, true);
            break;
        }
        if (getU32(&prefix[0]) != WAL_MAGIC) {
            Wal_setTailTorn(self, true);
            break;
        }
        u32 op = getU32(&prefix[4]);
        u64 sequence = getU64(&prefix[8]);
        u32 length = getU32(&prefix[16]);
        if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT || length > WAL_PAYLOAD_MAX)
            return WAL_CORRUPT;
        if (expected == UINT64_MAX || sequence != expected + 1u)
            return WAL_CORRUPT;
        u32 crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
        u32 remaining = length;
        bytes offset = 0u;
        while (remaining > 0u) {
            u8 chunk[WAL_READ_CHUNK];
            u32 want = remaining < WAL_READ_CHUNK ? remaining : WAL_READ_CHUNK;
            if (!readExact(file, chunk, want)) {
                Wal_setTailTorn(self, true);
                break;
            }
            crc = Crc32_update(crc, chunk, want);
            if (scratch != nullptr && offset + want <= scratchCap) {
                memcpy(scratch + offset, chunk, want);
                offset += want;
            }
            remaining -= want;
        }
        if (Wal_tailTorn(self))
            break;
        if (!readExact(file, trailer, 4u)) {
            Wal_setTailTorn(self, true);
            break;
        }
        if (Crc32_finalize(crc) != getU32(trailer))
            return WAL_CORRUPT;
        if (visit != nullptr) {
            if (length > scratchCap)
                return WAL_INVALID;
            WalRecord record = { op, sequence, scratch, length };
            ++delivered;
            if (!visit(context, &record))
                break;
        }
        expected = sequence;
        ++seen;
        if (adopt) {
            Wal_setSequence(self, sequence);
            Wal_setCount(self, seen);
        }
    }
    if (visit != nullptr)
        Wal_setRecovered(self, delivered);
    return Wal_tailTorn(self) ? WAL_TRUNCATED : WAL_OK;
}

static bool regionRead(Wal *self, u64 at, any dest, bytes size) {
    u64 cap = Wal_capacity(self);
    File *file = Wal_file(self);
    return at <= cap && size <= cap - at &&
           File_seek(file, (i64) (Wal_offset(self) + at)) &&
           readExact(file, dest, size);
}

static bool zeroBytes(const u8 *data, bytes size) {
    for (bytes i = 0u; i < size; ++i)
        if (data[i] != 0u)
            return false;
    return true;
}

/**
 * This walk never publishes its cursor except during initial open. A missing
 * marker is an uncommitted tail, not a redo intent. A present marker requires CRC.
 */
static WalCode scanRegion(Wal *self, u8 *scratch, bytes cap,
                          WalVisitFn visit, any context, bool adopt) {
    u64 at = WAL_HEADER_BYTES;
    u64 expected = Wal_baseSequence(self);
    u64 regionCap = Wal_capacity(self);
    u32 seen = 0u;
    Wal_setRecovered(self, 0u);
    Wal_setTailTorn(self, false);
    while (at < regionCap) {
        u8 marker[WAL_COMMIT_BYTES] = {0};
        u64 room = regionCap - at;
        bytes n = room < WAL_COMMIT_BYTES ? (bytes) room : WAL_COMMIT_BYTES;
        if (!regionRead(self, at, marker, n))
            return WAL_IO;
        if (zeroBytes(marker, n)) {
            u8 chunk[WAL_READ_CHUNK];
            for (u64 pos = at + n; pos < regionCap;) {
                bytes take = regionCap - pos < sizeof chunk ?
                              (bytes) (regionCap - pos) : sizeof chunk;
                if (!regionRead(self, pos, chunk, take))
                    return WAL_IO;
                if (!zeroBytes(chunk, take)) {
                    Wal_setTailTorn(self, true);
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
            u8 chunk[WAL_READ_CHUNK];
            bytes take = length - pos < sizeof chunk ? length - pos : sizeof chunk;
            if (!regionRead(self, at + WAL_COMMIT_BYTES + WAL_RECORD_FIXED + pos, chunk, take))
                return WAL_IO;
            crc = Crc32_update(crc, chunk, take);
            if (scratch != nullptr && length <= cap)
                memcpy(scratch + pos, chunk, take);
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
            Wal_setRecovered(self, Wal_recovered(self) + 1u);
        }
        at += total;
        expected = sequence;
        ++seen;
    }
    if (adopt) {
        Wal_setEnd(self, at);
        Wal_setSequence(self, expected);
        Wal_setCount(self, seen);
    }
    return Wal_tailTorn(self) ? WAL_TRUNCATED : WAL_OK;
}

;;CONSTRUCTOR
static Wal *allocWal(string path) {
    bytes length = strlen(path);
    if (length == 0u || length >= WAL_PATH_MAX) {
        THROW("Wal: path is empty or too long");
        return nullptr;
    }
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, Darkbase_arena(), sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    memcpy(Wal_pathMut(self), path, length + 1u);
    return self;
}

Wal *Wal_0() {
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, Darkbase_arena(), sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal_0: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

Wal *Wal_1(string path) {
    if (path == nullptr) {
        THROW("Wal_1: null path");
        return nullptr;
    }
    Wal *self = allocWal(path);
    if (self == nullptr)
        return nullptr;
    File *file = File_open(Wal_path(self), FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE);
    if (file == nullptr) {
        THROW("Wal_1: the log could not be opened");
        Wal_setLastCode(self, WAL_IO);
        Wal_setFile(self, nullptr);
        return self;
    }
    Wal_setFile(self, file);
    Wal_setOpen(self, true);
    WalCode code = scanHeader(self);
    if (code == WAL_OK)
        code = scanRecords(self, nullptr, 0u, nullptr, nullptr, true);
    Wal_setLastCode(self, code);
    if (code == WAL_CORRUPT)
        THROW("Wal_1: the log header or a record is corrupt");
    return self;
}

Wal *Wal_3(string path, u64 offset, u64 capacity) {
    if (path == nullptr || capacity < WAL_HEADER_BYTES || offset > INT64_MAX ||
        capacity > (u64) INT64_MAX - offset) {
        THROW("Wal_3: invalid path or region extent");
        return nullptr;
    }
    Wal *self = allocWal(path);
    if (self == nullptr)
        return nullptr;
    Wal_setRegion(self, true);
    Wal_setOffset(self, offset);
    Wal_setCapacity(self, capacity);
    Wal_setEnd(self, WAL_HEADER_BYTES);
    File *file = File_open(path, FILE_MODE_READ | FILE_MODE_WRITE);
    Wal_setFile(self, file);
    struct stat status;
    WalCode code = WAL_IO;
    if (file != nullptr && fstat(fileno(File_handle(file)), &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_size >= 0 &&
        (u64) status.st_size >= offset + capacity) {
        u8 header[WAL_HEADER_BYTES];
        if (regionRead(self, 0u, header, sizeof header)) {
            code = WAL_CORRUPT;
            if (zeroBytes(header, sizeof header)) {
                Wal_setOpen(self, true);
                code = scanRegion(self, nullptr, 0u, nullptr, nullptr, true);
                if (code == WAL_TRUNCATED)
                    code = WAL_CORRUPT;
                if (code == WAL_OK && (!writeHeader(self) || !syncFile(self)))
                    code = WAL_IO;
            } else if (getU32(header) == WAL_MAGIC && header[4] == WAL_REGION_VERSION &&
                       header[5] == 0u && header[6] == 0u && header[7] == 0u &&
                       getU32(header + 16) == 0u &&
                       getU32(header + 20) == Crc32_of(header, 20u)) {
                Wal_setBaseSequence(self, getU64(header + 8));
                code = scanRegion(self, nullptr, 0u, nullptr, nullptr, true);
            }
        }
    }
    Wal_setLastCode(self, code);
    bool isOpen = code == WAL_OK || code == WAL_TRUNCATED;
    Wal_setOpen(self, isOpen);
    Wal_setPoisoned(self, !isOpen);
    if (!isOpen)
        THROW("Wal_3: region admission failed (%s)", Wal_codeName(code));
    return self;
}

fn Wal_free(Wal *self) {
    if (self == nullptr)
        return;
    File *file = Wal_file(self);
    if (file != nullptr)
        File_close(file);
    Memory_free(self);
}

bool Wal_isOpen(const Wal *self)
{
    return self != nullptr && Wal_open(self);
}
string Wal_path(const Wal *self)
{
    return self == nullptr ? "" : (*self).path;
}
WalCode Wal_code(const Wal *self)
{
    return self == nullptr ? WAL_INVALID : (WalCode) Wal_lastCode(self);
}
u64 Wal_sequence(const Wal *self)
{
    return self == nullptr ? 0u : (*self).sequence;
}
u64 Wal_baseSequence(const Wal *self)
{
    return self == nullptr ? 0u : (*self).baseSequence;
}
u32 Wal_count(const Wal *self)
{
    return self == nullptr ? 0u : (*self).count;
}
u32 Wal_recovered(const Wal *self)
{
    return self == nullptr ? 0u : (*self).recovered;
}
bool Wal_tailTorn(const Wal *self)
{
    return self != nullptr && (*self).tailTorn;
}
u64 Wal_remaining(const Wal *self) {
    return self == nullptr || !Wal_open(self) ? 0u :
           (Wal_region(self) ? Wal_capacity(self) - Wal_end(self) : UINT64_MAX);
}
u64 Wal_exhaustionCount(const Wal *self) {
    return self == nullptr ? 0u : (*self).exhaustionCount;
}

WalCode Wal_append(Wal *self, u32 op, const u8 *payload, u32 length,
                   u64 *outSequence) {
    if (self == nullptr || !Wal_open(self)) {
        THROW("Wal_append: the owner is not open");
        if (self != nullptr)
            Wal_setLastCode(self, WAL_CLOSED);
        return WAL_CLOSED;
    }
    if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT ||
        (payload == nullptr && length != 0u) || length > WAL_PAYLOAD_MAX) {
        THROW("Wal_append: unknown op, null payload or an over-large payload");
        Wal_setLastCode(self, WAL_INVALID);
        return WAL_INVALID;
    }
    File *file = Wal_file(self);
    u64 curSeq = Wal_sequence(self);
    u32 curCount = Wal_count(self);
    if (Wal_poisoned(self) || curSeq == UINT64_MAX || curCount == UINT32_MAX) {
        THROW("Wal_append: poisoned owner or sequence/count overflow");
        Wal_setLastCode(self, WAL_INVALID);
        return WAL_INVALID;
    }
    bool isRegion = Wal_region(self);
    u64 total = WAL_RECORD_FIXED + (u64) length + 4u + WAL_COMMIT_BYTES;
    if (isRegion && total > Wal_remaining(self)) {
        u64 exh = Wal_exhaustionCount(self);
        Wal_setExhaustionCount(self, exh + 1u);
        if (exh == 0u)
            THROW("Wal_append: region exhausted, request %llu bytes (remaining %llu)",
                  (u64) total, (u64) Wal_remaining(self));
        Wal_setLastCode(self, WAL_FULL);
        return WAL_FULL;
    }
    u64 sequence = curSeq + 1u;
    u8 prefix[WAL_RECORD_FIXED];
    putU32(&prefix[0], WAL_MAGIC);
    putU32(&prefix[4], op);
    putU64(&prefix[8], sequence);
    putU32(&prefix[16], length);
    u32 crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
    crc = Crc32_update(crc, payload, length);
    u8 trailer[4];
    putU32(trailer, Crc32_finalize(crc));
    u64 offset = Wal_offset(self);
    u64 end = Wal_end(self);
    u64 cap = Wal_capacity(self);
    if (isRegion && Wal_tailTorn(self)) {
        u8 zeros[WAL_READ_CHUNK] = {0};
        bool cleared = File_seek(file, (i64) (offset + end));
        for (u64 pos = end; cleared && pos < cap;) {
            bytes take = cap - pos < sizeof zeros ?
                          (bytes) (cap - pos) : sizeof zeros;
            cleared = writeExact(file, zeros, take);
            pos += take;
        }
        if (!cleared || !syncFile(self)) {
            Wal_setPoisoned(self, true);
            Wal_setLastCode(self, WAL_IO);
            THROW("Wal_append: discarded tail could not be cleared");
            return WAL_IO;
        }
    }
    i64 position = isRegion ? (i64) (offset + end) : File_refreshSize(file);
    u8 zeroMarker[WAL_COMMIT_BYTES] = {0};
    if (!File_seek(file, position) ||
        (isRegion && !writeExact(file, zeroMarker, sizeof zeroMarker)) ||
        !writeExact(file, prefix, WAL_RECORD_FIXED) ||
        (length > 0u && !writeExact(file, payload, length)) || !writeExact(file, trailer, 4u)) {
        THROW("Wal_append: the record could not be written");
        Wal_setLastCode(self, WAL_IO);
        Wal_setPoisoned(self, isRegion);
        return WAL_IO;
    }
    if (isRegion) {
        u8 marker[WAL_COMMIT_BYTES];
        putU64(marker, WAL_COMMIT_MAGIC);
        if (!syncFile(self) || !File_seek(file, position) || !writeExact(file, marker, sizeof marker)) {
            Wal_setPoisoned(self, true);
            Wal_setLastCode(self, WAL_IO);
            THROW("Wal_append: record sync or marker publication failed");
            return WAL_IO;
        }
        Wal_setEnd(self, end + total);
        Wal_setTailTorn(self, false);
    }
    Wal_setCount(self, curCount + 1u);
    Wal_setSequence(self, sequence);
    if (outSequence != nullptr)
        *outSequence = sequence;
    Wal_setLastCode(self, WAL_OK);
    return WAL_OK;
}

WalCode Wal_sync(Wal *self) {
    if (self == nullptr || !Wal_open(self)) {
        THROW("Wal_sync: the owner is not open");
        return WAL_CLOSED;
    }
    if (Wal_poisoned(self)) {
        THROW("Wal_sync: poisoned owner requires reopen");
        Wal_setLastCode(self, WAL_INVALID);
        return WAL_INVALID;
    }
    if (!syncFile(self)) {
        THROW("Wal_sync: the log could not be made durable");
        Wal_setLastCode(self, WAL_IO);
        Wal_setPoisoned(self, Wal_region(self));
        return WAL_IO;
    }
    Wal_setLastCode(self, WAL_OK);
    return WAL_OK;
}

WalCode Wal_replay(Wal *self, u8 *scratch, bytes scratchCap,
                   WalVisitFn visit, any context) {
    if (self == nullptr || !Wal_open(self)) {
        THROW("Wal_replay: the owner is not open");
        if (self != nullptr)
            Wal_setLastCode(self, WAL_CLOSED);
        return WAL_CLOSED;
    }
    if (visit != nullptr && scratch == nullptr) {
        THROW("Wal_replay: a visitor was given no replay buffer");
        Wal_setLastCode(self, WAL_INVALID);
        return WAL_INVALID;
    }
    bool retainedTail = Wal_tailTorn(self);
    Wal_setTailTorn(self, false);
    bool isRegion = Wal_region(self);
    WalCode code = isRegion ? scanRegion(self, scratch, scratchCap, visit, context, false) :
                   scanRecords(self, scratch, scratchCap, visit, context, false);
    if (isRegion)
        Wal_setTailTorn(self, Wal_tailTorn(self) || retainedTail);
    if (isRegion && code != WAL_OK && code != WAL_TRUNCATED) {
        if (code == WAL_CORRUPT || code == WAL_IO)
            Wal_setPoisoned(self, true);
        THROW("Wal_replay: region walk refused (%s)", Wal_codeName(code));
    }
    Wal_setLastCode(self, code);
    return code;
}

WalCode Wal_checkpoint(Wal *self) {
    if (self == nullptr || !Wal_open(self)) {
        THROW("Wal_checkpoint: the owner is not open");
        return WAL_CLOSED;
    }
    if (Wal_region(self)) {
        THROW("Wal_checkpoint: embedded region cannot be checkpointed or truncated");
        Wal_setLastCode(self, WAL_INVALID);
        return WAL_INVALID;
    }
    if (!syncFile(self)) {
        THROW("Wal_checkpoint: the log could not be synced");
        Wal_setLastCode(self, WAL_IO);
        return WAL_IO;
    }
    Wal_setBaseSequence(self, Wal_sequence(self));
    Wal_setCount(self, 0u);
    if (!writeHeader(self)) {
        THROW("Wal_checkpoint: the header could not be rewritten");
        Wal_setLastCode(self, WAL_IO);
        return WAL_IO;
    }
    File *file = Wal_file(self);
#if defined(VEX_PLATFORM_WINDOWS)
    if (_chsize_s(_fileno(File_handle(file)), (i64) WAL_HEADER_BYTES) != 0) {
#else
    if (ftruncate(fileno(File_handle(file)), (off_t) WAL_HEADER_BYTES) != 0) {
#endif
        THROW("Wal_checkpoint: the log could not be truncated");
        Wal_setLastCode(self, WAL_IO);
        return WAL_IO;
    }
    (discard) File_refreshSize(file);
    if (!File_seek(file, (i64) WAL_HEADER_BYTES) || !syncFile(self)) {
        THROW("Wal_checkpoint: the truncated log could not be made durable");
        Wal_setLastCode(self, WAL_IO);
        return WAL_IO;
    }
    Wal_setLastCode(self, WAL_OK);
    return WAL_OK;
}

string Wal_codeName(WalCode code) {
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

static bool writeNull(utf8 *dest, bytes capacity, bool *outTruncated) {
    string text = "nullptr";
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

bool Wal_toString(const Wal *self, utf8 *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    i32 written = snprintf(dest, capacity, "Wal(open=%d, sequence=%llu, count=%u, base=%llu)",
                           self != nullptr && Wal_open(self) ? 1 : 0,
                           (u64) Wal_sequence(self), Wal_count(self),
                           (u64) Wal_baseSequence(self));
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

static fn quotePath(string path, utf8 dest[WAL_PATH_MAX * 4u + 3u]) {
    static const utf8 hex[] = "0123456789abcdef";
    bytes used = 0u;
    dest[used++] = '"';
    for (bytes i = 0u; i < WAL_PATH_MAX && path[i] != '\0'; ++i) {
        u8 byte = (u8) path[i];
        if (byte == '"' || byte == '\\') {
            dest[used++] = '\\';
            dest[used++] = (utf8) byte;
        } else if (byte < 32u || byte == 127u) {
            dest[used++] = '\\';
            dest[used++] = 'x';
            dest[used++] = hex[byte >> 4];
            dest[used++] = hex[byte & 15u];
        } else {
            dest[used++] = (utf8) byte;
        }
    }
    dest[used++] = '"';
    dest[used] = '\0';
}

bool Wal_toStringStruct(const Wal *self, utf8 *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    utf8 path[WAL_PATH_MAX * 4u + 3u];
    quotePath(Wal_path(self), path);
    i32 written = snprintf(dest, capacity,
                            "Wal{file=<owned>, path=%s, baseSequence=%llu, sequence=%llu, count=%u, "
                            "recovered=%u, lastCode=%s, tailTorn=%d, open=%d, offset=%llu, "
                            "capacity=%llu, end=%llu, exhaustionCount=%llu, region=%d, poisoned=%d}",
                            path,
                            (u64) Wal_baseSequence(self),
                            (u64) Wal_sequence(self), Wal_count(self),
                            Wal_recovered(self), Wal_codeName((WalCode) Wal_lastCode(self)),
                            Wal_tailTorn(self) ? 1 : 0, Wal_open(self) ? 1 : 0,
                            (u64) Wal_offset(self),
                            (u64) Wal_capacity(self),
                            (u64) Wal_end(self),
                            (u64) Wal_exhaustionCount(self),
                            Wal_region(self) ? 1 : 0, Wal_poisoned(self) ? 1 : 0);
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
