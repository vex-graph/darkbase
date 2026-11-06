#ifndef DARKBASE_UTIL_CRC32_H
#define DARKBASE_UTIL_CRC32_H

#include <stddef.h>
#include <stdint.h>

// util/crc32.h — the engine's one CRC32 (reflected IEEE, polynomial 0xEDB88320).
//
// This is the canonical checksum implementation used by the WAL, legacy snapshot
// compatibility code and the paged format's per-page seals.
//
// The accumulator is NOT finalized: `Crc32_update` returns the running value and
// `Crc32_finalize` complements it. That split is what lets a checksum be computed
// over pieces that are written separately.

// Run the accumulator over more bytes. Start from CRC32_SEED for a fresh checksum.
uint32_t Crc32_update(uint32_t crc, const void *data, size_t length);
// Complement the accumulator: the value that gets stored.
uint32_t Crc32_finalize(uint32_t crc);
// One-shot checksum over a single span.
uint32_t Crc32_of(const void *data, size_t length);

#define CRC32_SEED 0xFFFFFFFFu

#endif
