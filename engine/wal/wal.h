#ifndef DARKBASE_WAL_WAL_H
#define DARKBASE_WAL_WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "c23/constructor.h"

// wal/wal.h — the write-ahead log: the reason a crash mid-write is survivable.
//
// A record is appended BEFORE the store mutates anything, and the log is synced
// before the mutation is acknowledged. On recovery the log is replayed in order, so
// the store can redo work whose data pages never made it to disk. An incomplete
// TRAILING record is dropped (a crash tore the tail); every complete record must
// verify, and a checksum failure is reported as corruption rather than skipped,
// because a hole in a log means the order cannot be trusted.
//
// The log borrows the engine's `File` for all IO: there is no second file layer and
// no second CRC here. Append/replay own no temporary allocation: append writes the record in place, and
// replay streams payloads through a buffer the CALLER supplies, refusing a record
// that does not fit rather than truncating it.
//
// Not offered: transactions across entities, group commit, a standalone size ceiling,
// compression, a background fsync thread, or any claim about the data file's own
// atomicity. `checkpoint` truncates the log; making the data durable first is the
// caller's ordering obligation and is stated as such.

// The record and header magic: the bytes 'V','E','X','W' in memory order, so the
// little-endian host floor needs no byte-order machinery to compare it.
#define WAL_MAGIC    0x56455857u
#define WAL_VERSION  1u

#define WAL_PATH_MAX      1024u
#define WAL_HEADER_BYTES    24u   // magic, version, reserved, baseSequence, reserved, crc
#define WAL_RECORD_FIXED    20u   // magic, op, sequence, length (then payload, then crc)
#define WAL_PAYLOAD_MAX  (1u << 20)
// Region v3 places its publication marker BEFORE the length-bearing record body.
#define WAL_REGION_VERSION 3u
#define WAL_COMMIT_BYTES 8u
#define WAL_COMMIT_MAGIC UINT64_C(0x54494d4d4f435857)

typedef enum WalCode {
    WAL_OK = 0,
    WAL_INVALID = 1,     // null/illegal argument, unknown op, or an over-large payload
    WAL_IO = 2,          // the OS refused a read, write, sync, seek or truncate
    WAL_CORRUPT = 3,     // a complete record failed its checksum (never skipped)
    WAL_TRUNCATED = 4,   // an incomplete trailing record was dropped
    WAL_CLOSED = 5,      // the owner is not open
    WAL_FULL = 6,        // caller-selected region capacity exhausted
} WalCode;

typedef enum WalOp {
    WAL_OP_INSERT = 1,
    WAL_OP_UPDATE = 2,
    WAL_OP_DELETE = 3,
    WAL_OP_CHECKPOINT = 4,
} WalOp;

// A record as handed to the replay visitor: the payload is borrowed and valid only
// for that call.
typedef struct WalRecord {
    uint32_t op;
    uint64_t sequence;
    const uint8_t *payload;
    uint32_t length;
} WalRecord;

// Standalone: false stops successfully. Region: false is WAL_INVALID (including
// on the final record), so a refused redo can never be mistaken for success.
typedef bool (*WalVisitFn)(void *context, const WalRecord *record);

typedef struct Wal {
    void *file;              // engine File* owner; void* keeps this header IO-free
    char path[WAL_PATH_MAX];
    uint64_t baseSequence;   // last checkpointed sequence, from the header
    uint64_t sequence;       // highest sequence written or recovered
    uint32_t count;          // records appended since the last checkpoint
    uint32_t recovered;      // records seen by the last successful replay
    int32_t lastCode;        // last WalCode refusal
    bool tailTorn;           // recovery found an incomplete trailing record
    bool open;
    uint64_t offset;         // absolute region start; zero for standalone
    uint64_t capacity;       // finite region extent including its header
    uint64_t end;            // relative append position, adopted only on open/append
    uint64_t exhaustionCount;
    bool region;
    bool poisoned;          // failed IO/corruption requires close and reopen
} Wal;

// Arity constructors: Wal_0 is a closed owner, Wal_1 creates or opens the log at
// `path` and recovers its tail. Invalid constructor arguments return null; file or
// format failures return an owner whose Wal_code reports the refusal. Wal_3 keeps
// such an owner closed; Wal_1 retains its legacy open-state behavior.
Wal *Wal_0(void);
Wal *Wal_1(const char *path);
// Existing regular file only; trusted host excludes every alias. No create,
// resize or checkpoint. Capacity is an explicit finite journal safety bound.
// Region record bytes are synced BEFORE publishing the commit marker; caller
// must Wal_sync the marker BEFORE applying redo to data pages. IO failure poisons
// this owner until reopen. An uncommitted tail is ignored and overwritten.
Wal *Wal_3(const char *path, uint64_t offset, uint64_t capacity);
#define Wal(...) CONSTRUCTOR_DISPATCH(Wal, __VA_ARGS__)
#define Wal_zero() Wal_0()

void        Wal_free(Wal *self);
bool        Wal_isOpen(const Wal *self);
const char *Wal_path(const Wal *self);
WalCode     Wal_code(const Wal *self);
uint64_t    Wal_sequence(const Wal *self);
uint64_t    Wal_baseSequence(const Wal *self);
uint32_t    Wal_count(const Wal *self);
uint32_t    Wal_recovered(const Wal *self);
bool        Wal_tailTorn(const Wal *self);
uint64_t    Wal_remaining(const Wal *self);
uint64_t    Wal_exhaustionCount(const Wal *self);

// Append one intent. Writes the record and does NOT sync: `Wal_sync` is the
// durability boundary, and the caller acknowledges a mutation only after it.
WalCode Wal_append(Wal *self, uint32_t op, const uint8_t *payload, uint32_t length,
                   uint64_t *outSequence);
// Make every appended record durable on this host.
WalCode Wal_sync(Wal *self);
// Replay every valid record in order through the caller's buffer and visitor.
// Returns TRUNCATED when the trailing record was dropped, CORRUPT when a complete
// record failed its checksum.
WalCode Wal_replay(Wal *self, uint8_t *scratch, size_t scratchCap,
                   WalVisitFn visit, void *context);
// Truncate the log once the caller has made the DATA durable. The sequence keeps
// counting up across a checkpoint; it never restarts.
WalCode Wal_checkpoint(Wal *self);
const char *Wal_codeName(WalCode code);

bool Wal_toString(const Wal *self, char *dest, size_t capacity, bool *outTruncated);
bool Wal_toStringStruct(const Wal *self, char *dest, size_t capacity, bool *outTruncated);

#endif
