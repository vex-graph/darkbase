#include "util/crc32.h"
#include "annotation/annotation.h"

;;DEFINITION
/** The reflected IEEE CRC32 every durable byte in this engine is checked with. It
 * is a pure byte function: no file, no device, no state, no allocation. Its body is
 * is the one implementation shared by the WAL, snapshot compatibility code and the
 * paged format's per-page seals. */
;;OVERVIEW
/** ========================================================================
 * MODULE: Crc32 (util/crc32.c)
 * FUNCTION REGISTRY (exports)
 *   Crc32_update(crc, data, length)   running accumulator (poly 0xEDB88320)
 *   Crc32_finalize(crc)               complement before storing
 *   Crc32_of(data, length)            one-shot: seed, update, finalize
 * CONTRACT: a null span with a nonzero length is the caller's error, not a checked
 * rejection (no pointer arithmetic happens over zero bytes); no allocation, no
 * logging, no state. ======================================================================== */

u32 Crc32_update(u32 crc, const void *data, bytes length) {
    const u8 *bytes = (const u8*) data;
    for (bytes i = 0u; i < length; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32) (-(i32) (crc & 1u)));
    }
    return crc;
}

u32 Crc32_finalize(u32 crc) { return ~crc; }

u32 Crc32_of(const void *data, bytes length) {
    return Crc32_finalize(Crc32_update(CRC32_SEED, data, length));
}
